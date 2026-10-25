// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "SpamTestSpamAssassin.h"

#include "SpamTestData.h"
#include "SpamTestResult.h"

#include "AntiSpamConfiguration.h"

#include "SpamAssassin/SpamAssassinClient.h"

#include "../TCPIP/IOService.h"
#include "../TCPIP/TCPConnection.h"
#include "../TCPIP/DNSResolver.h"

#include "../BO/MessageData.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../BO/MessageRecipient.h"
#include "../Application/IniFileSettings.h"
#include "../Util/Unicode.h"
#include "../Util/Event.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   String 
   SpamTestSpamAssassin::GetName() const
   {
      return "SpamTestSpamAssassin";
   }

   bool 
   SpamTestSpamAssassin::GetIsEnabled()
   {
      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();
      if (config.GetSpamAssassinEnabled())
         return true;
      else
         return false;
   }

   std::set<std::shared_ptr<SpamTestResult> >
   SpamTestSpamAssassin::RunTest(std::shared_ptr<SpamTestData> pTestData)
   {
      std::set<std::shared_ptr<SpamTestResult> > setSpamTestResults;
      
      AntiSpamConfiguration& config = Configuration::Instance()->GetAntiSpamConfiguration();

      std::shared_ptr<Message> pMessage = pTestData->GetMessageData()->GetMessage();
      const String sFilename = MessageStore::WorkingFile(pMessage);

      // The envelope sender, as the Return-Path a final delivery writes (RFC 5321
      // section 4.4): SpamAssassin's SPF checks and a number of its stock and
      // custom rules read the envelope sender from it. It is sent to spamd as the
      // first line of the message and nowhere else - in place of any Return-Path the
      // message arrived with, which a final delivery may remove - and spamd's reply
      // comes back without it (SpamAssassinClient, SpamAssassinReturnPath.h). The
      // stored message is not written for it, so a scan that fails or never starts
      // leaves the message exactly as it was.
      //
      // A message collected from an external account has no envelope sender here:
      // the one it is given is a guess from its From field. The other server's
      // final delivery recorded the real one in the message's topmost Return-Path,
      // and the collection hands its path over (SpamTestData::GetRecordedReturnPath),
      // which is sent instead when there is one. Before 6.3.7 the guess was sent,
      // above the message's own Return-Path fields, and where one of those stood
      // under a Received field - Gmail's layout - SpamAssassin read the envelope
      // sender from no Return-Path at all (it trusts none once a Received field
      // stands above one).
      AnsiString sReturnPath = pTestData->GetRecordedReturnPath();
      if (sReturnPath.IsEmpty())
      {
         const String sEnvelopeFrom = pTestData->GetEnvelopeFrom();
         const AnsiString envelopePath = sEnvelopeFrom.IsEmpty() ? "<>" : "<" + sEnvelopeFrom + ">";
         sReturnPath = envelopePath;
      }

      std::shared_ptr<IOService> pIOService = Application::Instance()->GetIOService();

      // Shared with the client (not a reference to this stack frame) so the bounded
      // wait below can return while the connection is still alive.
      std::shared_ptr<bool> testCompleted = std::make_shared<bool>(false);

      std::shared_ptr<Event> disconnectEvent = std::shared_ptr<Event>(new Event());
      std::shared_ptr<SpamAssassinClient> pSAClient = std::shared_ptr<SpamAssassinClient>(new SpamAssassinClient(sFilename, pIOService->GetIOContext(), pIOService->GetClientContext(), disconnectEvent, testCompleted));
      
      String sHost = config.GetSpamAssassinHost();
      int iPort = config.GetSpamAssassinPort();

      
      DNSResolver resolver;

      std::vector<String> ip_addresses;
      resolver.GetIpAddresses(sHost, ip_addresses, true);

      String ip_address;
      if (ip_addresses.size())
      {
         ip_address = *(ip_addresses.begin());
      }
      else
      {
         String message = "The IP address for SpamAssassin could not be resolved. Aborting tests.";
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5507, "SpamTestSpamAssassin::RunTest", message);
         return setSpamTestResults;  
      }

      // The user spamd scans as - the one whose preferences it applies. A scan runs once
      // per message, so a message to several recipients has no one user whose
      // preferences would be the honest choice: it is scanned as the fixed profile, or
      // as nobody, the way every message was before these settings existed.
      String spamd_user = IniFileSettings::Instance()->GetSpamAssassinUser();
      if (IniFileSettings::Instance()->GetSpamAssassinUserFromRecipient())
      {
         // owned by this call: the recipients of the message under test, held by its session or delivery
         const std::vector<std::shared_ptr<MessageRecipient> > &recipients = pMessage->GetRecipients()->GetVector();
         if (recipients.size() == 1 && recipients[0])
            spamd_user = recipients[0]->GetAddress();
      }
      pSAClient->SetUser(Unicode::ToANSI(spamd_user));
      pSAClient->SetReturnPath(sReturnPath);

      // Here we handle of the ownership to the TCPIP-connection layer.
      if (pSAClient->Connect(ip_address, iPort, IPAddress()))
      {
         // Make sure we keep no references to the TCP connection so that it
         // can be terminated whenever. We're longer own the connection.
         pSAClient.reset();

         // Deliberately unbounded. The connection carries an absolute session
         // ceiling (set in its constructor), so this wait is guaranteed to be
         // released: the event fires when the connection is destroyed, and the
         // ceiling guarantees it is. Bounding the wait here instead would let this
         // thread continue while the connection is still alive and still holding
         // the message file open - and it moves its result over that file, which
         // is the live spool file of the message being accepted.
         disconnectEvent->Wait();
      }

      if (!*testCompleted)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5508, "SpamTestSpamAssassin::RunTest",
            "The SpamAssassin tests did not complete within the time limit, or the configuration (host name and port) is invalid, or SpamAssassin is not running. The message was accepted without a SpamAssassin verdict.");

         return setSpamTestResults;
      }

      // The message's size is its file's, which a completed scan's result has
      // replaced. A message collected from an external account is saved with the
      // size it holds straight after these tests (ExternalFetchClientBase), and the
      // global rules that read a message's size at delivery read that. Until 6.3.7
      // the rewrite that took the Return-Path out set it (MessageData::Write);
      // nothing here writes the file any more, so it is set here.
      pMessage->SetSize(MessageStore::FileSize(sFilename));

      // Check if the message is tagged as spam.
      std::shared_ptr<MessageData> pMessageData = pTestData->GetMessageData();

      // The message file is now spamd's result - the message as SpamAssassin left
      // it, without the Return-Path it was sent - or, where the result could not be
      // applied, the message as it was. Nothing here writes it: it is read back for
      // SpamAssassin's verdict, and for the tests that run after this one. Until
      // 6.3.7 the Return-Path was written into the file before the scan and taken
      // out here by parsing the message and writing it again, which changed bytes
      // SpamAssassin never touched.
      if (!pMessageData->RefreshFromMessage())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5509, "SpamTestSpamAssassin::RunTest",
            "The message could not be reloaded after SpamAssassin processing, so SpamAssassin's verdict could not be read; the message is delivered as SpamAssassin returned it, without a SpamAssassin score. It may be too large for the MIME parser (80 MB) or malformed.");
      }

      bool bIsSpam = false;
      AnsiString sSpamStatus = pMessageData->GetFieldValue("X-Spam-Status");
      if (sSpamStatus.Mid(0, 3).ToUpper() == "YES")
         bIsSpam = true;

      if (bIsSpam)
      {
         int iScore = 0;
         if (config.GetSpamAssassinMergeScore())
            iScore = ParseSpamAssassinScore_(sSpamStatus);
         else
            iScore = config.GetSpamAssassinScore();

         String sMessage = "Tagged as Spam by SpamAssassin";
         std::shared_ptr<SpamTestResult> pResult = std::shared_ptr<SpamTestResult>(new SpamTestResult(GetName(), SpamTestResult::Fail, iScore, sMessage));
         setSpamTestResults.insert(pResult);   
      }
      
      return setSpamTestResults;
   }

   int 
   SpamTestSpamAssassin::ParseSpamAssassinScore_(const AnsiString &sHeader)
   {
      int iStartPos = sHeader.FindNoCase("score=");
      if (iStartPos < 0)
         return 0;

      iStartPos += 6;

      int iScoreEnd = sHeader.Find(".", iStartPos);
      if (iScoreEnd < 0)
         return 0;

      int iScoreLength = iScoreEnd - iStartPos;

      if (iScoreLength <= 0)
         return 0;

      AnsiString sScore = sHeader.Mid(iStartPos, iScoreLength);

      int iRetVal = atoi(sScore);
      return iRetVal;

   }




}