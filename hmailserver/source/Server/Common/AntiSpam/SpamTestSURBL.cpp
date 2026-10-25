// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "SpamTestSURBL.h"

#include "SpamTestData.h"
#include "SpamTestResult.h"

#include "SURBL.h"

#include "../BO/SURBLServers.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   String 
   SpamTestSURBL::GetName() const
   {
      return "SpamTestSURBL";
   }

   bool 
   SpamTestSURBL::GetIsEnabled()
   {
      std::shared_ptr<SURBLServers> pSURBLServers = Configuration::Instance()->GetAntiSpamConfiguration().GetSURBLServers();

      std::vector<std::shared_ptr<SURBLServer> > vec = pSURBLServers->GetSnapshot();
      auto iter = vec.begin();
      auto iterEnd = vec.end();

      for (; iter != iterEnd; iter++)
      {
         std::shared_ptr<SURBLServer> pSURBLServer = (*iter);

         if (pSURBLServer->GetIsActive()) 
         {
            return true;
         }
      }

      return false;
   }

   std::set<std::shared_ptr<SpamTestResult> >
   SpamTestSURBL::RunTest(std::shared_ptr<SpamTestData> pTestData)
   {
      std::set<std::shared_ptr<SpamTestResult> > setSpamTestResults;

      std::shared_ptr<MessageData> pMessageData = pTestData->GetMessageData();
      std::shared_ptr<SURBLServers> pSURBLServers = Configuration::Instance()->GetAntiSpamConfiguration().GetSURBLServers();

      SURBL surblTester;

      SURBL::Choice choice;
      if (surblTester.ExtractUrls(pMessageData, choice))
      {
         for (std::shared_ptr<SURBLServer> pSURBLServer : pSURBLServers->GetSnapshot())
         {
            if (pSURBLServer->GetIsActive())
            {
               if (!surblTester.Run(pSURBLServer, choice.names))
               {
                  // Blocked
                  int iSomeScore = pSURBLServer->GetScore();
                  std::shared_ptr<SpamTestResult> pResult = std::shared_ptr<SpamTestResult>(new SpamTestResult(GetName(), SpamTestResult::Fail, iSomeScore, pSURBLServer->GetRejectMessage()));

                  setSpamTestResults.insert(pResult);
               }
            }
         }

         // A message whose links name more domains than the lists are asked
         // about (SURBL::MaxDomainsAsked) is not asked about whole: once, however
         // many lists there are, it scores what padding a message with made-up
         // domains, to hide a listed one among them, is to cost. A fixed number
         // of names drawn from all of them - 48, in the 6.3.6 tree before its
         // release - left a listed link beside N made-up domains asked about in
         // 48 of N+1 messages, and cost the sender nothing.
         if (!choice.EveryDomainAsked())
         {
            const int score = Configuration::Instance()->GetAntiSpamConfiguration().GetSurblTooManyDomainsScore();
            if (score != 0)
            {
               const String reason = Formatter::Format(_T("The links name {0} domains, more than the {1} the URI blocklists are asked about"),
                  (unsigned __int64) choice.domains, (unsigned __int64) SURBL::MaxDomainsAsked);

               setSpamTestResults.insert(std::shared_ptr<SpamTestResult>(new SpamTestResult(GetName(), SpamTestResult::Fail, score, reason)));
            }
         }
      }

      return setSpamTestResults;
   }

}