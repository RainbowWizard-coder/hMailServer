// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Server status counts as auto-bans the ranges the server counts as its bans - an
   /// expiring range named "Auto-ban: ..." (AutoBanList.IsBan, the test of the Auto-ban
   /// page's Banned now card, the IP ranges page's filter and the server's own
   /// AutoBanFirewall) - where it counted every range that expires, and said localhost
   /// was banned for any expiring range of 127.0.0.1 (release review finding CP-7).
   /// </summary>
   public class ConfigurationChecksTests
   {
      private static ConfigurationChecks.RangeFacts Range_(string name, string lower, string upper, bool expires)
         => new ConfigurationChecks.RangeFacts(name, lower, upper, expires, true, true, false, false);

      [Fact]
      public void AnAdministratorsOwnExpiringRange_IsNotCountedAsABan()
      {
         var ranges = new List<ConfigurationChecks.RangeFacts>
         {
            Range_("My computer", "127.0.0.1", "127.0.0.1", false),
            Range_("Internet", "0.0.0.0", "255.255.255.255", false),
            // A partner let in until Friday: expiring, and no ban of the server's.
            Range_("Partner until Friday", "198.51.100.0", "198.51.100.255", true),
            Range_("Auto-ban: hacker@example.com", "203.0.113.9", "203.0.113.9", true),
            Range_("Auto-ban: spam trap (2)", "203.0.113.10", "203.0.113.10", true),
            // Named as a ban and permanent: the server lifts no such range, and does not count it.
            Range_("Auto-ban: kept by hand", "203.0.113.11", "203.0.113.11", false)
         };

         ConfigurationChecks.Judge("mail.example.com", false, ranges, out int bans);

         Assert.Equal(2, bans);
      }

      [Fact]
      public void LocalhostIsBanned_OnlyByABanOfTheServers()
      {
         IReadOnlyList<ConfigurationWarning> warnings = ConfigurationChecks.Judge("mail.example.com", false,
            new[] { Range_("Maintenance window", "127.0.0.1", "127.0.0.1", true) }, out int bans);
         Assert.Equal(0, bans);
         Assert.DoesNotContain(warnings, warning => warning.Text.Contains("Localhost", System.StringComparison.Ordinal));

         warnings = ConfigurationChecks.Judge("mail.example.com", false,
            new[] { Range_("Auto-ban: administrator", "127.0.0.1", "127.0.0.1", true) }, out bans);
         Assert.Equal(1, bans);
         ConfigurationWarning banned = warnings.Single(warning => warning.Text.Contains("Localhost", System.StringComparison.Ordinal));
         Assert.Equal(StatusLevel.Warning, banned.Level);
         Assert.Equal("ipranges", banned.Page);
      }

      /// <summary>The other checks, as they were: an empty host name, a denied null sender and an open relay.</summary>
      [Fact]
      public void TheOtherChecks_AreAsTheyWere()
      {
         var relay = new ConfigurationChecks.RangeFacts("Internet", "0.0.0.0", "255.255.255.255", false, true, true, true, false);
         IReadOnlyList<ConfigurationWarning> warnings = ConfigurationChecks.Judge("", true, new[] { relay }, out int bans);

         Assert.Equal(0, bans);
         Assert.Equal(new[] { StatusLevel.Warning, StatusLevel.Warning, StatusLevel.Critical }, warnings.Select(warning => warning.Level));
         Assert.Equal(DashboardAttention.HostNameKey, warnings[0].Key);
         Assert.Contains("external-to-external", warnings[2].Text, System.StringComparison.Ordinal);
      }
   }
}
