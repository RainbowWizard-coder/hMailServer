// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "HttpsClient.h"

#include "../TCPIP/CertificateVerifier.h"
#include "../TCPIP/SslContextInitializer.h"
#include "FileUtilities.h"
#include "../Application/IniFileSettings.h"
#include <fstream>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef std::chrono::steady_clock Clock;

      // The moment a step - or, for Request, the whole request - must be over by.
      Clock::time_point DeadlineIn_(int timeout_seconds)
      {
         return Clock::now() + std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 20);
      }

      // Waits, until deadline, for the one asynchronous operation just started on
      // socket (or on a TLS stream over it) to call its handler, which sets finished.
      //
      // Every step of a request runs this way rather than synchronously, because a
      // synchronous step has no deadline that holds everywhere. SO_RCVTIMEO bounds one
      // read and never the request, so an endpoint that sent a byte every few seconds
      // kept the thread for as long as it liked; POSIX takes it as a timeval and
      // refused the DWORD this client used to pass; and once an asynchronous connect
      // has put the socket into Asio's internal non-blocking mode, a synchronous read
      // on POSIX polls with no timeout at all. The thread waiting is one of the REST
      // API's four workers, or the one a COM client is blocked on, so "until the peer
      // lets go" is not an answer.
      //
      // On the deadline the socket is closed, which completes the pending operation
      // with an error, and the context is run until that handler has run, so nothing
      // is left referring to the caller's frame; then the step is reported as not done
      // in the time allowed.
      void AwaitWithin_(boost::asio::io_context &io, boost::asio::ip::tcp::socket &socket,
                        const Clock::time_point &deadline, const bool &finished, const char *step)
      {
         io.restart();
         if (Clock::now() < deadline)
            io.run_until(deadline);

         if (finished)
            return;

         boost::system::error_code ignored;
         socket.close(ignored);
         io.restart();
         io.run();
         throw std::runtime_error(std::string(step) + " in the time allowed");
      }

      template <typename EndpointSequence>
      void ConnectWithin_(boost::asio::io_context &io, boost::asio::ip::tcp::socket &socket,
                          const EndpointSequence &endpoints, const Clock::time_point &deadline)
      {
         bool finished = false;
         boost::system::error_code result;

         // Closing the socket is also what ends the composed operation: it stops
         // trying the next endpoint once the socket is not open.
         boost::asio::async_connect(socket, endpoints,
            [&finished, &result](const boost::system::error_code &ec, const boost::asio::ip::tcp::endpoint &)
            {
               result = ec;
               finished = true;
            });

         AwaitWithin_(io, socket, deadline, finished, "no connection was made");

         if (result)
            throw boost::system::system_error(result);
      }

      template <typename TlsStream>
      void HandshakeWithin_(boost::asio::io_context &io, TlsStream &stream, const Clock::time_point &deadline)
      {
         bool finished = false;
         boost::system::error_code result;

         stream.async_handshake(boost::asio::ssl::stream_base::client,
            [&finished, &result](const boost::system::error_code &ec)
            {
               result = ec;
               finished = true;
            });

         AwaitWithin_(io, stream.next_layer(), deadline, finished, "the TLS handshake did not finish");

         if (result)
            throw boost::system::system_error(result);
      }

      template <typename Stream>
      void WriteWithin_(boost::asio::io_context &io, boost::asio::ip::tcp::socket &socket, Stream &stream,
                        const char *data, size_t size, const Clock::time_point &deadline)
      {
         bool finished = false;
         boost::system::error_code result;

         boost::asio::async_write(stream, boost::asio::buffer(data, size),
            [&finished, &result](const boost::system::error_code &ec, std::size_t)
            {
               result = ec;
               finished = true;
            });

         AwaitWithin_(io, socket, deadline, finished, "the request was not sent");

         if (result)
            throw boost::system::system_error(result);
      }

      // One read of whatever has arrived. error is set as read_some sets it - at the
      // end of the stream among other things - and the bytes read are returned.
      template <typename Stream>
      size_t ReadSomeWithin_(boost::asio::io_context &io, boost::asio::ip::tcp::socket &socket, Stream &stream,
                             char *data, size_t size, const Clock::time_point &deadline, boost::system::error_code &error)
      {
         bool finished = false;
         size_t bytes = 0;

         stream.async_read_some(boost::asio::buffer(data, size),
            [&finished, &bytes, &error](const boost::system::error_code &ec, std::size_t transferred)
            {
               error = ec;
               bytes = transferred;
               finished = true;
            });

         AwaitWithin_(io, socket, deadline, finished, "the answer was not complete");
         return bytes;
      }

      bool PortNumber_(const std::string &port, unsigned short &number)
      {
         if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
            return false;

         const unsigned long value = strtoul(port.c_str(), nullptr, 10);
         if (value > 65535)
            return false;

         number = (unsigned short) value;
         return true;
      }

      // The addresses of host:port, within the deadline.
      //
      // A literal address is taken as written, without asking the resolver. A name is
      // resolved on a thread of its own, which this one waits for until the deadline
      // and then leaves behind: getaddrinfo cannot be cancelled, and Asio's own
      // asynchronous resolve runs it on a thread the io_context joins when it is
      // destroyed, which would wait out a hung lookup just as surely. The thread ends
      // when the operating system's resolver gives up, and what it writes to is shared
      // with it, so it may outlive this frame.
      std::vector<boost::asio::ip::tcp::endpoint> ResolveWithin_(const std::string &host, const std::string &port,
                                                                 const Clock::time_point &deadline)
      {
         boost::system::error_code notLiteral;
         const boost::asio::ip::address literal = boost::asio::ip::make_address(host, notLiteral);
         unsigned short portNumber = 0;
         if (!notLiteral && PortNumber_(port, portNumber))
            return std::vector<boost::asio::ip::tcp::endpoint>(1, boost::asio::ip::tcp::endpoint(literal, portNumber));

         struct Lookup
         {
            std::mutex lock;
            std::condition_variable done;
            bool finished = false;
            boost::system::error_code error;
            std::vector<boost::asio::ip::tcp::endpoint> endpoints;
         };

         const std::shared_ptr<Lookup> lookup = std::make_shared<Lookup>();

         std::thread([lookup, host, port]()
         {
            boost::system::error_code error;
            std::vector<boost::asio::ip::tcp::endpoint> endpoints;

            try
            {
               boost::asio::io_context io;
               boost::asio::ip::tcp::resolver resolver(io);
               for (const auto &entry : resolver.resolve(host, port, error))
                  endpoints.push_back(entry.endpoint());
            }
            catch (const boost::system::system_error &e)
            {
               error = e.code();
            }
            catch (...)
            {
               error = boost::asio::error::no_recovery;
            }

            std::lock_guard<std::mutex> guard(lookup->lock);
            lookup->error = error;
            lookup->endpoints.swap(endpoints);
            lookup->finished = true;
            lookup->done.notify_all();
         }).detach();

         std::unique_lock<std::mutex> guard(lookup->lock);
         if (!lookup->done.wait_until(guard, deadline, [&lookup]() { return lookup->finished; }))
            throw std::runtime_error("the host name was not resolved in the time allowed");

         if (lookup->error)
            throw boost::system::system_error(lookup->error);

         return lookup->endpoints;
      }

      // The whole answer, up to max_response_bytes. Past it, false - unless
      // stop_at_limit, when the read stops there, what was read is kept to be
      // parsed, and truncated says so.
      template <typename ReadSome>
      bool ReadWholeResponse_(ReadSome read_some, size_t max_response_bytes, std::string &raw, bool stop_at_limit, bool &truncated)
      {
         char buffer[4096];
         boost::system::error_code errorCode;
         truncated = false;

         for (;;)
         {
            const size_t bytesRead = read_some(buffer, sizeof(buffer), errorCode);
            if (bytesRead > 0)
               raw.append(buffer, bytesRead);

            // The end of the answer first, as it always was: a last read that
            // arrives with the end may run past the cap without failing.
            if (errorCode)
               break;

            if (raw.size() > max_response_bytes)
            {
               if (!stop_at_limit)
                  return false;

               truncated = true;
               break;
            }
         }

         if (stop_at_limit && raw.size() > max_response_bytes)
         {
            raw.resize(max_response_bytes);
            truncated = true;
         }

         return true;
      }

      bool ParseResponse_(const std::string &raw, HttpsClient::Response &response, String &error)
      {
         const size_t firstLineEnd = raw.find("\r\n");
         const size_t headerEnd = raw.find("\r\n\r\n");
         if (firstLineEnd == std::string::npos || headerEnd == std::string::npos)
         {
            error = _T("The response carried no HTTP headers.");
            return false;
         }

         const std::string statusLine = raw.substr(0, firstLineEnd);
         const size_t space = statusLine.find(' ');
         if (statusLine.compare(0, 5, "HTTP/") != 0 || space == std::string::npos)
         {
            error = _T("The response did not start with an HTTP status line.");
            return false;
         }

         response.status_code = atoi(statusLine.c_str() + space + 1);
         response.headers = raw.substr(0, headerEnd).c_str();
         // The body by its length rather than through c_str(): every caller
         // until the Web Key Directory lookup fetched JSON or a token, and a
         // conversion that stops at the first zero octet was invisible. A key
         // served by a directory is binary and has zero octets all through it,
         // and one taken that way would arrive a few bytes long.
         const std::string body = raw.substr(headerEnd + 4);
         response.body.assign(body.data(), body.size());

         return true;
      }

      // Why a request failed, in the words an administrator reads in the error log
      // and in a REST answer. A system_error's what() ends with the source location
      // Boost attaches to a code raised inside its own headers - the path of
      // win_iocp_socket_connect_op.hpp on the build machine, a line number and a
      // template signature longer than the sentence - so for one of those the
      // message and the category:value are taken from the code instead.
      String ReasonOf_(const std::exception &e)
      {
         const boost::system::system_error *systemError = dynamic_cast<const boost::system::system_error *>(&e);
         if (!systemError)
            return String(e.what());

         const boost::system::error_code code = systemError->code();
         const std::string reason = code.message() + " [" + code.category().name() + ":" + std::to_string(code.value()) + "]";
         return String(reason.c_str());
      }
   }

   namespace
   {
      // HttpProxy split into host and port; both empty when there is none. A value
      // without a port is an error the caller reports rather than a silent direct
      // connection: somebody who set a proxy expects the traffic to go through it.
      bool ProxySetting_(std::string &proxy_host, std::string &proxy_port, String &error)
      {
         proxy_host.clear();
         proxy_port.clear();

         AnsiString setting = AnsiString(IniFileSettings::Instance()->GetHttpProxy());
         std::string value = std::string(setting.c_str());

         size_t first = value.find_first_not_of(" \t");
         size_t last = value.find_last_not_of(" \t");
         value = (first == std::string::npos) ? std::string() : value.substr(first, last - first + 1);

         if (value.empty())
            return true;

         const size_t colon = value.rfind(':');
         if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size())
         {
            error = _T("HttpProxy must be host:port (or [ipv6]:port).");
            return false;
         }

         std::string host = value.substr(0, colon);
         if (host.size() > 2 && host.front() == '[' && host.back() == ']')
            host = host.substr(1, host.size() - 2);
         else if (host.find(':') != std::string::npos)
         {
            // An IPv6 literal without brackets: the last colon is inside the address.
            error = _T("HttpProxy must be host:port (or [ipv6]:port).");
            return false;
         }

         const std::string port = value.substr(colon + 1);
         if (port.find_first_not_of("0123456789") != std::string::npos)
         {
            error = _T("HttpProxy must be host:port (or [ipv6]:port).");
            return false;
         }

         proxy_host = host;
         proxy_port = port;
         return true;
      }

      // Whether a request to host goes through the proxy. An address on this machine
      // is reached directly unless the proxy is on this machine too: a proxy anywhere
      // else would connect to its own loopback address rather than this server's, and
      // a plain-http request - which this client sends only to a loopback address -
      // would cross the network to it in the clear on the way, a breach check mirror's
      // prefix among it.
      bool ThroughProxy_(const std::string &proxy_host, const AnsiString &host)
      {
         if (proxy_host.empty())
            return false;

         if (!HttpsClient::IsLoopbackHost(host))
            return true;

         return HttpsClient::IsLoopbackHost(AnsiString(proxy_host.c_str()));
      }

      // host[:port] as a Host header or a CONNECT target writes it: an IPv6 literal in
      // the brackets ParseUrl took off, and the port - always, or only when it is not
      // the scheme's default.
      std::string Authority_(const AnsiString &host, const AnsiString &port, bool always_port, bool https)
      {
         std::string authority = host.c_str();
         if (authority.find(':') != std::string::npos)
            authority = "[" + authority + "]";

         if (always_port || port != (https ? "443" : "80"))
            authority += ":" + std::string(port.c_str());

         return authority;
      }

      // An IPv4 address a request made on an outsider's behalf may reach: not
      // this host, not this network, not a range nobody routes.
      bool IsPublicV4(unsigned long v)
      {
         const unsigned a = (v >> 24) & 0xff, b = (v >> 16) & 0xff, c = (v >> 8) & 0xff;
         if (a == 0 || a == 10 || a == 127)                   // this network, RFC 1918, loopback
            return false;
         if (a == 100 && (b & 0xc0) == 64)                    // 100.64/10, carrier NAT
            return false;
         if (a == 169 && b == 254)                            // link-local, the metadata address among them
            return false;
         if (a == 172 && (b & 0xf0) == 16)                    // 172.16/12
            return false;
         if (a == 192 && b == 168)                            // 192.168/16
            return false;
         if (a == 192 && b == 0 && (c == 0 || c == 2))        // 192.0.0/24 protocol assignments, TEST-NET-1
            return false;
         if (a == 198 && (b == 18 || b == 19))                // 198.18/15 benchmarking
            return false;
         if ((a == 198 && b == 51 && c == 100) || (a == 203 && b == 0 && c == 113))   // TEST-NET-2, TEST-NET-3
            return false;
         if (a >= 224)                                        // multicast, reserved, broadcast
            return false;
         return true;
      }

      // The same for either family. An IPv6 address that carries an IPv4 one -
      // ::ffff:a.b.c.d, ::a.b.c.d, NAT64's 64:ff9b::/96, 6to4's 2002::/16 - is
      // judged as that IPv4 address, since that is where it leads.
      bool IsPublicAddress(const boost::asio::ip::address &address)
      {
         if (address.is_v4())
            return IsPublicV4(address.to_v4().to_uint());

         const boost::asio::ip::address_v6::bytes_type b = address.to_v6().to_bytes();
         const auto v4At = [&b](size_t at)
         {
            return ((unsigned long) b[at] << 24) | ((unsigned long) b[at + 1] << 16) | ((unsigned long) b[at + 2] << 8) | b[at + 3];
         };
         bool zeroTo10 = true;
         for (size_t i = 0; i < 10; i++)
            zeroTo10 = zeroTo10 && b[i] == 0;
         if (zeroTo10 && ((b[10] == 0xff && b[11] == 0xff) || (b[10] == 0 && b[11] == 0)))
            return IsPublicV4(v4At(12));                      // v4-mapped, v4-compatible, ::1 and :: among them
         bool nat64 = b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b;
         for (size_t i = 4; nat64 && i < 12; i++)
            nat64 = b[i] == 0;
         if (nat64)
            return IsPublicV4(v4At(12));
         if (b[0] == 0x20 && b[1] == 0x02)
            return IsPublicV4(v4At(2));
         if (b[0] == 0xff)                                    // multicast
            return false;
         if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)           // fe80::/10 link-local
            return false;
         if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0)           // fec0::/10 site-local
            return false;
         if ((b[0] & 0xfe) == 0xfc)                           // fc00::/7 unique local
            return false;
         if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)   // 2001:db8::/32 documentation
            return false;
         return true;
      }

      // An IPv4 address no endpoint an administrator names may be, even one on
      // this server's own network: link-local, where every cloud answers its
      // instance metadata, and the metadata addresses the clouds keep outside
      // that range. The credentials a metadata service hands out are the
      // machine's, and a request this server makes on somebody's say-so must not
      // be the way to them.
      bool IsLinkLocalOrMetadataV4(unsigned long v)
      {
         const unsigned a = (v >> 24) & 0xff, b = (v >> 16) & 0xff;
         if (a == 169 && b == 254)                            // 169.254/16; 169.254.169.254 among them
            return true;
         if (v == 0x646464C8UL)                               // 100.100.100.200, Alibaba Cloud's metadata
            return true;
         if (v == 0xA83F8110UL)                               // 168.63.129.16, Azure's host endpoint
            return true;
         if (v == 0xC00000C0UL)                               // 192.0.0.192, Oracle Cloud Classic's metadata
            return true;
         return false;
      }

      // The same for either family. An IPv6 address that carries an IPv4 one is
      // judged as that IPv4 address, as IsPublicAddress judges it.
      bool IsLinkLocalOrMetadataAddress(const boost::asio::ip::address &address)
      {
         if (address.is_v4())
            return IsLinkLocalOrMetadataV4(address.to_v4().to_uint());

         const boost::asio::ip::address_v6::bytes_type b = address.to_v6().to_bytes();
         const auto v4At = [&b](size_t at)
         {
            return ((unsigned long) b[at] << 24) | ((unsigned long) b[at + 1] << 16) | ((unsigned long) b[at + 2] << 8) | b[at + 3];
         };
         bool zeroTo10 = true;
         for (size_t i = 0; i < 10; i++)
            zeroTo10 = zeroTo10 && b[i] == 0;
         if (zeroTo10 && ((b[10] == 0xff && b[11] == 0xff) || (b[10] == 0 && b[11] == 0)))
            return IsLinkLocalOrMetadataV4(v4At(12));        // v4-mapped and v4-compatible
         bool nat64 = b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b;
         for (size_t i = 4; nat64 && i < 12; i++)
            nat64 = b[i] == 0;
         if (nat64)
            return IsLinkLocalOrMetadataV4(v4At(12));
         if (b[0] == 0x20 && b[1] == 0x02)
            return IsLinkLocalOrMetadataV4(v4At(2));
         if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)           // fe80::/10 link-local
            return true;

         // fd00:ec2::254, the Amazon instance metadata service over IPv6.
         static const unsigned char awsMetadata[16] =
            { 0xfd, 0x00, 0x0e, 0xc2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54 };
         bool metadata = true;
         for (size_t i = 0; i < 16; i++)
            metadata = metadata && b[i] == awsMetadata[i];
         return metadata;
      }

      // Connects socket to host:port by deadline - directly, or through the proxy
      // when proxy_host is not empty. For an https target the proxy is asked to
      // CONNECT and the TLS handshake then runs inside the tunnel, so the proxy sees
      // the name it was asked for and nothing of what follows. For plain http the
      // caller has put the absolute URL in its request line and the proxy forwards it.
      //
      // public_only (RequestPublic): the name is resolved here, every address it
      // gave is judged, and the connection is made to the addresses that passed
      // and to no other - so a name that answers a public address to one lookup
      // and a private one to the next gains nothing. Never through the proxy,
      // which would resolve the name itself, out of sight of this.
      //
      // not_link_local (RequestNotLinkLocal): the same, with the line drawn at
      // link-local and the metadata addresses instead of at every private one.
      bool Connect_(boost::asio::io_context &io, boost::asio::ip::tcp::socket &socket, const AnsiString &host,
                    const AnsiString &port, bool https, const std::string &proxy_host, const std::string &proxy_port,
                    const Clock::time_point &deadline, String &error, bool public_only = false, bool not_link_local = false)
      {
         if (public_only || not_link_local)
         {
            std::vector<boost::asio::ip::tcp::endpoint> vetted;
            std::string refused;
            for (const boost::asio::ip::tcp::endpoint &endpoint : ResolveWithin_(std::string(host.c_str()), std::string(port.c_str()), deadline))
            {
               const bool allowed = public_only
                  ? IsPublicAddress(endpoint.address())
                  : !IsLinkLocalOrMetadataAddress(endpoint.address());

               if (allowed)
                  vetted.push_back(endpoint);
               else if (refused.empty())
                  refused = endpoint.address().to_string();
            }
            if (vetted.empty())
            {
               if (public_only)
               {
                  error = Formatter::Format(_T("{0} is not a public address (loopback, private, link-local or reserved), and this request reaches only public ones."),
                     refused.empty() ? String(host) : String(refused.c_str()));
               }
               else
               {
                  error = Formatter::Format(_T("{0} is a link-local or cloud metadata address, which this request never reaches."),
                     refused.empty() ? String(host) : String(refused.c_str()));
               }
               return false;
            }
            ConnectWithin_(io, socket, vetted, deadline);
            return true;
         }

         if (proxy_host.empty())
         {
            ConnectWithin_(io, socket, ResolveWithin_(std::string(host.c_str()), std::string(port.c_str()), deadline), deadline);
            return true;
         }

         ConnectWithin_(io, socket, ResolveWithin_(proxy_host, proxy_port, deadline), deadline);

         if (!https)
            return true;

         const std::string target = Authority_(host, port, true, true);
         const std::string connectRequest = "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\nUser-Agent: hMailServer\r\n\r\n";
         WriteWithin_(io, socket, socket, connectRequest.data(), connectRequest.size(), deadline);

         // The proxy's answer ends at the blank line, and a proxy sends nothing more
         // until the client speaks, so read_until cannot swallow TLS bytes. A proxy
         // that talks for longer than 16 KB before that line is refused by the cap.
         boost::asio::streambuf answer(16384);
         bool finished = false;
         boost::system::error_code result;
         boost::asio::async_read_until(socket, answer, "\r\n\r\n",
            [&finished, &result](const boost::system::error_code &ec, std::size_t)
            {
               result = ec;
               finished = true;
            });
         AwaitWithin_(io, socket, deadline, finished, "the proxy did not answer");
         if (result)
            throw boost::system::system_error(result);

         std::istream lines(&answer);
         std::string statusLine;
         std::getline(lines, statusLine);
         if (!statusLine.empty() && statusLine.back() == '\r')
            statusLine.pop_back();

         const size_t space = statusLine.find(' ');
         const int status = (statusLine.compare(0, 5, "HTTP/") == 0 && space != std::string::npos) ? atoi(statusLine.c_str() + space + 1) : 0;

         if (status < 200 || status > 299)
         {
            error = Formatter::Format(_T("The proxy {0} refused CONNECT to {1}: {2}"),
               String((proxy_host + ":" + proxy_port).c_str()), String(target.c_str()), String(statusLine.c_str()));
            return false;
         }

         return true;
      }

      // Whether the host of a URL is an address rather than a name - ParseUrl has
      // taken an IPv6 address out of its brackets - which decides whether a TLS
      // server name is sent (RFC 6066 section 3: never an address).
      bool IsAddressLiteral_(const AnsiString &host)
      {
         boost::system::error_code notAddress;
         boost::asio::ip::make_address(std::string(host.c_str()), notAddress);
         return !notAddress;
      }

      // The request-target for the request line: the path, or through a proxy for
      // plain http the absolute URL, which is how a forward proxy is told where to go.
      AnsiString RequestTarget_(const AnsiString &host, const AnsiString &port, const AnsiString &path, bool https, bool via_proxy)
      {
         if (https || !via_proxy)
            return path;

         const std::string absolute = "http://" + Authority_(host, port, true, false) + std::string(path.c_str());
         return AnsiString(absolute.c_str());
      }
   }

   bool
   HttpsClient::ParseUrl(const AnsiString &url, bool &https, AnsiString &host, AnsiString &port, AnsiString &path)
   {
      std::string s = url.c_str();

      if (s.compare(0, 8, "https://") == 0)
      {
         https = true;
         s = s.substr(8);
      }
      else if (s.compare(0, 7, "http://") == 0)
      {
         https = false;
         s = s.substr(7);
      }
      else
      {
         return false;
      }

      const size_t slash = s.find('/');
      std::string authority = slash == std::string::npos ? s : s.substr(0, slash);
      std::string p = slash == std::string::npos ? "/" : s.substr(slash);

      if (authority.empty() || authority.find('@') != std::string::npos)
         return false;

      std::string h = authority;
      std::string prt = https ? "443" : "80";

      if (!h.empty() && h[0] == '[')
      {
         // [IPv6]:port
         const size_t close = h.find(']');
         if (close == std::string::npos)
            return false;
         if (close + 1 < h.size())
         {
            if (h[close + 1] != ':')
               return false;
            prt = h.substr(close + 2);
         }
         h = h.substr(1, close - 1);
      }
      else
      {
         const size_t colon = h.find(':');
         if (colon != std::string::npos)
         {
            prt = h.substr(colon + 1);
            h = h.substr(0, colon);
         }
      }

      if (h.empty() || prt.empty() || prt.find_first_not_of("0123456789") != std::string::npos)
         return false;

      host = h.c_str();
      port = prt.c_str();
      path = p.c_str();

      return true;
   }

   bool
   HttpsClient::IsLoopbackHost(const AnsiString &host)
   {
      AnsiString h = host;
      h.MakeLower();

      if (h == "localhost")
         return true;

      // An address, parsed - never a prefix of the text. "127." used to be enough,
      // which let http://127.attacker.example/ through as "loopback" for the JWKS
      // document, the introspection verdict and the update feed alike; that name is
      // whatever its owner's DNS says it is. 127.0.0.0/8, ::1, and 127/8 carried in
      // an IPv4-mapped IPv6 address.
      boost::system::error_code ec;
      const boost::asio::ip::address address = boost::asio::ip::make_address(std::string(h.c_str()), ec);
      if (ec)
         return false;

      if (address.is_v6() && address.to_v6().is_v4_mapped())
         return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).is_loopback();

      return address.is_loopback();
   }

   AnsiString
   HttpsClient::FormEncode(const AnsiString &value)
   {
      static const char *hex = "0123456789ABCDEF";
      AnsiString result;

      for (int i = 0; i < value.GetLength(); i++)
      {
         const unsigned char c = (unsigned char) value[i];
         if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
         {
            result += (char) c;
         }
         else
         {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0F];
         }
      }

      return result;
   }

   bool
   HttpsClient::Request(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                        const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                        int timeout_seconds, size_t max_response_bytes)
   {
      return Request_(method, url, extra_headers, content_type, body, response, error, timeout_seconds, max_response_bytes, false, false);
   }

   bool
   HttpsClient::RequestNotLinkLocal(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                                    const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                                    int timeout_seconds, size_t max_response_bytes, bool keep_head_of_long_answer)
   {
      return Request_(method, url, extra_headers, content_type, body, response, error, timeout_seconds, max_response_bytes, false, true,
                      keep_head_of_long_answer);
   }

   bool
   HttpsClient::HostIsNotLinkLocal(const AnsiString &host, bool &resolved, String &error)
   {
      resolved = false;

      try
      {
         const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
         const std::vector<boost::asio::ip::tcp::endpoint> endpoints = ResolveWithin_(std::string(host.c_str()), "443", deadline);

         resolved = !endpoints.empty();

         for (const boost::asio::ip::tcp::endpoint &endpoint : endpoints)
         {
            if (IsLinkLocalOrMetadataAddress(endpoint.address()))
            {
               error = Formatter::Format(_T("{0} is at {1}, a link-local or cloud metadata address, which this server never posts to."),
                  String(host), String(endpoint.address().to_string().c_str()));
               return false;
            }
         }

         return true;
      }
      catch (...)
      {
         // Not resolved now, which is not a refusal: the connection judges the
         // addresses again when it is made.
         resolved = false;
         return true;
      }
   }

   bool
   HttpsClient::HostIsPublic(const AnsiString &host, String &error)
   {
      try
      {
         const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
         const std::vector<boost::asio::ip::tcp::endpoint> endpoints = ResolveWithin_(std::string(host.c_str()), "143", deadline);
         if (endpoints.empty())
         {
            error = Formatter::Format(_T("{0} does not resolve."), String(host));
            return false;
         }
         for (const boost::asio::ip::tcp::endpoint &endpoint : endpoints)
         {
            if (!IsPublicAddress(endpoint.address()))
            {
               error = Formatter::Format(_T("{0} is at {1}, which is not a public address (loopback, private, link-local or reserved)."),
                  String(host), String(endpoint.address().to_string().c_str()));
               return false;
            }
         }
         return true;
      }
      catch (...)
      {
         error = Formatter::Format(_T("{0} does not resolve."), String(host));
         return false;
      }
   }

   bool
   HttpsClient::RequestPublic(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                              const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                              int timeout_seconds, size_t max_response_bytes, bool keep_head_of_long_answer)
   {
      return Request_(method, url, extra_headers, content_type, body, response, error, timeout_seconds, max_response_bytes, true, false,
                      keep_head_of_long_answer);
   }

   bool
   HttpsClient::Request_(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                         const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                         int timeout_seconds, size_t max_response_bytes, bool public_only, bool not_link_local,
                         bool keep_head_of_long_answer)
   {
      bool https = false;
      AnsiString host, port, path;
      if (!ParseUrl(url, https, host, port, path))
      {
         error = _T("The URL is not an http or https URL with a host.");
         return false;
      }

      if (!https && !IsLoopbackHost(host))
      {
         error = _T("Plain http is only accepted to a loopback address; use https.");
         return false;
      }

      std::string proxyHost, proxyPort;
      if (!ProxySetting_(proxyHost, proxyPort, error))
         return false;
      if (public_only || not_link_local || !ThroughProxy_(proxyHost, host))
      {
         proxyHost.clear();
         proxyPort.clear();
      }
      const bool viaProxy = !proxyHost.empty();

      AnsiString request;
      request.append(method);
      request.append(" ");
      request.append(RequestTarget_(host, port, path, https, viaProxy));
      request.append(" HTTP/1.0\r\nHost: ");
      request.append(Authority_(host, port, false, https).c_str());
      request.append("\r\nUser-Agent: hMailServer\r\n");

      // JSON unless the caller asks for something else: a CardDAV or CalDAV
      // request (Application/MigrationManager.h) wants XML back, and names it.
      bool acceptGiven = false;
      for (const AnsiString &header : extra_headers)
         acceptGiven = acceptGiven || header.StartsWith("Accept:");
      if (!acceptGiven)
         request.append("Accept: application/json\r\n");

      for (const AnsiString &header : extra_headers)
      {
         request.append(header);
         request.append("\r\n");
      }

      // A body is framed for any method that carries one - PROPFIND and REPORT
      // do - and for a POST whatever it carries, as it always was.
      if (method == "POST" || !body.IsEmpty())
      {
         AnsiString length;
         length.Format("%d", body.GetLength());
         request.append("Content-Type: ");
         request.append(content_type.IsEmpty() ? AnsiString("application/x-www-form-urlencoded") : content_type);
         request.append("\r\nContent-Length: ");
         request.append(length);
         request.append("\r\n");
      }

      request.append("Connection: close\r\n\r\n");
      request.append(body);

      try
      {
         // One deadline for all of it, taken before the name is looked up: the lookup,
         // the connection, the proxy's CONNECT, the handshake, the request and every
         // read of the answer spend the same timeout_seconds between them.
         const Clock::time_point deadline = DeadlineIn_(timeout_seconds);

         boost::asio::io_context ioContext;

         std::string raw;
         bool truncated = false;

         if (https)
         {
            boost::asio::ssl::context sslContext(boost::asio::ssl::context::tls_client);
            // An HTTPS client of a web service: TLS 1.2 is the floor whatever the mail
            // protocol toggles allow, since there is no 2008-era CA, token issuer or
            // policy host to accommodate.
            sslContext.set_options(HM_TLS_CONTEXT_FLOOR);
            sslContext.set_default_verify_paths();
            SslContextInitializer::InitClient(sslContext, false);

            boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(ioContext, sslContext);
            if (!Connect_(ioContext, stream.next_layer(), host, port, true, proxyHost, proxyPort, deadline, error, public_only, not_link_local))
               return false;

            stream.set_verify_mode(boost::asio::ssl::verify_peer);
            stream.set_verify_callback(CertificateVerifier(0, CSSSL, String(host)));

            // No server name for an endpoint given by its address: RFC 6066 section 3
            // allows only a host name there, and a server that holds to it refuses
            // the handshake. The certificate is still checked against the address
            // (CertificateVerifier::MatchesHost). MigrationImapClient does the same.
            if (!IsAddressLiteral_(host) && !SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
            {
               error = _T("Could not set the TLS server name.");
               return false;
            }

            HandshakeWithin_(ioContext, stream, deadline);
            WriteWithin_(ioContext, stream.next_layer(), stream, request.c_str(), (size_t) request.GetLength(), deadline);

            const auto readSome = [&ioContext, &stream, &deadline](char *data, size_t size, boost::system::error_code &ec)
            {
               return ReadSomeWithin_(ioContext, stream.next_layer(), stream, data, size, deadline, ec);
            };

            if (!ReadWholeResponse_(readSome, max_response_bytes, raw, keep_head_of_long_answer, truncated))
            {
               error = _T("The response exceeded the size limit.");
               return false;
            }
         }
         else
         {
            boost::asio::ip::tcp::socket socket(ioContext);
            if (!Connect_(ioContext, socket, host, port, false, proxyHost, proxyPort, deadline, error, public_only, not_link_local))
               return false;

            WriteWithin_(ioContext, socket, socket, request.c_str(), (size_t) request.GetLength(), deadline);

            const auto readSome = [&ioContext, &socket, &deadline](char *data, size_t size, boost::system::error_code &ec)
            {
               return ReadSomeWithin_(ioContext, socket, socket, data, size, deadline, ec);
            };

            if (!ReadWholeResponse_(readSome, max_response_bytes, raw, keep_head_of_long_answer, truncated))
            {
               error = _T("The response exceeded the size limit.");
               return false;
            }
         }

         if (!ParseResponse_(raw, response, error))
            return false;

         response.truncated = truncated;
         return true;
      }
      catch (const std::exception &e)
      {
         error = Formatter::Format(_T("The request to {0} failed: {1}"), String(host), ReasonOf_(e));
         return false;
      }
   }

   namespace
   {
      // Reads one response through read_some: the status line and headers into
      // header_block, then the body to sink, up to max_bytes. too_large is set when the
      // body went past max_bytes; the read stops there.
      template <typename ReadSome>
      bool ReadResponseToSink_(ReadSome read_some, std::string &header_block, std::ofstream *sink, size_t max_bytes, size_t &body_bytes, bool &too_large)
      {
         char buffer[16384];
         std::string pending;
         bool headersDone = false;
         boost::system::error_code errorCode;
         body_bytes = 0;
         too_large = false;

         for (;;)
         {
            const size_t bytesRead = read_some(buffer, sizeof(buffer), errorCode);
            if (bytesRead > 0)
            {
               if (!headersDone)
               {
                  pending.append(buffer, bytesRead);
                  const size_t headerEnd = pending.find("\r\n\r\n");
                  if (headerEnd != std::string::npos)
                  {
                     header_block = pending.substr(0, headerEnd);
                     headersDone = true;
                     const std::string first = pending.substr(headerEnd + 4);
                     body_bytes += first.size();
                     if (body_bytes > max_bytes)
                     {
                        too_large = true;
                        return true;
                     }
                     if (sink && !first.empty())
                        sink->write(first.data(), (std::streamsize) first.size());
                     pending.clear();
                  }
                  else if (pending.size() > 64 * 1024)
                     return false;   // headers that never end are not a response
               }
               else
               {
                  body_bytes += bytesRead;
                  if (body_bytes > max_bytes)
                  {
                     too_large = true;
                     return true;
                  }
                  if (sink)
                     sink->write(buffer, (std::streamsize) bytesRead);
               }
            }
            if (errorCode)
               break;
         }

         return headersDone;
      }

      // The value of one header in a header block, case-insensitively; empty when absent.
      std::string HeaderValue_(const std::string &header_block, const std::string &name)
      {
         size_t position = 0;
         while (position < header_block.size())
         {
            size_t end = header_block.find("\r\n", position);
            if (end == std::string::npos)
               end = header_block.size();
            std::string line = header_block.substr(position, end - position);
            position = end + 2;

            size_t colon = line.find(':');
            if (colon == std::string::npos || colon != name.size())
               continue;
            if (_strnicmp(line.c_str(), name.c_str(), name.size()) != 0)
               continue;
            std::string value = line.substr(colon + 1);
            size_t start = value.find_first_not_of(" \t");
            size_t stop = value.find_last_not_of(" \t\r");
            return start == std::string::npos ? std::string() : value.substr(start, stop - start + 1);
         }
         return std::string();
      }
   }

   bool
   HttpsClient::Download(const AnsiString &url, const String &path, size_t max_bytes, int &status_code, String &error, int timeout_seconds)
   {
      status_code = 0;
      AnsiString current = url;

      for (int hop = 0; hop < 6; hop++)
      {
         bool https = false;
         AnsiString host, port, requestPath;
         if (!ParseUrl(current, https, host, port, requestPath))
         {
            error = Formatter::Format(_T("{0} is not an http or https URL with a host."), String(current));
            return false;
         }
         if (!https && !IsLoopbackHost(host))
         {
            error = Formatter::Format(_T("{0}: plain http is only accepted to a loopback address; use https."), String(current));
            return false;
         }

         std::string proxyHost, proxyPort;
         if (!ProxySetting_(proxyHost, proxyPort, error))
            return false;
         if (!ThroughProxy_(proxyHost, host))
         {
            proxyHost.clear();
            proxyPort.clear();
         }

         AnsiString request;
         request.append("GET ");
         request.append(RequestTarget_(host, port, requestPath, https, !proxyHost.empty()));
         request.append(" HTTP/1.0\r\nHost: ");
         request.append(Authority_(host, port, false, https).c_str());
         request.append("\r\nUser-Agent: hMailServer\r\nAccept: */*\r\nConnection: close\r\n\r\n");

         std::string headerBlock;
         size_t bodyBytes = 0;
         bool tooLarge = false;
         bool responded = false;

         // The file is opened once the status is known to be 200, so a redirect or
         // an error leaves nothing behind; a body that turns out too large is
         // removed below.
         std::ofstream sink;
         try
         {
            // A download is bounded step by step rather than as a whole: an installer
            // is tens of megabytes, and a slow link is not a hung one. What it must not
            // do is wait on any one step - the lookup and connection, the handshake,
            // sending the request, one read - for longer than timeout_seconds.
            boost::asio::io_context ioContext;

            // The headers are read first with no sink; the body's first bytes are
            // held by the reader until the sink exists. To keep the reader simple the
            // status is decided from the header block it hands back, and the sink
            // is opened before the body is streamed - which means the reader must
            // be told the sink up front. So: open the file now, and delete it unless
            // the response was a 200 written in full.
#ifdef HM_PLATFORM_POSIX
            // A wide path is an extension MSVC's ofstream carries and libstdc++ does
            // not, so the path is narrowed here the way the rest of the tree narrows
            // a String for an API that takes char - the string class does the
            // conversion on construction.
            const AnsiString narrowPath = path.c_str();
            sink.open(narrowPath.c_str(), std::ios::binary | std::ios::trunc);
#else
            sink.open(path.c_str(), std::ios::binary | std::ios::trunc);
#endif
            if (!sink)
            {
               error = Formatter::Format(_T("{0} could not be created."), path);
               return false;
            }

            if (https)
            {
               boost::asio::ssl::context sslContext(boost::asio::ssl::context::tls_client);
               sslContext.set_options(HM_TLS_CONTEXT_FLOOR);
               sslContext.set_default_verify_paths();
               SslContextInitializer::InitClient(sslContext, false);

               boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(ioContext, sslContext);
               if (!Connect_(ioContext, stream.next_layer(), host, port, true, proxyHost, proxyPort, DeadlineIn_(timeout_seconds), error))
               {
                  sink.close();
                  FileUtilities::DeleteFile(path);
                  return false;
               }
               stream.set_verify_mode(boost::asio::ssl::verify_peer);
               stream.set_verify_callback(CertificateVerifier(0, CSSSL, String(host)));
               // No server name for an address, as in Request_.
               if (!IsAddressLiteral_(host) && !SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
               {
                  error = _T("Could not set the TLS server name.");
                  sink.close();
                  FileUtilities::DeleteFile(path);
                  return false;
               }
               HandshakeWithin_(ioContext, stream, DeadlineIn_(timeout_seconds));
               WriteWithin_(ioContext, stream.next_layer(), stream, request.c_str(), (size_t) request.GetLength(), DeadlineIn_(timeout_seconds));

               const auto readSome = [&ioContext, &stream, timeout_seconds](char *data, size_t size, boost::system::error_code &ec)
               {
                  return ReadSomeWithin_(ioContext, stream.next_layer(), stream, data, size, DeadlineIn_(timeout_seconds), ec);
               };
               responded = ReadResponseToSink_(readSome, headerBlock, &sink, max_bytes, bodyBytes, tooLarge);
            }
            else
            {
               boost::asio::ip::tcp::socket socket(ioContext);
               if (!Connect_(ioContext, socket, host, port, false, proxyHost, proxyPort, DeadlineIn_(timeout_seconds), error))
               {
                  sink.close();
                  FileUtilities::DeleteFile(path);
                  return false;
               }
               WriteWithin_(ioContext, socket, socket, request.c_str(), (size_t) request.GetLength(), DeadlineIn_(timeout_seconds));

               const auto readSome = [&ioContext, &socket, timeout_seconds](char *data, size_t size, boost::system::error_code &ec)
               {
                  return ReadSomeWithin_(ioContext, socket, socket, data, size, DeadlineIn_(timeout_seconds), ec);
               };
               responded = ReadResponseToSink_(readSome, headerBlock, &sink, max_bytes, bodyBytes, tooLarge);
            }
         }
         catch (const std::exception &e)
         {
            sink.close();
            FileUtilities::DeleteFile(path);
            error = Formatter::Format(_T("The request to {0} failed: {1}"), String(host), ReasonOf_(e));
            return false;
         }

         sink.close();

         if (!responded)
         {
            FileUtilities::DeleteFile(path);
            error = Formatter::Format(_T("{0} sent no HTTP response."), String(host));
            return false;
         }

         const size_t firstLineEnd = headerBlock.find("\r\n");
         const std::string statusLine = headerBlock.substr(0, firstLineEnd);
         const size_t space = statusLine.find(' ');
         if (statusLine.compare(0, 5, "HTTP/") != 0 || space == std::string::npos)
         {
            FileUtilities::DeleteFile(path);
            error = _T("The response did not start with an HTTP status line.");
            return false;
         }
         status_code = atoi(statusLine.c_str() + space + 1);

         if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308)
         {
            FileUtilities::DeleteFile(path);
            std::string location = HeaderValue_(headerBlock, "Location");
            if (location.empty())
            {
               error = Formatter::Format(_T("{0} redirected without saying where."), String(host));
               return false;
            }
            if (location[0] == '/')
            {
               // Relative to the host: same scheme, host and port.
               std::string origin = https ? "https://" : "http://";
               origin += Authority_(host, port, false, https);
               location = origin + location;
            }
            current = location.c_str();
            continue;
         }

         if (status_code != 200)
         {
            FileUtilities::DeleteFile(path);
            error = Formatter::Format(_T("{0} answered HTTP {1}."), String(host), status_code);
            return false;
         }

         if (tooLarge)
         {
            FileUtilities::DeleteFile(path);
            error = Formatter::Format(_T("The response from {0} exceeded the size limit."), String(host));
            return false;
         }

         return true;
      }

      FileUtilities::DeleteFile(path);
      error = Formatter::Format(_T("{0} redirected too many times."), String(url));
      return false;
   }
}
