// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "DeliveryReportParser.h"

#include "../Common/Mime/Mime.h"
#include "../Common/Util/ByteBuffer.h"
#include "../Common/Util/File.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/Util/MessageOrigin.h"
#include "../Common/Util/Unicode.h"

#include <chrono>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      String FromUtf8(const AnsiString &value)
      {
         String wide;
         if (!Unicode::MultiByteToWide(value, wide))
            return String();

         return wide;
      }

      String Lower(const String &value)
      {
         String result = value;
         result.ToLower();
         return result;
      }

      // "rfc822; someone@example.test" -> type "rfc822", value "someone@example.test".
      void SplitTyped(const String &field, String &type, String &value)
      {
         int semicolon = field.Find(_T(";"));

         if (semicolon < 0)
         {
            type = String();
            value = field;
         }
         else
         {
            type = field.Mid(0, semicolon);
            value = field.Mid(semicolon + 1);
         }

         type.Trim();
         type.ToLower();
         value.Trim();
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      AnsiString Crlf(const char *text)
      {
         // The fixtures below are written with bare LF for legibility; a message
         // file always has CRLF, which is what MimeBody's boundary search expects.
         AnsiString result;
         for (const char *p = text; *p; p++)
         {
            if (*p == '\n')
               result += "\r\n";
            else
               result += *p;
         }
         return result;
      }
   }

   String
   DeliveryReportParser::NormaliseMessageId(const String &value)
   {
      String trimmed = value;
      trimmed.Trim();

      int open = trimmed.Find(_T("<"));
      if (open >= 0)
      {
         int close = trimmed.Find(_T(">"), open + 1);
         if (close > open)
            trimmed = trimmed.Mid(open + 1, close - open - 1);
         else
            trimmed = trimmed.Mid(open + 1);
      }
      else
      {
         // No brackets: the first word, so a trailing comment is not part of it.
         for (int i = 0; i < trimmed.GetLength(); i++)
         {
            wchar_t c = trimmed.GetAt(i);
            if (c == ' ' || c == '\t' || c == '(')
            {
               trimmed = trimmed.Mid(0, i);
               break;
            }
         }
      }

      trimmed.Trim();
      trimmed.ToLower();

      // Anything longer is not a Message-ID a real MTA produces, and the key is a
      // hash of it anyway.
      return trimmed.Left(998);
   }

   int
   DeliveryReportParser::LeadingBlankLines(const AnsiString &text)
   {
      int at = 0;
      while (at + 1 < text.GetLength() && text.GetAt(at) == '\r' && text.GetAt(at + 1) == '\n')
         at += 2;

      return at;
   }

   void
   DeliveryReportParser::ReadFieldGroups(const AnsiString &text, std::vector<FieldGroup> &groups)
   {
      groups.clear();

      FieldGroup current;
      String pendingName;
      AnsiString pendingValue;
      bool havePending = false;

      auto flushField = [&]()
      {
         if (havePending)
         {
            pendingValue.Trim();
            current.push_back(std::make_pair(pendingName, FromUtf8(pendingValue)));
         }

         havePending = false;
         pendingName = String();
         pendingValue = AnsiString();
      };

      auto flushGroup = [&]()
      {
         flushField();

         if (!current.empty())
            groups.push_back(current);

         current.clear();
      };

      int position = 0;
      const int length = text.GetLength();

      while (position <= length)
      {
         int lineEnd = text.Find("\n", position);
         if (lineEnd < 0)
            lineEnd = length;

         AnsiString line = text.Mid(position, lineEnd - position);
         if (!line.IsEmpty() && line.GetAt(line.GetLength() - 1) == '\r')
            line = line.Mid(0, line.GetLength() - 1);

         position = lineEnd + 1;

         AnsiString trimmed = line;
         trimmed.Trim();

         if (trimmed.IsEmpty())
         {
            flushGroup();

            if (lineEnd >= length)
               break;

            continue;
         }

         if (line.GetAt(0) == ' ' || line.GetAt(0) == '\t')
         {
            // A folded continuation of the field above it (RFC 5322 2.2.3).
            if (havePending)
               pendingValue += " " + trimmed;

            if (lineEnd >= length)
               break;

            continue;
         }

         flushField();

         int colon = line.Find(":");
         if (colon > 0)
         {
            AnsiString name = line.Mid(0, colon);
            name.Trim();
            name.ToLower();

            pendingName = String(name);
            pendingValue = line.Mid(colon + 1);
            havePending = true;
         }

         if (lineEnd >= length)
            break;
      }

      flushGroup();
   }

   String
   DeliveryReportParser::FieldValue_(const FieldGroup &group, const wchar_t *name, bool &present)
   {
      present = false;

      for (const auto &field : group)
      {
         if (field.first == name)
         {
            present = true;
            return field.second;
         }
      }

      return String();
   }

   bool
   DeliveryReportParser::ReadRecipientGroup_(const FieldGroup &group, DeliveryReportRecipient &recipient)
   {
      bool present = false;

      String finalRecipient = FieldValue_(group, L"final-recipient", present);
      if (!present)
         return false;

      String type;
      String address;
      SplitTyped(finalRecipient, type, address);

      // RFC 3464 2.3.2 with RFC 6533 3.2: an Internet address, ASCII or UTF-8.
      // Any other address type names something this server never sent to.
      if (type != _T("rfc822") && type != _T("utf-8"))
         return false;

      if (address.GetLength() >= 2 && address.GetAt(0) == '<' && address.GetAt(address.GetLength() - 1) == '>')
         address = address.Mid(1, address.GetLength() - 2);

      // One address, and nothing that could end a line or a field somewhere this
      // value is written back out.
      for (int i = 0; i < address.GetLength(); i++)
      {
         wchar_t c = address.GetAt(i);
         if (c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == ',' || c == 0)
            return false;
      }

      if (!StringParser::IsValidEmailAddress(address, true))
         return false;

      recipient.final_recipient = Lower(address).Left(255);

      String action = FieldValue_(group, L"action", present);
      action.Trim();
      int actionEnd = action.GetLength();
      for (int i = 0; i < action.GetLength(); i++)
      {
         wchar_t c = action.GetAt(i);
         if (c == ' ' || c == '\t' || c == '(')
         {
            actionEnd = i;
            break;
         }
      }
      recipient.action = Lower(action.Mid(0, actionEnd)).Left(32);

      // "5.1.1 (bad destination mailbox)" is legal: the code, and a comment.
      String status = FieldValue_(group, L"status", present);
      status.Trim();
      int statusEnd = status.GetLength();
      for (int i = 0; i < status.GetLength(); i++)
      {
         wchar_t c = status.GetAt(i);
         if (c == ' ' || c == '\t' || c == '(')
         {
            statusEnd = i;
            break;
         }
      }
      status = status.Mid(0, statusEnd);
      recipient.status = (status.GetLength() <= 11) ? status : String();

      String diagnosticType;
      String diagnostic;
      SplitTyped(FieldValue_(group, L"diagnostic-code", present), diagnosticType, diagnostic);
      recipient.diagnostic = diagnostic.Left(1000);
      recipient.diagnostic_type = diagnosticType.Left(32);

      String mtaType;
      String remoteMta;
      SplitTyped(FieldValue_(group, L"remote-mta", present), mtaType, remoteMta);
      recipient.remote_mta = remoteMta.Left(255);

      return true;
   }

   bool
   DeliveryReportParser::Parse(const AnsiString &raw, DeliveryReport &report)
   {
      report = DeliveryReport();

      if (raw.IsEmpty() || (size_t) raw.GetLength() > MaxReportBytes)
         return false;

      MimeBody message;
      size_t index = 0;
      bool partLoaded = false;

      if (message.Load(raw.c_str(), raw.GetLength(), index, partLoaded) == 0)
         return false;

      // RFC 3462: the report is the message itself, not an attachment somewhere
      // inside one. Anything else - a report forwarded as an attachment, a
      // multipart/mixed that happens to hold one - is mail about a report and is
      // left alone.
      if (Lower(message.GetCleanContentType()) != _T("multipart/report"))
         return false;

      String reportType = Lower(String(message.GetParameter("Content-Type", "report-type").c_str()));
      reportType.Trim();
      if (reportType != _T("delivery-status"))
         return false;

      AnsiString statusText;
      bool haveStatus = false;

      for (std::shared_ptr<MimeBody> part = message.FindFirstPart(); part; part = message.FindNextPart())
      {
         const String contentType = Lower(part->GetCleanContentType());

         if (!haveStatus && (contentType == _T("message/delivery-status") || contentType == _T("message/global-delivery-status")))
         {
            if (part->GetDecodedContent(statusText))
               haveStatus = true;

            continue;
         }

         // The original message, or its headers. Only the Message-ID is taken,
         // and from the header block alone: FirstFieldValue stops at the blank
         // line, so a body line that looks like a field is never read.
         if (report.original_message_id.IsEmpty() &&
             (contentType == _T("text/rfc822-headers") || contentType == _T("message/rfc822") ||
              contentType == _T("message/global-headers") || contentType == _T("message/global")))
         {
            AnsiString content;
            if (!part->GetDecodedContent(content))
               continue;

            // A headers part may begin with the blank line that ended its own
            // MIME header, depending on how the sender folded it. The blank lines
            // are counted and cut off once: cut a pair at a time, each cut copied
            // the rest of the part, and a part of the size cap that is all blank
            // lines - a sender's to choose - took a time that grew with the
            // square of their number.
            content = content.Mid(LeadingBlankLines(content));

            AnsiString messageId = MessageOrigin::FirstFieldValue(content, "Message-ID");
            if (!messageId.IsEmpty())
               report.original_message_id = NormaliseMessageId(FromUtf8(messageId));
         }
      }

      if (!haveStatus)
         return false;

      std::vector<FieldGroup> groups;
      ReadFieldGroups(statusText, groups);

      for (const FieldGroup &group : groups)
      {
         bool present = false;

         String reportingMta = FieldValue_(group, L"reporting-mta", present);
         if (present && report.reporting_mta.IsEmpty())
         {
            String type;
            SplitTyped(reportingMta, type, report.reporting_mta);
            report.reporting_mta = report.reporting_mta.Left(255);
         }

         DeliveryReportRecipient recipient;
         if (ReadRecipientGroup_(group, recipient))
            report.recipients.push_back(recipient);

         // A bound keeps the work per report fixed, whatever a sender puts in it.
         if (report.recipients.size() >= MaxRecipientGroups)
            break;
      }

      return !report.recipients.empty();
   }

   bool
   DeliveryReportParser::ParseFile(const String &fileName, DeliveryReport &report)
   {
      report = DeliveryReport();

      unsigned __int64 size = 0;
      if (!FileUtilities::FileSize64(fileName, size))
         return false;

      if (size == 0 || size > MaxReportBytes)
         return false;

      File file;
      if (!file.Open(fileName, File::OTReadOnly))
         return false;

      std::shared_ptr<ByteBuffer> buffer = file.ReadFile();
      file.Close();

      if (!buffer || buffer->GetSize() == 0)
         return false;

      AnsiString raw;
      raw.append(buffer->GetCharBuffer(), buffer->GetSize());

      return Parse(raw, report);
   }

   void
   DeliveryReportParserTester::Test()
   {
      typedef DeliveryReportParser P;

      Expect(P::NormaliseMessageId(_T(" <Abc.123@Mail.Example.TEST> ")) == _T("abc.123@mail.example.test"), "A Message-ID was not normalised.");
      Expect(P::NormaliseMessageId(_T("abc@example.test (comment)")) == _T("abc@example.test"), "A Message-ID without brackets kept its comment.");

      // Postfix: text/rfc822-headers, a per-message group and one recipient.
      AnsiString postfix = Crlf(
         "From: MAILER-DAEMON@mx.remote.test (Mail Delivery System)\n"
         "Subject: Undelivered Mail Returned to Sender\n"
         "MIME-Version: 1.0\n"
         "Content-Type: multipart/report; report-type=delivery-status;\n"
         "\tboundary=\"4F1A2B.1726000000/mx.remote.test\"\n"
         "\n"
         "This is a MIME-encapsulated message.\n"
         "\n"
         "--4F1A2B.1726000000/mx.remote.test\n"
         "Content-Description: Notification\n"
         "Content-Type: text/plain; charset=us-ascii\n"
         "\n"
         "I'm sorry to have to inform you that your message could not be delivered.\n"
         "\n"
         "--4F1A2B.1726000000/mx.remote.test\n"
         "Content-Description: Delivery report\n"
         "Content-Type: message/delivery-status\n"
         "\n"
         "Reporting-MTA: dns; mx.remote.test\n"
         "X-Postfix-Queue-ID: 4F1A2B\n"
         "Arrival-Date: Tue, 15 Sep 2026 10:00:00 +0000 (UTC)\n"
         "\n"
         "Final-Recipient: rfc822; Gone@Remote.test\n"
         "Original-Recipient: rfc822;gone@remote.test\n"
         "Action: failed\n"
         "Status: 5.1.1\n"
         "Remote-MTA: dns; inbound.remote.test\n"
         "Diagnostic-Code: smtp; 550 5.1.1 <gone@remote.test>: Recipient address\n"
         "    rejected: User unknown in local recipient table\n"
         "\n"
         "--4F1A2B.1726000000/mx.remote.test\n"
         "Content-Description: Undelivered Message Headers\n"
         "Content-Type: text/rfc822-headers\n"
         "\n"
         "Received: from sender.example.test\n"
         "Message-ID: <Original.1@Sender.Example.test>\n"
         "From: someone@sender.example.test\n"
         "\n"
         "--4F1A2B.1726000000/mx.remote.test--\n");

      DeliveryReport report;
      Expect(P::Parse(postfix, report), "A Postfix report was not parsed.");
      Expect(report.recipients.size() == 1, "A Postfix report did not yield exactly one recipient.");
      Expect(report.recipients[0].final_recipient == _T("gone@remote.test"), "Final-Recipient was not read and lower-cased.");
      Expect(report.recipients[0].action == _T("failed"), "Action was not read.");
      Expect(report.recipients[0].status == _T("5.1.1"), "Status was not read.");
      Expect(report.recipients[0].remote_mta == _T("inbound.remote.test"), "Remote-MTA was not read.");
      Expect(report.recipients[0].diagnostic.Find(_T("User unknown in local recipient table")) > 0, "A folded Diagnostic-Code was not unfolded.");
      Expect(report.recipients[0].diagnostic_type == _T("smtp"), "The Diagnostic-Code's type was not kept.");
      Expect(report.original_message_id == _T("original.1@sender.example.test"), "The original Message-ID was not read from text/rfc822-headers.");
      Expect(report.reporting_mta == _T("mx.remote.test"), "Reporting-MTA was not read.");

      // The same report with its headers part begun by 400,000 blank lines, inside
      // the size cap: read in one pass. They were cut off a pair at a time, each
      // cut copying the rest of the part.
      Expect(P::LeadingBlankLines("") == 0 && P::LeadingBlankLines("\r") == 0 && P::LeadingBlankLines("\r\n\r") == 2 &&
             P::LeadingBlankLines("\r\n\r\nX") == 4 && P::LeadingBlankLines("\n\r\n") == 0,
             "The leading blank lines were not counted as whole CRLF pairs.");
      {
         const AnsiString marker = "Content-Type: text/rfc822-headers\r\n\r\n";
         const int at = postfix.Find(marker.c_str());
         Expect(at > 0, "The Postfix report lost its headers part.");

         AnsiString padded = postfix.Mid(0, at + marker.GetLength());
         padded.reserve(postfix.GetLength() + 800000);
         for (int i = 0; i < 400000; i++)
            padded += "\r\n";
         padded += postfix.Mid(at + marker.GetLength());

         const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         DeliveryReport paddedReport;
         const bool parsed = P::Parse(padded, paddedReport);
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         Expect(parsed && paddedReport.original_message_id == _T("original.1@sender.example.test"),
            "The original Message-ID was not read after 400,000 blank lines.");

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("A report whose headers part begins with 400,000 blank lines took " + std::to_string((long long) milliseconds) + " ms to read");
      }

      // Exchange Online: message/rfc822, two recipients, one of them delayed.
      AnsiString exchange = Crlf(
         "Content-Type: multipart/report; report-type=\"delivery-status\"; boundary=\"b1\"\n"
         "\n"
         "--b1\n"
         "Content-Type: text/plain\n"
         "\n"
         "Delivery has failed to these recipients or groups.\n"
         "\n"
         "--b1\n"
         "Content-Type: message/delivery-status\n"
         "\n"
         "Reporting-MTA: dns;AM0PR01MB0000.eurprd01.prod.outlook.test\n"
         "\n"
         "Final-recipient: RFC822; nobody@tenant.test\n"
         "Action: failed\n"
         "Status: 5.1.10\n"
         "Diagnostic-code: smtp;550 5.1.10 RESOLVER.ADR.RecipientNotFound; Recipient not found by SMTP address lookup\n"
         "\n"
         "Final-recipient: RFC822; slow@tenant.test\n"
         "Action: delayed\n"
         "Status: 4.4.7\n"
         "\n"
         "--b1\n"
         "Content-Type: message/rfc822\n"
         "\n"
         "Message-ID: <x2@example.test>\n"
         "Subject: hello\n"
         "\n"
         "Message-ID: <not-this-one@example.test>\n"
         "--b1--\n");

      Expect(P::Parse(exchange, report), "An Exchange-style report was not parsed.");
      Expect(report.recipients.size() == 2, "Two recipient groups did not yield two recipients.");
      Expect(report.recipients[0].status == _T("5.1.10"), "A five-character status was not read.");
      Expect(report.recipients[1].action == _T("delayed"), "A delayed group was not read as delayed.");
      Expect(report.original_message_id == _T("x2@example.test"), "The Message-ID was read from below the header block of message/rfc822.");

      // RFC 6533: message/global-delivery-status with a UTF-8 Final-Recipient.
      AnsiString global = Crlf(
         "Content-Type: multipart/report; report-type=delivery-status; boundary=\"g\"\n"
         "\n"
         "--g\n"
         "Content-Type: message/global-delivery-status\n"
         "Content-Transfer-Encoding: 8bit\n"
         "\n"
         "Reporting-MTA: dns; mx.remote.test\n"
         "\n"
         "Final-Recipient: utf-8; \xC3\xB8le@remote.test\n"
         "Action: failed\n"
         "Status: 5.1.1\n"
         "\n"
         "--g\n"
         "Content-Type: message/global-headers\n"
         "\n"
         "Message-ID: <g1@example.test>\n"
         "\n"
         "--g--\n");

      Expect(P::Parse(global, report), "A global delivery status report was not parsed.");
      Expect(report.recipients.size() == 1 && report.recipients[0].final_recipient.GetLength() == 15, "A UTF-8 Final-Recipient was not decoded.");
      Expect(report.original_message_id == _T("g1@example.test"), "The Message-ID was not read from message/global-headers.");

      // Not a report: a multipart/mixed that carries one as an attachment.
      AnsiString mixed = Crlf(
         "Content-Type: multipart/mixed; boundary=\"m\"\n"
         "\n"
         "--m\n"
         "Content-Type: message/delivery-status\n"
         "\n"
         "Reporting-MTA: dns; x.test\n"
         "\n"
         "Final-Recipient: rfc822; gone@remote.test\n"
         "Action: failed\n"
         "Status: 5.1.1\n"
         "\n"
         "--m--\n");

      Expect(!P::Parse(mixed, report), "A multipart/mixed was read as a delivery report.");

      // A report whose original headers are missing still parses, with no id.
      AnsiString noHeaders = Crlf(
         "Content-Type: multipart/report; report-type=delivery-status; boundary=\"n\"\n"
         "\n"
         "--n\n"
         "Content-Type: message/delivery-status\n"
         "\n"
         "Reporting-MTA: dns; x.test\n"
         "\n"
         "Final-Recipient: rfc822; gone@remote.test\n"
         "Action: failed\n"
         "Status: 5.1.1\n"
         "\n"
         "--n--\n");

      Expect(P::Parse(noHeaders, report), "A report without its original headers was not parsed.");
      Expect(report.original_message_id.IsEmpty(), "A report without its original headers produced a Message-ID.");

      // A Final-Recipient carrying a second address, or a line break, is refused.
      AnsiString injected = Crlf(
         "Content-Type: multipart/report; report-type=delivery-status; boundary=\"i\"\n"
         "\n"
         "--i\n"
         "Content-Type: message/delivery-status\n"
         "\n"
         "Reporting-MTA: dns; x.test\n"
         "\n"
         "Final-Recipient: rfc822; gone@remote.test\rBcc: victim@elsewhere.test\n"
         "Action: failed\n"
         "Status: 5.1.1\n"
         "\n"
         "Final-Recipient: rfc822; one@remote.test, two@remote.test\n"
         "Action: failed\n"
         "Status: 5.1.1\n"
         "\n"
         "--i--\n");

      // Whatever the transfer decoder makes of the bare CR, no address but the
      // one Final-Recipient names may come out of it, and the comma list none.
      P::Parse(injected, report);
      for (const DeliveryReportRecipient &recipient : report.recipients)
      {
         Expect(recipient.final_recipient == _T("gone@remote.test"),
            "A Final-Recipient holding a line break or two addresses produced a recipient.");
      }

      // Over the cap: refused without being parsed.
      AnsiString huge = postfix;
      while ((size_t) huge.GetLength() <= P::MaxReportBytes)
         huge.append(64 * 1024, 'x');

      Expect(!P::Parse(huge, report), "A report over the size cap was parsed.");

      std::vector<P::FieldGroup> groups;
      P::ReadFieldGroups(Crlf("A: 1\n  continued\nB: 2\n\n\nC: 3\n"), groups);
      Expect(groups.size() == 2, "Field groups were not split on blank lines.");
      Expect(groups[0].size() == 2 && groups[0][0].second == _T("1 continued"), "A folded field was not unfolded.");
   }
}
