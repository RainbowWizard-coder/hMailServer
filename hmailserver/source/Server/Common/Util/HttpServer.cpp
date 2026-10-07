// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// An HTTP/1.1 server on Boost.Asio for the optional listeners.
//
// What it replaces: each listener was a std::thread around a blocking accept()
// that read one request, answered it and closed - HTTP/1.0, Connection: close,
// one request at a time for the whole listener. Adequate for a metrics scrape
// every thirty seconds; hopeless for a client issuing dozens of small requests
// per screen, and the reason a slow client could hold the whole API for as
// long as a read timeout.
//
// The shape here:
//
//  - Its own io_context, run by a bounded pool of worker threads. Nothing the
//    mail protocols do shares an executor with this, so a request storm on the
//    API cannot delay an SMTP accept, and the pool size is the ceiling on how
//    many handlers can be in the database at once.
//
//  - Every connection is a strand: its reads, its writes and its two timers
//    never run concurrently, so the connection needs no lock of its own.
//
//  - Keep-alive. An HTTP/1.1 client gets it unless it says Connection: close;
//    an HTTP/1.0 client gets it only if it asks. The listeners' tests speak
//    HTTP/1.0 with Connection: close and are answered exactly as before.
//
//  - Absolute ceilings, not idle timeouts. A request timer starts when the
//    server begins waiting for a request and covers reading it; a connection
//    timer starts at accept and covers the connection's whole life. A client that dribbles a byte at a time just under an idle timeout
//    used to occupy the single worker indefinitely; here it is closed when the
//    deadline passes, whatever it is doing.
//
//  - The response is written in pieces of WriteChunkBytes, and the request
//    timer is armed afresh for each: a reader that stops reading is closed
//    after request_seconds without progress, and one that keeps reading gets
//    as long as the connection timer allows - which every answer moves out,
//    when it has to, to fit its size (below). Until 26 September 2026 the
//    request timer that covered reading the request covered writing the
//    response too, so a download larger than request_seconds of the reader's
//    bandwidth - a 100 MB file link at under 27 Mbit/s - was cut off part way,
//    every time.
//
//  - An answer once begun is written whole or the connection is reset; it is
//    never ended part way as though it were complete. A handler that runs past
//    request_seconds has its answer written all the same (the deadline is for
//    a peer that stalls, not for an answer slow to make), and a timer that ran
//    out while it ran no longer closes the connection behind the answer's first
//    piece (ArmConnectionTimerAt_). A close in the middle of an answer - a
//    reader that stopped reading, the server stopping - is an abortive one, a
//    reset, which no client takes for the end of the answer (CloseNow_).
//
//  - The head is held to max_request_bytes WHILE it is read (HttpHeadMatch), not
//    once its blank line has arrived: a head that never ends is refused as soon
//    as it is over the cap, where it used to grow to the size of the largest
//    body any route takes before anything noticed.
//
//  - A connection cap, enforced at accept before the TLS handshake, because the
//    handshake is the expensive part and the cap exists to bound that cost.
//
//  - A body on disk is sent from disk (HttpResponse::body_file), 64 KB at a
//    time, so a download is never held in memory whole; and the large
//    downloads - a body file, or a body in memory of large_response_bytes or
//    more - are capped server-wide (max_large_responses) and given a
//    connection deadline that fits their size (download_bytes_per_second).
//    Until 26 September 2026 a file link was read whole into memory, copied
//    twice more on its way to the socket and cut off at the connection timer,
//    and 64 slow readers of a 500 MB file could hold 32 GB.
//
// What it does not do yet, and says so: bodies are read whole (bounded by
// max_request_bytes), and a response a handler builds in memory is held in
// memory, so neither streaming shape a webmail needs for a request body exists
// here; chunked request bodies are refused with 411.

#include "StdAfx.h"

#include "HttpServer.h"
#include "HttpCompression.h"
#include "FileUtilities.h"
#include "../TCPIP/AcceptLoop.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef HM_PLATFORM_POSIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // The end of a request's head, for async_read_until, with the head's cap
   // applied while it is read. The connection's buffer is sized for the
   // largest body any route takes (16 MB on the REST listener), and the plain
   // "\r\n\r\n" delimiter only gives up when that buffer is full - so until
   // 26 September 2026 each of the listener's connections could be made to
   // hold 16 MB of a head that never ended, for as long as the request timer
   // ran, before the 64 KB cap on a head was ever asked. Answered as a match
   // once the buffer holds more than the cap with no blank line in it; OnHead_
   // sees a head that does not end in one and refuses it with 413.
   class HttpHeadMatch
   {
   public:
      HttpHeadMatch(const boost::asio::streambuf &buffer, size_t cap) : buffer_(&buffer), cap_(cap) { }

      template <typename Iterator>
      std::pair<Iterator, bool> operator()(Iterator begin, Iterator end) const
      {
         static const char terminator[] = "\r\n\r\n";
         Iterator found = std::search(begin, end, terminator, terminator + 4);
         if (found != end)
            return std::make_pair(found + 4, true);

         if (buffer_->size() > cap_)
            return std::make_pair(end, true);

         // Resume where a terminator split across two reads could begin.
         Iterator resume = begin;
         if (std::distance(begin, end) > 3)
            resume = end - 3;
         return std::make_pair(resume, false);
      }

   private:
      const boost::asio::streambuf *buffer_;
      size_t cap_;
   };

   // A response's body file, read a piece at a time. Opened so that it can
   // still be deleted while it is being sent: the sender removing a file link,
   // or the expiry sweep, while somebody downloads it. On Windows a stream
   // opened without FILE_SHARE_DELETE would make that delete fail, retry for
   // four seconds and be reported as an error; here the name goes at once and
   // the bytes when the download lets go of them, as they do on POSIX.
   class HttpBodyFile
   {
   public:
      HttpBodyFile() { }
      ~HttpBodyFile() { Close(); }

      HttpBodyFile(const HttpBodyFile &) = delete;
      HttpBodyFile &operator=(const HttpBodyFile &) = delete;

      bool Open(const String &path, unsigned long long &size)
      {
         Close();

#ifdef HM_PLATFORM_POSIX
         const AnsiString narrow = path.c_str();
         descriptor_ = ::open(narrow.c_str(), O_RDONLY | O_CLOEXEC);
         if (descriptor_ < 0)
            return false;

         struct stat information;
         if (::fstat(descriptor_, &information) != 0 || !S_ISREG(information.st_mode))
         {
            Close();
            return false;
         }

         size = (unsigned long long) information.st_size;
         return true;
#else
         handle_ = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
         if (handle_ == INVALID_HANDLE_VALUE)
            return false;

         LARGE_INTEGER length;
         if (!::GetFileSizeEx(handle_, &length) || length.QuadPart < 0)
         {
            Close();
            return false;
         }

         size = (unsigned long long) length.QuadPart;
         return true;
#endif
      }

      // What was read into buffer: 0 at the end of the file or on an error,
      // which the caller tells apart by the bytes it still expected.
      size_t Read(char *buffer, size_t count)
      {
#ifdef HM_PLATFORM_POSIX
         if (descriptor_ < 0)
            return 0;

         for (;;)
         {
            const ssize_t got = ::read(descriptor_, buffer, count);
            if (got < 0 && errno == EINTR)
               continue;
            return got > 0 ? (size_t) got : 0;
         }
#else
         if (handle_ == INVALID_HANDLE_VALUE)
            return 0;

         DWORD got = 0;
         if (!::ReadFile(handle_, buffer, (DWORD) count, &got, nullptr))
            return 0;
         return (size_t) got;
#endif
      }

      void Close()
      {
#ifdef HM_PLATFORM_POSIX
         if (descriptor_ >= 0)
            ::close(descriptor_);
         descriptor_ = -1;
#else
         if (handle_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle_);
         handle_ = INVALID_HANDLE_VALUE;
#endif
      }

   private:
#ifdef HM_PLATFORM_POSIX
      int descriptor_ = -1;
#else
      HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
   };
}

namespace boost
{
   namespace asio
   {
      template <> struct is_match_condition<HM::HttpHeadMatch> : public std::true_type { };
   }
}

namespace HM
{
   namespace
   {
      // How long Stop() waits for the connections it closed to drain before
      // stopping the io_context outright. A handler blocked in the database is
      // waited for by the join in any case; this bounds only the socket
      // shutdowns.
      const int StopDrainMilliseconds = 5000;

      // The over-cap refusal is logged for the first refusal and then once per
      // this many, so a flood produces a line the operator can find rather than
      // a log the flood writes.
      const unsigned OverCapLogInterval = 1000;

      const char *HeadTerminator = "\r\n\r\n";

      // How much of a response one write hands the socket. Each piece re-arms
      // the request timer, so this is the progress a reader has to make every
      // request_seconds to keep the connection. A body file is read from disk
      // in pieces of the same size, one at a time.
      const size_t WriteChunkBytes = 64 * 1024;

      // The refusal of a large download over the cap is logged for the first
      // and then once per this many, as the connection cap's is.
      const unsigned LargeRefusalLogInterval = 100;

      // What a client refused a large download is told to wait, in seconds.
      const char *LargeRefusalRetryAfter = "30";

      AnsiString ToLower(const AnsiString &value)
      {
         AnsiString result = value;
         result.MakeLower();
         return result;
      }

      AnsiString Trim(const AnsiString &value)
      {
         const char *text = value.c_str();
         size_t start = 0;
         size_t end = value.size();

         while (start < end && (text[start] == ' ' || text[start] == '\t'))
            start++;
         while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
            end--;

         return AnsiString(value.substr(start, end - start));
      }

      // True when the comma-separated Connection header carries the token.
      bool HasConnectionToken(const AnsiString &lowerCaseValue, const char *token)
      {
         size_t position = 0;
         while (position <= lowerCaseValue.size())
         {
            size_t comma = lowerCaseValue.find(',', position);
            if (comma == std::string::npos)
               comma = lowerCaseValue.size();

            if (Trim(AnsiString(lowerCaseValue.substr(position, comma - position))) == token)
               return true;

            position = comma + 1;
         }

         return false;
      }

      IPAddress PeerAddress(const boost::asio::ip::tcp::endpoint &endpoint)
      {
         boost::asio::ip::address address = endpoint.address();

         // An IPv4 client of a dual-stack listener arrives as ::ffff:a.b.c.d.
         // Unmapped here - not cosmetics: every consumer of the result matches
         // on address family. The auto-ban exclusion compares against
         // "127.0.0.1", an AllowedFrom restriction written as an IPv4 range
         // refuses any IPv6 peer outright, and the security ranges the auto-ban
         // creates are IPv4 ranges. A v4 client dressed as v6 would silently
         // match none of them - including the loopback exclusion.
         if (address.is_v6() && address.to_v6().is_v4_mapped())
            address = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());

         return IPAddress(address);
      }
   }

   AnsiString
   HttpRequest::Header(const AnsiString &lowerCaseName) const
   {
      for (size_t i = 0; i < headers.size(); i++)
      {
         if (headers[i].first == lowerCaseName)
            return headers[i].second;
      }

      return "";
   }

   struct HttpServer::Listener
   {
      Listener(boost::asio::io_context &io, int listen_port, std::shared_ptr<boost::asio::ssl::context> tls_context) :
         acceptor(io),
         tls(tls_context),
         port(listen_port)
      {
      }

      boost::asio::ip::tcp::acceptor acceptor;
      std::shared_ptr<boost::asio::ssl::context> tls;
      int port;
   };

   // One accepted socket, from accept to close. Every method runs inside
   // strand_.
   //
   // The connection holds the server weakly and copies what it needs from it
   // at construction. A strong reference would be a cycle: a handler queued on
   // the io_context holds the connection, the connection would hold the
   // server, and the server owns the io_context - so a server stopped with
   // handlers still queued could never be destroyed.
   class HttpConnection : public std::enable_shared_from_this<HttpConnection>
   {
   public:
      HttpConnection(std::shared_ptr<HttpServer> server, boost::asio::ip::tcp::socket socket,
                     std::shared_ptr<boost::asio::ssl::context> tls, int listen_port, const IPAddress &peer) :
         server_(server),
         limits_(server->limits_),
         handler_(server->handler_),
         error_responder_(server->error_responder_),
         large_request_filter_(server->large_request_filter_),
         large_request_ceiling_(server->large_request_ceiling_),
         strand_(boost::asio::make_strand(server->io_)),
         socket_(std::move(socket)),
         request_timer_(server->io_),
         connection_timer_(server->io_),
         buffer_(std::max(server->limits_.max_request_bytes, server->limits_.max_request_bytes_large) + 1),
         peer_(peer),
         listen_port_(listen_port),
         requests_(0),
         content_length_(0),
         keep_alive_(false),
         closed_(false)
      {
         if (tls)
            tls_.reset(new boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>(socket_, *tls));
      }

      // A connection let go without CloseNow_ - the server stopped with it
      // queued - still gives back its large-download slot and its temporary
      // body file.
      ~HttpConnection()
      {
         ReleaseLargeResponse_();
      }

      void Start()
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();

         boost::asio::post(strand_, [self]()
         {
            self->ArmConnectionTimer_();

            if (self->tls_)
               self->Handshake_();
            else
               self->ReadHead_();
         });
      }

      // From Stop(): closes whatever the connection is doing.
      void Close()
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();

         boost::asio::post(strand_, [self]()
         {
            self->CloseNow_();
         });
      }

   private:
      void ArmConnectionTimer_()
      {
         ArmConnectionTimerAt_(std::chrono::steady_clock::now() + std::chrono::seconds(limits_.connection_seconds));
      }

      // The one place the connection timer is set, and the request timer's
      // below the one place that is. Each setting is numbered, and a wait that
      // completes with a number that is no longer the latest does nothing.
      // Moving a deadline cancels the wait pending on the old one - but only a
      // wait that has not already completed: one whose time ran out while a
      // handler held the strand has its completion queued behind the handler
      // by then, cannot be cancelled, and arrives with success. Until 7 October
      // 2026 that completion closed the connection on the strand's next turn,
      // which came just after the handler's answer had handed the socket its
      // first 64 KB: a search that took longer than request_seconds answered
      // 65,413 bytes of its 67,996 and was closed - measured on server1 with
      // the full-text index on, Python's IncompleteRead on every such answer.
      void ArmConnectionTimerAt_(std::chrono::steady_clock::time_point when)
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();
         const unsigned long long arm = ++connection_timer_arms_;

         connection_timer_.expires_at(when);
         connection_timer_.async_wait(boost::asio::bind_executor(strand_,
            [self, arm](const boost::system::error_code &error)
            {
               if (!error && arm == self->connection_timer_arms_)
                  self->CloseNow_();
            }));
      }

      void ArmRequestTimer_()
      {
         ArmRequestTimer_(limits_.request_seconds);
      }

      void ArmRequestTimer_(unsigned seconds)
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();
         const unsigned long long arm = ++request_timer_arms_;

         // expires_after cancels a wait still pending, whose handler then runs
         // with operation_aborted and does nothing; one that completed already
         // is told apart by its number (ArmConnectionTimerAt_).
         request_timer_.expires_after(std::chrono::seconds(seconds));
         request_timer_.async_wait(boost::asio::bind_executor(strand_,
            [self, arm](const boost::system::error_code &error)
            {
               if (!error && arm == self->request_timer_arms_)
                  self->CloseNow_();
            }));
      }

      void Handshake_()
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();

         // The handshake counts against the first request's deadline: a client
         // that connects and never completes it is the same client as one that
         // never sends a request line.
         ArmRequestTimer_();

         tls_->async_handshake(boost::asio::ssl::stream_base::server, boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code &error)
            {
               if (self->closed_)
                  return;

               if (error)
               {
                  self->CloseNow_();
                  return;
               }

               self->ReadHead_();
            }));
      }

      void ReadHead_()
      {
         if (closed_)
            return;

         std::shared_ptr<HttpConnection> self = shared_from_this();

         ArmRequestTimer_();

         auto completion = boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code &error, size_t bytes)
            {
               self->OnHead_(error, bytes);
            });

         const HttpHeadMatch headEnd(buffer_, limits_.max_request_bytes);

         if (tls_)
            boost::asio::async_read_until(*tls_, buffer_, headEnd, completion);
         else
            boost::asio::async_read_until(socket_, buffer_, headEnd, completion);
      }

      void OnHead_(const boost::system::error_code &error, size_t bytes)
      {
         if (closed_)
            return;

         if (error)
         {
            // The buffer filled without a blank line: the head alone is over the
            // cap. Says nothing about the cap; an administrator hitting this
            // reads the documentation, a stranger measuring it learns nothing.
            if (error == boost::asio::error::not_found)
            {
               // On a kept-alive connection request_ still holds the request
               // before this one, and Write_ negotiates the refusal's coding
               // against whatever request_ holds; the refusal must not be
               // encoded on an Accept-Encoding this request never sent.
               request_ = HttpRequest();
               Fail_(413, "request too large");
               return;
            }

            // EOF while waiting for a request is the normal end of a kept-alive
            // connection. Anything else - a reset, a cancelled read - ends it
            // the same way.
            CloseNow_();
            return;
         }

         request_ = HttpRequest();
         request_.head.assign(boost::asio::buffers_begin(buffer_.data()), boost::asio::buffers_begin(buffer_.data()) + bytes);

         // HttpHeadMatch answered past the cap with no blank line in sight:
         // the head alone is over the cap, refused as the full buffer above is.
         if (request_.head.size() < 4 || request_.head.compare(request_.head.size() - 4, 4, HeadTerminator) != 0)
         {
            request_ = HttpRequest();
            Fail_(413, "request too large");
            return;
         }

         request_.peer = peer_;
         request_.over_tls = tls_ != nullptr;
         request_.listen_port = listen_port_;

         buffer_.consume(bytes);

         if (!ParseHead_())
         {
            Fail_(400, "malformed request");
            return;
         }

         // The head alone is held to the first cap whatever the route. The
         // body may be held to the second when the filter grants it - and
         // then the request gets the longer timer, since the body is the
         // point and takes time to arrive.
         size_t cap = limits_.max_request_bytes;
         if (request_.head.size() <= limits_.max_request_bytes && limits_.max_request_bytes_large > cap &&
             large_request_filter_ && large_request_filter_(request_.method, request_.target))
         {
            cap = limits_.max_request_bytes_large;

            // A granted route with a ceiling of its own below the large cap -
            // a form an identity provider posts - is held to that instead,
            // and keeps the ordinary timer: the longer one is for a body the
            // size of the large cap.
            bool ownCeiling = false;
            if (large_request_ceiling_)
            {
               const size_t own = large_request_ceiling_(request_.method, request_.target);
               if (own > limits_.max_request_bytes && own < cap)
               {
                  cap = own;
                  ownCeiling = true;
               }
            }

            if (!ownCeiling && limits_.request_seconds_large > limits_.request_seconds)
               ArmRequestTimer_(limits_.request_seconds_large);
         }

         if (request_.head.size() > limits_.max_request_bytes || request_.head.size() + content_length_ > cap)
         {
            // Refused before the body is read. The client may still be sending
            // it; the close that follows the response resets what is in flight,
            // which is the point - nothing of an oversized request is acted on.
            Fail_(413, "request too large");
            return;
         }

         size_t buffered = buffer_.size();
         size_t remaining = content_length_ > buffered ? content_length_ - buffered : 0;

         if (remaining > 0 && ToLower(request_.Header("expect")) == "100-continue")
         {
            // RFC 7231 section 5.1.1: the client is waiting to be told to send
            // the body it has declared. Answered now that the declaration has
            // passed the cap, so a refused body is never sent at all.
            WriteContinue_(remaining);
            return;
         }

         ReadBody_(remaining);
      }

      // Splits the head into the request line and the headers. False when the
      // request line is not three tokens with an HTTP/1.x version, or a header
      // line has no colon, or the head holds a NUL - there is no legitimate NUL
      // in a request line or a header, and a request cut at one would be
      // processed as a different string from the one the lengths described.
      bool ParseHead_()
      {
         content_length_ = 0;

         if (request_.head.find('\0') != std::string::npos)
            return false;

         size_t lineEnd = request_.head.find("\r\n");
         if (lineEnd == std::string::npos)
            return false;

         AnsiString requestLine(request_.head.substr(0, lineEnd));

         size_t firstSpace = requestLine.find(' ');
         size_t lastSpace = requestLine.rfind(' ');
         if (firstSpace == std::string::npos || lastSpace == firstSpace)
            return false;

         request_.method = AnsiString(requestLine.substr(0, firstSpace));
         request_.target = AnsiString(requestLine.substr(firstSpace + 1, lastSpace - firstSpace - 1));
         request_.version = AnsiString(requestLine.substr(lastSpace + 1));

         if (request_.method.empty() || request_.target.empty() ||
             request_.version.size() != 8 || request_.version.compare(0, 7, "HTTP/1.") != 0)
            return false;

         size_t position = lineEnd + 2;
         while (position < request_.head.size())
         {
            size_t end = request_.head.find("\r\n", position);
            if (end == std::string::npos)
               break;

            if (end == position)
               break;   // the blank line

            AnsiString line(request_.head.substr(position, end - position));
            size_t colon = line.find(':');
            if (colon == std::string::npos)
               return false;

            AnsiString name = ToLower(Trim(AnsiString(line.substr(0, colon))));
            AnsiString value = Trim(AnsiString(line.substr(colon + 1)));

            request_.headers.push_back(std::make_pair(name, value));

            position = end + 2;
         }

         AnsiString contentLength = request_.Header("content-length");
         if (!contentLength.empty())
         {
            if (contentLength.size() > 9 || contentLength.find_first_not_of("0123456789") != std::string::npos)
               return false;

            content_length_ = static_cast<size_t>(strtoul(contentLength.c_str(), nullptr, 10));
         }

         return true;
      }

      void WriteContinue_(size_t remaining)
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();

         out_ = "HTTP/1.1 100 Continue\r\n\r\n";

         auto completion = boost::asio::bind_executor(strand_,
            [self, remaining](const boost::system::error_code &error, size_t)
            {
               if (self->closed_)
                  return;

               if (error)
               {
                  self->CloseNow_();
                  return;
               }

               self->ReadBody_(remaining);
            });

         if (tls_)
            boost::asio::async_write(*tls_, boost::asio::buffer(out_.data(), out_.size()), completion);
         else
            boost::asio::async_write(socket_, boost::asio::buffer(out_.data(), out_.size()), completion);
      }

      void ReadBody_(size_t remaining)
      {
         if (remaining == 0)
         {
            OnBody_(boost::system::error_code(), 0);
            return;
         }

         std::shared_ptr<HttpConnection> self = shared_from_this();

         auto completion = boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code &error, size_t bytes)
            {
               self->OnBody_(error, bytes);
            });

         if (tls_)
            boost::asio::async_read(*tls_, buffer_, boost::asio::transfer_exactly(remaining), completion);
         else
            boost::asio::async_read(socket_, buffer_, boost::asio::transfer_exactly(remaining), completion);
      }

      void OnBody_(const boost::system::error_code &error, size_t)
      {
         if (closed_)
            return;

         if (error)
         {
            CloseNow_();
            return;
         }

         request_.body.assign(boost::asio::buffers_begin(buffer_.data()), boost::asio::buffers_begin(buffer_.data()) + content_length_);
         buffer_.consume(content_length_);

         // A NUL in a body is a text body that is not text, and is refused so
         // that no handler parses it as a string. A body declared binary -
         // application/octet-stream, a file's bytes uploaded in pieces - is
         // taken as it is: every byte value is data there.
         const AnsiString contentType = ToLower(request_.Header("content-type"));
         if (!contentType.StartsWith("application/octet-stream") && request_.body.find('\0') != std::string::npos)
         {
            Fail_(400, "malformed request");
            return;
         }

         request_.raw = request_.head + request_.body;

         if (!request_.Header("transfer-encoding").empty())
         {
            // A body this server would have to de-chunk to measure. Refused
            // rather than guessed at: the cap is on bytes, and a chunked body
            // has no declared size to hold against it.
            Fail_(411, "length required");
            return;
         }

         // Keep-alive is the HTTP/1.1 default and the HTTP/1.0 exception.
         AnsiString connection = ToLower(request_.Header("connection"));
         if (request_.version == "HTTP/1.1")
            keep_alive_ = !HasConnectionToken(connection, "close");
         else
            keep_alive_ = HasConnectionToken(connection, "keep-alive");

         requests_++;
         std::shared_ptr<HttpServer> server = server_.lock();
         if (requests_ >= limits_.max_requests_per_connection || !server || !server->running_)
            keep_alive_ = false;
         server.reset();

         HttpResponse response;
         try
         {
            response = handler_(request_);
         }
         catch (...)
         {
            response = error_responder_(500, "internal error");
            response.close = true;
         }

         if (response.close)
            keep_alive_ = false;

         Write_(response);
      }

      // A response the server gives on its own account. The connection is
      // closed afterwards: what is on the wire after a malformed or oversized
      // request cannot be framed.
      void Fail_(int status, const AnsiString &message)
      {
         HttpResponse response = error_responder_(status, message);
         keep_alive_ = false;
         Write_(response);
      }

      void Write_(HttpResponse &response)
      {
         // A body on disk is sent from disk, never compressed.
         if (!response.body_file.IsEmpty())
         {
            WriteFile_(response);
            return;
         }

         // The last thing done to an answer before it becomes bytes, and the
         // only place it is done: every response of both listeners - a
         // handler's, and the server's own refusals from Fail_ - passes through
         // here, and Serialize below takes Content-Length from whatever body it
         // is handed, so the framing follows the coding without being told.
         if (limits_.compression)
            HttpCompression::Apply(request_, response);

         // A large body held in memory is a large download: counted against the
         // cap - a refusal lets go of the body now rather than when a slow
         // reader has taken it - and given the time its size needs.
         if (limits_.large_response_bytes > 0 && (size_t) response.body.GetLength() >= limits_.large_response_bytes)
         {
            if (!TakeLargeResponse_())
            {
               RefuseLarge_();
               return;
            }

            // Closed after it, as a body file's connection is, so the longer
            // deadline is this download's and not the connection's.
            keep_alive_ = false;
         }

         // Whatever its size, an answer is begun only under a connection
         // deadline it can be written in; one that would come first - a
         // kept-alive connection near the end of its life, or a handler that
         // ran past it - is moved out, and the connection closed after this
         // answer, so the connection's life still ends where it did, give or
         // take the one answer it was in the middle of.
         if (ExtendConnectionDeadline_(response.body.GetLength()))
            keep_alive_ = false;

         out_ = HttpServer::Serialize(response, keep_alive_);
         written_ = 0;

         BeginResponse_();
      }

      // A response whose body is a file: the head now, the file's bytes a
      // piece at a time after it (WriteNext_ refills out_ from the file). The
      // connection is closed after it, so a download's longer deadline does not
      // become the connection's for whatever it is asked next.
      void WriteFile_(HttpResponse &response)
      {
         body_file_path_ = response.body_file;
         body_file_delete_ = response.body_file_delete;
         keep_alive_ = false;

         unsigned long long size = 0;
         if (!body_file_.Open(body_file_path_, size))
         {
            ReleaseLargeResponse_();
            HttpResponse missing = error_responder_(404, "not found");
            Write_(missing);
            return;
         }

         if (!TakeLargeResponse_())
         {
            ReleaseLargeResponse_();
            RefuseLarge_();
            return;
         }

         ExtendConnectionDeadline_(size);

         body_file_remaining_ = size;
         out_ = HttpServer::SerializeHead(response, false, size);
         written_ = 0;

         BeginResponse_();
      }

      // The next piece of a body file into out_. False when the file ended
      // before its declared size - it was truncated under us - which leaves
      // the response's framing a lie, so the caller closes the connection.
      bool RefillFromFile_()
      {
         const size_t wanted = (size_t) std::min<unsigned long long>(WriteChunkBytes, body_file_remaining_);

         out_.resize(wanted);
         const size_t got = body_file_.Read(&out_[0], wanted);
         if (got == 0)
            return false;

         out_.resize(got);
         body_file_remaining_ -= got;
         written_ = 0;
         return true;
      }

      // One large download too many: 503, with the time to wait, and closed.
      void RefuseLarge_()
      {
         std::shared_ptr<HttpServer> server = server_.lock();
         if (server)
         {
            const unsigned refused = ++server->refused_large_;
            if (refused == 1 || refused % LargeRefusalLogInterval == 0)
            {
               String message;
               message.Format(_T("%s: Refused a large download to %s - %u are being written, which is the limit. %u refused so far."),
                  String(server->name_).c_str(), String(peer_.ToString()).c_str(), limits_.max_large_responses, refused);
               LOG_APPLICATION(message);
            }
         }
         server.reset();

         HttpResponse busy = error_responder_(503, "too many large downloads at once; try again shortly");
         busy.extra_headers += AnsiString("Retry-After: ") + LargeRefusalRetryAfter + "\r\n";
         keep_alive_ = false;
         ExtendConnectionDeadline_(busy.body.GetLength());

         out_ = HttpServer::Serialize(busy, false);
         written_ = 0;
         BeginResponse_();
      }

      bool TakeLargeResponse_()
      {
         if (holds_large_response_)
            return true;

         std::shared_ptr<HttpServer> server = server_.lock();
         if (!server || !server->TakeLargeResponse_())
            return false;

         holds_large_response_ = true;
         return true;
      }

      // The slot back, and the body file closed - and removed, when it was a
      // temporary one. Safe to call more than once.
      void ReleaseLargeResponse_()
      {
         if (holds_large_response_)
         {
            holds_large_response_ = false;

            std::shared_ptr<HttpServer> server = server_.lock();
            if (server)
               server->GiveBackLargeResponse_();
         }

         body_file_.Close();
         body_file_remaining_ = 0;

         if (body_file_delete_ && !body_file_path_.IsEmpty())
            FileUtilities::DeleteFile(body_file_path_);

         body_file_delete_ = false;
         body_file_path_ = String();
      }

      // The connection timer moved out, never in, to the time bytes take at
      // download_bytes_per_second, with request_seconds to spare. True when it
      // moved: the connection's own deadline would have come first.
      bool ExtendConnectionDeadline_(unsigned long long bytes)
      {
         unsigned long long seconds = limits_.request_seconds;
         if (limits_.download_bytes_per_second > 0)
            seconds += bytes / limits_.download_bytes_per_second;

         const std::chrono::steady_clock::time_point wanted = std::chrono::steady_clock::now() + std::chrono::seconds((long long) seconds);

         if (connection_timer_.expiry() >= wanted)
            return false;

         ArmConnectionTimerAt_(wanted);
         return true;
      }

      // An answer's first byte is about to be handed to the socket. From here
      // until its last one has been, a close is an abortive one (CloseNow_).
      void BeginResponse_()
      {
         response_started_ = true;
         WriteNext_();
      }

      // One piece of out_, with the request timer armed afresh for it: the
      // deadline on a response is progress, a piece every request_seconds,
      // under the connection timer's absolute one. A single write of the whole
      // response under the request's own deadline cut every download off that
      // the reader could not take in within request_seconds.
      void WriteNext_()
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();

         ArmRequestTimer_();

         const size_t piece = std::min(WriteChunkBytes, out_.size() - written_);

         auto completion = boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code &error, size_t bytes)
            {
               if (self->closed_)
                  return;

               self->written_ += bytes;

               if (!error && self->written_ >= self->out_.size() && self->body_file_remaining_ > 0)
               {
                  // The head, or the last piece of a body file, is out: the
                  // next piece from disk.
                  if (!self->RefillFromFile_())
                  {
                     self->CloseNow_();
                     return;
                  }

                  self->WriteNext_();
                  return;
               }

               if (error || self->written_ >= self->out_.size())
               {
                  self->OnWritten_(error);
                  return;
               }

               self->WriteNext_();
            });

         if (tls_)
            boost::asio::async_write(*tls_, boost::asio::buffer(out_.data() + written_, piece), completion);
         else
            boost::asio::async_write(socket_, boost::asio::buffer(out_.data() + written_, piece), completion);
      }

      void OnWritten_(const boost::system::error_code &error)
      {
         if (closed_)
            return;

         // Every byte of the answer is with the socket, unless the write
         // failed - and then the close that follows may as well be abortive.
         if (!error)
            response_started_ = false;

         // A large download is done with, written or not.
         ReleaseLargeResponse_();

         // The memory a large response held goes now, not with the connection.
         if (out_.capacity() > WriteChunkBytes)
            std::string().swap(out_);

         if (error || !keep_alive_)
         {
            Shutdown_();
            return;
         }

         // The next request may already be in the buffer (pipelining);
         // async_read_until returns at once in that case.
         ReadHead_();
      }

      // The orderly end: a TLS close-notify where there is TLS, then the
      // socket. The request timer still runs, so a peer that never
      // acknowledges the shutdown is closed when it fires.
      void Shutdown_()
      {
         std::shared_ptr<HttpConnection> self = shared_from_this();

         if (tls_)
         {
            tls_->async_shutdown(boost::asio::bind_executor(strand_,
               [self](const boost::system::error_code &)
               {
                  self->CloseNow_();
               }));
            return;
         }

         boost::system::error_code ignored;
         socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored);
         CloseNow_();
      }

      void CloseNow_()
      {
         if (closed_)
            return;

         closed_ = true;

         boost::system::error_code ignored;
         request_timer_.cancel();
         connection_timer_.cancel();

         // Closed in the middle of an answer - a reader that stopped reading for
         // request_seconds, a download past its connection deadline, the server
         // stopping - is reset, not ended: an orderly close there is an end of
         // stream a client may read as the end of the answer, and a reset is
         // an error no client mistakes for one. What was still to be sent is
         // let go at once rather than sent after the close.
         if (response_started_)
            socket_.set_option(boost::asio::socket_base::linger(true, 0), ignored);

         socket_.close(ignored);

         ReleaseLargeResponse_();

         std::shared_ptr<HttpServer> server = server_.lock();
         if (server)
            server->Untrack_(this);
      }

      std::weak_ptr<HttpServer> server_;
      HttpLimits limits_;
      HttpServer::Handler handler_;
      HttpServer::ErrorResponder error_responder_;
      HttpServer::LargeRequestFilter large_request_filter_;
      HttpServer::LargeRequestCeiling large_request_ceiling_;
      boost::asio::strand<boost::asio::io_context::executor_type> strand_;
      boost::asio::ip::tcp::socket socket_;
      std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> tls_;
      boost::asio::steady_timer request_timer_;
      boost::asio::steady_timer connection_timer_;
      boost::asio::streambuf buffer_;
      IPAddress peer_;
      int listen_port_;
      unsigned requests_;

      HttpRequest request_;
      size_t content_length_;
      bool keep_alive_;
      bool closed_;
      std::string out_;
      size_t written_ = 0;

      // Which setting of each timer is the latest (ArmConnectionTimerAt_), and
      // whether an answer has begun and is not yet all with the socket
      // (BeginResponse_, CloseNow_).
      unsigned long long connection_timer_arms_ = 0;
      unsigned long long request_timer_arms_ = 0;
      bool response_started_ = false;

      // A response's body file while it is sent, and whether this connection
      // holds one of the server's large-download slots.
      HttpBodyFile body_file_;
      String body_file_path_;
      bool body_file_delete_ = false;
      unsigned long long body_file_remaining_ = 0;
      bool holds_large_response_ = false;
   };

   HttpServer::HttpServer(const AnsiString &name, const HttpLimits &limits, Handler handler,
                          AcceptFilter accept_filter, ErrorResponder error_responder) :
      name_(name),
      limits_(limits),
      handler_(handler),
      accept_filter_(accept_filter),
      error_responder_(error_responder),
      running_(false),
      refused_over_cap_(0),
      pending_accepts_(0),
      large_responses_(0),
      refused_large_(0),
      dropped_on_accept_(0)
   {
      if (limits_.worker_threads == 0)
         limits_.worker_threads = 1;
      if (limits_.max_connections == 0)
         limits_.max_connections = 1;
      if (limits_.max_requests_per_connection == 0)
         limits_.max_requests_per_connection = 1;

      if (!error_responder_)
      {
         error_responder_ = [](int status, const AnsiString &message)
         {
            HttpResponse response;
            response.status = status;
            response.content_type = "text/plain";
            response.body = message;
            return response;
         };
      }
   }

   HttpServer::~HttpServer()
   {
      Stop();
   }

   bool
   HttpServer::Listen(const String &bind_address, int port, std::shared_ptr<boost::asio::ssl::context> tls)
   {
      // "localhost" is a name, and this listener binds addresses. The IPv4
      // loopback is what it always meant here.
      AnsiString narrowBindAddress = bind_address == _T("localhost") ? AnsiString("127.0.0.1") : AnsiString(bind_address);

      // Read as IPAddress reads an address on every platform, so that a bind
      // address Linux refuses is refused on Windows too (127.1, 0x7f.1).
      boost::asio::ip::address address;
      if (!IPAddress::Parse(narrowBindAddress, address))
      {
         LOG_APPLICATION(String(name_) + ": Invalid bind address: " + bind_address);
         return false;
      }

      std::shared_ptr<Listener> listener(new Listener(io_, port, tls));
      boost::asio::ip::tcp::endpoint endpoint(address, static_cast<unsigned short>(port));

      boost::system::error_code error;
      listener->acceptor.open(endpoint.protocol(), error);
      if (error)
      {
         LOG_APPLICATION(String(name_) + ": Could not open a socket for " + FormatEndpoint(bind_address, port) + ": " + String(error.message().c_str()));
         return false;
      }

      // The unspecified IPv6 address means both families, as it does for the
      // mail protocols. A specific IPv6 address is left alone: it can only ever
      // accept IPv6, and clearing the option would be a no-op that reads as a
      // promise.
      if (address.is_v6() && address.is_unspecified())
      {
         listener->acceptor.set_option(boost::asio::ip::v6_only(false), error);
         if (error)
            LOG_APPLICATION(String(name_) + ": IPV6_V6ONLY could not be cleared for the :: bind, so this listener will accept IPv6 connections only. Bind 0.0.0.0 instead if IPv4 is what is needed.");
      }

#ifdef HM_PLATFORM_POSIX
      // SO_REUSEADDR on POSIX only, as TCPServer and AcceptLoop set it: there it
      // lets a reinitialise bind while the last connections linger in TIME_WAIT;
      // on Windows it let this listener bind a port another socket with the option
      // was listening on, and report itself started while the connections went to
      // the other (AcceptLoop::AllowRebindPastTimeWait has the measurements).
      listener->acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), error);
#else
      // SO_EXCLUSIVEADDRUSE, so that no other socket takes this port's requests on
      // a more specific address and this listener does not start on the wildcard
      // while another holds one address of the port (AcceptLoop::
      // ClaimPortExclusively has the measurements). The REST and web-services
      // listeners each bind one address of their own setting per port, so none of
      // the server's own listeners is refused by it.
      AcceptLoop::ClaimPortExclusively(listener->acceptor.native_handle());
#endif

      listener->acceptor.bind(endpoint, error);
      if (!error)
         listener->acceptor.listen(boost::asio::socket_base::max_listen_connections, error);

      if (error)
      {
         LOG_APPLICATION(String(name_) + ": Failed to bind to " + FormatEndpoint(bind_address, port) + ". Is the port in use?");
         return false;
      }

      listeners_.push_back(listener);
      return true;
   }

   void
   HttpServer::SetLargeRequestFilter(LargeRequestFilter filter)
   {
      large_request_filter_ = filter;
   }

   void
   HttpServer::SetLargeRequestCeiling(LargeRequestCeiling ceiling)
   {
      large_request_ceiling_ = ceiling;
   }

   void
   HttpServer::Start()
   {
      if (running_)
         return;

      running_ = true;

      work_.reset(new boost::asio::executor_work_guard<boost::asio::io_context::executor_type>(io_.get_executor()));

      for (size_t i = 0; i < listeners_.size(); i++)
         Accept_(listeners_[i]);

      for (unsigned i = 0; i < limits_.worker_threads; i++)
         threads_.push_back(std::thread(&HttpServer::RunWorker_, this));
   }

   void
   HttpServer::RunWorker_()
   {
      // The top frame of a worker. An exception that escapes a completion
      // handler - a handler's own are caught in OnBody_ - would otherwise be
      // std::terminate, a dead server; here it is logged and the worker goes
      // back to work. The loop ends when the io_context has been stopped.
      for (;;)
      {
         try
         {
            io_.run();
            return;
         }
         catch (...)
         {
            LOG_APPLICATION(String(name_) + ": An exception escaped a worker thread. The worker continues.");
         }

         if (io_.stopped())
            return;
      }
   }

   void
   HttpServer::Accept_(std::shared_ptr<Listener> listener)
   {
      // Weak, for the reason given at HttpConnection: the queued accept must
      // not keep the server - and with it the io_context the accept is queued
      // on - alive.
      std::weak_ptr<HttpServer> weak = shared_from_this();
      std::shared_ptr<boost::asio::ip::tcp::socket> socket(new boost::asio::ip::tcp::socket(io_));

      pending_accepts_++;

      listener->acceptor.async_accept(*socket, [weak, listener, socket](const boost::system::error_code &error)
      {
         std::shared_ptr<HttpServer> self = weak.lock();
         if (!self)
            return;

         self->pending_accepts_--;

         if (!self->running_)
            return;

         if (error)
         {
            // The acceptor was closed by Stop(), or accept failed for a reason
            // that leaves it usable (a peer that reset before it was accepted).
            // The first ends the loop; the second continues it.
            if (error == boost::asio::error::operation_aborted || !listener->acceptor.is_open())
               return;

            self->Accept_(listener);
            return;
         }

         self->Accept_(listener);

         try
         {
            boost::system::error_code endpointError;
            boost::asio::ip::tcp::endpoint endpoint = socket->remote_endpoint(endpointError);

            // A failed lookup leaves the peer as 0.0.0.0, which every non-empty
            // source restriction refuses and no auto-ban matches. Failing closed
            // is the right direction.
            IPAddress peer = endpointError ? IPAddress() : PeerAddress(endpoint);

            if (self->accept_filter_ && !self->accept_filter_(peer))
            {
               boost::system::error_code ignored;
               socket->close(ignored);
               return;
            }

            std::shared_ptr<HttpConnection> connection(new HttpConnection(self, std::move(*socket), listener->tls, listener->port, peer));

            if (!self->Track_(connection))
            {
               unsigned refused = ++self->refused_over_cap_;
               if (refused == 1 || refused % OverCapLogInterval == 0)
               {
                  String message;
                  message.Format(_T("%s: Refused a connection from %s - %u connections are open, which is the limit. %u refused so far."),
                     String(self->name_).c_str(), String(peer.ToString()).c_str(), self->limits_.max_connections, refused);
                  LOG_APPLICATION(message);
               }

               // The connection object closes the socket when it goes.
               return;
            }

            connection->Start();
         }
         catch (...)
         {
            // Constructing the connection can only realistically fail by
            // bad_alloc; the socket is closed by its destructor either way.
            // Said, as RunWorker_ says what escapes a worker - for the first
            // connection and then once per OverCapLogInterval with the count, so
            // that an error that stops every connection writes a line and not a
            // line per connection. The next accept is queued already; a throw
            // from the log itself goes to RunWorker_, which says it and goes on.
            const unsigned dropped = ++self->dropped_on_accept_;
            if (dropped == 1 || dropped % OverCapLogInterval == 0)
            {
               String message;
               message.Format(_T("%s: An accepted connection could not be started: an unexpected error stopped it, and it was closed. The listener goes on accepting. %u so far."),
                  String(self->name_).c_str(), dropped);
               LOG_APPLICATION(message);
            }
         }
      });
   }

   bool
   HttpServer::Track_(std::shared_ptr<HttpConnection> connection)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      if (connections_.size() >= limits_.max_connections)
         return false;

      connections_.insert(connection);
      return true;
   }

   void
   HttpServer::Untrack_(HttpConnection *connection)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      for (std::set<std::shared_ptr<HttpConnection>>::iterator iter = connections_.begin(); iter != connections_.end(); ++iter)
      {
         if (iter->get() == connection)
         {
            connections_.erase(iter);
            return;
         }
      }
   }

   bool
   HttpServer::TakeLargeResponse_()
   {
      // Zero is no cap: counted, never refused.
      if (limits_.max_large_responses == 0)
      {
         large_responses_++;
         return true;
      }

      unsigned current = large_responses_.load();
      do
      {
         if (current >= limits_.max_large_responses)
            return false;
      } while (!large_responses_.compare_exchange_weak(current, current + 1));

      return true;
   }

   void
   HttpServer::GiveBackLargeResponse_()
   {
      unsigned current = large_responses_.load();
      while (current > 0 && !large_responses_.compare_exchange_weak(current, current - 1))
      {
      }
   }

   size_t
   HttpServer::GetConnectionCount() const
   {
      std::lock_guard<std::mutex> guard(mutex_);
      return connections_.size();
   }

   int
   HttpServer::GetBoundPort() const
   {
      if (listeners_.empty())
         return 0;

      boost::system::error_code error;
      const boost::asio::ip::tcp::endpoint endpoint = listeners_[0]->acceptor.local_endpoint(error);
      return error ? 0 : (int) endpoint.port();
   }

   void
   HttpServer::Stop()
   {
      if (!running_)
      {
         listeners_.clear();
         return;
      }

      running_ = false;

      // The acceptors first, on the io_context, so no accept completes after
      // this point has passed; then every connection, each on its own strand.
      std::vector<std::shared_ptr<Listener>> listeners = listeners_;
      boost::asio::post(io_, [listeners]()
      {
         for (size_t i = 0; i < listeners.size(); i++)
         {
            boost::system::error_code ignored;
            listeners[i]->acceptor.close(ignored);
         }
      });

      std::vector<std::shared_ptr<HttpConnection>> connections;
      {
         std::lock_guard<std::mutex> guard(mutex_);
         connections.assign(connections_.begin(), connections_.end());
      }

      for (size_t i = 0; i < connections.size(); i++)
         connections[i]->Close();

      connections.clear();

      // Let the closes and the aborted accepts run, so that nothing is left
      // queued holding a connection; then stop the context whether or not they
      // all did. A handler that is in the database keeps its thread until it
      // returns; the join below waits for it, as Stop() always has.
      for (int waited = 0; waited < StopDrainMilliseconds; waited += 50)
      {
         if (GetConnectionCount() == 0 && pending_accepts_ == 0)
            break;

         std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      work_.reset();
      io_.stop();

      for (size_t i = 0; i < threads_.size(); i++)
      {
         if (threads_[i].joinable())
            threads_[i].join();
      }

      threads_.clear();

      {
         std::lock_guard<std::mutex> guard(mutex_);
         connections_.clear();
      }

      listeners_.clear();
      io_.restart();
   }

   AnsiString
   HttpServer::StatusText(int &status)
   {
      switch (status)
      {
      case 100: return "Continue";
      case 200: return "OK";
      case 201: return "Created";
      case 202: return "Accepted";
      case 204: return "No Content";
      case 207: return "Multi-Status";
      case 301: return "Moved Permanently";
      case 302: return "Found";
      case 304: return "Not Modified";
      case 400: return "Bad Request";
      case 401: return "Unauthorized";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 409: return "Conflict";
      case 411: return "Length Required";
      case 413: return "Payload Too Large";
      case 415: return "Unsupported Media Type";
      case 429: return "Too Many Requests";
      case 500: return "Internal Server Error";
      case 501: return "Not Implemented";
      case 502: return "Bad Gateway";
      case 503: return "Service Unavailable";
      case 504: return "Gateway Timeout";
      case 203: return "Non-Authoritative Information";
      case 205: return "Reset Content";
      case 206: return "Partial Content";
      case 303: return "See Other";
      case 307: return "Temporary Redirect";
      case 308: return "Permanent Redirect";
      case 402: return "Payment Required";
      case 406: return "Not Acceptable";
      case 408: return "Request Timeout";
      case 410: return "Gone";
      case 412: return "Precondition Failed";
      case 414: return "URI Too Long";
      case 416: return "Range Not Satisfiable";
      case 417: return "Expectation Failed";
      case 422: return "Unprocessable Content";
      case 423: return "Locked";
      case 428: return "Precondition Required";
      case 431: return "Request Header Fields Too Large";
      case 451: return "Unavailable For Legal Reasons";
      case 505: return "HTTP Version Not Supported";
      default:
         // A status this table does not name is still the status the handler
         // chose: it goes out as that number with its class as the phrase. It
         // used to become a 500, which is how the files route's 410 and the
         // unsubscribe route's 502 reached their clients as server errors.
         if (status >= 500) return "Server Error";
         if (status >= 400) return "Client Error";
         if (status >= 300) return "Redirection";
         if (status >= 200) return "Success";
         return "Informational";
      }
   }

   AnsiString
   HttpServer::Serialize(const HttpResponse &response, bool keep_alive)
   {
      int status = response.status;
      AnsiString statusText = StatusText(status);

      AnsiString out;
      out.Format("HTTP/1.1 %d %hs\r\nContent-Type: %hs\r\nContent-Length: %d\r\n",
         status, statusText.c_str(), response.content_type.c_str(), (int) response.body.GetLength());

      out += response.extra_headers;
      out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
      out += response.body;

      return out;
   }

   AnsiString
   HttpServer::SerializeHead(const HttpResponse &response, bool keep_alive, unsigned long long content_length)
   {
      int status = response.status;
      AnsiString statusText = StatusText(status);

      AnsiString out;
      out.Format("HTTP/1.1 %d %hs\r\nContent-Type: %hs\r\n",
         status, statusText.c_str(), response.content_type.c_str());

      out += AnsiString("Content-Length: ") + AnsiString(std::to_string(content_length).c_str()) + "\r\n";
      out += response.extra_headers;
      out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";

      return out;
   }

   String
   HttpServer::FormatEndpoint(const String &bind_address, int port)
   {
      String result;

      if (bind_address.Find(_T(":")) >= 0)
         result.Format(_T("[%s]:%d"), bind_address.c_str(), port);
      else
         result.Format(_T("%s:%d"), bind_address.c_str(), port);

      return result;
   }

   namespace
   {
      // A server of the tester's own on 127.0.0.1, on a port the system
      // chooses, answering with the handler given; stopped when it goes.
      class HttpTesterServer
      {
      public:
         HttpTesterServer(const HttpLimits &limits, HttpServer::Handler handler)
         {
            server_.reset(new HttpServer("HttpServerTester", limits, handler, HttpServer::AcceptFilter(), HttpServer::ErrorResponder()));
            if (!server_->Listen(_T("127.0.0.1"), 0, std::shared_ptr<boost::asio::ssl::context>()))
               throw std::logic_error("HttpServerTester: a listener on 127.0.0.1 could not be opened.");

            server_->Start();
            port_ = server_->GetBoundPort();
            if (port_ <= 0)
               throw std::logic_error("HttpServerTester: the listener's port could not be read back.");
         }

         ~HttpTesterServer()
         {
            server_->Stop();
         }

         int Port() const { return port_; }

      private:
         std::shared_ptr<HttpServer> server_;
         int port_ = 0;
      };

      // A client of the tester's: what it sends, and every byte it reads back
      // with how the reading ended, under a deadline of its own so that a
      // server that never answers fails the case rather than hanging it.
      class HttpTesterClient
      {
      public:
         HttpTesterClient(int port, int receiveBuffer) :
            socket_(io_)
         {
            socket_.open(boost::asio::ip::tcp::v4());

            // Set before the connection, so the window the server is offered
            // is this small from the start.
            if (receiveBuffer > 0)
               socket_.set_option(boost::asio::socket_base::receive_buffer_size(receiveBuffer));

            socket_.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), (unsigned short) port));
         }

         void Send(const std::string &text)
         {
            boost::asio::write(socket_, boost::asio::buffer(text.data(), text.size()));
         }

         // Reads until `total` bytes have arrived in all, the peer ends the
         // connection, or `seconds` pass.
         void ReadUntil(size_t total, unsigned seconds)
         {
            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            char chunk[65536];

            while (bytes.size() < total && !ended && !timed_out)
            {
               const size_t want = std::min(sizeof(chunk), total - bytes.size());
               bool done = false;
               size_t got = 0;
               boost::system::error_code error;

               socket_.async_read_some(boost::asio::buffer(chunk, want),
                  [&done, &got, &error](const boost::system::error_code &e, size_t n)
                  {
                     error = e;
                     got = n;
                     done = true;
                  });

               io_.restart();
               io_.run_until(deadline);

               if (!done)
               {
                  timed_out = true;
                  boost::system::error_code ignored;
                  socket_.close(ignored);
                  io_.restart();
                  io_.run();
                  return;
               }

               bytes.append(chunk, got);

               if (error)
               {
                  end = error;
                  ended = true;
               }
            }
         }

         std::string bytes;
         boost::system::error_code end;
         bool ended = false;
         bool timed_out = false;

      private:
         boost::asio::io_context io_;
         boost::asio::ip::tcp::socket socket_;
      };

      // The head of the first answer in bytes, and the Content-Length it
      // declares; false while the head has not all arrived.
      bool HttpTesterHead(const std::string &bytes, std::string &head, size_t &contentLength)
      {
         const size_t end = bytes.find("\r\n\r\n");
         if (end == std::string::npos)
            return false;

         head = bytes.substr(0, end + 4);

         const size_t field = head.find("Content-Length: ");
         if (field == std::string::npos)
            throw std::logic_error("HttpServerTester: an answer came without a Content-Length.");

         contentLength = (size_t) strtoull(head.c_str() + field + 16, nullptr, 10);
         return true;
      }

      // A body whose every byte is known, so that one cut short or put
      // together wrongly cannot pass for the whole.
      std::string HttpTesterBody(size_t size)
      {
         std::string body(size, ' ');
         for (size_t i = 0; i < size; i++)
            body[i] = (i % 64 == 63) ? '\n' : (char) ('a' + (i * 7 + i / 64) % 26);
         return body;
      }

      HttpResponse HttpTesterAnswer(const std::string &body)
      {
         HttpResponse response;
         response.status = 200;
         response.content_type = "application/octet-stream";
         response.body = AnsiString(body);
         return response;
      }

      // The deadlines the cases run under: one second for a request, so that a
      // handler sleeping a little longer is past it.
      HttpLimits HttpTesterLimits()
      {
         HttpLimits limits;
         limits.request_seconds = 1;
         limits.connection_seconds = 60;
         limits.worker_threads = 2;
         limits.max_connections = 8;
         limits.compression = false;
         return limits;
      }

      const unsigned HttpTesterLateMilliseconds = 1600;

      std::string HttpTesterDescribe(const HttpTesterClient &client)
      {
         std::string text = std::to_string(client.bytes.size()) + " bytes read, then ";
         if (client.timed_out)
            return text + "nothing more for the client's own deadline";
         if (client.end == boost::asio::error::eof)
            return text + "an orderly end of the connection";
         return text + "the error \"" + client.end.message() + "\"";
      }
   }

   void
   HttpServerTester::Test()
   {
      TestLateAnswerArrivesWhole_();
      TestConnectionDeadlineDuringHandler_();
      TestStalledReaderIsReset_();
      TestKeepAliveAfterLateAnswer_();
   }

   // A handler that returns after request_seconds: its answer, larger than a
   // piece of the writer's, is written whole. Until 7 October 2026 the expiry
   // that came while the handler ran closed the connection behind the first
   // 64 KB.
   void
   HttpServerTester::TestLateAnswerArrivesWhole_()
   {
      const std::string body = HttpTesterBody(300000);

      HttpTesterServer server(HttpTesterLimits(), [&body](const HttpRequest &)
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(HttpTesterLateMilliseconds));
         return HttpTesterAnswer(body);
      });

      HttpTesterClient client(server.Port(), 0);
      client.Send("GET /late HTTP/1.1\r\nHost: tester\r\nConnection: close\r\n\r\n");
      client.ReadUntil(std::string::npos, 30);

      std::string head;
      size_t contentLength = 0;
      if (!HttpTesterHead(client.bytes, head, contentLength))
         throw std::logic_error("HttpServerTester: a handler that answered after the request deadline sent no complete head (" + HttpTesterDescribe(client) + ").");

      if (contentLength != body.size() || client.bytes.size() - head.size() != body.size() || client.bytes.compare(head.size(), std::string::npos, body) != 0)
         throw std::logic_error("HttpServerTester: an answer of " + std::to_string(body.size()) + " bytes that its handler returned after the request deadline did not arrive whole: " +
                                HttpTesterDescribe(client) + ", the head being " + std::to_string(head.size()) + ".");

      if (client.end != boost::asio::error::eof)
         throw std::logic_error("HttpServerTester: a whole answer the client asked to be closed after was not ended in order (" + HttpTesterDescribe(client) + ").");
   }

   // The connection's own deadline running out while the handler runs: the
   // answer is written whole, under a deadline moved out to fit it, and the
   // connection closed after it, as its head says.
   void
   HttpServerTester::TestConnectionDeadlineDuringHandler_()
   {
      const std::string body = HttpTesterBody(300000);

      HttpLimits limits = HttpTesterLimits();
      limits.request_seconds = 30;
      limits.connection_seconds = 1;

      HttpTesterServer server(limits, [&body](const HttpRequest &)
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(HttpTesterLateMilliseconds));
         return HttpTesterAnswer(body);
      });

      HttpTesterClient client(server.Port(), 0);
      client.Send("GET /late HTTP/1.1\r\nHost: tester\r\n\r\n");
      client.ReadUntil(std::string::npos, 30);

      std::string head;
      size_t contentLength = 0;
      if (!HttpTesterHead(client.bytes, head, contentLength) || contentLength != body.size() ||
          client.bytes.size() - head.size() != body.size() || client.bytes.compare(head.size(), std::string::npos, body) != 0)
         throw std::logic_error("HttpServerTester: an answer whose handler outlasted the connection's deadline did not arrive whole: " + HttpTesterDescribe(client) + ".");

      if (head.find("Connection: close\r\n") == std::string::npos)
         throw std::logic_error("HttpServerTester: a connection kept past its deadline for one answer did not say it would be closed after it.");

      if (client.end != boost::asio::error::eof)
         throw std::logic_error("HttpServerTester: a connection past its deadline was not ended in order after its last answer (" + HttpTesterDescribe(client) + ").");
   }

   // A reader that stops reading part way through an answer is closed after
   // request_seconds without progress - and reset, so that what it has is
   // never taken for the whole: an orderly end there reads as the end of the
   // answer to a client that does not count the bytes against Content-Length.
   void
   HttpServerTester::TestStalledReaderIsReset_()
   {
      // Larger than the socket buffers on both sides can hold, so the
      // server's write stalls for as long as the client does not read.
      const std::string body = HttpTesterBody(16 * 1024 * 1024);

      HttpLimits limits = HttpTesterLimits();
      limits.large_response_bytes = 0;

      HttpTesterServer server(limits, [&body](const HttpRequest &)
      {
         return HttpTesterAnswer(body);
      });

      HttpTesterClient client(server.Port(), 16 * 1024);
      client.Send("GET /large HTTP/1.1\r\nHost: tester\r\nConnection: close\r\n\r\n");
      client.ReadUntil(4096, 30);

      if (client.ended || client.timed_out)
         throw std::logic_error("HttpServerTester: the start of a large answer did not arrive (" + HttpTesterDescribe(client) + ").");

      // Not reading for longer than the request deadline.
      std::this_thread::sleep_for(std::chrono::milliseconds(2500));

      client.ReadUntil(std::string::npos, 60);

      std::string head;
      size_t contentLength = 0;
      if (!HttpTesterHead(client.bytes, head, contentLength))
         throw std::logic_error("HttpServerTester: a large answer sent no complete head (" + HttpTesterDescribe(client) + ").");

      if (client.bytes.size() - head.size() >= contentLength)
         throw std::logic_error("HttpServerTester: the socket buffers took the whole of a " + std::to_string(body.size()) +
                                "-byte answer, so a reader that stopped could not stall the writer and this case says nothing.");

      if (client.timed_out || client.end == boost::asio::error::eof)
         throw std::logic_error("HttpServerTester: a reader that stopped part way through an answer was not reset after the request deadline: " +
                                HttpTesterDescribe(client) + ", of " + std::to_string(contentLength) + " declared.");
   }

   // A late answer on a kept-alive connection, and then the next request on
   // the same connection: answered too. The expiry from the late one must
   // neither close the connection nor shorten the next request's deadline.
   void
   HttpServerTester::TestKeepAliveAfterLateAnswer_()
   {
      const std::string body = HttpTesterBody(100000);

      HttpTesterServer server(HttpTesterLimits(), [&body](const HttpRequest &request)
      {
         if (request.target == "/quick")
            return HttpTesterAnswer("quick");

         std::this_thread::sleep_for(std::chrono::milliseconds(HttpTesterLateMilliseconds));
         return HttpTesterAnswer(body);
      });

      HttpTesterClient client(server.Port(), 0);
      client.Send("GET /late HTTP/1.1\r\nHost: tester\r\n\r\n");

      std::string head;
      size_t contentLength = 0;
      client.ReadUntil(1, 30);
      while (!client.ended && !client.timed_out && !HttpTesterHead(client.bytes, head, contentLength))
         client.ReadUntil(client.bytes.size() + 1, 30);

      if (head.empty())
         throw std::logic_error("HttpServerTester: a late answer on a kept-alive connection sent no complete head (" + HttpTesterDescribe(client) + ").");

      client.ReadUntil(head.size() + contentLength, 30);

      if (contentLength != body.size() || client.bytes.size() != head.size() + body.size() || client.bytes.compare(head.size(), std::string::npos, body) != 0)
         throw std::logic_error("HttpServerTester: a late answer on a kept-alive connection did not arrive whole: " + HttpTesterDescribe(client) + ".");

      if (head.find("Connection: keep-alive\r\n") == std::string::npos)
         throw std::logic_error("HttpServerTester: a late answer on a young connection did not keep it alive.");

      const size_t first = client.bytes.size();

      // A moment, so that anything the late answer's deadline left queued has
      // run before the next request goes.
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      client.Send("GET /quick HTTP/1.1\r\nHost: tester\r\nConnection: close\r\n\r\n");
      client.ReadUntil(std::string::npos, 30);

      const std::string second = client.bytes.substr(first);
      if (second.compare(0, 15, "HTTP/1.1 200 OK") != 0 || second.size() < 5 || second.compare(second.size() - 5, 5, "quick") != 0)
         throw std::logic_error("HttpServerTester: the request after a late answer on the same connection was not answered: " + HttpTesterDescribe(client) + ".");
   }
}
