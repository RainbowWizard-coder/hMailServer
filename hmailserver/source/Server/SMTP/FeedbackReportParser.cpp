// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "FeedbackReportParser.h"
#include "DeliveryReportParser.h"

#include "../Common/Mime/Mime.h"
#include "../Common/Util/ByteBuffer.h"
#include "../Common/Util/File.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/Util/MessageOrigin.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/Unicode.h"

#include <stdexcept>

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

      // A header value as a reader would see it. An RFC 2047 encoded word is
      // decoded through the tree's own field coder - the one the message trace,
      // the webmail and the vacation responder use - because the value this is
      // wanted for, the subject of the message complained about, is drawn on a
      // page and searched by its words: left raw, a German or Japanese newsletter
      // (the mail most likely to draw a complaint) reads as "=?UTF-8?B?..."
      // everywhere and matches nothing anybody would type. A value with no
      // encoded word in it is 8-bit text a provider sent as it stood, which is
      // UTF-8 in practice, so it keeps the UTF-8 reading; the field coder would
      // read those bytes in the machine's ANSI code page instead and make
      // mojibake of them.
      String HeaderText(const AnsiString &raw)
      {
         if (raw.Find("=?") >= 0)
            return MimeHeader::GetUnicodeFieldValue("Subject", raw);

         String wide;
         if (Unicode::MultiByteToWide(raw, wide))
            return wide;

         // A best effort rather than nothing: a value this server cannot read as
         // UTF-8 is still what the provider said the subject was, and an empty
         // field on the page would say the message had no subject at all.
         return String(raw);
      }

      String Lower(const String &value)
      {
         String result = value;
         result.ToLower();
         return result;
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
   FeedbackReportParser::Address(const String &value)
   {
      String text = value;
      text.Trim();

      if (text.IsEmpty())
         return String();

      // A comment at the end, which providers do add: "(a reader of ours)".
      int comment = text.Find(_T("("));
      if (comment >= 0)
      {
         text = text.Mid(0, comment);
         text.Trim();
      }

      // "rfc822; someone@example.test", the Original-Recipient shape RFC 3464 gives
      // and RFC 5965 borrows. Only a type made of the characters a type is made of
      // is taken as one, so a local part holding a semicolon is not mistaken for it.
      int semicolon = text.Find(_T(";"));
      if (semicolon > 0)
      {
         bool typeLike = true;
         for (int i = 0; i < semicolon; i++)
         {
            const wchar_t c = text.GetAt(i);
            if (!iswalnum(c) && c != '-' && c != '_' && c != '.')
            {
               typeLike = false;
               break;
            }
         }

         if (typeLike)
         {
            text = text.Mid(semicolon + 1);
            text.Trim();
         }
      }

      // A display name in front of an angle-addr, and the brackets themselves.
      const int open = text.Find(_T("<"));
      if (open >= 0)
      {
         const int close = text.Find(_T(">"), open + 1);
         if (close <= open)
            return String();

         text = text.Mid(open + 1, close - open - 1);
         text.Trim();
      }

      // One address, and nothing that could end a line or a field somewhere this
      // value is written back out.
      for (int i = 0; i < text.GetLength(); i++)
      {
         const wchar_t c = text.GetAt(i);
         if (c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == ',' || c == 0)
            return String();
      }

      if (!StringParser::IsValidEmailAddress(text, true))
         return String();

      return Lower(text).Left(255);
   }

   String
   FeedbackReportParser::NormaliseDate(const String &value)
   {
      String text = value;
      text.Trim();

      if (text.IsEmpty())
         return String();

      const DateTime when = Time::GetDateTimeFromMimeHeader(text);

      // An unreadable date reads as the zero date, which no arrival ever is.
      if (when.GetYear() < 1971 || when.GetYear() > 9998)
         return String();

      return Time::GetTimeStampFromDateTime(when);
   }

   void
   FeedbackReportParser::ReadFields(const AnsiString &text, Fields &fields)
   {
      fields.clear();

      String pendingName;
      AnsiString pendingValue;
      bool havePending = false;

      auto flush = [&]()
      {
         if (havePending)
         {
            pendingValue.Trim();
            fields.push_back(std::make_pair(pendingName, FromUtf8(pendingValue)));
         }

         havePending = false;
         pendingName = String();
         pendingValue = AnsiString();
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
            // A blank line before the first field is the one that ended the part's
            // own MIME header, which some senders leave on the decoded body - the
            // same thing DeliveryReportParser strips from a returned-headers part.
            // Stopping there would refuse every report from such a sender.
            if (fields.empty() && !havePending)
            {
               if (lineEnd >= length)
                  break;

               continue;
            }

            // A blank line after them ends the block. The part's fields are all that
            // is read; whatever a sender puts after them is not a field of this
            // report.
            break;
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

         flush();

         const int colon = line.Find(":");
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

      flush();
   }

   String
   FeedbackReportParser::FieldValue_(const Fields &fields, const wchar_t *name)
   {
      for (const auto &field : fields)
      {
         if (field.first == name)
            return field.second;
      }

      return String();
   }

   bool
   FeedbackReportParser::Parse(const AnsiString &raw, FeedbackReport &report)
   {
      report = FeedbackReport();

      if (raw.IsEmpty() || (size_t) raw.GetLength() > MaxReportBytes)
         return false;

      MimeBody message;
      size_t index = 0;
      bool partLoaded = false;

      if (message.Load(raw.c_str(), raw.GetLength(), index, partLoaded) == 0)
         return false;

      // RFC 5965 2: the report IS the message. A report forwarded as an attachment,
      // or a multipart/mixed that happens to hold one, is mail about a report and is
      // left alone - exactly as DeliveryReportParser leaves one alone.
      if (Lower(message.GetCleanContentType()) != _T("multipart/report"))
         return false;

      String reportType = Lower(String(message.GetParameter("Content-Type", "report-type").c_str()));
      reportType.Trim();
      if (reportType != _T("feedback-report"))
         return false;

      AnsiString reportText;
      bool haveReport = false;

      AnsiString originalHeaders;
      bool haveOriginal = false;

      for (std::shared_ptr<MimeBody> part = message.FindFirstPart(); part; part = message.FindNextPart())
      {
         const String contentType = Lower(part->GetCleanContentType());

         if (!haveReport && contentType == _T("message/feedback-report"))
         {
            if (part->GetDecodedContent(reportText))
               haveReport = true;

            continue;
         }

         // The message complained about, or its headers. Only the header block is
         // ever read: FirstFieldValue stops at the blank line, so a body line that
         // looks like a field is never taken for one.
         if (!haveOriginal &&
             (contentType == _T("text/rfc822-headers") || contentType == _T("message/rfc822") ||
              contentType == _T("message/global-headers") || contentType == _T("message/global")))
         {
            AnsiString content;
            if (!part->GetDecodedContent(content))
               continue;

            // A headers part may begin with the blank line that ended its own MIME
            // header, depending on how the sender folded it.
            while (content.GetLength() >= 2 && content.Left(2) == "\r\n")
               content = content.Mid(2);

            originalHeaders = content;
            haveOriginal = true;
         }
      }

      if (!haveReport)
         return false;

      Fields fields;
      ReadFields(reportText, fields);

      String feedbackType = FieldValue_(fields, L"feedback-type");
      feedbackType.Trim();

      // The one field RFC 5965 3.1 makes mandatory, and the one this server acts on.
      if (feedbackType.IsEmpty())
         return false;

      report.feedback_type = Lower(feedbackType).Left(32);
      report.user_agent = FieldValue_(fields, L"user-agent").Left(255);
      report.original_mail_from = Address(FieldValue_(fields, L"original-mail-from"));
      report.original_rcpt_to = Address(FieldValue_(fields, L"original-rcpt-to"));
      report.arrival_date = NormaliseDate(FieldValue_(fields, L"arrival-date"));

      String sourceIp = FieldValue_(fields, L"source-ip");
      sourceIp.Trim();
      report.source_ip = StringParser::IsValidIPAddress(sourceIp) ? sourceIp.Left(45) : String();

      String reportedDomain = FieldValue_(fields, L"reported-domain");
      reportedDomain.Trim();
      report.reported_domain = StringParser::IsValidDomainName(reportedDomain) ? Lower(reportedDomain).Left(255) : String();

      if (haveOriginal)
      {
         report.original_message_id =
            DeliveryReportParser::NormaliseMessageId(FromUtf8(MessageOrigin::FirstFieldValue(originalHeaders, "Message-ID")));
         report.original_from = Address(FromUtf8(MessageOrigin::FirstFieldValue(originalHeaders, "From")));
         report.original_to = Address(FromUtf8(MessageOrigin::FirstFieldValue(originalHeaders, "To")));

         String subject = HeaderText(MessageOrigin::FirstFieldValue(originalHeaders, "Subject"));
         subject.Trim();
         report.original_subject = subject.Left(255);
      }

      return true;
   }

   bool
   FeedbackReportParser::ParseFile(const String &fileName, FeedbackReport &report)
   {
      report = FeedbackReport();

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
   FeedbackReportParserTester::Test()
   {
      typedef FeedbackReportParser P;

      // ---------------------------------------------------------------- Address
      Expect(P::Address(_T("<Reader@Provider.TEST>")) == _T("reader@provider.test"), "An angle-addr was not read.");
      Expect(P::Address(_T("rfc822; Reader@Provider.test")) == _T("reader@provider.test"), "A typed address was not read.");
      Expect(P::Address(_T(" reader@provider.test (a reader of ours) ")) == _T("reader@provider.test"), "A commented address was not read.");
      Expect(P::Address(_T("A Reader <reader@provider.test>")) == _T("reader@provider.test"), "A display name was not stripped.");
      Expect(P::Address(_T("redacted")).IsEmpty(), "A redacted recipient was read as an address.");
      Expect(P::Address(_T("one@a.test, two@b.test")).IsEmpty(), "Two addresses were read as one.");
      Expect(P::Address(_T("")).IsEmpty(), "An empty value was read as an address.");
      Expect(P::Address(_T("a@b.test\r\nX-Injected: yes")).IsEmpty(), "A value carrying a line break was accepted.");

      // ------------------------------------------------------------- Arrival date
      Expect(P::NormaliseDate(_T("Thu, 8 Mar 2005 14:00:00 GMT")) == _T("2005-03-08 14:00:00"),
             "An RFC 5322 arrival date was not normalised.");
      Expect(P::NormaliseDate(_T("not a date")).IsEmpty(), "A date that is not one was not refused.");
      Expect(P::NormaliseDate(_T("")).IsEmpty(), "An absent date was not refused.");

      // ------------------------------------------------------------------- Fields
      P::Fields fields;
      P::ReadFields(Crlf("Feedback-Type: abuse\nUser-Agent: SomeGenerator\n  /1.0\nVersion: 1\n\nnot-a-field: no\n"), fields);
      Expect(fields.size() == 3, "The feedback-report fields were not read as three.");
      Expect(fields[1].first == _T("user-agent") && fields[1].second == _T("SomeGenerator /1.0"),
             "A folded field was not unfolded.");

      // The blank line that ended the part's own MIME header, left on the decoded
      // body by some senders. Stopping there refuses the whole report.
      P::Fields leading;
      P::ReadFields(Crlf("\nFeedback-Type: abuse\nUser-Agent: SomeGenerator\n\ntrailing text\n"), leading);
      Expect(leading.size() == 2 && leading[0].second == _T("abuse"),
             "A blank line before the first field stopped the fields being read.");

      // ------------------------------------------------------------- A real report
      const AnsiString report = Crlf(
         "From: fbl@provider.test\n"
         "To: abuse@example.com\n"
         "Subject: FW: Earn money\n"
         "Content-Type: multipart/report; report-type=feedback-report; boundary=\"b\"\n"
         "MIME-Version: 1.0\n"
         "\n"
         "--b\n"
         "Content-Type: text/plain\n"
         "\n"
         "This is an email abuse report for an email message received from IP 192.0.2.1.\n"
         "\n"
         "--b\n"
         "Content-Type: message/feedback-report\n"
         "\n"
         "Feedback-Type: abuse\n"
         "User-Agent: SomeGenerator/1.0\n"
         "Version: 1\n"
         "Original-Mail-From: <sender@example.com>\n"
         "Original-Rcpt-To: <reader@provider.test>\n"
         "Arrival-Date: Thu, 8 Mar 2005 14:00:00 GMT\n"
         "Source-IP: 192.0.2.1\n"
         "Reported-Domain: Example.COM\n"
         "Reported-URI: http://example.com/earn_money.html\n"
         "\n"
         "--b\n"
         "Content-Type: message/rfc822\n"
         "\n"
         "From: <sender@example.com>\n"
         "To: <reader@provider.test>\n"
         "Subject: Earn money\n"
         "Message-ID: <ABC.123@Mail.Example.COM>\n"
         "\n"
         "Body that is never read.\n"
         "Subject: not a field\n"
         "\n"
         "--b--\n");

      FeedbackReport parsed;
      Expect(P::Parse(report, parsed), "A well-formed feedback report was not read.");
      Expect(parsed.feedback_type == _T("abuse"), "The feedback type was not read.");
      Expect(parsed.user_agent == _T("SomeGenerator/1.0"), "The user agent was not read.");
      Expect(parsed.original_mail_from == _T("sender@example.com"), "Original-Mail-From was not read.");
      Expect(parsed.original_rcpt_to == _T("reader@provider.test"), "Original-Rcpt-To was not read.");
      Expect(parsed.arrival_date == _T("2005-03-08 14:00:00"), "The arrival date was not read.");
      Expect(parsed.source_ip == _T("192.0.2.1"), "The source IP was not read.");
      Expect(parsed.reported_domain == _T("example.com"), "The reported domain was not lower-cased.");
      Expect(parsed.original_message_id == _T("abc.123@mail.example.com"), "The original Message-ID was not normalised.");
      Expect(parsed.original_from == _T("sender@example.com"), "The original From was not read.");
      Expect(parsed.original_to == _T("reader@provider.test"), "The original To was not read.");
      Expect(parsed.original_subject == _T("Earn money"), "The original Subject was not read, or a body line was taken for it.");

      // ------------------------------------------------- An encoded subject
      // The mail most likely to draw a complaint is a newsletter, and a newsletter
      // in any language but English carries an RFC 2047 encoded word for a Subject.
      // Stored as it stands it is drawn on the Deck as "=?utf-8?Q?..." and matches
      // no search term anybody would type.
      FeedbackReport encoded;
      Expect(P::Parse(Crlf(
         "Content-Type: multipart/report; report-type=feedback-report; boundary=\"b\"\n"
         "\n"
         "--b\n"
         "Content-Type: message/feedback-report\n"
         "\n"
         "Feedback-Type: abuse\n"
         "\n"
         "--b\n"
         "Content-Type: message/rfc822\n"
         "\n"
         "From: <sender@example.com>\n"
         "Subject: =?utf-8?Q?Gr=C3=BC=C3=9Fe_aus_Berlin?=\n"
         "\n"
         "Body.\n"
         "\n"
         "--b--\n"), encoded),
             "A report whose wrapped subject is an encoded word was not read.");
      Expect(encoded.original_subject == _T("Gr\u00fc\u00dfe aus Berlin"),
             "An RFC 2047 encoded subject was stored as its encoded word rather than as its words.");

      // ------------------------------------------------- What is not a report
      FeedbackReport refused;

      Expect(!P::Parse(Crlf(
         "Content-Type: multipart/report; report-type=delivery-status; boundary=\"b\"\n"
         "\n"
         "--b\n"
         "Content-Type: message/delivery-status\n"
         "\n"
         "Final-Recipient: rfc822; gone@elsewhere.test\n"
         "Action: failed\n"
         "\n"
         "--b--\n"), refused), "A delivery report was read as a feedback report.");

      Expect(!P::Parse(Crlf(
         "Content-Type: multipart/mixed; boundary=\"b\"\n"
         "\n"
         "--b\n"
         "Content-Type: message/feedback-report\n"
         "\n"
         "Feedback-Type: abuse\n"
         "\n"
         "--b--\n"), refused), "A feedback report attached to ordinary mail was read as one.");

      Expect(!P::Parse(Crlf(
         "Content-Type: multipart/report; report-type=feedback-report; boundary=\"b\"\n"
         "\n"
         "--b\n"
         "Content-Type: text/plain\n"
         "\n"
         "Somebody complained.\n"
         "\n"
         "--b--\n"), refused), "A report with no feedback-report part was read.");

      Expect(!P::Parse(Crlf(
         "Content-Type: multipart/report; report-type=feedback-report; boundary=\"b\"\n"
         "\n"
         "--b\n"
         "Content-Type: message/feedback-report\n"
         "\n"
         "User-Agent: SomeGenerator/1.0\n"
         "Original-Rcpt-To: <reader@provider.test>\n"
         "\n"
         "--b--\n"), refused), "A report with no Feedback-Type was read.");

      Expect(!P::Parse(AnsiString(), refused), "An empty message was read as a report.");

      // A report that names nobody is still a report: the provider redacted the
      // reader, which several of them do, and the domain still has a complaint.
      FeedbackReport redacted;
      Expect(P::Parse(Crlf(
         "Content-Type: multipart/report; report-type=feedback-report; boundary=\"b\"\n"
         "\n"
         "--b\n"
         "Content-Type: message/feedback-report\n"
         "\n"
         "Feedback-Type: abuse\n"
         "Original-Rcpt-To: redacted\n"
         "Source-IP: not an address\n"
         "Reported-Domain: not a domain name!\n"
         "\n"
         "--b--\n"), redacted), "A redacted report was refused.");
      Expect(redacted.original_rcpt_to.IsEmpty(), "A redacted recipient was read as an address.");
      Expect(redacted.source_ip.IsEmpty(), "A source IP that is not one was kept.");
      Expect(redacted.reported_domain.IsEmpty(), "A reported domain that is not one was kept.");
      Expect(redacted.original_message_id.IsEmpty(), "A report with no returned message named one.");
   }
}
