// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The recovery report is the only place the server publishes what its
   /// message-store consistency scan found, so the Diagnostics page can only
   /// stay truthful while these tests match what
   /// MessageStoreConsistencyTask::WriteReport_ writes.
   /// </summary>
   public class MessageStoreConsistencyReportTests
   {
      // Byte-for-byte the layout the server writes: CRLF line endings, four
      // comment lines, then tab-separated rows.
      private const string ServerFormat =
         "# hMailServer message-store consistency report\r\n" +
         "# Generated: 2026-08-10 21:14:03\r\n" +
         "# Missing files: 2\r\n" +
         "# Columns: messageid<TAB>account<TAB>expected-path\r\n" +
         "1042\tuser@example.com\tC:\\hMailServer\\Data\\example.com\\user\\{A1}.eml\r\n" +
         "1043\tuser@example.com\tC:\\hMailServer\\Data\\example.com\\user\\{A2}.eml\r\n";

      [Fact]
      public void Parse_ReadsHeaderAndRows()
      {
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(ServerFormat);

         Assert.Equal("2026-08-10 21:14:03", report.Generated);
         Assert.Equal(2, report.ReportedMissingCount);
         Assert.Equal(2, report.MissingCount);
         Assert.False(report.IsTruncated);

         MessageStoreConsistencyEntry first = report.Entries.First();
         Assert.Equal("1042", first.MessageId);
         Assert.Equal("user@example.com", first.Account);
         Assert.Equal(@"C:\hMailServer\Data\example.com\user\{A1}.eml", first.ExpectedPath);
      }

      [Fact]
      public void Parse_CommentLinesAreNeverTreatedAsMissingMessages()
      {
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(ServerFormat);

         Assert.Equal(2, report.Entries.Count);
         Assert.DoesNotContain(report.Entries, e => e.MessageId.StartsWith("#"));
      }

      [Fact]
      public void Parse_CleanScanHasNoEntries()
      {
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(
            "# hMailServer message-store consistency report\r\n" +
            "# Generated: 2026-08-10 21:14:03\r\n" +
            "# Missing files: 0\r\n" +
            "# Columns: messageid<TAB>account<TAB>expected-path\r\n");

         Assert.Empty(report.Entries);
         Assert.Equal(0, report.MissingCount);
         Assert.False(report.IsTruncated);
      }

      [Fact]
      public void Parse_FallsBackToRowCountWhenHeaderCountIsMissing()
      {
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(
            "1\ta@b.com\tC:\\store\\1.eml\r\n");

         Assert.Null(report.ReportedMissingCount);
         Assert.Equal(1, report.MissingCount);
         Assert.False(report.IsTruncated);
      }

      [Fact]
      public void Parse_FlagsAReportCaughtMidRewrite()
      {
         // The server rewrites the file in place, so a read can land between the
         // header and the rows. Say so rather than reporting a false "all clear".
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(
            "# Missing files: 5\r\n" +
            "1\ta@b.com\tC:\\store\\1.eml\r\n");

         Assert.True(report.IsTruncated);
         Assert.Equal(5, report.MissingCount);
         Assert.Single(report.Entries);
      }

      [Fact]
      public void Parse_HandlesAMessageWithNoMatchingAccount()
      {
         // The server left-joins hm_accounts, so an orphaned message row yields
         // an empty account field.
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(
            "# Missing files: 1\r\n" +
            "77\t\tC:\\store\\77.eml\r\n");

         MessageStoreConsistencyEntry entry = Assert.Single(report.Entries);
         Assert.Equal("77", entry.MessageId);
         Assert.Equal("", entry.Account);
         Assert.Equal(@"C:\store\77.eml", entry.ExpectedPath);
      }

      [Theory]
      [InlineData(null)]
      [InlineData("")]
      public void Parse_EmptyInputIsAnEmptyReport(string text)
      {
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(text);

         Assert.Empty(report.Entries);
         Assert.Empty(report.UnreferencedBlobs);
         Assert.Null(report.Generated);
         Assert.Null(report.ReportedMissingCount);
         Assert.Null(report.ReportedUnreferencedBlobCount);
         Assert.Equal(0, report.MissingCount);
      }

      // The message store's section (docs/MessageStore.md), byte for byte as
      // MessageStoreConsistencyTask::WriteReport_ appends it after the rows.
      private const string BlobSection =
         "# Blobs nothing references: 2\r\n" +
         "# Columns: blob<TAB>sha256\r\n" +
         "blob\t" + "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" + "\r\n" +
         "blob\t" + "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210" + "\r\n";

      [Fact]
      public void Parse_ABlobNothingReferencesIsNotAMissingMessage()
      {
         // A clean scan with two stray blobs: no problem found, and no false
         // "read while it was being rewritten" - the header says 0 missing and
         // no missing row is listed.
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(
            "# hMailServer message-store consistency report\r\n" +
            "# Generated: 2026-09-24 21:14:03\r\n" +
            "# Missing files: 0\r\n" +
            "# Columns: messageid<TAB>account<TAB>expected-path\r\n" +
            BlobSection);

         Assert.Empty(report.Entries);
         Assert.Equal(0, report.MissingCount);
         Assert.False(report.IsTruncated);
         Assert.Equal(2, report.ReportedUnreferencedBlobCount);
         Assert.Equal(new[]
         {
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
         }, report.UnreferencedBlobs);
      }

      [Fact]
      public void Parse_MissingMessagesAndStrayBlobsAreReadApart()
      {
         // Negative control of the one above: with missing files as well, the
         // rows before the blob section are the missing messages and nothing
         // after it is one.
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(ServerFormat + BlobSection);

         Assert.Equal(2, report.Entries.Count);
         Assert.Equal(2, report.MissingCount);
         Assert.False(report.IsTruncated);
         Assert.DoesNotContain(report.Entries, e => e.MessageId == "blob");
         Assert.Equal(2, report.UnreferencedBlobs.Count);
      }

      [Fact]
      public void Parse_AMissingBlobIsAMissingMessage()
      {
         // A stored message whose file is there but whose part held once is
         // not is written among the missing files, with the blob in its path
         // column - a missing message, which is what it is.
         MessageStoreConsistencyReport report = MessageStoreConsistencyReport.Parse(
            "# Missing files: 1\r\n" +
            "9\ta@b.com\tC:\\store\\9.eml (blob 0123 missing)\r\n" +
            "# Blobs nothing references: 0\r\n" +
            "# Columns: blob<TAB>sha256\r\n");

         MessageStoreConsistencyEntry entry = Assert.Single(report.Entries);
         Assert.Equal("9", entry.MessageId);
         Assert.Empty(report.UnreferencedBlobs);
         Assert.False(report.IsTruncated);
      }
   }
}
