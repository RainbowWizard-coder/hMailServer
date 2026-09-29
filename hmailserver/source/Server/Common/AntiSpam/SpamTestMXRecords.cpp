// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SpamTestMXRecords.h"

#include "SpamTestData.h"
#include "SpamTestResult.h"

#include "AntiSpamConfiguration.h"

#include "../TCPIP/DNSResolver.h"
#include "DMARC/PublicSuffixList.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   String 
   SpamTestMXRecords::GetName() const
   {
      return "SpamTestMXRecords";
   }

   bool 
   SpamTestMXRecords::GetIsEnabled()
   {
      if (Configuration::Instance()->GetAntiSpamConfiguration().GetUseMXChecks())
         return true;
      else
         return false;
   }


   std::set<std::shared_ptr<SpamTestResult> >
   SpamTestMXRecords::RunTest(std::shared_ptr<SpamTestData> pTestData)
   {
      std::set<std::shared_ptr<SpamTestResult> > setSpamTestResults;


      String sEnvelopeFrom = pTestData->GetEnvelopeFrom();

      if (!sEnvelopeFrom.IsEmpty() && !HasAnyMXRecords_(sEnvelopeFrom))
      {
         // Blocked by SPF.
         String sMessage = _T("Sender domain does not have any MX records.");
         int iScore = Configuration::Instance()->GetAntiSpamConfiguration().GetUseMXChecksScore();;

         std::shared_ptr<SpamTestResult> pResult = std::shared_ptr<SpamTestResult>(new SpamTestResult(GetName(), SpamTestResult::Fail, iScore, sMessage));
         setSpamTestResults.insert(pResult);   

      }

      return setSpamTestResults;
   }

   bool 
   SpamTestMXRecords::HasAnyMXRecords_(const String &sSenderEMail)
   {
      const String sHostName = StringParser::ExtractDomain(sSenderEMail);

      // Check if full host name has any MX records.
      std::vector<String> vecFoundNames;

      DNSResolver resolver;
      if (!resolver.GetMXRecords(sHostName, vecFoundNames))
      {
         // DNS query failed. Let's pretend he has MX records.
         return true;
      }

      // DNS query succeeded. Check if MX records exists.
      if (vecFoundNames.size() > 0)
         return true;

      // No MX records were found.

      // Extract the domain name from the host name. For example, replace
      // cgi.example.com with example.com, and mail.example.act.edu.au with
      // example.act.edu.au: the registrable domain by the Public Suffix List.
      bool bIsIPAddress = false;
      String sDomainName = sHostName;
      if (!PublicSuffixList::GetRegistrableDomain(sDomainName, bIsIPAddress))
      {
         // No registrable domain to fall back to. A name of two labels or more,
         // none of them empty, is then a public suffix itself - github.io, co.uk,
         // act.edu.au - with no registrant above it whose MX could answer for it,
         // and its own MX was asked about above and has none: it fails, as a
         // registered name without an MX does. Until 6.3.6 the cut knew only the
         // names in SURBL's list of 2006 for suffixes, so a sender at github.io
         // was cut to github.io and failed; passing every suffix instead would let
         // a sender at any of the list's thousands of names past this test. One
         // label, or a name with an empty label, is not judged, as before.
         String name = sHostName;
         name.TrimRight(_T("."));
         const std::vector<String> labels = StringParser::SplitString(name, _T("."));

         bool wellFormed = labels.size() >= 2;
         for (const String &label : labels)
         {
            if (label.IsEmpty())
               wellFormed = false;
         }

         return !wellFormed;
      }

      if (bIsIPAddress)
      {
         // Sender address contains an IP address. Skip MX checks.
         return true;
      }

      if (sDomainName == sHostName)
      {
         // Host name is same as domain name. We have already queried the host name
         // so there's no need to re-run this query.
         return false;
      }

      if (!resolver.GetMXRecords(sDomainName, vecFoundNames))
      {
         // DNS query failed. Let's pretend he has MX records.
         return true;
      }

      return (vecFoundNames.size() > 0);
   }


}