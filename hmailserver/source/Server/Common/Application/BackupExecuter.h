// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once


namespace HM
{
   class Domain;
   class IMAPFolders;
   class IMAPFolder;
   class Messages;
   class Message;
   class BackupManager;
   class BackupRestorer;

   class BackupExecuter
   {
   public:
      BackupExecuter();
      ~BackupExecuter(void);

      bool StartBackup();
      bool StartRestore(std::shared_ptr<Backup> pBackup);

   private:

      void LoadSettings_();

      bool BackupDomains_(XNode *pNode);
      bool BackupDataDirectory_(const String &sDataBackupDir);

      // Reads back the archive this run has just written and confirms it is one an
      // hMailServer of this version could restore. Runs before BackupRetention is
      // allowed to consider deleting anything, so an archive that cannot be read is
      // never the reason an older one that could be read was deleted.
      bool VerifyArchive_(const String &sZipFile);

      // The verified restore. Extracts the archive's message store to a scratch
      // directory through the code path a restore uses, holds it to exactly the
      // files this run staged for compression, and reconciles the message rows in
      // the index against those files. discardArchive says whether a false return
      // means the archive itself is bad (discard it) or only that it could not be
      // checked (keep it, but do not call the backup a success). Off with
      // BackupVerifyRestore=0.
      bool VerifyRestore_(const String &sZipFile, XNode *pBackupNode, bool &discardArchive);

      // Takes the restorer rather than the Backup and the XML node, because by the
      // time this is called the archive has been validated and the message store is
      // already on local disk - and because the one thing this function has to be
      // able to say, when a copy that failed could not be undone in full, is "keep
      // the staged files". The store is copied in first, over what is there; what
      // the archive does not hold is removed only once the copy is complete; a copy
      // that fails is undone; and a file of this server's that the service's
      // account may not replace or remove - this installation's own self-signed
      // key - is left as it is and said (BackupExecuter.cpp, "HOW A RESTORE
      // REPLACES THE DATA DIRECTORY"). archiveFile is the archive being restored:
      // a restore leaves it, the destination and the temp directory where they
      // are, wherever they are inside the data directory.
      bool RestoreDataDirectory_(BackupRestorer &restorer, const String &archiveFile);
      void ReportRestoreFailure_(const String &message);
      
      int backup_mode_;

      // What BackupDataDirectory_ staged for compression - the count and the bytes
      // the extracted store is held to by VerifyRestore_.
      unsigned int staged_files_;
      unsigned __int64 staged_bytes_;
      
      // Backup properties
      String destination_;
      String xmlfile_;

      // Whether this run's archive is to be encrypted, and the passphrase it is
      // encrypted with. Read once at the start of the run (LoadSettings_), so that
      // a passphrase changed while a backup is running cannot leave one archive
      // written under two of them.
      bool encrypt_;
      String passphrase_;
   };

   // What the copy of the data directory a backup makes, and the restore of one
   // (RestoreStoreInto_), leave alone and do (DataDirectoryExclusions_,
   // IsKeptOutOfBackup_ and "HOW A RESTORE REPLACES THE DATA DIRECTORY" in
   // BackupExecuter.cpp), on scratch trees of its own under the system's temp
   // directory: a destination inside the data
   // directory, written in capitals with forward slashes on Windows and through
   // a symbolic link elsewhere, is left out of the copy with the temp directory
   // and the live update's folder, and one directory whose name begins with the
   // data directory's is not taken for it, and what an encrypted backup
   // measures first is what the copy then stages; a restore of an older
   // archive's store keeps the destination's archives, the temp directory with
   // the store extracted into it, the archive being restored three levels down
   // and the live update's folder, puts the mail back, the mail it holds in the
   // archive's own folder too, and not its live update outcome, nor its copies
   // of the destination and the temp directory; leaves a file here byte for
   // byte, replaces one that differs, removes what the archive does not hold,
   // and at the root only the queue's files and what a writer left of one,
   // never this server's own - the Linux key file of the LINUX1 secrets among
   // them - nor puts a file of the store's root over one, nor this
   // installation's own self-signed pair when the archive's key is another,
   // which it restores when the key is the same or the data directory has none;
   // a copy that fails at any of five files, outright or leaving part of the
   // file as a full disk does, leaves the data directory exactly as it was, and
   // the same restore then completes; a file gone from the store before it
   // could be copied leaves this server's file of that name in place; and, on
   // Windows, a
   // file this process may read and neither delete nor write, in a folder it
   // may not delete a child of - as the service's account is to a new
   // installation's key - is left as it is, whether it differs from the
   // archive's, matches it or is not in it, and everything around it restored.
   // The exclusions are built by the functions the backup and the restore build
   // theirs with. It runs inside the service
   // with the other class tests (ClassTester), and on Linux as backup-tests.
   // It needs no database and no setting. It throws std::logic_error naming the
   // first thing that is wrong.
   class BackupExecuterTester
   {
   public:
      void Test();
   };
}