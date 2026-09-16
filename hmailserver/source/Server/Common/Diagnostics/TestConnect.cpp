// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "TestConnect.h"

#include "TestConnection.h"
#include "TestConnectionResult.h"

#include "../TCPIP/IOService.h"

#include "../TCPIP/DNSResolver.h"
#include "../../SMTP/SMTPConfiguration.h"

using boost::asio::ip::tcp;

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   bool
   TestConnect::PerformTest(ConnectionSecurity connection_security, const String  &localAddressStr, const String &server, int port, String &result)
   {
      std::shared_ptr<IOService> io_service_wrapper = Application::Instance()->GetIOService();

      // The setting may name one address of each family; which one applies is
      // known only once the remote has been resolved, so it is chosen per
      // address below (SMTPConfiguration::OutboundBindAddress).
      if (!localAddressStr.IsEmpty())
         result.append(Formatter::Format("Outbound connections bind to {0}.\r\n", localAddressStr));

      result.append(Formatter::Format("Trying to connect to host {0}...\r\n", server));
      

      // Get a list of endpoints corresponding to the server name.
      DNSResolver resolver;

      std::vector<String> ipaddresses;
      auto dnsResult = resolver.GetIpAddresses(server, ipaddresses, true);

      if (!dnsResult || ipaddresses.size() == 0)
      {
         // Host was not found.
         String formattedString;
         formattedString.Format(_T("ERROR: The host name %s could not be resolved.\r\n"), server.c_str());

         result.append(formattedString);
         return false;
      }

      String last_error_message;

      for (auto ipaddress : ipaddresses)
      {
         String formattedString;
         formattedString.Format(_T("Trying to connect to TCP/IP address %s on port %d.\r\n"), ipaddress.c_str(), port);
         result.append(formattedString);

         IPAddress remoteAddress;
         remoteAddress.TryParse(AnsiString(ipaddress), false);

         IPAddress localAddress = SMTPConfiguration::OutboundBindAddress(localAddressStr, remoteAddress);

         if (!localAddressStr.IsEmpty())
         {
            if (localAddress.IsAny())
               result.append(_T("No bound address is of this address's family, so the system chooses the source address.\r\n"));
            else
               result.append(Formatter::Format("Local address is {0}.\r\n", localAddress.ToString()));
         }

         std::shared_ptr<Event> disconnectEvent = std::shared_ptr<Event>(new Event());

         std::shared_ptr<TestConnectionResult> connection_result = std::make_shared<TestConnectionResult>();

         std::shared_ptr<TestConnection> connection = std::make_shared<TestConnection>(connection_security, io_service_wrapper->GetIOContext(), io_service_wrapper->GetClientContext(), disconnectEvent, server, connection_result);
         if (connection->Connect(ipaddress, port, localAddress))
         {
            connection.reset();

            disconnectEvent->Wait();

            if (connection_result->GetConnectedSuccesfully())
            {
               result.append(_T("Connected successfully.\r\n"));

               if (connection_security == CSSSL)
               {
                  if (connection_result->GetHandshakeCompletedSuccesfully())
                  {
                     result.append(_T("SSL/TLS handshake completed successfully.\r\n"));
                     return true;
                  }
                  else
                  {
                     result.append(_T("ERROR: Handshake failed.\r\n"));
                     return false;
                  }
               }
               else
               {
                  return true;
               }
            }
            else
            {
               result.append(Formatter::Format("ERROR: It was not possible to connect. Error: {0}.\r\n", connection_result->GetErrorMessage()));
            }
         }
      }

      // We were unable to connect.
      result.append(_T("ERROR: Failed to connect to all servers.\r\n"));
      return false;
   }

   
      
}
