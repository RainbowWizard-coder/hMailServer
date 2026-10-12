// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The one way the Control Panel shows a date and time (review finding F321): the
   /// delivery queue, Server status and the quarantine showed the server's raw
   /// "2026-10-08 02:36:53", the audit trail Unix times in the same fixed form, the REST
   /// API keys "7 January 2027 10:00", and nothing said whose clock a time was read by.
   /// These hold the helper's forms, the pages' use of it, and that none of those pages
   /// writes a date in a form of its own again.
   /// </summary>
   public class ServerTimeTextTests : IDisposable
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private readonly CultureInfo culture_ = CultureInfo.CurrentCulture;

      public ServerTimeTextTests() => CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-GB");

      public void Dispose() => CultureInfo.CurrentCulture = culture_;

      [Theory]
      [InlineData("2026-10-08 02:36:53")]
      [InlineData(" 2026-10-08 02:36:53 ")]
      [InlineData("2026-10-08 02:36:53.000")]
      [InlineData("2026-10-08T02:36:53")]
      public void TheServersForms_AreRead(string text)
         => Assert.Equal(new DateTime(2026, 10, 8, 2, 36, 53), ServerTimeText.Parse(text));

      [Theory]
      [InlineData("1901-01-01 00:00:00")]
      [InlineData("1899-12-30 00:00:00")]
      public void TheServersNoDate_IsNoTime_AndIsToldApart(string text)
      {
         Assert.Null(ServerTimeText.Parse(text));
         Assert.True(ServerTimeText.IsNoDate(text));
      }

      [Theory]
      [InlineData("")]
      [InlineData(null)]
      [InlineData("yesterday")]
      [InlineData("08/10/2026 02:36")]
      public void AnythingElse_IsNeitherATimeNorTheNoDate(string text)
      {
         Assert.Null(ServerTimeText.Parse(text));
         Assert.False(ServerTimeText.IsNoDate(text));
      }

      [Fact]
      public void ATime_IsShownInTheReadersFormat_MarkedAsTheServers()
      {
         var when = new DateTime(2026, 10, 8, 2, 36, 53);
         Assert.Equal("08/10/2026 02:36", ServerTimeText.Cell(when));
         Assert.Equal("08/10/2026 02:36:53", ServerTimeText.CellWithSeconds(when));
         Assert.Equal("08/10/2026 02:36 (server time)", ServerTimeText.Marked(when));
         Assert.Equal("08/10/2026 02:36:53 (server time)", ServerTimeText.Full(when));
         Assert.Equal("08/10/2026 02:36", ServerTimeText.Cell("2026-10-08 02:36:53"));
         Assert.Equal("not a time", ServerTimeText.Cell(" not a time "));
         Assert.Equal("08/10/2026 02:36:53", ServerTimeText.CellWithSeconds("2026-10-08 02:36:53"));
         Assert.Equal("not a time", ServerTimeText.CellWithSeconds(" not a time "));
         Assert.Equal("08/10/2026", ServerTimeText.Date(when));

         CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
         Assert.Equal("08.10.2026 02:36", ServerTimeText.Cell(when));
         Assert.Equal("08.10.2026", ServerTimeText.Date(when));
         CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("ja-JP");
         Assert.Equal("2026/10/08", ServerTimeText.Date(when));
      }

      [Theory]
      [InlineData(0, "now")]
      [InlineData(30, "now")]
      [InlineData(-30, "now")]
      [InlineData(12 * 60, "in 12 min")]
      [InlineData(3 * 3600 + 60, "in 3 h")]
      [InlineData(3 * 86400, "in 3 d")]
      [InlineData(-12 * 60, "12 min ago")]
      [InlineData(-5 * 3600, "5 h ago")]
      [InlineData(-13 * 86400, "13 d ago")]
      public void WhereTheClocksAreOne_ATimeIsHowLongAgoOrHowSoon(int seconds, string expected)
      {
         var now = new DateTime(2026, 10, 8, 12, 0, 0);
         Assert.Equal(expected, ServerTimeText.Relative(now.AddSeconds(seconds), now));
      }

      [Fact]
      public void AGridCell_IsRelativeHere_TheDateElsewhere_AndSaysTheNoDate()
      {
         var now = new DateTime(2026, 10, 8, 2, 48, 53);
         Assert.Equal("12 min ago", ServerTimeText.GridCell("2026-10-08 02:36:53", now));
         Assert.Equal("08/10/2026 02:36", ServerTimeText.GridCell("2026-10-08 02:36:53", null));
         Assert.Equal("As soon as possible", ServerTimeText.GridCell("1901-01-01 00:00:00", now, "As soon as possible"));
         Assert.Equal("", ServerTimeText.GridCell("1901-01-01 00:00:00", null));
         Assert.Equal("whenever", ServerTimeText.GridCell(" whenever ", null));
         Assert.Equal("08/10/2026 02:36:53 (server time)", ServerTimeText.GridTip("2026-10-08 02:36:53"));
         Assert.Null(ServerTimeText.GridTip("1901-01-01 00:00:00"));
      }

      [Fact]
      public void AUnixTime_IsThisComputersLocalTime_AndNoneIsNull()
      {
         Assert.Null(ServerTimeText.FromUnix(0));
         Assert.Null(ServerTimeText.FromUnix(-5));
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1791462413).ToLocalTime().DateTime, ServerTimeText.FromUnix(1791462413));
      }

      [Fact]
      public void TheOtherHelpers_SayTheSameAsThisOne()
      {
         var when = new DateTime(2026, 10, 8, 2, 36, 53);
         Assert.Equal(ServerTimeText.Marked(when), LastSignInText.Format("2026-10-08 02:36:53"));
         Assert.Equal(ServerTimeText.Marked(when), ServerUptimeText.Started("2026-10-08 02:36:53"));
         Assert.Equal(ServerTimeText.Cell(when), DomainPolicyWords.Added("2026-10-08 02:36:53"));
         Assert.Equal(ServerTimeText.Cell(when), BackupRestoreReport.LocalDate("2026-10-08 02:36:53"));
      }

      [Fact]
      public void TheQueue_NamesTheServersClock_WhereItsCellsAreItsDates()
      {
         string code = Read("QueueView.xaml.cs");
         string reload = WithheldRowTests.MemberText(code, "private void Reload()");
         // The next try is t213c's State column, the wait to it in words and its time in the
         // cell's tool tip (QueueWords.StateTip); Created names the clock as it is written.
         Assert.Contains("DateTime? localNow = sameClock ? now : null;", reload, StringComparison.Ordinal);
         Assert.Contains("CreatedColumn.Header = localNow == null ? L(\"Created (server time)\") : L(\"Created\");", reload, StringComparison.Ordinal);
         Assert.Contains("Created = ServerTimeText.GridCell(columns[1], localNow),", reload, StringComparison.Ordinal);
         Assert.DoesNotContain("NextTryColumn", reload, StringComparison.Ordinal);

         XElement grid = XDocument.Parse(Read("QueueView.xaml")).Root.Descendants(Wpf + "DataGrid").Single();
         foreach ((string name, string sort, string tip) in new[] { ("CreatedColumn", "CreatedAt", "{Binding CreatedTip}") })
         {
            XElement column = grid.Descendants(Wpf + "DataGridTextColumn").Single(c => (string)c.Attribute(X + "Name") == name);
            Assert.Equal(sort, (string)column.Attribute("SortMemberPath"));
            Assert.Equal(tip, (string)column.Descendants(Wpf + "Setter").Single(s => (string)s.Attribute("Property") == "ToolTip").Attribute("Value"));
            Assert.NotEqual("150", (string)column.Attribute("Width"));
         }
      }

      [Fact]
      public void TheAuditTrailAndDiscovery_NameTheClockOfTheirUnixTimes_AndSortByThem()
      {
         string audit = Read("AuditTrailView.cs");
         Assert.Contains("=> ServerSession.IsLocalSession ? L(\"When (server time)\") : L(\"When (this computer's time)\");", audit, StringComparison.Ordinal);
         Assert.Contains("When = ServerTimeText.FromUnix(row.Time) is DateTime when ? ServerTimeText.CellWithSeconds(when) : \"\",", audit, StringComparison.Ordinal);
         Assert.Contains("whenColumn_.SortMemberPath = nameof(RowView.Time);", audit, StringComparison.Ordinal);

         string discovery = Read("DiscoveryView.cs");
         Assert.Contains("=> ServerSession.IsLocalSession ? L(\"Opened (server time)\") : L(\"Opened (this computer's time)\");", discovery, StringComparison.Ordinal);
         Assert.Contains("whenColumn_ = Text_(AuditTrailView.WhenHeader(), nameof(LineRow.When), DataGridLength.Auto);", discovery, StringComparison.Ordinal);
         Assert.Contains("openedColumn_.SortMemberPath = nameof(CaseRow.OpenedTime);", discovery, StringComparison.Ordinal);
      }

      /// <summary>
      /// The message trace's times and the suppression list's dates are the reader's, under
      /// headers that name the server's clock, and each column sorts by the time: the trace
      /// showed the server's "2026-10-10 20:26:26" under "Time", and the list its
      /// "2026-10-08 18:30:00" under Since, Expires, Last bounce and Last delivered.
      /// </summary>
      [Fact]
      public void TheMessageTraceAndTheSuppressionList_SayTheServersDatesInTheReadersForm()
      {
         string trace = Read("MessageTraceView.cs");
         Assert.Contains("public string Time => ServerTimeText.CellWithSeconds(OccurredTime);", trace, StringComparison.Ordinal);
         Assert.Contains("public DateTime? OccurredAt => ServerTimeText.Parse(OccurredTime);", trace, StringComparison.Ordinal);
         Assert.Contains("public string TimeTip => ServerTimeText.GridTip(OccurredTime);", trace, StringComparison.Ordinal);
         string columns = WithheldRowTests.MemberText(trace, "private void BuildColumns()");
         Assert.Contains("Header = L(\"When (server time)\"),", columns, StringComparison.Ordinal);
         Assert.Contains("Binding = new System.Windows.Data.Binding(nameof(TraceRow.Time)),", columns, StringComparison.Ordinal);
         Assert.Contains("SortMemberPath = nameof(TraceRow.OccurredAt),", columns, StringComparison.Ordinal);
         Assert.Contains("new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(TraceRow.TimeTip)))", columns, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Time\")", trace, StringComparison.Ordinal);

         string list = Read("SuppressionListView.cs");
         foreach (string line in new[]
         {
            "Since = ServerTimeText.Cell(row.Created),",
            "SinceAt = ServerTimeText.Parse(row.Created),",
            "Expires = row.Expires.Length == 0 ? L(\"Never\") : row.Expired ? F(\"{0} (expired)\", ServerTimeText.Cell(row.Expires)) : ServerTimeText.Cell(row.Expires),",
            "ExpiresAt = row.Expires.Length == 0 ? DateTime.MaxValue : ServerTimeText.Parse(row.Expires),",
            "LastBounce = ServerTimeText.Cell(row.LastBounce),",
            "LastBounceAt = ServerTimeText.Parse(row.LastBounce),",
            "LastDelivered = row.LastDelivered.Length == 0 ? L(\"Never\") : ServerTimeText.Cell(row.LastDelivered),",
            "LastDeliveredAt = ServerTimeText.Parse(row.LastDelivered),",
         })
            Assert.Contains(line, list, StringComparison.Ordinal);

         string built = WithheldRowTests.MemberText(list, "private void BuildColumns_()");
         foreach ((string header, string sort) in new[] { ("Since (server time)", "EntryView.SinceAt"), ("Expires (server time)", "EntryView.ExpiresAt") })
         {
            Match column = Regex.Match(built, @"^.*L\(""" + Regex.Escape(header) + @"""\).*$", RegexOptions.Multiline);
            Assert.True(column.Success, "The suppression list has no column headed " + header + ".");
            Assert.Contains("nameof(" + sort + ")", column.Value, StringComparison.Ordinal);
         }

         // The allow and block list's Expires, a date the server gives, is the reader's form
         // under a header that names the server's clock.
         string editor = WithheldRowTests.MemberText(HostedAddressTests.ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs")), "internal static string FormatCell(object v)");
         Assert.Contains("if (v is DateTime when) return when.Year < 1970 ? \"\" : ServerTimeText.Cell(when);", editor, StringComparison.Ordinal);
         Assert.Contains("Prop = \"ExpiresTime\", Label = L(\"Expires on (YYYY-MM-DD HH:MM:SS)\"), GridLabel = L(\"Expires (server time)\")",
            HostedAddressTests.ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs")), StringComparison.Ordinal);

         // The allowed and blocked domains' Added sorts by the server's own form, as the times do.
         string policy = WithheldRowTests.MemberText(Read("DomainPolicyListView.cs"), "private void BuildColumns_()");
         Assert.Contains("Binding = new System.Windows.Data.Binding(nameof(EntryView.Added)),", policy, StringComparison.Ordinal);
         Assert.Contains("SortMemberPath = nameof(EntryView.Created),", policy, StringComparison.Ordinal);
      }

      /// <summary>
      /// No page or helper that F321 found writing a date in a form of its own does so
      /// again, nor any that the unit's review found after it: every date there goes
      /// through ServerTimeText.
      /// </summary>
      [Theory]
      [InlineData("Views", "QueueView.xaml.cs")]
      [InlineData("Views", "StatusView.xaml.cs")]
      [InlineData("Views", "AuditTrailView.cs")]
      [InlineData("Views", "DiscoveryView.cs")]
      [InlineData("Views", "ApiKeysView.cs")]
      [InlineData("Views", "QuarantineView.cs")]
      [InlineData("Services", "QuarantineReview.cs")]
      [InlineData("Services", "LastSignInText.cs")]
      [InlineData("Services", "ServerUptimeText.cs")]
      [InlineData("Services", "DomainPolicyWords.cs")]
      [InlineData("Services", "BackupRestoreReport.cs")]
      [InlineData("Views", "MessageTraceView.cs")]
      [InlineData("Views", "SuppressionListView.cs")]
      [InlineData("Views", "ProfilesView.cs")]
      [InlineData("Services", "DkimRotationStatus.cs")]
      [InlineData("Views", "DomainDialog.cs")]
      [InlineData("Services", "CertificateInspector.cs")]
      [InlineData("Views", "FeatureSettingsView.xaml.cs")]
      [InlineData("Views", "ServerSettingsView.xaml.cs")]
      [InlineData("Views", "CollectionEditorView.cs")]
      public void NoneOfTheseWritesADateInAFormOfItsOwn(string folder, string file)
      {
         string source = HostedAddressTests.ReadControlPanelFile(Path.Join(folder, file));
         MatchCollection own = Regex.Matches(source, @"\.ToString\(\s*""(?:[^""]*(?:yyyy|MMM|HH|dd)[^""]*|[gGdDfFtT])""");
         Assert.True(own.Count == 0, file + " writes a date in a form of its own: " + string.Join(", ", own.Select(m => m.Value)));
         Assert.DoesNotContain("F(\"{0} (server time)\"", source, StringComparison.Ordinal);
      }

      /// <summary>
      /// The final check of Tier 2 found five more dates in the server's own form, against
      /// the owner's decision 13: the Updates page's "6.3.7 is available (published
      /// 2026-10-01)" and "checked 2026-10-12 08:00:00", Diagnostics' "Last scan:
      /// 2026-10-05 21:39:37.", the setup checklist's "applied on 2026-10-01", the update
      /// install's "recorded in the Sigstore log at 2026-10-10T09:12:00Z", and the
      /// Auto-ban never-ban names' Added cells under a header that named no clock.
      /// </summary>
      [Fact]
      public void TheUpdatesDiagnosticsChecklistInstallAndNeverBanDates_AreTheReaders()
      {
         Assert.Equal("6.3.7 is available (published 01/10/2026)", UpdateStatusText.For(2, "6.3.7", "2026-10-01", "", ""));
         Assert.Equal("This is the latest release, checked 12/10/2026 08:00 (server time)", UpdateStatusText.For(1, "", "", "2026-10-12 08:00:00", ""));
         // A text in another form is shown as it came.
         Assert.Equal("6.3.7 is available (published next week)", UpdateStatusText.For(2, "6.3.7", "next week", "", ""));

         Assert.Equal(new[] { "Small office", "01/10/2026" }, FirstHourDays.ReaderArguments("profile_applied", new[] { "Small office", "2026-10-01" }));
         Assert.Equal(new[] { "05/10/2026" }, FirstHourDays.ReaderArguments("profile_declined", new[] { "2026-10-05" }));
         Assert.Equal(new[] { "k1", "03/10/2026" }, FirstHourDays.ReaderArguments("stored_secrets_key_exported", new[] { "k1", "2026-10-03" }));
         Assert.Equal(new[] { "12", "2026-10-01" }, FirstHourDays.ReaderArguments("profile_in_service", new[] { "12", "2026-10-01" }));
         Assert.Empty(FirstHourDays.ReaderArguments("profile_applied", null));

         Assert.Equal("10/10/2026 09:12 UTC", ServerTimeText.Utc("2026-10-10T09:12:00Z"));
         Assert.Equal("10/10/2026 07:12 UTC", ServerTimeText.Utc("2026-10-10T09:12:00+02:00"));
         Assert.Equal("not a time", ServerTimeText.Utc(" not a time "));

         var row = new NeverBanNameRow { Pattern = ".example.net", Created = "2026-09-01 10:12:00" };
         Assert.Equal("01/09/2026 10:12", row.AddedCell);
         Assert.Equal(new DateTime(2026, 9, 1, 10, 12, 0), row.AddedAt);

         string diagnostics = HostedAddressTests.ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         Assert.Contains("F(\"Last scan: {0}.\", ServerTimeText.Parse(report.Generated) is DateTime scanned ? ServerTimeText.Marked(scanned) : report.Generated)", diagnostics, StringComparison.Ordinal);
         string install = HostedAddressTests.ReadControlPanelFile(Path.Join("Views", "UpdateInstallWindow.cs"));
         Assert.Contains("ServerTimeText.Utc(Value_(\"log-time\"))", install, StringComparison.Ordinal);
         string checks = HostedAddressTests.ReadControlPanelFile(Path.Join("Services", "ExternalSetupChecks.cs"));
         Assert.Contains("args = FirstHourDays.ReaderArguments(code, args);", checks, StringComparison.Ordinal);
         string names = HostedAddressTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.NeverBanNames.cs"));
         Assert.Contains("Column_(L(\"Added (server time)\"), nameof(NeverBanNameRow.AddedCell), DataGridLength.Auto);", names, StringComparison.Ordinal);
         Assert.Contains("added.SortMemberPath = nameof(NeverBanNameRow.AddedAt);", names, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Added\")", names, StringComparison.Ordinal);
      }

      private static string Read(string file) => HostedAddressTests.ReadControlPanelFile(Path.Join("Views", file));
   }
}
