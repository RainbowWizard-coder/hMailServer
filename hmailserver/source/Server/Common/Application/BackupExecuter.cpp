// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "BackupExecuter.h"
#include "Backup.h"

#include "../Util/Utilities.h"
#include "../Util/FileUtilities.h"
#include "../Util/Time.h"
#include "../BO/Domains.h"
#include "../BO/Domain.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Accounts.h"
#include "../BO/Aliases.h"
#include "../BO/DomainAliases.h"
#include "../BO/DistributionLists.h"

#include "../Persistence/PersistentMessage.h"
#include "../Util/Compression.h"
// ServiceManager wraps the Windows service control manager, which the roadmap
// section "Linux and AArch64" leaves out of the POSIX build entirely - systemd
// owns that job there. Nothing below actually uses it, so the include simply
// goes away on Linux.
#ifdef _MSC_VER
#include "../Util/ServiceManager.h"
#endif

#include "AccountStores.h"
#include "RoleManager.h"
#include "BackupManager.h"
#include "BackupRetention.h"
#include "BackupRestorer.h"
#include "ACLManager.h"
#include "Reinitializator.h"

#include "../SQL/DatabaseUnavailableMarker.h"
#include "../AntiSpam/DKIM/DkimRotation.h"
#include "../Util/DiscoveryHolds.h"
#include "../Util/GUIDCreator.h"
#include "../Persistence/MessageStoreBlobs.h"
#include "../Persistence/MessageStoreObjects.h"

#include "../../IMAP/IMAPConfiguration.h"

#include <boost/filesystem.hpp>

#include <map>
#include <set>

#ifdef HM_PLATFORM_POSIX
#include <sys/stat.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // WHY THIS FILE COPIES DIRECTORY TREES ITSELF INSTEAD OF CALLING
   // FileUtilities::CopyDirectory
   //
   // CopyDirectory copies each file with the throwing overload of
   // boost::filesystem::copy_file, with no copy_options, and lets whatever comes out
   // of it escape. Three ordinary things make it throw, and the consequence of any
   // of them is the same and is much worse than a failed backup:
   //
   //  * The destination file already exists. copy_options::none means "fail if the
   //    destination exists", so staging into a DataBackup folder left behind by an
   //    earlier run throws. With CompressDestinationFiles switched off nothing ever
   //    removes that folder - it *is* the backup - so on that configuration the
   //    second backup the server ever takes throws, every time.
   //
   //  * The source file no longer exists. The tree is enumerated and then copied,
   //    and this is a live mail server: a POP3 DELE, an IMAP EXPUNGE or a delivery
   //    finishing removes message files continuously. Between listing a file and
   //    copying it, it can be gone.
   //
   //  * Something else has the file open in a way that denies read sharing - an
   //    anti-virus scanner mid-scan being the usual one.
   //
   // Where the exception goes is the problem. ExceptionHandler::Run reports it and
   // rethrows; WorkQueue::ExecuteTask releases its blocking slot and rethrows;
   // io_context::run propagates it; WorkQueue::IoServiceRunWorker rethrows anything
   // that is not ERROR_ABANDONED_WAIT_0 - and it is then an exception escaping a
   // boost::thread function, which calls std::terminate. A message file being
   // deleted while a backup runs takes the whole service down with it, and because
   // OnBackupFailed is never reached there is no BACKUP ERROR line and no
   // OnBackupFailed event to explain why.
   //
   // So the backup does its own copy, with the error_code overloads, and gives each
   // of the three cases the answer it deserves: overwrite, skip and count, retry
   // briefly then fail the backup naming the file. FileUtilities::CopyDirectory is
   // left alone because its other callers are not backups and a shared file is not
   // this change's to redefine.

   // How long, in total, one copy may spend waiting for files something else has
   // open. Bounded because it is not: a store of two hundred thousand messages
   // behind a scanner that is holding all of them would otherwise retry for days.
   // Past the budget the next locked file fails the run, with its name.
   const unsigned int COPY_RETRY_BUDGET_MS = 30000;
   const unsigned int COPY_RETRY_PAUSE_MS = 250;

   struct TreeCopyOutcome
   {
      unsigned int copied;

      // Files that were listed and then were not there. Normal on a live server,
      // reported as a count so that a number which is not small is visible.
      unsigned int vanished;

      unsigned int retry_sleep_ms;

      String failed_file;
      String failure_detail;

      // The directories left out because they are the destination, the temp
      // directory or this run's staging folder (DataDirectoryExclusions_), in
      // the words the backup log says them in.
      std::vector<String> excluded;

      TreeCopyOutcome() :
         copied(0),
         vanished(0),
         retry_sleep_ms(0)
      {
      }
   };

   static bool
   CopyOneFile_(const boost::filesystem::path &from, const boost::filesystem::path &to, TreeCopyOutcome &outcome)
   {
      for (;;)
      {
         boost::system::error_code copyError;
         boost::filesystem::copy_file(from, to, boost::filesystem::copy_options::overwrite_existing, copyError);

         if (!copyError)
         {
            outcome.copied++;
            return true;
         }

         // Gone rather than unreadable. hMailServer opens its own message files with
         // _SH_DENYNO (see File::Open), so a file that is still there and still will
         // not copy is being held by something outside the server, which is worth
         // waiting for and then failing over. A file that has been deleted since the
         // directory was listed is just mail flow, and failing the nightly backup
         // because a user emptied their trash during it would be absurd.
         //
         // "Positively established as absent", not "could not be found": exists()
         // reports a failure to look through its error_code, and treating that as
         // absence would silently leave a message out of the backup - which is the
         // outcome all of this exists to prevent.
         boost::system::error_code existsError;
         bool sourceStillExists = boost::filesystem::exists(from, existsError);

         if (!existsError && !sourceStillExists)
         {
            outcome.vanished++;
            return true;
         }

         if (outcome.retry_sleep_ms >= COPY_RETRY_BUDGET_MS)
         {
            outcome.failed_file = String(from.wstring());
            outcome.failure_detail = copyError.message().c_str();
            return false;
         }

         Sleep(COPY_RETRY_PAUSE_MS);
         outcome.retry_sleep_ms += COPY_RETRY_PAUSE_MS;
      }
   }

   static bool
   IsKeptOutOfBackup_(const boost::filesystem::path &directory)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The directories at the data directory's root that a backup does not carry
   // and a restore neither deletes nor puts back: the recoverable store
   // (docs/RecoverDeleted.md), the eDiscovery cases' results and exports
   // (docs/Discovery.md), the kept copies of messages that expire
   // (docs/ConfidentialMessages.md) and the ciphertext of password-protected
   // messages (docs/ProtectedMessages.md). Each is indexed by tables the XML
   // backup does not hold - hm_recoverable, the four discovery tables,
   // hm_confidential and hm_protectedmessages - so a backup's copy is files no
   // row names, and a restore that replaced them would leave live rows naming
   // files that are gone: a case's results and exports, what a legal hold kept,
   // the message a link still being read points at. An export is a copy of
   // the custodians' mail of up to 4 GB besides, and a backup that carried
   // every export ever made would grow with each one. And a message that
   // expires is a copy made to be deleted at a time: carried, the whole of it,
   // attachments and all and in the clear even for a mailbox encrypted at
   // rest, would outlive its expiry and its withdrawal in every archive taken
   // while it was live (AccountStores.cpp, hm_confidential).
   //
   // And the live update's folder (UpdateDownloader::UpdatesDirectory,
   // docs/UpdateFeed.md): the installer or package waiting to be applied and
   // its Sigstore bundle, the rollback image, the helper's copy, its logs and
   // request, the single-use apply token and the outcome of the last apply.
   // None of it is mail and none of it is wanted after a restore, on this
   // machine or another: an installer is downloaded and verified again, a
   // token is good for one apply within the hour on the machine that issued
   // it, and the outcome file is read at start-up, so a restored copy of it
   // would have a server report an apply that never happened there. It was
   // most of every backup taken before an unattended apply - the package
   // that apply was about to install, 15 MB beside 353 bytes of mail in the
   // first Linux live update, and on Windows the installer (88 MB for 6.3.5)
   // and the rollback image beside it. A restore leaves this machine's own
   // folder as it is.
   //---------------------------------------------------------------------------()
   {
      const String name(directory.filename().wstring());
      return name.CompareNoCase(_T("Recoverable")) == 0 || name.CompareNoCase(_T("Discovery")) == 0 ||
             name.CompareNoCase(_T("Confidential")) == 0 || name.CompareNoCase(_T("Protected")) == 0 ||
             name.CompareNoCase(_T("Updates")) == 0;
   }

   // WHERE A BACKUP DOES NOT GO WHEN IT COPIES THE DATA DIRECTORY
   //
   // A backup copies the whole data directory into a staging folder, and a
   // restore empties it before copying the archive's back in. Three of the
   // directories either one works with may be inside it: the backup
   // destination, which on a Linux package is naturally under
   // /var/lib/hmailserver because that and /var/log/hmailserver are the only
   // places the packaged unit lets the service write; the temp directory,
   // which the Linux packages' hMailServer.ini puts at /var/lib/hmailserver/temp
   // and where an encrypted backup stages and every restore extracts; and the
   // staging folder itself. A copy that walked into its own staging folder
   // copied the copy into itself until the path was too long - HM5014 at every
   // run, and so every unattended Linux update refused, as the first real one
   // on 29 September 2026 found - and a restore that emptied the data
   // directory took the destination's archives with it, and on a Linux package
   // the extracted store it was about to copy back.
   //
   // So both leave those directories out, found by what they ARE rather than
   // by how they are spelt: a directory is compared by its file system identity
   // - device and inode, or volume, file identifier and creation time
   // (IdentifyDirectory_) - which is the same for every
   // path that reaches it whatever its case, its separators, a symbolic link or
   // a short name, and is never the same for /var/lib/hmailserver2 and
   // /var/lib/hmailserver. A destination inside the data directory is therefore
   // accepted, as it always was - refusing it would stop configurations that
   // are running today - and it works; what it means is that the backup lives
   // on the disk it backs up (docs/Backup.md).
   struct DirectoryIdentity_
   {
      unsigned long long volume;
      unsigned long long high;
      unsigned long long low;
      unsigned long long created;

      DirectoryIdentity_() : volume(0), high(0), low(0), created(0) {}

      bool operator<(const DirectoryIdentity_ &other) const
      {
         if (volume != other.volume)
            return volume < other.volume;
         if (high != other.high)
            return high < other.high;
         if (low != other.low)
            return low < other.low;
         return created < other.created;
      }
   };

   static bool
   IdentifyDirectory_(const boost::filesystem::path &path, DirectoryIdentity_ &identity)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What the path leads to, following links. False when it cannot be opened
   // or examined - a path that is not there - or when the file system gives it
   // no identifier of its own, which callers read as "not one of the
   // directories asked about".
   //
   // A false match is the costly mistake here, not a missed one: a directory
   // of mail taken for the destination is left out of every backup, and out of
   // the restore's copy, with nothing said. A missed one is the failure there
   // was before any of this - the copy into itself, HM5014, loud. So on
   // Windows the identifier is the file system's 128-bit one (FileIdInfo)
   // where it has one: the 64-bit index of BY_HANDLE_FILE_INFORMATION "is not
   // guaranteed to be unique on ReFS" (Microsoft's documentation of that
   // structure), and a server's data on a ReFS volume could have had a
   // mailbox's directory taken for the destination on the same volume. The
   // creation time goes with it, which no second path to one directory
   // changes, so that two volumes that share a serial number - a disk and its
   // clone, both attached - cannot make two directories one either. And an
   // identifier of 0 or all ones, which a file system or a network server
   // that has none answers for every file, is none: every directory there
   // would have been every other.
   //---------------------------------------------------------------------------()
   {
#ifdef HM_PLATFORM_POSIX
      struct stat status {};
      if (::stat(path.string().c_str(), &status) != 0)
         return false;

      identity.volume = (unsigned long long) status.st_dev;
      identity.high = 0;
      identity.low = (unsigned long long) status.st_ino;
      identity.created = 0;
      return identity.low != 0;
#else
      HANDLE handle = ::CreateFileW(path.wstring().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
      if (handle == INVALID_HANDLE_VALUE)
         return false;

      BY_HANDLE_FILE_INFORMATION information;
      memset(&information, 0, sizeof(information));
      const BOOL answered = ::GetFileInformationByHandle(handle, &information);

      FILE_ID_INFO longIdentifier;
      memset(&longIdentifier, 0, sizeof(longIdentifier));
      const BOOL answeredLong = answered && ::GetFileInformationByHandleEx(handle, FileIdInfo, &longIdentifier, sizeof(longIdentifier));
      ::CloseHandle(handle);

      if (!answered)
         return false;

      identity.volume = (unsigned long long) information.dwVolumeSerialNumber;

      if (answeredLong)
      {
         // Both halves as they lie; on NTFS the upper one is 0 and the lower one
         // the 64-bit index, so a path that reaches the directory through a file
         // system without FileIdInfo still meets it under the same identity.
         memcpy(&identity.low, &longIdentifier.FileId.Identifier[0], sizeof(identity.low));
         memcpy(&identity.high, &longIdentifier.FileId.Identifier[8], sizeof(identity.high));
      }
      else
      {
         identity.high = 0;
         identity.low = ((unsigned long long) information.nFileIndexHigh << 32) | (unsigned long long) information.nFileIndexLow;
      }

      identity.created = ((unsigned long long) information.ftCreationTime.dwHighDateTime << 32) |
                         (unsigned long long) information.ftCreationTime.dwLowDateTime;

      const unsigned long long allOnes = ~0ull;
      if (identity.high == 0 && identity.low == 0)
         return false;
      if (identity.low == allOnes && (identity.high == 0 || identity.high == allOnes))
         return false;

      return true;
#endif
   }

   class DataDirectoryExclusions_
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The directories a copy of the data directory leaves out and a restore does
   // not empty, each with the words the backup log says it in, and every
   // directory above each of them - which a restore empties around them rather
   // than removing whole. Directories that are not there are not held: nothing
   // can be met in a copy that does not exist.
   //---------------------------------------------------------------------------()
   {
   public:

      void Add(const String &directory, const String &description)
      {
         if (directory.IsEmpty())
            return;

         boost::filesystem::path path(directory.c_str());

         DirectoryIdentity_ identity;
         if (!IdentifyDirectory_(path, identity))
            return;

         if (excluded_.find(identity) != excluded_.end())
            return;

         excluded_[identity] = description + _T(" ") + directory;

         // Every directory above it, from the path with its links resolved: a
         // destination reached through a link lies under the link's target, not
         // under the directories the link's own name is in.
         boost::system::error_code canonicalError;
         boost::filesystem::path real = boost::filesystem::canonical(path, canonicalError);
         if (canonicalError)
            real = boost::filesystem::absolute(path, canonicalError);
         if (canonicalError)
            return;

         for (boost::filesystem::path parent = real.parent_path(); !parent.empty(); parent = parent.parent_path())
         {
            DirectoryIdentity_ above;
            if (IdentifyDirectory_(parent, above))
               above_.insert(above);

            if (parent == parent.root_path())
               break;
         }
      }

      bool Empty() const
      {
         return excluded_.empty();
      }

      // True, with the words and the path, when directory is one of them.
      bool Contains(const boost::filesystem::path &directory, String &description) const
      {
         if (excluded_.empty())
            return false;

         DirectoryIdentity_ identity;
         if (!IdentifyDirectory_(directory, identity))
            return false;

         auto found = excluded_.find(identity);
         if (found == excluded_.end())
            return false;

         description = (*found).second;
         return true;
      }

      // Whether directory holds one of them, however deep.
      bool Holds(const boost::filesystem::path &directory) const
      {
         if (above_.empty())
            return false;

         DirectoryIdentity_ identity;
         return IdentifyDirectory_(directory, identity) && above_.find(identity) != above_.end();
      }

   private:

      std::map<DirectoryIdentity_, String> excluded_;
      std::set<DirectoryIdentity_> above_;
   };

   // Which side of a copy DataDirectoryExclusions_ is asked about: the source for
   // a backup, which copies the data directory, and the target for a restore,
   // which copies the archive's store into it.
   enum ExclusionSide_
   {
      ExcludeBySource_,
      ExcludeByTarget_
   };

   static void
   AddBackupExclusions_(DataDirectoryExclusions_ &exclusions, const String &destination, const String &temporaryDirectory,
                        const String &stagingFolder)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What a backup's copy of the data directory leaves out: the destination, the
   // temp directory and - once it has been made - the run's staging folder and
   // the folder that holds it, which is the destination for a backup that is
   // not encrypted and a folder of its own under the temp directory for one
   // that is. One function for the backup, its measure beforehand and
   // BackupExecuterTester, so that the tester holds what the backup does.
   //---------------------------------------------------------------------------()
   {
      exclusions.Add(destination, _T("the backup destination"));
      exclusions.Add(temporaryDirectory, _T("the temp directory"));

      if (!stagingFolder.IsEmpty())
      {
         exclusions.Add(String(boost::filesystem::path(stagingFolder.c_str()).parent_path().wstring()), _T("this backup's staging folder"));
         exclusions.Add(stagingFolder, _T("this backup's staging folder"));
      }
   }

   enum RestoreExclusionUse_
   {
      ForTheEmptying_,
      ForTheCopy_
   };

   static void
   AddRestoreExclusions_(DataDirectoryExclusions_ &exclusions, const String &destination, const String &temporaryDirectory,
                         const String &archiveFile, const String &stagedStore, RestoreExclusionUse_ use)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What a restore leaves in place when it empties the data directory, and does
   // not copy an older archive's copy of into: the destination, with its
   // archives; the temp directory, where the store to copy back has been
   // extracted; and that store. One function for the restore and
   // BackupExecuterTester.
   //
   // The folder of the archive being restored is left in place by the emptying
   // and NOT by the copy. Left in place, because it holds the archive, and
   // other archives beside it where it is a destination no longer configured.
   // Written into, because it can be a directory the archive restores: an
   // archive somebody put in a domain's or an account's folder would otherwise
   // keep that folder as it is now and have the archive's copy of it - that
   // mailbox's mail as it was backed up - left out of the restore, with rows
   // put back naming files that are not there. Written into, the archive's
   // files are put back over and beside what is there.
   //---------------------------------------------------------------------------()
   {
      exclusions.Add(destination, _T("the backup destination"));
      exclusions.Add(temporaryDirectory, _T("the temp directory"));
      if (use == ForTheEmptying_ && !archiveFile.IsEmpty())
         exclusions.Add(String(boost::filesystem::path(archiveFile.c_str()).parent_path().wstring()), _T("the folder of the archive being restored"));
      exclusions.Add(stagedStore, _T("the message store being restored"));
   }

   static bool
   CopyTree_(const boost::filesystem::path &from, const boost::filesystem::path &to, TreeCopyOutcome &outcome,
             bool skipKeptOutOfBackup = false, const DataDirectoryExclusions_ *exclusions = nullptr,
             ExclusionSide_ side = ExcludeBySource_)
   {
      boost::system::error_code createError;
      boost::filesystem::create_directories(to, createError);

      if (createError)
      {
         outcome.failed_file = String(to.wstring());
         outcome.failure_detail = createError.message().c_str();
         return false;
      }

      boost::system::error_code iterateError;
      boost::filesystem::directory_iterator file(from, iterateError);

      if (iterateError)
      {
         // A source directory that cannot be listed is a real failure, including the
         // case where it does not exist. CopyDirectory threw std::logic_error for
         // that one, which no caller was catching.
         outcome.failed_file = String(from.wstring());
         outcome.failure_detail = iterateError.message().c_str();
         return false;
      }

      boost::filesystem::directory_iterator end;

      for (; file != end; ++file)
      {
         boost::filesystem::path current(file->path());

         boost::system::error_code statusError;
         bool isDirectory = boost::filesystem::is_directory(current, statusError);

         if (statusError)
         {
            // Listed, and then we could not ask what it was. Almost always because it
            // has just been deleted - but only "almost", so the entry has to be shown
            // to be gone before it is written off. Skipping a directory we simply
            // could not read would leave a whole account out of the backup.
            boost::system::error_code existsError;
            bool stillExists = boost::filesystem::exists(current, existsError);

            if (!existsError && !stillExists)
            {
               outcome.vanished++;
               continue;
            }

            outcome.failed_file = String(current.wstring());
            outcome.failure_detail = statusError.message().c_str();
            return false;
         }

         if (isDirectory)
         {
            // At the data directory's root only; see IsKeptOutOfBackup_.
            if (skipKeptOutOfBackup && IsKeptOutOfBackup_(current))
               continue;

            // At any depth; see DataDirectoryExclusions_.
            const boost::filesystem::path target = to / current.filename();
            String excludedAs;
            if (exclusions && exclusions->Contains(side == ExcludeBySource_ ? current : target, excludedAs))
            {
               outcome.excluded.push_back(excludedAs);

               // A backup's staging folder can already hold a copy of the
               // directory it leaves out: the nested tree a copy into itself left
               // behind up to 6.3.5, which a later run would otherwise archive.
               // It is a copy, in the backup's own folder, of something that is
               // not mail, so it goes - never a directory that is itself one of
               // the exclusions.
               String alsoExcluded;
               boost::system::error_code staleError;
               if (side == ExcludeBySource_ && boost::filesystem::exists(target, staleError) && !staleError &&
                   !exclusions->Contains(target, alsoExcluded))
               {
                  boost::filesystem::remove_all(target, staleError);
               }

               continue;
            }

            if (!CopyTree_(current, target, outcome, false, exclusions, side))
               return false;
         }
         else
         {
            // A backup archive in the data directory's root is there because the
            // destination is the data directory itself. It is not mail, and the
            // staged root's files are deleted before the archive is written
            // anyway (BackupDataDirectory_), so copying every earlier archive
            // into the staging folder first would only cost their size again.
            DateTime created;
            if (skipKeptOutOfBackup && BackupRetention::ParseArchiveName(String(current.filename().wstring()), created))
               continue;

            if (!CopyOneFile_(current, to / current.filename(), outcome))
               return false;
         }
      }

      return true;
   }

   static bool
   CopyTreeGuarded_(const String &from, const String &to, TreeCopyOutcome &outcome, bool skipKeptOutOfBackup,
                    const DataDirectoryExclusions_ *exclusions = nullptr, ExclusionSide_ side = ExcludeBySource_)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // CopyTree_ with a backstop. Every operation inside it uses an error_code
   // overload, so nothing should reach here - but the whole point of this code is
   // that an exception escaping a work-queue task terminates the process, and that
   // property must not depend on having enumerated every throwing call correctly.
   // Both callers copy a data directory, and both leave out what
   // IsKeptOutOfBackup_ names at its root.
   //---------------------------------------------------------------------------()
   {
      try
      {
         return CopyTree_(boost::filesystem::path(from.c_str()), boost::filesystem::path(to.c_str()), outcome, skipKeptOutOfBackup, exclusions, side);
      }
      catch (boost::thread_interrupted const &)
      {
         // The service is shutting down. Task::Run handles this one and must go on
         // being allowed to.
         throw;
      }
      catch (std::exception const &exception)
      {
         outcome.failed_file = from;
         outcome.failure_detail = exception.what();
         return false;
      }
      catch (...)
      {
         outcome.failed_file = from;
         outcome.failure_detail = "an unrecognised error";
         return false;
      }
   }

   static String
   DescribeCopyFailure_(const TreeCopyOutcome &outcome)
   {
      return Formatter::Format("{0} could not be copied: {1}.", outcome.failed_file, outcome.failure_detail);
   }

   static bool
   EmptyAround_(const boost::filesystem::path &directory, const DataDirectoryExclusions_ &exclusions, std::vector<String> &kept)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Removes everything in directory - a directory that holds one of the
   // exclusions, somewhere below it - but the exclusions and the directories on
   // the way to them, which are emptied around them in turn. A link is removed
   // as the link it is, never followed: remove_all, which this stands in for,
   // does not follow one either. Listed first and removed after.
   //---------------------------------------------------------------------------()
   {
      std::vector<boost::filesystem::path> entries;

      boost::system::error_code iterateError;
      for (boost::filesystem::directory_iterator file(directory, iterateError);
           !iterateError && file != boost::filesystem::directory_iterator(); file.increment(iterateError))
      {
         entries.push_back(file->path());
      }

      if (iterateError)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5700, "BackupExecuter::EmptyAround_",
            Formatter::Format("Could not list the directory {0}: {1}.", String(directory.wstring()), String(iterateError.message().c_str())));
         return false;
      }

      for (const boost::filesystem::path &current : entries)
      {
         String excludedAs;
         if (exclusions.Contains(current, excludedAs))
         {
            kept.push_back(excludedAs);
            continue;
         }

         boost::system::error_code statusError;
         boost::filesystem::file_status linkStatus = boost::filesystem::symlink_status(current, statusError);

         if (!statusError && boost::filesystem::is_directory(linkStatus) && exclusions.Holds(current))
         {
            if (!EmptyAround_(current, exclusions, kept))
               return false;
            continue;
         }

         boost::system::error_code removeError;
         boost::filesystem::remove_all(current, removeError);

         if (removeError)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5700, "BackupExecuter::EmptyAround_",
               Formatter::Format("Could not delete {0}: {1}.", String(current.wstring()), String(removeError.message().c_str())));
            return false;
         }
      }

      return true;
   }

   static bool
   DeleteDataDirectoriesForRestore_(const String &dataDirectory, const DataDirectoryExclusions_ &exclusions, std::vector<String> &kept)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // FileUtilities::DeleteDirectoriesInDirectory, less what IsKeptOutOfBackup_
   // names: every directory at the data directory's root is removed but the
   // recoverable store, the eDiscovery cases' files, the stores of messages
   // that expire and of password-protected messages, and the live update's
   // folder. And less what exclusions holds, wherever it is inside: the backup
   // destination, the temp directory, the archive being restored and the store
   // extracted from it are left where they are, and a directory on the way to
   // one of them is emptied around it rather than removed (EmptyAround_). Until
   // 6.3.6 a destination inside the data directory went with the rest - every
   // archive in it - and on a Linux package, whose temp directory is
   // /var/lib/hmailserver/temp, so did the store the restore had just
   // extracted and was about to copy back. Listed first and removed after, so
   // the listing is not walked while it changes.
   //---------------------------------------------------------------------------()
   {
      std::vector<boost::filesystem::path> doomed;

      boost::system::error_code iterateError;
      for (boost::filesystem::directory_iterator file(boost::filesystem::path(dataDirectory.c_str()), iterateError);
           !iterateError && file != boost::filesystem::directory_iterator(); ++file)
      {
         boost::filesystem::path current(file->path());

         boost::system::error_code statusError;
         if (boost::filesystem::is_directory(current, statusError) && !statusError && !IsKeptOutOfBackup_(current))
            doomed.push_back(current);
      }

      for (const boost::filesystem::path &directory : doomed)
      {
         String excludedAs;
         if (exclusions.Contains(directory, excludedAs))
         {
            kept.push_back(excludedAs);
            continue;
         }

         boost::system::error_code statusError;
         boost::filesystem::file_status linkStatus = boost::filesystem::symlink_status(directory, statusError);

         if (!statusError && boost::filesystem::is_directory(linkStatus) && exclusions.Holds(directory))
         {
            if (!EmptyAround_(directory, exclusions, kept))
               return false;
            continue;
         }

         boost::system::error_code removeError;
         boost::filesystem::remove_all(directory, removeError);

         if (removeError)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5700, "BackupExecuter::DeleteDataDirectoriesForRestore_",
               Formatter::Format("Could not delete the directory {0}: {1}.", String(directory.wstring()), String(removeError.message().c_str())));
            return false;
         }
      }

      return true;
   }

   static bool
   DeleteDataFilesForRestore_(const String &dataDirectory, const String &archiveFile)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // FileUtilities::DeleteFilesInDirectory on the data directory's root - the
   // queue's message files, which a restore does not carry back - less the
   // backup archives there: a file named as BackupRetention names one, which is
   // in the data directory's root only when the destination is the data
   // directory itself, and the archive being restored, whatever it is called.
   //---------------------------------------------------------------------------()
   {
      DirectoryIdentity_ archiveIdentity;
      const bool archiveKnown = !archiveFile.IsEmpty() && IdentifyDirectory_(boost::filesystem::path(archiveFile.c_str()), archiveIdentity);

      std::vector<boost::filesystem::path> doomed;

      boost::system::error_code iterateError;
      for (boost::filesystem::directory_iterator file(boost::filesystem::path(dataDirectory.c_str()), iterateError);
           !iterateError && file != boost::filesystem::directory_iterator(); file.increment(iterateError))
      {
         boost::filesystem::path current(file->path());

         boost::system::error_code statusError;
         if (boost::filesystem::is_directory(current, statusError))
            continue;

         DateTime created;
         if (BackupRetention::ParseArchiveName(String(current.filename().wstring()), created))
            continue;

         DirectoryIdentity_ identity;
         if (archiveKnown && IdentifyDirectory_(current, identity) && !(identity < archiveIdentity) && !(archiveIdentity < identity))
            continue;

         doomed.push_back(current);
      }

      bool allDeleted = true;

      for (const boost::filesystem::path &current : doomed)
      {
         boost::system::error_code removeError;
         boost::filesystem::remove(current, removeError);

         if (removeError && allDeleted)
         {
            allDeleted = false;
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5700, "BackupExecuter::DeleteDataFilesForRestore_",
               Formatter::Format("Could not delete the file {0}: {1}.", String(current.wstring()), String(removeError.message().c_str())));
         }
      }

      return allDeleted;
   }

   static bool
   MeasureTree_(const boost::filesystem::path &root, unsigned int &files, unsigned __int64 &bytes,
      std::set<String> *names, String &failureDetail, const DataDirectoryExclusions_ *exclusions = nullptr)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Counts the regular files under root and adds up their sizes, optionally
   // collecting their lower-cased names. Symbolic links are neither followed nor
   // counted: the backup copies real files, so a link inside the staged or the
   // extracted tree would itself be a discrepancy, and the count is how it shows.
   // Given exclusions, root is the data directory and what the copy leaves out
   // is not counted: the directories DataDirectoryExclusions_ holds, those
   // IsKeptOutOfBackup_ names at the root and the backup archives in the root
   // - so that an encrypted backup's check
   // of the temp directory's room is not failed by the archives of a
   // destination inside the data directory, which are never staged.
   // Every call uses an error_code overload for the reason given at the top of
   // this file: an exception escaping the backup task terminates the service.
   //---------------------------------------------------------------------------()
   {
      boost::system::error_code error;
      boost::filesystem::recursive_directory_iterator entry(root, boost::filesystem::directory_options::none, error);

      if (error)
      {
         failureDetail = Formatter::Format("{0} could not be listed: {1}.", String(root.wstring()), String(error.message().c_str()));
         return false;
      }

      boost::filesystem::recursive_directory_iterator end;

      while (entry != end)
      {
         boost::filesystem::file_status status = entry->symlink_status(error);

         if (error)
         {
            failureDetail = Formatter::Format("{0} could not be examined: {1}.", String(entry->path().wstring()), String(error.message().c_str()));
            return false;
         }

         String excludedAs;
         DateTime archiveCreated;
         if (exclusions && boost::filesystem::is_directory(status) &&
             ((entry.depth() == 0 && IsKeptOutOfBackup_(entry->path())) || exclusions->Contains(entry->path(), excludedAs)))
         {
            entry.disable_recursion_pending();
         }
         else if (exclusions && entry.depth() == 0 && boost::filesystem::is_regular_file(status) &&
                  BackupRetention::ParseArchiveName(String(entry->path().filename().wstring()), archiveCreated))
         {
            // A backup archive at the data directory's root, which is there when
            // the destination is the data directory itself and which the copy
            // does not stage either (CopyTree_): counted, a destination keeping
            // a week of archives there asked the temp directory for room for all
            // of them as well as the store.
         }
         else if (boost::filesystem::is_regular_file(status))
         {
            unsigned __int64 size = boost::filesystem::file_size(entry->path(), error);

            if (error)
            {
               failureDetail = Formatter::Format("{0} could not be measured: {1}.", String(entry->path().wstring()), String(error.message().c_str()));
               return false;
            }

            files++;
            bytes += size;

            if (names)
            {
               String name(entry->path().filename().wstring());
               names->insert(name.ToLower());
            }
         }

         entry.increment(error);

         if (error)
         {
            failureDetail = Formatter::Format("{0} could not be listed: {1}.", String(root.wstring()), String(error.message().c_str()));
            return false;
         }
      }

      return true;
   }

   static void
   CollectMessageFilenames_(XNode *node, std::vector<String> &filenames)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Every Filename a Message element in the backup index carries, wherever the
   // element sits - an account's folders, a public folder - since Message::XMLStore
   // is the one writer and stores the bare file name.
   //---------------------------------------------------------------------------()
   {
      if (!node)
         return;

      if (node->name == _T("Message"))
      {
         LPXAttr filename = node->GetAttr(_T("Filename"));

         if (filename && !filename->value.IsEmpty())
            filenames.push_back(filename->value);
      }

      for (int i = 0; i < node->GetChildCount(); i++)
         CollectMessageFilenames_(node->GetChild((unsigned int) i), filenames);
   }

   static bool
   DatabaseReadsFailed_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // True, and the backup has been failed, when at least one database read since
   // the enclosing DatabaseUnavailableMarker::Scope did not run.
   //
   // Note the distinction the marker exists to draw: "did not run" is not the same
   // as "ran and returned nothing". A server with no domains configured must still
   // produce a (correctly empty) backup, so an empty result on its own cannot be
   // treated as a failure - which is precisely why the empty archive described at
   // the call site was indistinguishable from a legitimate one.
   //
   // A file-scope function rather than a member because BackupExecuter.h is not
   // part of this change, and the message must be identical at both call sites: an
   // administrator comparing two backup logs should not have to work out whether
   // two different wordings mean the same thing.
   //---------------------------------------------------------------------------()
   {
      if (!DatabaseUnavailableMarker::IsMarked())
         return false;

      Application::Instance()->GetBackupManager()->OnBackupFailed(
         "The database stopped answering while the domains, accounts and settings were being read, so the backup would have been incomplete. No backup file has been written and the previous backups have not been touched.");

      return true;
   }

   static String
   ArchiverEncryptionHint_(bool encrypting)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What to add to a failure that happened while writing an ENCRYPTED archive.
   // The archiver is the system's on Linux and whichever of four names was found
   // on the path (Compression::GetExecutableFullPath_), and one of them - 7zr,
   // p7zip's reduced build - has no encryption in it at all and fails every such
   // run with an unhelpful exit code. Naming it costs one sentence and saves an
   // administrator the afternoon it would otherwise take.
   //---------------------------------------------------------------------------()
   {
      if (!encrypting)
         return String();

      return " This archive was to be encrypted, and not every archiver can encrypt: 7zr, the reduced p7zip build, cannot, and fails here. Install 7-Zip (7zz) or p7zip-full, or clear the backup passphrase.";
   }

   class StagingDirectoryGuard
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Removes the directory an encrypted backup stages its PLAINTEXT in, on every
   // way out of StartBackup - the success, the twenty-odd refusals, and the
   // exception that is not supposed to be able to get out of a work-queue task.
   //
   // It exists because of where that directory is. An unencrypted backup stages
   // in the destination and its own code deletes it; an encrypted one cannot,
   // because the destination is a share or a cloud mount somebody chose on the
   // understanding that what lands there is unreadable - see StartBackup. So the
   // staging moves to the temp directory, and a copy of every mailbox on the
   // server left lying about in the temp directory because a backup failed on its
   // way past is not a better outcome than the one that was being avoided. This
   // makes the deletion the compiler's job rather than a reviewer's.
   //---------------------------------------------------------------------------()
   {
   public:

      StagingDirectoryGuard()
      {
      }

      ~StagingDirectoryGuard()
      {
         Remove();
      }

      void Hold(const String &directory)
      {
         directory_ = directory;
      }

      void Remove()
      {
         if (directory_.IsEmpty())
            return;

         String directory = directory_;
         directory_.Empty();

         if (!FileUtilities::Exists(directory))
            return;

         // No exception may leave here: this runs from a destructor, and one that
         // escapes a work-queue task terminates the service. DeleteDirectory is
         // error_code-based throughout, and the catch is the backstop for that
         // being true rather than a substitute for it.
         try
         {
            if (FileUtilities::DeleteDirectory(directory, true))
               return;
         }
         catch (...)
         {
         }

         // Said in the backup log rather than swallowed: it names a directory
         // that has a readable copy of the message store in it, and nothing else
         // is going to mention it.
         Logger::Instance()->LogBackup("The staging directory " + directory + " could not be removed. It holds an unencrypted copy of the message files this backup was working on - delete it by hand.");
      }

   private:

      StagingDirectoryGuard(const StagingDirectoryGuard &);
      StagingDirectoryGuard &operator=(const StagingDirectoryGuard &);

      String directory_;
   };

   static void
   DiscardIncompleteArchive_(const String &sZipFile, bool existedBeforeThisRun)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Removes the archive this run created, once it is established that it can never
   // be completed.
   //
   // Leaving it behind is worse than it looks. It carries a name BackupRetention
   // recognises as one of ours and a timestamp newer than every real archive in the
   // directory, so from then on it counts towards ScheduledBackupKeepCount and is
   // treated as the most recent backup. A destination that fails late in the run -
   // which is what running out of space looks like - would therefore accumulate
   // truncated archives that push the good ones out of the keep count, and
   // BackupScheduleTask would conclude on the next service start that a recent
   // backup already exists. It is also the file an administrator would reach for
   // first in a disaster, being the newest one there.
   //
   // existedBeforeThisRun is false in every reachable case; the flag is here so
   // that this can never be the code that deletes an archive it did not create. See
   // where it is computed. A file-scope function rather than a member because
   // BackupExecuter.h is not part of this change.
   //---------------------------------------------------------------------------()
   {
      if (existedBeforeThisRun)
         return;

      if (!FileUtilities::Exists(sZipFile))
         return;

      if (FileUtilities::DeleteFile(sZipFile))
      {
         Logger::Instance()->LogBackup("Removed the incomplete archive " + sZipFile + ".");
         return;
      }

      // Not escalated to the error log: the caller already reports the backup
      // failure itself as critical, and this line tells whoever reads that report
      // which file to remove by hand.
      Logger::Instance()->LogBackup("The incomplete archive " + sZipFile + " could not be removed. Delete it by hand - while it is there it counts as a backup for retention purposes.");
   }

   BackupExecuter::BackupExecuter() :
      backup_mode_(0),
      staged_files_(0),
      staged_bytes_(0),
      encrypt_(false)
   {
   }

   BackupExecuter::~BackupExecuter(void)
   {
      // The passphrase this run was given, overwritten rather than left in the
      // freed block: a backup of a real message store runs for hours, and a
      // minidump taken in that window is the one read nobody plans for. See
      // Utilities::SecureWipe for what that does and does not claim.
      Utilities::SecureWipe(passphrase_);
   }

   void
   BackupExecuter::LoadSettings_()
   {
      destination_ = Configuration::Instance()->GetBackupDestination();
      if (destination_.Right(1) == FileUtilities::PathSeparator)
         destination_ = destination_.Left(destination_.GetLength() - 1);

      backup_mode_ = Configuration::Instance()->GetBackupOptions();

      encrypt_ = Configuration::Instance()->GetBackupEncryptionPassphraseSet();
      passphrase_ = Configuration::Instance()->GetBackupEncryptionPassphrase();
   }

   bool
   BackupExecuter::StartBackup()
   {
      Logger::Instance()->LogBackup("Loading backup settings....");

      LoadSettings_();

      // Special temp setting to skip files during backup/restore while still storing/restoring db file/message info.
      bool bMessagesDBOnly = IniFileSettings::Instance()->GetBackupMessagesDBOnly();


      if (backup_mode_ & Backup::BOMessages)
      {
         if (!PersistentMessage::GetAllMessageFilesAreInDataFolder())
         {
            Application::Instance()->GetBackupManager()->OnBackupFailed("All messages are not located in the data folder.");
            return false;
         }
      }

      if (!FileUtilities::Exists(destination_))
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed("The specified backup directory is not accessible: " + destination_);
         return false;
      }

      // THE THREE WAYS ENCRYPTION IS ASKED FOR AND CANNOT BE DELIVERED. All three
      // refuse before anything is written, because the alternative in each case is
      // an archive of every mailbox on the server written in a way somebody would
      // not have chosen: in the clear, to a folder picked on the understanding
      // that it would not be, or under a passphrase nobody has been told about.
      if (encrypt_ && passphrase_.IsEmpty())
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup is set to be encrypted, but the stored passphrase could not be read on this machine - a database carried over from another installation, or a secret store that has changed (the error log says which). No archive has been written: an unencrypted copy of every mailbox is not written in place of an encrypted one. Enter the backup passphrase again on this machine, or clear it to take unencrypted backups on purpose.");
         return false;
      }

      // The same disagreement the other way round: the integer says no passphrase
      // is set while the string still holds one. It is reachable, and once it
      // happens it is permanent - clearing the passphrase is two writes, the flag
      // first and the value second (Configuration::SetBackupEncryptionPassphrase),
      // so a clear interrupted between them leaves exactly this and PropertySet
      // reloads it at every start.
      //
      // It has to refuse rather than carry on, because what actually encrypts an
      // archive is the passphrase and not the flag: Compression::Encryption-
      // Switches_ adds -p -mhe=on whenever it is given a passphrase. So this run
      // would write a fully encrypted archive while the line logged below said it
      // was writing a plain one, and VerifyArchive_ would not catch the
      // contradiction because its read-back check asks the flag as well. That is
      // an archive nobody can open, recorded as one anybody can, produced on the
      // day somebody decided the passphrase was no longer needed and may have
      // thrown it away. Setting the passphrase again or clearing it again makes
      // the two agree, and either is one write.
      if (!encrypt_ && !passphrase_.IsEmpty())
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup settings disagree about encryption: this server has no passphrase recorded as set, and a stored passphrase is still there - which is what a clear that was interrupted half way leaves behind. No archive has been written, because the archive that would have been written is encrypted with a passphrase this server has been told nobody is using. Set the backup passphrase again, or clear it again; either one makes the two agree.");
         return false;
      }

      if (encrypt_ && (backup_mode_ & Backup::BOMessages) && !(backup_mode_ & Backup::BOCompression) && !bMessagesDBOnly)
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup is set to be encrypted, but 'compress destination files' is off, which copies every message file into a DataBackup folder beside the archive instead of putting them inside it - and files beside the archive are not encrypted by anything. No archive has been written. Switch compression on to encrypt the messages too, or clear the backup passphrase.");
         return false;
      }

      // The fourth: the plaintext has nowhere to go. An encrypted run stages the
      // message files in the temp directory instead of in the destination (see
      // where the staging directory is made, below), so the temp volume has to be
      // able to hold the message store for as long as it takes 7-Zip to read it.
      // Measured and refused here, before the copy starts, because the alternative
      // is discovering it an hour in, out of space, with most of a plaintext copy
      // of the store written and the backup failed anyway.
      if (encrypt_ && (backup_mode_ & Backup::BODomains) && (backup_mode_ & Backup::BOMessages) && !bMessagesDBOnly)
      {
         String temporaryDirectory = IniFileSettings::Instance()->GetTempDirectory();

         unsigned int storeFiles = 0;
         unsigned __int64 storeBytes = 0;
         String measureDetail;

         // What BackupDataDirectory_ will stage, as near as can be known before
         // it: the data directory less what the copy leaves out.
         DataDirectoryExclusions_ exclusions;
         AddBackupExclusions_(exclusions, destination_, temporaryDirectory, String());

         if (!MeasureTree_(boost::filesystem::path(IniFileSettings::Instance()->GetDataDirectory().c_str()), storeFiles, storeBytes, 0, measureDetail, &exclusions))
         {
            // Not a refusal. The copy below reads the same tree and reports a real
            // failure with the file that caused it, and refusing a backup because
            // a directory could not be listed for a space estimate would be a
            // backup lost to the estimate rather than to the problem.
            Logger::Instance()->LogBackup("The message store could not be measured before staging, so the free space in the temp directory was not checked. Details: " + measureDetail);
         }
         else
         {
            boost::system::error_code spaceError;
            boost::filesystem::space_info tempSpace = boost::filesystem::space(boost::filesystem::path(temporaryDirectory.c_str()), spaceError);

            const unsigned __int64 margin = 64ull * 1024 * 1024;

            if (!spaceError && tempSpace.available < storeBytes + margin)
            {
               Application::Instance()->GetBackupManager()->OnBackupFailed(Formatter::Format(
                  "The backup is encrypted, so the message files are staged in the temp directory {0} rather than in the destination - the destination is where the encrypted archive goes, and a plaintext copy of every mailbox written there first would defeat the encryption for as long as it sat there. Staging needs {1} byte(s) and that directory has {2} byte(s) free, so nothing has been written. Free space there, point TempDirectory at a disk with room, or clear the backup passphrase to stage in the destination as an unencrypted backup does.",
                  temporaryDirectory, storeBytes + margin, (unsigned __int64) tempSpace.available));

               return false;
            }
         }
      }

      if (encrypt_)
         Logger::Instance()->LogBackup("This archive will be encrypted: 7-Zip AES-256, with the file names encrypted as well, so that nothing in it - not even what it contains - can be read without the backup passphrase.");
      else
         Logger::Instance()->LogBackup("This archive will not be encrypted: no backup passphrase is set.");

      // Refuse, before writing anything, if the parts of the backup that come out
      // of the database cannot come out of the database.
      //
      // Domains::Refresh - and every collection underneath it - returns void and
      // discards the result of its load. A query that fails because the database is
      // down therefore leaves an empty collection, and an empty collection stores no
      // XML at all. So a backup taken during a database outage used to be written,
      // reported as "Backup completed successfully", and contain no domains, no
      // accounts and no aliases. Restoring that archive deletes every domain on the
      // server - Domains::DeleteAll runs first, by design, so that the data
      // directory is not restored and then dropped - and then puts nothing back.
      //
      // That was survivable while a backup only ever happened because somebody
      // asked for one at a moment they had chosen. On a schedule it happens
      // unattended, at the same time as whatever else runs at 02:00, and with
      // retention enabled the hollow archive is also the newest one - so it is the
      // one somebody reaches for, and the good ones are the ones retention has been
      // deleting.
      //
      // GetIsConnected only reports whether the pool holds any connections, so it
      // catches "the pool never came up" and not "the server stopped answering a
      // moment ago". That second case is what the marker checks below are for; this
      // check is here because it is free and it turns the common case into a clean
      // refusal instead of a partly-done backup.
      //
      // Ordered after the destination check deliberately: a missing destination is
      // by far the more common misconfiguration, and its existing message is the one
      // callers and the OnBackupFailed scripting event already expect for it.
      if (backup_mode_ & (Backup::BODomains | Backup::BOSettings))
      {
         std::shared_ptr<DatabaseConnectionManager> databaseManager = Application::Instance()->GetDBManager();

         if (!databaseManager || !databaseManager->GetIsConnected())
         {
            Application::Instance()->GetBackupManager()->OnBackupFailed("The database is not available, so the domains, accounts and settings could not be read. No backup file has been written and the previous backups have not been touched.");
            return false;
         }
      }

      // Everything read out of the database from here on is watched. The marker is
      // thread-local and this whole function runs on one work-queue thread, so the
      // scope covers the reads below without any of them having to be moved; it also
      // clears any stale failure left on this thread by earlier work, which is why
      // it is a Scope and not a bare IsMarked() call.
      DatabaseUnavailableMarker::Scope databaseScope;

      String sTime = Time::GetCurrentDateTime();
      sTime.Replace(_T(":"), _T(""));

      // Generate name for zip file. We always create zip
      // file
      String sZipFile = destination_ + FileUtilities::PathSeparator + _T("HMBackup ") + sTime + _T(".7z");

      // Whether the archive already existed before this run touched it. Nothing
      // should be able to make that true - the name carries the local time to the
      // second and only one backup may run at a time - but the failure paths below
      // delete the archive they created, and "did it exist before?" is what makes
      // that provably a deletion of our own half-written file rather than of
      // somebody's backup.
      bool zipExistedBeforeThisRun = FileUtilities::Exists(sZipFile);

      // WHERE THE PLAINTEXT SITS WHILE THE ARCHIVE IS BEING WRITTEN, and why an
      // encrypted backup cannot leave it in the destination.
      //
      // Both of the things that go into the archive are written to a file first.
      // The index carries every domain, account and alias, and the server's own
      // stored secrets with them; the message files are copied whole into a
      // DataBackup folder, because 7-Zip is given a directory to archive. Both
      // used to be written into the destination, archived from there and deleted -
      // which for an unencrypted backup is unremarkable, since the archive beside
      // them is readable too.
      //
      // For an encrypted one it is the hole the encryption exists to close. The
      // destination is whatever an administrator typed: often a share, sometimes a
      // cloud mount whose sync client uploads a file as it appears. A plaintext
      // copy of every mailbox on the server, sitting there for the minutes or
      // hours the compression takes, is the same exposure as an unencrypted
      // backup - and on a failure it stays there.
      //
      // So an encrypted run stages in a directory of its own under TempDirectory,
      // which is local and is already where the verified restore extracts to.
      //
      // The archive is unchanged by the move, which was measured rather than
      // assumed: given an ABSOLUTE path, 7-Zip names members from the last
      // component onwards and nothing before it. "7za a a.7z <abs>\DataBackup\ -r"
      // stores DataBackup\one.txt and DataBackup\sub\two.txt wherever the parent
      // is, and "7za a a.7z <abs>\hMailServerBackup.xml" stores the bare name -
      // checked with the bundled 7za 19.00 against both paths. (Given a RELATIVE
      // path it stores the path as given, which is why this is about absolute
      // paths: both of these are, as the destination has always been.)
      //
      // The guard removes the directory on every exit path; the free space it
      // needs was checked above.
      String stagingRoot = destination_;
      StagingDirectoryGuard stagingGuard;

      if (encrypt_)
      {
         String encryptedStagingDirectory = Utilities::GetUniqueTempDirectory();

         if (!FileUtilities::CreateDirectory(encryptedStagingDirectory))
         {
            Application::Instance()->GetBackupManager()->OnBackupFailed(
               "The backup is encrypted and the directory it stages the message files and the index in, " + encryptedStagingDirectory + ", could not be created. Nothing has been written. The staging directory is under TempDirectory rather than in the destination, because what is staged is in the clear.");

            return false;
         }

         stagingGuard.Hold(encryptedStagingDirectory);
         stagingRoot = encryptedStagingDirectory;
      }

      String sXMLFile = stagingRoot + FileUtilities::PathSeparator + _T("hMailServerBackup.xml");

      // The name of the backup directory that
      // contains all the data files.
      String sDataBackupDir = stagingRoot + FileUtilities::PathSeparator + "DataBackup";

      // Backup all properties.
      XDoc oDoc;

      XNode *pBackupNode = oDoc.AppendChild(_T("Backup"));
      XNode *pBackupInfoNode = pBackupNode->AppendChild(_T("BackupInformation"));

      // Store backup mode
      pBackupInfoNode->AppendAttr(_T("Mode"), StringParser::IntToString(backup_mode_));
      pBackupInfoNode->AppendAttr(_T("Version"), Application::Instance()->GetVersionNumber());

      // The custom roles, a server-wide store beside the settings, written
      // whenever the archive carries the accounts or the settings. The accounts
      // carry their role assignments by the role's name (AccountStores), and a
      // custom role is a row of this server's hm_roles that a fresh server does
      // not have - so without this an archive restored onto one dropped every
      // assignment of a custom role. The restore puts these back before the
      // domains; see StartRestore and RoleManager.h.
      if (backup_mode_ & (Backup::BODomains | Backup::BOSettings))
      {
         Logger::Instance()->LogBackup("Backing up custom roles...");

         if (!RoleManager::XMLStore(pBackupNode))
         {
            Application::Instance()->GetBackupManager()->OnBackupFailed("The custom roles could not be read, so they could not be written to the backup. No archive has been created.");
            return false;
         }
      }

      // Backup business objects
      if (backup_mode_ & Backup::BODomains)
      {
         Logger::Instance()->LogBackup("Backing up domains...");

         if (!BackupDomains_(pBackupNode))
         {
            Application::Instance()->GetBackupManager()->OnBackupFailed("Could not backup domains.");
            return false;
         }

         // Checked here as well as at the end, and not only to fail earlier: the
         // data-directory copy below can run for hours on a real message store, and
         // copying an entire mail store to produce an archive we already know will
         // be refused is hours of disk contention on a live server for nothing.
         if (DatabaseReadsFailed_())
            return false;

         // Backup message files
         if (backup_mode_ & Backup::BOMessages && !bMessagesDBOnly)
         {
            Logger::Instance()->LogBackup("Backing up data directory...");
            if (!BackupDataDirectory_(sDataBackupDir))
            {
               Application::Instance()->GetBackupManager()->OnBackupFailed("Could not backup data directory.");
               return false;
            }


         }
      }

      // Save information in the XML file where messages can be found.
      if (backup_mode_ & Backup::BOMessages)
      {
         XNode *pMessageFile = pBackupInfoNode->AppendChild(_T("DataFiles"));

         if (backup_mode_ & Backup::BOCompression)
         {
            pMessageFile->AppendAttr(_T("Format"), _T("7z"));
            // FileSize returns long, and IntToString has an int, an unsigned int and
            // an __int64 overload - three integral conversions of equal rank, so the
            // call is ambiguous to a conforming compiler and only MSVC's rule that
            // int and long convert trivially picks one. The cast names the overload
            // MSVC picks, IntToString(int), and loses nothing: FileSize saturates at
            // LONG_MAX and never returns a negative.
            pMessageFile->AppendAttr(_T("Size"), StringParser::IntToString((int) FileUtilities::FileSize(sZipFile)));
         }
         else
         {
            pMessageFile->AppendAttr(_T("Format"), _T("Raw"));
            pMessageFile->AppendAttr(_T("FolderName"), _T("DataBackup"));
         }
      }

      if (backup_mode_ & Backup::BOSettings)
      {
         Logger::Instance()->LogBackup("Backing up settings...");

         // Checked, which it was not - and until Configuration::XMLStore was made
         // capable of answering false there was nothing here to check. The two went
         // together: a settings section that could not fail, called by a caller that
         // would not have listened.
         if (!Configuration::Instance()->XMLStore(pBackupNode))
         {
            Application::Instance()->GetBackupManager()->OnBackupFailed("The server settings could not be written to the backup. Check the hMailServer error log. No archive has been created.");
            return false;
         }
      }

      // The last database read has happened. Nothing has been written to the
      // destination yet, which is the property that makes refusing here safe: the
      // previous backups are untouched and no partial archive exists to confuse the
      // next retention pass.
      if (DatabaseReadsFailed_())
         return false;

      Logger::Instance()->LogBackup(_T("Writing XML file..."));
      String sXMLData = oDoc.GetXML();
      if (!FileUtilities::WriteToFile(sXMLFile, sXMLData, true))
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed("Could not write to the XML file.");
         return false;
      }

      // Compress the XML file. The result is checked before the source is deleted:
      // an archive that could not be written must fail the backup, not be reported
      // as complete and then have the only other copy removed.
      Compression oComp;
      bool xmlCompressed = oComp.AddFile(sZipFile, sXMLFile, passphrase_);

      // Delete the XML file
      FileUtilities::DeleteFile(sXMLFile);

      if (!xmlCompressed)
      {
         DiscardIncompleteArchive_(sZipFile, zipExistedBeforeThisRun);

         Application::Instance()->GetBackupManager()->OnBackupFailed("Could not add the backup settings to the archive. The backup is incomplete." + ArchiverEncryptionHint_(encrypt_));
         return false;
      }

      // Should we compress the message files?
      if (backup_mode_ & Backup::BOMessages &&
          backup_mode_ & Backup::BOCompression && !bMessagesDBOnly)
      {
         Logger::Instance()->LogBackup("Compressing message files...");

         // Same again, and it matters more here: the staged copy below is deleted
         // straight afterwards, so a compression failure that went unnoticed would
         // leave a truncated archive and no message files at all, announced as a
         // successful backup.
         if (backup_mode_ & Backup::BOMessages)
         {
            if (!oComp.AddDirectory(sZipFile, sDataBackupDir + FileUtilities::PathSeparator, passphrase_))
            {
               DiscardIncompleteArchive_(sZipFile, zipExistedBeforeThisRun);

               // An unencrypted run leaves the staged copy where it is and says
               // where: it is in the destination, it is the same mail that is
               // already there in the clear, and it is worth keeping while
               // somebody works out what went wrong. An encrypted one cannot -
               // the staged copy is plaintext and the guard removes it - so it
               // says that instead, rather than naming a directory that will not
               // be there.
               Application::Instance()->GetBackupManager()->OnBackupFailed(encrypt_
                  ? String("Could not add the message files to the archive. The backup is incomplete. The staged copy of the message files has been removed: this backup is encrypted, so it was staged in the clear under the temp directory and is not left lying about." + ArchiverEncryptionHint_(true))
                  : String("Could not add the message files to the archive. The backup is incomplete; the staged message files have been left in " + sDataBackupDir + "."));
               return false;
            }
         }

         // Since the files are now compressed, we can deleted
         // the data backup directory
         if (!FileUtilities::DeleteDirectory(sDataBackupDir, true))
         {
            // The archive itself is complete at this point, so it is deliberately
            // left alone: this failure is about tidying up the staging folder, and
            // deleting a good archive because we could not delete a temporary
            // directory would be a far worse outcome than the untidy destination.
            //
            // Which directory it is depends on the run, and it matters enough to
            // say: an encrypted backup staged in the clear under the temp
            // directory, and a staged copy that could not be deleted there is a
            // readable copy of the mail the archive beside it is protecting.
            Application::Instance()->GetBackupManager()->OnBackupFailed(encrypt_
               ? String("The archive was written, but the staged copy of the message files in " + sDataBackupDir + " could not be deleted. That copy is not encrypted - this backup staged it under the temp directory so that it would never be written to the destination - so delete it by hand.")
               : String("Could not delete files from the destination directory."));
            return false;
         }
       }

      // The archive is read back before it is called a backup.
      //
      // Everything above checks that each step reported success. None of it checks
      // that the result can be opened - and "reported success but cannot be opened"
      // is exactly what a destination that ran out of space part-way through a write
      // produces, along with an archive on a share that was interrupted, and one
      // 7za's exit code called a warning rather than an error. An archive nobody has
      // ever opened is not a backup; the trailing zero in 3-2-1-1-0 is "verified
      // restores", and this is the smallest honest version of it.
      //
      // It happens before retention on purpose. An unverifiable archive must never be
      // the reason an older archive that could be restored was deleted.
      if (!VerifyArchive_(sZipFile))
      {
         DiscardIncompleteArchive_(sZipFile, zipExistedBeforeThisRun);
         return false;
      }

      // And then the mail inside it. Only when there is a message store to extract
      // - BackupMessagesDBOnly writes rows and no files - and only while the
      // administrator has not turned it off, which is said in the log rather than
      // left to be inferred from a line that is missing.
      if ((backup_mode_ & Backup::BOMessages) && (backup_mode_ & Backup::BODomains) && !bMessagesDBOnly)
      {
         if (IniFileSettings::Instance()->GetBackupVerifyRestore())
         {
            bool discardArchive = false;

            if (!VerifyRestore_(sZipFile, pBackupNode, discardArchive))
            {
               if (discardArchive)
                  DiscardIncompleteArchive_(sZipFile, zipExistedBeforeThisRun);
               else
                  Logger::Instance()->LogBackup("The archive " + sZipFile + " has been left in place. It has not been verified and does not count as a successful backup until one is.");

               return false;
            }
         }
         else
         {
            Logger::Instance()->LogBackup("Verified restore skipped: BackupVerifyRestore is 0, so the message store was not extracted and checked.");
         }
      }

      // Retention runs here and nowhere else. This is the one point that every
      // failure path above has already returned past, so the new archive is
      // complete on disk before anything old is even considered for deletion. That
      // ordering is the whole difference between retention and a data-loss feature:
      // a destination with room for one more archive but not two would otherwise
      // have had yesterday's deleted and today's fail to write, leaving nothing at
      // all.
      //
      // It runs before OnBackupCompleted rather than after, so that "Backup
      // completed successfully" in the backup log means the destination is already
      // in its final state. Anything watching for that line - the administrative
      // UI, the regression tests, an administrator's OnBackupCompleted script -
      // would otherwise be racing the deletions.
      //
      // Deliberately not conditional on the run having been started by the
      // schedule. A policy that only bounded scheduled runs would stop bounding the
      // directory the moment somebody took a backup by hand into the same place,
      // which is exactly when the destination is fullest.
      BackupRetention::Apply(destination_, sZipFile,
         IniFileSettings::Instance()->GetScheduledBackupKeepCount(),
         IniFileSettings::Instance()->GetScheduledBackupMaxAgeDays());

      Application::Instance()->GetBackupManager()->OnBackupCompleted();

      return true;
   }

   bool
   BackupExecuter::BackupDataDirectory_(const String &sDataBackupDir)
   {
      String sDataDir = IniFileSettings::Instance()->GetDataDirectory();

      // See the comment at the top of this file for why this is not
      // FileUtilities::CopyDirectory any more. In short: that call terminated the
      // service if a message file was deleted while the copy was running, or if a
      // DataBackup folder from an earlier run was still there.
      TreeCopyOutcome outcome;

      // The staging folder is made first, so that it is a directory the copy
      // can recognise when it meets it inside the data directory; CopyTree_
      // would make it anyway, and says so if it cannot.
      boost::system::error_code createError;
      boost::filesystem::create_directories(boost::filesystem::path(sDataBackupDir.c_str()), createError);

      // What the copy must not walk into, wherever it lies inside the data
      // directory: see DataDirectoryExclusions_. The staging root is the
      // destination for a backup that is not encrypted and a folder of its own
      // under the temp directory for one that is.
      DataDirectoryExclusions_ exclusions;
      AddBackupExclusions_(exclusions, destination_, IniFileSettings::Instance()->GetTempDirectory(), sDataBackupDir);

      // Without the recoverable store, the eDiscovery cases' files, the stores
      // of messages that expire and password-protected messages, and the live
      // update's folder: see IsKeptOutOfBackup_.
      bool bResult = CopyTreeGuarded_(sDataDir, sDataBackupDir, outcome, true, &exclusions);

      // Said at every run, since it is so at every run: what is inside the data
      // directory and was not copied. A destination there is where the backup
      // is meant to go, and it works; the line is what tells an administrator
      // reading the log that it lives on the disk it backs up.
      for (const String &excluded : outcome.excluded)
         Logger::Instance()->LogBackup(Formatter::Format("Left out of the copy of the data directory {0}: {1}, which is inside it.", sDataDir, excluded));

      if (!bResult)
      {
         Logger::Instance()->LogBackup("Failed to copy data directory. Details: " + DescribeCopyFailure_(outcome));
         return bResult;
      }

      if (outcome.vanished > 0)
      {
         // Worth a line, and worth being honest about what it means: a backup of a
         // running server is not a snapshot. The message rows were read out of the
         // database before this copy started, so a message deleted in between leaves
         // a row in the archive whose file is not in it, and after a restore that
         // message is listed but cannot be fetched. Small numbers are simply what
         // mail flow looks like; a large number means the backup was taken during
         // something like a bulk expunge and is worth taking again.
         String line;
         line.Format(_T("Backed up %u file(s). %u file(s) were deleted by the running server while the copy was in progress and are not in this backup."),
            outcome.copied, outcome.vanished);

         Logger::Instance()->LogBackup(line);
      }

      // The messages an object store keeps (docs/ObjectStorage.md), which the
      // copy of the data directory above did not find: read into the staged
      // tree under the names they have there, so that the archive holds every
      // message whichever backend holds it now. A store that cannot be read
      // fails the backup rather than let it be taken without them. Nothing is
      // asked while no store is configured.
      unsigned int fetched = 0;
      String objectProblem;
      if (!MessageStoreObjects::StageInto(sDataBackupDir, fetched, objectProblem))
      {
         Logger::Instance()->LogBackup("Failed to read the messages the object store keeps into the backup. Details: " + objectProblem);
         return false;
      }

      if (fetched > 0)
      {
         String line;
         line.Format(_T("Read %u message(s) and part(s) held once from the object store into the backup."), fetched);
         Logger::Instance()->LogBackup(line);
      }

      bResult = FileUtilities::DeleteFilesInDirectory(sDataBackupDir);

      if (!bResult)
      {
         Logger::Instance()->LogBackup("Failed to delete files in backup root directory. Please see hMailServer error log.");
         return false;
      }

      // Measured now, after the root files have gone, so that this is exactly the
      // tree about to be compressed: the number the extracted store is held to in
      // VerifyRestore_.
      staged_files_ = 0;
      staged_bytes_ = 0;

      String detail;
      if (!MeasureTree_(boost::filesystem::path(sDataBackupDir.c_str()), staged_files_, staged_bytes_, 0, detail))
      {
         Logger::Instance()->LogBackup("Failed to measure the staged message files. Details: " + detail);
         return false;
      }

      return true;
   }

   bool
   BackupExecuter::BackupDomains_(XNode *pBackupNode)
   {
      std::shared_ptr<Domains> pDomains = std::shared_ptr<Domains>(new Domains);
      pDomains->Refresh();

      // The result is returned rather than discarded. Collection::XMLStore stops and
      // returns false the first time one of its items cannot be stored, so
      // discarding it meant a domain whose accounts or aliases could not be written
      // produced an archive containing the domains up to that point - and that was
      // still reported as a successful backup.
      //
      // Refresh itself returns void and cannot be checked here: at this level a
      // failed load is indistinguishable from a server that has no domains, which is
      // why the caller consults DatabaseUnavailableMarker instead.
      return pDomains->XMLStore(pBackupNode, backup_mode_);
   }

   bool
   BackupExecuter::VerifyArchive_(const String &sZipFile)
   {
      Logger::Instance()->LogBackup("Verifying the archive...");

      int archiveContents = 0;
      String archiveVersion;
      String failureReason;

      // Deliberately the same code path a restore uses to open an archive, rather
      // than a private check of its own. A verification that passes where a restore
      // would fail is worth nothing, and the only way to keep the two honest is for
      // them to be one function.
      if (!BackupRestorer::ReadArchiveIndex(sZipFile, passphrase_, archiveContents, archiveVersion, failureReason))
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup was written but could not be read back afterwards, so it is not a usable backup. " + failureReason);

         return false;
      }

      // Read back from the archive's own header rather than believed of the
      // switches that were set. An archiver that took -mhe=on and did nothing with
      // it, or a run that somehow added to an archive left from an unencrypted
      // one, would otherwise pass every other check in this file and leave a
      // readable copy of every mailbox in the destination, reported as a
      // successful encrypted backup.
      if (encrypt_ && Compression::GetArchiveEncryption(sZipFile) != Compression::EncryptionHeaders)
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup was to be encrypted, but the archive reads back with its file names in the clear, so it is not encrypted. It is not the backup that was asked for and has been removed." + ArchiverEncryptionHint_(true));

         return false;
      }

      if (encrypt_)
         Logger::Instance()->LogBackup("The archive is encrypted: its contents cannot be listed, and nothing in it can be read, without the backup passphrase.");

      if (archiveContents != backup_mode_)
      {
         // The index survived but does not say what this run put in it, which means
         // the archive on disk is not the archive this run intended to write.
         String message;
         message.Format(_T("The backup was written but reads back as containing %d rather than %d, so its index does not describe what was backed up. It is not a usable backup."),
            archiveContents, backup_mode_);

         Application::Instance()->GetBackupManager()->OnBackupFailed(message);

         return false;
      }

      return true;
   }

   bool
   BackupExecuter::VerifyRestore_(const String &sZipFile, XNode *pBackupNode, bool &discardArchive)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The trailing zero in 3-2-1-1-0. VerifyArchive_ proves the index can be read;
   // this proves the mail can be got back out: the message store is extracted to a
   // scratch directory by the same BackupRestorer a restore uses, held to exactly
   // the files this run staged for compression, and reconciled against the
   // message rows in the index.
   //
   // Two questions, with two different consequences. "Does the archive contain
   // what this run wrote?" is a question about the archive, and a wrong answer
   // discards it: a store that reads back with fewer files or bytes than went in
   // is a truncated archive, whatever 7za's exit code said. "Does every message
   // row have a file?" is a question about the server, not the archive - the rows
   // were read before the copy started, and mail flow does not stop for a backup
   // - so its answer is reported, with names, and never fails the backup. An
   // archive that faithfully records a store with a file missing is exactly the
   // backup an administrator whose store has lost a file wants to have.
   //
   // The cost is the extraction: for the duration of the check the message store
   // exists twice, in the temp directory. A temp volume that cannot hold it is
   // not a reason to fail a good backup, so that case is skipped with an
   // explanation; BackupVerifyRestore=0 skips the step altogether.
   //---------------------------------------------------------------------------()
   {
      discardArchive = false;

      boost::system::error_code spaceError;
      boost::filesystem::space_info tempSpace = boost::filesystem::space(
         boost::filesystem::path(IniFileSettings::Instance()->GetTempDirectory().c_str()), spaceError);

      // A margin for the index and the archive's own overhead; the store itself is
      // the number that matters.
      const unsigned __int64 margin = 64ull * 1024 * 1024;

      if (!spaceError && tempSpace.available < staged_bytes_ + margin)
      {
         Logger::Instance()->LogBackup(Formatter::Format(
            "Verified restore skipped: extracting the message store needs {0} byte(s) and the temp directory {1} has {2} byte(s) free. The archive index was verified; the mail inside was not. Free space there, or set BackupVerifyRestore=0 to stop this line.",
            staged_bytes_ + margin, IniFileSettings::Instance()->GetTempDirectory(), (unsigned __int64) tempSpace.available));

         return true;
      }

      Logger::Instance()->LogBackup("Verifying the backup can be restored: extracting the message store...");

      std::shared_ptr<Backup> probe = std::shared_ptr<Backup>(new Backup);
      probe->SetBackupFile(sZipFile);
      probe->SetRestoreOptions(Backup::BODomains | Backup::BOMessages);

      // The verification extracts the archive through the same code a restore
      // uses, so on an encrypted archive it needs the passphrase exactly as a
      // restore does. Which also makes this the check that the archive can be
      // opened with the passphrase the server holds - not merely with one it used
      // a moment ago.
      probe->SetPassphrase(passphrase_);

      // The restorer removes the scratch directory when it goes out of scope. The
      // same Prepare a restore runs: the version check, the subset checks and the
      // extraction, so what passes here is what a restore would accept.
      BackupRestorer restorer;
      String failureReason;

      if (!restorer.Prepare(probe, failureReason))
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup was written but its message store could not be extracted for verification, so it has not been shown to be restorable. " + failureReason);

         return false;
      }

      // THE ROWS, BEFORE THE FILES.
      //
      // The message store is the big thing in the archive, but it is not the only
      // thing this run wrote into it: an account's address book, its webmail
      // settings, its scheduled messages, its files, its S/MIME keys, its password
      // history and its calendar are rows in the index rather than files beside it.
      // Reconciling only the files would leave exactly the failure this whole change
      // is about - a verification that keeps passing while a store is missing - so
      // what this run built in memory is counted against what the archive now reads
      // back, per store, through the restorer's own copy of the index.
      //
      // A difference here is a question about the ARCHIVE, not about the server, so
      // it discards: the index has just been written and read back, nothing has had
      // an opportunity to change in between, and an index that does not contain what
      // was put in it is a truncated one.
      std::map<String, __int64> writtenRows;
      std::map<String, __int64> archivedRows;

      AccountStores::CountRows(pBackupNode, writtenRows);
      AccountStores::CountRows(restorer.GetBackupNode(), archivedRows);

      __int64 totalWritten = 0;

      for (std::map<String, __int64>::const_iterator row = writtenRows.begin(); row != writtenRows.end(); row++)
      {
         totalWritten += row->second;

         std::map<String, __int64>::const_iterator archived = archivedRows.find(row->first);
         __int64 found = archived == archivedRows.end() ? 0 : archived->second;

         if (found == row->second)
            continue;

         discardArchive = true;

         Application::Instance()->GetBackupManager()->OnBackupFailed(Formatter::Format(
            "The backup was written but its index reads back as holding {0} row(s) of {1} where this run wrote {2}. The archive does not contain what was backed up and is not a usable backup.",
            found, row->first, row->second));

         return false;
      }

      // Said only when there was something to say. A server whose accounts have no
      // address book, no calendar and nothing scheduled writes no such rows, and a
      // line reporting that none were checked would be noise in every backup log on
      // every installation that does not use the webmail.
      if (!writtenRows.empty())
      {
         Logger::Instance()->LogBackup(Formatter::Format(
            "Verified restore: {0} row(s) across {1} per-account store(s) - address books, webmail settings, scheduled mail, file links, S/MIME keys, password history, calendars, first-contact memory and role assignments - read back from the archive exactly as they were written.",
            totalWritten, (unsigned int) writtenRows.size()));
      }

      String store = restorer.GetStagedMessageStore();

      unsigned int extractedFiles = 0;
      unsigned __int64 extractedBytes = 0;
      std::set<String> extractedNames;
      String detail;

      if (!MeasureTree_(boost::filesystem::path(store.c_str()), extractedFiles, extractedBytes, &extractedNames, detail))
      {
         Application::Instance()->GetBackupManager()->OnBackupFailed(
            "The backup was written but its message store could not be read back after extraction, so it has not been shown to be restorable. " + detail);

         return false;
      }

      if (extractedFiles != staged_files_ || extractedBytes != staged_bytes_)
      {
         discardArchive = true;

         Application::Instance()->GetBackupManager()->OnBackupFailed(Formatter::Format(
            "The backup was written but its message store reads back as {0} file(s) totalling {1} byte(s), where this run wrote {2} file(s) totalling {3} byte(s). The archive does not contain what was backed up and is not a usable backup.",
            extractedFiles, extractedBytes, staged_files_, staged_bytes_));

         return false;
      }

      std::vector<String> rowFiles;
      CollectMessageFilenames_(pBackupNode, rowFiles);

      const unsigned int namedLimit = 10;
      unsigned int missing = 0;
      String missingNames;

      for (size_t i = 0; i < rowFiles.size(); i++)
      {
         String name = rowFiles[i];
         name.ToLower();

         if (extractedNames.find(name) != extractedNames.end())
            continue;

         missing++;

         if (missing <= namedLimit)
         {
            if (missing > 1)
               missingNames += _T(", ");

            missingNames += rowFiles[i];
         }
      }

      Logger::Instance()->LogBackup(Formatter::Format(
         "Verified restore: the message store was extracted to {0} and reads back as {1} file(s) totalling {2} byte(s), exactly what this run wrote.",
         store, extractedFiles, extractedBytes));

      Logger::Instance()->LogBackup(Formatter::Format(
         "{0} message row(s) in the index were checked against the extracted store; {1} of them have no file in this backup.",
         (unsigned int) rowFiles.size(), missing));

      if (missing > 0)
      {
         Logger::Instance()->LogBackup(Formatter::Format(
            "Message row(s) whose file is not in this backup{0}: {1}. They were deleted from the message store before or while the backup ran; after a restore they are listed but cannot be fetched. Small numbers are what mail flow looks like; a large number means the backup was taken during a bulk deletion and is worth taking again.",
            missing > namedLimit ? String(_T(" (the first ten)")) : String(_T("")), missingNames));
      }

      return true;
   }

   bool
   BackupExecuter::StartRestore(std::shared_ptr<Backup> pBackup)
   {
      bool bMessagesDBOnly = IniFileSettings::Instance()->GetBackupMessagesDBOnly();

      Logger::Instance()->LogBackup("Reading XML file...");

      // PHASE ONE: everything that can be established without changing anything.
      //
      // A restore has no transaction and nothing can undo it, so every question that
      // has an answer before the first deletion has to be asked before the first
      // deletion - including "is the message store this restore needs actually
      // inside the archive?", which is what used to be asked forty lines too late.
      // See BackupRestorer for the four ways that ended in a server with nothing on
      // it. A false return here means the server has not been touched.
      BackupRestorer restorer;
      String failureReason;

      if (!restorer.Prepare(pBackup, failureReason))
      {
         ReportRestoreFailure_(failureReason);
         return false;
      }

      XNode *pBackupNode = restorer.GetBackupNode();
      int iRestoreOptions = restorer.GetRestoreOptions();

      // A legal hold (docs/Discovery.md) is one of the questions with an answer
      // before the first deletion. A restore with the domains option deletes
      // every domain on this server - Domains::DeleteAll below, or the DeleteAll
      // Collection::XMLLoad begins with under BackupMessagesDBOnly - and the
      // domains go in name order, each refused while it or an account in it is
      // held: so a hold on a domain that sorts after another used to cost that
      // other domain everything it had, and then stop the restore with nothing
      // put back. Refused here instead, whole, naming what is held, before
      // anything is touched. Not in BackupRestorer::Prepare: VerifyRestore_
      // runs that on every backup, and a backup taken under a hold is exactly
      // the backup a case wants taken.
      if (iRestoreOptions & Backup::BODomains)
      {
         String held;
         if (DiscoveryHolds::AnyAccountOrDomainHeld(held))
         {
            ReportRestoreFailure_(DiscoveryHolds::RestoreRefusal(held));
            return false;
         }
      }

      // PHASE TWO: the destructive part. From here on, a failure leaves the server
      // part-way through a restore, which is why nothing that could have been
      // checked is checked below.
      Logger::Instance()->LogBackup("Backup archive accepted. Starting the restore...");

      // Nothing a restore that failed part-way through left waiting is carried
      // into this one. See ACLManager::RestoredGrant.
      ACLManager::ForgetRestoredGrants();

      // The custom roles FIRST: before the domains, and so before the accounts and
      // the role assignments they carry, which name a role by its name and can be
      // written only once that role exists here. Before anything is deleted, too,
      // so that a role that cannot be written stops a restore before any domain
      // has been removed; the roles created before the failure stay, and the log
      // names them. A role this server already has is kept as it is here. An
      // archive with no roles in it restores none.
      if (iRestoreOptions & (Backup::BODomains | Backup::BOSettings))
      {
         Logger::Instance()->LogBackup("Restoring custom roles...");

         if (!RoleManager::XMLLoad(pBackupNode))
         {
            String sErrorMessage = "Restore of custom roles failed. No domain has been removed; the custom roles created before the failure are named in the log above, and nothing else has been restored. Please check hMailServer log.";
            Logger::Instance()->LogBackup(sErrorMessage);
            Application::Instance()->GetBackupManager()->OnBackupFailed(sErrorMessage);
            return false;
         }
      }

      if (iRestoreOptions & Backup::BODomains)
      {
         // First drop all domains. We need to do this prior to restoring
         // the data directory. When we drop the domains, we will also
         // drop the domain folders from the data directory. If we do this
         // in the wrong order, we'll hence first restore the data directory
         // and then drop it.
         //
         // The key files a DKIM key rotation generated are the exception, for as
         // long as this block runs - the delete below and the one XMLLoad begins
         // with. The domains the backup puts back name the same files, and a
         // restore that does not bring the data directory back has no way to put a
         // deleted private key back: its domain would go on naming a file that no
         // longer exists, for a record already published.
         DkimRotation::KeepKeysOnDomainDelete keepRotationKeys;

         // And the parts the message store holds once (Persistence/MessageStoreBlobs.h):
         // deleting every domain lets every blob's count go to none, and the
         // backup's messages that are put back reference the same blobs and
         // arrive uncounted. None is deleted while this block runs; when it ends
         // - the restore done, or stopped part-way - every count is raised to
         // what the stored files that are there reference, with the hold still
         // in force, and only then does a blob whose count is none still go.
         MessageStoreBlobs::DeletionHold keepBlobs(true);

         std::shared_ptr<Domains> pDomains = std::shared_ptr<Domains>(new Domains);

         pDomains->Refresh();

         if (!bMessagesDBOnly)
         {
            // Checked, unlike before, and this is the one place where stopping on a
            // failed deletion is better than carrying on. Collection::XMLLoad also
            // begins by calling DeleteAll, and when that fails it returns *true* and
            // loads nothing - so a restore that could not clear the existing domains
            // used to be reported as having completed successfully against a server
            // it had emptied. (That "return true" is in Collection.h and is not this
            // change's to make; refusing here means it cannot be reached this way.)
            if (!pDomains->DeleteAll())
            {
               ReportRestoreFailure_("Restore failed: the domains that are on this server now could not be deleted, so the backup's domains cannot be put in their place. The restore has been stopped part-way through - some domains may already have been removed. Check the hMailServer error log, then restore again.");
               return false;
            }
         }

         // We need to do the same with public folders - and to check it, which this
         // did not, one line below the domain delete that does. The consequence is
         // not symmetrical with the domains case: XMLLoad puts the backup's public
         // folders in on top of whatever survived, so a failed delete leaves the
         // server holding folders the backup never contained, alongside duplicates
         // of the ones it did, and reports a successful restore over the top of it.
         if (iRestoreOptions & Backup::BOSettings && !bMessagesDBOnly)
         {
            if (!Configuration::Instance()->GetIMAPConfiguration()->GetPublicFolders()->DeleteAll())
            {
               ReportRestoreFailure_("Restore failed: the public folders that are on this server now could not be deleted, so the backup's public folders cannot be put in their place. The restore has been stopped part-way through - the domains have already been removed. Check the hMailServer error log, then restore again.");
               return false;
            }
         }

         // Should we restore messages as well? Prepare staged the message store
         // exactly when the options and BackupMessagesDBOnly call for one, so asking
         // it is the same test as before and cannot disagree with what was staged.
         if (restorer.GetHasStagedMessageStore())
         {
            Logger::Instance()->LogBackup("Restoring data directory...");

            if (!RestoreDataDirectory_(restorer, pBackup->GetBackupFile()))
               return false;
         }

         Logger::Instance()->LogBackup("Restoring domains...");

         if (!pDomains->XMLLoad(pBackupNode, iRestoreOptions))
         {
            String sErrorMessage = "Restore of domains failed. Please check hMailServer log.";
            Logger::Instance()->LogBackup(sErrorMessage);
            Application::Instance()->GetBackupManager()->OnBackupFailed(sErrorMessage);

            return false;
         }
      }

      // Backup settings last since they may be referring to objects in the domains.
      if (iRestoreOptions & Backup::BOSettings)
      {
         Logger::Instance()->LogBackup("Restoring settings...");
         if (!Configuration::Instance()->XMLLoad(pBackupNode, iRestoreOptions))
         {
            String sErrorMessage = "Restore of settings failed. Please check hMailServer log.";
            Logger::Instance()->LogBackup(sErrorMessage);
            Application::Instance()->GetBackupManager()->OnBackupFailed(sErrorMessage);
            return false;
         }
      }


      // Last of all, the grants on accounts' own folders that named somebody the
      // restore had not reached when the folder was put back - an account further
      // down the archive, or a group, which comes back with the settings above.
      // Everything they can name exists by now; see ACLManager::RestoredGrant.
      if (!ACLManager::ApplyRestoredGrants())
      {
         String sErrorMessage = "Restore of folder permissions failed. Please check hMailServer log.";
         Logger::Instance()->LogBackup(sErrorMessage);
         Application::Instance()->GetBackupManager()->OnBackupFailed(sErrorMessage);
         return false;
      }

      // Reinitialize server since everything may have changed.
      // We can't run Application::ReInitialize here, since this
      // thread is owned by the backup manager, which is owned
      // by the application. And the backup manager is recreated
      // upon reinitialization.
      Logger::Instance()->LogBackup("Reinitializing server (async)...");

      Reinitializator::Instance()->ReInitialize();

      Logger::Instance()->LogBackup("Restore completed successfully.");

      return true;
   }

   bool
   BackupExecuter::RestoreDataDirectory_(BackupRestorer &restorer, const String &archiveFile)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Replaces the live data directory with the message store the restorer has
   // already put on local disk.
   //
   // Everything that used to happen here before the deletion - working out where the
   // messages are, extracting them, confirming they exist - has moved into
   // BackupRestorer::Prepare, which runs before any domain is deleted. What is left
   // is the part that cannot be made safe by ordering: between the delete and the
   // end of the copy there is no complete copy of the mail store on this server, only
   // the one in the staging directory.
   //---------------------------------------------------------------------------()
   {
      String sDirContainingDataFiles = restorer.GetStagedMessageStore();

      // Delete all directories from the data directory
      // so that we're sure that we're doing a clean restore
      String sDataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      // Every directory but the recoverable store, the eDiscovery cases' files,
      // the stores of messages that expire and password-protected messages,
      // and the live update's folder, which a backup does not carry and a
      // restore leaves where they are (IsKeptOutOfBackup_); the copy below
      // leaves an older backup's copy of them out in the same way. And none of
      // the directories this restore is working with, where they are inside
      // the data directory (DataDirectoryExclusions_): the destination, with
      // its archives; the temp directory, where the store to copy back has
      // been extracted; the folder of the archive being restored; and that
      // store (AddRestoreExclusions_).
      const String destination = Configuration::Instance()->GetBackupDestination();
      const String temporaryDirectory = IniFileSettings::Instance()->GetTempDirectory();

      DataDirectoryExclusions_ exclusions;
      AddRestoreExclusions_(exclusions, destination, temporaryDirectory, archiveFile, sDirContainingDataFiles, ForTheEmptying_);

      std::vector<String> kept;
      DeleteDataFilesForRestore_(sDataDirectory, archiveFile);
      DeleteDataDirectoriesForRestore_(sDataDirectory, exclusions, kept);

      for (const String &excluded : kept)
         Logger::Instance()->LogBackup(Formatter::Format("Left in place while the data directory {0} was emptied for the restore: {1}, which is inside it.", sDataDirectory, excluded));

      // Nor into them: an archive 6.3.5 or earlier made can carry a copy of the
      // destination or the temp directory - the nested tree a copy into itself
      // left in the staging folder, or the temp directory's scratch - and put
      // back, it would be written into the destination or the temp directory.
      // Into the folder of the archive, though, which may be a mailbox's.
      DataDirectoryExclusions_ copyExclusions;
      AddRestoreExclusions_(copyExclusions, destination, temporaryDirectory, archiveFile, sDirContainingDataFiles, ForTheCopy_);

      TreeCopyOutcome outcome;
      bool copied = CopyTreeGuarded_(sDirContainingDataFiles, sDataDirectory, outcome, true, &copyExclusions, ExcludeByTarget_);

      if (!copied)
      {
         // The data directory has already been emptied, so the staged copy is
         // deliberately kept: it is the only copy of the messages that still exists,
         // and the administrator is told where it is. Without this the restorer's
         // destructor would delete it on the way out - which is what the previous
         // version of this code got right and is the reason RetainStagedFiles exists.
         restorer.RetainStagedFiles();

         ReportRestoreFailure_("Restore failed while copying messages into " + sDataDirectory + ". " + DescribeCopyFailure_(outcome) +
                               " The messages from the backup have been left in " + sDirContainingDataFiles +
                               " - do not delete that folder until they have been recovered.");
         return false;
      }

      return true;
   }

   void
   BackupExecuter::ReportRestoreFailure_(const String &message)
   {
      Logger::Instance()->LogBackup(message);
      Application::Instance()->GetBackupManager()->OnBackupFailed(message);
   }

   namespace
   {
      void CheckBackupTree_(bool condition, const String &sentence)
      {
         if (!condition)
            throw std::logic_error(std::string(AnsiString(_T("backup exclusions: ") + sentence).c_str()));
      }

      void MakeBackupTreeFile_(const boost::filesystem::path &file, const char *content)
      {
         boost::system::error_code error;
         boost::filesystem::create_directories(file.parent_path(), error);
         CheckBackupTree_(!error, _T("the scratch directory ") + String(file.parent_path().wstring()) + _T(" could not be made"));

         FILE *handle = nullptr;
#ifdef HM_PLATFORM_POSIX
         handle = fopen(file.string().c_str(), "wb");
#else
         _wfopen_s(&handle, file.wstring().c_str(), L"wb");
#endif
         CheckBackupTree_(handle != nullptr, _T("the scratch file ") + String(file.wstring()) + _T(" could not be opened"));
         if (handle == nullptr)
            return;

         const bool written = fputs(content, handle) >= 0;
         const bool closed = fclose(handle) == 0;
         CheckBackupTree_(written && closed, _T("the scratch file ") + String(file.wstring()) + _T(" could not be written"));
      }

      bool BackupTreeHas_(const boost::filesystem::path &path)
      {
         boost::system::error_code error;
         return boost::filesystem::exists(path, error) && !error;
      }

      String BackupTreeText_(const boost::filesystem::path &path)
      {
         return String(path.wstring());
      }
   }

   void
   BackupExecuterTester::Test()
   {
      namespace fs = boost::filesystem;

      boost::system::error_code error;
      String unique = GUIDCreator::GetGUID();
      unique.Replace(_T("{"), _T(""));
      unique.Replace(_T("}"), _T(""));
      const fs::path scratch = fs::temp_directory_path(error) / fs::path(std::wstring(_T("hm-backup-exclusions-") + unique));
      CheckBackupTree_(!error, _T("the system's temp directory could not be found"));

      // Removed however the test ends: it is a copy of nothing but its own files.
      struct Scratch
      {
         explicit Scratch(const fs::path &path) : path_(path) {}
         ~Scratch() { boost::system::error_code ignored; fs::remove_all(path_, ignored); }
         fs::path path_;
      } removeAtTheEnd(scratch);

      // A data directory with everything a backup and a restore meet inside it:
      // a mailbox, a destination at its root holding an archive and a destination
      // three levels down holding another, the temp directory, the live update's
      // folder, a queue file and an archive in the root; and beside it a
      // directory whose name begins with the data directory's.
      const fs::path data = scratch / "data";
      const fs::path twin = scratch / "data2";
      const fs::path message = fs::path("example.test") / "someone" / "AB" / "{B5DDB7C4-7E40-4F4B-8A5D-3B0F0A6D5E01}.eml";

      MakeBackupTreeFile_(data / message, "mail");
      MakeBackupTreeFile_(data / "backup" / "HMBackup 2026-09-29 120000.7z", "archive");
      MakeBackupTreeFile_(data / "deep" / "nested" / "destination" / "HMBackup 2026-09-28 120000.7z", "archive");
      MakeBackupTreeFile_(data / "deep" / "other" / "file.txt", "beside the way down");
      MakeBackupTreeFile_(data / "temp" / "scratch.tmp", "temporary");
      MakeBackupTreeFile_(data / "Updates" / "hmailserver_9.9.9_amd64.deb", "package");
      MakeBackupTreeFile_(data / "queue.eml", "queued");
      MakeBackupTreeFile_(data / "HMBackup 2026-09-27 120000.7z", "archive in the root");
      MakeBackupTreeFile_(twin / "example.test" / "twin.eml", "twin");

      // The destination as an administrator might have written it: on Windows
      // in capitals and with forward slashes, which reach the same directory;
      // elsewhere through a symbolic link to it, which does too.
#ifdef HM_PLATFORM_POSIX
      const fs::path link = scratch / "link-to-backup";
      fs::create_directory_symlink(data / "backup", link, error);
      CheckBackupTree_(!error, _T("the link to the destination could not be made"));
      const String destinationAsWritten = BackupTreeText_(link);
#else
      String destinationAsWritten = BackupTreeText_(data / "backup");
      destinationAsWritten.ToUpper();
      destinationAsWritten.Replace(_T("\\"), _T("/"));
#endif

      // 1. THE BACKUP'S COPY. The staging folder is inside the destination, as
      //    an unencrypted backup's is, and the destination inside the data
      //    directory - the case that copied itself into itself until HM5014.
      const fs::path staging = data / "backup" / "DataBackup";
      fs::create_directories(staging, error);
      CheckBackupTree_(!error, _T("the staging folder could not be made"));

      // What a failed run of 6.3.5 or earlier left in the staging folder: the
      // start of the copy into itself.
      MakeBackupTreeFile_(staging / "backup" / "DataBackup" / "backup" / "left.txt", "a copy of a copy");
      MakeBackupTreeFile_(staging / "temp" / "left.tmp", "a copy of the temp directory");

      // As BackupDataDirectory_ builds them, and one more to show a directory
      // whose name begins with the data directory's is not the data directory.
      DataDirectoryExclusions_ exclusions;
      AddBackupExclusions_(exclusions, destinationAsWritten, BackupTreeText_(data / "temp"), BackupTreeText_(staging));
      exclusions.Add(BackupTreeText_(twin), _T("a directory beside the data directory"));

      String excludedAs;
      CheckBackupTree_(!exclusions.Contains(data, excludedAs),
         _T("the data directory was taken for ") + excludedAs + _T(", as though its name being the start of another's made them one"));
      CheckBackupTree_(exclusions.Contains(twin, excludedAs), _T("the directory beside the data directory was not recognised as itself"));
      CheckBackupTree_(exclusions.Contains(data / "backup", excludedAs) && excludedAs.StartsWith(_T("the backup destination")),
         _T("the destination, written another way, was not recognised"));

      TreeCopyOutcome outcome;
      CheckBackupTree_(CopyTreeGuarded_(BackupTreeText_(data), BackupTreeText_(staging), outcome, true, &exclusions),
         _T("the copy of the data directory failed: ") + DescribeCopyFailure_(outcome));

      CheckBackupTree_(BackupTreeHas_(staging / message), _T("the mailbox was not copied"));
      CheckBackupTree_(!BackupTreeHas_(staging / "backup"), _T("the destination was copied into itself, or an earlier run's copy of it was kept"));
      CheckBackupTree_(!BackupTreeHas_(staging / "temp"), _T("the temp directory was copied, or an earlier run's copy of it was kept"));
      CheckBackupTree_(!BackupTreeHas_(staging / "Updates"), _T("the live update's folder was copied"));
      CheckBackupTree_(BackupTreeHas_(staging / "deep" / "other" / "file.txt"), _T("a directory that is not excluded was left out"));
      CheckBackupTree_(BackupTreeHas_(staging / "queue.eml"), _T("a file at the root that is not an archive was left out"));
      CheckBackupTree_(!BackupTreeHas_(staging / "HMBackup 2026-09-27 120000.7z"), _T("an archive at the data directory's root was copied"));
      CheckBackupTree_(outcome.excluded.size() == 2,
         Formatter::Format(_T("{0} directories were left out of the copy, where the destination and the temp directory are the two inside it"), (unsigned int) outcome.excluded.size()));

      // What an encrypted backup measures before it stages, to ask the temp
      // directory for room, is what the copy then stages: not the destination's
      // archives, the temp directory, the live update's folder or an archive in
      // the root.
      unsigned int measuredFiles = 0;
      unsigned int stagedFiles = 0;
      unsigned __int64 measuredBytes = 0;
      unsigned __int64 stagedBytes = 0;
      String measureDetail;
      CheckBackupTree_(MeasureTree_(data, measuredFiles, measuredBytes, nullptr, measureDetail, &exclusions),
         _T("the data directory could not be measured: ") + measureDetail);
      CheckBackupTree_(MeasureTree_(staging, stagedFiles, stagedBytes, nullptr, measureDetail),
         _T("the staged copy could not be measured: ") + measureDetail);
      CheckBackupTree_(measuredFiles == stagedFiles && measuredBytes == stagedBytes,
         Formatter::Format(_T("the data directory measured {0} file(s) of {1} byte(s) for the copy, which staged {2} file(s) of {3} byte(s)"),
            measuredFiles, measuredBytes, stagedFiles, stagedBytes));

      fs::remove_all(staging, error);

      // 2. THE RESTORE'S EMPTYING. The destination with its archive, the temp
      //    directory where the store was extracted, and the folder of the
      //    archive being restored, three levels down, stay; the directories on
      //    the way down to it are emptied around it; the rest goes.
      //
      //    The store is extracted into the temp directory inside the data
      //    directory, as a restore on a Linux package extracts it: an archive
      //    an older version made, which carried the live update's folder and
      //    its outcome, the nested tree of a copy into itself, and the temp
      //    directory of its day.
      const fs::path archive = data / "deep" / "nested" / "destination" / "HMBackup 2026-09-28 120000.7z";
      const fs::path older = data / "temp" / "extracted" / "DataBackup";
      MakeBackupTreeFile_(older / "example.test" / "someone" / "AB" / "restored.eml", "restored mail");
      MakeBackupTreeFile_(older / "Updates" / "last-apply.txt", "ok 6.3.5 an apply this machine never made");
      MakeBackupTreeFile_(older / "backup" / "DataBackup" / "backup" / "nested.txt", "a copy into itself, archived");
      MakeBackupTreeFile_(older / "temp" / "scratch-of-then.tmp", "the temp directory, archived");
      // Mail the archive holds in the directory the archive being restored was
      // put in - a mailbox's folder, as far as the copy can know.
      MakeBackupTreeFile_(older / "deep" / "nested" / "destination" / "beside-the-archive.eml", "mail where the archive is");

      // As RestoreDataDirectory_ builds them.
      DataDirectoryExclusions_ restoreExclusions;
      AddRestoreExclusions_(restoreExclusions, destinationAsWritten, BackupTreeText_(data / "temp"), BackupTreeText_(archive), BackupTreeText_(older),
                            ForTheEmptying_);

      std::vector<String> kept;
      CheckBackupTree_(DeleteDataFilesForRestore_(BackupTreeText_(data), BackupTreeText_(archive)), _T("the data directory's root files could not be deleted"));
      CheckBackupTree_(DeleteDataDirectoriesForRestore_(BackupTreeText_(data), restoreExclusions, kept), _T("the data directory could not be emptied"));

      CheckBackupTree_(!BackupTreeHas_(data / "example.test"), _T("the mailbox's domain was not removed"));
      CheckBackupTree_(BackupTreeHas_(data / "backup" / "HMBackup 2026-09-29 120000.7z"), _T("the destination's archive was deleted"));
      CheckBackupTree_(BackupTreeHas_(archive), _T("the archive being restored was deleted"));
      CheckBackupTree_(!BackupTreeHas_(data / "deep" / "other"), _T("a directory beside the way down to the archive's folder was kept"));
      CheckBackupTree_(BackupTreeHas_(data / "temp" / "scratch.tmp"), _T("the temp directory was emptied"));
      CheckBackupTree_(BackupTreeHas_(older / "example.test" / "someone" / "AB" / "restored.eml"), _T("the store the restore extracted into the temp directory was deleted before it was copied back"));
      CheckBackupTree_(BackupTreeHas_(data / "Updates" / "hmailserver_9.9.9_amd64.deb"), _T("the live update's folder was emptied"));
      CheckBackupTree_(!BackupTreeHas_(data / "queue.eml"), _T("a queue file at the root was kept"));
      CheckBackupTree_(BackupTreeHas_(data / "HMBackup 2026-09-27 120000.7z"), _T("an archive at the data directory's root was deleted"));
      CheckBackupTree_(BackupTreeHas_(twin / "example.test" / "twin.eml"), _T("the directory beside the data directory was touched"));
      CheckBackupTree_(kept.size() == 3,
         Formatter::Format(_T("{0} directories were left in place, where the destination, the temp directory and the archive's folder are three"), (unsigned int) kept.size()));

      // 3. THE RESTORE'S COPY of that store, from the temp directory inside the
      //    data directory into it: the mail is put back, the live update's
      //    folder and its outcome are not, and neither are the older archive's
      //    copies of the destination and the temp directory. The archive's own
      //    folder is written into, and what it held when it was backed up is
      //    put back beside the archive.
      DataDirectoryExclusions_ copyExclusions;
      AddRestoreExclusions_(copyExclusions, destinationAsWritten, BackupTreeText_(data / "temp"), BackupTreeText_(archive), BackupTreeText_(older),
                            ForTheCopy_);

      TreeCopyOutcome restored;
      CheckBackupTree_(CopyTreeGuarded_(BackupTreeText_(older), BackupTreeText_(data), restored, true, &copyExclusions, ExcludeByTarget_),
         _T("the restore's copy failed: ") + DescribeCopyFailure_(restored));
      CheckBackupTree_(BackupTreeHas_(archive.parent_path() / "beside-the-archive.eml"),
         _T("what the archive holds in the folder the archive was restored from was not put back"));
      CheckBackupTree_(BackupTreeHas_(archive), _T("the restore's copy disturbed the archive being restored"));
      CheckBackupTree_(BackupTreeHas_(data / "example.test" / "someone" / "AB" / "restored.eml"), _T("the restored mail is not there"));
      CheckBackupTree_(!BackupTreeHas_(data / "Updates" / "last-apply.txt"), _T("an older backup's live update outcome was restored"));
      CheckBackupTree_(!BackupTreeHas_(data / "backup" / "DataBackup"), _T("an older archive's copy of the destination was written into the destination"));
      CheckBackupTree_(!BackupTreeHas_(data / "temp" / "scratch-of-then.tmp"), _T("an older archive's copy of the temp directory was written into it"));
      CheckBackupTree_(BackupTreeHas_(data / "backup" / "HMBackup 2026-09-29 120000.7z"), _T("the restore's copy disturbed the destination"));
      CheckBackupTree_(BackupTreeHas_(older / "example.test" / "someone" / "AB" / "restored.eml"), _T("the restore's copy disturbed the store it copied from"));
   }
}
