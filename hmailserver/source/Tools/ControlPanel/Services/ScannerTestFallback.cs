// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What a scanner test on the Anti-spam and Anti-virus settings pages says when it
   /// failed and the server gave no detail (review finding F049): where it asked and
   /// what to check, where it said only "Connection failed." for SpamAssassin and
   /// ClamAV and "Scanner test failed." for ClamWin. The server's own words, where it
   /// gives them, are shown as they are; these are for the rest. Without the window,
   /// so that ScannerTestFallbackTests can hold them.
   /// </summary>
   public static class ScannerTestFallback
   {
      /// <summary>SpamAssassin's test: spamd at the host and port typed, the address in brackets where it is IPv6.</summary>
      public static string SpamAssassin(string host, int port)
         => F("SpamAssassin did not answer at {0}. Check that spamd is running, listens on that port and allows this server (--allowed-ips).",
            ListenerProbe.Endpoint(host, port));

      /// <summary>ClamAV's test: clamd at the host and port typed, which it answers only over TCP.</summary>
      public static string ClamAv(string host, int port)
         => F("ClamAV did not answer at {0}. Check that clamd is running and listens on TCP (TCPSocket in clamd.conf).",
            ListenerProbe.Endpoint(host, port));

      /// <summary>ClamWin's test: the scanner the server runs from the path typed, with the signatures in the folder typed.</summary>
      public static string ClamWin()
         => L("The scanner gave no result. Check the clamscan.exe path and the signature database folder.");
   }
}
