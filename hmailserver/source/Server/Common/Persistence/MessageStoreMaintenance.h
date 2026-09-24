// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The message store taken as a whole: every stored file, walked.
//
//   hmailserver --measure-store [--account <address> | --domain <name>]
//        what compression and holding large parts once WOULD save, changing
//        nothing: each plain file compressed in memory, its large parts found,
//        a part met twice counted once, a file with several names counted once.
//
//   hmailserver --compress-store [--account <address> | --domain <name>] [--decompress]
//        every stored file rewritten in place, compressed with its large parts
//        held once, or back to plain RFC 822 with --decompress. Each file is
//        written beside itself, read back and compared with the message byte for
//        byte before it is renamed over its name; the message's row is untouched,
//        because the name does not change. A file with several names under
//        DeliveryHardLinks is rewritten once and every name relinked to the new
//        one - or, when some of its names are outside the walk (another account's
//        folders under --account), left and counted: never turned into a copy
//        per name. --compress-store leaves a zero-access mailbox's files as they
//        are (ciphertext does not compress); --decompress puts back any container
//        among them, as it does everywhere else.
//
//   hmailserver --recount-store
//        every blob's count set to what the stored files reference, lowered where
//        it is too high and a row nothing references removed: the one recount
//        that lowers, and so the one that runs only with the server stopped.
//
// The rewrite and the recount are refused beside a running server: on Windows by
// asking the service controller (hMailServer.cpp), on Linux by a lock the server
// takes for as long as it runs (TakeStoreLock). Exit 4.
//
// And for the server: the totals GET /api/v1/store answers, measured on the
// asynchronous work queue; the recount a restore ends with, which only raises;
// and the blobs the consistency check reports.

#pragma once

namespace HM
{
   class MessageStoreMaintenance
   {
   public:

      enum Verb
      {
         VerbMeasure = 1,
         VerbCompress = 2,
         VerbDecompress = 3,
         VerbRecount = 4
      };

      // The command-line verbs, over the whole store, one account, or one
      // domain's accounts (accountAddress and domainName, at most one given).
      // The exit code: 0 done, 1 a file or the store could not be read or
      // written, 2 not configured or an unknown account or domain, 4 the server
      // is running (Linux; Windows answers 4 before this is called).
      static int RunCommand(Verb verb, const String &accountAddress, const String &domainName);

      // One stored file: a delivered message's, in an account's folder or a
      // public folder.
      struct StoredFile
      {
         StoredFile() : message_id(0), size(0), zero_access(false) {}
         __int64 message_id;
         __int64 size;          // the row's messagesize
         String account;        // empty for a public folder
         String path;
         bool zero_access;
      };

      // Every stored file, one account's, or one domain's accounts' - the
      // account and the domain found as the server finds them at a sign-in,
      // without regard to case on every database. False when the database
      // cannot be read or the account or domain is not there.
      static bool ListStoredFiles(const String &accountAddress, const String &domainName, std::vector<StoredFile> &files, String &problem);

      // The files the recover-deleted store keeps, which reference blobs too.
      static void ListKeptFiles(std::vector<String> &files);

      // Every blob's count set from the stored files and the kept copies. exact
      // lowers and removes as well as raises - the command line's, with the
      // server stopped; otherwise a count is only raised (MessageStoreBlobs::
      // SetCounts), which is what the end of a restore runs in the service.
      static bool Recount(String &summary, bool exact);

      // For the consistency check: a referenced blob that is missing, as a
      // missing file ("messageid<TAB>account<TAB>path (blob <sha256> missing)"),
      // and every blob nothing references. Nothing is changed.
      static void FindBlobProblems(std::vector<String> &missing, std::vector<AnsiString> &unreferenced);

      struct Totals
      {
         Totals() : measured(0), messages(0), logical_bytes(0), stored_bytes(0), containers(0),
                    blobs(0), blob_bytes(0), blob_references(0) {}
         __int64 measured;         // seconds since 1970; 0 for never
         __int64 messages;         // delivered messages whose files could be sized
         __int64 logical_bytes;    // what IMAP and POP3 say those messages are
         __int64 stored_bytes;     // what their files take on disk
         __int64 containers;       // how many of the files are containers
         __int64 blobs;
         __int64 blob_bytes;
         __int64 blob_references;
      };

      // When a request for the totals starts a measurement.
      enum MeasureWhen
      {
         // When the last is older than a quarter of an hour, or there is none:
         // COM's MessageStoreStatus, which the Control Panel reads when its page
         // is opened.
         MeasureIfDue = 0,

         // The same, and only while the store is in use - compression on, a
         // part held once, or a container seen by the last measurement:
         // GET /api/v1/store, which the Deck's dashboard asks on every refresh,
         // and which on the shipped default walks nothing.
         MeasureIfDueWhileInUse = 1,

         // Now, unless one is running: POST /api/v1/store/measure.
         MeasureImmediately = 2
      };

      // The last measurement, and whether one is running.
      static Totals GetTotals(bool &measuring, MeasureWhen when);

      // The measurement itself, run on the asynchronous work queue.
      static void MeasureNow();

      // What GET /api/v1/store answers, and COM's Settings.MessageStoreStatus:
      // the last measurement with the two settings beside it.
      static AnsiString TotalsJson(MeasureWhen when);

      // The lock that keeps the walk's rewrite and recount from running beside
      // the server where no service controller can be asked (POSIX; on Windows
      // it answers taken and does nothing). The server takes it shared for as
      // long as it runs, a walk exclusive until it exits: 0 taken, 1 the other
      // holds it, 2 it could not be opened (the reason in problem). A file in
      // the log directory, which a restore of the data directory leaves alone.
      static int TakeStoreLock(bool exclusive, String &problem);
   };
}
