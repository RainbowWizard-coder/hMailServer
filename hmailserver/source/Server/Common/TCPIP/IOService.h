// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <boost/function.hpp>
#include "../Application/SessionManager.h"
#include "../Threading/Task.h"
#include "../Util/Event.h"

#include "SocketConstants.h"



namespace HM
{
   class TCPServer;

   // Owned through a shared_ptr (Application::StartServers_) and by more than one
   // owner: the work queues whose tasks hold connections on its io_context - the
   // IOCP queue its DoWork makes and the session queues - keep it alive for as long
   // as they exist (WorkQueue's outlives), so that it is freed after them and never
   // under a thread still in one of them. Hence enable_shared_from_this, which
   // DoWork hands the IOCP queue.
   class IOService : public Task, public std::enable_shared_from_this<IOService>
   {
   public:
      IOService(void);
      ~IOService(void);

      void DoWork();

      void Initialize();

      // Session types
      bool RegisterSessionType(SessionType st);

      // The listeners' half of a drain (Application::EnterDrain, and the first
      // thing StopServers does). StopAccepting closes every SMTP, IMAP and POP3
      // acceptor - a connection attempted from then on is refused at the socket,
      // not accepted and cut - and remembers that it did, so that the stop in
      // DoWork does not close them a second time. StartAccepting opens them
      // again on the same addresses and ports: the TCPServer objects DoWork
      // built are kept through the drain, so the port table is the one
      // Initialize used and a port added meanwhile waits for a restart, as it
      // always has. A port that cannot be reopened (something else took it
      // during the drain) is named in notReopened and reported as HM4316 by the
      // acceptor as at start; the rest are opened.
      //
      // Both run on the io_context's own threads - the acceptor is not
      // thread-safe, and a close from another thread while an accept is
      // pending is a race - and wait for that to have happened, bounded, so
      // that a caller's next line ("listeners closed") is true when it is
      // logged. False when the io_context did not run the request within the
      // bound, which only happens once it has been stopped.
      bool StopAccepting();
      bool StartAccepting(std::vector<String> &notReopened);
      bool IsAccepting() const { return accepting_; }

      boost::asio::io_context &GetIOContext();
      boost::asio::ssl::context &GetClientContext();
   private:

      // Holds a handler on a listener's strand across a stop and a start of the
      // servers, and looks for the next start's listener on that port.
      friend class IOServiceLifetimeTester;

      const String asynchronous_tasks_queue_;

      std::set<SessionType> session_types_;
      boost::asio::io_context io_context_;

      /*
         Keeps the I/O context alive across moments when it has no outstanding work.

         io_context::run() returns as soon as the last operation completes, and the
         worker task treats that return as "we are done" and exits the thread for
         good. On a server where every listener failed to bind - another mail server
         already on port 25 is the ordinary way to get there - or where SMTP, POP3
         and IMAP are all switched off, there is no outstanding accept from the
         start, so every TCP/IP thread exits within moments of being created.

         Nothing announces that, and the listeners are not the only user of this
         context: outbound SMTP delivery, the external POP3 fetcher, the
         SpamAssassin client and the connectivity diagnostics all post to it and
         then block on a disconnect event that only the connection's own destructor
         sets. With no thread left to run the connect, that event is never set and
         the delivery thread waits on it forever. Mail simply stops leaving, with
         the bind failures in the error log as the only clue and nothing at all
         linking the two.
      */
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;

      // Written by DoWork on the server work queue's thread, read and acted on
      // by the two drain halves on the io threads: one lock for all three.
      boost::mutex tcp_servers_mutex_;
      std::vector<std::shared_ptr<TCPServer> > tcp_servers_;

      // True from construction until StopAccepting closes the listeners; back to
      // true when StartAccepting reopens them; false for good from the stop in
      // DoWork, which closes every listener whatever this says.
      std::atomic<bool> accepting_;

      boost::condition_variable do_work_dummy;

      boost::asio::ssl::context client_context_;
   };


}