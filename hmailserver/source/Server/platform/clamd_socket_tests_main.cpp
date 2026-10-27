// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The ClamAV client over clamd's Unix socket, a program of its own for the POSIX
// build, which linux-build.sh runs with every other *-tests program.
//
// On Linux ClamAVHost may be the absolute path of clamd's LocalSocket in place of a
// host name (AntiVirusConfiguration::IsClamAVSocketPath), and the client then
// connects to that socket (SynchronousConnection::ConnectLocal) and says what it
// says over TCP. Against a fake clamd of its own on a socket under a directory of
// its own, this holds:
//   - a clean sample, streamed with INSTREAM in length-prefixed chunks and ended
//     with a zero length, is clean, and the fake received it byte for byte - from
//     memory (ScanData, which the connection test uses) and from a file of three
//     chunks (Scan, which every message goes through);
//   - an infected one is reported with the name clamd gave;
//   - PING is answered PONG and VERSION's reply is the daemon's identity, and a
//     PING answered otherwise is reported as such, naming the socket;
//   - the connection test (VirusScannerTester::TestClamAVConnect, what COM's
//     TestClamAVScanner runs) passes against it and names the daemon;
//   - a path with no socket at it, and a socket nothing accepts on, are reported as
//     nothing listening there; a socket the user may not write to as a matter of
//     its permissions, and a socket in a directory the user may not enter as a
//     matter of that directory (both as an ordinary user only: root is never
//     refused); a directory or a file at the path as not a socket;
//   - a clamd whose queue of connections waiting to be accepted is full is waited
//     for until the deadline, as over TCP: drained inside it, the scan is answered,
//     and never drained, the deadline is reported, naming the socket;
//   - a clamd that takes the stream and never answers is let go at the deadline
//     (ClamMinTimeout and ClamMaxTimeout, 2 seconds in this program's settings);
//   - a path longer than a socket address holds is refused before anything is
//     tried, and a host name is not a path.
//
// Given the path of a real clamd's socket as its argument, it then runs the
// connection test against that clamd too: PING, VERSION, the plain sample and
// EICAR, as the Control Panel's button does.
//
// A configuration file of its own and the sockets in a directory under the
// system's temp directory (or /tmp, when that path would be too long for a socket
// address), removed again. No database, no network: only Unix sockets of its own,
// for which the source packages' tester lists, which hold the testers that need no
// socket, leave it out (build/check-source-tarball.py). About six seconds.
//
// It prints one line per case and exits non-zero when any fails.

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>

#include "../Common/AntiVirus/AntiVirusConfiguration.h"
#include "../Common/AntiVirus/ClamAVVirusScanner.h"
#include "../Common/AntiVirus/VirusScannerTester.h"
#include "../Common/AntiVirus/VirusScanningResult.h"
#include "../Common/Application/IniFileSettings.h"

namespace
{
   typedef std::chrono::steady_clock Clock;

   double SecondsSince(Clock::time_point start)
   {
      return std::chrono::duration<double>(Clock::now() - start).count();
   }

   std::string Narrow(const HM::String &text)
   {
      return HM::AnsiString(text).c_str();
   }

   bool Contains(const std::string &text, const std::string &part)
   {
      return text.find(part) != std::string::npos;
   }

   // The address of the Unix socket at path. The structure is zeroed and no more
   // than sun_path's size less one byte is copied, so the path is always ended by
   // a zero; the paths this program binds and connects to are made to fit.
   sockaddr_un UnixAddress(const std::string &path)
   {
      sockaddr_un address = {};
      address.sun_family = AF_UNIX;
      path.copy(address.sun_path, sizeof(address.sun_path) - 1);
      return address;
   }

   // A clamd of this program's own on a Unix socket: one connection at a time, one
   // command a connection, as clamd takes the n-prefixed commands.
   class FakeClamd
   {
   public:
      enum Behaviour
      {
         Answers,        // PONG, the version, and a verdict on the stream
         WrongPing,      // anything but PONG to PING
         NeverAnswers    // reads the command and the stream and says nothing
      };

      // held: accepts nothing until Release, so that the listener's queue of
      // connections waiting to be accepted, backlog long, can be filled.
      FakeClamd(const std::string &path, Behaviour behaviour, int backlog = 8, bool held = false) :
         path_(path),
         behaviour_(behaviour),
         listener_(-1),
         stop_(false),
         held_(held)
      {
         listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
         if (listener_ < 0)
            return;

         sockaddr_un address = UnixAddress(path_);
         ::unlink(path_.c_str());
         if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener_, backlog) != 0)
         {
            ::close(listener_);
            listener_ = -1;
            return;
         }

         thread_ = std::thread([this]() { Serve_(); });
      }

      ~FakeClamd()
      {
         stop_ = true;
         if (thread_.joinable())
            thread_.join();
         if (listener_ >= 0)
            ::close(listener_);
         ::unlink(path_.c_str());
      }

      bool Listening() const { return listener_ >= 0; }

      // Starts accepting, and so drains the queue.
      void Release() { held_ = false; }

      std::vector<std::string> Commands()
      {
         std::lock_guard<std::mutex> lock(mutex_);
         return commands_;
      }

      // What each INSTREAM carried, joined, and the sizes of its chunks.
      std::vector<std::string> Streams()
      {
         std::lock_guard<std::mutex> lock(mutex_);
         return streams_;
      }

      std::vector<std::vector<size_t>> Chunks()
      {
         std::lock_guard<std::mutex> lock(mutex_);
         return chunks_;
      }

      static const char *Version() { return "ClamAV 1.4.3/27800/Mon Oct 26 09:00:00 2026"; }
      static const char *Marker() { return "FAKE-CLAMD-INFECTED-MARKER"; }
      static const char *VirusName() { return "Fake-Clamd.Test-Signature"; }

   private:
      void Serve_()
      {
         while (!stop_)
         {
            if (held_)
            {
               std::this_thread::sleep_for(std::chrono::milliseconds(20));
               continue;
            }

            pollfd ready = { listener_, POLLIN, 0 };
            if (::poll(&ready, 1, 100) <= 0)
               continue;

            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0)
               continue;

            Converse_(client);
            ::close(client);
         }
      }

      // Reads exactly size bytes, or gives up when the client goes or this stops.
      bool Read_(int client, char *into, size_t size)
      {
         size_t have = 0;
         while (have < size && !stop_)
         {
            pollfd ready = { client, POLLIN, 0 };
            if (::poll(&ready, 1, 100) <= 0)
               continue;

            const ssize_t got = ::recv(client, into + have, size - have, 0);
            if (got <= 0)
               return false;
            have += static_cast<size_t>(got);
         }
         return have == size;
      }

      // A client that has gone is not answered, and there is nothing more to do.
      bool Say_(int client, const std::string &line)
      {
         const std::string text = line + "\n";
         return ::send(client, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size());
      }

      void Converse_(int client)
      {
         std::string command;
         char c = 0;
         while (Read_(client, &c, 1) && c != '\n' && command.size() < 64)
            command += c;

         {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(command);
         }

         if (command == "nPING")
         {
            if (behaviour_ == Answers)
               Say_(client, "PONG");
            else if (behaviour_ == WrongPing)
               Say_(client, "HELLO");
            else
               WaitUntilStopped_(client);
            return;
         }

         if (command == "nVERSION")
         {
            if (behaviour_ == NeverAnswers)
               WaitUntilStopped_(client);
            else
               Say_(client, Version());
            return;
         }

         if (command != "nINSTREAM")
            return;

         std::string stream;
         std::vector<size_t> sizes;
         for (;;)
         {
            unsigned char prefix[4];
            if (!Read_(client, reinterpret_cast<char*>(prefix), sizeof(prefix)))
               return;

            const size_t size = (static_cast<size_t>(prefix[0]) << 24) | (static_cast<size_t>(prefix[1]) << 16) |
                                (static_cast<size_t>(prefix[2]) << 8) | static_cast<size_t>(prefix[3]);
            if (size == 0)
               break;
            if (size > size_t{1024} * 1024)
               return;

            std::string chunk(size, '\0');
            if (!Read_(client, &chunk[0], size))
               return;
            stream += chunk;
            sizes.push_back(size);
         }

         {
            std::lock_guard<std::mutex> lock(mutex_);
            streams_.push_back(stream);
            chunks_.push_back(sizes);
         }

         if (behaviour_ == NeverAnswers)
         {
            WaitUntilStopped_(client);
            return;
         }

         // clamd's own forms of the two verdicts: the marker, and the EICAR test
         // string by the part of it that is not the start of the file.
         if (Contains(stream, Marker()))
            Say_(client, std::string("stream: ") + VirusName() + " FOUND");
         else if (Contains(stream, "STANDARD-ANTIVIRUS-TEST-FILE"))
            Say_(client, "stream: Eicar-Test-Signature FOUND");
         else
            Say_(client, "stream: OK");
      }

      // Holds the connection open and silent until the client gives up or this stops.
      void WaitUntilStopped_(int client)
      {
         char ignored[256];
         while (!stop_)
         {
            pollfd ready = { client, POLLIN, 0 };
            if (::poll(&ready, 1, 100) <= 0)
               continue;
            if (::recv(client, ignored, sizeof(ignored), 0) <= 0)
               return;
         }
      }

      const std::string path_;
      const Behaviour behaviour_;
      int listener_;
      std::atomic<bool> stop_;
      std::atomic<bool> held_;
      std::thread thread_;

      std::mutex mutex_;
      std::vector<std::string> commands_;
      std::vector<std::string> streams_;
      std::vector<std::vector<size_t>> chunks_;
   };

   int failures = 0;

   void Report(bool passed, const std::string &name, const std::string &said)
   {
      printf("%s  %s: %s\n", passed ? "ok  " : "FAIL", name.c_str(), said.c_str());
      if (!passed)
         failures++;
   }

   // A directory for the sockets whose paths fit a socket address with room to
   // spare: the system's temp directory, or /tmp when that is too deep.
   boost::filesystem::path ScratchDirectory()
   {
      const std::string leaf = "hm-clamd-socket-tests-" + std::to_string(static_cast<long long>(::getpid()));

      boost::system::error_code error;
      boost::filesystem::path base = boost::filesystem::temp_directory_path(error);
      if (error || (base / leaf / "never-answers.sock").string().size() > 90)
         base = "/tmp";

      boost::filesystem::path scratch = base / leaf;
      boost::filesystem::create_directories(scratch, error);
      return error ? boost::filesystem::path() : scratch;
   }

   // Connects to the socket at path without waiting, until the listener's queue of
   // connections waiting to be accepted is full, which a non-blocking connect() to a
   // Unix socket reports as EAGAIN. Returns the connections made, for the caller to
   // close; full says whether the queue was filled.
   std::vector<int> FillQueue(const std::string &path, bool &full)
   {
      std::vector<int> made;
      full = false;

      sockaddr_un address = UnixAddress(path);

      for (int i = 0; i < 64; i++)
      {
         const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
         if (client < 0)
            break;

         if (::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
         {
            made.push_back(client);
            continue;
         }

         full = errno == EAGAIN;
         ::close(client);
         break;
      }

      return made;
   }

   void CloseAll(std::vector<int> &connections)
   {
      for (const int connection : connections)
         ::close(connection);
      connections.clear();
   }

   void TheFakeCases(const std::string &directory)
   {
      const std::string answers = directory + "/clamd.sock";
      const HM::String host(answers.c_str());

      {
         FakeClamd clamd(answers, FakeClamd::Answers);
         if (!clamd.Listening())
         {
            Report(false, "the fake clamd", "no socket could be bound at " + answers);
            return;
         }

         // A clean sample from memory, the way the connection test sends it.
         HM::VirusScanningResult clean = HM::ClamAVVirusScanner::ScanData(host, 0, "Test");
         std::vector<std::string> streams = clamd.Streams();
         std::vector<std::vector<size_t>> chunks = clamd.Chunks();
         std::vector<std::string> commands = clamd.Commands();
         Report(!clean.GetVirusFound() && !clean.GetErrorOccured() && Narrow(clean.GetDetails()) == "Result: stream: OK" &&
                commands.size() == 1 && commands[0] == "nINSTREAM" && streams.size() == 1 && streams[0] == "Test" &&
                chunks[0].size() == 1 && chunks[0][0] == 4,
            "a clean sample from memory",
            Narrow(clean.GetDetails()) + " (" + std::to_string(commands.size()) + " command(s), " +
            (streams.empty() ? std::string("no stream") : "a stream of " + std::to_string(streams.back().size()) + " bytes") + ")");

         // A file of three chunks, the way every message is sent: 4096 + 4096 + 1808.
         const std::string file = directory + "/message.eml";
         std::string body;
         for (int i = 0; body.size() < 10000; i++)
            body += "Line " + std::to_string(i) + " of a message that carries nothing.\r\n";
         body.resize(10000);
         {
            std::ofstream out(file.c_str(), std::ios::binary | std::ios::trunc);
            out << body;
         }

         HM::VirusScanningResult fileClean = HM::ClamAVVirusScanner::Scan(host, 0, HM::String(file.c_str()));
         streams = clamd.Streams();
         chunks = clamd.Chunks();
         Report(!fileClean.GetVirusFound() && !fileClean.GetErrorOccured() && streams.size() == 2 && streams[1] == body &&
                chunks[1] == std::vector<size_t>({ 4096, 4096, 1808 }),
            "a clean file in three chunks",
            Narrow(fileClean.GetDetails()) + (streams.size() == 2 && streams[1] == body ? ", the fake received the file byte for byte" : ", the fake did not receive the file"));

         // An infected one, by the marker the fake looks for.
         {
            std::ofstream out(file.c_str(), std::ios::binary | std::ios::trunc);
            out << body << FakeClamd::Marker() << "\r\n";
         }
         HM::VirusScanningResult infected = HM::ClamAVVirusScanner::Scan(host, 0, HM::String(file.c_str()));
         Report(infected.GetVirusFound() && Narrow(infected.GetDetails()) == FakeClamd::VirusName(),
            "an infected file", "virus found: " + Narrow(infected.GetDetails()));

         // PING and VERSION, each on a connection of its own.
         HM::String version, error;
         const size_t before = clamd.Commands().size();
         const bool pinged = HM::ClamAVVirusScanner::Ping(host, 0, version, error);
         commands = clamd.Commands();
         Report(pinged && Narrow(version) == FakeClamd::Version() && commands.size() == before + 2 &&
                commands[before] == "nPING" && commands[before + 1] == "nVERSION",
            "PING and VERSION", pinged ? "PONG, and the daemon is " + Narrow(version) : Narrow(error));

         // The connection test, as COM's TestClamAVScanner runs it: PING, VERSION, a
         // clean sample and EICAR, each on a connection of its own.
         HM::VirusScannerTester tester;
         HM::String message;
         const size_t first = clamd.Commands().size();
         const bool passed = tester.TestClamAVConnect(host, 0, message);
         commands = clamd.Commands();
         Report(passed && Narrow(message) == std::string(FakeClamd::Version()) + ". Eicar-Test-Signature" &&
                commands.size() == first + 4 && commands[first] == "nPING" && commands[first + 1] == "nVERSION" &&
                commands[first + 2] == "nINSTREAM" && commands[first + 3] == "nINSTREAM",
            "the connection test", Narrow(message));

         ::unlink(file.c_str());
      }

      // A PING answered with something other than PONG.
      {
         const std::string path = directory + "/wrong-ping.sock";
         FakeClamd clamd(path, FakeClamd::WrongPing);
         HM::String version, error;
         const bool pinged = HM::ClamAVVirusScanner::Ping(HM::String(path.c_str()), 0, version, error);
         Report(!pinged && Narrow(error) == "clamd at " + path + " answered PING with 'HELLO' rather than PONG.",
            "a PING answered otherwise", Narrow(error));
      }

      // No socket at the path.
      {
         const std::string path = directory + "/no-such.sock";
         HM::String version, error;
         const bool pinged = HM::ClamAVVirusScanner::Ping(HM::String(path.c_str()), 0, version, error);
         HM::VirusScanningResult scanned = HM::ClamAVVirusScanner::ScanData(HM::String(path.c_str()), 0, "Test");
         const std::string expected = "Unable to connect to ClamAV server at " + path + ": nothing is listening there, as there is no socket at that path.";
         Report(!pinged && Contains(Narrow(error), expected) && scanned.GetErrorOccured() && Contains(Narrow(scanned.GetDetails()), expected),
            "a missing socket", Narrow(error));
      }

      // A socket nothing accepts on: bound, and its listener gone, which leaves the
      // file behind as a clamd that stopped uncleanly does.
      {
         const std::string path = directory + "/stale.sock";
         const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
         sockaddr_un address = UnixAddress(path);
         const bool bound = stale >= 0 && ::bind(stale, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
         if (stale >= 0)
            ::close(stale);

         HM::String version, error;
         const bool pinged = bound && HM::ClamAVVirusScanner::Ping(HM::String(path.c_str()), 0, version, error);
         Report(bound && !pinged && Contains(Narrow(error), "Unable to connect to ClamAV server at " + path + ": nothing is listening there: the socket is there, and no clamd accepts on it."),
            "a socket nothing accepts on", bound ? Narrow(error) : "no socket could be bound at " + path);
         ::unlink(path.c_str());
      }

      // A socket this user may not write to. Root is never refused, so the case
      // says it was not run rather than passing.
      if (::geteuid() == 0)
      {
         printf("skip  a socket this user may not write to: running as root, which no socket's mode refuses\n");
      }
      else
      {
         const std::string path = directory + "/no-write.sock";
         FakeClamd clamd(path, FakeClamd::Answers);
         const bool closed = clamd.Listening() && ::chmod(path.c_str(), 0) == 0;
         HM::String version, error;
         const bool pinged = closed && HM::ClamAVVirusScanner::Ping(HM::String(path.c_str()), 0, version, error);
         Report(closed && !pinged && Contains(Narrow(error), "the socket is there, and this server's user may not connect to it") &&
                Contains(Narrow(error), "LocalSocketMode 666"),
            "a socket this user may not write to", Narrow(error));
      }

      // A path that is there and is not a socket: the directory the sockets are in,
      // and a file, this program's configuration.
      {
         HM::VirusScanningResult scanned = HM::ClamAVVirusScanner::ScanData(HM::String(directory.c_str()), 0, "Test");
         Report(scanned.GetErrorOccured() && Contains(Narrow(scanned.GetDetails()), "Unable to connect to ClamAV server at " + directory + ": it is a directory, not a socket.") &&
                Contains(Narrow(scanned.GetDetails()), "LocalSocket in clamd.conf"),
            "a directory at the path", Narrow(scanned.GetDetails()));

         const std::string file = directory + "/hMailServer.ini";
         HM::String version, error;
         const bool pinged = HM::ClamAVVirusScanner::Ping(HM::String(file.c_str()), 0, version, error);
         Report(!pinged && Contains(Narrow(error), "Unable to connect to ClamAV server at " + file + ": it is a file, not a socket."),
            "a file at the path", Narrow(error));
      }

      // A socket this user could write to, in a directory this user may not enter, as
      // Fedora's /run/clamd.scan (0710 clamscan:virusgroup) is to a user outside its
      // group. Root enters every directory, so the case says it was not run.
      if (::geteuid() == 0)
      {
         printf("skip  a directory this user may not enter: running as root, which no directory's mode refuses\n");
      }
      else
      {
         const std::string closed = directory + "/closed";
         const bool made = ::mkdir(closed.c_str(), 0700) == 0;
         {
            const std::string path = closed + "/clamd.sock";
            FakeClamd clamd(path, FakeClamd::Answers);
            const bool shut = made && clamd.Listening() && ::chmod(path.c_str(), 0666) == 0 && ::chmod(closed.c_str(), 0600) == 0;
            HM::String version, error;
            const bool pinged = shut && HM::ClamAVVirusScanner::Ping(HM::String(path.c_str()), 0, version, error);
            Report(shut && !pinged && Contains(Narrow(error), "this server's user may not enter " + closed + ", a directory on the way to the socket") &&
                   !Contains(Narrow(error), "LocalSocketMode 666"),
               "a directory this user may not enter", Narrow(error));

            // Opened again before the fake removes its socket.
            ::chmod(closed.c_str(), 0700);
         }
         ::rmdir(closed.c_str());
      }

      // A clamd whose queue of connections waiting to be accepted is full is waited
      // for until the deadline, as over TCP: a scan begun with the queue full and the
      // queue drained half a second later is answered, where it once failed at once
      // with "Unable to write INSTREAM command." on a socket connected to nothing.
      {
         const std::string path = directory + "/queue-drained.sock";
         FakeClamd clamd(path, FakeClamd::Answers, 1, true);
         bool full = false;
         std::vector<int> queued = clamd.Listening() ? FillQueue(path, full) : std::vector<int>();

         std::thread drain([&clamd, &queued]()
            {
               std::this_thread::sleep_for(std::chrono::milliseconds(500));
               CloseAll(queued);
               clamd.Release();
            });

         const Clock::time_point started = Clock::now();
         HM::VirusScanningResult scanned = HM::ClamAVVirusScanner::ScanData(HM::String(path.c_str()), 0, "Test");
         const double seconds = SecondsSince(started);
         drain.join();

         Report(full && !scanned.GetErrorOccured() && !scanned.GetVirusFound() && Narrow(scanned.GetDetails()) == "Result: stream: OK" &&
                seconds >= 0.4 && seconds < 2,
            "a queue full until half a second later",
            (full ? std::string() : "the queue was never full; ") + Narrow(scanned.GetDetails()) + " after " + std::to_string(seconds).substr(0, 4) + " s");
      }

      // And one that stays full is the deadline, naming the socket, not a write that
      // failed.
      {
         const std::string path = directory + "/queue-full.sock";
         FakeClamd clamd(path, FakeClamd::Answers, 1, true);
         bool full = false;
         std::vector<int> queued = clamd.Listening() ? FillQueue(path, full) : std::vector<int>();

         const Clock::time_point started = Clock::now();
         HM::VirusScanningResult scanned = HM::ClamAVVirusScanner::ScanData(HM::String(path.c_str()), 0, "Test");
         const double seconds = SecondsSince(started);
         CloseAll(queued);
         clamd.Release();

         Report(full && scanned.GetErrorOccured() &&
                Contains(Narrow(scanned.GetDetails()), "Unable to connect to ClamAV server at " + path + ": it did not accept the connection in the time allowed") &&
                seconds >= 1.5 && seconds < 10,
            "a queue that stays full",
            (full ? std::string() : "the queue was never full; ") + Narrow(scanned.GetDetails()) + " after " + std::to_string(seconds).substr(0, 4) + " s");
      }

      // A clamd that takes the stream and never answers is let go at the deadline.
      {
         const std::string path = directory + "/never-answers.sock";
         FakeClamd clamd(path, FakeClamd::NeverAnswers);
         const Clock::time_point started = Clock::now();
         HM::VirusScanningResult scanned = HM::ClamAVVirusScanner::ScanData(HM::String(path.c_str()), 0, "Test");
         const double seconds = SecondsSince(started);
         Report(scanned.GetErrorOccured() && Narrow(scanned.GetDetails()) == "Unable to read response (after streaming)." &&
                seconds >= 1.5 && seconds < 10,
            "a clamd that never answers", Narrow(scanned.GetDetails()) + " after " + std::to_string(seconds).substr(0, 4) + " s");
      }

      // A path longer than a socket address holds, refused before anything is tried.
      {
         const std::string path = "/" + std::string(sizeof(static_cast<sockaddr_un*>(nullptr)->sun_path), 'x');
         const HM::String refusal = HM::AntiVirusConfiguration::ClamAVHostRefusal(HM::String(path.c_str()));
         HM::String version, error;
         const bool pinged = HM::ClamAVVirusScanner::Ping(HM::String(path.c_str()), 0, version, error);
         Report(!pinged && Narrow(error) == Narrow(refusal) && Contains(Narrow(refusal), "can have at most 107") &&
                HM::AntiVirusConfiguration::ClamAVHostRefusal(HM::String(answers.c_str())).IsEmpty(),
            "a path too long for a socket address", Narrow(refusal));
      }

      // A host name is not a path, and a path is not refused here.
      Report(!HM::AntiVirusConfiguration::IsClamAVSocketPath(_T("localhost")) &&
             !HM::AntiVirusConfiguration::IsClamAVSocketPath(_T("")) &&
             !HM::AntiVirusConfiguration::IsClamAVSocketPath(_T("clamav/clamd.ctl")) &&
             HM::AntiVirusConfiguration::IsClamAVSocketPath(_T("/var/run/clamav/clamd.ctl")) &&
             HM::AntiVirusConfiguration::ClamAVHostRefusal(_T("localhost")).IsEmpty() &&
             HM::AntiVirusConfiguration::ClamAVHostRefusal(_T("/var/run/clamav/clamd.ctl")).IsEmpty(),
         "what is a socket path", "localhost and a relative path are hosts; /var/run/clamav/clamd.ctl is a path, and taken");
   }

   void TheRealClamd(const std::string &path)
   {
      HM::VirusScannerTester tester;
      HM::String message;
      const bool passed = tester.TestClamAVConnect(HM::String(path.c_str()), 0, message);
      Report(passed && Contains(Narrow(message), "ClamAV "), "the connection test against the clamd at " + path, Narrow(message));
   }
}

int
main(int argc, char *argv[])
{
   try
   {
      // The fake writes to clients that may have gone, as the service does.
      struct sigaction ignore = {};
      ignore.sa_handler = SIG_IGN;
      sigaction(SIGPIPE, &ignore, nullptr);

      const boost::filesystem::path scratch = ScratchDirectory();
      if (scratch.empty())
      {
         printf("FAIL  the clamd socket: no scratch directory could be made\n");
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
             << "ClamMinTimeout=2\n"
             << "ClamMaxTimeout=2\n";
      }
      HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
      HM::IniFileSettings::Instance()->LoadSettings();

      TheFakeCases(directory);

      if (argc > 1)
         TheRealClamd(argv[1]);

      boost::system::error_code error;
      boost::filesystem::remove_all(scratch, error);
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the clamd socket: %s\n", failure.what());
      return 1;
   }

   return failures == 0 ? 0 : 1;
}
