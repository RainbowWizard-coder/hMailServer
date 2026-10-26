// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "ClamAVVirusScanner.h"
#include "AntiVirusConfiguration.h"

#include "../TCPIP/SynchronousConnection.h"
#include "../Util/ByteBuffer.h"
#include "../Util/File.h"
#include "../Util/Unicode.h"

#include "../Application/TimeoutCalculator.h"

#include <boost/regex.hpp>
#include <functional>

#ifdef HM_PLATFORM_POSIX
#include <sys/stat.h>
#include <cerrno>
#include <string>
#endif

using namespace boost;


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Where clamd is, as the messages name it: the path of its socket, or the host
      // and the port.
      String Where_(const String &hostName, int port)
      {
         if (AntiVirusConfiguration::IsClamAVSocketPath(hostName))
            return hostName;

         return Formatter::Format("{0}:{1}", hostName, port);
      }

#ifdef HM_PLATFORM_POSIX
      // What is at path where it is reachable and not a socket - a directory, such as
      // the one clamd's socket is in, or a file, such as clamd.conf - and empty
      // otherwise. connect() follows a symbolic link, and so does this.
      String NotASocket_(const AnsiString &path)
      {
         struct stat status = {};
         if (::stat(path.c_str(), &status) != 0 || S_ISSOCK(status.st_mode))
            return String();

         if (S_ISDIR(status.st_mode))
            return "it is a directory, not a socket. Set ClamAVHost to the socket's own path, the one LocalSocket in clamd.conf names.";

         if (S_ISREG(status.st_mode))
            return "it is a file, not a socket. Set ClamAVHost to the socket's own path, the one LocalSocket in clamd.conf names.";

         return "it is not a socket. Set ClamAVHost to the socket's own path, the one LocalSocket in clamd.conf names.";
      }

      // The directory on the way to path that this server's user may not enter, found
      // as the kernel finds it: the first leading part of the path that cannot be
      // looked up for want of permission is in that directory. Empty where every part
      // can be looked up.
      AnsiString DirectoryNotEntered_(const AnsiString &path)
      {
         const std::string whole(path.c_str());
         size_t end = 0;
         while (end != std::string::npos)
         {
            end = whole.find('/', end + 1);
            const std::string part = whole.substr(0, end);

            struct stat status = {};
            if (::stat(part.c_str(), &status) == 0)
               continue;

            if (errno != EACCES)
               return AnsiString();

            const size_t slash = part.rfind('/');
            return AnsiString(slash == 0 ? std::string("/") : part.substr(0, slash));
         }

         return AnsiString();
      }

      // Why a local socket could not be reached, in the administrator's terms: no
      // socket at the path and a socket nobody accepts on are both nothing listening;
      // a path that is something else is not a socket; a directory on the way that
      // this server's user may not enter is a matter of that directory, and a socket
      // the user may not write to of the socket's mode or group, not of clamd; and a
      // queue that stays full is a clamd too busy to take the connection.
      String LocalFailure_(const boost::system::error_code &error, const AnsiString &path)
      {
         if (error == boost::system::errc::no_such_file_or_directory)
            return "nothing is listening there, as there is no socket at that path. Check that clamd is running and that LocalSocket in clamd.conf names this path.";

         if (error == boost::system::errc::connection_refused || error == boost::system::errc::permission_denied)
         {
            const String notASocket = NotASocket_(path);
            if (!notASocket.IsEmpty())
               return notASocket;
         }

         if (error == boost::system::errc::connection_refused)
            return "nothing is listening there: the socket is there, and no clamd accepts on it. Check that clamd is running.";

         if (error == boost::system::errc::permission_denied)
         {
            const AnsiString directory = DirectoryNotEntered_(path);
            if (!directory.IsEmpty())
               return Formatter::Format("this server's user may not enter {0}, a directory on the way to the socket, so the socket's own mode does not help. "
                                        "Put that user in a group that may enter {0}, such as the directory's own group (ls -ld {0} shows it).", String(directory));
         }

         if (error == boost::system::errc::permission_denied || error == boost::system::errc::operation_not_permitted)
            return "the socket is there, and this server's user may not connect to it. Give that user write permission on the socket "
                   "(LocalSocketMode 666 in clamd.conf, or LocalSocketGroup a group the user is in) and a way to it.";

         if (error == boost::asio::error::timed_out)
            return "it did not accept the connection in the time allowed: the queue of connections waiting for clamd stayed full. "
                   "clamd is busy (MaxThreads, MaxQueue and MaxConnectionQueueLength in clamd.conf) or reloading its signatures.";

         return String(AnsiString(error.message()));
      }
#endif

      // Connects to clamd at ClamAVHost: over TCP to hostName and port, or, where
      // hostName is the absolute path of a Unix socket, to that socket. error says why
      // when it fails; a path that cannot be used here is refused before anything is
      // tried.
      bool Connect_(SynchronousConnection &connection, const String &hostName, int port, String &error)
      {
         if (AntiVirusConfiguration::IsClamAVSocketPath(hostName))
         {
            const String refusal = AntiVirusConfiguration::ClamAVHostRefusal(hostName);
            if (!refusal.IsEmpty())
            {
               error = refusal;
               return false;
            }

#ifdef HM_PLATFORM_POSIX
            AnsiString path;
            Unicode::WideToMultiByte(hostName, path);
            if (connection.ConnectLocal(path))
               return true;

            error = Formatter::Format("Unable to connect to ClamAV server at {0}: {1}", hostName, LocalFailure_(connection.GetLastError(), path));
            return false;
#endif
         }

         if (connection.Connect(hostName, port))
            return true;

         error = Formatter::Format("Unable to connect to ClamAV server at {0}:{1}.", hostName, port);
         return false;
      }

      // The INSTREAM chunk prefix: the chunk's length as four octets in network order,
      // and a zero length ends the stream.
      bool WriteChunkLength_(SynchronousConnection &connection, uint32_t size)
      {
         const uint32_t networkOrder = htonl(size);

         ByteBuffer prefix;
         prefix.Add(reinterpret_cast<const BYTE*>(&networkOrder), sizeof(networkOrder));
         return connection.Write(prefix);
      }

      // One INSTREAM exchange: nINSTREAM, length-prefixed chunks from the source until
      // it yields an empty one, the terminator, and clamd's one-line verdict.
      VirusScanningResult StreamToClamd_(const String &hostName, int port,
                                         const std::function<std::shared_ptr<ByteBuffer>()> &nextChunk)
      {
         TimeoutCalculator calculator;

         LOG_DEBUG("Connecting to ClamAV virus scanner...");
         SynchronousConnection commandConnection(calculator.Calculate(IniFileSettings::Instance()->GetClamMinTimeout(), IniFileSettings::Instance()->GetClamMaxTimeout()));
         String connectError;
         if (!Connect_(commandConnection, hostName, port, connectError))
            return VirusScanningResult(_T("ClamAVVirusScanner::Scan"), connectError);

         if (!commandConnection.Write("nINSTREAM\n"))
            return VirusScanningResult("ClamAVVirusScanner::Scan", "Unable to write INSTREAM command.");

         const int maxIterations = 100000;
         for (int i = 0; i < maxIterations; i++)
         {
            std::shared_ptr<ByteBuffer> pBuf = nextChunk();

            if (!pBuf || pBuf->GetSize() == 0)
               break;

            if (!WriteChunkLength_(commandConnection, static_cast<uint32_t>(pBuf->GetSize())))
               return VirusScanningResult("ClamAVVirusScanner::Scan", "Unable to write packet size to stream port.");

            if (!commandConnection.Write(*pBuf))
               return VirusScanningResult("ClamAVVirusScanner::Scan", "Unable to write packet data to stream port.");
         }

         if (!WriteChunkLength_(commandConnection, 0))
            return VirusScanningResult("ClamAVVirusScanner::Scan", "Unable to write end of stream.");

         AnsiString readData;
         if (!commandConnection.ReadUntil("\n", readData))
            return VirusScanningResult("ClamAVVirusScanner::Scan", "Unable to read response (after streaming).");

         commandConnection.Close();

         readData.TrimRight("\n");

         // Parse the response and see if a virus was reported.
         try
         {
            const regex expression("^stream.*: (.*) FOUND$");
            cmatch what;
            if (regex_match(readData.c_str(), what, expression))
            {
               LOG_DEBUG("Virus detected: " + what[1]);
               return VirusScanningResult(VirusScanningResult::VirusFound, String(what[1]));
            }
            else
            {
               LOG_DEBUG("No virus detected: " + readData);
               return VirusScanningResult(VirusScanningResult::NoVirusFound, Formatter::Format("Result: {0}", readData));
            }
         }
         catch (std::runtime_error&) // regex_match will throw runtime_error if regexp is too complex.
         {
            return VirusScanningResult("ClamAVVirusScanner::Scan", "Unable to parse regular expression.");
         }
      }
   }

   ClamAVVirusScanner::ClamAVVirusScanner(void)
   {
   }

   ClamAVVirusScanner::~ClamAVVirusScanner(void)
   {

   }

   bool
   ClamAVVirusScanner::Ping(const String &hostName, int port, String &version, String &error)
   {
      version.Empty();
      error.Empty();

      TimeoutCalculator calculator;
      const int timeout = calculator.Calculate(IniFileSettings::Instance()->GetClamMinTimeout(), IniFileSettings::Instance()->GetClamMaxTimeout());

      {
         SynchronousConnection connection(timeout);
         if (!Connect_(connection, hostName, port, error))
            return false;

         AnsiString reply;
         if (!connection.Write("nPING\n") || !connection.ReadUntil("\n", reply))
         {
            error = Formatter::Format("clamd at {0} did not answer PING.", Where_(hostName, port));
            return false;
         }
         connection.Close();

         reply.TrimRight("\n");
         if (reply != "PONG")
         {
            error = Formatter::Format("clamd at {0} answered PING with '{1}' rather than PONG.", Where_(hostName, port), String(reply));
            return false;
         }
      }

      // The version is a courtesy: a daemon that answers PING but not VERSION is
      // still a daemon, so a failure here is reported in the text, not as a failure.
      SynchronousConnection connection(timeout);
      String versionError;
      if (Connect_(connection, hostName, port, versionError))
      {
         AnsiString reply;
         if (connection.Write("nVERSION\n") && connection.ReadUntil("\n", reply))
         {
            reply.TrimRight("\n");
            version = reply;
         }
         connection.Close();
      }

      return true;
   }

   VirusScanningResult
   ClamAVVirusScanner::Scan(const String &sFilename)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   //---------------------------------------------------------------------------()
   {
      AntiVirusConfiguration& config = Configuration::Instance()->GetAntiVirusConfiguration();

      String hostName = config.GetClamAVHost();
      int primaryPort = config.GetClamAVPort();

      return Scan(hostName, primaryPort, sFilename);
   }

   VirusScanningResult
   ClamAVVirusScanner::Scan(const String &hostName, int primaryPort, const String &sFilename)
   {
      File oFile;
      if (!oFile.Open(sFilename, File::OTReadOnly))
      {
         String sErrorMsg = Formatter::Format("Could not send file {0} via socket since it does not exist.", sFilename);
         return VirusScanningResult("ClamAVVirusScanner::Scan", sErrorMsg);
      }

      const int STREAM_BLOCK_SIZE = 4096;
      return StreamToClamd_(hostName, primaryPort, [&oFile, STREAM_BLOCK_SIZE]()
         {
            return oFile.ReadChunk(STREAM_BLOCK_SIZE);
         });
   }

   VirusScanningResult
   ClamAVVirusScanner::ScanData(const String &hostName, int port, const AnsiString &data)
   {
      bool sent = false;
      return StreamToClamd_(hostName, port, [&data, &sent]()
         {
            std::shared_ptr<ByteBuffer> chunk = std::shared_ptr<ByteBuffer>(new ByteBuffer());

            if (!sent && data.GetLength() > 0)
            {
               chunk->Add(reinterpret_cast<const BYTE*>(data.c_str()), static_cast<size_t>(data.GetLength()));
               sent = true;
            }

            return chunk;
         });
   }
}
