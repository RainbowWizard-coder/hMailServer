// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "./ExternalFetch.h"
#include "../Common/BO/FetchAccount.h"
#include "../Common/Util/Event.h"
#include "../Common/TCPIP/IOService.h"
#include "../Common/TCPIP/DNSResolver.h"
#include "../Common/TCPIP/TCPConnection.h"
#include "../Common/Util/HttpsClient.h"
#include "../Common/Application/Configuration.h"
#include "POP3ClientConnection.h"
#include "IMAPClientConnection.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ExternalFetch::ExternalFetch(void)
   {
      
   }

   ExternalFetch::~ExternalFetch(void)
   {

   }

   void 
   ExternalFetch::Start(std::shared_ptr<FetchAccount> pFA)
   {  
      LOG_DEBUG(Formatter::Format("Retrieving messages from external account {0}", pFA->GetName()));
      
      std::shared_ptr<IOService> pIOService = Application::Instance()->GetIOService();

      std::shared_ptr<Event> disconnectEvent = std::shared_ptr<Event>(new Event()) ;
      // The account says which protocol its server speaks; both clients share
      // everything that happens to a message once it has been collected.
      std::shared_ptr<ExternalFetchClientBase> pClientConnection;
      if (pFA->GetServerType() == FetchAccount::IMAP)
      {
         pClientConnection = std::shared_ptr<IMAPClientConnection>
            (new IMAPClientConnection(pFA,
                                      pFA->GetConnectionSecurity(),
                                      pIOService->GetIOContext(),
                                      pIOService->GetClientContext(),
                                      disconnectEvent,
                                      pFA->GetServerAddress()));
      }
      else
      {
         pClientConnection = std::shared_ptr<POP3ClientConnection>
            (new POP3ClientConnection(pFA,
                                      pFA->GetConnectionSecurity(),
                                      pIOService->GetIOContext(),
                                      pIOService->GetClientContext(),
                                      disconnectEvent,
                                      pFA->GetServerAddress()));
      }

      DNSResolver resolver;

      std::vector<String> ip_addresses;
      resolver.GetIpAddresses(pFA->GetServerAddress(), ip_addresses, true);

      if (ip_addresses.empty())
      {
         String error_message = Formatter::Format("The IP address for external account {0} could not be resolved. Aborting fetch.", pFA->GetName());
         LOG_APPLICATION(error_message);
         return;
      }

      // An account a reader made names a server on a mailbox user's word, and the
      // connection is made from inside this server's network. The name was judged
      // public when it was saved (RestApiFetchAccounts.cpp), by another resolver
      // and at another moment: a name whose owner answers a public address then
      // and 127.0.0.1 or 10.x now (DNS rebinding), or one this server's DNSServer
      // answers differently from the system's (split horizon), would otherwise
      // reach the inside of the network every interval. So the addresses judged
      // here are the ones connected to, and only the public ones are kept -
      // unless the administrator allowed readers private hosts. An
      // administrator's account is not judged: collecting from a server inside
      // the network is what one is often for.
      if (pFA->GetReaderCreated() && !Configuration::Instance()->GetReaderFetchAccountsPrivateAddresses())
      {
         std::vector<String> public_addresses;
         for (const String &candidate : ip_addresses)
         {
            if (HttpsClient::AddressIsPublic(AnsiString(candidate)))
               public_addresses.push_back(candidate);
         }

         if (public_addresses.empty())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5909, "ExternalFetch::Start",
               Formatter::Format("External account {0}, which a reader added in the webmail, was not collected: {1} resolves to {2}, which is not a public address (loopback, private, link-local or reserved). Turn on reader_fetch_accounts_private_addresses to allow it.",
                  pFA->GetName(), pFA->GetServerAddress(), ip_addresses.front()));
            return;
         }

         ip_addresses.swap(public_addresses);
      }

      const String ip_address = ip_addresses.front();

      if (pClientConnection->Connect(ip_address, pFA->GetPort(), IPAddress()))
      {
         // Keep a WEAK reference before dropping the strong one. The strong reference has
         // to go, as it always did, so the connection can be torn down whenever it likes;
         // the weak one exists solely so the ceiling below can tell it to.
         std::weak_ptr<ExternalFetchClientBase> weakConnection = pClientConnection;

         pClientConnection.reset();

         // This wait had no ceiling at all, and disconnectEvent is set only by
         // ~TCPConnection. POP3ClientConnection sets an idle timeout and no absolute
         // bound, and an idle timeout is not a ceiling because it re-arms on every byte -
         // so a remote server that sends one byte before each expiry held this work-queue
         // thread for as long as it cared to. MaxNumberOfExternalFetchThreads of those and
         // external fetching stops for every account on the server.
         //
         // The ceiling is deliberately generous: a large mailbox over a slow link is a
         // legitimate hour, and this is a backstop for the pathological case rather than a
         // performance bound.
         const boost::chrono::minutes fetchCeiling(60);

         if (!disconnectEvent->WaitFor(fetchCeiling))
         {
            // Timing out is not enough on its own. The session is still running, and
            // returning here releases the fetch account for the next cycle - which would
            // then collect the same mailbox concurrently with the session we walked away
            // from, and that is how the same message gets delivered twice. So the
            // abandoned connection is disconnected before we let go of it.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6004, "ExternalFetch::Start",
               Formatter::Format("Fetching from external account {0} was still running after {1} minutes and has been abandoned. The connection has been closed. The remote server was sending just enough data to keep the session alive without completing it.",
                  pFA->GetName(), (int) fetchCeiling.count()));

            // EnqueueDisconnect rather than Disconnect: the latter is private, and it is
            // private for a good reason - this is not the connection's own thread, and the
            // enqueued form posts the shutdown onto the strand that owns the socket.
            if (auto connection = weakConnection.lock())
               connection->EnqueueDisconnect();
         }
      }

      LOG_DEBUG("Completed retrieval of messages from external account.");
   }


};