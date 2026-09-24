// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// eDiscovery's work: a search across the custodians' mailboxes and the archive,
// and an export of what it found (docs/Discovery.md).
//
// ONE AT A TIME. The maintenance work queue has five threads, and a search reads
// every message it is asked about whole; so a search or an export is a row
// queued in its table, and one runner at a time works through the queue, the
// oldest first. Kick starts the runner when it is not running; the runner
// stops when the queue is empty.
//
// WHAT A SEARCH MATCHES. Not the webmail's matcher, which is RestApiServer.cpp's
// own and reads neither a message over 1 MB nor the text in attachments. A
// discovery query is narrower - custodians, a date range, words that must all
// be found, an attachment's name, type and size - and it is matched here
// against what IMAP SEARCH TEXT reads: the decoded header, the text body, the
// HTML body and the text of the text/* attachments (MessageData::
// GetAttachmentText, up to 1 MB of characters a message), a word found when it
// is a case-insensitive substring of any of them. That is what the full-text
// index is built from, so when the index is on it rules out a message that
// cannot contain a word exactly as SEARCH does, and is never the answer. Every
// other candidate is read whole, up to the 80 MB MessageData reads; a message
// that cannot be read is listed as not searched, with the reason, never dropped
// as a non-match. A zero-access mailbox is matched on its headers alone - its
// bodies are encrypted to a key this server does not have - and its hits say so
// (headers_only, zero_access). So is a message encrypted end to end - S/MIME or
// PGP/MIME - wherever it is (headers_only); one whose headers lack a word is
// listed as not searched, since its body might hold it.
//
// WHAT IT READS. Each custodian - an address, a domain (every account in it) or
// "*" (every account and the whole archive) - in three places: the account's
// folders (hm_messages), the account's deleted items kept by a hold or by
// RecoverDeletedDays (hm_recoverable), and the archive copies filed under the
// address or the domain (hm_archiveindex, paged by id; the index holds no body,
// so each archived file is read). A search copies nothing: it lists, into
// <data>\Discovery\case-<id>\search-<id>.json, at most 100,000 hits and then
// "truncated".
//
// WHAT AN EXPORT WRITES. <data>\Discovery\case-<id>\export-<id>.zip: every hit
// as the message's own bytes under <custodian>/<folder>/<n>-<message id>.eml,
// an mbox per custodian folder beside them (in parts of at most 256 MB,
// <folder>-part2.mbox and on), SHA256SUMS - the sha256sum lines of every file
// written - and manifest.json, which names the case, the search and its query,
// who ran and who exported it, every item with its SHA-256, size, message id,
// date, from, to, subject and whether it is held, every hit not written with the
// reason, what the search could not read with the reason, and the SHA-256 of
// SHA256SUMS. The zip's own SHA-256 is kept in the export's row. ZipWriter
// writes the 32-bit format, so an export that would pass 4 GB is refused with
// the sentence, and so is one that would leave the data directory's volume with
// less than a gigabyte free; a zero-access mailbox's messages - one that has
// zero access on when the export runs included - are listed as withheld and not
// written, since that mailbox's administrative export answers 403 by design.
#pragma once

#include <vector>

namespace HM
{
   class DiscoverySearch
   {
   public:
      // The hits a search keeps before it stops with "truncated".
      static const int MaxHits = 100000;

      // The unsearched entries a result file lists; the count goes on.
      static const int MaxUnsearchedListed = 10000;

      // The bytes of one message a search reads, MessageData's own ceiling.
      static const int MaxMessageBytes = 80 * 1024 * 1024;

      struct Query
      {
         std::vector<String> custodians;   // lower-cased: addresses, domains, "*"
         String from;                      // YYYY-MM-DD, inclusive; empty for none
         String to;                        // YYYY-MM-DD, inclusive; empty for none
         std::vector<String> words;        // each must be found; lower-cased
         bool attachment = false;          // an attachment criterion was given
         String attachment_name;           // contains, lower-cased; empty for any
         String attachment_type;           // a media type, or a prefix ending in "/"
         __int64 attachment_min_bytes = 0; // decoded size at least this
         bool mailboxes = true;
         bool archive = true;
      };

      // The route's body, or the row's stored query, read into a query. False
      // with a sentence for anything the route should answer 400 to: no
      // custodian, a custodian that is neither an address, a domain nor "*", a
      // date that is not YYYY-MM-DD, from after to, a scope that is not
      // mailboxes, archive or both, a member this query does not have.
      static bool ParseQuery(const AnsiString &json, Query &query, String &problem);

      // The query as the row keeps it and the audit trail records it: the
      // members in one order, the words as given.
      static AnsiString QueryJson(const Query &query);

      // Starts the runner on the maintenance work queue when it is not running.
      // Every route that queues a search or an export calls it, and so does
      // every read of a case, so that what was queued when the server stopped
      // is taken up again.
      static void Kick();

      // One page of a finished search's hits, each the JSON object the result
      // file holds for it, with total the number of hits; and, paged on their
      // own, the entries it could not search, with unsearchedListed the number
      // the file lists (at most MaxUnsearchedListed; the search's row counts
      // every one). False when the file cannot be read.
      static bool ReadResults(const String &path, int offset, int limit, std::vector<AnsiString> &hits, int &total,
                              int unsearchedOffset, int unsearchedLimit, std::vector<AnsiString> &unsearched, int &unsearchedListed);

      // The runner's flags dropped with the maintenance queue that ran it:
      // Application::Reinitialize calls it once that queue has been taken away,
      // so that a runner the queue never started, or one the stop cut off, does
      // not keep every later search and export queued (the flag would otherwise
      // outlive the queue, being this file's and not the queue's).
      static void Forget();

      // The runner's body: the queue worked through, the oldest first.
      static void RunQueue();

   private:
      static bool RunSearch_(__int64 searchId);
      static bool RunExport_(__int64 exportId);
   };
}
