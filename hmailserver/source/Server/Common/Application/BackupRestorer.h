// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Backup;

   // Everything a restore has to establish BEFORE it deletes anything.
   //
   // WHY THIS IS A SEPARATE PHASE
   //
   // A restore is the only operation in hMailServer that deletes every domain,
   // every account, every public folder and the whole message store on purpose,
   // and there is no transaction around any of it: Collection::DeleteAll issues one
   // DELETE per object and PersistentDomain::DeleteObject also removes the domain's
   // folder from disk. Nothing can put that back if the restore then fails. So a
   // restore has exactly one safe shape - check everything that can be checked, and
   // get everything that has to be copied onto local disk, before the first
   // deletion.
   //
   // That was not the shape it had. BackupExecuter::StartRestore dropped all the
   // domains first and only afterwards went looking inside the archive for the
   // things it needed, which is how each of these was reachable from the Control
   // Panel's three restore checkboxes and from BackupManager.LoadBackup over COM -
   // neither of which consults what the archive actually contains:
   //
   //  * Restoring "messages" from an archive that has no message store (any backup
   //    taken with BackupMessages off) dereferenced the null returned by
   //    GetChildAttr("DataFiles", "Format"). The access violation is trapped and
   //    logged by ExceptionHandler::Run rather than crashing the service, so the
   //    visible result was a server with every domain deleted, nothing restored, no
   //    BACKUP ERROR line in the backup log and no OnBackupFailed event - just an
   //    exception dump. The guard added later in RestoreDataDirectory_ for "the
   //    backup does not contain a message store" could never run, because the null
   //    dereference happened forty lines earlier.
   //
   //  * Restoring "domains" from an archive that contains no domains deleted every
   //    domain and reported success. Collection::XMLLoad deletes what is there and
   //    then loads from a child node that is simply absent, which is
   //    indistinguishable from a collection that was empty at backup time.
   //
   //  * Restoring "settings" from an archive that contains no settings did the same
   //    to the SSL certificates, TCP/IP ports, security ranges, global rules,
   //    blocked attachments, DNS black lists and white lists - every collection
   //    Configuration::XMLLoad touches.
   //
   //  * An archive written by a newer hMailServer was restored into an older one
   //    without a word. The backup index has carried a Version attribute since
   //    2010 and nothing had ever read it.
   //
   // The class only decides and stages. It reports its reasons back to the caller
   // as text instead of logging them itself, so that the one place that knows
   // whether this is a restore or a LoadBackup - and therefore whether the reason
   // belongs in the backup log, in an OnBackupFailed event, or in a COM error - is
   // the place that says so.
   class BackupRestorer
   {
   public:
      BackupRestorer();
      ~BackupRestorer();

      // Reads an archive's index and reports what the archive contains (a
      // combination of Backup::BackupOptions) and which hMailServer wrote it.
      // False, with a reason, when the archive cannot be read or is not one of
      // ours - which is what a truncated or half-copied archive looks like.
      //
      // Static and self-contained because two callers need exactly this and nothing
      // more: BackupManager::LoadBackup, which must not hand out a Backup object
      // for an archive that cannot be restored, and the check
      // BackupExecuter::StartBackup makes on the archive it has just written,
      // before retention is allowed to delete an older one.
      //
      // archiveOpenTimeoutMilliseconds bounds the archiver run that reads the
      // index out of the archive. Zero is no bound at all, which is what the two
      // original callers need: both are on a thread whose whole job is this one
      // backup, and 7za reading a large archive off a slow disk is not a fault.
      // A caller on a thread that has to be given back - the REST route, on one
      // of four HTTP workers - passes one, and openTimedOut then says whether
      // that bound is the reason this returned false, because "it did not open in
      // the time allowed" and "it did not open" send the reader to different
      // places.
      static bool ReadArchiveIndex(const String &archiveFile, const String &passphrase, int &archiveContents, String &archiveVersion, String &failureReason,
                                   unsigned int archiveOpenTimeoutMilliseconds = 0, bool *openTimedOut = nullptr);

      // Whether what the caller asked to restore is in the archive at all, and
      // whether the parts asked for make sense together. False leaves the reason -
      // the sentence the administrator is shown - in failureReason.
      //
      // Static and separate from Prepare because the answer is worth having before
      // a restore is started at all: the REST route refuses the request with it,
      // synchronously, rather than queueing a restore that will fail on the
      // maintenance thread and report itself as a critical error.
      static bool CheckSelection(int archiveContents, int restoreOptions, String &failureReason);

      // False, with a reason, when archiveVersion is a later hMailServer version
      // than the one running. An unrecognised or absent version is accepted: the
      // point is to stop a restore that is known to be backwards, not to refuse
      // every archive whose provenance cannot be proved.
      static bool VersionIsRestorable(const String &archiveVersion, String &failureReason);

      // Validates the archive against what the caller has asked to restore and
      // stages whatever has to be on local disk first. Nothing outside the
      // temporary directory is touched, so a false return leaves the server exactly
      // as it was.
      bool Prepare(std::shared_ptr<Backup> backup, String &failureReason);

      // Opens an archive for a restore of PART of it - one account's mailbox, or
      // one message of it (MailboxRestore) - the way Prepare opens one for a
      // whole restore: the index read and checked, the version checked. Nothing
      // is staged; the caller stages the one directory it needs with
      // StageAccountMessageStore. The timeout and openTimedOut are
      // ReadArchiveIndex's, for the caller that is on an HTTP worker.
      bool OpenIndex(const String &archiveFile, const String &passphrase, unsigned int archiveOpenTimeoutMilliseconds,
                     String &failureReason, bool *openTimedOut);

      // What the opened index says the archive contains: a combination of
      // Backup::BackupOptions. Valid after OpenIndex or Prepare.
      int GetArchiveContents() const;

      // Stages one account's message files - DataBackup\<domain>\<account> and
      // nothing else - so that restoring one mailbox never extracts the whole
      // store for it. Afterwards GetStagedMessageStore is the ACCOUNT's directory
      // (the two-character fan-out folders are under it), or is empty when the
      // archive holds no files under that name, which the caller judges: an
      // account whose folders held nothing has no directory in the archive, and
      // one whose index lists messages but whose files are missing was backed up
      // with BackupMessagesDBOnly. Needs OpenIndex first and an archive that
      // contains messages. The directory names are the store's own
      // (StoreDirectoryName), which is how the executer wrote them.
      //
      // The timeout bounds the archiver run, for the listing that stages on an
      // HTTP worker to read subjects; zero is no bound, which the restore itself
      // uses. On the bound the staged directory is whatever had been extracted
      // and timedOut says so.
      bool StageAccountMessageStore(const String &archiveFile, const String &domainDirectory, const String &accountDirectory,
                                    unsigned int archiveOpenTimeoutMilliseconds, bool *timedOut, String &failureReason);

      // The <Backup> element of the validated index. Owned by this object - it dies
      // with the BackupRestorer, so the restore has to finish inside its lifetime.
      XNode *GetBackupNode();

      // What Prepare accepted, which is what the restore should act on.
      int GetRestoreOptions() const;

      bool GetHasStagedMessageStore() const;

      // The directory holding the message store to copy into the data directory.
      // Either a directory this object extracted (deleted again by the destructor)
      // or, for an uncompressed backup, the DataBackup folder sitting next to the
      // archive - which is part of the backup itself and is never deleted here.
      String GetStagedMessageStore() const;

      // Keeps the extracted message store on disk instead of removing it. Called by
      // the one caller that has already emptied the live data directory: at that
      // point the extracted copy is the only copy of the mail that exists, and
      // tidying up would destroy it.
      void RetainStagedFiles();

   private:

      // Not copyable. This object owns an XDoc whose nodes are handed out as raw
      // pointers, and temporary directories that its destructor removes; a copy
      // would delete the other's directory and dangle the other's nodes.
      BackupRestorer(const BackupRestorer &);
      BackupRestorer &operator=(const BackupRestorer &);

      bool ReadIndex_(const String &archiveFile, const String &passphrase, unsigned int archiveOpenTimeoutMilliseconds, String &failureReason, bool *openTimedOut);
      bool StageMessageStore_(const String &archiveFile, String &failureReason);

      XDoc document_;
      XNode *backup_node_;

      int archive_contents_;
      int restore_options_;

      String archive_version_;

      String passphrase_;

      String index_directory_;
      String staging_directory_;
      String staged_message_store_;

      bool retain_staged_files_;
   };
}
