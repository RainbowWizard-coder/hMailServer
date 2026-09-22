// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <set>
#include <vector>

namespace HM
{
   class Account;
   class IMAPFolder;
   class IMAPFolders;
   class BackupRestorer;

   // One message row as the backup index holds it (Message::XMLStore). The index
   // carries no subject: that is in the file, which is read only where a listing
   // is asked for it (MailboxRestore::ReadSubjects).
   struct ArchivedMessage
   {
      ArchivedMessage() : id(0), size(0), flags(0), uid(0) { }

      // The ID attribute: the row's database id when the backup was taken.
      // Unique within one archive, which is what a listing and a one-message
      // restore name a message by. It is not the id the restored row gets.
      __int64 id;

      // The bare file name, which is the message's identity: the executer names
      // a file once, with a GUID, and the same name in the live mailbox is the
      // same message.
      String filename;

      String from_address;
      String create_time;
      String keywords;
      String subject;
      int size;
      int flags;
      unsigned int uid;
   };

   struct ArchivedFolder
   {
      ArchivedFolder() : subscribed(false), special_use(0) { }

      // INBOX, or Projects then 2026: the names down from the account's root.
      std::vector<String> path;
      bool subscribed;
      int special_use;
      std::vector<ArchivedMessage> messages;
   };

   struct ArchivedMailbox
   {
      String address;      // as the archive spells it
      String domain_name;
      std::vector<ArchivedFolder> folders;

      size_t MessageCount() const;
   };

   // RESTORING ONE MAILBOX, OR ONE MESSAGE, FROM A BACKUP
   //
   // A whole restore (BackupExecuter::StartRestore) deletes every domain and
   // puts the archive's back; this puts one account's folders and messages back
   // as the archive holds them, and touches nothing else on the server - not the
   // account's settings, rules, contacts or calendars, which are the account
   // and not its mailbox, and not any other account.
   //
   // Two modes. MERGE creates each archived folder that is missing and adds each
   // archived message whose file is not in the live mailbox; a message that is
   // still there, in whatever folder, is left where it is, so merging the same
   // archive twice adds nothing the second time. REPLACE empties every live
   // folder first and then fills as merge would, so that the mailbox is the
   // archive's - mail that arrived since the backup is gone; the folders
   // themselves, with their subscriptions and rights, stay. A single message is
   // merge of one row, into the folder it was in or a named one.
   //
   // Modelled on BackupRestorer's staging and never on the executer's deleting:
   // the archive is opened and its index checked on the caller's thread, so a
   // wrong request is refused with the reason before anything runs; the job
   // then runs on the backup task's thread under the same run slot as a whole
   // restore, so two restores never overlap, and it reads everything it needs -
   // the index, the account's message files, extracted to a directory of its
   // own - before replace deletes anything. The rows are written through
   // PersistentMessage, so the mailbox's UIDs, mod-sequences and size follow,
   // and every session with the folder selected is told the way delivery tells
   // it.
   class MailboxRestore
   {
   public:
      enum Mode
      {
         ModeMerge = 1,
         ModeReplace = 2
      };

      struct Request
      {
         Request() : mode(ModeMerge), message_id(0) { }

         String archive;        // as the caller named it
         String archive_file;   // as ResolveArchive resolved it
         String passphrase;     // an encrypted archive's; empty otherwise
         String address;
         Mode mode;
         __int64 message_id;    // 0: the whole mailbox; else this one message
         String folder;         // for one message: the live folder path, or empty for the archived one
      };

      // What became of a job, as the status route and the log report it.
      struct Outcome
      {
         Outcome() : job(0), message_id(0), folders(0), added(0), skipped(0), failed(0) { }

         int job;
         String state;          // queued, running, done, failed
         String archive;
         String address;
         String mode;           // merge, replace, message
         __int64 message_id;
         int folders;           // live folders written to (created or existing)
         int added;
         int skipped;           // still in the live mailbox: the same file name
         int failed;            // a file the archive does not hold, or a row that could not be written
         String error;
         String started;
         String finished;
      };

      // The names the two modes go by over every interface.
      static String ModeName(Mode mode);
      static bool ParseMode(const String &name, Mode &mode);

      // Resolves what a caller named - the bare file name the archive listing
      // answers, or a path - to a file inside the backup destination, and
      // refuses everything else: a path outside the destination, a path with
      // no destination set, a file that is not there. notFound tells "not
      // there" from "not allowed", because the two are different answers.
      static bool ResolveArchive(const String &named, String &archiveFile, bool &notFound, String &failureReason);

      // The account's folders and messages as the archive holds them. False
      // with the reason when the archive cannot be read or holds no messages;
      // accountNotFound when it opened and has no such account. The timeout and
      // openTimedOut are BackupRestorer::ReadArchiveIndex's.
      static bool ReadMailbox(const String &archiveFile, const String &passphrase, const String &address,
                              unsigned int openTimeoutMilliseconds, bool *openTimedOut,
                              ArchivedMailbox &mailbox, bool &accountNotFound, String &failureReason);

      // Fills each listed message's subject from its file, staging the
      // account's files from the archive to do it. Bounded, because a listing
      // is answered on an HTTP worker: at most SUBJECTS_LIMIT messages are read,
      // and the archiver run is bounded by the timeout. False, with the reason,
      // when the subjects could not be read; the listing is still good without
      // them.
      static const size_t SUBJECTS_LIMIT = 500;
      static bool ReadSubjects(const String &archiveFile, const String &passphrase, ArchivedMailbox &mailbox,
                               unsigned int timeoutMilliseconds, String &failureReason);

      // Everything that can be checked before the job is queued, on the caller's
      // thread: the archive resolved and opened, the account found in it and on
      // this server, the mode, the message and the folder. A job ready to hand
      // to BackupManager::StartMailboxRestore, or nothing with the status code
      // the refusal deserves (400, 404 or 504) and the reason.
      static std::shared_ptr<MailboxRestore> Prepare(const Request &request, unsigned int openTimeoutMilliseconds,
                                                     int &statusCode, String &failureReason);

      // The registry of jobs, process-wide for the reason BackupManager's run
      // slot is: the manager is recreated on every reinitialization and a job
      // outlives one. The last fifty outcomes are kept.
      static bool GetOutcome(int job, Outcome &outcome);
      static std::vector<Outcome> ListOutcomes();

      // The two documents every interface answers with, built once so that
      // REST and COM cannot describe one mailbox two ways: a listing (archive,
      // address, folders with their messages, the count, whether subjects
      // were read and why not) and an outcome. Wide text; the REST routes
      // encode it as UTF-8 and COM hands it out as it is.
      static String ListingJson(const String &archiveFile, const ArchivedMailbox &mailbox, bool subjects, const String &subjectsNote);
      static String OutcomeJson(const Outcome &outcome);

      // A folder's path as one string, with the IMAP hierarchy delimiter.
      static String FolderPathText(const std::vector<String> &path);

      int GetJob() const;
      const Request &GetRequest() const;

      // The sentence the audit trail records the request with, and the backup
      // log opens the run with.
      String Describe() const;

      // The run, on the backup task's thread.
      void Run();

      ~MailboxRestore();

   private:
      explicit MailboxRestore(const Request &request);

      MailboxRestore(const MailboxRestore &);
      MailboxRestore &operator=(const MailboxRestore &);

      static bool ParseMailbox_(XNode *backupNode, const String &address, ArchivedMailbox &mailbox);
      static void ParseFolders_(XNode *foldersNode, const std::vector<String> &parentPath, ArchivedMailbox &mailbox);

      static int RegisterJob_(const Request &request);
      static void UpdateJob_(const Outcome &outcome);

      bool RunInner_(std::shared_ptr<const Account> account, String &failureReason);
      bool RestoreFolder_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> tree,
                          const ArchivedFolder &archived, const std::vector<String> &livePath,
                          const std::vector<const ArchivedMessage *> &messages, const String &stagedStore,
                          std::set<String> &liveFileNames);
      int EmptyLiveFolders_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> folders, int &foldersEmptied);
      void Log_(const String &line) const;

      Request request_;
      Outcome outcome_;
      ArchivedMailbox mailbox_;

      // The first ten files the archive did not hold, for the log.
      std::vector<String> missing_named_;
   };
}
