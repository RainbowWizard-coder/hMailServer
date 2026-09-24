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

      struct Report
      {
         std::vector<FolderTally> folders;
         PhaseTally contacts;
         PhaseTally calendars;
         std::vector<ErrorEntry> errors;
         AnsiString firstMessage;    // the earliest internal date copied, "YYYY-MM-DD HH:MM:SS"
         AnsiString lastMessage;
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
