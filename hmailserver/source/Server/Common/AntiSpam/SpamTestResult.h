// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <vector>

namespace HM
{
   class SpamTestResult
   {
   public:

      enum Result
      {
         Neutral = 0,
         Fail = 1,
         Pass = 2,

         // The test could not finish and the transaction should be answered
         // 451 rather than accepted on a guess: SPF's TempError with
         // SPFTempErrorDefer on. Carries no score - a deferred message is never
         // accepted, so there is nothing to mark - and is not a Fail, so the
         // score-threshold refusal and the X-hMailServer-Reason header never
         // read it as one. SMTPConnection::DoSpamProtection_ answers it.
         Defer = 3
      };

      SpamTestResult(const String &testName, Result result, int iSpamScore, const String &sMessage);
      virtual ~SpamTestResult();

      Result GetResult() {return result_; }
      int GetSpamScore() {return spam_score_; }
      String GetMessage() {return message_; }
      String GetTestName() {return testName_; }

      // Whose result this is: the domains the test actually authenticated. SPF
      // records the identity it was evaluated for (RFC 7208 section 2.4), DKIM
      // every d= whose signature verified, DMARC the RFC5322.From domain. A
      // test that authenticates no particular name records none, and a caller
      // that asks whether a result belongs to a domain must read an empty list
      // as "not that domain's": a pass with no identity behind it says
      // something verified but not whose, which is the difference between
      // "this message is from the partner" and "this message is signed".
      void AddIdentityDomain(const String &domain);
      const std::vector<String> &GetIdentityDomains() const { return identity_domains_; }

   private:

      int spam_score_;
      String message_;
      String testName_;
      Result result_;
      std::vector<String> identity_domains_;

   };

}