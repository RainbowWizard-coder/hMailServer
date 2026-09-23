// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <ctime>
#include <iostream>

#include "SocketConstants.h"

#include "../BO/SSLCertificates.h"
#include "../BO/SSLCertificate.h"

#include "TCPConnectionFactory.h"

using boost::asio::ip::tcp;

namespace HM
{
   class TCPConnection;

   class TCPServer
   {
   public:
      TCPServer(boost::asio::io_context& io_context, const IPAddress &ipaddress, int port, SessionType sessionType, std::shared_ptr<SSLCertificate> certificate, std::shared_ptr<TCPConnectionFactory> connectionFactory, ConnectionSecurity connection_security,
         ClientCertificatePolicy client_certificate_policy, const String &client_certificate_ca_file);
      ~TCPServer(void);

      void Run();
      void StopAccept();

      // Opens the acceptor again after StopAccept closed it - the same address,
      // port and TLS context Run built, so a drain that is left (Application::
      // LeaveDrain) listens exactly where it listened before. True when the
      // port is listening on return, which includes "it never stopped". False
      // when Run never got as far as the acceptor (a certificate that would not
      // load, a policy the port cannot carry) or when the bind fails now; the
      // bind failure is reported as HM4316/HM4317 by InitAcceptor as at start.
      // Only ever called on the io_context's own thread, as StopAccept during a
      // drain is: IOService posts both, because the acceptor is not thread-safe
      // and a close from another thread while an accept is pending is a race.
      bool RestartAccept();

      bool IsListening() const { return acceptor_.is_open(); }
      int GetPort() const { return port_; }
      const IPAddress &GetAddress() const { return ipaddress_; }

   private:
      
      std::string GetPassword() const;

      bool InitAcceptor();
      void StartAccept();
      void HandleAccept(std::shared_ptr<TCPConnection> connection, const boost::system::error_code &error);

      bool FireOnAcceptEvent(std::shared_ptr<TCPConnection> connection, const IPAddress &remoteAddress, int port);

      // Loads the port's client-CA bundle into the SSL context so that OpenSSL
      // can verify inbound client certificates against it. Returns false when
      // the listener must not start; see the definition for when that is.
      bool InitInboundClientCertificateVerification_();

      std::shared_ptr<TCPConnectionFactory> connectionFactory_;

      boost::asio::ip::tcp::acceptor acceptor_;
      boost::asio::ssl::context context_;
      boost::asio::io_context& io_context_;
      SessionType sessionType_;
      std::shared_ptr<SSLCertificate> certificate_;

      IPAddress ipaddress_;
      int port_;

      ConnectionSecurity connection_security_;

      // Mutual-TLS settings from the TCPIPPort this listener was built from.
      // The policy is handed to every accepted connection; the CA bundle is
      // loaded once into context_, which every connection's ssl stream shares.
      ClientCertificatePolicy client_certificate_policy_;
      String client_certificate_ca_file_;

      // Set by Run once the TLS context and the client-certificate policy have
      // been accepted, which is everything RestartAccept must not redo: a
      // context initialised twice would load the certificate twice, and a port
      // Run refused to start must stay refused.
      bool prepared_;
   };
}