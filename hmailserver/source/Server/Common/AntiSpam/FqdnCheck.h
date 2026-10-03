// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Whether the name a session gives for itself is a fully qualified domain name,
// and the refusal RefuseHeloNotFqdn and RefuseMailFromDomainNotFqdn make with it
// at MAIL FROM (GitLab issue 51, docs/SmtpPolicy.md).
//
// No DNS is asked anything. A name is judged by its shape and by the Public
// Suffix List compiled into the server - QuarantineReview::IsPublicSuffix, the
// test the quarantine's review makes before it writes a suffix on the domain
// list. A name is an FQDN here when, with one trailing root dot taken off
// (mail.example.com. is mail.example.com written absolutely):
//
//   - it is not empty, and not the root "." alone;
//   - it is not an address: nothing in brackets ([192.0.2.7], [IPv6:2001:db8::7]
//     and whatever else a client puts in them), and no IPv4 or IPv6 address
//     written without them. An IPv4 address is four decimal parts (192.0.2.7),
//     as IPAddress reads one on every platform; the older forms Windows' own
//     parser also reads - 192.0.2, 3232235777, 0x7f.1, 0300.0250.0.1 - are not
//     addresses, and are judged by the rules below as the names they then are;
//   - every label holds something, and only letters, digits, hyphens and
//     underscores, or characters beyond ASCII (an internationalised name under
//     SMTPUTF8). A space, a colon, an @ or a bracket is in no host's name, and a
//     HELO of "mail.example.com extra" is not one name;
//   - it has two labels or more: localhost is not an FQDN;
//   - its last label is not all digits, since no top-level domain is (RFC 3696
//     section 2): 192.0.2 and 192.0.2.300 are not names, and not addresses either;
//   - it is not itself a public suffix: com, co.uk, github.io. A name that IS the
//     suffix names no host and no organization; mail.co.uk is an FQDN, co.uk is
//     not.
//
// Underscores and a label that begins or ends with a hyphen are let through:
// both are outside the host-name grammar and both are on real mail servers'
// HELO lines, and this check is about names that are not names at all, not
// about spelling.
//
// The refusal is a protocol-shaped one, like the multi-recipient null sender's,
// not a score. It is judged at MAIL FROM because AUTH has had its chance by
// then; at HELO a submission client that greets with whatever its stack puts
// there and then authenticates would be refused. It never judges:
//
//   - an authenticated session;
//   - a session the spam tests do not run for - an IP range with spam
//     protection off, or a sender the anti-spam white list covers;
//   - a session from an incoming relay, whose HELO is this organisation's own
//     gateway's and not the originator's, and whose envelope a refusal here
//     would turn into a bounce from that gateway rather than a refusal to the
//     sender. The gateway is where the check belongs for mail behind one.
//
// MAIL FROM:<> does not excuse a bad HELO: the null reverse-path has no domain,
// so the sender's limb has nothing to judge, and the HELO limb judges the HELO
// as it would for any sender.
//
// It needs no database, no configuration and no network, so FqdnCheckTester
// runs inside the service with the other class tests (ClassTester) and on
// Linux in public-suffix-tests, beside the list it reads.

#pragma once

namespace HM
{
   class FqdnCheck
   {
   public:
      enum Verdict
      {
         VerdictFqdn = 0,
         VerdictEmpty,
         VerdictAddressLiteral,
         VerdictAddress,
         VerdictNotAHostName,
         VerdictSingleLabel,
         VerdictNumericTopLabel,
         VerdictPublicSuffix
      };

      // What a name is, by the rules above. Case does not matter.
      static Verdict Judge(const String &name);

      // The verdict as the words after "it is", for the reply and the log line:
      // "an address literal", "a public suffix".
      static String Describe(Verdict verdict);

      // Which of the two names refused the session.
      enum Limb
      {
         LimbNone = 0,
         LimbHelo,
         LimbSender
      };

      // What the connection knows at MAIL FROM.
      struct Session
      {
         // AUTH succeeded on this session (or a trusted XCLIENT said it had).
         bool authenticated = false;

         // The spam tests run for this session: its IP range runs them and the
         // anti-spam white list does not cover its sender.
         bool spamTestsRun = true;

         // The connecting address is one of the incoming relays.
         bool incomingRelay = false;

         // The HELO/EHLO name as the client gave it, and the envelope sender
         // as MAIL FROM gave it - empty for the null reverse-path.
         String helo;
         String envelopeSender;
      };

      struct Decision
      {
         Limb limb = LimbNone;
         Verdict verdict = VerdictFqdn;
      };

      // Whether the session is refused, and for which name. The HELO is judged
      // first, so a session wrong in both is refused for its HELO. LimbNone
      // when neither switch is on, for every session the rules above never
      // judge, and when every name judged is an FQDN.
      static Decision Decide(bool refuseHelo, bool refuseSender, const Session &session);

      // The domain of an envelope sender: the text after its last @, or empty
      // when it has none.
      static String SenderDomain(const String &envelopeSender);
   };

   // FqdnCheck held to the item's own examples and to the edges of each rule:
   // addresses with and without brackets, IPv6 in both forms, the root, a
   // single label, public suffixes of the ICANN and PRIVATE sections, the
   // wildcard and exception rules and an internationalised suffix in its ASCII
   // and its Unicode form, names under them, case, the root dot, empty
   // labels, characters no name holds, an all-digit top label; and Decide's
   // carve-outs and switches one at a time, the null sender with a bad HELO
   // and with a good one, and the HELO limb judged before the sender's. Throws
   // std::logic_error naming the first thing that is wrong.
   class FqdnCheckTester
   {
   public:
      void Test();
   };
}
