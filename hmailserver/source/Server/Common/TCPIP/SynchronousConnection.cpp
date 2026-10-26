// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "SynchronousConnection.h"
#include "../Util/ByteBuffer.h"
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>

using namespace boost::system;

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   void set_result(boost::optional<boost::system::error_code>* a, boost::system::error_code b) 
   { 
      a->reset(b); 
   } 

   // async_connect reports the endpoint it connected to as well. It's of no
   // interest here, but the handler signature requires it.
   void set_connect_result(boost::optional<boost::system::error_code>* a, boost::system::error_code b, const tcp::endpoint&)
   {
      a->reset(b);
   }

   SynchronousConnection::SynchronousConnection(int timeoutSeconds) :
      socket_(io_context_),
#ifdef HM_PLATFORM_POSIX
      local_socket_(io_context_),
      local_(false),
#endif
      seconds_(timeoutSeconds),
      remote_port_(0)
   {
      
   }

   SynchronousConnection::~SynchronousConnection()
   {
      try
      {
         boost::system::error_code err;
         socket_.shutdown(tcp::socket::shutdown_both, err);
         socket_.close(err);
#ifdef HM_PLATFORM_POSIX
         local_socket_.shutdown(boost::asio::socket_base::shutdown_both, err);
         local_socket_.close(err);
#endif
      }
      catch (...)
      {

      }
   }
   
   bool 
   SynchronousConnection::Connect(const AnsiString &hostName, int port)
   {
      remote_host_ = hostName;
      remote_port_ = port;

      try
      {
         tcp::resolver resolver(io_context_);
         boost::system::error_code resolve_error;

         // The name lookup remains synchronous. A lookup which is already in
         // progress cannot be aborted, so a deadline around it could not be
         // enforced - only the connection attempt below is bounded.
         auto endpoints = resolver.resolve(hostName, AnsiString(StringParser::IntToString(port)), resolve_error);
         if (resolve_error)
         {
            last_error_ = resolve_error;
            LOG_DEBUG(Formatter::Format("Failed while resolving {0}. Error: {1}", hostName, AnsiString(resolve_error.message())));

            return false;
         }

         // Each endpoint is tried in turn until one of them accepts the connection.
         boost::optional<error_code> connect_result;
         boost::asio::async_connect(socket_, endpoints, std::bind(set_connect_result, &connect_result, std::placeholders::_1, std::placeholders::_2));

         if (!WaitForCompletion_(connect_result, "connecting"))
         {
            Close();

            return false;
         }

         return true;
      }
      catch (boost::system::system_error &error)
      {
         last_error_ = error.code();
         Close();

         return false;
      }
   }

#ifdef HM_PLATFORM_POSIX
   bool
   SynchronousConnection::ConnectLocal(const AnsiString &path)
   {
      remote_host_ = path;
      remote_port_ = 0;
      local_ = true;

      try
      {
         // A path longer than a socket address holds is refused here, by the
         // endpoint, with name_too_long.
         const boost::asio::local::stream_protocol::endpoint endpoint(std::string(path.c_str()));

         boost::optional<error_code> connect_result;
         local_socket_.async_connect(endpoint, std::bind(set_result, &connect_result, std::placeholders::_1));

         if (!WaitForCompletion_(connect_result, "connecting"))
         {
            Close();

            return false;
         }

         return true;
      }
      catch (boost::system::system_error &error)
      {
         last_error_ = error.code();
         Close();

         return false;
      }
   }
#endif

   bool
   SynchronousConnection::Write(const AnsiString &data)
   {
      return Write_((const unsigned char*) data.data(), data.GetLength());

   }

   bool
   SynchronousConnection::Write(const ByteBuffer &data)
   {
      return Write_(data.GetBuffer(), data.GetSize());
   }

   bool 
   SynchronousConnection::Write_(const unsigned char *buf, size_t bufSize)
   {
      try
      {
         return OnSocket_([&](auto &socket)
            {
               // Start an asynchronous write.
               boost::optional<error_code> write_result;
               async_write(socket, boost::asio::buffer(buf, bufSize), std::bind(set_result, &write_result, std::placeholders::_1));

               // Wait for data to be written.
               return WaitForCompletion_(write_result, "sending data");
            });
      }
      catch (boost::system::system_error &error)
      {
         last_error_ = error.code();
         return false;
      } 
   }

   bool 
   SynchronousConnection::ReadUntil(const AnsiString &delimiter, AnsiString &readData)
   {
      readData.clear();

      try
      {
         // The buffer must outlive the asynchronous operation. WaitForCompletion_
         // does not return until the operation has reported its result.
         boost::asio::streambuf readBuffer;

         const bool read = OnSocket_([&](auto &socket)
            {
               // Start an asynchronous read.
               boost::optional<error_code> read_result;
               async_read_until(socket, readBuffer, delimiter, std::bind(set_result, &read_result, std::placeholders::_1));

               // Wait for input.
               return WaitForCompletion_(read_result, "reading response");
            });

         if (!read)
            return false;

         std::istream is(&readBuffer);

         readData.append((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());

         return true;
      }
      catch (boost::system::system_error &error)
      {
         last_error_ = error.code();
         return false;
      }  
   }

   bool
   SynchronousConnection::WaitForCompletion_(boost::optional<boost::system::error_code> &operation_result, const AnsiString &operation_description)
   {
      boost::optional<error_code> timer_result;

      // Create the timeout timer. A timeout of zero seconds means that the
      // operation is unbounded, in which case no deadline is armed at all.
      boost::asio::steady_timer timer(io_context_);
      if (seconds_ > 0)
      {
         timer.expires_after(std::chrono::seconds(seconds_));
         timer.async_wait(std::bind(set_result, &timer_result, std::placeholders::_1));
      }

      bool deadline_reached = false;

      io_context_.restart();

      while (io_context_.run_one())
      {
         if (operation_result)
         {
            timer.cancel();
         }
         else if (timer_result && !*timer_result && !deadline_reached)
         {
            // Closing the socket is what makes the pending operation report a
            // result. Without it, run_one() would simply block on the very same
            // operation again and the deadline would have no effect. The flag
            // guards against closing a second time if the loop comes back around
            // before the operation has reported.
            deadline_reached = true;

            Close();
         }
      }

      // The operation may have succeeded even though the deadline was reached, if
      // its result was already queued when the timer fired.
      if (operation_result && !*operation_result)
         return true;

      if (deadline_reached)
      {
         last_error_ = boost::asio::error::timed_out;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5523, "SynchronousConnection::WaitForCompletion_",
            Formatter::Format("Timed out after {0} seconds while {1}. Remote server: {2}.", seconds_, operation_description, GetRemoteName_()));

         return false;
      }

      if (operation_result)
      {
         last_error_ = *operation_result;
         LOG_DEBUG(Formatter::Format("Failed while {0}. Remote server: {1}. Error: {2}", operation_description, GetRemoteName_(), AnsiString(operation_result->message())));
      }
      else
      {
         last_error_ = boost::asio::error::operation_aborted;
         LOG_DEBUG(Formatter::Format("No result was reported while {0}. Remote server: {1}.", operation_description, GetRemoteName_()));
      }

      return false;
   }

   String
   SynchronousConnection::GetRemoteName_() const
   {
#ifdef HM_PLATFORM_POSIX
      // A local socket is named by its path alone.
      if (local_)
         return String(remote_host_);
#endif
      return Formatter::Format("{0}:{1}", remote_host_, remote_port_);
   }

   void
   SynchronousConnection::Close()
   {
      boost::system::error_code err;
      socket_.close(err);
#ifdef HM_PLATFORM_POSIX
      local_socket_.close(err);
#endif
   }


}
