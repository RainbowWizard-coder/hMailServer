// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// One migration job's run, on its own thread (MigrationManager.h has the model).
//
// THE MAIL. The source is logged in to - LOGIN with the password, or XOAUTH2
// with an access token refreshed from the job's refresh token through the
// provider's token URL, as a signed-in fetch account's is - and listed. Each
// folder is mapped to one here: the INBOX to the INBOX; a special-use folder
// (\Sent, \Drafts, \Junk, \Trash, \Archive, by the attribute the source sends,
// else by the name table this server's own folders are known by) to the
// account's own folder of that use; every other folder to the same path with the
// source's delimiter written as this server's. \Flagged and \Important are views
// of mail kept elsewhere and are not copied. \All is copied last, and only what
// the job has not already copied into some other folder - which is how a Gmail
// message in several labels, or archived under none, arrives exactly once.
//
// Other users' mailboxes and the shared ones, as the source's NAMESPACE names
// them, are left to their owners' own jobs and said in the report. A folder is
// opened with EXAMINE - a refusal time mends (UNAVAILABLE, INUSE, LIMIT...) is a
// retry, one it does not (NONEXISTENT, NOPERM, a bare NO) leaves the folder
// uncopied, said in the report's errors and the application log - and the UIDs
// above its mark are fetched one at a time (UID FLAGS INTERNALDATE
// BODY.PEEK[]), the body spooled to the queue directory and filed by
// MailImporter::ImportMessage - the import entry every importer of a mailbox
// shares - with its flags, keywords and internal date, \Seen included, since
// EXAMINE and BODY.PEEK set nothing on the source; the import asks APPEND's size
// ceilings, the volume's room and the quota, and makes the folder when it is
// missing. A message the source will not give is asked for three times, then
// skipped as refused, said the same way; the job goes on. With a limit of
// messages a minute, the job waits between fetches.
//
// THE RESUME MARK. The folder's UIDVALIDITY and the highest UID dealt with are
// written after every 50 messages dealt with - copied or skipped - and at the
// folder's end, with the job's counters and its report. An interruption between
// two marks leaves up to 49 messages copied past the mark; so a job that has run
// before builds, before it fetches, the keys of what it wrote into every folder
// of its plan since it first started - a folder finished before included, since
// the check across folders has to know what went there - (the Message-ID and
// the size, else the Date, Subject and size), and the import skips a message
// found there as a duplicate. A UIDVALIDITY that changed restarts the folder
// from its first UID against the same keys. What arrived beside the job's writes
// - a delivery to the same folder during the migration - is among them too,
// which only ever suppresses a second copy of a message the folder holds
// already. A run that copies a message counts the job's attempts afresh, so the
// ceiling is attempts in a row that achieved nothing.
//
// CONTACTS AND CALENDARS, after the mail: MigrationSources reads them, asking
// before every request whether the job has been told to stop, and each card and
// object up to the megabyte this server's own DAV takes goes into the account's
// address books and calendars through the stores the webmail and DAV write
// through (ContactStore, CalendarStore). A phase that cannot run - no DAV on the
// host, a token without the scope - is skipped with its reason in the report,
// and never fails the job; one that keeps meeting a busy source is skipped the
// same way after MaxDavAttempts in a row.
//
// THE REPORT, JSON in the job's row, written at every mark and at the end, so it
// is readable while the job runs: per folder the source's count, what was
// copied, what was skipped and why; the contacts and events copied; the bytes;
// the duration; the errors met and retried; the first and last message dates.
//
// A PST FILE (kind pst, MigrationJobPst.cpp) is a job of its own shape: its
// source is a file in the target domain's PST import directory rather than a
// mailbox on a server, read by this server's own PST reader (Common/Pst) in a
// process of its own (Pst/PstReaderProcess.h) and filed through the same import
// entry, with the same marks, report and throttle; MigrationJobPst.cpp has the
// whole of it. Its contacts, calendar
// items and tasks are the contacts and calendars phases, read from the same
// file into the same stores (MigrationJobPstItems.cpp).

#pragma once

#include "../Util/MailImporter.h"
#include "../Util/MigrationStore.h"

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace HM
{
   class Account;
   class JsonValue;
   class MigrationImapClient;
   struct MigrationProvider;

   class MigrationJob
   {
   public:
      MigrationJob(__int64 jobId, std::shared_ptr<std::atomic<int> > signal);
      ~MigrationJob();

      // The thread's body. Everything it decides it writes to the job's row
      // before it returns.
      void Run();

      // What the report holds, in memory; public for MigrationJobTester.
      struct FolderTally
      {
         FolderTally() : sourceMessages(0), copied(0), uidValidityRestarts(0) { }

         AnsiString source;          // UTF-8, as the source named it
         AnsiString target;          // UTF-8, the path here
         AnsiString specialUse;      // "\\Sent", or empty
         __int64 sourceMessages;
         int copied;
         std::map<AnsiString, int> skipped;   // reason word -> count
         std::map<AnsiString, int> warnings;  // copied without something: word -> count (a PST's)
         int uidValidityRestarts;
         AnsiString note;
      };

      struct PhaseTally
      {
         PhaseTally() : state("pending"), copied(0) { }

         AnsiString state;           // pending, copied, skipped
         int copied;
         std::map<AnsiString, int> skipped;
         AnsiString reason;          // why it was skipped
      };

      struct ErrorEntry
      {
         ErrorEntry() : at(0), attempt(0) { }

         __int64 at;
         int attempt;
         AnsiString error;
      };

      // What a PST job's report adds: the file and its store, what was not
      // read, the items counted and not imported by class, and a refusal.
      // Every string from the file is cut short and has no control
      // characters, and the classes are at most twenty a count and the rest
      // together, so that the part stays small whatever the file holds.
      struct PstSummary
      {
         PstSummary() : size(0), version(0), crypt(0), listAttempts(0), readerRestarts(0) { }

         AnsiString file;            // UTF-8, the name the job carries
         __int64 size;
         AnsiString storeName;
         AnsiString recordKey;       // hex
         int version;
         int crypt;
         std::vector<AnsiString> notRead;             // a sentence each
         // Items not imported by class, counted two ways: the folders that are
         // not mail, by their container class and their count - counted afresh
         // at every list pass - and the messages in mail folders that are not
         // mail, by their message class, one at a time as the job meets them,
         // adding up across runs. The report's not_mail is the two together;
         // each is kept under its own key as well, which is what is read back.
         std::map<AnsiString, int> notMailFolders;
         std::map<AnsiString, int> notMail;
         AnsiString refusal;
         int listAttempts;           // store-and-list passes begun and not finished: the service stopped in one
         int readerRestarts;         // readers started after one ended mid-request, across the job's runs and phases
      };

      struct Report
      {
         Report() : isPst(false) { }

         std::vector<FolderTally> folders;
         PhaseTally contacts;
         PhaseTally calendars;
         std::vector<ErrorEntry> errors;
         AnsiString firstMessage;    // the earliest internal date copied, "YYYY-MM-DD HH:MM:SS"
         AnsiString lastMessage;
         bool isPst;
         PstSummary pst;
      };

      // The report as JSON, and back. Kept under MigrationStore::MaximumReportBytes
      // by leaving out folders past what fits, which the JSON then says.
      static AnsiString ReportJson(const MigrationJobRecord &job, const AnsiString &target, const Report &report);
      static void ParseReport(const AnsiString &json, Report &report);

      // A JSON string of UTF-8 text, quotes included.
      static AnsiString JsonString(const AnsiString &utf8);

      // What a state and a phase are called in the report and over REST.
      static const char *StateWord(int state);
      static const char *PhaseWord(int phase);

      // A PST import's rules (MigrationJobPst.cpp), public for PstTester.
      //
      // The path a PST folder is filed into, from its names in the file (UTF-8,
      // the IPM subtree's child first): each name made one this server's folders
      // may carry - the hierarchy delimiter inside it an underscore, a first
      // name starting with # given an underscore in front, an empty one
      // "Unnamed folder", and each cut at a character boundary until its
      // modified UTF-7 form, which is what the folder table holds, is at most
      // 255 characters - an "Inbox" at the top the INBOX, and folders deeper than
      // IMAPFolder::MaxFolderDepth filed into their ancestor at that depth. The
      // names come back encoded, as FolderFor takes them; renames names each
      // change, "from -> to".
      static std::vector<String> PstTargetPath(const std::vector<std::string> &names, const String &delimiter,
                                               std::vector<AnsiString> &renames);

      // One name, cleaned as above and encoded.
      static String PstFolderName(const std::string &utf8, const String &delimiter, bool topLevel);

      // A category as an IMAP keyword: every character an atom may not hold,
      // and every character outside ASCII, an underscore.
      static String PstKeyword(const std::string &utf8);

      // The IMAP INTERNALDATE ("17-Jul-1996 03:44:25 +0100") of a FILETIME, in
      // this server's time with its offset - the import keeps the date and time
      // and not the zone, as a delivery's is kept - or empty when it is 0 or not
      // between 1981 and a day after now (unix).
      static String PstInternalDate(unsigned __int64 fileTime, __int64 now);

   private:
      enum Outcome
      {
         OutcomeDone,
         OutcomeStopped,
         OutcomeRetry,
         OutcomeFailed
      };

      bool StopRequested_() const;
      bool WaitUntil_(__int64 milliseconds);

      // Something left undone for good - a folder the source would not open, a
      // message it would not give - kept in the report's errors and said in the
      // application log, so that a job that ends done says so too.
      void Record_(const String &what);

      Outcome RunMail_(String &problem);
      Outcome RunDav_(bool calendars, String &problem);

      // A PST file's mail (MigrationJobPst.cpp).
      Outcome RunPst_(String &problem);

      // A PST file's contacts, into the account's address books; and its
      // calendar items and tasks, into its calendars (MigrationJobPstItems.cpp).
      Outcome RunPstItems_(bool calendars, String &problem);

      // A contacts or calendars phase that met a busy source: a retry, or -
      // after MaxDavAttempts in a row - the phase skipped with the reason.
      Outcome DavAgain_(PhaseTally &tally, const String &why, String &problem);
      bool AccessToken_(const AnsiString &scope, String &token, AnsiString &granted, bool &transient, String &problem);

      FolderTally &Tally_(const AnsiString &source);
      void Save_();
      void Finish_(int state, const String &error);
      void Retry_(const String &error);

      __int64 job_id_;
      std::shared_ptr<std::atomic<int> > signal_;

      MigrationRecord migration_;
      MigrationJobRecord job_;
      std::shared_ptr<const Account> owner_;
      const MigrationProvider *provider_;

      // True on the job's first run; false once it has run before, when what it
      // wrote since it first started is read back for the duplicate check.
      bool first_run_;

      // True once this run has copied a message: its attempts are then counted
      // afresh.
      bool progressed_;

      Report report_;

      // report_.folders by their source, the first of each, and how many of the
      // list that index has seen: a job of a hundred thousand folders is not a
      // hundred thousand scans of it (Tally_). Reset when the report is read.
      std::map<AnsiString, size_t> tally_at_;
      size_t tally_indexed_ = 0;

      // What the job has written, by target folder and anywhere, for the
      // duplicate check the import makes (see the note at the top).
      MailImporter::WrittenKeys written_;
   };

   class MigrationJobTester
   {
   public:
      void Test();
   };
}
