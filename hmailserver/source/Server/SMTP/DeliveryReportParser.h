// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Reading an RFC 3464 delivery status notification that arrives for one of this
// installation's senders: which recipients it reports, what it says happened to
// each (Action, Status, Diagnostic-Code, Remote-MTA), and the Message-ID of the
// message it reports on, which is what lets BounceTracker ask whether this server
// really sent that message before believing a word of it.
//
// Pure over bytes. It is handed hostile input by construction - anybody can send
// a message that looks like a report - so it has a size cap, reads one level of
// MIME and no deeper, refuses a Final-Recipient that is not one address, and
// never throws on malformed input: a report it cannot read is simply not a report.

#pragma once

namespace HM
{
   struct DeliveryReportRecipient
   {
      String final_recipient;     // the address, lower-cased, without the type
      String action;              // failed, delayed, delivered, relayed, expanded - lower-cased
      String status;              // class.subject.detail, or empty when absent or malformed
      String diagnostic;          // the Diagnostic-Code text after its type
      String remote_mta;          // the Remote-MTA name after its type
   };

   struct DeliveryReport
   {
      String reporting_mta;
      String original_message_id; // normalised - see NormaliseMessageId
      std::vector<DeliveryReportRecipient> recipients;
   };

   class DeliveryReportParser
   {
   public:
      // A report larger than this is refused unread. Reports are a few kilobytes;
      // the returned original message is what can make one large, and only its
      // headers are ever looked at.
      static const size_t MaxReportBytes = 2 * 1024 * 1024;

      // True when raw is a multipart/report; report-type=delivery-status holding a
      // message/delivery-status or message/global-delivery-status part with at
      // least one usable per-recipient group.
      static bool Parse(const AnsiString &raw, DeliveryReport &report);

      // Parse on a message file, refusing a file over MaxReportBytes without
      // reading it.
      static bool ParseFile(const String &fileName, DeliveryReport &report);

      // "<Abc@Host>" -> "abc@host": the first bracketed id, trimmed and lower-cased,
      // or the whole trimmed value when it carries no brackets. The one spelling
      // used on both sides of a match - when a message is sent and when a report
      // about it arrives.
      static String NormaliseMessageId(const String &value);

      // The delivery-status body: groups of fields separated by blank lines, each
      // field unfolded. Exposed for the tester.
      typedef std::vector<std::pair<String, String> > FieldGroup;
      static void ReadFieldGroups(const AnsiString &text, std::vector<FieldGroup> &groups);

   private:
      static bool ReadRecipientGroup_(const FieldGroup &group, DeliveryReportRecipient &recipient);
      static String FieldValue_(const FieldGroup &group, const wchar_t *name, bool &present);
   };

   class DeliveryReportParserTester
   {
   public:
      void Test();
   };
}
