// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <set>
#include <vector>

namespace HM
{
   class SpamTestResult;
   class GreyListingBypassEntry;
   class IPAddress;

   // The per-domain greylisting bypass (discussion #284): whether a message from
   // this envelope sender and HELO host has earned, by the authentication results
   // it carries, the right to skip greylisting that a row of hm_greylisting_bypass
   // grants it.
   //
   // Asked twice for one message when it has to be. SPF is known at RCPT TO, where
   // greylisting decides; DKIM and DMARC exist only once the message has been
   // received, so a row that requires either cannot be judged there. Such a row
   // answers NeedsData at RCPT TO, and SMTPConnection lets the recipient through
   // for now and asks again after DATA with the post-transmission results in
   // hand, deferring or refusing the whole message then - a 451 after DATA being
   // the same thing the empty-sender delay has always done. A row requiring DKIM
   // while DKIM verification is off, or DMARC while DMARC is off, is never
   // NeedsData: it can never be satisfied, so the message is greylisted at RCPT TO
   // as if the row were not there, rather than received first and greylisted after.
   //
   // A required result must belong to a name the row covers. That is the whole of
   // the rule, and without it the list grants a privilege on somebody else's
   // authentication: SPF authenticates the envelope sender's domain (or the HELO
   // host where there is no envelope sender), DKIM each d= that verified, DMARC
   // the RFC5322.From domain - three identities that a message may state
   // differently, and a bare "it passed" cannot tell them apart. A row matched on
   // the HELO and requiring SPF is answered by a check of the HELO identity of its
   // own (RFC 7208 section 2.3), since the mailfrom check says nothing about the
   // name the client gave.
   class GreyListingBypass
   {
   public:
      enum Outcome
      {
         // No row covers the sender, or none that does can be satisfied.
         NoMatch = 0,
         // A row covers the sender and every result it requires is a pass.
         Satisfied = 1,
         // A row covers the sender, its SPF requirement (if any) holds, and it
         // requires DKIM or DMARC, which are not known until after DATA.
         NeedsData = 2
      };

      // envelopeFrom is MAIL FROM's address (its domain is what an envelope row
      // is matched against), heloHost the HELO/EHLO name, originatingIP the
      // connecting address (the HELO identity's SPF check needs it), results the
      // spam test results gathered so far, afterData whether the
      // post-transmission tests have run. matchedPattern names the row that
      // decided, for the log.
      static Outcome Evaluate(const String &envelopeFrom, const String &heloHost, const IPAddress &originatingIP,
                              const std::set<std::shared_ptr<SpamTestResult> > &results,
                              bool afterData, String &matchedPattern);

      // The list is read into memory once and re-read after a save or a delete
      // through any interface (PersistentGreyListingBypassEntry calls this), in
      // BlockedSenderCache's shape: a read that fails keeps what is held and
      // tries again on the next message, so an empty list is never a failed one.
      // On a server with no entries the whole of the cost is one read at start.
      static void SetNeedRefresh();

   private:
      // Whether that test passed FOR a name this entry covers. A pass whose
      // identity the entry does not cover, and a pass that carries no identity
      // at all, are both "not this entry's".
      static bool PassedFor_(const std::set<std::shared_ptr<SpamTestResult> > &results, const String &testName,
                             const GreyListingBypassEntry &entry);
      // check_host() for the HELO identity, RFC 7208 section 2.3. Asked at most
      // once per message, and only for a row that matched on the HELO and wants
      // SPF.
      static bool HeloIdentityPasses_(const String &heloHost, const IPAddress &originatingIP);
      static std::vector<std::shared_ptr<GreyListingBypassEntry> > Entries_();
      static void Refresh_();

      static std::vector<std::shared_ptr<GreyListingBypassEntry> > entries_;
      static bool needRefresh_;
   };
}
