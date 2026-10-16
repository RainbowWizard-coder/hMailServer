// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "IOService.h"

#include "TCPServer.h"
#include "TCPConnection.h"

#include "LocalIPAddresses.h"

#include "../Threading/WorkQueue.h"
#include "../Threading/WorkQueueManager.h"
#include "../Util/ByteBuffer.h"
#include "../Util/ServerStatus.h"
#include "../BO/TCPIPPort.h"
#include "../BO/TCPIPPorts.h"

#include "IOQueueWorkerTask.h"
#include "SocketConstants.h"
#include "SslContextInitializer.h"

#include "../../SMTP/SMTPConnection.h"
#include "../../IMAP/IMAPConnection.h"
#include "../../POP3/POP3Connection.h"

#include <map>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IOService::IOService(void) :
       Task("IOService"),
       work_guard_(boost::asio::make_work_guard(io_context_)),
       accepting_(true),
       client_context_(boost::asio::ssl::context::sslv23)
   {
      // The floor no setting can lower, set the moment the context exists; the
      // shared initialiser applies the configured TLS versions on top.
      client_context_.set_options(HM_TLS_CONTEXT_FLOOR);
   }

   IOService::~IOService(void)
   {
      
   }

   bool 
   IOService::RegisterSessionType(SessionType st)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Registers a new connection type.
   //---------------------------------------------------------------------------()
   {
      session_types_.insert(st);

      return true;
   }

   void 
   IOService::Initialize()
   {
      SslContextInitializer::InitClient(client_context_);

      // The shared state OpenSSL's DANE needs, made once for the context every
      // outbound connection is made from, before any of them exists. A
      // connection only uses it when it is armed with TLSA records
      // (DaneVerifier::Arm); one armed when this failed fails its handshake.
      if (SSL_CTX_dane_enable(client_context_.native_handle()) <= 0)
      {
         ERR_clear_error();
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5510, "IOService::Initialize",
            "OpenSSL's DANE support could not be enabled. Deliveries to hosts that publish DNSSEC-validated TLSA records will be deferred.");
      }
   }

   void
   IOService::DoWork()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Creates the IO completion port, creates the worker threads, listen sockets etc.
   //---------------------------------------------------------------------------()
   {
      SetIsStarted();

      

      LOG_DEBUG("IOService::Start()");

      // Make sure information on which local ports are in use is reset.
      LocalIPAddresses::Instance()->LoadIPAddresses();

      // Create one socket for each IP address specified in the multi-homing settings.
      std::vector<std::shared_ptr<TCPIPPort> > vecTCPIPPorts = Configuration::Instance()->GetTCPIPPorts()->GetSnapshot();

      // How many configured ports have each number in each family. A port listed
      // once claims its number exclusively on Windows (TCPServer::InitAcceptor); a
      // number listed twice in one family - 0.0.0.0 and 127.0.0.1, say, with other
      // settings - is bound twice, which that would refuse, so neither claims it.
      std::map<std::pair<int, bool>, int> listedPerNumber;
      for (const std::shared_ptr<TCPIPPort> &listed : vecTCPIPPorts)
         listedPerNumber[std::make_pair(listed->GetPortNumber(), listed->GetAddress().GetAddress().is_v6())]++;

      auto iterPort = vecTCPIPPorts.begin();
      auto iterPortEnd = vecTCPIPPorts.end();


      for (; iterPort != iterPortEnd; iterPort++)
      {
         std::shared_ptr<TCPIPPort> pPort = (*iterPort);
         IPAddress address = pPort->GetAddress();
         int iPort = pPort->GetPortNumber();
         SessionType st = pPort->GetProtocol();
         ConnectionSecurity connection_security = pPort->GetConnectionSecurity();

         std::shared_ptr<SSLCertificate> pSSLCertificate;

         if (connection_security == CSSSL ||
             connection_security == CSSTARTTLSOptional ||
             connection_security == CSSTARTTLSRequired)
         {
            std::shared_ptr<SSLCertificates> pSSLCertificates = Configuration::Instance()->GetSSLCertificates();
            pSSLCertificate = pSSLCertificates->GetItemByDBID(pPort->GetSSLCertificateID());
         }

         if (session_types_.find(st) == session_types_.end())
            continue;

         std::shared_ptr<TCPConnectionFactory> pConnectionFactory;
         std::shared_ptr<TCPServer> pTCPServer;

         switch (st)
         {
         case STSMTP:
            pConnectionFactory = std::shared_ptr<TCPConnectionFactory>(new SMTPConnectionFactory());
            break;
         case STIMAP:
            pConnectionFactory = std::shared_ptr<TCPConnectionFactory>(new IMAPConnectionFactory());
            break;
         case STPOP3:
            pConnectionFactory = std::shared_ptr<TCPConnectionFactory>(new POP3ConnectionFactory());
            break;

         default:
            // Skip the port entirely rather than falling through to construct a
            // TCPServer around a null factory - StartAccept dereferences it on the
            // very first accept, which is a null-pointer fault in the accept loop
            // rather than the "unable to start server" this branch reports.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4325, "IOService::DoWork()", "Unable to start server- Unsupported session type.");
            continue;
         }

         pTCPServer = std::shared_ptr<TCPServer>(new TCPServer(io_context_, address, iPort, st, pSSLCertificate, pConnectionFactory, connection_security,
            pPort->GetClientCertificatePolicy(), pPort->GetClientCertificateCAFile(),
            st == STSMTP ? pPort->GetSMTPAuthentication() : SAMOn));

         pTCPServer->SetExclusiveAddressUse(listedPerNumber[std::make_pair(iPort, address.GetAddress().is_v6())] == 1);
         pTCPServer->Run();

         {
            boost::lock_guard<boost::mutex> guard(tcp_servers_mutex_);
            tcp_servers_.push_back(pTCPServer);
         }
      }


      const int iThreadCount = Configuration::Instance()->GetTCPIPThreads();

      if (iThreadCount <= 0)
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4325, "IOService::DoWork()", "The number of TCP/IP threads has been set to zero.");

      // The queue keeps this service alive for as long as it exists: its threads
      // run in io_context_, and a stop that gives up on one (a session's handler
      // in a wait the stop cannot interrupt) leaves the queue to WorkQueueManager,
      // which frees it once the thread has left - and this service after it, not
      // under that thread when StopServers lets go of io_service_.
      size_t iQueueID = WorkQueueManager::Instance()->CreateWorkQueue(iThreadCount, "IOCPQueue", shared_from_this());

      std::shared_ptr<WorkQueue> pWorkQueue = WorkQueueManager::Instance()->GetQueue("IOCPQueue");

      // Launch a thread that holds the IOCP objects
      for (int i = 0; i < iThreadCount; i++)
      {
         std::shared_ptr<IOCPQueueWorkerTask> pWorkerTask = std::shared_ptr<IOCPQueueWorkerTask>(new IOCPQueueWorkerTask(io_context_));
         WorkQueueManager::Instance()->AddTask(iQueueID, pWorkerTask);
      }  

      try
      {
         boost::mutex do_work_dummy_mutex;
         boost::unique_lock<boost::mutex> lock(do_work_dummy_mutex);

         do_work_dummy.wait(lock);
      }
      catch (const boost::thread_interrupted&)
      {
         boost::this_thread::disable_interruption disabled;

         LOG_DEBUG("IOService::Stop()");

         // Release the keep-alive first, so that even if stop() were not called the
         // context could still drain and the workers exit on their own.
         work_guard_.reset();

         io_context_.stop();

         // Every listener closed for good, here, whatever the drain did. A drain
         // (StopAccepting, which StopServers calls first) asks each listener's
         // strand to close it, and a strand a thread is still in - a handler
         // that has re-armed its accept and is running the OnClientConnect
         // script, say - runs that request only once the thread leaves it, which
         // the io_context, stopped above, then never lets it do. Left to it, such
         // a listener would stay open and bound for as long as this service is
         // kept - and a stop that gives up on that thread keeps it until the
         // thread has left (WorkQueueManager) - so that the next start's listener
         // on that port could not bind. TCPServer::Close takes the acceptor's own
         // lock, so it is made here, beside any handler still on the strand, and
         // nothing opens the acceptor again after it.
         accepting_ = false;
         {
            boost::lock_guard<boost::mutex> guard(tcp_servers_mutex_);
            auto iterServer = tcp_servers_.begin();
            auto iterEnd = tcp_servers_.end();
            for (; iterServer != iterEnd; iterServer++)
            {
               (*iterServer)->Close();
            }
         }

         LOG_DEBUG("IOService::DoWork() - removing Queue IOCP Queue")
            // Now the worker queues will get notifications that the outstanding
            // acceptex sockets are dropped.
         WorkQueueManager::Instance()->RemoveQueue("IOCPQueue");

         LOG_DEBUG("IOService::Stop() - Complete");
         return;
      }

   }

   namespace
   {
      // How long a drain half waits for the io threads to have run it. They run
      // it within microseconds while the io_context is running, and never once
      // it has been stopped - in which case the acceptors are closed anyway.
      const int ACCEPTOR_REQUEST_WAIT_MILLISECONDS = 10000;

      // One count per listener. Each listener's request runs on that listener's
      // own strand - the strand its accept completions run on, so a close and a
      // re-armed accept can never be on two workers at once - and counts itself
      // down when it has run; the caller waits for the count to reach zero, or
      // for the bound to pass. The failures a reopen collects are written from
      // several strands at once, hence the lock around them.
      class ListenerRequests
      {
      public:
         explicit ListenerRequests(size_t count) :
            remaining_(count)
         {
         }

         void Done()
         {
            boost::lock_guard<boost::mutex> guard(mutex_);
            if (remaining_ > 0)
               remaining_--;
            if (remaining_ == 0)
               all_done_.notify_all();
         }

         void Failed(const String &listener)
         {
            boost::lock_guard<boost::mutex> guard(mutex_);
            failures_.push_back(listener);
         }

         bool WaitForAll(int milliseconds)
         {
            boost::unique_lock<boost::mutex> lock(mutex_);
            return all_done_.wait_for(lock, boost::chrono::milliseconds(milliseconds), [this]() { return remaining_ == 0; });
         }

         std::vector<String> Failures()
         {
            boost::lock_guard<boost::mutex> guard(mutex_);
            return failures_;
         }

      private:
         boost::mutex mutex_;
         boost::condition_variable all_done_;
         size_t remaining_;
         std::vector<String> failures_;
      };
   }

   bool
   IOService::StopAccepting()
   {
      if (!accepting_.exchange(false))
         return true;

      std::vector<std::shared_ptr<TCPServer> > servers;
      {
         boost::lock_guard<boost::mutex> guard(tcp_servers_mutex_);
         servers = tcp_servers_;
      }

      std::shared_ptr<ListenerRequests> requests = std::make_shared<ListenerRequests>(servers.size());

      // dispatch, not post: run inline if the caller is already on that strand,
      // so that a wait on this thread can never be for this thread.
      for (auto server = servers.begin(); server != servers.end(); ++server)
      {
         std::shared_ptr<TCPServer> listener = *server;
         boost::asio::dispatch(listener->GetStrand(), [listener, requests]()
         {
            listener->StopAccept();
            requests->Done();
         });
      }

      if (!requests->WaitForAll(ACCEPTOR_REQUEST_WAIT_MILLISECONDS))
      {
         LOG_APPLICATION("Drain: the TCP/IP threads did not close every listener within ten seconds, so the listeners are NOT known to be closed; they are only ever this slow once the io threads have stopped.");
         return false;
      }

      return true;
   }

   bool
   IOService::StartAccepting(std::vector<String> &notReopened)
   {
      notReopened.clear();

      if (accepting_.exchange(true))
         return true;

      std::vector<std::shared_ptr<TCPServer> > servers;
      {
         boost::lock_guard<boost::mutex> guard(tcp_servers_mutex_);
         servers = tcp_servers_;
      }

      std::shared_ptr<ListenerRequests> requests = std::make_shared<ListenerRequests>(servers.size());

      for (auto server = servers.begin(); server != servers.end(); ++server)
      {
         std::shared_ptr<TCPServer> listener = *server;
         boost::asio::dispatch(listener->GetStrand(), [listener, requests]()
         {
            if (!listener->RestartAccept())
               requests->Failed(Formatter::Format("{0}:{1}", String(listener->GetAddress().ToString()), listener->GetPort()));
            requests->Done();
         });
      }

      if (!requests->WaitForAll(ACCEPTOR_REQUEST_WAIT_MILLISECONDS))
      {
         // Not reopened, and said so: the flag goes back to closed, so that the
         // next attempt is an attempt and not a no-op, and the caller keeps the
         // state at Draining rather than declaring Running over closed ports.
         accepting_ = false;
         LOG_APPLICATION("Drain: the TCP/IP threads did not reopen the listeners within ten seconds; the listeners are not open and the drain has not been left.");
         return false;
      }

      notReopened = requests->Failures();
      return true;
   }

   boost::asio::ssl::context &
   IOService::GetClientContext()
   {
      return client_context_;
   }

   boost::asio::io_context &
   IOService::GetIOContext()
   {
      return io_context_;
   }


}