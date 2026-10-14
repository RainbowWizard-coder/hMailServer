// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The spam quarantine: a store for messages the server would otherwise have
// refused, so that a human can look before they are gone.
//
// Until now the two outcomes were "mark it" and "refuse it", and refusing
// happens during the SMTP conversation with a 550 - the message is never
// accepted, so there is nothing to review and nothing to release. That is a
// perfectly defensible design and it is what most of this server still does.
// What it cannot do is let an administrator find the invoice that scored 12
// because somebody's newsletter host got listed.
//
// Quarantining is therefore a DIFFERENT decision, not a tidier one, and the
// difference is worth stating plainly:
//
//   * The message is ACCEPTED. The sender is told 250 and believes it was
//     delivered, which is exactly the outcome a false positive needs - no
//     backscatter, no bounce to a forged return path - and exactly the outcome
//     a true positive does not deserve. That is the trade.
//   * The server now stores mail it considers spam, so the store is bounded by
//     a retention sweep rather than left to grow.
//   * Nothing is released automatically. A release is an administrator saying
//     "this was wrong", and it delivers straight to the mailbox rather than
//     back through the filters that quarantined it in the first place.
//
// Only POST-transmission verdicts can be quarantined, and that is not a
// limitation to be fixed later - it is arithmetic. A pre-transmission verdict
// (a blacklisted connecting IP, a bad HELO) is reached before DATA, when there
// is no message to hold; refusing at that point is also strictly cheaper for
// everyone, since the body never crosses the wire. Those stay refusals.
//
// Off by default. An installation that has been refusing spam for years must
// not silently start accepting and storing it because of an upgrade.

#pragma once

namespace HM
{
   class Message;
   class SQLCommand;

   struct QuarantinedMessage
   {
      __int64 id = 0;
      String file_name;      // relative to the quarantine root
      String sender;
      String recipients;     // comma separated, as accepted
      String subject;
      String reason;
      int score = 0;
      int size = 0;
      String created;

      // The HELO or EHLO name the client gave in the SMTP session that brought
      // the message in (schema 6105). Empty for a message held before that
      // step, and for one that did not arrive over SMTP.
      String helo;
   };

   class QuarantineStore
   {
   public:
      static bool GetEnabled();

      // Copies the message's file into the store and records it. Returns false if
      // anything failed, in which case the caller must fall back to refusing the
      // message: quietly accepting mail that was not actually stored would turn a
      // spam refusal into silent deletion, which is the one outcome nobody wants.
      //
      // The HELO name is taken from the Received header this server wrote when
      // the message arrived (HeloFromReceivedHeader): a hold made at delivery -
      // a rule, a block list, an account's own threshold, the sandbox - happens
      // long after the SMTP session has gone.
      static bool Quarantine(std::shared_ptr<Message> message, const String &reason, int score);

      // The same, for a caller who already knows where the message file is. The
      // three-argument form resolves the file the way the SMTP conversation needs
      // it resolved - without an account, which lands on the queue or public
      // folder - and that is wrong for an account-level delivery copy, whose file
      // has already moved into the recipient's own folder. Local delivery's
      // per-account delete threshold passes that path in here.
      static bool Quarantine(std::shared_ptr<Message> message, const String &reason, int score, const String &sourceFile);

      // The hold the SMTP session makes itself, which knows the HELO name the
      // client gave and records it as it was given.
      static bool QuarantineFromSession(std::shared_ptr<Message> message, const String &reason, int score, const String &heloName);

      static std::vector<QuarantinedMessage> List(int maxCount);
      static bool GetById(__int64 id, QuarantinedMessage &out_message);

      // A search of the held messages (Search): what it looks for, and which page
      // of the answer it wants.
      struct SearchRequest
      {
         // Text the sender, the recipients, the subject or the reason holds, in
         // any case. Empty finds every message.
         String text;

         // Only the messages held for this address - one of their recipients,
         // whole and in any case, as IsRecipient reads it - and the text is then
         // not looked for among the recipients, which say who else a message was
         // for. Empty for every message.
         String recipient;

         // count messages after the first offset, newest first.
         int offset = 0;
         int count = 100;
      };

      // The longest text a search takes, the most messages one page holds and the
      // furthest page it goes to. Every column the text is looked for in is cut at
      // 255 characters when the row is written but the recipients', at 1,000.
      static constexpr int MaxSearchText = 255;
      static constexpr int MaxSearchPage = 1000;
      static constexpr int MaxSearchOffset = 999999999;

      // The held messages a search finds, newest first: the page asked for in
      // out_page, and how many match in all in out_matches. Each column is
      // matched as the row holds it - the envelope sender, the recipients as
      // they were accepted, the subject and the reason - and the text's own %, _
      // and [ are matched as they are, on every database. False, with nothing
      // found, when the request is outside the bounds above or the database could
      // not be read.
      static bool Search(const SearchRequest &request, std::vector<QuarantinedMessage> &out_page, int &out_matches);

      // The two statements a search sends - the count of what matches and the
      // page - for a database that writes a row limit as TOP (SQL Server and
      // Compact) or as LIMIT (MySQL and PostgreSQL). Public for
      // QuarantineSearchTester, which reads them without a database.
      static void BuildSearchCommands(const SearchRequest &request, bool topSyntax, SQLCommand &out_count, SQLCommand &out_page);

      // The statement List sends: every held message, newest first, with the id
      // breaking a tie as it does in the search's pages, so that Search("", 500,
      // 500) is the page after the one Refresh loads. Public for
      // QuarantineSearchTester, which holds the order without a database: the
      // bench's SQL Server Compact returns messages held in one second newest id
      // first with or without the id, and the Linux suite cannot seed rows held in
      // one second, so no test that reads rows shows it in either gate.
      static void BuildListCommand(SQLCommand &out_command);

      // What a reviewer may read of a held message without releasing it: the
      // header block as it is stored, and a plain-text preview - the text part,
      // decoded, or failing that the HTML part with its markup taken out. The
      // HTML is never rendered anywhere. Each is cut at its limit, and
      // out_truncated says so. False, with the reason, when there is no such
      // entry or its file cannot be read.
      static constexpr int MaxHeaderCharacters = 65536;
      static constexpr int MaxPreviewCharacters = 16384;
      static bool ReadHeaders(__int64 id, String &out_headers, bool &out_truncated, String &out_error);
      static bool ReadPreview(__int64 id, String &out_text, bool &out_truncated, bool &out_fromHtml, String &out_error);

      // The HELO name in the Received header this server writes for a message it
      // receives over SMTP ("from <helo> (<ptr> [<address>]) by <this computer>
      // ..."), when localComputerName is the host the header says received it.
      // Empty for somebody else's Received header, and for one written for a
      // client that gave no HELO, whose place the server fills with the address.
      static String HeloFromReceivedHeader(const String &receivedValue, const String &localComputerName);

      // The HTML part's text, for the preview: markup, style sheets and scripts
      // taken out and the entities a reader would see unwrapped.
      static String TextOfHtml(const String &html);

      // Delivers a quarantined message to its original recipients and removes it
      // from the store. Delivery is direct rather than back through the filters:
      // a release is an administrator overruling those filters, and re-running them
      // would simply quarantine it again.
      static bool Release(__int64 id, String &out_error);

      // True when the address is one of the message's recorded recipients,
      // whole and case-insensitively.
      static bool IsRecipient(const QuarantinedMessage &message, const String &address);

      // Release for one recipient only: delivered to that address, which then
      // leaves the entry; the entry stays for the other recipients and goes
      // when it was the last.
      static bool ReleaseTo(__int64 id, const String &recipient, String &out_error);

      // One recipient gives up its copy: the address leaves the entry, which
      // goes when it was the last. Nothing is delivered.
      static bool DiscardFor(__int64 id, const String &recipient);

      static bool Delete(__int64 id);

      // Removes everything older than QuarantineRetentionDays. Returns the number
      // deleted. Zero days means never, which is a real answer here: an
      // administrator who wants to keep the evidence should not have it swept.
      static int DeleteExpired();

      static int GetCount();

      // The directory holding quarantined message files. Created on demand.
      static String GetQuarantineDirectory();

      // Where a held message came from, as the quarantine keeps it: whether it was
      // mail from outside, which DKIMSigner does not sign as the domain its From names
      // (Message::GetFromOutside, DKIMSigner::SignsAsAuthorDomain). The row has no
      // column for it, so the name of the stored copy carries it - the GUID, then
      // ".outside.eml" or ".inside.eml" (StoredName) - and a release reads it back
      // from there (OriginOfStoredName), never from the message, whose header a
      // sender, a rule or a Sieve script may have written. A copy held before 7
      // October 2026 has a name without it, OriginUnknown, and its header is read
      // instead (HeaderSaysFromOutside).
      enum StoredOrigin
      {
         OriginUnknown,
         OriginOutside,
         OriginInside
      };

      // And what else the queued message records of where it came from, which a
      // release puts back on the message it queues, so that a copy of it relayed on
      // is sealed as one never held would be: the SPF result this server's own check
      // reached (Message::GetReceivedSpfResult), which the ARC set attests; whether the
      // domain that check evaluated (Message::GetReceivedSpfDomain), which the set
      // names beside it, is the domain of the held message's envelope sender, which
      // the row keeps - a domain is too long for a file name, and is that sender's for
      // every message held as it arrived; and the hosted domain that sends it on
      // (DKIMSigner::RelayingDomainID), whose key seals that set - an alias's, whose
      // expansion a release does not repeat; and, for mail from outside, whether it
      // passed DMARC when it arrived for the domain of the From the stored copy carries
      // (Message::GetAuthDomainsOnArrival), which a release gives the message it queues
      // as what DMARC passed for (Message::SetCopiedAuthDomains), for the envelope a
      // forward of it is sent on with - the domain read from the stored copy again,
      // which nothing changes once it is held. Each is a word of the name between the
      // GUID and the end - ".spf-pass", ".mailfrom", ".dmarc", ".relay-12" - written
      // only when there is something to say, and none holds a dot. A copy held before
      // 7 October 2026 has none, and is released as it always was, with no SPF result
      // and no domain recorded to seal it; one held before ".dmarc" was written is
      // released with no DMARC pass recorded.
      struct StoredMarks
      {
         // Whether the name says where the message came from at all: false for a
         // copy held before it did, whose other marks are then empty too.
         bool known = false;
         bool from_outside = false;
         String spf_result;
         bool spf_domain_is_senders = false;
         bool dmarc_passed_for_from = false;
         __int64 relaying_domain_id = 0;
      };

      static String StoredName(const String &guid, const StoredMarks &marks);
      static StoredMarks MarksOfStoredName(const String &relativePath);
      static StoredOrigin OriginOfStoredName(const String &relativePath);

      // The domain of the From a message's header names, as the DMARC check reads it -
      // the address in the first From field, decoded (DMARC::ExtractAddressFromHeaderValue)
      // - lower-cased, or empty.
      static String FromDomainAsDmarcReadsIt(const AnsiString &header);

      // Whether authDomains (Message::GetAuthDomainsOnArrival: words between commas or
      // blanks) records a DMARC pass for domain itself, "dmarc=" and that domain. False
      // for an empty domain.
      static bool RecordsDmarcPassFor(const String &authDomains, const String &domain);

      // For a copy held before its name said where it came from: from outside unless
      // its trace fields show this server made it or took it from a signed-in client -
      // no Received field at all (a reply, a notice, a message made over COM or the
      // webmail), this server's own SMTP Received field for a session that signed in,
      // or the one the send API writes. A Received field of this server's for a session
      // that did not sign in is from outside, and so is one another server wrote on
      // top - a download from another mailbox, a message imported from a file - which
      // until 7 October 2026 was released as not from outside and signed as its From's
      // domain. No configuration, database or file: the header and the name this
      // server writes in its Received fields are given.
      static bool HeaderSaysFromOutside(const AnsiString &header, const String &thisServerName);

   private:
      static bool Quarantine_(std::shared_ptr<Message> message, const String &reason, int score, const String &sourceFile,
                              bool heloKnown, const String &heloName);
      static String HeloOfStoredCopy_(const String &file);

      // Whether a released copy is mail from outside (Message::GetFromOutside): what
      // the stored copy's name says (OriginOfStoredName), or for a copy held before it
      // said so, what its header says (HeaderSaysFromOutside).
      static bool ReleasedCopyIsFromOutside_(const String &relativePath, const String &file);

      // Puts back on a released copy's message what the stored copy's name kept of
      // where it came from beside the origin (StoredMarks): the SPF result, the domain
      // it was reached for when that is the sender's, the hosted domain that sends it
      // on, and the DMARC pass for its From's domain, read from file, the released copy.
      static void ApplyStoredMarks_(const QuarantinedMessage &quarantined, Message &message, const String &file);
      static bool OpenFile_(__int64 id, String &out_file, String &out_error);

      // A new copy's path under the quarantine directory: the day's directory and the
      // stored name, which says where the message came from (StoredName).
      static String BuildRelativePath_(const StoredMarks &marks);
      static bool ReadRow_(std::shared_ptr<DALRecordset> recordset, QuarantinedMessage &out_message);
      static bool RemoveRecipient_(__int64 id, const QuarantinedMessage &quarantined, const String &recipient);
   };

   // The statements a search of the quarantine sends (QuarantineStore::
   // BuildSearchCommands), read without a database: every parameter named once,
   // in the order the statement names them and none the start of another, which
   // SQL Server's binding by position needs (ADOConnection::
   // InitializeCommandParameters); the text's own wildcards escaped and both
   // sides lowered, by the database where the text is written into the
   // statement (MySQL and PostgreSQL), so that it lowers a capital outside ASCII
   // as it lowers the column's; the recipients looked in only where no
   // recipient is given;
   // a recipient found whole; the page newest first with a tie broken by the
   // id, and as long as the offset and the page together; and the list's
   // statement (BuildListCommand) in the pages' order. ClassTester runs it on
   // Windows, quarantine-search-tests on Linux.
   class QuarantineSearchTester
   {
   public:
      void Test();
   };
}
