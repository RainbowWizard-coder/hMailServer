// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "BounceClassifier.h"

#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Refused on policy. Checked FIRST, because a reply that says both "user
      // unknown" and "blocked" is almost always a policy engine explaining itself
      // badly, and a policy refusal must never put an address on a list.
      const wchar_t *PolicyWords[] =
      {
         L"spam", L"blocked", L"blacklist", L"blocklist", L"listed", L"policy",
         L"reputation", L"rbl", L"spamhaus", L"denied", L"not allowed", L"relay",
      };

      const wchar_t *MailboxFullWords[] =
      {
         L"mailbox full", L"mailbox is full", L"over quota", L"quota exceeded",
         L"insufficient storage", L"mailbox size limit",
      };

      // Deliberately not "mailbox unavailable": that is RFC 5321's own text for a
      // 550 of any kind, and servers use it for policy refusals as readily as for
      // dead addresses.
      const wchar_t *HardWords[] =
      {
         L"user unknown", L"unknown user", L"no such user", L"no such mailbox",
         L"mailbox not found", L"recipient not found", L"recipnotfound",
         L"does not exist", L"unrouteable address", L"invalid recipient",
         L"unknown recipient",
      };

      bool ContainsAny(const String &lowerText, const wchar_t *const *words, size_t count)
      {
         for (size_t i = 0; i < count; i++)
         {
            if (lowerText.Find(words[i]) >= 0)
               return true;
         }

         return false;
      }

      // The last line that says anything. A multi-line reply repeats its code on
      // every line and the last one carries the verdict, which is the line
      // DeliveryFailure::ExtractRemoteEnhancedStatus reads too.
      String LastLine(const String &text)
      {
         String value = text;
         value.TrimRight(_T("\r\n \t"));

         int lastBreak = -1;
         for (int i = value.GetLength() - 1; i >= 0; i--)
         {
            if (value.GetAt(i) == '\n' || value.GetAt(i) == '\r')
            {
               lastBreak = i;
               break;
            }
         }

         if (lastBreak >= 0)
            value = value.Mid(lastBreak + 1);

         value.TrimLeft();
         return value;
      }

      bool IsUndefinedDetail(const String &code)
      {
         // X.0.0: "other or undefined status", which names no condition at all.
         int firstDot = code.Find(_T("."));
         if (firstDot < 0)
            return false;

         return code.Mid(firstDot) == _T(".0.0");
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }
   }

   bool
   BounceClassifier::IsEnhancedStatusCode(const String &token)
   {
      if (token.GetLength() < 5)
         return false;

      const wchar_t classDigit = token.GetAt(0);
      if (classDigit != '2' && classDigit != '4' && classDigit != '5')
         return false;

      if (token.GetAt(1) != '.')
         return false;

      int position = 2;

      for (int part = 0; part < 2; part++)
      {
         int digits = 0;
         while (position < token.GetLength() && token.GetAt(position) >= '0' && token.GetAt(position) <= '9')
         {
            position++;
            digits++;
         }

         if (digits == 0 || digits > 3)
            return false;

         if (part == 0)
         {
            if (position >= token.GetLength() || token.GetAt(position) != '.')
               return false;

            position++;
         }
      }

      // Nothing may follow: "5.1.1:" is not a status code.
      return position == token.GetLength();
   }

   int
   BounceClassifier::ReplyCode(const String &text)
   {
      String value = text;
      value.TrimLeft();

      if (value.GetLength() < 3)
         return 0;

      for (int i = 0; i < 3; i++)
      {
         if (value.GetAt(i) < '0' || value.GetAt(i) > '9')
            return 0;
      }

      if (value.GetLength() > 3 && value.GetAt(3) != ' ' && value.GetAt(3) != '-' && value.GetAt(3) != '\t')
         return 0;

      return (value.GetAt(0) - '0') * 100 + (value.GetAt(1) - '0') * 10 + (value.GetAt(2) - '0');
   }

   String
   BounceClassifier::ExtractEnhancedCode(const String &text)
   {
      String line = LastLine(text);
      if (line.IsEmpty())
         return String();

      const int replyCode = ReplyCode(line);

      String rest = replyCode > 0 ? line.Mid(4) : line;
      rest.TrimLeft();

      int end = rest.GetLength();
      for (int i = 0; i < rest.GetLength(); i++)
      {
         wchar_t c = rest.GetAt(i);
         if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
         {
            end = i;
            break;
         }
      }

      String token = rest.Mid(0, end);

      if (!IsEnhancedStatusCode(token))
         return String();

      // RFC 3463 2: the class digit must agree with the reply code. A server that
      // answers "550 4.2.2" contradicts itself, and the three-digit code is the one
      // the protocol acted on - so neither half is believed.
      if (replyCode > 0 && token.GetAt(0) != (wchar_t) ('0' + (replyCode / 100)))
         return String();

      return token;
   }

   BounceCategory
   BounceClassifier::FromCode(const String &code)
   {
      if (!IsEnhancedStatusCode(code))
         return BounceOther;

      const wchar_t classDigit = code.GetAt(0);

      if (classDigit == '2')
         return BounceNone;

      int firstDot = code.Find(_T("."));
      int secondDot = code.Find(_T("."), firstDot + 1);

      const int subject = _ttoi(code.Mid(firstDot + 1, secondDot - firstDot - 1));
      const int detail = _ttoi(code.Mid(secondDot + 1));

      // X.2.2, mailbox full, in either class. Whether a permanent one counts
      // towards suppression is BounceMailboxFullIsHard's decision, not this table's.
      if (subject == 2 && detail == 2)
         return BounceMailboxFull;

      if (classDigit == '4')
         return BounceSoft;

      if (subject == 7)
         return BouncePolicy;

      // Bad destination mailbox (X.1.1), bad destination system (X.1.2), bad
      // destination mailbox syntax (X.1.3), mailbox moved with no forwarding
      // address (X.1.6), a domain with a null MX (X.1.10, RFC 7505), and a
      // disabled mailbox (X.2.1).
      if (subject == 1 && (detail == 1 || detail == 2 || detail == 3 || detail == 6 || detail == 10))
         return BounceHard;

      if (subject == 2 && detail == 1)
         return BounceHard;

      return BounceOther;
   }

   BounceCategory
   BounceClassifier::FromKeywords(const String &text, wchar_t classDigit)
   {
      String lower = text;
      lower.ToLower();

      const size_t policyCount = sizeof(PolicyWords) / sizeof(PolicyWords[0]);
      const size_t fullCount = sizeof(MailboxFullWords) / sizeof(MailboxFullWords[0]);
      const size_t hardCount = sizeof(HardWords) / sizeof(HardWords[0]);

      // A temporary failure is temporary whatever its words say, except that a
      // full mailbox is worth telling apart from the rest.
      if (classDigit == '4')
         return ContainsAny(lower, MailboxFullWords, fullCount) ? BounceMailboxFull : BounceNone;

      if (classDigit != '5')
         return BounceNone;

      if (ContainsAny(lower, PolicyWords, policyCount))
         return BouncePolicy;

      if (ContainsAny(lower, MailboxFullWords, fullCount))
         return BounceMailboxFull;

      if (ContainsAny(lower, HardWords, hardCount))
         return BounceHard;

      return BounceNone;
   }

   BounceClassifier::Verdict
   BounceClassifier::ClassifyRefusal(const String &statusCode, const String &remoteReply)
   {
      Verdict verdict;

      String reply = remoteReply;
      reply.Trim();

      // No remote server answered. The code is this server's own reason - a
      // connection that failed, a conversion the remote could not take - and it
      // says nothing about whether the mailbox exists.
      if (reply.IsEmpty())
      {
         verdict.status = statusCode;

         const wchar_t classDigit = statusCode.IsEmpty() ? '5' : statusCode.GetAt(0);

         if (classDigit == '2')
            verdict.category = BounceNone;
         else if (classDigit == '4')
            verdict.category = BounceSoft;
         else
            verdict.category = BounceOther;

         return verdict;
      }

      const int replyCode = ReplyCode(reply);

      if (replyCode >= 200 && replyCode <= 399)
      {
         verdict.status = statusCode;
         verdict.category = BounceNone;
         return verdict;
      }

      wchar_t classDigit = '5';
      if (replyCode >= 400 && replyCode <= 499)
         classDigit = '4';
      else if (replyCode == 0 && !statusCode.IsEmpty() && statusCode.GetAt(0) == '4')
         classDigit = '4';

      const String remoteCode = ExtractEnhancedCode(reply);

      if (!remoteCode.IsEmpty())
      {
         // The remote's own code, which is the only kind that is believed.
         verdict.trusted = true;
         verdict.status = remoteCode;
         verdict.category = FromCode(remoteCode);

         if (IsUndefinedDetail(remoteCode))
         {
            const BounceCategory fromWords = FromKeywords(reply, classDigit);
            if (fromWords != BounceNone)
               verdict.category = fromWords;
         }

         return verdict;
      }

      // No enhanced code in the reply, so the one this server recorded was
      // derived from the three digits alone and is not believed. The words decide,
      // and when they say nothing a permanent failure is "other" - never hard.
      verdict.trusted = false;
      verdict.status = statusCode.IsEmpty() ? String(classDigit == '4' ? _T("4.0.0") : _T("5.0.0")) : statusCode;

      const BounceCategory fromWords = FromKeywords(reply, classDigit);

      if (fromWords != BounceNone)
         verdict.category = fromWords;
      else
         verdict.category = classDigit == '4' ? BounceSoft : BounceOther;

      return verdict;
   }

   BounceClassifier::Verdict
   BounceClassifier::ClassifyReport(const String &status, const String &diagnostic)
   {
      Verdict verdict;

      String statusField = status;
      statusField.Trim();

      // A report group without a usable Status is not a report anybody can act
      // on. It is recorded as "other", which counts and never suppresses.
      if (!IsEnhancedStatusCode(statusField))
      {
         verdict.status = statusField.Left(16);
         verdict.category = BounceOther;
         return verdict;
      }

      // The Status field is the reporting MTA's verdict in RFC 3464's own words,
      // so it is trusted as a remote's enhanced code is.
      verdict.trusted = true;
      verdict.status = statusField;

      const wchar_t classDigit = statusField.GetAt(0);

      if (classDigit == '2')
      {
         verdict.category = BounceNone;
         return verdict;
      }

      if (!IsUndefinedDetail(statusField))
      {
         verdict.category = FromCode(statusField);
         return verdict;
      }

      // X.0.0 says nothing, so the diagnostic is asked: first its own enhanced
      // code, when it has one of the same class, then its words.
      const String diagnosticCode = ExtractEnhancedCode(diagnostic);

      if (!diagnosticCode.IsEmpty() && diagnosticCode.GetAt(0) == classDigit && !IsUndefinedDetail(diagnosticCode))
      {
         verdict.status = diagnosticCode;
         verdict.category = FromCode(diagnosticCode);
         return verdict;
      }

      const BounceCategory fromWords = FromKeywords(diagnostic, classDigit);

      if (fromWords != BounceNone)
         verdict.category = fromWords;
      else
         verdict.category = classDigit == '4' ? BounceSoft : BounceOther;

      return verdict;
   }

   bool
   BounceClassifier::Suppresses(BounceCategory category, bool mailboxFullIsHard)
   {
      if (category == BounceHard)
         return true;

      return category == BounceMailboxFull && mailboxFullIsHard;
   }

   const char *
   BounceClassifier::Name(BounceCategory category)
   {
      switch (category)
      {
      case BounceNone:        return "none";
      case BounceHard:        return "hard";
      case BounceSoft:        return "soft";
      case BounceMailboxFull: return "mailbox_full";
      case BouncePolicy:      return "policy";
      case BounceOther:       return "other";
      }

      return "other";
   }

   BounceCategory
   BounceClassifier::FromName(const String &name)
   {
      for (int value = BounceNone; value <= BounceOther; value++)
      {
         if (name.CompareNoCase(String(Name((BounceCategory) value)).c_str()) == 0)
            return (BounceCategory) value;
      }

      return BounceOther;
   }

   void
   BounceClassifierTester::Test()
   {
      typedef BounceClassifier C;

      Expect(C::IsEnhancedStatusCode(_T("5.1.1")), "5.1.1 was not read as an enhanced status code.");
      Expect(C::IsEnhancedStatusCode(_T("5.1.10")), "5.1.10 was not read as an enhanced status code.");
      Expect(!C::IsEnhancedStatusCode(_T("5.1")), "5.1 was read as an enhanced status code.");
      Expect(!C::IsEnhancedStatusCode(_T("6.1.1")), "A class 6 code was accepted.");
      Expect(!C::IsEnhancedStatusCode(_T("5.1.1:")), "A code with trailing punctuation was accepted.");
      Expect(!C::IsEnhancedStatusCode(_T("5.1234.1")), "A four-digit subject was accepted.");

      C::Verdict v = C::ClassifyRefusal(_T("5.1.1"), _T("550 5.1.1 <gone@example.test>: User unknown"));
      Expect(v.category == BounceHard && v.trusted, "A remote's own 5.1.1 was not classified as a hard bounce.");

      v = C::ClassifyRefusal(_T("5.1.10"), _T("550 5.1.10 RESOLVER.ADR.RecipientNotFound; Recipient not found by SMTP address lookup"));
      Expect(v.category == BounceHard, "A 5.1.10 from Exchange Online was not classified as a hard bounce.");

      v = C::ClassifyRefusal(_T("5.2.2"), _T("552 5.2.2 Mailbox full"));
      Expect(v.category == BounceMailboxFull, "552 5.2.2 was not classified as a full mailbox.");
      Expect(!C::Suppresses(v.category, false), "A full mailbox suppressed with BounceMailboxFullIsHard off.");
      Expect(C::Suppresses(v.category, true), "A full mailbox did not suppress with BounceMailboxFullIsHard on.");

      v = C::ClassifyRefusal(_T("5.7.1"), _T("550 5.7.1 Service unavailable; client host blocked using Spamhaus"));
      Expect(v.category == BouncePolicy, "A 5.7.1 refusal was not classified as policy.");
      Expect(!C::Suppresses(v.category, true), "A policy refusal was allowed to suppress an address.");

      // THE NEGATIVE CONTROL. A bare 550 with nothing else is recorded as 5.1.1 by
      // DeliveryFailure::EnhancedStatusFromReplyCodeOnly, and must not be believed.
      v = C::ClassifyRefusal(_T("5.1.1"), _T("550 <gone@example.test>"));
      Expect(v.category == BounceOther && !v.trusted, "A 5.1.1 this server derived from a bare 550 was trusted as a dead mailbox.");
      Expect(!C::Suppresses(v.category, true), "A bare 550 was allowed to suppress an address.");

      v = C::ClassifyRefusal(_T("5.1.1"), _T("550 No such user here"));
      Expect(v.category == BounceHard && !v.trusted, "A bare 550 saying 'No such user' was not classified hard by its words.");

      v = C::ClassifyRefusal(_T("5.1.1"), _T("550 Recipient address rejected: Access denied"));
      Expect(v.category == BouncePolicy, "Policy words did not win over the rest of a bare 550.");

      v = C::ClassifyRefusal(_T("4.7.1"), _T("451 4.7.1 Greylisted, please try again later"));
      Expect(v.category == BounceSoft, "A greylisting deferral was not classified soft.");

      v = C::ClassifyRefusal(_T("4.2.0"), _T("450 Mailbox is full, try later"));
      Expect(v.category == BounceMailboxFull, "A 450 saying the mailbox is full was not classified as a full mailbox.");

      v = C::ClassifyRefusal(_T("5.6.3"), _T(""));
      Expect(v.category == BounceOther, "This server's own 5.6.3 was not classified as other.");

      v = C::ClassifyRefusal(_T("5.3.4"), _T(""));
      Expect(v.category == BounceOther, "This server's own 5.3.4 was not classified as other.");

      v = C::ClassifyRefusal(_T("5.1.1"), _T(""));
      Expect(v.category != BounceHard, "A 5.1.1 with no remote reply was classified hard.");

      v = C::ClassifyRefusal(_T("4.4.1"), _T(""));
      Expect(v.category == BounceSoft, "This server's own 4.4.1 was not classified soft.");

      v = C::ClassifyRefusal(_T("2.0.0"), _T("250 2.0.0 Ok: queued"));
      Expect(v.category == BounceNone, "A 250 was classified as a bounce.");

      v = C::ClassifyRefusal(_T("5.0.0"), _T("550 5.1.1: malformed"));
      Expect(v.category == BounceOther && !v.trusted, "A malformed enhanced code was trusted.");

      v = C::ClassifyRefusal(_T("5.0.0"), _T("550 4.2.2 contradicting itself"));
      Expect(v.category == BounceOther && !v.trusted, "An enhanced code whose class disagrees with the reply was trusted.");

      v = C::ClassifyRefusal(_T("5.1.1"),
         _T("550-5.1.1 The email account that you tried to reach does not exist.\r\n550 5.1.1 https://support.google.com/mail/?p=NoSuchUser - gsmtp"));
      Expect(v.category == BounceHard && v.trusted, "A multi-line 5.1.1 was not classified hard.");

      v = C::ClassifyRefusal(_T("5.0.0"), _T("550 5.0.0 <gone@example.test>... User unknown"));
      Expect(v.category == BounceHard && v.trusted, "An X.0.0 reply was not read by its words.");

      // Delivery reports.
      v = C::ClassifyReport(_T("5.0.0"), _T("550 5.1.1 <gone@example.test>: User unknown"));
      Expect(v.category == BounceHard, "A 5.0.0 report whose diagnostic says 5.1.1 was not classified hard.");

      v = C::ClassifyReport(_T("5.1.1"), _T(""));
      Expect(v.category == BounceHard && v.trusted, "A report's Status 5.1.1 was not classified hard.");

      v = C::ClassifyReport(_T("4.4.7"), _T(""));
      Expect(v.category == BounceSoft, "A report's Status 4.4.7 was not classified soft.");

      v = C::ClassifyReport(_T("5.2.2"), _T("552 5.2.2 Over quota"));
      Expect(v.category == BounceMailboxFull, "A report's 5.2.2 was not classified as a full mailbox.");

      v = C::ClassifyReport(_T("2.0.0"), _T(""));
      Expect(v.category == BounceNone, "A delivered report group was classified as a bounce.");

      v = C::ClassifyReport(_T("nonsense"), _T("550 5.1.1 User unknown"));
      Expect(v.category == BounceOther && !v.trusted, "A report group with no usable Status was trusted.");

      v = C::ClassifyReport(_T("5.0.0"), _T("550 Message rejected as spam"));
      Expect(v.category == BouncePolicy, "A 5.0.0 report saying spam was not classified as policy.");

      Expect(C::FromName(_T("mailbox_full")) == BounceMailboxFull, "mailbox_full did not name BounceMailboxFull.");
      Expect(String(C::Name(BouncePolicy)) == _T("policy"), "BouncePolicy is not named policy.");
      Expect(C::ExtractEnhancedCode(_T("5.7.26 Unauthenticated email is not accepted")) == _T("5.7.26"), "A bare enhanced code was not extracted.");
      Expect(C::ReplyCode(_T("550-5.1.1 x")) == 550, "A continuation line's reply code was not read.");
   }
}
