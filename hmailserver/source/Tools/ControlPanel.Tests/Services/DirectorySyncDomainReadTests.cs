// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F141: when the server's domain list could not be read, the
   /// Directory synchronisation page's state card said so at the violet Information
   /// level and with no next step, which understated a failed read. It is said at
   /// the level of the cause - Critical for a lost link, as the shell's pill calls
   /// it, and a Warning, with the server's words last, for a server that answered
   /// and could not give the list (ComErrorText.LevelFor) - and says to press
   /// Reload.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class DirectorySyncDomainReadTests
   {
      [Fact]
      public void AFailedRead_RecordsItsCausesLevel_AndItsReason()
      {
         string load = WithheldRowTests.MemberText(DirectorySyncPageTests.Source(), "private void LoadDomains_()");
         Assert.True(load != null, "DirectorySyncView.LoadDomains_ is gone.");
         Assert.Contains("domainsReadLevel_ = ComErrorText.LevelFor(fatalCheck);", load, StringComparison.Ordinal);
         Assert.Contains("domainsReadWhy_ = ServerSession.DescribeComError(fatalCheck);", load, StringComparison.Ordinal);
      }

      [Fact]
      public void TheStateCard_SaysItAtThatLevel_WithPressReload()
      {
         string state = WithheldRowTests.MemberText(DirectorySyncPageTests.Source(), "private void RefreshState_()");
         Assert.True(state != null, "DirectorySyncView.RefreshState_ is gone.");

         int unread = state.IndexOf("if (!domainsRead_)", StringComparison.Ordinal);
         Assert.True(unread >= 0, "The state card no longer says that the domain list could not be read.");
         string branch = state.Substring(unread, state.IndexOf("return;", unread, StringComparison.Ordinal) - unread);

         Assert.Contains("State_(domainsReadLevel_, domainsReadLevel_ == StatusLevel.Critical", branch, StringComparison.Ordinal);
         Assert.DoesNotContain("StatusLevel.Information", branch, StringComparison.Ordinal);
         Assert.Contains("That is a connection problem rather than a configuration one: press Reload once the connection is back.", branch, StringComparison.Ordinal);
         Assert.Contains("Press Reload to read it again. Why: {0}\", domainsReadWhy_)", branch, StringComparison.Ordinal);
      }
   }
}
