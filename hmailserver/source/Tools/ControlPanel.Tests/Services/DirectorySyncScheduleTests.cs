// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

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
   /// and a visit to the page after the save said nothing of it.
   /// </summary>
   public class DirectorySyncScheduleTests
   {
      private const string StartedAtNine = "2026-10-09 09:00:00";
      private const string StartedAtTen = "2026-10-09 10:00:00";

      [Fact]
      public void TheScheduleTheServerStartedWith_DoesNotWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, "60");
         Assert.False(record.Waits);

         Assert.False(record.Saved(StartedAtNine, "60"));
         Assert.False(record.Waits);
      }

      [Fact]
      public void AChangedSchedule_Waits_ThroughLaterSaves_UntilTheServerStartsAgain()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, "60");

         Assert.True(record.Saved(StartedAtNine, "30"));

         // The second save of the selection alone, the schedule left at 30: it was
         // "no service restart is needed".
         Assert.True(record.Saved(StartedAtNine, "30"));

         // The page entered again, the server not started since.
         record.Read(StartedAtNine, "30");
         Assert.True(record.Waits);

         // A restart or a reinitialise registered the schedule the file held.
         record.Read(StartedAtTen, "30");
         Assert.False(record.Waits);
         Assert.False(record.Saved(StartedAtTen, "30"));
      }

      [Fact]
      public void AScheduleChangedBackBeforeTheServerStarted_DoesNotWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, "60");

         Assert.True(record.Saved(StartedAtNine, "30"));
         Assert.False(record.Saved(StartedAtNine, " 60 "));
      }

      [Fact]
      public void AStartBetweenTheReadAndTheSave_RegisteredWhatTheFileHeldThen()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, "60");
         Assert.True(record.Saved(StartedAtNine, "30"));

         // The service restarted while the page was open: it took 30. A save that
         // keeps 30 waits for nothing, and one that moves it to 15 waits again.
         Assert.False(record.Saved(StartedAtTen, "30"));
         Assert.True(record.Saved(StartedAtTen, "15"));
      }

      [Fact]
      public void AServerThatCannotSayWhenItStarted_NeverClearsAWait()
      {
         var record = new DirectorySyncSchedule();
         record.Read(StartedAtNine, "60");
         Assert.True(record.Saved(StartedAtNine, "30"));

         // A status that could not be read is no start: the change still waits
         // rather than being called applied.
         Assert.True(record.Saved(null, "30"));
         record.Read("", "30");
         Assert.True(record.Waits);

         // A start reported again is measured as before.
         record.Read(StartedAtTen, "30");
         Assert.False(record.Waits);
      }

      [Fact]
      public void AStartFirstReportedAfterASave_IsTakenForTheRunAlreadyOnRecord()
      {
         var record = new DirectorySyncSchedule();
         record.Read(null, "60");
         Assert.True(record.Saved(null, "30"));

         // The first start the server reports cannot be told from the run that was
         // going when the page first read the file, so the wait is kept.
         Assert.True(record.Saved(StartedAtNine, "30"));

         // Only a start other than that one ends it.
         record.Read(StartedAtTen, "30");
         Assert.False(record.Waits);
      }

      [Fact]
      public void NothingWaits_BeforeTheFileWasRead()
      {
         Assert.False(new DirectorySyncSchedule().Waits);
      }
   }
}
