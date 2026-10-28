// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // What a task or a session that a stop of the servers could not end reaches
   // after the stop. A stop gives up on a thread still in a wait it cannot
   // interrupt within ten seconds - a DNS lookup in the system's receive, a
   // scanner's answer - and the thread goes on when the wait ends, with the
   // servers' TCP/IP service let go of (Application::GetIOService is empty until
   // the next start), and, in a reinitialise, with no database manager until
   // OpenDatabase. The sessions on the service such a thread kept end when the
   // queue that holds it is freed, which can be in the next start, and are
   // still subscribed to the notification server until then. Three parts:
   //
   //   - a session on an I/O service that has stopped is told nothing of a
   //     change to its folder: in IDLE it claims no message's \Recent, and
   //     otherwise it keeps no change (IMAPNotificationClient::OnNotification);
   //   - with no TCP/IP service: a message's SpamAssassin test, a lesson for
   //     SpamAssassin, the test of the connection to SpamAssassin, the
   //     diagnostics' connection test, a fetch from an external account and a
   //     delivery to one server each return, saying why, and make no connection
   //     (on Windows, through ClassTester, the servers are stopped for it and
   //     started again; on Linux they have never started);
   //   - with no database manager, which only stopped-servers-tests has: a
   //     folder's messages refreshed, their \Recent removed and claimed, as an
   //     IMAP session that ends during a reinitialise does
   //     (IMAPConnection::CloseCurrentFolder), and such a session ending; and
   //     there too, with no delivery manager, a clear of the delivery queue,
   //     which COM can ask for with the servers stopped, reports that they are
   //     and says it is over (in a running server the regression suite's
   //     MainOperations holds it through COM, since the clear reports an error).
   //
   // Until 6.3.8 each of the second and third read through an empty pointer -
   // an access violation on Windows, a SIGSEGV that ended the process on Linux,
   // or, for the database manager, a lock of no mutex - and the first claimed
   // \Recent for a session that would never answer, and kept every change for
   // it; the clear read its queue's name through the empty pointer and never
   // said it was over, so that no clear did anything again. On an account id
   // and a folder id nothing has. A second or two for the stop and the start.
   // Throws std::logic_error naming everything that did not hold; the servers
   // are started again whatever happened.
   class StoppedServersTester
   {
   public:
      void Test();

      // The case running, for a program's fault handler to name: a string
      // literal, never freed. Empty between cases.
      static const char *GetCurrentCase();

   private:
      String TestASessionOnAStoppedIOService_();
      String TestWithoutTheIOService_();
      String TestWithoutTheDeliveryManager_();
      String TestWithoutTheDatabase_();
   };
}
