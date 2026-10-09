// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Directory synchronisation page says Restart to apply while the schedule in
   /// hMailServer.INI is not the one the running server registered as it started
   /// (review findings F218 and F120). The page compared the schedule with the one it
   /// had last saved, so a second save after a schedule change, with no restart
   /// between, said "no service restart is needed" over a change that still waited,
   /// and a visit to the page after the save said nothing of it. And it compared the
   /// minutes as text and nothing else, where the server registers a run only while
   /// [LDAP] Enabled is on, and clamps the minutes: with directory access off a changed
   /// schedule waited for a restart that registers nothing, and switching directory
   /// access on after a start with it off waited for nothing.
   /// </summary>
   public class DirectorySyncScheduleTests
   {
      private const string StartedAtNine = "2026-10-09 09:00:00";
      private const string StartedAtTen = "2026-10-09 10:00:00";
      private const bool On = true;
      private const bool Off = false;

      [Fact]
      public void TheScheduleTheServerStartedWith_DoesNotWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, 60);
         Assert.False(record.Waits);

         Assert.False(record.Saved(StartedAtNine, On, 60));
         Assert.False(record.Waits);
      }

      [Fact]
      public void AChangedSchedule_Waits_ThroughLaterSaves_UntilTheServerStartsAgain()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, 60);

         Assert.True(record.Saved(StartedAtNine, On, 30));

         // The second save of the selection alone, the schedule left at 30: it was
         // "no service restart is needed".
         Assert.True(record.Saved(StartedAtNine, On, 30));

         // The page entered again, the server not started since.
         record.Read(StartedAtNine, On, 30);
         Assert.True(record.Waits);

         // A restart or a reinitialise registered the schedule the file held.
         record.Read(StartedAtTen, On, 30);
         Assert.False(record.Waits);
         Assert.False(record.Saved(StartedAtTen, On, 30));
      }

      [Fact]
      public void AScheduleChangedBackBeforeTheServerStarted_DoesNotWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, 60);

         Assert.True(record.Saved(StartedAtNine, On, 30));
         Assert.False(record.Saved(StartedAtNine, On, 60));
      }

      [Fact]
      public void AStartBetweenTheReadAndTheSave_RegisteredWhatTheFileHeldThen()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, 60);
         Assert.True(record.Saved(StartedAtNine, On, 30));

         // The service restarted while the page was open: it took 30. A save that
         // keeps 30 waits for nothing, and one that moves it to 20 waits again.
         Assert.False(record.Saved(StartedAtTen, On, 30));
         Assert.True(record.Saved(StartedAtTen, On, 20));
      }

      [Fact]
      public void AServerThatCannotSayWhenItStarted_NeverClearsAWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, 60);
         Assert.True(record.Saved(StartedAtNine, On, 30));

         // A status that could not be read is no start: the change still waits
         // rather than being called applied.
         Assert.True(record.Saved(null, On, 30));
         record.Read("", On, 30);
         Assert.True(record.Waits);

         // A start reported again is measured as before.
         record.Read(StartedAtTen, On, 30);
         Assert.False(record.Waits);
      }

      [Fact]
      public void AStartFirstReportedAfterASave_IsTakenForTheRunAlreadyOnRecord()
      {
         var record = new DirectorySyncSchedule();
         record.Read(null, On, 60);
         Assert.True(record.Saved(null, On, 30));

         // The first start the server reports cannot be told from the run that was
         // going when the page first read the file, so the wait is kept.
         Assert.True(record.Saved(StartedAtNine, On, 30));

         // Only a start other than that one ends it.
         record.Read(StartedAtTen, On, 30);
         Assert.False(record.Waits);
      }

      [Fact]
      public void NothingWaits_BeforeTheFileWasRead()
      {
         Assert.False(new DirectorySyncSchedule().Waits);
      }

      /// <summary>
      /// With directory access off the server registers nothing, and a task it did
      /// register skips every tick: a schedule changed then waits for nothing. It
      /// said Restart to apply, with a restart that registers no schedule at all.
      /// </summary>
      [Fact]
      public void AScheduleChangedWhileDirectoryAccessIsOff_DoesNotWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, Off, 60);

         Assert.False(record.Saved(StartedAtNine, Off, 30));
         Assert.False(record.Saved(StartedAtNine, Off, 0));
         record.Read(StartedAtNine, Off, 45);
         Assert.False(record.Waits);
      }

      /// <summary>
      /// The server started with directory access off and a schedule in the file, and
      /// the Directory authentication page switched it on: no synchronisation runs
      /// until the service restarts. That page said no restart was needed.
      /// </summary>
      [Fact]
      public void DirectoryAccessSwitchedOn_AfterAStartWithItOff_WaitsForTheSchedule()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, Off, 60);

         Assert.True(record.Saved(StartedAtNine, On, 60));
         record.Read(StartedAtNine, On, 60);
         Assert.True(record.Waits);

         // Switched off again before a restart: nothing runs, as the file says.
         Assert.False(record.Saved(StartedAtNine, Off, 60));

         // On again, and the service restarted with it on: the schedule runs.
         Assert.True(record.Saved(StartedAtNine, On, 60));
         record.Read(StartedAtTen, On, 60);
         Assert.False(record.Waits);
      }

      /// <summary>
      /// Switched on with no schedule in the file, nothing is registered either way,
      /// so nothing waits.
      /// </summary>
      [Fact]
      public void DirectoryAccessSwitchedOn_WithNoSchedule_DoesNotWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, Off, 0);
         Assert.False(record.Saved(StartedAtNine, On, 0));
      }

      /// <summary>
      /// A server that started with directory access on keeps its task while it is
      /// switched off, and the task skips every tick; switched on again it runs at the
      /// interval it registered. So the interval in the file waits only once directory
      /// access is on again, and only when it is not that one.
      /// </summary>
      [Fact]
      public void DirectoryAccessSwitchedOffAndOn_WaitsOnlyForAChangedInterval()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, 60);

         Assert.False(record.Saved(StartedAtNine, Off, 60));
         Assert.False(record.Saved(StartedAtNine, Off, 30));
         Assert.True(record.Saved(StartedAtNine, On, 30));
         Assert.False(record.Saved(StartedAtNine, On, 60));

         // Off with no schedule at all is still nothing running, as the file says.
         Assert.False(record.Saved(StartedAtNine, Off, 0));
         // On with none, while the server's task still fires every hour: waits.
         Assert.True(record.Saved(StartedAtNine, On, 0));
      }

      /// <summary>
      /// The minutes are compared as the server registers them: 1 to 14 run every 15,
      /// anything over a week weekly, and none is none however it is written.
      /// </summary>
      [Theory]
      [InlineData(5u, 15u)]
      [InlineData(14u, 15u)]
      [InlineData(10080u, 20000u)]
      [InlineData(20000u, 4294967295u)]
      [InlineData(0u, 0u)]
      public void TwoSchedulesTheServerRunsAlike_DoNotWait(uint started, uint saved)
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, On, started);
         Assert.False(record.Saved(StartedAtNine, On, saved));
      }

      [Theory]
      [InlineData(0u, 0)]
      [InlineData(1u, 15)]
      [InlineData(14u, 15)]
      [InlineData(15u, 15)]
      [InlineData(60u, 60)]
      [InlineData(10080u, 10080)]
      [InlineData(10081u, 10080)]
      [InlineData(4294967295u, 10080)]
      public void TheInterval_IsTheServers(uint minutes, int interval)
         => Assert.Equal(interval, DirectorySyncSchedule.Interval(minutes));

      /// <summary>
      /// The pages read [LDAP] SyncScheduleMinutes and Enabled with the server's own
      /// call, GetPrivateProfileInt, rather than parsing the text: an empty value and 0
      /// are both no schedule, 0x10 is 16 minutes and -1 a week, and a word is 0. Held
      /// here against the real API on a scratch file.
      /// </summary>
      [Fact]
      public void TheFileIsRead_AsTheServerReadsIt()
      {
         string file = Path.Join(Path.GetTempPath(), "hm-dirsync-" + System.Guid.NewGuid().ToString("N") + ".ini");
         try
         {
            File.WriteAllLines(file, new[]
            {
               "[LDAP]", "Empty=", "Zero=0", "Five=5", "Spaced= 60 ", "Hex=0x10", "Minus=-1", "Word=hourly", "Trailing=30min"
            });

            uint Read(string key) => ProfileApi.ReadInt("LDAP", key, 0, file);

            Assert.Equal(0, DirectorySyncSchedule.Interval(Read("Empty")));
            Assert.Equal(0, DirectorySyncSchedule.Interval(Read("Zero")));
            Assert.Equal(0, DirectorySyncSchedule.Interval(Read("Missing")));
            Assert.Equal(15, DirectorySyncSchedule.Interval(Read("Five")));
            Assert.Equal(60, DirectorySyncSchedule.Interval(Read("Spaced")));
            Assert.Equal(16, DirectorySyncSchedule.Interval(Read("Hex")));
            Assert.Equal(DirectorySyncSchedule.MostMinutes, DirectorySyncSchedule.Interval(Read("Minus")));
            Assert.Equal(0, DirectorySyncSchedule.Interval(Read("Word")));
            Assert.Equal(30, DirectorySyncSchedule.Interval(Read("Trailing")));
         }
         finally
         {
            File.Delete(file);
         }
      }
   }
}
