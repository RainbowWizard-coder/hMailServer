// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The two halves of stopping a thread that is blocked in accept() on a
   // listening socket of its own - the shape of the metrics, ManageSieve and
   // ACME challenge listeners, which are plain blocking accept loops on a
   // std::thread rather than Boost.Asio acceptors.
   //
   // Winsock and POSIX disagree about what wakes such a thread. On Windows,
   // closesocket() on the listening socket makes the blocked accept() return
   // with an error, so closing it is how the loop has always been told to
   // stop. On Linux, close() does no such thing: the descriptor leaves the
   // table, but the thread inside accept() holds its own reference to the
   // socket and stays there until a connection arrives - which on a listener
   // nobody is scraping is never. Measured on 13 September 2026 on the Linux
   // bench: MetricsServer::Stop() sat in join() behind a worker parked in
   // __libc_accept for as long as the process was left, and every later
   // reinitialise queued on the application lock behind it. What does wake the
   // thread on Linux is shutdown(SHUT_RDWR) on the listening socket: accept()
   // returns EINVAL at once. The descriptor is then closed after the join and
   // never before it, because a descriptor closed under a thread that has not
   // yet re-entered accept() can be handed out again by the next socket() or
   // open() anywhere in the process, and the loop would wake up accepting on
   // somebody else's socket.
   //
   // So: Interrupt() before the join, Release() after it, on both platforms,
   // and each does what its platform needs.
   namespace AcceptLoop
   {
      // Makes a blocking accept() on the socket return, from another thread.
      // Leaves the descriptor open on POSIX; closes it on Windows, where the
      // close is the wake-up.
      inline void Interrupt(SOCKET &listenSocket)
      {
         if (listenSocket == INVALID_SOCKET)
            return;

#ifdef HM_PLATFORM_POSIX
         shutdown(listenSocket, SHUT_RDWR);
#else
         closesocket(listenSocket);
         listenSocket = INVALID_SOCKET;
#endif
      }

      // Gives the descriptor back, once the loop's thread has been joined.
      inline void Release(SOCKET &listenSocket)
      {
         if (listenSocket == INVALID_SOCKET)
            return;

         closesocket(listenSocket);
         listenSocket = INVALID_SOCKET;
      }

      // SO_REUSEADDR on the listening socket, before it is bound - on POSIX only.
      // The option means something different on the two platforms, as TCPServer
      // says where it sets it for the mail protocols. On POSIX it lets a listener
      // that is started again (a reinitialise) bind while the connections it served
      // before linger in TIME_WAIT, which is what every daemon wants and without
      // which the restart fails for a minute or two. On Windows it lets a socket
      // bind a port that another socket with the option is already listening on -
      // another program, or another listener of this server given the same port -
      // and the listener then reports itself started while the connections go to
      // the other (measured on Windows 11 on 30 September 2026: 20 connections of
      // 20). So there it is not set: without it that bind fails with WSAEADDRINUSE
      // and the listener says so. Windows does not hold a port in TIME_WAIT, or
      // while a connection the old listener accepted is still open, against a new
      // listener without the option (measured the same way), so a reinitialise
      // loses nothing by its absence.
      inline void AllowRebindPastTimeWait(SOCKET listenSocket)
      {
#ifdef HM_PLATFORM_POSIX
         const int reuseAddress = 1;
         setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));
#else
         (void) listenSocket;
#endif
      }

      // SO_EXCLUSIVEADDRUSE on a listening socket, before it is bound - on Windows
      // only. POSIX has no such option and needs none: there a second socket is
      // refused a port another listens on, SO_REUSEADDR or not.
      //
      // Without it Windows lets another socket of the same account bind a more
      // specific address on a port a listener holds on the wildcard - 127.0.0.1
      // under 0.0.0.0, ::1 under :: - and that socket then takes every connection
      // made to that address; and a listener that binds the wildcard after another
      // socket bound one address on the port starts, and reports itself listening,
      // while the connections to that address go to the other. With it the first
      // of those binds is refused (WSAEACCES) and so is the second (WSAEADDRINUSE),
      // and whichever listener comes second says so.
      //
      // What it does not do is keep a listener from being bound again on its own
      // address. Measured on Windows 11 (26200) on 1 October 2026 with a Winsock
      // probe on free ports above 49151, as one account: a listener with the option,
      // closed and bound again with it at once, bound whatever state the connections
      // it had accepted were left in - TIME_WAIT, still ESTABLISHED (a drain left
      // while sessions go on), FIN_WAIT_1 with an answer queued to a client that
      // stopped reading, FIN_WAIT_2 and CLOSE_WAIT - over the loopback and over a LAN
      // address, IPv4 and IPv6, and after the process that held it had ended (a
      // service restart). Microsoft's page on the option says such a port can stay
      // unavailable while a connection on it is active; on this Windows it did not.
      //
      // What it can refuse is a listener of this server's own that overlaps another
      // on one port: a wildcard and a specific address of one family, or a dual-stack
      // :: and 0.0.0.0 (measured the same way). So it is set only on a listener the
      // server never binds such a second one beside: TCPServer (the mail protocols,
      // whose ports an administrator can list twice on one number), HttpServer,
      // MetricsServer, ManageSieveServer and the SNMP agent say how each decides.
      // False when the option could not be set; the listener binds without it.
      inline bool ClaimPortExclusively(SOCKET listenSocket)
      {
#ifdef HM_PLATFORM_POSIX
         (void) listenSocket;
         return true;
#else
         const BOOL exclusive = TRUE;
         return setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == 0;
#endif
      }

      // Bounds every receive and every send on a blocking socket - a connection
      // one of these loops accepted - to the given number of milliseconds, the
      // TLS handshake on it included. Winsock takes SO_RCVTIMEO and SO_SNDTIMEO as a
      // DWORD of milliseconds and POSIX as a struct timeval. The DWORD these
      // listeners used to pass on both was refused on Linux with EINVAL, which left
      // the socket with no timeout at all: a client that connected and said nothing
      // held the single-threaded loop, and with it the listener, for as long as it
      // stayed connected. True when both options were set.
      inline bool SetTimeouts(SOCKET connectedSocket, unsigned int milliseconds)
      {
#ifdef HM_PLATFORM_POSIX
         struct timeval timeout = {};
         // Each part in its own type before it is multiplied, so nothing is
         // computed in unsigned int and widened afterwards.
         timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(milliseconds / 1000);
         timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(milliseconds % 1000) * 1000;
#else
         const DWORD timeout = milliseconds;
#endif
         const bool receiveSet = setsockopt(connectedSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
         const bool sendSet = setsockopt(connectedSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
         return receiveSet && sendSet;
      }
   }
}
