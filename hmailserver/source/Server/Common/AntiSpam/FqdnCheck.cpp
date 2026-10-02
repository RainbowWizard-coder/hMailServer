// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "FqdnCheck.h"
#include "QuarantineReview.h"

#include "../Util/Parsing/StringParser.h"

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
      bool IsLabelCharacter(wchar_t c)
      {
         if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return true;

         if (c == '-' || c == '_')
            return true;

         // Beyond ASCII: a U-label of an internationalised name. wchar_t is
         // signed on some platforms, hence both halves.
         return c > 127 || c < 0;
      }

      bool IsDigits(const String &label)
      {
         if (label.IsEmpty())
            return false;

         for (int i = 0; i < label.GetLength(); i++)
         {
            const wchar_t c = label.GetAt(i);
            if (c < '0' || c > '9')
               return false;
         }

         return true;
      }
   }

   FqdnCheck::Verdict
   FqdnCheck::Judge(const String &name)
   {
      String value = name;
      value.Trim();

      // One root dot, which makes a name absolute and is not a label. Two are an
      // empty label, judged below.
      if (value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      if (value.IsEmpty())
         return VerdictEmpty;

      // [192.0.2.7], [IPv6:2001:db8::7], and anything else a client brackets:
      // RFC 5321's address literal, which is never a name.
      if (value.StartsWith(_T("[")))
         return VerdictAddressLiteral;

      // An address without its brackets, which RFC 5321 does not allow and
      // clients send anyway.
      if (StringParser::IsValidIPAddress(value))
         return VerdictAddress;

      // The labels, each of something and of nothing a host name cannot hold.
      int labelCount = 0;
      int labelStart = 0;
      String lastLabel;

      const int length = value.GetLength();
      for (int i = 0; i <= length; i++)
      {
         if (i == length || value.GetAt(i) == '.')
         {
            if (i == labelStart)
               return VerdictNotAHostName;

            lastLabel = value.Mid(labelStart, i - labelStart);
            labelCount++;
            labelStart = i + 1;
            continue;
         }

         if (!IsLabelCharacter(value.GetAt(i)))
            return VerdictNotAHostName;
      }

      if (labelCount < 2)
         return VerdictSingleLabel;

      if (IsDigits(lastLabel))
         return VerdictNumericTopLabel;

      if (QuarantineReview::IsPublicSuffix(value))
         return VerdictPublicSuffix;

      return VerdictFqdn;
   }

   String
   FqdnCheck::Describe(Verdict verdict)
   {
      switch (verdict)
      {
      case VerdictFqdn: return _T("a fully qualified domain name");
      case VerdictEmpty: return _T("empty");
      case VerdictAddressLiteral: return _T("an address literal");
      case VerdictAddress: return _T("an IP address");
      case VerdictNotAHostName: return _T("not a host name");
      case VerdictSingleLabel: return _T("a single label");
      case VerdictNumericTopLabel: return _T("numeric where its top-level domain should be");
      case VerdictPublicSuffix: return _T("a public suffix");
      }

      return _T("not a host name");
   }

   String
   FqdnCheck::SenderDomain(const String &envelopeSender)
   {
      const int at = envelopeSender.ReverseFind(_T("@"));
      if (at < 0)
         return String();

      return envelopeSender.Mid(at + 1);
   }

   FqdnCheck::Decision
   FqdnCheck::Decide(bool refuseHelo, bool refuseSender, const Session &session)
   {
      Decision decision;

      // Off, nothing is judged and nothing is computed: the server as it was.
      if (!refuseHelo && !refuseSender)
         return decision;

      // The sessions the item says are never refused.
      if (session.authenticated || !session.spamTestsRun || session.incomingRelay)
         return decision;

      if (refuseHelo)
      {
         const Verdict verdict = Judge(session.helo);

         if (verdict != VerdictFqdn)
         {
            decision.limb = LimbHelo;
            decision.verdict = verdict;
            return decision;
         }
      }

      // The null reverse-path has no domain, so this limb has nothing to judge;
      // the HELO limb above has judged the HELO all the same. A sender that is
      // not null always has an @ by here - MAIL FROM refuses an address that is
      // not one - and a sender that somehow had none would have an empty
      // domain, which is not an FQDN either.
      if (refuseSender && !session.envelopeSender.IsEmpty())
      {
         const Verdict verdict = Judge(SenderDomain(session.envelopeSender));

         if (verdict != VerdictFqdn)
         {
            decision.limb = LimbSender;
            decision.verdict = verdict;
         }
      }

      return decision;
   }

   namespace
   {
      void ExpectVerdict(const String &name, FqdnCheck::Verdict expected)
      {
         const FqdnCheck::Verdict actual = FqdnCheck::Judge(name);

         if (actual != expected)
         {
            AnsiString text = name;
            throw std::logic_error(std::string("FqdnCheck::Judge(\"") + text.c_str() + "\") answered " +
                                   std::to_string((int) actual) + ", not " + std::to_string((int) expected) + ".");
         }
      }

      FqdnCheck::Session Unauthenticated(const String &helo, const String &sender)
      {
         FqdnCheck::Session session;
         session.helo = helo;
         session.envelopeSender = sender;
         return session;
      }

      void ExpectDecision(const char *what, const FqdnCheck::Decision &decision, FqdnCheck::Limb limb)
      {
         if (decision.limb != limb)
            throw std::logic_error(std::string("FqdnCheck::Decide: ") + what + " answered limb " +
                                   std::to_string((int) decision.limb) + ", not " + std::to_string((int) limb) + ".");
      }
   }

   void
   FqdnCheckTester::Test()
   {
      // Names, each a host's: two labels and more, under a public suffix of one
      // label, of two, of the PRIVATE section and of an exception rule; case;
      // the root dot; a hyphen, an underscore and a number inside a name; and an
      // internationalised label.
      ExpectVerdict(_T("mail.example.com"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("example.com"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("mail.co.uk"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("MAIL.Example.CO.UK"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("mail.example.com."), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("  mx1.example.com  "), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("tenant.github.io"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("www.ck"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("mail-1.example.com"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("exchange_01.corp.example"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("192-0-2-7.example.net"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("xn--bcher-kva.example"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("mail.b\u00FCcher.example"), FqdnCheck::VerdictFqdn);

      // The item's own: an address, with and without brackets, in either family.
      ExpectVerdict(_T("192.0.2.7"), FqdnCheck::VerdictAddress);
      ExpectVerdict(_T("192.0.2.7."), FqdnCheck::VerdictAddress);
      ExpectVerdict(_T("[192.0.2.7]"), FqdnCheck::VerdictAddressLiteral);
      ExpectVerdict(_T("2001:db8::7"), FqdnCheck::VerdictAddress);
      ExpectVerdict(_T("::1"), FqdnCheck::VerdictAddress);
      ExpectVerdict(_T("[IPv6:2001:db8::7]"), FqdnCheck::VerdictAddressLiteral);
      ExpectVerdict(_T("[2001:db8::7]"), FqdnCheck::VerdictAddressLiteral);
      ExpectVerdict(_T("[mail.example.com]"), FqdnCheck::VerdictAddressLiteral);

      // Empty, the root alone, and one label.
      ExpectVerdict(_T(""), FqdnCheck::VerdictEmpty);
      ExpectVerdict(_T("   "), FqdnCheck::VerdictEmpty);
      ExpectVerdict(_T("."), FqdnCheck::VerdictEmpty);
      ExpectVerdict(_T("localhost"), FqdnCheck::VerdictSingleLabel);
      ExpectVerdict(_T("localhost."), FqdnCheck::VerdictSingleLabel);
      ExpectVerdict(_T("example"), FqdnCheck::VerdictSingleLabel);
      ExpectVerdict(_T("com"), FqdnCheck::VerdictSingleLabel);

      // A public suffix used as a host: the ICANN section's of two labels, the
      // PRIVATE section's, and a wildcard rule's (*.ck makes foo.ck a suffix and
      // !www.ck above takes www.ck out of it). The 6.3.5 rule this must not
      // become: 2.7, the last two labels of an address, is no suffix to judge.
      ExpectVerdict(_T("co.uk"), FqdnCheck::VerdictPublicSuffix);
      ExpectVerdict(_T("CO.UK."), FqdnCheck::VerdictPublicSuffix);
      ExpectVerdict(_T("github.io"), FqdnCheck::VerdictPublicSuffix);
      ExpectVerdict(_T("foo.ck"), FqdnCheck::VerdictPublicSuffix);

      // The same of a PRIVATE wildcard: *.compute-1.amazonaws.com makes the
      // name an EC2 instance of us-east-1 is given a suffix of its own, so a
      // host that greets with it is refused (docs/SmtpPolicy.md says so and
      // what to do); an instance of another region has a label more and is a
      // name.
      ExpectVerdict(_T("ec2-198-51-100-7.compute-1.amazonaws.com"), FqdnCheck::VerdictPublicSuffix);
      ExpectVerdict(_T("ec2-198-51-100-7.eu-west-1.compute.amazonaws.com"), FqdnCheck::VerdictFqdn);

      // An internationalised suffix of two labels, in the form DNS carries it
      // and in the form an SMTPUTF8 sender's domain can, and a name under each.
      // Written with \u escapes, as every literal beyond ASCII in this file
      // must be: the file has no byte-order mark and the Windows build does
      // not pass /utf-8, so MSVC would read raw UTF-8 bytes in the ANSI code
      // page and compile other characters than GCC does.
      ExpectVerdict(_T("xn--55qx5d.cn"), FqdnCheck::VerdictPublicSuffix);
      ExpectVerdict(_T("\u516C\u53F8.cn"), FqdnCheck::VerdictPublicSuffix);
      ExpectVerdict(_T("example.xn--55qx5d.cn"), FqdnCheck::VerdictFqdn);
      ExpectVerdict(_T("example.\u516C\u53F8.cn"), FqdnCheck::VerdictFqdn);

      // Not a name at all: an empty label, a character no host name holds, two
      // words, and a top label of digits that does not make an address.
      ExpectVerdict(_T(".example.com"), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("mail..example.com"), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("mail.example.com.."), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("mail.example.com extra"), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("user@mail.example.com"), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("mail.example.com]"), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("IPv6:2001:db8::7"), FqdnCheck::VerdictNotAHostName);
      ExpectVerdict(_T("192.0.2"), FqdnCheck::VerdictNumericTopLabel);
      ExpectVerdict(_T("192.0.2.300"), FqdnCheck::VerdictNumericTopLabel);
      ExpectVerdict(_T("mail.example.123"), FqdnCheck::VerdictNumericTopLabel);

      // The sender's domain is what follows its last @.
      if (FqdnCheck::SenderDomain(_T("user@[192.0.2.7]")) != _T("[192.0.2.7]") ||
          FqdnCheck::SenderDomain(_T("\"a@b\"@example.com")) != _T("example.com") ||
          !FqdnCheck::SenderDomain(_T("")).IsEmpty() ||
          !FqdnCheck::SenderDomain(_T("postmaster")).IsEmpty())
         throw std::logic_error("FqdnCheck::SenderDomain did not give the text after the last @.");

      // Every verdict has words, and none of them is empty.
      for (int verdict = FqdnCheck::VerdictFqdn; verdict <= FqdnCheck::VerdictPublicSuffix; verdict++)
      {
         if (FqdnCheck::Describe((FqdnCheck::Verdict) verdict).IsEmpty())
            throw std::logic_error("FqdnCheck::Describe has no words for a verdict.");
      }

      const String goodHelo = _T("mail.example.com");
      const String goodSender = _T("user@example.com");

      // Both switches off: nothing is refused, however wrong both names are.
      ExpectDecision("both switches off", FqdnCheck::Decide(false, false, Unauthenticated(_T("192.0.2.7"), _T("user@192.0.2.7"))), FqdnCheck::LimbNone);

      // The HELO limb: refused, with its verdict; and the same session passes
      // with the HELO a name. The negative control for every refusal is the
      // same call with the one name corrected.
      FqdnCheck::Decision heloRefused = FqdnCheck::Decide(true, false, Unauthenticated(_T("[192.0.2.7]"), goodSender));
      ExpectDecision("a bracketed address HELO", heloRefused, FqdnCheck::LimbHelo);
      if (heloRefused.verdict != FqdnCheck::VerdictAddressLiteral)
         throw std::logic_error("FqdnCheck::Decide did not carry the HELO's verdict.");

      ExpectDecision("a localhost HELO", FqdnCheck::Decide(true, false, Unauthenticated(_T("localhost"), goodSender)), FqdnCheck::LimbHelo);
      ExpectDecision("a co.uk HELO", FqdnCheck::Decide(true, false, Unauthenticated(_T("co.uk"), goodSender)), FqdnCheck::LimbHelo);
      ExpectDecision("a good HELO", FqdnCheck::Decide(true, false, Unauthenticated(goodHelo, goodSender)), FqdnCheck::LimbNone);
      ExpectDecision("a mail.co.uk HELO", FqdnCheck::Decide(true, false, Unauthenticated(_T("mail.co.uk"), goodSender)), FqdnCheck::LimbNone);

      // The HELO switch alone says nothing about the sender.
      ExpectDecision("the HELO switch and a bad sender", FqdnCheck::Decide(true, false, Unauthenticated(goodHelo, _T("user@192.0.2.7"))), FqdnCheck::LimbNone);

      // The sender limb: an address with and without brackets, IPv6, a single
      // label, a public suffix; and a good domain.
      ExpectDecision("a sender at an address", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@192.0.2.7"))), FqdnCheck::LimbSender);
      ExpectDecision("a sender at an address literal", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@[192.0.2.7]"))), FqdnCheck::LimbSender);
      ExpectDecision("a sender at an IPv6 literal", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@[IPv6:2001:db8:0:0:0:0:0:7]"))), FqdnCheck::LimbSender);
      ExpectDecision("a sender at localhost", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@localhost"))), FqdnCheck::LimbSender);
      ExpectDecision("a sender at co.uk", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@co.uk"))), FqdnCheck::LimbSender);
      ExpectDecision("an SMTPUTF8 sender at an internationalised suffix", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@\u516C\u53F8.cn"))), FqdnCheck::LimbSender);
      ExpectDecision("an SMTPUTF8 sender under an internationalised suffix", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@example.\u516C\u53F8.cn"))), FqdnCheck::LimbNone);
      ExpectDecision("a sender at mail.co.uk", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T("user@mail.co.uk"))), FqdnCheck::LimbNone);
      ExpectDecision("a good sender", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, goodSender)), FqdnCheck::LimbNone);

      // The sender switch alone says nothing about the HELO.
      ExpectDecision("the sender switch and a bad HELO", FqdnCheck::Decide(false, true, Unauthenticated(_T("localhost"), goodSender)), FqdnCheck::LimbNone);

      // The null sender: no domain, so the sender limb has nothing to judge...
      ExpectDecision("a null sender, sender switch", FqdnCheck::Decide(false, true, Unauthenticated(goodHelo, _T(""))), FqdnCheck::LimbNone);
      ExpectDecision("a null sender and a bad HELO, sender switch", FqdnCheck::Decide(false, true, Unauthenticated(_T("192.0.2.7"), _T(""))), FqdnCheck::LimbNone);

      // ...and it is no pass for the HELO.
      ExpectDecision("a null sender and a bad HELO, HELO switch", FqdnCheck::Decide(true, false, Unauthenticated(_T("192.0.2.7"), _T(""))), FqdnCheck::LimbHelo);
      ExpectDecision("a null sender and a bad HELO, both", FqdnCheck::Decide(true, true, Unauthenticated(_T("192.0.2.7"), _T(""))), FqdnCheck::LimbHelo);
      ExpectDecision("a null sender and a good HELO, both", FqdnCheck::Decide(true, true, Unauthenticated(goodHelo, _T(""))), FqdnCheck::LimbNone);

      // Both wrong: the HELO is judged first.
      ExpectDecision("both names wrong", FqdnCheck::Decide(true, true, Unauthenticated(_T("localhost"), _T("user@localhost"))), FqdnCheck::LimbHelo);
      ExpectDecision("the sender wrong, both switches", FqdnCheck::Decide(true, true, Unauthenticated(goodHelo, _T("user@localhost"))), FqdnCheck::LimbSender);

      // The carve-outs, one at a time, with both switches on and both names
      // wrong: each alone is enough, and the session without it is refused.
      FqdnCheck::Session wrong = Unauthenticated(_T("192.0.2.7"), _T("user@192.0.2.7"));
      ExpectDecision("the carve-outs' control", FqdnCheck::Decide(true, true, wrong), FqdnCheck::LimbHelo);

      FqdnCheck::Session authenticated = wrong;
      authenticated.authenticated = true;
      ExpectDecision("an authenticated session", FqdnCheck::Decide(true, true, authenticated), FqdnCheck::LimbNone);

      FqdnCheck::Session untested = wrong;
      untested.spamTestsRun = false;
      ExpectDecision("a session the spam tests do not run for", FqdnCheck::Decide(true, true, untested), FqdnCheck::LimbNone);

      FqdnCheck::Session relay = wrong;
      relay.incomingRelay = true;
      ExpectDecision("an incoming relay", FqdnCheck::Decide(true, true, relay), FqdnCheck::LimbNone);
   }
}
