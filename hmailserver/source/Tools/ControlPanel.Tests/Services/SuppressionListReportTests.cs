// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Suppression list page draws what Utilities.GetSuppressionList and
   /// Utilities.GetBounceCounts return, which BounceTracker::SuppressionsJson and
   /// CountsJson write. These pin that shape: a key renamed on one side fails here
   /// rather than drawing an empty column, and a document the page cannot read is an
   /// error it can show rather than an exception on the UI thread.
   /// </summary>
   public class SuppressionListReportTests
   {
      // Byte-for-byte the shape the server writes: flat keys, the items array last.
      private const string Suppressions =
         "{\"domain\":\"example.com\",\"processing_enabled\":true,\"action\":\"refuse\",\"total\":3,\"offset\":0,\"items\":[" +
         "{\"address\":\"gone@remote.test\",\"source\":\"bounce\",\"category\":\"hard\",\"status\":\"5.1.1\"," +
         "\"diagnostic\":\"550 5.1.1 <gone@remote.test>: User unknown\",\"remote_mta\":\"mx.remote.test\"," +
         "\"created\":\"2026-09-14 10:00:00\",\"expires\":\"\",\"expired\":false}," +
         "{\"address\":\"old@remote.test\",\"source\":\"manual\",\"category\":\"none\",\"status\":\"\"," +
         "\"diagnostic\":\"From the 2019 export\",\"remote_mta\":\"\",\"created\":\"2026-09-01 08:00:00\"," +
         "\"expires\":\"2026-09-10 08:00:00\",\"expired\":true}" +
         "]}";

      private const string Counts =
         "{\"domain\":\"example.com\",\"total\":1,\"offset\":0,\"items\":[" +
         "{\"address\":\"gone@remote.test\",\"hard\":2,\"soft\":1,\"mailbox_full\":0,\"policy\":3,\"other\":4," +
         "\"consecutive_hard\":2,\"last_status\":\"5.1.1\",\"last_category\":\"hard\",\"last_bounce\":\"2026-09-14 10:00:00\"," +
         "\"last_delivered\":\"\",\"suppressed\":true}" +
         "]}";

      [Fact]
      public void ParseSuppressions_ReadsTheHeaderAndEveryField()
      {
         SuppressionListReport report = SuppressionListReport.ParseSuppressions(Suppressions);

         Assert.Null(report.Error);
         Assert.Equal("example.com", report.Domain);
         Assert.True(report.ProcessingEnabled);
         Assert.Equal("refuse", report.Action);
         Assert.Equal(3, report.Total);
         Assert.Equal(2, report.Entries.Count);

         SuppressionEntryRow first = report.Entries.First();
         Assert.Equal("gone@remote.test", first.Address);
         Assert.Equal("bounce", first.Source);
         Assert.Equal("hard", first.Category);
         Assert.Equal("5.1.1", first.Status);
         Assert.Contains("User unknown", first.Diagnostic);
         Assert.Equal("mx.remote.test", first.RemoteMta);
         Assert.Equal("2026-09-14 10:00:00", first.Created);
         Assert.Equal("", first.Expires);
         Assert.False(first.Expired);

         SuppressionEntryRow second = report.Entries.Last();
         Assert.Equal("manual", second.Source);
         Assert.Equal("From the 2019 export", second.Diagnostic);
         Assert.True(second.Expired);
      }

      [Fact]
      public void ParseSuppressions_TotalCanExceedTheRowsReturned()
      {
         // The page says "showing the N most recent" on exactly this difference.
         SuppressionListReport report = SuppressionListReport.ParseSuppressions(Suppressions);

         Assert.True(report.Total > report.Entries.Count);
      }

      [Fact]
      public void ParseCounts_ReadsEveryCounter()
      {
         SuppressionListReport report = SuppressionListReport.ParseCounts(Counts);

         Assert.Null(report.Error);
         Assert.Single(report.Counts);

         BounceCountRow row = report.Counts[0];
         Assert.Equal("gone@remote.test", row.Address);
         Assert.Equal(2, row.Hard);
         Assert.Equal(1, row.Soft);
         Assert.Equal(0, row.MailboxFull);
         Assert.Equal(3, row.Policy);
         Assert.Equal(4, row.Other);
         Assert.Equal(2, row.ConsecutiveHard);
         Assert.Equal("5.1.1", row.LastStatus);
         Assert.Equal("hard", row.LastCategory);
         Assert.Equal("", row.LastDelivered);
         Assert.True(row.Suppressed);
      }

      [Fact]
      public void ParseSuppressions_WithProcessingOff_SaysSo()
      {
         SuppressionListReport report = SuppressionListReport.ParseSuppressions(
            "{\"domain\":\"example.com\",\"processing_enabled\":false,\"action\":\"observe\",\"total\":0,\"offset\":0,\"items\":[]}");

         Assert.Null(report.Error);
         Assert.False(report.ProcessingEnabled);
         Assert.Equal("observe", report.Action);
         Assert.Empty(report.Entries);
      }

      [Theory]
      [InlineData("")]
      [InlineData("   ")]
      [InlineData("not json at all")]
      [InlineData("[1,2,3]")]
      [InlineData("{\"items\":[")]
      public void MalformedInput_IsAnErrorAndNoRows_NeverAnException(string json)
      {
         SuppressionListReport entries = SuppressionListReport.ParseSuppressions(json);
         SuppressionListReport counts = SuppressionListReport.ParseCounts(json);

         Assert.NotNull(entries.Error);
         Assert.Empty(entries.Entries);
         Assert.NotNull(counts.Error);
         Assert.Empty(counts.Counts);
      }

      [Fact]
      public void AMemberOfTheWrongType_IsReadAsEmpty_AndTheRestStillRead()
      {
         // A number where the address belongs, and a string where a count belongs:
         // what a hand-edited or future document might hold. The row survives with
         // the fields that could be read.
         SuppressionListReport report = SuppressionListReport.ParseCounts(
            "{\"total\":1,\"items\":[{\"address\":42,\"hard\":\"two\",\"soft\":1,\"suppressed\":\"yes\"},7]}");

         Assert.Null(report.Error);
         Assert.Single(report.Counts);
         Assert.Equal("", report.Counts[0].Address);
         Assert.Equal(0, report.Counts[0].Hard);
         Assert.Equal(1, report.Counts[0].Soft);
         Assert.False(report.Counts[0].Suppressed);
      }
   }
}
