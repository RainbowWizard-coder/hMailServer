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
#include "../Persistence/MessageStoreBlobs.h"

#include "../../IMAP/IMAPConfiguration.h"

#include <boost/filesystem.hpp>

#include <set>

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
   // (docs/RecoverDeleted.md) and the eDiscovery cases' results and exports
   // (docs/Discovery.md). Each is indexed by tables the XML backup does not hold
   // - hm_recoverable, and the four discovery tables - and that a restore leaves
   // as they are, so a backup's copy is files no row names, and a restore that
   // replaced them would leave live rows naming files that are gone: a case's
   // results and exports, and what a legal hold kept. An export is a copy of
   // the custodians' mail of up to 4 GB besides, and a backup that carried every
   // export ever made would grow with each one.
   //---------------------------------------------------------------------------()
   {
      const String name(directory.filename().wstring());
      return name.CompareNoCase(_T("Recoverable")) == 0 || name.CompareNoCase(_T("Discovery")) == 0;
   }

   static bool
   CopyTree_(const boost::filesystem::path &from, const boost::filesystem::path &to, TreeCopyOutcome &outcome,
             bool skipKeptOutOfBackup = false)
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

            if (!CopyTree_(current, to / current.filename(), outcome))
               return false;
         }
         else
         {
            if (!CopyOneFile_(current, to / current.filename(), outcome))
               return false;
         }
      }

      return true;
   }

   static bool
   CopyTreeGuarded_(const String &from, const String &to, TreeCopyOutcome &outcome, bool skipKeptOutOfBackup)
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
         return CopyTree_(boost::filesystem::path(from.c_str()), boost::filesystem::path(to.c_str()), outcome, skipKeptOutOfBackup);
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
   DeleteDataDirectoriesForRestore_(const String &dataDirectory)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // FileUtilities::DeleteDirectoriesInDirectory, less what IsKeptOutOfBackup_
   // names: every directory at the data directory's root is removed but the
   // recoverable store and the eDiscovery cases' files. Listed first and removed
   // after, so the listing is not walked while it changes.
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
   MeasureTree_(const boost::filesystem::path &root, unsigned int &files, unsigned __int64 &bytes,
      std::set<String> *names, String &failureDetail)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Counts the regular files under root and adds up their sizes, optionally
   // collecting their lower-cased names. Symbolic links are neither followed nor
   // counted: the backup copies real files, so a link inside the staged or the
   // extracted tree would itself be a discrepancy, and the count is how it shows.
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

         if (boost::filesystem::is_regular_file(status))
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

      for (size_t i = 0; i < node->childs.size(); i++)
         CollectMessageFilenames_(node->childs[i], filenames);
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

         if (!MeasureTree_(boost::filesystem::path(IniFileSettings::Instance()->GetDataDirectory().c_str()), storeFiles, storeBytes, 0, measureDetail))
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

      // Without the recoverable store and the eDiscovery cases' files: see
      // IsKeptOutOfBackup_.
      bool bResult = CopyTreeGuarded_(sDataDir, sDataBackupDir, outcome, true);
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

            if (!RestoreDataDirectory_(restorer))
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
   BackupExecuter::RestoreDataDirectory_(BackupRestorer &restorer)
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

      // Every directory but the recoverable store and the eDiscovery cases'
      // files, which a backup does not carry and a restore leaves where they
      // are (IsKeptOutOfBackup_); the copy below leaves an older backup's copy
      // of them out in the same way.
      FileUtilities::DeleteFilesInDirectory(sDataDirectory);
      DeleteDataDirectoriesForRestore_(sDataDirectory);

      TreeCopyOutcome outcome;
      bool copied = CopyTreeGuarded_(sDirContainingDataFiles, sDataDirectory, outcome, true);

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
}
