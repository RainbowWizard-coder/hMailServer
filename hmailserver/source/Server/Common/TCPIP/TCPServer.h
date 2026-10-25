// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <ctime>
#include <iostream>

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

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
         ClientCertificatePolicy client_certificate_policy, const String &client_certificate_ca_file,
         SMTPAuthenticationMode smtp_authentication = SAMOn);
      ~TCPServer(void);

      void Run();

      // Closes the acceptor, which RestartAccept may open again: a drain's half.
      void StopAccept();

      // Closes the acceptor for good: nothing opens it again, RestartAccept
      // included. The stop of the I/O service (IOService::DoWork) closes every
      // listener this way once its io_context is stopped - from its own thread,
      // whatever a drain did before it - so that the port is free for the next
      // start's listener even while a thread is still in a handler on this
      // listener's strand, which no request dispatched there can then reach.
      void Close();

      // Opens the acceptor again after StopAccept closed it - the same address,
      // port and TLS context Run built, so a drain that is left (Application::
      // LeaveDrain) listens exactly where it listened before. True when the
      // port is listening on return, which includes "it never stopped". False
      // when Run never got as far as the acceptor (a certificate that would not
      // load, a policy the port cannot carry), when Close has closed it, or when
      // the bind fails now; the bind failure is reported as HM4316/HM4317 by
      // InitAcceptor as at start. Called on this listener's strand, as
      // StopAccept during a drain is: IOService dispatches both there.
      bool RestartAccept();

      bool IsListening() const
      {
         boost::lock_guard<boost::mutex> guard(acceptor_mutex_);
         return acceptor_.is_open();
      }
      int GetPort() const { return port_; }
      const IPAddress &GetAddress() const { return ipaddress_; }

      // Whether the listener claims its port exclusively on Windows
      // (SO_EXCLUSIVEADDRUSE), set before Run: true unless another configured
      // TCP/IP port has this one's number in this one's family, which the option
      // would refuse (InitAcceptor says more). Off until set; no effect on POSIX.
      void SetExclusiveAddressUse(bool exclusive) { exclusive_address_use_ = exclusive; }

      // The accept completion (HandleAccept, which re-posts the accept) runs on
      // this strand, and so do the close and the reopen a drain dispatches. The
      // io_context has as many threads as GetTCPIPThreads says, so "on the io
      // thread" was never one thread - a close on one worker while another was
      // re-arming the accept was a race on the acceptor's state. IOService
      // dispatches StopAccept and RestartAccept onto this strand. Close is the
      // one call made elsewhere - by the stop, on its own thread - which is why
      // acceptor_mutex_ guards the acceptor as well.
      boost::asio::strand<boost::asio::io_context::executor_type> &GetStrand() { return strand_; }

   private:
      
      std::string GetPassword() const;

      // Each takes acceptor_mutex_ for what it does to acceptor_, and InitAcceptor
      // reports a failure after letting go of it: a report can run the OnError
      // script, which must not hold up a close made meanwhile.
      bool InitAcceptor();
      void StartAccept();
      void HandleAccept(std::shared_ptr<TCPConnection> connection, const boost::system::error_code &error);

      bool FireOnAcceptEvent(std::shared_ptr<TCPConnection> connection, const IPAddress &remoteAddress, int port);

      // Loads the port's client-CA bundle into the SSL context so that OpenSSL
      // can verify inbound client certificates against it. Returns false when
      // the listener must not start; see the definition for when that is.
      bool InitInboundClientCertificateVerification_();

      std::shared_ptr<TCPConnectionFactory> connectionFactory_;

      // Declared before acceptor_ so that it is constructed first; both are
      // built from the io_context.
      boost::asio::strand<boost::asio::io_context::executor_type> strand_;

      // Guards acceptor_ and closed_. The strand orders the accept completion,
      // the drain's close and its reopen among themselves; Close comes from the
      // stop's own thread, while a thread may still be in a handler on the
      // strand - one that has re-armed its accept and is running the
      // OnClientConnect script, say - or at the start of one. Held only for the
      // calls on the acceptor, never while anything is reported.
      mutable boost::mutex acceptor_mutex_;

      // Set by Close: nothing opens the acceptor again.
      bool closed_;

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

      // What the TCPIPPort says this listener does with AUTH (GitLab work item
      // #54), handed to every accepted connection; only SMTPConnection reads it.
      SMTPAuthenticationMode smtp_authentication_;

      // Set by Run once the TLS context and the client-certificate policy have
      // been accepted, which is everything RestartAccept must not redo: a
      // context initialised twice would load the certificate twice, and a port
      // Run refused to start must stay refused.
      bool prepared_;

      // SetExclusiveAddressUse.
      bool exclusive_address_use_;
   };
}