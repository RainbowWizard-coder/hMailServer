// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Reading an RFC 5965 abuse report - the message a large provider sends when one
// of its readers marks a message from this installation as spam. The shape is the
// delivery report's: a multipart/report, its report-type feedback-report rather
// than delivery-status, holding a message/feedback-report part of fields and,
// beside it, the message that was complained about (or its headers alone, which
// is what most providers send).
//
// What is read: the fields the row asks for - Feedback-Type, User-Agent,
// Original-Mail-From, Original-Rcpt-To, Arrival-Date, Source-IP and
// Reported-Domain - and, from the returned message, its Message-ID, From, To and
// Subject. Nothing else: Reported-URI and the rest are left where they are.
//
// Pure over bytes, as DeliveryReportParser is, and for the same reason: it is
// handed hostile input by construction, since anybody who can post to the
// complaint mailbox can put anything in it. It has a size cap, reads one level of
// MIME and no deeper, refuses an address that is not one address, and never
// throws on malformed input - a report it cannot read is simply not a report, and
// the message is delivered unchanged either way.

#pragma once

namespace HM
{
   struct FeedbackReport
   {
      // message/feedback-report, RFC 5965 3.1 and 3.2.
      String feedback_type;         // abuse, fraud, virus, not-spam, other - lower-cased
      String user_agent;            // the provider's generator, verbatim
      String original_mail_from;    // the envelope sender of the message complained about
      String original_rcpt_to;      // the reader who complained, as the report gives it
      String arrival_date;          // YYYY-MM-DD HH:MM:SS local, or empty when unreadable
      String source_ip;             // the address the provider says sent it
      String reported_domain;       // lower-cased

      // The message the report wraps, read from its header block alone.
      String original_message_id;   // normalised as DeliveryReportParser normalises one
      String original_from;         // the From address, lower-cased
      String original_to;           // the first To address, lower-cased
      String original_subject;      // as given, at most 255 characters
   };

   class FeedbackReportParser
   {
   public:
      // A report larger than this is refused unread. A feedback report is a few
      // kilobytes; the returned message is what can make one large, and only its
      // headers are ever looked at.
      static const size_t MaxReportBytes = 2 * 1024 * 1024;

      // True when raw is a multipart/report; report-type=feedback-report holding a
      // message/feedback-report part with a Feedback-Type. Everything else in the
      // report is optional, because providers differ in what they redact: a report
      // that names no reader still says that somebody complained about this
      // domain's mail, which is the number that matters.
      static bool Parse(const AnsiString &raw, FeedbackReport &report);

      // Parse on a message file, refusing a file over MaxReportBytes without
      // reading it.
      static bool ParseFile(const String &fileName, FeedbackReport &report);

      // One address out of a field that may carry a type, angle brackets, a display
      // name or a comment: "rfc822; <A@B> (a reader)" -> "a@b". Empty when the value
      // holds no single valid address, which is what a redacted one gives.
      static String Address(const String &value);

      // An RFC 5322 date as YYYY-MM-DD HH:MM:SS - the one spelling every other time
      // this feature stores is written in - read the way Time::GetDateTimeFromMimeHeader
      // reads a Date field, which applies the zone the value names and does not
      // convert to this server's. Empty when the value cannot be read as a date.
      static String NormaliseDate(const String &value);

      // The fields of one header block, unfolded and lower-cased by name. Exposed
      // for the tester.
      typedef std::vector<std::pair<String, String> > Fields;
      static void ReadFields(const AnsiString &text, Fields &fields);

   private:
      static String FieldValue_(const Fields &fields, const wchar_t *name);
   };

   class FeedbackReportParserTester
   {
   public:
      void Test();
   };
}
