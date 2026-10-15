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
   /// Review finding F247: Server status worked out the uptime from the server's
   /// local start time and the Control Panel's own clock, wrong from another time
   /// zone, in units fixed as "d h m" in every language; its statistics gave counts
   /// since the start beside counts of now with nothing to say which; it named the
   /// counters other than the Dashboard does ("Processed messages", "Spam removed"
   /// against "Messages processed", "Spam blocked" - the counter is every message the
   /// spam checks detected, deleted or marked); and its Update row's caption was the
   /// verb, "Aktualisieren" in German. The uptime and the start are
   /// ServerUptimeText's; the pages' wiring is read from their source.
   /// </summary>
   public class ServerUptimeTextTests
   {
      private static readonly DateTime Now = new(2026, 10, 8, 14, 30, 0);

      [Fact]
      public void TheUptime_IsInCataloguedUnits_WhereTheClocksAreOne()
      {
         Assert.Equal("5 d 3 h 12 min", ServerUptimeText.Uptime("2026-10-03 11:18:00", Now, sameClock: true));
         Assert.Equal("2 h 5 min", ServerUptimeText.Uptime("2026-10-08 12:25:00", Now, sameClock: true));
         Assert.Equal("7 min", ServerUptimeText.Uptime("2026-10-08 14:23:00", Now, sameClock: true));
         Assert.Equal("5 d 3 h", ServerUptimeText.Uptime("2026-10-03 11:18:00", Now, sameClock: true, compact: true));

         // A start after now (the clocks a little apart) is not a negative uptime.
         Assert.Equal("0 min", ServerUptimeText.Uptime("2026-10-08 14:31:00", Now, sameClock: true));
      }

      [Fact]
      public void TheUptime_IsNotWorkedOutAcrossTwoComputersClocks()
      {
         Assert.Equal("Not known here: the server is on another computer", ServerUptimeText.Uptime("2026-10-03 11:18:00", Now, sameClock: false));
         Assert.Equal("-", ServerUptimeText.Uptime("2026-10-03 11:18:00", Now, sameClock: false, compact: true));
         Assert.Equal("-", ServerUptimeText.Uptime("", Now, sameClock: true));
         Assert.Null(ServerUptimeText.ParseStart("not a time"));
      }

      [Fact]
      public void TheStart_IsMarkedAsTheServersTime()
      {
         Assert.EndsWith(" (server time)", ServerUptimeText.Started("2026-10-03 11:18:00"), StringComparison.Ordinal);
         Assert.Equal("-", ServerUptimeText.Started(""));
      }

      /// <summary>
      /// The time the server gives (Status.StartTime, Application::start_time_) is the
      /// servers' last start, which Apply now, a reinitialise and a resume make again while
      /// the service runs on, and the counts under the statistics heading are the
      /// service's, kept from its process's start: after an Apply now a service three days
      /// up said "Uptime: 1 min" and "Since the service started at" a minute ago over
      /// 41,200 messages processed (release review finding CP-3). The rows name the time
      /// for what it is, a line under them says what starts the servers again, the
      /// heading names no time, and the Dashboard's tile is named as the row is.
      /// </summary>
      [Fact]
      public void TheServersStart_IsNamedForWhatItIs_AndTheHeadingOverTheServicesCountsNamesNoTime()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml.cs"));
         string xaml = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml"));
         string dashboard = ReadControlPanelFile(Path.Join("Views", "DashboardView.xaml"));
         string dashboardCode = ReadControlPanelFile(Path.Join("Views", "DashboardView.xaml.cs"));
         string uptime = ReadControlPanelFile(Path.Join("Services", "ServerUptimeText.cs"));

         Assert.Contains("SinceStartHeading.Text = L(\"Since the service started\");", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Since the service started at", code + uptime, StringComparison.Ordinal);
         Assert.Contains("SetValue_(StartedValue, L(\"Servers last started\"), ServerUptimeText.Started(snap.StartTime));", code, StringComparison.Ordinal);
         Assert.Contains("SetValue_(UptimeValue, L(\"Servers running for\"), ServerUptimeText.Uptime(", code, StringComparison.Ordinal);

         Assert.Contains("{loc:L 'Servers last started'}", xaml, StringComparison.Ordinal);
         Assert.Contains("{loc:L 'Servers running for'}", xaml, StringComparison.Ordinal);
         Assert.Contains("{loc:L 'Apply now, a reinitialise and a resume start the servers again while the service runs on. The statistics count from the start of the service itself.'}", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("{loc:L 'Uptime'}", xaml + dashboard, StringComparison.Ordinal);
         Assert.DoesNotContain("{loc:L 'Started'}", xaml, StringComparison.Ordinal);

         Assert.Contains("{loc:L 'Servers running for'}", dashboard, StringComparison.Ordinal);
         Assert.Contains("SetKpi_(KpiUptime, L(\"Servers running for\"), ", dashboardCode, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Uptime\")", code + dashboardCode, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePages_NameEachCounterOnce_AndSayWhichStatisticsAreSinceTheStart()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml"));
         string code = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml.cs"));
         string dashboard = ReadControlPanelFile(Path.Join("Views", "DashboardView.xaml"));
         string dashboardCode = ReadControlPanelFile(Path.Join("Views", "DashboardView.xaml.cs"));

         foreach (string old in new[] { "'Processed messages'", "'Spam removed'", "'Spam blocked'", "\"Processed messages\"", "\"Spam removed\"", "\"Spam blocked\"" })
         {
            Assert.DoesNotContain(old, xaml + code + dashboard + dashboardCode, StringComparison.Ordinal);
         }

         Assert.Contains("{loc:L 'Messages processed'}", xaml, StringComparison.Ordinal);
         Assert.Contains("{loc:L 'Spam detected'}", xaml, StringComparison.Ordinal);
         Assert.Contains("{loc:L 'Spam detected'}", dashboard, StringComparison.Ordinal);
         Assert.Contains("{loc:L 'Since the service started'}", xaml, StringComparison.Ordinal);
         Assert.Contains("{loc:L 'Connected now'}", xaml, StringComparison.Ordinal);
         Assert.True(xaml.IndexOf("'Since the service started'", StringComparison.Ordinal) < xaml.IndexOf("'Messages processed'", StringComparison.Ordinal)
            && xaml.IndexOf("'Connected now'", StringComparison.Ordinal) < xaml.IndexOf("'SMTP sessions'", StringComparison.Ordinal),
            "The statistics are not under the lines that say which are since the start and which are now.");

         // The update row has a caption of its own, not the verb - on the Updates page,
         // where the row went with the three steps (review finding F272).
         string updates = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml"));
         Assert.Contains("{loc:L 'Update status'}", updates, StringComparison.Ordinal);
         Assert.DoesNotContain("{loc:L 'Update'}", xaml + updates, StringComparison.Ordinal);

         // Both pages work the uptime out in one place, and neither from this computer's clock alone.
         Assert.DoesNotContain("FormatUptime", code + dashboardCode, StringComparison.Ordinal);
         Assert.Contains("ServerUptimeText.Uptime(snap.StartTime, DateTime.Now, ServerSession.IsLocalSession)", code, StringComparison.Ordinal);
         Assert.Contains("ServerUptimeText.Uptime(snap.StartTime, DateTime.Now, ServerSession.IsLocalSession, compact: true)", dashboardCode, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
