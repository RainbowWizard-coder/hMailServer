// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The deadlines of the server's own network clients and raw-socket listeners, held
// on Linux against peers that never answer - a program of its own for the POSIX
// build, the counterpart of spf-tests and backup-tests, which linux-build.sh runs
// with every other *-tests program.
//
// Until 6.3.6 each of these set its timeout on the socket in the Windows form, a
// DWORD of milliseconds, which Linux refuses (it takes a struct timeval), and the
// three HTTPS clients read with Asio's synchronous calls, which on POSIX wait with
// no timeout at all: on Linux a peer that took the connection and never answered
// held the calling thread for as long as it stayed connected, and a client that
// connected to one of the single-threaded listeners and said nothing held the
// listener. Windows honoured the DWORD, so nothing on Windows showed it.
//
// What it holds, all at once, each against a peer of its own on 127.0.0.1 that
// accepts and never says a word:
//   - the OAuth2 token request (OutboundOAuth2TokenClient::GetToken), 20 seconds;
//   - an ACME request (AcmeClient::RequestCertificate, whose first step fetches
//     the directory), 30 seconds;
//   - the DANE TLSA query over UDP (TlsPolicy::RunDnsQuery_), 5 seconds;
//   - the ACME http-01 responder (AcmeChallengeServer), which serves one client at
//     a time: a client that connects and sends nothing is let go after 5 seconds,
//     and the CA's request behind it answered - and the responder then starts
//     again on the same port at once, which SO_REUSEADDR is for on POSIX;
// and, without the network, the ACME header lookup (AcmeClient::GetHeaderValue),
// which took the header that comes last for no header at all.
//
// The MTA-STS policy fetch (TlsPolicy::HttpsGet_), whose host the recipient's
// domain names, is held to public addresses and to a host that is a name, so a
// silent peer on 127.0.0.1 is what it must refuse rather than wait on: at the same
// time as the others, a host that carries a port (127.0.0.1:<a peer's port>) is
// refused and that peer never reached, and 127.0.0.1 is refused as written and as
// 2130706433, the same address as one number, which the system's resolver reads
// without asking DNS - the refusal naming 2130706433 and not the 127.0.0.1 it led
// to, since 6.3.6. Then, once those cases have ended, with HttpProxy set to a
// proxy of its own on 127.0.0.1 that takes a request and never answers it: 127.0.0.1
// is still refused and the proxy never asked, and 3227017985 - 192.88.99.1, an
// address the client counts public, written as one number - goes to the proxy as
// CONNECT 192.88.99.1:443, the address that was judged and not the text it was
// given, and the fetch gives up at its 15 seconds. Then, through the same proxy,
// a Web Key Directory's fetch (OpenPgpRing::FetchFromDirectory), whose host the
// address a person typed names, and which until 6.3.6 went direct whatever
// HttpProxy said: 2130706433 is refused, without the address it led to, and the
// proxy not asked, and 3227017985
// is asked of it as CONNECT 192.88.99.1:443, given up at five seconds. Nothing is
// sent to 192.88.99.1: the proxy is the only peer, and it says nothing.
//
// Each one must end in its own time and not much before it - a quick failure for
// another reason is not a deadline - and each peer must have been reached, so a
// client that never connected cannot pass. A case that has not ended a while after
// its deadline is reported as stuck, the others are waited for, and the program
// then exits without joining it: a thread that is stuck in a read cannot be
// joined, which is the defect this program exists to catch.
//
// First, an error reported where the settings hold no UseScriptServer, as this
// program's do not, must come back: until 6.3.6 the report read the switch, the
// read reported it missing, and that went round until the stack ran out - which
// this program met on Debian 12 and Ubuntu 24.04, whose OpenSSL 3.0 makes the
// first TLS context report the key-exchange groups it lacks.
//
// It writes a configuration file of its own in a directory of its own under the
// system's temp directory, which it removes again. No database, no name looked up in
// DNS (the two numbers above are read by the resolver as written), no connection but
// to 127.0.0.1 - the http-01 responder listens on every address, as the server's
// does, for the few seconds its case takes - and no root. It needs the loopback, so
// it cannot run where sockets are refused altogether: the source packages' builds
// run under build/ci/packages/offline.c, and their tester lists (debian/rules,
// hmailserver.spec) leave it out. About three quarters of a minute: the ACME case's
// deadline, then the two proxied fetches'.
//
// It prints one line per case and exits non-zero when any fails.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>

#include "../Common/Application/Configuration.h"
#include "../Common/Application/Constants.h"
#include "../Common/Application/ErrorManager.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/Property.h"
#include "../Common/Application/PropertySet.h"
#include "../Common/Util/AcmeClient.h"
#include "../Common/Util/OpenPgpRing.h"
#include "../Common/Util/OutboundOAuth2TokenClient.h"
#include "../SMTP/TlsPolicy.h"

namespace HM
{
   // The TLS settings the server's HTTPS clients read, for a program with no
   // database. HttpsClient builds each TLS context through
   // SslContextInitializer::InitClient, which reads the protocol versions, the TLS
   // options and the cipher list from the configuration's settings - rows of
   // hm_settings, loaded by Configuration::Load, which this program has none of.
   // So a set is filled in memory, with no row behind any property, and published
   // in its place: SslVersions 24 (TLS 1.2 and 1.3) and TlsOptions 0, as a new
   // database holds them, and an empty cipher list, which leaves OpenSSL's own - the
   // ClientHello these peers never answer does not depend on it. A friend of
   // Configuration and PropertySet, for this alone.
   class NetworkDeadlineTests
   {
   public:
      static void UseTlsSettingsWithoutDatabase()
      {
         std::shared_ptr<PropertySet> settings(new PropertySet());
         const auto hold = [&settings](const String &name, long value, const String &text)
         {
            settings->items_[name] = std::shared_ptr<Property>(new Property(name, value, text));
         };

         hold(PROPERTY_SSLVERSIONS, TlsVersion12 | TlsVersion13, _T(""));
         hold(PROPERTY_TLSOPTIONS, 0, _T(""));
         hold(PROPERTY_SSLCIPHERLIST, 0, _T(""));

         Configuration *configuration = Configuration::Instance();
         configuration->Publish_(configuration->property_set_, settings);
      }
   };
}

namespace
{
   typedef std::chrono::steady_clock Clock;

   double SecondsSince(const Clock::time_point &start)
   {
      return std::chrono::duration<double>(Clock::now() - start).count();
   }

   // "12.3 s"
   std::string Seconds(double seconds)
   {
      std::ostringstream text;
      text << std::fixed << std::setprecision(1) << seconds << " s";
      return text.str();
   }

   // A TCP listener on 127.0.0.1 that accepts every connection and never reads
   // or writes a byte on any of them, and holds them open until it is destroyed.
   class SilentTcpPeer
   {
   public:
      SilentTcpPeer() : listen_socket_(-1), port_(0), accepted_(0), stop_(false)
      {
         listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
         if (listen_socket_ < 0)
            return;

         sockaddr_in address = {};
         address.sin_family = AF_INET;
         address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

         socklen_t length = sizeof(address);
         if (bind(listen_socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
             listen(listen_socket_, 16) != 0 ||
             getsockname(listen_socket_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
         {
            close(listen_socket_);
            listen_socket_ = -1;
            return;
         }

         port_ = ntohs(address.sin_port);
         worker_ = std::thread(&SilentTcpPeer::Run_, this);
      }

      ~SilentTcpPeer()
      {
         stop_ = true;
         if (worker_.joinable())
            worker_.join();

         for (int held : held_)
            close(held);

         if (listen_socket_ >= 0)
            close(listen_socket_);
      }

      SilentTcpPeer(const SilentTcpPeer &) = delete;
      SilentTcpPeer &operator=(const SilentTcpPeer &) = delete;

      int Port() const { return port_; }
      int Accepted() const { return accepted_; }

   private:
      void Run_()
      {
         while (!stop_)
         {
            pollfd waiting = {};
            waiting.fd = listen_socket_;
            waiting.events = POLLIN;

            if (poll(&waiting, 1, 100) <= 0)
               continue;

            const int connection = accept(listen_socket_, nullptr, nullptr);
            if (connection < 0)
               continue;

            // Held, never read: the client's request, and its TLS ClientHello, stay
            // unanswered in the socket's buffer.
            held_.push_back(connection);
            accepted_++;
         }
      }

      int listen_socket_;
      int port_;
      std::atomic<int> accepted_;
      std::atomic<bool> stop_;
      std::vector<int> held_;
      std::thread worker_;
   };

   // A forward proxy on 127.0.0.1 that reads what each client asks for - the
   // request line, a CONNECT's target among it - and then answers nothing, and
   // holds the connection open until it is destroyed.
   class SilentProxy
   {
   public:
      SilentProxy() : listen_socket_(-1), port_(0), accepted_(0), stop_(false)
      {
         listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
         if (listen_socket_ < 0)
            return;

         sockaddr_in address = {};
         address.sin_family = AF_INET;
         address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

         socklen_t length = sizeof(address);
         if (bind(listen_socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
             listen(listen_socket_, 16) != 0 ||
             getsockname(listen_socket_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
         {
            close(listen_socket_);
            listen_socket_ = -1;
            return;
         }

         port_ = ntohs(address.sin_port);
         worker_ = std::thread(&SilentProxy::Run_, this);
      }

      ~SilentProxy()
      {
         stop_ = true;
         if (worker_.joinable())
            worker_.join();

         for (int held : held_)
            close(held);

         if (listen_socket_ >= 0)
            close(listen_socket_);
      }

      SilentProxy(const SilentProxy &) = delete;
      SilentProxy &operator=(const SilentProxy &) = delete;

      int Port() const { return port_; }
      int Accepted() const { return accepted_; }

      // The request lines read so far, one per connection, in order.
      std::vector<std::string> RequestLines() const
      {
         std::lock_guard<std::mutex> guard(lock_);
         return request_lines_;
      }

   private:
      void Run_()
      {
         while (!stop_)
         {
            pollfd waiting = {};
            waiting.fd = listen_socket_;
            waiting.events = POLLIN;

            if (poll(&waiting, 1, 100) <= 0)
               continue;

            const int connection = accept(listen_socket_, nullptr, nullptr);
            if (connection < 0)
               continue;

            held_.push_back(connection);
            accepted_++;

            // The request line, read for up to two seconds; nothing is answered.
            std::string received;
            const Clock::time_point until = Clock::now() + std::chrono::seconds(2);
            while (received.find("\r\n") == std::string::npos && Clock::now() < until)
            {
               pollfd readable = {};
               readable.fd = connection;
               readable.events = POLLIN;
               if (poll(&readable, 1, 100) <= 0)
                  continue;

               char buffer[512];
               const ssize_t count = recv(connection, buffer, sizeof(buffer), 0);
               if (count <= 0)
                  break;
               received.append(buffer, static_cast<size_t>(count));
            }

            std::lock_guard<std::mutex> guard(lock_);
            request_lines_.push_back(received.substr(0, received.find("\r\n")));
         }
      }

      int listen_socket_;
      int port_;
      std::atomic<int> accepted_;
      std::atomic<bool> stop_;
      std::vector<int> held_;
      mutable std::mutex lock_;
      std::vector<std::string> request_lines_;
      std::thread worker_;
   };

   // A UDP socket on 127.0.0.1 that takes every datagram and answers none.
   class SilentUdpPeer
   {
   public:
      SilentUdpPeer() : socket_(-1), port_(0)
      {
         socket_ = socket(AF_INET, SOCK_DGRAM, 0);
         if (socket_ < 0)
            return;

         sockaddr_in address = {};
         address.sin_family = AF_INET;
         address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

         socklen_t length = sizeof(address);
         if (bind(socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
             getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
         {
            close(socket_);
            socket_ = -1;
            return;
         }

         port_ = ntohs(address.sin_port);
      }

      ~SilentUdpPeer()
      {
         if (socket_ >= 0)
            close(socket_);
      }

      SilentUdpPeer(const SilentUdpPeer &) = delete;
      SilentUdpPeer &operator=(const SilentUdpPeer &) = delete;

      int Port() const { return port_; }

      // Whether a datagram is waiting - the query, which was sent here.
      bool Received() const
      {
         pollfd waiting = {};
         waiting.fd = socket_;
         waiting.events = POLLIN;
         return poll(&waiting, 1, 0) > 0;
      }

   private:
      int socket_;
      int port_;
   };

   // A port on 127.0.0.1 that nothing is bound to at the moment of asking.
   int FreeTcpPort()
   {
      const int probe = socket(AF_INET, SOCK_STREAM, 0);
      if (probe < 0)
         return 0;

      sockaddr_in address = {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      socklen_t length = sizeof(address);
      int port = 0;
      if (bind(probe, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
          getsockname(probe, reinterpret_cast<sockaddr *>(&address), &length) == 0)
         port = ntohs(address.sin_port);

      close(probe);
      return port;
   }

   // A connection to 127.0.0.1:port, or -1.
   int ConnectTo(int port)
   {
      const int connection = socket(AF_INET, SOCK_STREAM, 0);
      if (connection < 0)
         return -1;

      sockaddr_in address = {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(static_cast<unsigned short>(port));

      if (connect(connection, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
      {
         close(connection);
         return -1;
      }

      return connection;
   }

   // Everything the peer sends until it closes the connection, or until seconds have
   // passed; timedOut says which.
   std::string ReadUntilClosed(int connection, int seconds, bool &timedOut)
   {
      std::string received;
      timedOut = false;
      const Clock::time_point deadline = Clock::now() + std::chrono::seconds(seconds);

      for (;;)
      {
         const long long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
         if (left <= 0)
         {
            timedOut = true;
            return received;
         }

         pollfd waiting = {};
         waiting.fd = connection;
         waiting.events = POLLIN;
         if (poll(&waiting, 1, static_cast<int>(left)) <= 0)
            continue;

         char buffer[1024];
         const ssize_t count = recv(connection, buffer, sizeof(buffer), 0);
         if (count <= 0)
            return received;

         received.append(buffer, static_cast<size_t>(count));
      }
   }

   struct Outcome
   {
      Outcome() : finished(false), passed(false) {}

      std::atomic<bool> finished;
      bool passed;
      std::string said;
   };

   struct Case
   {
      std::string name;
      double deadline_seconds = 0;   // how long after the start it may still be running
      std::shared_ptr<Outcome> outcome;
   };

   // Runs one case on a thread of its own, which is detached: a case stuck in a
   // read is reported by the watch in main, which cannot wait for it.
   Case Start(const std::string &name, double deadline_seconds, const std::function<bool(std::string &)> &body)
   {
      Case started;
      started.name = name;
      started.deadline_seconds = deadline_seconds;
      started.outcome = std::make_shared<Outcome>();

      std::shared_ptr<Outcome> outcome = started.outcome;
      std::thread([outcome, body]()
      {
         std::string said;
         bool passed = false;
         try
         {
            passed = body(said);
         }
         catch (const std::exception &failure)
         {
            said = std::string("an exception: ") + failure.what();
         }
         catch (...)
         {
            said = "an exception of no standard type";
         }

         outcome->passed = passed;
         outcome->said = said;
         outcome->finished = true;
      }).detach();

      return started;
   }

   // Whether a step that should end at its deadline did: not before it (less a
   // second for the clock), and not long after it.
   bool EndedAtItsDeadline(double seconds, double deadline, std::string &said)
   {
      if (seconds < deadline - 1.0 || seconds > deadline + 10.0)
      {
         said = "it ended after " + Seconds(seconds) + ", where its deadline is " + Seconds(deadline);
         return false;
      }

      said = "gave up after " + Seconds(seconds) + " (deadline " + Seconds(deadline) + ")";
      return true;
   }

   bool HeaderLookup(std::string &said)
   {
      struct Row
      {
         const char *headers;
         const char *name;
         const char *expected;
      };

      const char *const response = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nLocation: https://ca.example/acct/1\r\nReplay-Nonce: last-nonce ";
      const Row rows[] =
      {
         // The header that comes last has no line break after it: the block is cut
         // at the blank line.
         { response, "replay-nonce", "last-nonce" },
         { response, "location", "https://ca.example/acct/1" },
         { response, "content-type", "application/json" },
         { response, "link", "" },
         // A name that only ends another header's is not that header.
         { "HTTP/1.1 200 OK\r\nContent-Location: https://ca.example/x", "location", "" },
         // A last header with nothing after its colon, and a block of the status
         // line alone.
         { "HTTP/1.1 200 OK\r\nReplay-Nonce:", "replay-nonce", "" },
         { "HTTP/1.1 204 No Content", "replay-nonce", "" },
      };

      for (const Row &row : rows)
      {
         const HM::AnsiString value = HM::AcmeClient::GetHeaderValue(row.headers, row.name);
         if (value != row.expected)
         {
            said = std::string("'") + row.name + "' read as '" + value.c_str() + "', not '" + row.expected + "'";
            return false;
         }
      }

      said = "the last header, one in the middle, the first, an absent one, Content-Location not taken for Location, an empty last value and no headers at all";
      return true;
   }
}

// Everything but the barrier main puts around it.
static int
RunNetworkDeadlineTests()
{
   // The service ignores SIGPIPE (platform/main.cpp), and so does this: the
   // responder writes to a client that may have gone.
   struct sigaction ignore = {};
   ignore.sa_handler = SIG_IGN;
   if (sigaction(SIGPIPE, &ignore, nullptr) != 0)
   {
      printf("FAIL  the network deadlines: SIGPIPE could not be ignored\n");
      return 1;
   }

   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-network-deadline-tests-" + std::to_string(static_cast<long long>(::getpid())));
   if (!error)
      boost::filesystem::create_directories(scratch, error);

   if (error)
   {
      printf("FAIL  the network deadlines: no scratch directory could be made under the system's temp directory\n");
      return 1;
   }

   // One peer for each client, so that each case knows its own was reached.
   SilentTcpPeer tokenEndpoint;
   SilentTcpPeer policyHost;
   SilentTcpPeer certificateAuthority;
   SilentUdpPeer nameServer;
   const int responderPort = FreeTcpPort();
   const int responderFloodPort = FreeTcpPort();

   if (tokenEndpoint.Port() == 0 || policyHost.Port() == 0 || certificateAuthority.Port() == 0 || nameServer.Port() == 0 ||
       responderPort == 0 || responderFloodPort == 0 || responderFloodPort == responderPort)
   {
      printf("FAIL  the network deadlines: no socket could be bound to 127.0.0.1\n");
      boost::filesystem::remove_all(scratch, error);
      return 1;
   }

   const std::string directory = scratch.string();
   const boost::filesystem::path file = scratch / "hMailServer.ini";
   {
      std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
      ini << "[Directories]\n"
          << "ProgramFolder=" << directory << "\n"
          << "DataFolder=" << directory << "\n"
          << "LogFolder=" << directory << "\n"
          << "TempFolder=" << directory << "\n"
          << "EventFolder=" << directory << "\n"
          << "DatabaseFolder=" << directory << "\n"
          << "\n"
          << "[Settings]\n"
          << "OutboundOAuth2TokenUrl=https://127.0.0.1:" << tokenEndpoint.Port() << "/token\n"
          << "OutboundOAuth2ClientId=network-deadline-tests\n"
          << "OutboundOAuth2ClientSecret=not-a-secret\n"
          << "AcmeDirectoryUrl=https://127.0.0.1:" << certificateAuthority.Port() << "/directory\n"
          << "AcmeDomains=mail.example.test\n"
          << "AcmeCertificateDirectory=" << directory << "\n";
      if (!ini)
      {
         printf("FAIL  the network deadlines: %s could not be written\n", file.string().c_str());
         boost::filesystem::remove_all(scratch, error);
         return 1;
      }
   }

   try
   {
      HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
      HM::IniFileSettings::Instance()->LoadSettings();
      HM::NetworkDeadlineTests::UseTlsSettingsWithoutDatabase();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the network deadlines: the configuration could not be loaded: %s\n", failure.what());
      boost::filesystem::remove_all(scratch, error);
      return 1;
   }

   // An error reported where the settings hold no UseScriptServer, as this
   // program's do not: the report reads the switch, the read reports the setting
   // missing (HM5015), and until 6.3.6 that report read the switch again, without
   // end, until the thread's stack ran out - here at the first TLS context made
   // against an OpenSSL older than 3.5, which reports the key-exchange groups it
   // lacks (HM5720). Coming back from the report is the check.
   HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::Low, 5015, "network-deadline-tests",
      "An error reported where the settings hold no UseScriptServer.");
   printf("ok    an error reported where the settings hold no UseScriptServer: logged, no event fired, and the report came back\n");

   std::vector<Case> cases;

   cases.push_back(Start("the ACME header lookup", 5, [](std::string &said)
   {
      return HeaderLookup(said);
   }));

   cases.push_back(Start("the OAuth2 token request", 20 + 15, [&tokenEndpoint](std::string &said)
   {
      const Clock::time_point started = Clock::now();
      HM::String token, why;
      const bool obtained = HM::OutboundOAuth2TokenClient::Instance()->GetToken(token, why);
      const double seconds = SecondsSince(started);
      const std::string reason = HM::AnsiString(why).c_str();

      if (obtained)
      {
         said = "a token was obtained from a peer that never answered";
         return false;
      }
      if (tokenEndpoint.Accepted() == 0)
      {
         said = "the token endpoint's port was never reached: " + reason;
         return false;
      }
      if (reason.find("in the time allowed") == std::string::npos)
      {
         said = "it did not end at the deadline: " + reason;
         return false;
      }
      return EndedAtItsDeadline(seconds, 20, said);
   }));

   cases.push_back(Start("the MTA-STS policy fetch, a host that carries a port", 5, [&policyHost](std::string &said)
   {
      HM::AnsiString body;
      HM::String why;
      const bool fetched = HM::TlsPolicy::HttpsGet_(HM::String("127.0.0.1:") + HM::String(std::to_string(policyHost.Port()).c_str()),
                                                    "/.well-known/mta-sts.txt", body, why);
      const std::string reason = HM::AnsiString(why).c_str();

      if (fetched || policyHost.Accepted() != 0)
      {
         said = "the peer on the port the host named was reached: " + reason;
         return false;
      }
      if (reason.find("not a host name") == std::string::npos)
      {
         said = "it was not refused as no host name: " + reason;
         return false;
      }
      said = "refused, and the port it named never reached";
      return true;
   }));

   cases.push_back(Start("the MTA-STS policy fetch, a host at a private address", 5, [](std::string &said)
   {
      // As written, and as one number, which the system's resolver reads as the
      // same address: the address it leads to is judged, not the text. The
      // refusal names the host as it was given and not the address the number
      // led to (since 6.3.6): which private address a name leads to is not for
      // whoever named it.
      for (const char *host : { "127.0.0.1", "2130706433" })
      {
         HM::AnsiString body;
         HM::String why;
         const bool fetched = HM::TlsPolicy::HttpsGet_(HM::String(host), "/.well-known/mta-sts.txt", body, why);
         const std::string reason = HM::AnsiString(why).c_str();

         if (fetched || reason.find("not a public address") == std::string::npos)
         {
            said = std::string(host) + " was not refused as a private address: " + reason;
            return false;
         }
         if (std::string(host) != "127.0.0.1" && reason.find("127.0.0.1") != std::string::npos)
         {
            said = std::string(host) + " was refused naming the address it led to: " + reason;
            return false;
         }
      }
      said = "127.0.0.1 and 2130706433 refused as private addresses, the second without the address it led to";
      return true;
   }));

   cases.push_back(Start("the ACME request", 30 + 15, [&certificateAuthority](std::string &said)
   {
      const Clock::time_point started = Clock::now();
      HM::AcmeClient client;
      const bool issued = client.RequestCertificate();
      const double seconds = SecondsSince(started);

      if (issued)
      {
         said = "a certificate was issued by a peer that never answered";
         return false;
      }
      if (certificateAuthority.Accepted() == 0)
      {
         said = "the directory's port was never reached";
         return false;
      }
      return EndedAtItsDeadline(seconds, 30, said);
   }));

   cases.push_back(Start("the DANE TLSA query", 5 + 15, [&nameServer](std::string &said)
   {
      sockaddr_in server = {};
      server.sin_family = AF_INET;
      server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      server.sin_port = htons(static_cast<unsigned short>(nameServer.Port()));

      const Clock::time_point started = Clock::now();
      std::vector<unsigned char> response;
      const bool answered = HM::TlsPolicy::RunDnsQuery_(server, "_25._tcp.mail.example.test", 52, response);
      const double seconds = SecondsSince(started);

      if (answered)
      {
         said = "a reply was taken from a server that never sent one";
         return false;
      }
      if (!nameServer.Received())
      {
         said = "the query never reached the server";
         return false;
      }
      return EndedAtItsDeadline(seconds, 5, said);
   }));

   cases.push_back(Start("the ACME http-01 responder", 5 + 30, [responderPort](std::string &said)
   {
      HM::AcmeChallengeServer responder;
      if (!responder.Start(responderPort))
      {
         said = "it could not listen on port " + std::to_string(responderPort);
         return false;
      }
      responder.SetChallenge("network-deadline-token", "network-deadline-token.key-authorization");

      // A client that connects and sends nothing, then the CA's request behind it.
      const int idle = ConnectTo(responderPort);
      const int validator = ConnectTo(responderPort);
      if (idle < 0 || validator < 0)
      {
         said = "no connection could be made to it";
         if (idle >= 0)
            close(idle);
         if (validator >= 0)
            close(validator);
         responder.Stop();
         return false;
      }

      const std::string request = "GET /.well-known/acme-challenge/network-deadline-token HTTP/1.0\r\nHost: mail.example.test\r\n\r\n";
      const Clock::time_point started = Clock::now();
      const bool sent = send(validator, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size());

      bool timedOut = false;
      const std::string answer = sent ? ReadUntilClosed(validator, 20, timedOut) : std::string();
      const double seconds = SecondsSince(started);

      // Closed before the stop, so that a responder still waiting on the idle
      // client - the defect - wakes and can be stopped.
      close(idle);
      close(validator);
      responder.Stop();

      if (!sent || timedOut || answer.find(" 200 ") == std::string::npos ||
          answer.find("network-deadline-token.key-authorization") == std::string::npos)
      {
         said = "the request behind an idle client was not answered with the key authorization within " + Seconds(seconds);
         return false;
      }
      if (seconds > 5 + 10)
      {
         said = "the request behind an idle client was answered only after " + Seconds(seconds);
         return false;
      }

      // Started again on the same port at once, while the connections it has just
      // closed are in TIME_WAIT: SO_REUSEADDR, which POSIX keeps.
      if (!responder.Start(responderPort))
      {
         said = "it could not listen again on port " + std::to_string(responderPort) + " straight after serving";
         return false;
      }
      responder.Stop();

      said = "an idle client let go and the request behind it answered after " + Seconds(seconds) + "; started again on its port at once";
      return true;
   }));

   cases.push_back(Start("the ACME http-01 responder under parallel idle connections", 30, [responderFloodPort](std::string &said)
   {
      // A crowd of clients that connect and send nothing, and then the CA's own
      // request. Served one at a time, each idle client holds the responder for
      // its five-second receive timeout, so the validator behind sixteen of them
      // would wait about eighty seconds; served concurrently, each idle client
      // holds only its own thread and the validator is answered at once. So the
      // validator must be answered well inside one idle client's timeout - two
      // seconds is far below sixteen times five and far above a loopback
      // round-trip - which it cannot be unless the connections are concurrent
      // (W-11).
      HM::AcmeChallengeServer responder;
      if (!responder.Start(responderFloodPort))
      {
         said = "it could not listen on port " + std::to_string(responderFloodPort);
         return false;
      }
      responder.SetChallenge("flood-token", "flood-token.key-authorization");

      const int idleCount = 16;
      std::vector<int> idle;
      for (int i = 0; i < idleCount; i++)
      {
         const int connection = ConnectTo(responderFloodPort);
         if (connection >= 0)
            idle.push_back(connection);
      }

      if ((int) idle.size() != idleCount)
      {
         for (int connection : idle)
            close(connection);
         responder.Stop();
         said = "only " + std::to_string(idle.size()) + " of " + std::to_string(idleCount) + " idle connections could be made";
         return false;
      }

      const int validator = ConnectTo(responderFloodPort);
      if (validator < 0)
      {
         for (int connection : idle)
            close(connection);
         responder.Stop();
         said = "the validator could not connect behind the idle clients";
         return false;
      }

      const std::string request = "GET /.well-known/acme-challenge/flood-token HTTP/1.0\r\nHost: mail.example.test\r\n\r\n";
      const Clock::time_point started = Clock::now();
      const bool sent = send(validator, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size());

      bool timedOut = false;
      const std::string answer = sent ? ReadUntilClosed(validator, 10, timedOut) : std::string();
      const double seconds = SecondsSince(started);

      for (int connection : idle)
         close(connection);
      close(validator);
      responder.Stop();

      if (!sent || timedOut || answer.find(" 200 ") == std::string::npos ||
          answer.find("flood-token.key-authorization") == std::string::npos)
      {
         said = "the CA's request behind " + std::to_string(idleCount) + " idle clients was not answered with the key authorization within " + Seconds(seconds);
         return false;
      }
      if (seconds > 2)
      {
         said = "the CA's request behind " + std::to_string(idleCount) + " idle clients was answered only after " + Seconds(seconds) + " - the responder serves one connection at a time";
         return false;
      }

      said = "the CA's request behind " + std::to_string(idleCount) + " idle clients was answered in " + Seconds(seconds);
      return true;
   }));

   // The watch: every case has its own deadline, and one that has not finished by
   // it is stuck - reported, and the others waited for. A stuck case's thread is
   // blocked in a read and cannot be joined, so the program then ends with _exit,
   // which runs nothing more: the peers that thread is reading from stay put.
   const Clock::time_point started = Clock::now();
   int failures = 0;
   bool stuck = false;
   std::vector<bool> reported(cases.size(), false);
   size_t left = cases.size();

   while (left > 0)
   {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const double seconds = SecondsSince(started);

      for (size_t i = 0; i < cases.size(); i++)
      {
         if (reported[i])
            continue;

         if (cases[i].outcome->finished)
         {
            printf("%s  %s: %s\n", cases[i].outcome->passed ? "ok  " : "FAIL", cases[i].name.c_str(), cases[i].outcome->said.c_str());
            failures += cases[i].outcome->passed ? 0 : 1;
         }
         else if (seconds > cases[i].deadline_seconds)
         {
            printf("FAIL  %s: still waiting after %s - a peer that never answers holds it\n", cases[i].name.c_str(), Seconds(seconds).c_str());
            failures++;
            stuck = true;
         }
         else
         {
            continue;
         }

         reported[i] = true;
         left--;
      }
   }

   // The second phase: the MTA-STS fetch through HttpProxy. HttpProxy carries every
   // request of HttpsClient's to an address on this machine when the proxy is on it
   // too, so it is set only now that the token and ACME requests of the first phase
   // have ended; a stuck case skips it, since its thread may still be in one.
   SilentProxy proxy;
   if (!stuck)
   {
      bool configured = proxy.Port() != 0;
      if (configured)
      {
         std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::app);
         ini << "HttpProxy=127.0.0.1:" << proxy.Port() << "\n";
         configured = static_cast<bool>(ini);
      }
      if (configured)
      {
         try
         {
            HM::IniFileSettings::Instance()->LoadSettings();
         }
         catch (const std::exception &failure)
         {
            printf("FAIL  the MTA-STS policy fetch through HttpProxy: the configuration could not be loaded again: %s\n", failure.what());
            configured = false;
         }
      }
      else
      {
         printf("FAIL  the MTA-STS policy fetch through HttpProxy: no proxy could be set up on 127.0.0.1\n");
      }

      if (!configured)
      {
         failures++;
      }
      else
      {
         std::vector<Case> proxied;
         proxied.push_back(Start("the MTA-STS policy fetch through HttpProxy", 15 + 15, [&proxy](std::string &said)
         {
            HM::AnsiString body;
            HM::String why;

            // A private address is refused before the proxy is asked anything.
            if (HM::TlsPolicy::HttpsGet_(HM::String("2130706433"), "/.well-known/mta-sts.txt", body, why) ||
                HM::AnsiString(why).Find("not a public address") < 0)
            {
               said = std::string("2130706433 was not refused as a private address: ") + HM::AnsiString(why).c_str();
               return false;
            }
            if (proxy.Accepted() != 0)
            {
               said = "the proxy was asked for a tunnel to a private address";
               return false;
            }

            // A public one goes through it, as the address that was judged.
            const Clock::time_point started = Clock::now();
            const bool fetched = HM::TlsPolicy::HttpsGet_(HM::String("3227017985"), "/.well-known/mta-sts.txt", body, why);
            const double seconds = SecondsSince(started);
            const std::string reason = HM::AnsiString(why).c_str();
            const std::vector<std::string> lines = proxy.RequestLines();

            if (fetched)
            {
               said = "a policy was fetched through a proxy that never answered";
               return false;
            }
            if (lines.size() != 1 || lines[0] != "CONNECT 192.88.99.1:443 HTTP/1.1")
            {
               said = "the proxy was not asked for one tunnel to 192.88.99.1:443 but for: ";
               for (const std::string &line : lines)
                  said += "'" + line + "' ";
               said += "(" + reason + ")";
               return false;
            }
            if (reason.find("in the time allowed") == std::string::npos)
            {
               said = "it did not end at the deadline: " + reason;
               return false;
            }
            if (!EndedAtItsDeadline(seconds, 15, said))
               return false;

            said = "2130706433 refused with the proxy never asked; 3227017985 asked of the proxy as CONNECT 192.88.99.1:443, and " + said;
            return true;
         }));

         const Clock::time_point second = Clock::now();
         while (!proxied[0].outcome->finished && SecondsSince(second) <= proxied[0].deadline_seconds)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

         if (proxied[0].outcome->finished)
         {
            printf("%s  %s: %s\n", proxied[0].outcome->passed ? "ok  " : "FAIL", proxied[0].name.c_str(), proxied[0].outcome->said.c_str());
            failures += proxied[0].outcome->passed ? 0 : 1;
         }
         else
         {
            printf("FAIL  %s: still waiting after %s - a proxy that never answers holds it\n", proxied[0].name.c_str(), Seconds(SecondsSince(second)).c_str());
            failures++;
            stuck = true;
         }

         // Then, through the same proxy, a Web Key Directory's fetch, whose
         // host an address a person typed names (OpenPgpRing::FetchFromDirectory):
         // held to public addresses as the policy fetch is, and since 6.3.6 asked
         // of the proxy as the address that was judged, where until then it went
         // direct whatever HttpProxy said. Five seconds, as the lookup would give
         // a leg with five seconds left.
         if (!stuck)
         {
            proxied.push_back(Start("the Web Key Directory fetch through HttpProxy", 5 + 15, [&proxy](std::string &said)
            {
               HM::AnsiString body;
               int status = 0;
               HM::String why;
               const size_t linesBefore = proxy.RequestLines().size();
               const int acceptedBefore = proxy.Accepted();

               // A private address is refused before the proxy is asked anything,
               // and the refusal - which the person who typed the address reads -
               // does not name the address the host led to.
               if (HM::OpenPgpRing::FetchFromDirectory("https://2130706433/.well-known/openpgpkey/hu/x?l=a", 5, 4096, body, status, why) ||
                   HM::AnsiString(why).Find("not a public address") < 0 || HM::AnsiString(why).Find("127.0.0.1") >= 0)
               {
                  said = std::string("2130706433 was not refused as a private address, or the refusal named the address it led to: ") + HM::AnsiString(why).c_str();
                  return false;
               }
               if (proxy.Accepted() != acceptedBefore)
               {
                  said = "the proxy was asked for a tunnel to a private address";
                  return false;
               }

               // A public one goes through it, as the address that was judged.
               const Clock::time_point started = Clock::now();
               const bool fetched = HM::OpenPgpRing::FetchFromDirectory("https://3227017985/.well-known/openpgpkey/hu/x?l=a", 5, 4096, body, status, why);
               const double seconds = SecondsSince(started);
               const std::string reason = HM::AnsiString(why).c_str();
               const std::vector<std::string> lines = proxy.RequestLines();

               if (fetched)
               {
                  said = "a key was fetched through a proxy that never answered";
                  return false;
               }
               if (lines.size() != linesBefore + 1 || lines.back() != "CONNECT 192.88.99.1:443 HTTP/1.1")
               {
                  said = "the proxy was not asked for one tunnel to 192.88.99.1:443 but for: ";
                  for (size_t i = linesBefore; i < lines.size(); i++)
                     said += "'" + lines[i] + "' ";
                  said += "(" + reason + ")";
                  return false;
               }
               if (reason.find("in the time allowed") == std::string::npos)
               {
                  said = "it did not end at the deadline: " + reason;
                  return false;
               }
               if (!EndedAtItsDeadline(seconds, 5, said))
                  return false;

               said = "2130706433 refused with the proxy never asked; 3227017985 asked of the proxy as CONNECT 192.88.99.1:443, and " + said;
               return true;
            }));

            const Clock::time_point third = Clock::now();
            while (!proxied[1].outcome->finished && SecondsSince(third) <= proxied[1].deadline_seconds)
               std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (proxied[1].outcome->finished)
            {
               printf("%s  %s: %s\n", proxied[1].outcome->passed ? "ok  " : "FAIL", proxied[1].name.c_str(), proxied[1].outcome->said.c_str());
               failures += proxied[1].outcome->passed ? 0 : 1;
            }
            else
            {
               printf("FAIL  %s: still waiting after %s - a proxy that never answers holds it\n", proxied[1].name.c_str(), Seconds(SecondsSince(third)).c_str());
               failures++;
               stuck = true;
            }
         }
      }
   }

   boost::filesystem::remove_all(scratch, error);

   if (stuck)
   {
      // std::flush and not fflush: it writes out what printf buffered all the same
      // (the streams are synchronised with stdio), and _exit would not.
      std::cout << std::flush;
      _exit(1);
   }

   return failures == 0 ? 0 : 1;
}

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      return RunNetworkDeadlineTests();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the network deadlines: %s\n", failure.what());
   }
   catch (...)
   {
      printf("FAIL  the network deadlines: an exception of no standard type\n");
   }
   return 1;
}
