// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Journaling and dual delivery: a copy of a domain's mail, or of one account's,
// sent to an address - Exchange's journaling and Google's dual delivery, which
// are the same mechanism wearing two shapes.
//
// WHERE IT RUNS
//
// Once per accepted message, in SMTPConnection, after the row is saved and the
// file is settled and the same recipients the archive was decided from are
// known. Not at delivery: the deliverer runs again for every retry and for
// every route a message splits into, and a compliance archive that gained a
// second copy because a remote host was busy would be worse than useless. One
// acceptance, one evaluation, one report per matching rule.
//
// And once per message a fetch account collects, in ExternalFetchClientBase,
// at the same point in that path: after the row is saved, before the deliverer
// is told. A mailbox whose mail is collected from another provider by POP3 or
// IMAP is still that mailbox's mail, and an archive that held everything except
// what the domain happened to fetch would be an archive with a hole in it that
// nothing said was there. A download has no authenticated sender, so it is the
// inbound half of a rule that speaks for it.
//
// WHAT A RULE SAYS (Common/BO/JournalRule.h)
//
//   * whose mail: a hosted domain, or one account in it;
//   * which way: mail arriving for them (inbound), mail they send over an
//     authenticated session (outbound), or both;
//   * where the copy goes: one address, local or anywhere else;
//   * what shape it takes: a report, or a copy.
//
// THE TWO SHAPES
//
//   report - the Exchange journal shape. A multipart/mixed message whose first
//            part is the envelope report, in the fixed four-field form below,
//            and whose second part is the original message attached as
//            message/rfc822 and written through byte for byte. The envelope is
//            the point: a message's own To and Cc headers are what the sender
//            typed, and the envelope is where it actually went.
//
//                Sender: <envelope sender, or <> >
//                Subject: <the subject, decoded>
//                Message-Id: <as the message carries it>
//                Recipient: <one line per envelope recipient>
//
//   copy   - the message itself, as it was accepted, with one header line added
//            (X-hMailServer-Journal) and nothing else changed. What a migration
//            or a second mail system wants: the mail, not a report about it.
//
// Both are queued through the ordinary delivery queue, so a journal address
// somewhere else is delivered, retried and bounced exactly as any other mail is,
// and a local one lands in a mailbox.
//
// NOT LOOPING
//
// One guard, and it does not take the sender's word for anything. Every journal
// message this server writes carries X-hMailServer-Journal, and its value is the
// rule's id, a full stop, and a mark this process chose at random when it first
// needed one - 128 bits nobody outside this process has seen. The evaluation
// reads that header before anything else and stops only when the mark is this
// process's own.
//
// That distinction is the whole of it. The header's name is published in the
// documentation, so a guard that honoured the header as written would let anyone
// under an outbound compliance rule switch the archive off for a message with
// one line in their mail client, and an outsider do the same for a journalled
// domain's incoming mail. A message that carries a mark this process did not
// write is journaled exactly as it would have been without the header.
//
// The mark is per process, and nothing is stored for it anywhere. A restart
// forgets the marks it wrote, so one of this server's own reports that comes
// back afterwards is journaled once more - and the report that makes carries a
// mark this process does know, so it stops there. A loop cannot run away, and
// the price of keeping no secret on disk is one duplicate in a configuration
// that is already sending mail round in a circle.
//
// What the mark is not: a signature over the message. It travels in the messages
// it marks, because a guard that must recognise a message coming back has no
// other way of doing it, so whoever can read this server's journal mail - the
// journal address, and anything relaying to it - could quote the mark back in a
// message of their own and keep that message out of the archive. A journal
// address is the one destination this feature already trusts with every copy it
// makes, and the alternative was a published header name that everybody could
// use. Somebody worried about the relay should journal to a local mailbox or
// over an authenticated route, as they would for the copies themselves.
//
// There is no second guard. There was one - a rule whose journal address was
// itself covered by an enabled rule was skipped - and it defended against a loop
// that cannot happen: Evaluate runs from an SMTP session, from a fetch
// account's download and from the send API (POST /api/v1/send), and a journal
// message is written straight into the delivery queue, so a copy to a local
// mailbox never passes any of those points. What
// it did instead was empty the archive of the most ordinary arrangement there
// is, "journal example.com into archive@example.com": the rule matched, the copy
// was dropped with a line in the log, and every surface went on showing the rule
// as enabled. Journaling a domain into a mailbox of that domain works.
//
// WHAT IT DELIBERATELY IS NOT
//
//   * not the on-disk archive (ArchiveDir, ArchiveDomains): that keeps files on
//     this server and this sends mail elsewhere. Neither replaces the other;
//   * not an account's forwarding, which the account holder controls and which
//     can deliver INSTEAD of the mailbox. A journal copy never affects what the
//     mailbox gets;
//   * not a mail flow rule's forward or copy action, which runs per delivery
//     attempt, can be edited by the account and has conditions;
//   * not retention. Once a journal message leaves the queue it belongs to
//     whatever receives it; this server keeps no second copy and expires
//     nothing.
//
// Off unless a rule exists: with an empty table the whole of this is one
// comparison on an already-held lock, per accepted message.

#pragma once

#include <vector>

namespace HM
{
   class JournalRule;
   class Message;

   class Journaling
   {
   public:

      // The header every journal message carries, and the one the evaluation
      // reads first.
      static const char *HeaderName() { return "X-hMailServer-Journal"; }

      // That header's value for a rule: the rule's id, a full stop, and this
      // process's mark. The mark is what makes the guard a guard rather than a
      // request - see NOT LOOPING above.
      static AnsiString MarkValue(__int64 ruleId);

      // Whether a header value is one this process wrote. False for an empty
      // value, for a value carrying no mark, and for any mark but this
      // process's - including a mark another hMailServer wrote.
      static bool IsOwnMark(const AnsiString &value);

      // Every save and delete of a rule, and every delete of a domain or an
      // account that took rules with it, tells the cache to read the table again.
      static void SetNeedRefresh();

      // What a report says. Filled from the message's envelope and its header,
      // read once per accepted message.
      struct Envelope
      {
         String sender;

         // The subject twice: decoded for the report, whose part declares
         // UTF-8 and can carry it, and exactly as the message wrote it for
         // the wrapper Subject header, which is a header and must stay
         // seven-bit - a decoded subject put back into a header would be
         // raw UTF-8 where an encoded word belongs.
         String subject;
         AnsiString raw_subject;
         String message_id;
         std::vector<String> recipients;
      };

      // The envelope report's text, exactly as it goes into the first part. Its
      // own function so that JournalingTester can pin the shape without a
      // database, a message file or a delivery queue.
      static AnsiString BuildReportText(const Envelope &envelope);

      // ------------------------------------------------------- the rules, edited
      //
      // One vocabulary for every surface. The column holds a number and every
      // interface says a word, so the two conversions live here and nowhere else:
      // REST, COM, the Control Panel, the Control Deck and hmctl all speak the
      // same three small vocabularies because they all come through these.

      static String ScopeName(int scope);
      static String DirectionName(int direction);
      static String ModeName(int mode);

      // One rule as a JSON object: id, scope, target, journal_address,
      // direction, mode, enabled, note - the same fields in the same order on
      // every surface, because they are written once, here.
      static AnsiString RuleJson(std::shared_ptr<JournalRule> rule);

      // The rules as a JSON document - {"items":[...]}, each item id, scope,
      // target, journal_address, direction, mode, enabled, note - which is what
      // the Control Panel reads over COM and what the REST listing answers from.
      // readable says whether the table could be read at all.
      static AnsiString RulesDocument(bool &readable);

      // One rule, by id; an empty pointer when there is no such row.
      static std::shared_ptr<JournalRule> ReadRule(__int64 id);

      // A rule written from the words the surfaces use. id 0 creates, any other
      // id changes that row. False with the sentence when a word is not one of
      // the three vocabularies, when the target is not a domain or an account
      // this server hosts, when the journal address is not an address, when an
      // account rule would copy that account's mail back into the same mailbox,
      // or when the row could not be written; nothing is changed in any of those
      // cases.
      static bool SaveRule(__int64 id, const String &scope, const String &target,
                           const String &journalAddress, const String &direction, const String &mode,
                           bool enabled, const String &note, __int64 &savedId, String &error);

      // found says whether there was a rule with that id; the return value is
      // false only when the delete itself failed.
      static bool DeleteRule(__int64 id, bool &found);

      // ---------------------------------------------------------- the evaluation

      // The evaluation. Returns how many journal messages were queued - 0 when
      // there are no rules, when the message is one this server journaled, or
      // when nothing matched. authenticatedAddress is the address the session
      // signed in as, empty for an unauthenticated one and for a fetch account's
      // download; it is what the outbound half of a rule is judged against,
      // because "mail this domain sent" means mail somebody in it was signed in
      // to send, not mail that merely claims the domain in its envelope.
      //
      // sessionId is the SMTP session the message arrived on; 0 says there was
      // none, and then what would have gone to that session's log goes to the
      // application log instead, because a fetch has no SMTP session to write to.
      static int Evaluate(std::shared_ptr<Message> message, const String &authenticatedAddress,
                          unsigned int sessionId, const String &connectingAddress);

   private:

      static bool Refresh_();

      static bool Queue_(std::shared_ptr<JournalRule> rule, const String &originalFileName,
                         const Envelope &envelope, unsigned int sessionId, bool fromOutside = false);
   };

   class JournalingTester
   {
   public:
      void Test();
   };
}
