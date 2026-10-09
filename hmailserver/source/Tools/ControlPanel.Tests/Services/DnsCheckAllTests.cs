// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F277, the DNS records page. Verifying a domain took a Check on
   /// each of seven cards - MX, SPF, DKIM, DMARC, the MTA-STS record and its policy,
   /// TLS-RPT - and a scroll past all of them to read the seven lines, with nothing
   /// that said how the domain stood; and Copy host and Copy value put the text on
   /// the clipboard best effort and said nothing either way. These hold the summary
   /// Check all records gives - the worst level any check reached, a line per
   /// verdict naming its records, worst first - and the page's wiring: every check is
   /// one Check all runs, and every copy says it copied or why it did not.
   /// </summary>
   public class DnsCheckAllTests
   {
      private static readonly DnsCheckSummary.Result[] Checked =
      {
         new("MX", StatusLevel.Good, "Published and correct"),
         new("SPF", StatusLevel.Information, "Published, differs from the suggestion"),
         new("DKIM", StatusLevel.Warning, "Not published"),
         new("DMARC", StatusLevel.Good, "Published and correct"),
         new("MTA-STS", StatusLevel.Good, "Published and correct"),
         new("MTA-STS policy", StatusLevel.Warning, "Not served"),
         new("TLS-RPT", StatusLevel.Warning, "Not published"),
      };

      [Fact]
      public void TheSummary_IsAtTheWorstLevelAnyCheckReached()
      {
         Assert.Equal(StatusLevel.Warning, DnsCheckSummary.Worst(Checked));
         Assert.Equal(StatusLevel.Critical, DnsCheckSummary.Worst(Checked.Append(new DnsCheckSummary.Result("DKIM", StatusLevel.Critical, "Published, but wrong key"))));
         Assert.Equal(StatusLevel.Good, DnsCheckSummary.Worst(Checked.Where(r => r.Level == StatusLevel.Good)));
         Assert.Equal(StatusLevel.Normal, DnsCheckSummary.Worst(Array.Empty<DnsCheckSummary.Result>()));
      }

      [Fact]
      public void TheSummary_HasALinePerVerdict_WorstFirst_NamingItsRecords()
      {
         Assert.Equal(new[]
         {
            "Not published: DKIM, TLS-RPT",
            "Not served: MTA-STS policy",
            "Published, differs from the suggestion: SPF",
            "Published and correct: MX, DMARC, MTA-STS",
         }, DnsCheckSummary.Lines(Checked));
      }

      [Fact]
      public void TheSummary_SaysWhatWasChecked_ThenItsLines()
      {
         string text = DnsCheckSummary.Text("example.com", Checked);
         string[] lines = text.Split('\n');
         Assert.Equal("The records of example.com, checked from this machine:", lines[0]);
         Assert.Equal(5, lines.Length);
      }

      /// <summary>Every card's check is one Check all records runs, the MTA-STS policy's fetch with them.</summary>
      [Fact]
      public void EveryRecordsCheck_IsOneCheckAllRecordsRuns()
      {
         string page = ReadPage();
         int wired = Regex.Matches(page, @"\.Click \+= async \(s, e\) => await (Check(?!All_)\w+|FetchMtaStsPolicy)\(").Count;
         int registered = Regex.Matches(page, @"checks_\.Add\(new RecordCheck\(L\(""[^""]+""\), \w+(\.Mark)?, \(\) => (Check\w+|FetchMtaStsPolicy)\(").Count;
         Assert.Equal(7, wired);
         Assert.Equal(wired, registered);

         string all = WithheldRowTests.MemberText(page, "private async Task CheckAll_()");
         Assert.True(all != null, "DnsRecordsView.CheckAll_ is gone.");
         Assert.Contains("await Task.WhenAll(checks.Select(check => check.Run()));", all, StringComparison.Ordinal);
         Assert.Contains("checkSummary_.Level = DnsCheckSummary.Worst(results);", all, StringComparison.Ordinal);
         Assert.Contains("checkSummary_.Text = DnsCheckSummary.Text(domain.Name, results);", all, StringComparison.Ordinal);

         string build = WithheldRowTests.MemberText(page, "private void BuildCards()");
         Assert.True(build != null, "DnsRecordsView.BuildCards is gone.");
         Assert.Contains("checks_.Clear();", build, StringComparison.Ordinal);
         Assert.Contains("checkSummary_.Visibility = Visibility.Collapsed;", build, StringComparison.Ordinal);
      }

      /// <summary>Every copy says it copied, on the button, or why it did not, in a toast.</summary>
      [Fact]
      public void EveryCopy_SaysItCopied_OrWhyItDidNot()
      {
         string page = ReadPage();
         Assert.DoesNotContain("CopyToClipboard(", page, StringComparison.Ordinal);
         Assert.Equal(3, Regex.Matches(page, @"CopyAndConfirm_\(\w+(\.\w+)?, ").Count);

         string copy = WithheldRowTests.MemberText(page, "private static void CopyAndConfirm_(Wpf.Ui.Controls.Button button, string text)");
         Assert.True(copy != null, "DnsRecordsView.CopyAndConfirm_ is gone.");
         Assert.Contains("if (!ClipboardWriter.TrySetText(text, out string error))", copy, StringComparison.Ordinal);
         Assert.Contains("Toast.Failure(", copy, StringComparison.Ordinal);
         Assert.Contains("CopiedState.Show(button);", copy, StringComparison.Ordinal);

         string show = WithheldRowTests.MemberText(page, "public static void Show(Wpf.Ui.Controls.Button button)");
         Assert.True(show != null, "DnsRecordsView.CopiedState.Show is gone.");
         Assert.Contains("L(\"Copied\")", show, StringComparison.Ordinal);
         Assert.Contains("Interval = TimeSpan.FromSeconds(2)", show, StringComparison.Ordinal);
         Assert.Contains("button.Content = state.content_;", show, StringComparison.Ordinal);
         Assert.Contains("Speak_(button, copied);", show, StringComparison.Ordinal);
      }

      private static string ReadPage()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "DnsRecordsView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/DnsRecordsView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
