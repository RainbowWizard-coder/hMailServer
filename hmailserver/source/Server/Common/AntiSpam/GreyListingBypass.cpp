// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "GreyListingBypass.h"
#include "AntiSpamConfiguration.h"
#include "SpamTestResult.h"
#include "SpamTestSPF.h"
#include "DMARC/SpamTestDMARC.h"
#include "../BO/GreyListingBypassEntry.h"
#include "../BO/GreyListingBypassEntries.h"
#include "../../SMTP/SPF/SPF.h"

#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/locks.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   boost::shared_mutex greyListingBypassAccessMutex_;

   std::vector<std::shared_ptr<GreyListingBypassEntry> > GreyListingBypass::entries_;
   bool GreyListingBypass::needRefresh_ = true;

   void
   GreyListingBypass::SetNeedRefresh()
   {
      needRefresh_ = true;
   }

   void
   GreyListingBypass::Refresh_()
   {
      GreyListingBypassEntries entries;
      if (!entries.Refresh())
         return;

      // owned by this call: entries is the local collection just loaded; the cache's copy is guarded by the cache's own lock
      entries_ = entries.GetConstVector();
      needRefresh_ = false;
   }

   std::vector<std::shared_ptr<GreyListingBypassEntry> >
   GreyListingBypass::Entries_()
   {
      boost::upgrade_lock<boost::shared_mutex> lock(greyListingBypassAccessMutex_);

      if (needRefresh_)
      {
         boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
         Refresh_();
      }

      return entries_;
   }

   bool
   GreyListingBypass::PassedFor_(const std::set<std::shared_ptr<SpamTestResult> > &results, const String &testName,
                                 const GreyListingBypassEntry &entry)
   {
      for (std::shared_ptr<SpamTestResult> result : results)
      {
         if (result->GetTestName() != testName || result->GetResult() != SpamTestResult::Pass)
            continue;

         // The identities the test authenticated. A pass that carries none - a
         // test that has not been taught to say whose result it is - satisfies
         // nothing here, deliberately: the entry names a domain, and a result
         // that cannot be attributed to that domain is not evidence about it.
         for (const String &identity : result->GetIdentityDomains())
         {
            if (entry.Covers(identity))
               return true;
         }
      }

      return false;
   }

   bool
   GreyListingBypass::HeloIdentityPasses_(const String &heloHost, const IPAddress &originatingIP)
   {
      if (heloHost.IsEmpty() || originatingIP.IsAny())
         return false;

      // An empty sender is how check_host() is asked about the HELO identity:
      // SPF::GetCheckedDomain answers the HELO argument for one, and the
      // evaluator stands postmaster at that host in for the sender (RFC 7208
      // sections 2.4 and 4.3), which is the section 2.3 check exactly. One
      // lookup, and only for a row that asked for it.
      String explanation;
      return SPF::Instance()->Test(originatingIP.ToString(), String(), heloHost, explanation) == SPF::Result::Pass;
   }

   GreyListingBypass::Outcome
   GreyListingBypass::Evaluate(const String &envelopeFrom, const String &heloHost, const IPAddress &originatingIP,
                               const std::set<std::shared_ptr<SpamTestResult> > &results,
                               bool afterData, String &matchedPattern)
   {
      matchedPattern.Empty();

      std::vector<std::shared_ptr<GreyListingBypassEntry> > entries = Entries_();
      if (entries.empty())
         return NoMatch;

      const String senderDomain = StringParser::ExtractDomain(envelopeFrom);

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();
      const bool dkimPossible = config.GetDKIMVerificationEnabled();
      const bool dmarcPossible = config.GetDMARCEnabled();
      const bool spfPossible = config.GetUseSPF();

      // The HELO identity's own SPF verdict, asked at most once for a message
      // and only when a row that matched on the HELO wants SPF: -1 not asked,
      // 0 no, 1 yes. It is not asked at all while SPF is switched off, so that
      // "the test is off, the row can never be satisfied" holds for SPF as it
      // does for DKIM and DMARC, and a server with SPF off makes no lookup.
      int heloIdentity = -1;

      Outcome outcome = NoMatch;
      String pendingPattern;

      for (std::shared_ptr<GreyListingBypassEntry> entry : entries)
      {
         const int matchOn = entry->GetMatchOn();
         const bool coversEnvelope = (matchOn == GreyListingBypassEntry::MatchEnvelope || matchOn == GreyListingBypassEntry::MatchBoth) && entry->Covers(senderDomain);
         const bool coversHelo = (matchOn == GreyListingBypassEntry::MatchHelo || matchOn == GreyListingBypassEntry::MatchBoth) && entry->Covers(heloHost);

         if (!coversEnvelope && !coversHelo)
            continue;

         const int require = entry->GetRequire();
         const bool wantsSpf = (require & GreyListingBypassEntry::RequireSpf) != 0;
         const bool wantsDkim = (require & GreyListingBypassEntry::RequireDkim) != 0;
         const bool wantsDmarc = (require & GreyListingBypassEntry::RequireDmarc) != 0;

         // SPF is decided by now whichever phase this is: a row that wants it and
         // has not got it is not this message's row.
         //
         // "Got it" means a pass for a name THIS row covers. The check that has
         // already run is the mailfrom one, and it authenticates the envelope
         // sender's domain: for a row matched on the HELO that is somebody
         // else's name, and a pass for evil.test would otherwise satisfy a row
         // reading .mail.partner.test the moment a client said HELO
         // gw1.mail.partner.test - free text, authenticated by nothing. What
         // authenticates the HELO name is a check made for the HELO identity,
         // RFC 7208 section 2.3, so that is what such a row is answered with.
         if (wantsSpf)
         {
            bool spfSatisfied = PassedFor_(results, SpamTestSPF::GetTestName(), *entry);

            if (!spfSatisfied && coversHelo && spfPossible)
            {
               if (heloIdentity < 0)
                  heloIdentity = HeloIdentityPasses_(heloHost, originatingIP) ? 1 : 0;

               spfSatisfied = heloIdentity == 1;
            }

            if (!spfSatisfied)
               continue;
         }

         if (!wantsDkim && !wantsDmarc)
         {
            matchedPattern = entry->GetPattern();
            return Satisfied;
         }

         if (afterData)
         {
            // Both of these must be the row's own domain's, as SPF must:
            // DKIM's identity is each d= that verified - not necessarily the
            // sender's domain, since anybody can sign a message with a key
            // they publish themselves - and DMARC's is the RFC5322.From
            // domain, which the envelope need not agree with at all.
            // SpamTestDKIM has no static name of its own; the string is the
            // one its GetName answers.
            const bool dkimSatisfied = !wantsDkim || PassedFor_(results, _T("SpamTestDKIM"), *entry);
            const bool dmarcSatisfied = !wantsDmarc || PassedFor_(results, SpamTestDMARC::GetTestName(), *entry);

            if (dkimSatisfied && dmarcSatisfied)
            {
               matchedPattern = entry->GetPattern();
               return Satisfied;
            }

            continue;
         }

         // Before DATA, with DKIM or DMARC wanted. Worth waiting for only when the
         // test that would answer it is going to run at all.
         if ((wantsDkim && !dkimPossible) || (wantsDmarc && !dmarcPossible))
            continue;

         outcome = NeedsData;
         if (pendingPattern.IsEmpty())
            pendingPattern = entry->GetPattern();
      }

      if (outcome == NeedsData)
         matchedPattern = pendingPattern;

      return outcome;
   }
}
