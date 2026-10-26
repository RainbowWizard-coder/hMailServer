// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <boost/optional.hpp>

using boost::asio::ip::tcp;

namespace HM
{
   class SynchronousConnection
   {
   public:
      SynchronousConnection(int timeoutSeconds);
      virtual ~SynchronousConnection();

      bool Connect(const AnsiString &hostName, int port);

#ifdef HM_PLATFORM_POSIX
      // A Unix stream socket at path, such as clamd's LocalSocket, in place of a host
      // and a port. Connecting, writing and reading on it keep the deadlines of the
      // TCP connection; only the socket differs. A listener whose queue of
      // connections waiting to be accepted is full is waited for until the deadline,
      // as a TCP connection is, and not taken for a connection made.
      bool ConnectLocal(const AnsiString &path);
#endif

      bool Write(const AnsiString &data);
      bool Write(const ByteBuffer &buffer);
      bool ReadUntil(const AnsiString &delimiter, AnsiString &readData);
      void Close();

      // What the last operation that failed reported: the resolver's or the
      // socket's error, or timed_out where the deadline ended it.
      const boost::system::error_code &GetLastError() const { return last_error_; }

   private:

      bool Write_(const unsigned char *buf, size_t bufSize);

      // Runs the io_context until the pending asynchronous operation completes,
      // enforcing the connection timeout on it.
      bool WaitForCompletion_(boost::optional<boost::system::error_code> &operation_result, const AnsiString &operation_description);

      // Calls operation with the socket this connection was made on, the TCP one
      // or the local one: what is written and read, and how, is the same on both.
      template <typename Operation>
      bool OnSocket_(Operation &&operation)
      {
#ifdef HM_PLATFORM_POSIX
         if (local_)
            return operation(local_socket_);
#endif
         return operation(socket_);
      }

      // The deadline ended operation_description: kept as timed_out, and reported
      // as HM5523, naming the remote server.
      void ReportTimeout_(const AnsiString &operation_description);

      String GetRemoteName_() const;

#ifdef HM_PLATFORM_POSIX
      void CloseLocal_();
#endif

      boost::asio::io_context io_context_;
      tcp::socket socket_;
#ifdef HM_PLATFORM_POSIX
      boost::asio::local::stream_protocol::socket local_socket_;
      bool local_;
#endif
      int seconds_;

      AnsiString remote_host_;
      int remote_port_;

      boost::system::error_code last_error_;
   };


}
