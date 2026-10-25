// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

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
#include "NewInstallDefaults.h"

#include "../SQL/DatabaseUnavailableMarker.h"
#include "../AntiSpam/DKIM/DkimRotation.h"
#include "../Util/DiscoveryHolds.h"
#include "../Util/GUIDCreator.h"
#include "../Persistence/MessageStoreBlobs.h"
#include "../Persistence/MessageStoreObjects.h"

#include "../../IMAP/IMAPConfiguration.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <map>
#include <set>

#ifdef HM_PLATFORM_POSIX
#include <sys/stat.h>
#endif
#ifndef HM_PLATFORM_POSIX
#include <sddl.h>
#include <aclapi.h>
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
   IsGone_(const boost::filesystem::path &path)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Whether path is positively not there - removed since it was listed - and
   // not merely out of reach. boost::filesystem::exists(path, error) does not
   // say that on its own: for a path that is not there it answers false AND sets
   // error, since Boost's status() reports every errno and every Windows error,
   // the ones it answers file_not_found for among them. So the test this file
   // used from 6.2.19 to 6.3.5, "no error and not there", never held: a message
   // file a POP3 DELE or an IMAP EXPUNGE removed while a backup was copying the
   // data directory failed the backup - after 30 s of retries, or at once when
   // it went between the listing and the question what it was (HM5014) -
   // instead of being counted as gone, and a restore whose staged copy lost a
   // file failed the same way. Only the answers that mean "no such file" count
   // here: ENOENT and ENOTDIR on Linux, ERROR_FILE_NOT_FOUND and
   // ERROR_PATH_NOT_FOUND on Windows. Not a share or a drive that went away,
   // which Boost answers file_not_found for as well: counted as gone, that would
   // have a backup leave out everything after it with nothing said but a count.
   //---------------------------------------------------------------------------()
   {
      boost::system::error_code error;
      const boost::filesystem::file_status status = boost::filesystem::status(path, error);
      if (status.type() != boost::filesystem::file_not_found)
         return false;

      if (!error)
         return true;

      if (error.category() != boost::system::system_category())
         return false;

#ifdef HM_PLATFORM_POSIX
      return error.value() == ENOENT || error.value() == ENOTDIR;
#else
      return error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND;
#endif
   }

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
         // "Positively established as absent", not "could not be found" (IsGone_):
         // treating a failure to look as absence would silently leave a message
         // out of the backup - which is the outcome all of this exists to prevent.
         if (IsGone_(from))
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
   // not empty, each with the words the backup log says it in. Directories that
   // are not there are not held: nothing can be met in a copy that does not
   // exist. A restore walks the whole data directory and leaves each of them
   // where it meets it (RemoveWhatTheBackupHasNot_), so the directories on the
   // way down to one are emptied around it and never removed whole.
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

   private:

      std::map<DirectoryIdentity_, String> excluded_;
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
             bool skipKeptOutOfBackup = false, const DataDirectoryExclusions_ *exclusions = nullptr)
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
            if (IsGone_(current))
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
            if (exclusions && exclusions->Contains(current, excludedAs))
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
               if (boost::filesystem::exists(target, staleError) && !staleError &&
                   !exclusions->Contains(target, alsoExcluded))
               {
                  boost::filesystem::remove_all(target, staleError);
               }

               continue;
            }

            if (!CopyTree_(current, target, outcome, false, exclusions))
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
                    const DataDirectoryExclusions_ *exclusions = nullptr)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // CopyTree_ with a backstop. Every operation inside it uses an error_code
   // overload, so nothing should reach here - but the whole point of this code is
   // that an exception escaping a work-queue task terminates the process, and that
   // property must not depend on having enumerated every throwing call correctly.
   // The backup's copy of the data directory, which leaves out what
   // IsKeptOutOfBackup_ names at its root; a restore's copy is RestoreStoreInto_,
   // which has a backstop of its own.
   //---------------------------------------------------------------------------()
   {
      try
      {
         return CopyTree_(boost::filesystem::path(from.c_str()), boost::filesystem::path(to.c_str()), outcome, skipKeptOutOfBackup, exclusions);
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

   // HOW A RESTORE REPLACES THE DATA DIRECTORY
   //
   // Up to 6.3.5 a restore of messages emptied the data directory first - every
   // directory at its root but the ones IsKeptOutOfBackup_ names, and every file
   // there - and then copied the archive's store in. Two things went wrong with
   // that order, and a third with what it deleted.
   //
   //  * A file the service may not delete stopped it half-way. The self-signed
   //    key an installation makes for itself (NewInstallDefaults) is written by
   //    the administrator's elevated process with permissions of its own: Read,
   //    and nothing more, for the account the service runs as. From 6.3.4 that
   //    account is NT SERVICE\hMailServer by default, and its Modify on the data
   //    folder does not include deleting a child of a folder. So the emptying
   //    failed on Data\Certificates (HM5700), left the directories after it in
   //    place, and the copy then failed on the archive's copy of the same key
   //    (HM5014, after its 30 s of retries) - before it had reached the
   //    domains, whose directories the restore had deleted with the domains.
   //    Every full restore of messages on such an installation ended with the
   //    mail in the Temp folder's staging copy only, and no domain on the
   //    server. A key or certificate an administrator moved into the data
   //    folder, which keeps the permissions it had where it was, does the same
   //    wherever those permissions do not let the service's account delete it.
   //
   //  * Between the emptying and the end of the copy, the data directory held
   //    neither what it had nor what the archive has, and a failure in that
   //    window - a full disk, a file that would not copy - left it that way.
   //
   //  * The emptying deleted every file at the data directory's root, which was
   //    once only the delivery queue's message files. It is now also this
   //    server's own state: on Linux the key file every LINUX1 secret on the
   //    machine is sealed under (DataProtector, .hmailserver-secret-key) -
   //    from 6.3.4 [Security] StoredSecretsKey among them, so after a restore
   //    no second factor and no [Settings] secret opened - the container's
   //    .hmailserver-environment, the sending limits' state and the first
   //    start's marker. None of it is in an archive, so none of it came back.
   //
   // So the restore now copies first and removes after (RestoreStoreInto_), and
   // undoes what it did when the copy fails:
   //
   //  1. The archive's store is copied into the data directory over what is
   //     there. A file the data directory already holds byte for byte is left
   //     as it is, whoever owns it. One that differs is moved aside - renamed,
   //     beside itself - and the archive's copied in its place; one that cannot
   //     be moved aside, because the account the service runs as may not delete
   //     it, is left as it is and named, and the restore goes on. This
   //     installation's own self-signed certificate and key are left as they
   //     are when the archive's key is not the same, and named: the archive's
   //     pair is another installation's, and under NT SERVICE\hMailServer its
   //     key is one the service could not put in the place of this one's
   //     anyway. No file at the store's root
   //     is copied: no archive holds one (BackupDataDirectory_ deletes them).
   //  2. When the copy fails, everything it wrote is removed - the part of a
   //     file a copy that failed on a full disk left too - and everything it
   //     moved aside is put back, so the data directory is what it was before
   //     the restore's copy, and the archive is where it was. Until the copy is
   //     complete the data directory holds the archive's files beside the ones
   //     they replace, so a restore needs room for both - less the domains'
   //     folders, which a restore of domains deletes first - and a disk that
   //     fills is a copy that fails.
   //  3. Only once the copy is complete is what the archive does not hold
   //     removed: every file below the directories the old emptying removed
   //     that the archive's store has no file at, and the directories left
   //     empty that it has no directory at, but this installation's own pair.
   //     At the root, only the delivery queue's message files, and what a
   //     writer of the store left of one. A file that cannot be removed is left
   //     and named. Then what was moved aside goes.
   //
   // What the old emptying left alone is left alone: the directories
   // IsKeptOutOfBackup_ names, and wherever they are inside the data directory
   // the destination, the temp directory, the folder of the archive being
   // restored and the store extracted from it (DataDirectoryExclusions_).

   // What a restore moved aside, wrote and made, in that order, so that it can be
   // undone (RollBack) until the copy is complete, and what it moved aside
   // removed once it is (Commit).
   class RestoreJournal_
   {
   public:

      RestoreJournal_()
      {
         String unique = GUIDCreator::GetGUID();
         unique.Replace(_T("{"), _T(""));
         unique.Replace(_T("}"), _T(""));
         suffix_ = _T(".hmailserver-previous-") + unique.Left(8);
      }

      // Renames target - a file, a directory or a link, as itself - to a name
      // beside it, so that it is on the same volume whatever links the data
      // directory has. False, with why, when it could not be: it is then where
      // it was.
      bool MoveAside(const boost::filesystem::path &target, String &why)
      {
         boost::filesystem::path aside(target);
         aside += boost::filesystem::path(std::wstring(suffix_.c_str()));

         boost::system::error_code error;
         boost::filesystem::rename(target, aside, error);

         if (error)
         {
            why = error.message().c_str();
            return false;
         }

         moved_.push_back(std::make_pair(target, aside));
         return true;
      }

      // The last MoveAside undone at once: the file that was to take its place
      // was gone from the store before it could be copied - something removed it
      // from the staged copy meanwhile, an anti-virus scanner most likely - so
      // this server's file is put back, and the removal of what the archive does
      // not hold leaves it (WasPutBack). False, with why, when it could not be
      // put back: it is then the only copy of that file, so it stays where it was
      // moved - Commit never removes it, and RollBack tries again.
      bool PutBackLast(String &why, String &where)
      {
         if (moved_.empty())
            return true;

         boost::system::error_code error;
         boost::filesystem::rename(moved_.back().second, moved_.back().first, error);
         if (error)
         {
            why = error.message().c_str();
            where = String(moved_.back().second.wstring());
            stranded_.push_back(moved_.back());
         }
         else
            put_back_.insert(Key_(moved_.back().first));

         moved_.pop_back();
         return !error;
      }

      bool WasPutBack(const boost::filesystem::path &path) const
      {
         return put_back_.find(Key_(path)) != put_back_.end();
      }

      void Added(const boost::filesystem::path &file)
      {
         added_files_.push_back(file);
      }

      // Whether path is one this journal moved aside, by its name: Commit
      // removes those, and nothing else is to count them.
      bool IsAside(const boost::filesystem::path &path) const
      {
         const String name(path.filename().wstring());
         return name.GetLength() > suffix_.GetLength() && name.Right(suffix_.GetLength()) == suffix_;
      }

      void AddedDirectory(const boost::filesystem::path &directory)
      {
         added_directories_.push_back(directory);
      }

      // Everything undone, last first: the files written removed, what was moved
      // aside put back, the directories made removed. notPutBack names what could
      // not be, and is empty when the data directory is as it was.
      void RollBack(std::vector<String> &notPutBack)
      {
         for (auto file = added_files_.rbegin(); file != added_files_.rend(); ++file)
         {
            boost::system::error_code error;
            boost::filesystem::remove(*file, error);
            if (error)
               notPutBack.push_back(Formatter::Format("{0}, which the restore wrote, could not be removed: {1}", String((*file).wstring()), String(error.message().c_str())));
         }

         for (auto moved = moved_.rbegin(); moved != moved_.rend(); ++moved)
         {
            boost::system::error_code error;
            boost::filesystem::rename((*moved).second, (*moved).first, error);
            if (error)
               notPutBack.push_back(Formatter::Format("{0} could not be put back from {1}: {2}", String((*moved).first.wstring()), String((*moved).second.wstring()), String(error.message().c_str())));
         }

         for (const auto &stranded : stranded_)
         {
            boost::system::error_code error;
            boost::filesystem::rename(stranded.second, stranded.first, error);
            if (error)
               notPutBack.push_back(Formatter::Format("{0} could not be put back from {1}: {2}", String(stranded.first.wstring()), String(stranded.second.wstring()), String(error.message().c_str())));
         }

         // A directory the restore made that is not empty now holds something
         // that is not the restore's - a message delivered meanwhile - and stays.
         for (auto directory = added_directories_.rbegin(); directory != added_directories_.rend(); ++directory)
         {
            boost::system::error_code error;
            if (boost::filesystem::is_empty(*directory, error) && !error)
               boost::filesystem::remove(*directory, error);
         }

         added_files_.clear();
         moved_.clear();
         stranded_.clear();
         added_directories_.clear();
      }

      // What was moved aside removed, now that what replaced it is complete. left
      // names what could not be. What PutBackLast could not put back is not
      // removed: nothing replaced it.
      void Commit(std::vector<String> &left)
      {
         for (const auto &moved : moved_)
         {
            boost::system::error_code error;
            boost::filesystem::remove_all(moved.second, error);
            if (error)
               left.push_back(Formatter::Format("{0} (the copy {1} had before the restore) could not be removed: {2}", String(moved.second.wstring()), String(moved.first.wstring()), String(error.message().c_str())));
         }

         added_files_.clear();
         moved_.clear();
         stranded_.clear();
         added_directories_.clear();
      }

   private:

      // A path as the data directory's file system compares it: without case
      // on Windows (SameName_).
      static String Key_(const boost::filesystem::path &path)
      {
         String key(path.wstring());
#ifndef HM_PLATFORM_POSIX
         key.ToLower();
#endif
         return key;
      }

      String suffix_;
      std::set<String> put_back_;
      std::vector<std::pair<boost::filesystem::path, boost::filesystem::path> > moved_;
      std::vector<std::pair<boost::filesystem::path, boost::filesystem::path> > stranded_;
      std::vector<boost::filesystem::path> added_files_;
      std::vector<boost::filesystem::path> added_directories_;
   };

   // What a restore of the data directory did, in the words the backup log says
   // it in.
   struct RestoreOutcome_
   {
      // The copy: what it copied, what vanished from the store meanwhile, where
      // it failed, and the directories it did not write into (the exclusions).
      TreeCopyOutcome copy;

      // Files the data directory held already, byte for byte.
      unsigned int already;

      // Files the archive's copy replaced, the data directory's moved aside.
      unsigned int replaced;

      // Files the archive does not hold, removed once the copy was complete.
      unsigned int removed;

      // Files at the store's root, which are put back nowhere (RestoreCopyTree_).
      unsigned int root_files_left_out;

      // The exclusions the removal left in place.
      std::vector<String> left_in_place;

      // Files of this server left as they are, each with why: one that differs
      // from the archive's and could not be moved aside, one the archive does
      // not hold that could not be removed.
      std::vector<String> kept;

      // This installation's own self-signed pair, left as it is (the sentence),
      // or empty.
      String own_pair;

      // After a failed copy: what could not be put back (empty when the data
      // directory is as it was).
      std::vector<String> not_put_back;

      // After a complete one: what was moved aside and could not be removed.
      std::vector<String> left_aside;

      RestoreOutcome_() :
         already(0),
         replaced(0),
         removed(0),
         root_files_left_out(0)
      {
      }
   };

   // Whether two paths name one file the way the data directory's file system
   // compares names: without case on Windows.
   static bool
   SameName_(const boost::filesystem::path &first, const boost::filesystem::path &second)
   {
#ifdef HM_PLATFORM_POSIX
      return first == second;
#else
      return String(first.wstring()).CompareNoCase(String(second.wstring())) == 0;
#endif
   }

   static bool
   SameContents_(const boost::filesystem::path &first, const boost::filesystem::path &second)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Whether two regular files hold the same bytes. False when either cannot be
   // read: then the restore treats them as different, which costs at most a copy.
   //---------------------------------------------------------------------------()
   {
      boost::system::error_code error;
      const boost::uintmax_t size = boost::filesystem::file_size(first, error);
      if (error)
         return false;

      const boost::uintmax_t otherSize = boost::filesystem::file_size(second, error);
      if (error || size != otherSize)
         return false;

      boost::filesystem::ifstream one(first, std::ios::in | std::ios::binary);
      boost::filesystem::ifstream other(second, std::ios::in | std::ios::binary);
      if (!one || !other)
         return false;

      const size_t chunk = 65536;
      std::vector<char> oneBuffer(chunk);
      std::vector<char> otherBuffer(chunk);

      boost::uintmax_t remaining = size;
      while (remaining > 0)
      {
         const std::streamsize wanted = (std::streamsize) (remaining < chunk ? remaining : chunk);
         one.read(&oneBuffer[0], wanted);
         other.read(&otherBuffer[0], wanted);

         if (one.gcount() != wanted || other.gcount() != wanted)
            return false;

         if (memcmp(&oneBuffer[0], &otherBuffer[0], (size_t) wanted) != 0)
            return false;

         remaining -= (boost::uintmax_t) wanted;
      }

      return true;
   }

   // This installation's own self-signed certificate and key, which it made for
   // itself when it was installed (NewInstallDefaults), and whether a restore
   // leaves them as they are: when this installation has the key and the
   // archive's store has not the same key, byte for byte. The two are kept or
   // replaced together, since a key and a certificate that are not a pair stop
   // every listener that uses them.
   struct OwnCertificatePair_
   {
      boost::filesystem::path key;
      boost::filesystem::path certificate;
      bool keep;

      OwnCertificatePair_() : keep(false) {}

      bool Covers(const boost::filesystem::path &file) const
      {
         return keep && (SameName_(file, key) || SameName_(file, certificate));
      }
   };

   static OwnCertificatePair_
   JudgeOwnCertificatePair_(const boost::filesystem::path &store, const boost::filesystem::path &data, String &sentence)
   {
      OwnCertificatePair_ pair;

      const boost::filesystem::path folder(NewInstallDefaults::PairFolderName());
      const boost::filesystem::path keyName(NewInstallDefaults::PairKeyFileName());
      const boost::filesystem::path certificateName(NewInstallDefaults::PairCertificateFileName());

      pair.key = data / folder / keyName;
      pair.certificate = data / folder / certificateName;

      boost::system::error_code error;
      if (!boost::filesystem::is_regular_file(boost::filesystem::symlink_status(pair.key, error)))
         return pair;

      const boost::filesystem::path archived = store / folder / keyName;
      const bool archiveHasOne = boost::filesystem::is_regular_file(boost::filesystem::symlink_status(archived, error));

      if (archiveHasOne && SameContents_(archived, pair.key))
         return pair;

      pair.keep = true;
      sentence = Formatter::Format("This installation's own self-signed certificate and key, {0} and {1}, which it made for itself when it was installed, are left as they are: {2} The archive's pair, if it is wanted instead, is in the archive under {3}{4}: stop the service, copy both files over these, and start it again.",
         String(pair.certificate.wstring()), String(pair.key.wstring()),
         archiveHasOne ? String(_T("the archive holds another installation's pair, whose key is not this one.")) : String(_T("the archive holds no such pair.")),
         String(folder.wstring()), String(FileUtilities::PathSeparator));
      return pair;
   }

   // A fault the tester injects at a known point of the copy, at the file after
   // restore_fault_after_for_test it has written: the copy fails there; or fails
   // and leaves the start of that file behind it, as boost::filesystem::copy_file
   // does on Linux when the disk fills; or the store's copy of that file and of
   // every one after it is gone before it can be copied, as when a scanner takes
   // it from the staged copy. NoRestoreFault_, as everywhere but
   // BackupExecuterTester, is none.
   enum RestoreFault_
   {
      NoRestoreFault_,
      RestoreCopyFails_,
      RestoreCopyFailsLeavingPart_,
      RestoreStoreFileVanishes_
   };

   static RestoreFault_ restore_fault_for_test = NoRestoreFault_;
   static unsigned int restore_fault_after_for_test = 0;

   static bool
   PlaceDirectory_(const boost::filesystem::path &target, RestoreJournal_ &journal, RestoreOutcome_ &outcome)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // target a directory of its own for the restore to copy into: as it is when it
   // is one; made when nothing is there; and when something else is there - a
   // file, a link, a junction - that moved aside first, as the old emptying
   // removed it.
   //---------------------------------------------------------------------------()
   {
      // Boost sets the error_code for a path that is not there too, and answers
      // file_not_found: only status_error is a failure to look.
      boost::system::error_code error;
      const boost::filesystem::file_status status = boost::filesystem::symlink_status(target, error);

      if (status.type() == boost::filesystem::status_error)
      {
         outcome.copy.failed_file = String(target.wstring());
         outcome.copy.failure_detail = error.message().c_str();
         return false;
      }

      if (boost::filesystem::is_directory(status))
         return true;

      if (boost::filesystem::exists(status))
      {
         String why;
         if (!journal.MoveAside(target, why))
         {
            outcome.copy.failed_file = String(target.wstring());
            outcome.copy.failure_detail = _T("it is not a directory, where the archive has one, and it could not be moved aside: ") + why;
            return false;
         }
      }

      error.clear();
      boost::filesystem::create_directory(target, error);
      if (error)
      {
         outcome.copy.failed_file = String(target.wstring());
         outcome.copy.failure_detail = error.message().c_str();
         return false;
      }

      journal.AddedDirectory(target);
      return true;
   }

   static bool
   RestoreCopyTree_(const boost::filesystem::path &from, const boost::filesystem::path &to, bool atRoot,
                    const DataDirectoryExclusions_ &exclusions, const OwnCertificatePair_ &ownPair,
                    RestoreJournal_ &journal, RestoreOutcome_ &outcome)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Step 1 of RestoreStoreInto_: the store from, or a directory of it, copied
   // into to over what is there, each step recorded in journal. Leaves out what
   // the old restore's copy left out - at the root the directories
   // IsKeptOutOfBackup_ names, and wherever they are the exclusions, by
   // target - every file at the store's root, and this installation's own pair
   // when it is kept.
   // False, with the file and why in outcome.copy, when something the archive
   // holds could not be put in place.
   //---------------------------------------------------------------------------()
   {
      boost::system::error_code iterateError;
      boost::filesystem::directory_iterator file(from, iterateError);

      for (; !iterateError && file != boost::filesystem::directory_iterator(); file.increment(iterateError))
      {
         const boost::filesystem::path current(file->path());
         const boost::filesystem::path target = to / current.filename();

         boost::system::error_code statusError;
         const bool isDirectory = boost::filesystem::is_directory(current, statusError);

         if (statusError)
         {
            // As CopyTree_: gone is mail flow, anything else is a failure.
            if (IsGone_(current))
            {
               outcome.copy.vanished++;
               continue;
            }

            outcome.copy.failed_file = String(current.wstring());
            outcome.copy.failure_detail = statusError.message().c_str();
            return false;
         }

         if (isDirectory)
         {
            if (atRoot && IsKeptOutOfBackup_(current))
               continue;

            String excludedAs;
            if (exclusions.Contains(target, excludedAs))
            {
               outcome.copy.excluded.push_back(excludedAs);
               continue;
            }

            if (!PlaceDirectory_(target, journal, outcome))
               return false;

            if (!RestoreCopyTree_(current, target, false, exclusions, ownPair, journal, outcome))
               return false;

            continue;
         }

         // No file at the store's root is put back. A backup deletes the root's
         // files from its staged copy before it archives it (BackupDataDirectory_),
         // and has since before 6.0: they are the delivery queue's, whose rows a
         // restore does not put back. So none is in an archive this server made,
         // and one that is - an archive put together by hand - could only land on
         // one of this server's own files there: an archive, or on Linux the key
         // file the LINUX1 secrets are sealed under, which is this machine's.
         if (atRoot)
         {
            outcome.root_files_left_out++;
            continue;
         }

         if (ownPair.Covers(target))
            continue;

         // As in PlaceDirectory_: a target that is not there is no failure.
         boost::system::error_code targetError;
         const boost::filesystem::file_status targetStatus = boost::filesystem::symlink_status(target, targetError);

         if (targetStatus.type() == boost::filesystem::status_error)
         {
            outcome.copy.failed_file = String(target.wstring());
            outcome.copy.failure_detail = targetError.message().c_str();
            return false;
         }

         bool replacing = false;

         if (boost::filesystem::exists(targetStatus))
         {
            if (boost::filesystem::is_regular_file(targetStatus) && SameContents_(current, target))
            {
               outcome.already++;
               continue;
            }

            String why;
            if (!journal.MoveAside(target, why))
            {
               outcome.kept.push_back(Formatter::Format("{0}, which is not the archive's copy and could not be moved aside to make room for it ({1}), is left as it is", String(target.wstring()), why));
               continue;
            }

            replacing = true;
         }

         const unsigned int copiedBefore = outcome.copy.copied;
         bool copiedOne = false;

         const bool faultHere = restore_fault_for_test != NoRestoreFault_ && outcome.copy.copied >= restore_fault_after_for_test;
         if (faultHere && restore_fault_for_test == RestoreStoreFileVanishes_)
         {
            boost::system::error_code ignored;
            boost::filesystem::remove(current, ignored);
         }

         if (faultHere && restore_fault_for_test != RestoreStoreFileVanishes_)
         {
            if (restore_fault_for_test == RestoreCopyFailsLeavingPart_)
            {
               boost::filesystem::ofstream part(target, std::ios::out | std::ios::binary | std::ios::trunc);
               part << "the start of a copy the disk had no room for";
            }

            outcome.copy.failed_file = String(current.wstring());
            outcome.copy.failure_detail = "the fault BackupExecuterTester injects";
         }
         else
            copiedOne = CopyOneFile_(current, target, outcome.copy);

         if (!copiedOne)
         {
            // Whatever the failed copy left at target is the restore's to remove
            // in the undo: boost::filesystem::copy_file leaves the part it wrote
            // on Linux when the disk fills, and nothing else can be there - what
            // was, has been moved aside, and is put back after it.
            journal.Added(target);
            return false;
         }

         if (outcome.copy.copied != copiedBefore)
         {
            journal.Added(target);
            if (replacing)
               outcome.replaced++;
         }
         else if (replacing)
         {
            // The store's copy was gone before it could be copied - it vanished
            // from the staged store - so this server's file is put back.
            String why;
            String where;
            if (!journal.PutBackLast(why, where))
               outcome.kept.push_back(Formatter::Format("{0}, whose copy in the archive's store was gone before it could be copied, could not be put back from {1}, where it was moved aside to make room for that copy ({2}): it is there, the only copy of it", String(target.wstring()), where, why));
         }
      }

      if (iterateError)
      {
         outcome.copy.failed_file = String(from.wstring());
         outcome.copy.failure_detail = iterateError.message().c_str();
         return false;
      }

      return true;
   }

   static bool
   IsQueueMessageFile_(const boost::filesystem::path &file)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // A delivery queue's message file at the data directory's root, {GUID}.eml,
   // or what a writer of the message store left of one when it stopped part-way:
   // the name with one of the marks it stages a file beside its name with, and a
   // GUID after it ({GUID}.eml.writing-{GUID}, MessageStore.cpp). The old
   // emptying removed those with every other file at the root.
   //---------------------------------------------------------------------------()
   {
      const String name(file.filename().wstring());

      String extension(file.extension().wstring());
      if (extension.CompareNoCase(_T(".eml")) == 0)
         return true;

      static const TCHAR *const marks[] = { _T(".eml.writing-"), _T(".eml.encrypting-"), _T(".eml.plaintext-"), _T(".eml.linking-"), _T(".eml.store-") };
      String lower(name);
      lower.ToLower();
      for (const TCHAR *mark : marks)
      {
         if (lower.Find(mark) > 0 && lower.EndsWith(_T("}")))
            return true;
      }

      return false;
   }

   static void
   RemoveWhatTheBackupHasNot_(const boost::filesystem::path &directory, const boost::filesystem::path &counterpart, bool atRoot,
                              const DataDirectoryExclusions_ &exclusions, const OwnCertificatePair_ &ownPair,
                              const DirectoryIdentity_ *archive, const RestoreJournal_ &journal, RestoreOutcome_ &outcome)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Step 3 of RestoreStoreInto_, once the copy is complete: below directory,
   // every file the archive's store has no file at (counterpart, the same place
   // in the store) removed, and every directory left empty that it has no
   // directory at. The exclusions are left in place, and said; so is this
   // installation's own pair when it is kept. At the root, the directories
   // IsKeptOutOfBackup_ names are left, and of the files only the delivery
   // queue's message files are removed - never the archive being restored, nor
   // this server's own files there, which no archive carries. What the copy
   // moved aside is left for journal's Commit. A link is removed as the link it
   // is and never followed. A file that cannot be removed is left and said.
   // Listed first and removed after, so that no listing is walked while it
   // changes.
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
         outcome.kept.push_back(Formatter::Format("{0} could not be listed ({1}), so what is in it that the archive does not hold is left there", String(directory.wstring()), String(iterateError.message().c_str())));
         return;
      }

      for (const boost::filesystem::path &current : entries)
      {
         if (journal.IsAside(current))
            continue;

         // Gone since it was listed is nothing to remove; only status_error is a
         // failure to look (Boost sets the error_code for a path not there too).
         boost::system::error_code statusError;
         const boost::filesystem::file_status status = boost::filesystem::symlink_status(current, statusError);

         if (status.type() == boost::filesystem::file_not_found)
            continue;

         if (status.type() == boost::filesystem::status_error)
         {
            outcome.kept.push_back(Formatter::Format("{0} could not be examined ({1}), and is left as it is", String(current.wstring()), String(statusError.message().c_str())));
            continue;
         }

         // Only a directory, or a link that may lead to one, can be one of the
         // exclusions, which are known by what they lead to.
         if (!boost::filesystem::is_regular_file(status))
         {
            String excludedAs;
            if (exclusions.Contains(current, excludedAs))
            {
               outcome.left_in_place.push_back(excludedAs);
               continue;
            }
         }

         const boost::filesystem::path counterpartHere = counterpart / current.filename();
         boost::system::error_code counterpartError;
         const boost::filesystem::file_status counterpartStatus = boost::filesystem::status(counterpartHere, counterpartError);

         if (boost::filesystem::is_directory(status))
         {
            if (atRoot && IsKeptOutOfBackup_(current))
               continue;

            RemoveWhatTheBackupHasNot_(current, counterpartHere, false, exclusions, ownPair, archive, journal, outcome);

            boost::system::error_code emptyError;
            if (!boost::filesystem::is_directory(counterpartStatus) && boost::filesystem::is_empty(current, emptyError) && !emptyError)
            {
               boost::system::error_code removeError;
               boost::filesystem::remove(current, removeError);
            }

            continue;
         }

         if (atRoot)
         {
            if (!IsQueueMessageFile_(current) || !boost::filesystem::is_regular_file(status))
               continue;

            // Never an archive, by its name or as the one being restored,
            // whatever a queue file is taken to be.
            DateTime created;
            if (BackupRetention::ParseArchiveName(String(current.filename().wstring()), created))
               continue;

            DirectoryIdentity_ identity;
            if (archive && IdentifyDirectory_(current, identity) && !(identity < *archive) && !(*archive < identity))
               continue;
         }
         else if (ownPair.Covers(current))
            continue;

         // The copy put the archive's file here, or found it here already, or left
         // this server's in its place and said so. Not at the root, where it puts
         // no file (RestoreCopyTree_).
         if (!atRoot && boost::filesystem::exists(counterpartStatus) && !boost::filesystem::is_directory(counterpartStatus))
            continue;

         // Or put this server's back, the store's copy having gone before it
         // could be copied (RestoreJournal_::PutBackLast).
         if (journal.WasPutBack(current))
            continue;

         boost::system::error_code removeError;
         boost::filesystem::remove(current, removeError);

         if (removeError)
            outcome.kept.push_back(Formatter::Format("{0}, which the archive does not hold, could not be removed ({1}), and is left as it is", String(current.wstring()), String(removeError.message().c_str())));
         else
            outcome.removed++;
      }
   }

   static bool
   RestoreStoreInto_(const boost::filesystem::path &store, const boost::filesystem::path &data, const String &archiveFile,
                     const DataDirectoryExclusions_ &forTheEmptying, const DataDirectoryExclusions_ &forTheCopy,
                     RestoreOutcome_ &outcome)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The data directory data made what the archive's message store, extracted to
   // store, holds: see "HOW A RESTORE REPLACES THE DATA DIRECTORY" above. True
   // when the store is in place - with what was left as it is in outcome.kept
   // and outcome.own_pair. False when the copy failed, with outcome.copy saying
   // where, after everything the copy did has been undone; outcome.not_put_back
   // then names what could not be, and is empty when the data directory is as it
   // was. The exceptions nothing below should throw are caught here, as
   // CopyTreeGuarded_ catches them, and undone the same way.
   //---------------------------------------------------------------------------()
   {
      RestoreJournal_ journal;
      bool copied = false;

      try
      {
         const OwnCertificatePair_ ownPair = JudgeOwnCertificatePair_(store, data, outcome.own_pair);

         copied = RestoreCopyTree_(store, data, true, forTheCopy, ownPair, journal, outcome);

         if (copied)
         {
            DirectoryIdentity_ archiveIdentity;
            const bool archiveKnown = !archiveFile.IsEmpty() && IdentifyDirectory_(boost::filesystem::path(archiveFile.c_str()), archiveIdentity);

            RemoveWhatTheBackupHasNot_(data, store, true, forTheEmptying, ownPair, archiveKnown ? &archiveIdentity : nullptr, journal, outcome);
         }
      }
      catch (boost::thread_interrupted const &)
      {
         // The service is shutting down: what the copy did is undone first, so
         // that it stops with the data directory as it was - or, the copy being
         // complete, what it moved aside goes as it would have.
         if (!copied)
            journal.RollBack(outcome.not_put_back);
         else
            journal.Commit(outcome.left_aside);
         throw;
      }
      catch (std::exception const &exception)
      {
         if (!copied)
         {
            outcome.copy.failed_file = String(store.wstring());
            outcome.copy.failure_detail = exception.what();
         }
         else
            outcome.kept.push_back(Formatter::Format("The removal of what the archive does not hold stopped: {0}", String(exception.what())));
      }
      catch (...)
      {
         if (!copied)
         {
            outcome.copy.failed_file = String(store.wstring());
            outcome.copy.failure_detail = "an unrecognised error";
         }
         else
            outcome.kept.push_back(_T("The removal of what the archive does not hold stopped: an unrecognised error"));
      }

      if (!copied)
      {
         journal.RollBack(outcome.not_put_back);
         return false;
      }

      journal.Commit(outcome.left_aside);
      return true;
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

         // A file removed since it was listed is not counted, as the copy does
         // not copy it (IsGone_); any other failure to look is a failure.
         if (error && IsGone_(entry->path()))
         {
            entry.increment(error);
            if (error)
            {
               failureDetail = Formatter::Format("{0} could not be listed: {1}.", String(root.wstring()), String(error.message().c_str()));
               return false;
            }

            continue;
         }

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

            if (error && !IsGone_(entry->path()))
            {
               failureDetail = Formatter::Format("{0} could not be measured: {1}.", String(entry->path().wstring()), String(error.message().c_str()));
               return false;
            }

            if (!error)
            {
               files++;
               bytes += size;

               if (names)
               {
                  String name(entry->path().filename().wstring());
                  names->insert(name.ToLower());
               }
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

      // "Restore completed successfully." is written by BackupTask, once the run
      // slot is free (BackupManager::OutcomeHold).
      return true;
   }

   bool
   BackupExecuter::RestoreDataDirectory_(BackupRestorer &restorer, const String &archiveFile)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Makes the live data directory what the message store the restorer has
   // already put on local disk holds: copied in first, and what the archive does
   // not hold removed only once the copy is complete; a copy that fails is
   // undone (RestoreStoreInto_, and "HOW A RESTORE REPLACES THE DATA DIRECTORY").
   //
   // Everything that used to happen here before the deletion - working out where the
   // messages are, extracting them, confirming they exist - has moved into
   // BackupRestorer::Prepare, which runs before any domain is deleted.
   //---------------------------------------------------------------------------()
   {
      const String sDirContainingDataFiles = restorer.GetStagedMessageStore();
      const String sDataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      // Not touched: the recoverable store, the eDiscovery cases' files, the
      // stores of messages that expire and password-protected messages, and the
      // live update's folder, which a backup does not carry and a restore leaves
      // where they are (IsKeptOutOfBackup_), an older backup's copy of them
      // left out of the copy in the same way. Nor the directories this restore
      // is working with, where they are inside the data directory
      // (DataDirectoryExclusions_): the destination, with its archives; the temp
      // directory, where the store to copy back has been extracted; the folder
      // of the archive being restored; and that store (AddRestoreExclusions_).
      const String destination = Configuration::Instance()->GetBackupDestination();
      const String temporaryDirectory = IniFileSettings::Instance()->GetTempDirectory();

      DataDirectoryExclusions_ exclusions;
      AddRestoreExclusions_(exclusions, destination, temporaryDirectory, archiveFile, sDirContainingDataFiles, ForTheEmptying_);

      // Nor written into: an archive 6.3.5 or earlier made can carry a copy of
      // the destination or the temp directory - the nested tree a copy into
      // itself left in the staging folder, or the temp directory's scratch -
      // and put back, it would be written into the destination or the temp
      // directory. Into the folder of the archive, though, which may be a
      // mailbox's.
      DataDirectoryExclusions_ copyExclusions;
      AddRestoreExclusions_(copyExclusions, destination, temporaryDirectory, archiveFile, sDirContainingDataFiles, ForTheCopy_);

      RestoreOutcome_ outcome;
      const bool restored = RestoreStoreInto_(boost::filesystem::path(sDirContainingDataFiles.c_str()), boost::filesystem::path(sDataDirectory.c_str()),
                                              archiveFile, exclusions, copyExclusions, outcome);

      if (!restored)
      {
         String message = "Restore failed while copying the backup's messages into " + sDataDirectory + ". " + DescribeCopyFailure_(outcome.copy);

         if (outcome.not_put_back.empty())
         {
            // Nothing on this server is lost by stopping here: the data
            // directory is as it was before the copy, and the archive holds the
            // messages, so the staged copy is not kept.
            message += " What the copy had written has been removed and what it had moved aside put back, so the data directory is as it was before the restore's copy.";
            message += " The domains on this server were removed before the copy, as every restore of domains removes them; restore again once the cause is put right. The backup's messages are still in the backup " + archiveFile + ".";
         }
         else
         {
            // The data directory is not what it was, so the staged copy is kept,
            // as it always was after a copy that failed: without this the
            // restorer's destructor would delete it on the way out, which is the
            // reason RetainStagedFiles exists.
            restorer.RetainStagedFiles();

            message += Formatter::Format(" The copy could not be undone in full ({0} item(s)): ", (unsigned int) outcome.not_put_back.size());
            message += outcome.not_put_back[0];
            message += ". The backup log names each.";
            message += " The domains on this server were removed before the copy; restore again once the cause is put right. The backup's messages are still in the backup " + archiveFile +
                       " and have been left in " + sDirContainingDataFiles + " - do not delete that folder until they have been recovered.";

            for (const String &line : outcome.not_put_back)
               Logger::Instance()->LogBackup("Not put back: " + line + ".");
         }

         ReportRestoreFailure_(message);
         return false;
      }

      for (const String &excluded : outcome.left_in_place)
         Logger::Instance()->LogBackup(Formatter::Format("Left in place by the restore of the data directory {0}: {1}, which is inside it.", sDataDirectory, excluded));

      String summary;
      summary.Format(_T("The data directory now holds the backup's message store: %u file(s) copied from it, %u of them over a different copy here, %u here already byte for byte, and %u the backup does not hold removed."),
         outcome.copy.copied, outcome.replaced, outcome.already, outcome.removed);
      Logger::Instance()->LogBackup(summary);

      if (outcome.copy.vanished > 0)
         Logger::Instance()->LogBackup(Formatter::Format("{0} file(s) of the backup's message store were gone from the copy extracted in the temp directory before they could be copied - something removed them meanwhile, an anti-virus scanner most likely - and are not restored; where this server had a file of that name it is left in place. The archive still holds them.", outcome.copy.vanished));

      if (outcome.root_files_left_out > 0)
         Logger::Instance()->LogBackup(Formatter::Format("{0} file(s) at the root of the backup's message store were not put back: no archive this server makes holds a file there, and one there could only take the place of one of this server's own.", outcome.root_files_left_out));

      if (!outcome.own_pair.IsEmpty())
         Logger::Instance()->LogBackup(outcome.own_pair);

      for (const String &kept : outcome.kept)
         Logger::Instance()->LogBackup("Left as it is in the data directory: " + kept + ".");

      for (const String &left : outcome.left_aside)
         Logger::Instance()->LogBackup("Not removed after the restore: " + left + ". Delete it by hand.");

      // Said in the error log too, once: the restore is complete, and these
      // files are this server's and not the archive's. What the account the
      // service runs as may not change is changed by an administrator. Not
      // 5702, which SURBL reported until 6.3.6: a number is never given a
      // second meaning.
      if (!outcome.kept.empty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5705, "BackupExecuter::RestoreDataDirectory_",
            Formatter::Format("The restore of the data directory {0} is complete but for {1} file(s) of this server it left as they are: {2}. The backup log names each and why; to have the archive's copy of one instead, stop the service, put it in place as an administrator, and start the service again.",
               sDataDirectory, (unsigned int) outcome.kept.size(), outcome.kept[0]));
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

      std::string BackupTreeRead_(const boost::filesystem::path &path)
      {
         boost::filesystem::ifstream file(path, std::ios::in | std::ios::binary);
         if (!file)
            return std::string("(could not be read)");

         std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
         return content;
      }

      // Every directory and file below root, by its path relative to root, with
      // each file's bytes: what a data directory is, to compare before and after.
      void BackupTreeSnapshot_(const boost::filesystem::path &root, const boost::filesystem::path &directory, std::map<String, std::string> &entries)
      {
         boost::system::error_code error;
         for (boost::filesystem::directory_iterator file(directory, error); !error && file != boost::filesystem::directory_iterator(); file.increment(error))
         {
            const boost::filesystem::path current = file->path();
            const String relative(current.wstring().substr(root.wstring().size()));

            if (boost::filesystem::is_directory(boost::filesystem::symlink_status(current)))
            {
               entries[relative + _T("/")] = std::string();
               BackupTreeSnapshot_(root, current, entries);
            }
            else
               entries[relative] = BackupTreeRead_(current);
         }

         CheckBackupTree_(!error, _T("the scratch directory ") + String(directory.wstring()) + _T(" could not be listed"));
      }

      // Whether anything a restore moves aside is still below root.
      bool BackupTreeHasAside_(const boost::filesystem::path &root)
      {
         boost::system::error_code error;
         for (boost::filesystem::recursive_directory_iterator entry(root, error); !error && entry != boost::filesystem::recursive_directory_iterator(); entry.increment(error))
         {
            if (String(entry->path().filename().wstring()).Find(_T(".hmailserver-previous-")) >= 0)
               return true;
         }

         return false;
      }

#ifndef HM_PLATFORM_POSIX
      // The SID of the account this process runs as, as text.
      String BackupTreeSelfSid_()
      {
         HANDLE token = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return String();

         DWORD needed = 0;
         GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
         std::vector<unsigned char> buffer(needed > 0 ? needed : 1);

         String text;
         if (needed > 0 && GetTokenInformation(token, TokenUser, &buffer[0], needed, &needed))
         {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER *) &buffer[0])->User.Sid, &sid))
            {
               text = sid;
               LocalFree(sid);
            }
         }

         CloseHandle(token);
         return text;
      }

      // path given the permissions the SDDL text states, inheriting none.
      void BackupTreeProtect_(const boost::filesystem::path &path, const String &sddl)
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         CheckBackupTree_(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            _T("the permissions ") + sddl + _T(" could not be built"));

         BOOL present = FALSE;
         BOOL defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);

         std::wstring writable(path.wstring());
         const DWORD result = SetNamedSecurityInfoW(&writable[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                    nullptr, nullptr, dacl, nullptr);
         LocalFree(descriptor);

         CheckBackupTree_(result == ERROR_SUCCESS, Formatter::Format(_T("the permissions of {0} could not be set (Windows error {1})"), String(path.wstring()), (int) result));
      }

      // path given back to whoever may remove it: full control for this
      // process's account, the rest inherited again. Never throws: it runs while
      // the scratch tree is being cleaned up.
      void BackupTreeUnprotect_(const boost::filesystem::path &path)
      {
         const String sddl = Formatter::Format(_T("D:(A;OICI;FA;;;{0})"), BackupTreeSelfSid_());

         PSECURITY_DESCRIPTOR descriptor = nullptr;
         if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return;

         BOOL present = FALSE;
         BOOL defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);

         std::wstring writable(path.wstring());
         SetNamedSecurityInfoW(&writable[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
         LocalFree(descriptor);
      }
#endif
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

      // 2. THE RESTORE (RestoreStoreInto_), of a store extracted into the
      //    temp directory inside the data directory, as a restore on a Linux
      //    package extracts it: an archive an older version made, which carried
      //    the live update's folder and its outcome, the nested tree of a copy
      //    into itself, the temp directory of its day, mail in the folder the
      //    archive being restored is in, and another installation's
      //    self-signed pair. The destination with its archive, the temp
      //    directory with the store, and the folder of the archive being
      //    restored, three levels down, are left where they are; the
      //    directories on the way down to it are emptied around it; the store's
      //    files are put in place, a file here already byte for byte is left,
      //    one that differs is replaced; what the store does not hold goes once
      //    the copy is complete; and at the root only the queue's files go,
      //    never this server's own - the Linux key file every LINUX1 secret is
      //    sealed under among them - nor this installation's own pair.
      const fs::path archive = data / "deep" / "nested" / "destination" / "HMBackup 2026-09-28 120000.7z";
      const fs::path older = data / "temp" / "extracted" / "DataBackup";
      MakeBackupTreeFile_(older / "example.test" / "someone" / "AB" / "restored.eml", "restored mail");
      MakeBackupTreeFile_(older / "Updates" / "last-apply.txt", "ok 6.3.5 an apply this machine never made");
      MakeBackupTreeFile_(older / "backup" / "DataBackup" / "backup" / "nested.txt", "a copy into itself, archived");
      MakeBackupTreeFile_(older / "temp" / "scratch-of-then.tmp", "the temp directory, archived");
      // Mail the archive holds in the directory the archive being restored was
      // put in - a mailbox's folder, as far as the copy can know.
      MakeBackupTreeFile_(older / "deep" / "nested" / "destination" / "beside-the-archive.eml", "mail where the archive is");
      MakeBackupTreeFile_(older / "Sieve" / "example.test" / "same.sieve", "keep;");
      MakeBackupTreeFile_(older / "Sieve" / "example.test" / "changed.sieve", "discard;");
      MakeBackupTreeFile_(older / "Certificates" / "self-signed-key.pem", "another installation's key");
      MakeBackupTreeFile_(older / "Certificates" / "self-signed-certificate.pem", "another installation's certificate");
      MakeBackupTreeFile_(older / "example.test" / "Certificates" / "self-signed-key.pem", "a mailbox's folder of that name, not the pair");
      // Files at the store's root, which no archive this server makes holds: an
      // archive put together by hand, with another machine's key file in it.
      MakeBackupTreeFile_(older / ".hmailserver-secret-key", "another machine's key file");
      MakeBackupTreeFile_(older / "{6C1D0E55-5D3B-4F0E-9A57-2E1F3B7C9D10}.eml", "a queue file of another server");

      MakeBackupTreeFile_(data / "Sieve" / "example.test" / "same.sieve", "keep;");
      MakeBackupTreeFile_(data / "Sieve" / "example.test" / "changed.sieve", "keep; # this server's, older");
      MakeBackupTreeFile_(data / "Sieve" / "example.test" / "gone.sieve", "redirect \"x@example.test\";");
      MakeBackupTreeFile_(data / "Certificates" / "self-signed-key.pem", "this installation's key");
      MakeBackupTreeFile_(data / "Certificates" / "self-signed-certificate.pem", "this installation's certificate");
      MakeBackupTreeFile_(data / ".hmailserver-secret-key", "the key file of this machine's LINUX1 secrets");
      MakeBackupTreeFile_(data / ".hmailserver-environment", "the container's settings");
      MakeBackupTreeFile_(data / "hmailserver_sendinglimits.dat", "the sending limits' state");
      // What a writer of the store left of a queued message when it stopped.
      MakeBackupTreeFile_(data / "{2A4E6C8B-0D1F-4A3B-8C5D-7E9F1A2B3C4D}.eml.writing-{9F8E7D6C-5B4A-4392-8170-6F5E4D3C2B1A}", "half a queued message");

      // As RestoreDataDirectory_ builds them.
      DataDirectoryExclusions_ restoreExclusions;
      AddRestoreExclusions_(restoreExclusions, destinationAsWritten, BackupTreeText_(data / "temp"), BackupTreeText_(archive), BackupTreeText_(older),
                            ForTheEmptying_);
      DataDirectoryExclusions_ copyExclusions;
      AddRestoreExclusions_(copyExclusions, destinationAsWritten, BackupTreeText_(data / "temp"), BackupTreeText_(archive), BackupTreeText_(older),
                            ForTheCopy_);

      RestoreOutcome_ restored;
      CheckBackupTree_(RestoreStoreInto_(older, data, BackupTreeText_(archive), restoreExclusions, copyExclusions, restored),
         _T("the restore failed: ") + DescribeCopyFailure_(restored.copy));

      CheckBackupTree_(BackupTreeHas_(data / "example.test" / "someone" / "AB" / "restored.eml"), _T("the restored mail is not there"));
      CheckBackupTree_(!BackupTreeHas_(data / message), _T("a message the archive does not hold was kept"));
      CheckBackupTree_(BackupTreeHas_(data / "backup" / "HMBackup 2026-09-29 120000.7z"), _T("the destination's archive was deleted"));
      CheckBackupTree_(BackupTreeHas_(archive), _T("the archive being restored was deleted"));
      CheckBackupTree_(!BackupTreeHas_(data / "deep" / "other"), _T("a directory beside the way down to the archive's folder was kept"));
      CheckBackupTree_(BackupTreeHas_(archive.parent_path() / "beside-the-archive.eml"),
         _T("what the archive holds in the folder the archive was restored from was not put back"));
      CheckBackupTree_(BackupTreeHas_(data / "temp" / "scratch.tmp"), _T("the temp directory was emptied"));
      CheckBackupTree_(!BackupTreeHas_(data / "temp" / "scratch-of-then.tmp"), _T("an older archive's copy of the temp directory was written into it"));
      CheckBackupTree_(BackupTreeHas_(older / "example.test" / "someone" / "AB" / "restored.eml"), _T("the restore disturbed the store it copied from"));
      CheckBackupTree_(BackupTreeHas_(data / "Updates" / "hmailserver_9.9.9_amd64.deb"), _T("the live update's folder was emptied"));
      CheckBackupTree_(!BackupTreeHas_(data / "Updates" / "last-apply.txt"), _T("an older backup's live update outcome was restored"));
      CheckBackupTree_(!BackupTreeHas_(data / "backup" / "DataBackup"), _T("an older archive's copy of the destination was written into the destination"));
      CheckBackupTree_(!BackupTreeHas_(data / "queue.eml"), _T("a queue file at the root was kept"));
      CheckBackupTree_(BackupTreeHas_(data / "HMBackup 2026-09-27 120000.7z"), _T("an archive at the data directory's root was deleted"));
      CheckBackupTree_(BackupTreeHas_(data / ".hmailserver-secret-key"), _T("the key file this machine's LINUX1 secrets are sealed under was deleted"));
      CheckBackupTree_(BackupTreeHas_(data / ".hmailserver-environment") && BackupTreeHas_(data / "hmailserver_sendinglimits.dat"),
         _T("a file of this server's own at the data directory's root was deleted"));
      CheckBackupTree_(BackupTreeRead_(data / ".hmailserver-secret-key") == "the key file of this machine's LINUX1 secrets",
         _T("a file at the store's root was put over this server's own key file"));
      CheckBackupTree_(!BackupTreeHas_(data / "{6C1D0E55-5D3B-4F0E-9A57-2E1F3B7C9D10}.eml") && restored.root_files_left_out == 2,
         Formatter::Format(_T("the files at the store's root were put back, or not counted: {0} left out, where there are two"), restored.root_files_left_out));
      CheckBackupTree_(!BackupTreeHas_(data / "{2A4E6C8B-0D1F-4A3B-8C5D-7E9F1A2B3C4D}.eml.writing-{9F8E7D6C-5B4A-4392-8170-6F5E4D3C2B1A}"),
         _T("what a writer of the store left of a queued message at the root was kept"));
      CheckBackupTree_(BackupTreeHas_(twin / "example.test" / "twin.eml"), _T("the directory beside the data directory was touched"));
      CheckBackupTree_(BackupTreeRead_(data / "Sieve" / "example.test" / "changed.sieve") == "discard;", _T("a file that differs from the archive's was not replaced"));
      CheckBackupTree_(BackupTreeRead_(data / "Sieve" / "example.test" / "same.sieve") == "keep;", _T("a file the data directory had byte for byte was not left as it was"));
      CheckBackupTree_(!BackupTreeHas_(data / "Sieve" / "example.test" / "gone.sieve"), _T("a file the archive does not hold was kept"));
      CheckBackupTree_(BackupTreeRead_(data / "Certificates" / "self-signed-key.pem") == "this installation's key" &&
                       BackupTreeRead_(data / "Certificates" / "self-signed-certificate.pem") == "this installation's certificate",
         _T("this installation's own self-signed pair was replaced by another installation's"));
      CheckBackupTree_(!restored.own_pair.IsEmpty(), _T("the restore did not say it left this installation's own pair"));
      CheckBackupTree_(BackupTreeHas_(data / "example.test" / "Certificates" / "self-signed-key.pem"), _T("a file named as the pair in another folder was taken for it and not restored"));
      CheckBackupTree_(restored.already == 1 && restored.replaced == 1,
         Formatter::Format(_T("{0} file(s) were found here already and {1} replaced, where one is each"), restored.already, restored.replaced));
      CheckBackupTree_(restored.kept.empty(), _T("the restore left a file it could change: ") + (restored.kept.empty() ? String() : restored.kept[0]));
      CheckBackupTree_(restored.left_in_place.size() == 3,
         Formatter::Format(_T("{0} directories were left in place, where the destination, the temp directory and the archive's folder are three"), (unsigned int) restored.left_in_place.size()));
      CheckBackupTree_(!BackupTreeHasAside_(data), _T("a file the restore moved aside was left behind"));

      // 3. THE PAIR, the other two ways: an archive with this installation's
      //    key byte for byte restores its certificate over this one's, and a
      //    data directory without a pair is given the archive's.
      {
         const fs::path data3 = scratch / "pair-data";
         const fs::path store3 = scratch / "pair-store";
         MakeBackupTreeFile_(data3 / "Certificates" / "self-signed-key.pem", "the same key");
         MakeBackupTreeFile_(data3 / "Certificates" / "self-signed-certificate.pem", "this certificate");
         MakeBackupTreeFile_(store3 / "Certificates" / "self-signed-key.pem", "the same key");
         MakeBackupTreeFile_(store3 / "Certificates" / "self-signed-certificate.pem", "the archive's certificate for the same key");

         DataDirectoryExclusions_ none;
         RestoreOutcome_ samePair;
         CheckBackupTree_(RestoreStoreInto_(store3, data3, String(), none, none, samePair), _T("the restore with the same key failed: ") + DescribeCopyFailure_(samePair.copy));
         CheckBackupTree_(samePair.own_pair.IsEmpty(), _T("the pair was kept although the archive holds the same key"));
         CheckBackupTree_(BackupTreeRead_(data3 / "Certificates" / "self-signed-certificate.pem") == "the archive's certificate for the same key",
            _T("the archive's certificate for this installation's own key was not restored"));

         fs::remove_all(data3, error);
         fs::create_directories(data3, error);
         RestoreOutcome_ noPair;
         CheckBackupTree_(RestoreStoreInto_(store3, data3, String(), none, none, noPair), _T("the restore without a pair failed: ") + DescribeCopyFailure_(noPair.copy));
         CheckBackupTree_(noPair.own_pair.IsEmpty() && BackupTreeRead_(data3 / "Certificates" / "self-signed-key.pem") == "the same key",
            _T("the archive's pair was not restored where this installation has none"));
      }

      // 4. A COPY THAT FAILS is undone: whichever files it had written are
      //    removed, the one it had replaced is put back, the directories it made
      //    are removed, and nothing is removed that the archive does not hold -
      //    the data directory is as it was, byte for byte.
      {
         const fs::path data4 = scratch / "undo-data";
         const fs::path store4 = scratch / "undo-store";
         MakeBackupTreeFile_(data4 / "r" / "a.txt", "this server's a");
         MakeBackupTreeFile_(data4 / "r" / "kept.txt", "not in the archive, and removed only after a complete copy");
         MakeBackupTreeFile_(data4 / "{0B0C2E9A-1D6B-4A43-9D55-2B5D41A3F0C1}.eml", "a queued message");
         MakeBackupTreeFile_(store4 / "r" / "a.txt", "the archive's a");
         MakeBackupTreeFile_(store4 / "r" / "b.txt", "b");
         MakeBackupTreeFile_(store4 / "r" / "sub" / "c.txt", "c");
         MakeBackupTreeFile_(store4 / "n" / "d.txt", "d");
         MakeBackupTreeFile_(store4 / "n" / "e.txt", "e");

         std::map<String, std::string> before;
         BackupTreeSnapshot_(data4, data4, before);

         struct FaultInjected
         {
            FaultInjected(RestoreFault_ fault, unsigned int after) { restore_fault_for_test = fault; restore_fault_after_for_test = after; }
            ~FaultInjected() { restore_fault_for_test = NoRestoreFault_; restore_fault_after_for_test = 0; }
         };

         // At each of the five files, a copy that fails outright and one that
         // leaves the start of its file behind, as a full disk does on Linux.
         for (RestoreFault_ mode : { RestoreCopyFails_, RestoreCopyFailsLeavingPart_ })
         {
            for (unsigned int after = 0; after <= 4; after++)
            {
               RestoreOutcome_ failed;
               bool result = true;
               {
                  FaultInjected fault(mode, after);
                  DataDirectoryExclusions_ none;
                  result = RestoreStoreInto_(store4, data4, String(), none, none, failed);
               }

               const String how = mode == RestoreCopyFailsLeavingPart_ ? String(_T(", leaving part of its file,")) : String();
               CheckBackupTree_(!result, Formatter::Format(_T("the copy did not fail{0} where the fault was injected, after {1} file(s)"), how, after));
               CheckBackupTree_(failed.not_put_back.empty(), _T("the failed copy could not be undone: ") + (failed.not_put_back.empty() ? String() : failed.not_put_back[0]));

               std::map<String, std::string> afterwards;
               BackupTreeSnapshot_(data4, data4, afterwards);
               CheckBackupTree_(afterwards == before,
                  Formatter::Format(_T("after a copy that failed{0} after {1} file(s), the data directory is not what it was: {2} entries, where it had {3}"),
                     how, after, (unsigned int) afterwards.size(), (unsigned int) before.size()));
            }
         }

         // And without the fault, the same restore completes.
         RestoreOutcome_ complete;
         DataDirectoryExclusions_ none;
         CheckBackupTree_(RestoreStoreInto_(store4, data4, String(), none, none, complete), _T("the restore failed without a fault: ") + DescribeCopyFailure_(complete.copy));
         CheckBackupTree_(BackupTreeRead_(data4 / "r" / "a.txt") == "the archive's a" && BackupTreeHas_(data4 / "n" / "e.txt") &&
                          !BackupTreeHas_(data4 / "r" / "kept.txt") && !BackupTreeHas_(data4 / "{0B0C2E9A-1D6B-4A43-9D55-2B5D41A3F0C1}.eml"),
            _T("the restore after the failed ones did not make the data directory the archive's store"));
         CheckBackupTree_(!BackupTreeHasAside_(data4), _T("a file the restore moved aside was left behind"));
      }

      // 4a. A FILE GONE FROM THE STORE before it could be copied - a scanner
      //     took it from the staged copy - is not restored, and where this
      //     server had a file of that name it moved aside, that file is put
      //     back and left, not removed as one the archive does not hold.
      {
         const fs::path data6 = scratch / "vanish-data";
         const fs::path store6 = scratch / "vanish-store";
         MakeBackupTreeFile_(data6 / "r" / "a.txt", "this server's a");
         MakeBackupTreeFile_(store6 / "r" / "a.txt", "the archive's a, taken from the staged copy");
         MakeBackupTreeFile_(store6 / "r" / "b.txt", "b, taken from the staged copy");

         RestoreOutcome_ vanished;
         bool result = false;
         {
            restore_fault_for_test = RestoreStoreFileVanishes_;
            restore_fault_after_for_test = 0;
            DataDirectoryExclusions_ none;
            result = RestoreStoreInto_(store6, data6, String(), none, none, vanished);
            restore_fault_for_test = NoRestoreFault_;
         }

         CheckBackupTree_(result, _T("the restore failed where files were gone from the store: ") + DescribeCopyFailure_(vanished.copy));
         CheckBackupTree_(BackupTreeRead_(data6 / "r" / "a.txt") == "this server's a",
            _T("this server's file, whose copy in the store was gone before it could be copied, was not put back and left"));
         CheckBackupTree_(!BackupTreeHas_(data6 / "r" / "b.txt") && vanished.copy.vanished == 2,
            Formatter::Format(_T("{0} file(s) were found gone from the store, where two were"), vanished.copy.vanished));
         CheckBackupTree_(vanished.kept.empty() && !BackupTreeHasAside_(data6), _T("the file put back was said to be left, or something moved aside was left behind"));
      }

#ifndef HM_PLATFORM_POSIX
      // 5. WHAT THE SERVICE'S ACCOUNT MAY NOT CHANGE, as a new installation's
      //    self-signed key is to NT SERVICE\hMailServer (Read only, in a folder
      //    whose Modify does not include deleting a child): here a file this
      //    process may read and not delete or write, in a folder in which it may
      //    not delete a child. One that differs from the archive's, one the
      //    archive holds byte for byte and one the archive does not hold: all
      //    three left as they are, the first and the last said, and everything
      //    else restored. Windows only: on Linux the service owns its data
      //    directory, and a file in it is the service's to rename or remove.
      {
         const fs::path data5 = scratch / "protected-data";
         const fs::path store5 = scratch / "protected-store";
         MakeBackupTreeFile_(data5 / "Keys" / "imported.key", "this server's key");
         MakeBackupTreeFile_(data5 / "Keys" / "same.key", "a key the archive has byte for byte");
         MakeBackupTreeFile_(data5 / "Keys" / "orphan.key", "a key the archive does not hold");
         MakeBackupTreeFile_(data5 / "example.test" / "old.eml", "a message the archive does not hold");
         MakeBackupTreeFile_(store5 / "Keys" / "imported.key", "the archive's key");
         MakeBackupTreeFile_(store5 / "Keys" / "same.key", "a key the archive has byte for byte");
         MakeBackupTreeFile_(store5 / "Keys" / "new.pem", "a file the archive adds beside them");
         MakeBackupTreeFile_(store5 / "example.test" / "new.eml", "the archive's message");

         const std::vector<fs::path> protectedFiles = { data5 / "Keys" / "imported.key", data5 / "Keys" / "same.key", data5 / "Keys" / "orphan.key" };

         // Given back before the scratch tree is removed, however the test ends.
         struct Unprotect
         {
            std::vector<fs::path> paths;
            ~Unprotect() { for (const fs::path &path : paths) BackupTreeUnprotect_(path); }
         } unprotect;

         const String self = BackupTreeSelfSid_();
         CheckBackupTree_(!self.IsEmpty(), _T("this process's account could not be read"));

         unprotect.paths.push_back(data5 / "Keys");
         BackupTreeProtect_(data5 / "Keys", Formatter::Format(_T("D:P(D;;0x40;;;{0})(A;OICI;FA;;;{0})(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"), self));
         for (const fs::path &file : protectedFiles)
         {
            unprotect.paths.push_back(file);
            BackupTreeProtect_(file, Formatter::Format(_T("D:P(D;;0x10006;;;{0})(A;;FA;;;{0})(A;;FA;;;SY)(A;;FA;;;BA)"), self));
         }

         boost::system::error_code refused;
         fs::rename(data5 / "Keys" / "orphan.key", data5 / "Keys" / "orphan.key.moved", refused);
         CheckBackupTree_(refused.value() != 0, _T("the protected file could be moved aside, so this part proves nothing"));

         RestoreOutcome_ protectedOutcome;
         DataDirectoryExclusions_ none;
         CheckBackupTree_(RestoreStoreInto_(store5, data5, String(), none, none, protectedOutcome),
            _T("the restore stopped at a file the service may not change: ") + DescribeCopyFailure_(protectedOutcome.copy));
         CheckBackupTree_(BackupTreeRead_(data5 / "Keys" / "imported.key") == "this server's key", _T("the file the service may not change was changed"));
         CheckBackupTree_(BackupTreeHas_(data5 / "Keys" / "orphan.key"), _T("the file the service may not remove is gone"));
         CheckBackupTree_(BackupTreeRead_(data5 / "Keys" / "new.pem") == "a file the archive adds beside them", _T("the archive's file beside them was not restored"));
         CheckBackupTree_(BackupTreeHas_(data5 / "example.test" / "new.eml") && !BackupTreeHas_(data5 / "example.test" / "old.eml"),
            _T("the mail was not restored around the files the service may not change"));
         CheckBackupTree_(protectedOutcome.kept.size() == 2,
            Formatter::Format(_T("{0} file(s) were said to be left as they are, where two are: the one that differs and the one the archive does not hold"), (unsigned int) protectedOutcome.kept.size()));
         CheckBackupTree_(protectedOutcome.already == 1, _T("the file the archive holds byte for byte was not found to be so"));
         CheckBackupTree_(!BackupTreeHasAside_(data5), _T("a file the restore moved aside was left behind"));
      }
#endif
   }
}
