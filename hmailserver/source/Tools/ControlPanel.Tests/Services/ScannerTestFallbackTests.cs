// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F049: where the server gave no detail, the SpamAssassin and
   /// ClamAV tests said only "Connection failed." and the ClamWin test "Scanner test
   /// failed." - no reason and no next step. Each now says where it asked and what to
   /// check (ScannerTestFallback); the server's own words, where it gives them, are
   /// still shown as they are.
   /// </summary>
   public class ScannerTestFallbackTests
   {
      [Fact]
      public void SpamAssassin_SaysWhereItAsked_AndWhatToCheck()
      {
         Assert.Equal("SpamAssassin did not answer at mail.example.com:783. Check that spamd is running, listens on that port and allows this server (--allowed-ips).",
            ScannerTestFallback.SpamAssassin("mail.example.com", 783));
      }

      [Fact]
      public void ClamAv_SaysWhereItAsked_AndThatClamdMustListenOnTcp()
      {
         Assert.Equal("ClamAV did not answer at 127.0.0.1:3310. Check that clamd is running and listens on TCP (TCPSocket in clamd.conf).",
            ScannerTestFallback.ClamAv("127.0.0.1", 3310));
      }

      /// <summary>An IPv6 address is written in brackets, where address, colon and port would read ":::3310".</summary>
      [Fact]
      public void AnIPv6Host_IsWrittenInBrackets()
      {
         Assert.Contains(" at [::1]:3310. ", ScannerTestFallback.ClamAv("::1", 3310), StringComparison.Ordinal);
         Assert.Contains(" at [2001:db8::25]:783. ", ScannerTestFallback.SpamAssassin(" 2001:db8::25 ", 783), StringComparison.Ordinal);
      }

      [Fact]
      public void ClamWin_SaysWhatToCheck()
      {
         Assert.Equal("The scanner gave no result. Check the clamscan.exe path and the signature database folder.", ScannerTestFallback.ClamWin());
      }

      /// <summary>The three tests fall back on these, and the bare words are gone from the page.</summary>
      [Fact]
      public void TheSettingsPage_FallsBackOnThem()
      {
         string page = null;
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null && page == null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ServerSettingsView.xaml.cs");
            if (File.Exists(candidate))
               page = File.ReadAllText(candidate);
         }

         Assert.True(page != null, "ControlPanel/Views/ServerSettingsView.xaml.cs could not be found from the test binaries.");
         Assert.Contains("msg = ok ? L(\"Connection succeeded.\") : ScannerTestFallback.SpamAssassin(host, port);", page, StringComparison.Ordinal);
         Assert.Contains("msg = ok ? L(\"Connection succeeded.\") : ScannerTestFallback.ClamAv(host, port);", page, StringComparison.Ordinal);
         Assert.Contains("msg = ok ? L(\"Scanner test succeeded.\") : ScannerTestFallback.ClamWin();", page, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Connection failed.\")", page, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Scanner test failed.\")", page, StringComparison.Ordinal);
      }
   }
}
