// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SpamProtection.h"

#include "../AntiSpam/WhiteListCache.h"
#include "../AntiSpam/SpamTestRunner.h"
#include "../AntiSpam/SpamTestData.h"
#include "../AntiSpam/AuthenticationResults.h"
#include "../AntiSpam/SpamTestResult.h"

#include "../AntiSpam/SpamTestSPF.h"

#include "../BO/Message.h"
#include "../BO/Domain.h"
#include "../BO/MessageData.h"

#include "../Cache/CacheContainer.h"

#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"

#include "../TCPIP/DNSResolver.h"
#include "../TCPIP/HostNameAndIpAddress.h"

#include "../Util/FileUtilities.h"

#include "../../SMTP/GreyListing.h"
#include "GreyListingBypass.h"

#include "DKIM/DKIM.h"

#include "AntiSpamConfiguration.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SpamProtection::SpamProtection(void)
   {

   }

   SpamProtection::~SpamProtection(void)
   {
   }

   void
   SpamProtection::Load()
   {
      // Put in place once its tests are loaded, so that no run finds a runner
      // part way through LoadSpamTests; the one it replaces is let go of here,
      // and freed when the last run still going through its tests returns.
      std::shared_ptr<SpamTestRunner> runner = std::shared_ptr<SpamTestRunner>(new SpamTestRunner);
      runner->LoadSpamTests();

#if defined(__cpp_lib_atomic_shared_ptr)
      spam_test_runner_.store(runner);
#else
      std::atomic_store(&spam_test_runner_, runner);
#endif

      DKIM::Initialize();
   }

   std::shared_ptr<SpamTestRunner>
   SpamProtection::CurrentRunner_() const
   {
#if defined(__cpp_lib_atomic_shared_ptr)
      return spam_test_runner_.load();
#else
      return std::atomic_load(&spam_test_runner_);
#endif
   }

   std::set<std::shared_ptr<SpamTestResult> >
   SpamProtection::RunPreTransmissionTests(const String &sFromAddress, 
                                           const IPAddress &iOriginatingIP,
                                           const IPAddress &iConnectingIP,
                                           const String &sHeloHost,
                                           std::shared_ptr<AuthenticationResults> authenticationResults)
   {

      std::shared_ptr<SpamTestData> pTestData = std::shared_ptr<SpamTestData>(new SpamTestData);

      pTestData->SetEnvelopeFrom(sFromAddress);
      pTestData->SetHeloHost(sHeloHost);
      pTestData->SetOriginatingIP(iOriginatingIP);
      pTestData->SetConnectingIP(iConnectingIP);
      pTestData->SetAuthenticationResults(authenticationResults);

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();
      int maxScore = std::max(config.GetSpamDeleteThreshold(), config.GetSpamMarkThreshold());

      // This run's own reference: a start of the servers while the tests run
      // puts a new runner in place and leaves this one to this run (the
      // member's comment).
      std::shared_ptr<SpamTestRunner> runner = CurrentRunner_();
      if (!runner)
         return std::set<std::shared_ptr<SpamTestResult> >();

      std::set<std::shared_ptr<SpamTestResult> > setResult = runner->RunSpamTest(pTestData, SpamTest::PreTransmission, maxScore);

      return setResult;
   }

   std::set<std::shared_ptr<SpamTestResult> >
   SpamProtection::RunPostTransmissionTests(const String &sFromAddress, 
                                            const IPAddress & iOriginatingIP,
                                            const IPAddress & iConnectingIP,
                                            std::shared_ptr<Message> pMessage,
                                            std::shared_ptr<AuthenticationResults> authenticationResults,
                                            const AnsiString &recordedReturnPath)
   {

      const String fileName = MessageStore::WorkingFile(pMessage);

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();

      // Limit scanning by message size (MaxSizeToScanKB).
      const int maxSizeToScanKB = MaxSizeToScanKB();

      int messageSizeKB = FileUtilities::FileSize(fileName) / 1024;
      if (messageSizeKB > maxSizeToScanKB)
      {
         // The message is larger than the max message size to scan, so we'll skip scanning it.
         std::set<std::shared_ptr<SpamTestResult> > emptySet;
         return emptySet;
      }

      std::shared_ptr<SpamTestData> pTestData = std::shared_ptr<SpamTestData>(new SpamTestData);      
      std::shared_ptr<MessageData> pMessageData = std::shared_ptr<MessageData>(new MessageData);
      
      pMessageData->LoadFromMessage(fileName, pMessage);

      pTestData->SetEnvelopeFrom(sFromAddress);
      pTestData->SetRecordedReturnPath(recordedReturnPath);
      pTestData->SetOriginatingIP(iOriginatingIP);
      pTestData->SetConnectingIP(iConnectingIP);
      pTestData->SetMessageData(pMessageData);
      pTestData->SetAuthenticationResults(authenticationResults);

      int maxScore = std::max(config.GetSpamDeleteThreshold(), config.GetSpamMarkThreshold());

      // As in RunPreTransmissionTests.
      std::shared_ptr<SpamTestRunner> runner = CurrentRunner_();
      if (!runner)
         return std::set<std::shared_ptr<SpamTestResult> >();

      std::set<std::shared_ptr<SpamTestResult> > setResult = runner->RunSpamTest(pTestData, SpamTest::PostTransmission, maxScore);

      return setResult;
   }

   int
   SpamProtection::MaxSizeToScanKB()
   {
      // The hard ceiling is the MIME parser's own limit (MessageData::LoadFromMessage
      // ignores messages over 80 MB): the tests that read the message parse it, and
      // above it there is nothing to read - SpamAssassin's verdict is read from the
      // message it returns. (Until 6.3.7 the SpamAssassin test also rewrote the
      // message after the scan, and above the limit that wrote an empty MIME body
      // over the original.) Messages this large are very unlikely to be spam anyway.
      const int maxParserSizeKB = 1024 * 80;

      const int configured = Configuration::Instance()->GetAntiSpamConfiguration().GetAntiSpamMaxSizeKB();
      return configured > 0 ? std::min(configured, maxParserSizeKB) : maxParserSizeKB;
   }

   SpamProtection::GreyListingVerdict
   SpamProtection::PerformGreyListing(std::shared_ptr<Message> message, const std::set<std::shared_ptr<SpamTestResult> > &spamTestResults, const String &toAddress, const IPAddress &ipaddress, const String &heloHost, bool afterData)
   {
      if (!Configuration::Instance()->GetAntiSpamConfiguration().GetUseGreyListing())
      {
         // The sender is either local, or grey listing is not enabled. We should
         // not do grey list checks in these cases.
         return GreyListAllow;
      }

      // Check if we should use grey listing for the recipient domain.
      String sRecipientDomain = StringParser::ExtractDomain(toAddress);
      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(sRecipientDomain);         

      if (pDomain && !pDomain->GetASUseGreyListing())
      {
         // Grey listing not enabled in domain. Skip it.
         return GreyListAllow;
      }


      // Check if the SPF test has succeeded. If so, maybe we should not do
      if (Configuration::Instance()->GetAntiSpamConfiguration().GetBypassGreyListingOnSPFSuccess())
      {
         for(std::shared_ptr<SpamTestResult> testResult : spamTestResults)
         {
            if (testResult->GetTestName() == SpamTestSPF::GetTestName())
            {
               if (testResult->GetResult() == SpamTestResult::Pass)
               {
                  // We should not run grey listing since the SPF test has passed
                  LOG_DEBUG("SPF passed, skipping greylisting.");
                  return GreyListAllow;
               }
            }
         }
      }

      // Check if the mail is coming from the domain A record or MX record. If so, bypass grey listing.
      if (Configuration::Instance()->GetAntiSpamConfiguration().GetBypassGreyListingOnMailFromMX())
      {
         String senderDomain = StringParser::ExtractDomain(message->GetFromAddress());
         
         if (senderDomain.GetLength() > 0)
         {
            std::vector<String> found_ip_addresses;
               
            DNSResolver resolver;
            resolver.GetIpAddresses(senderDomain, found_ip_addresses, false);

            std::vector<HostNameAndIpAddress> host_name_with_addresses;
            resolver.GetEmailServers(senderDomain, host_name_with_addresses);

            for(HostNameAndIpAddress host_and_ip : host_name_with_addresses)
               found_ip_addresses.push_back(host_and_ip.GetIpAddress());

            String actualFromAddress = ipaddress.ToString();
            for(String found_ip_address : found_ip_addresses)
            {
               if (found_ip_address.CompareNoCase(actualFromAddress) == 0)
               {
                  // The message is coming from either an A record or a MX record. Skip greylisting.
                  LOG_DEBUG("Mail coming from A or MX record. Skipping grey listing.");
                  return GreyListAllow;
               }
            }
         }
      }

      // The bypass list (discussion #284): a privilege granted to a named domain
      // or HELO suffix, on the authentication results the message carries. A row
      // that wants DKIM or DMARC cannot be judged before DATA; the recipient is
      // let through for now and this is asked again after DATA.
      {
         String matchedPattern;
         GreyListingBypass::Outcome bypass = GreyListingBypass::Evaluate(message->GetFromAddress(), heloHost, ipaddress, spamTestResults, afterData, matchedPattern);

         if (bypass == GreyListingBypass::Satisfied)
         {
            LOG_DEBUG("Greylisting bypass " + matchedPattern + " satisfied by the authentication results; skipping greylisting for " + toAddress + ".");
            return GreyListAllow;
         }

         if (bypass == GreyListingBypass::NeedsData)
         {
            LOG_DEBUG("Greylisting bypass " + matchedPattern + " needs the DKIM or DMARC result of the message; greylisting of " + toAddress + " is decided after DATA.");
            return GreyListPending;
         }
      }

      // Grey listing is enabled, and there's no reason to skip it. Go ahead!
      return GreyListingVerdictFor(message->GetFromAddress(), toAddress, ipaddress);
   }

   std::shared_ptr<MessageData>
   SpamProtection::AddSpamScoreHeaders(std::shared_ptr<Message> pMessage, std::set<std::shared_ptr<SpamTestResult> > setResult, bool classifiedAsSpam)
   {
      std::shared_ptr<MessageData> pMessageData;

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();

      if (!config.GetAddHeaderSpam() && 
          !config.GetAddHeaderReason() &&
           !config.GetPrependSubject())
      {
         // Nothing to do.
         return pMessageData;
      }

      if (!pMessage)
         return pMessageData;

      // Header only when the message is too large to be read whole
      // (MessageData::MaxParseBytes): every field written below is a header
      // field, and the body is copied behind them as it is when the caller
      // writes the message. The SMTP session hands this MessageData on to the
      // signature and the disclaimer, which then see such a message for what it
      // is and say in the log that they were not appended to it. Until the
      // large-message row the load failed, the caller's write of it was refused
      // (HM6010), and the message went without its spam headers - and without a
      // word from either footer.
      pMessageData = std::shared_ptr<MessageData>(new MessageData);
      if (!pMessageData->LoadFromMessage(MessageStore::WorkingFile(pMessage), pMessage, MessageData::LargeMessage::HeaderOnly))
         return pMessageData;

      if (classifiedAsSpam)
      {
         if (config.GetAddHeaderSpam())
         {
            pMessageData->SetFieldValue("X-hMailServer-Spam", "YES");
         }

         if (config.GetPrependSubject())
         {
            // Add subject
            String sSubject = pMessageData->GetFieldValue("Subject");

            // Check if subject is already prepended. If so, don't do it again.
            if (sSubject.Find(config.GetPrependSubjectText()) != 0)
            {
               sSubject = config.GetPrependSubjectText() + " " + sSubject;
               pMessageData->SetFieldValue("Subject", sSubject);
            }
         }
      }

      if (config.GetAddHeaderReason())
      {
         auto iter = setResult.begin();
         auto iterEnd = setResult.end();

         int iFieldIdx = 1;
         int iTotalScore = 0;
         for (; iter != iterEnd; iter++, iFieldIdx++)
         {
            std::shared_ptr<SpamTestResult> pResult = (*iter);

            // Only if the test has failed should we add a header for it. If the test
            // is neutral or pass, we shouldn't mention it in the headers.
            if (pResult->GetResult() != SpamTestResult::Fail)
               continue;

            iTotalScore += pResult->GetSpamScore();

            String sHeaderValue;
            sHeaderValue.Format(_T("%s - (Score: %d)"), pResult->GetMessage().c_str(), pResult->GetSpamScore());

            pMessageData->SetFieldValue("X-hMailServer-Reason-" + StringParser::IntToString(iFieldIdx), sHeaderValue);
         }

         pMessageData->SetFieldValue("X-hMailServer-Reason-Score", StringParser::IntToString(iTotalScore));
      }

      return pMessageData;
      
   }



   SpamProtection::GreyListingVerdict
   SpamProtection::GreyListingVerdictFor(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress & iRemoteIP)
   {
      if (IsWhiteListed(sSenderAddress, iRemoteIP))
         return GreyListAllow;

      GreyListing oGreyList;
      switch (oGreyList.GetVerdict(sSenderAddress, sRecipientAddress, iRemoteIP))
      {
      case GreyListing::Defer:
         return GreyListDefer;
      case GreyListing::Reject:
         return GreyListReject;
      default:
         return GreyListAllow;
      }
   }

   bool 
   SpamProtection::IsWhiteListed(const String &sFromAddress, const IPAddress &iIPAddress)
   {
      WhiteListCache cache;
      return cache.IsWhitelisted(sFromAddress, iIPAddress);
   }

   int 
   SpamProtection::CalculateTotalSpamScore(std::set<std::shared_ptr<SpamTestResult> > result)
   {
      int iTotalSpamScore = 0;

      auto iter = result.begin();
      auto iterEnd = result.end();

      for (; iter != iterEnd; iter++)
      {
         std::shared_ptr<SpamTestResult> pResult = (*iter);
         iTotalSpamScore += pResult->GetSpamScore();

      }

      return iTotalSpamScore;
   }


}