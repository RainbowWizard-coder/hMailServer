// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The pages and cards that are not lists keep to the rule the list pages keep
   /// (ReadFailurePageTests): a read that failed is said, at the level its cause
   /// has, and never drawn as a fact.
   ///
   /// Scanned rather than rendered, as ReadFailurePageTests is: this assembly has
   /// no WPF. Each test fails against the tree before its fix.
   /// </summary>
   public class FailureStateTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      // ---- a lost link is one event at one level ---------------------------------

      /// <summary>
      /// The Dashboard says a failed read of its statistics in a notice under the
      /// header, through ReadFailure - Critical, with Try again - where it put
      /// "Connection to the server lost." in its grey subtitle.
      /// </summary>
      [Fact]
      public void TheDashboard_SaysAFailedRead_InANoticeUnderTheHeader()
      {
         XElement page = XDocument.Parse(ReadView("DashboardView.xaml")).Root;
         var children = page.Descendants(Scaffold + "PageHeader").Single().Parent.Elements().ToList();
         int header = children.FindIndex(e => e.Name == Scaffold + "PageHeader");
         Assert.True(header >= 0 && header + 1 < children.Count, "DashboardView.xaml has nothing under its header.");
         XElement next = children[header + 1];
         Assert.True(next.Name == Scaffold + "InlineNotice" && (string)next.Attribute(X + "Name") == "ReadNotice",
            "The element under the Dashboard's header is not the InlineNotice ReadNotice.");

         string source = ReadView("DashboardView.xaml.cs");
         string refresh = WithheldRowTests.MemberText(source, "private bool Refresh()");
         Assert.True(refresh != null, "DashboardView.xaml.cs no longer has private bool Refresh().");
         Assert.Contains("ReadFailure.Show(ReadNotice", refresh, StringComparison.Ordinal);
         Assert.Contains("ReadFailure.Clear(ReadNotice", refresh, StringComparison.Ordinal);
         Assert.DoesNotContain("Connection to the server lost.", source, StringComparison.Ordinal);

         // The timer runs only while the statistics can be read.
         string enter = WithheldRowTests.MemberText(source, "public void OnEnter()");
         Assert.Matches(new Regex(@"if \(Refresh\(\)\)\s*timer_\.Start\(\);"), enter);
      }

      /// <summary>
      /// A link lost while the Dashboard's setup summary ran is not counted into
      /// "things need attention": the card says it cannot tell. Values a server that
      /// answered could not give are a Warning notice, as the setup checklist says
      /// them, where they were a grey caption with no mark or word.
      /// </summary>
      [Fact]
      public void TheDashboardsSetupCard_CannotTellOverALostLink_AndWarnsOfUnreadValues()
      {
         string source = ReadView("DashboardView.xaml.cs");
         string begin = WithheldRowTests.MemberText(source, "private void BeginSetupCheck_()");
         Assert.Matches(new Regex(@"if \(checks\.LinkLost\)\s*\{\s*ShowSetupUncheckable_\("), begin);

         string render = WithheldRowTests.MemberText(source, "private void RenderAttention_(ExternalSetupChecks checks, int pendingDnsLookups)");
         Assert.Contains("AttentionReadNotice.Show(StatusLevel.Warning", render, StringComparison.Ordinal);
         int note = render.IndexOf("AttentionNote.Text = note;", StringComparison.Ordinal);
         Assert.True(note >= 0 && render.IndexOf("could not be read", StringComparison.Ordinal) > note,
            "The Dashboard's unread values are still in its grey caption.");

         XElement page = XDocument.Parse(ReadView("DashboardView.xaml")).Root;
         XElement notice = page.Descendants(Scaffold + "InlineNotice").SingleOrDefault(e => (string)e.Attribute(X + "Name") == "AttentionReadNotice");
         Assert.True(notice != null, "DashboardView.xaml has no InlineNotice AttentionReadNotice.");
      }

      /// <summary>
      /// The setup checklist and DNS records take the level of their read-failure
      /// notice from the cause, where both said a lost link as a Warning.
      /// </summary>
      [Theory]
      [InlineData("ExternalSetupView.cs", "readFailure_.Level = ComErrorText.LevelFor(checks_.LinkLost);")]
      [InlineData("DnsRecordsView.cs", "readFailure_.Level = ComErrorText.LevelFor(linkLost_);")]
      public void TheChecklistAndDnsRecords_SayALostLinkAtCritical(string file, string line)
      {
         Assert.Contains(line, ReadView(file), StringComparison.Ordinal);
      }

      // ---- the Dashboard's figures while they cannot be read ---------------------

      /// <summary>
      /// Every KPI starts as a dash, not a 0: "In queue 0" is a statement the page
      /// cannot make before it has asked, and the XAML's 0s were what the cards
      /// showed under a lost link.
      /// </summary>
      [Theory]
      [InlineData("KpiUptime")]
      [InlineData("KpiProcessed")]
      [InlineData("KpiQueue")]
      [InlineData("KpiSpam")]
      [InlineData("KpiViruses")]
      public void EveryKpi_StartsAsADash(string name)
      {
         XElement page = XDocument.Parse(ReadView("DashboardView.xaml")).Root;
         XElement value = page.Descendants().Single(e => (string)e.Attribute(X + "Name") == name);
         Assert.Equal("—", (string)value.Attribute("Text"));
      }

      /// <summary>
      /// A failed read of the statistics shows a dash in every card when nothing
      /// was ever read, and otherwise keeps the last figures, dimmed, named with the
      /// time they were read, under a subtitle that says when that was.
      /// </summary>
      [Fact]
      public void TheKpis_SayTheyWereNotRead_OrWhenTheyWere()
      {
         string source = ReadView("DashboardView.xaml.cs");
         string refresh = WithheldRowTests.MemberText(source, "private bool Refresh()");
         int catchAt = refresh.IndexOf("catch (Exception", StringComparison.Ordinal);
         Assert.True(catchAt > 0 && refresh.IndexOf("ShowKpisUnread_();", catchAt, StringComparison.Ordinal) > catchAt,
            "The Dashboard's failed read leaves the KPI cards as they were.");

         string unread = WithheldRowTests.MemberText(source, "private void ShowKpisUnread_()");
         Assert.True(unread != null, "DashboardView.xaml.cs no longer has ShowKpisUnread_.");
         foreach (string kpi in new[] { "KpiUptime", "KpiProcessed", "KpiQueue", "KpiSpam", "KpiViruses" })
            Assert.Contains("(" + kpi + ", L(", unread, StringComparison.Ordinal);
         Assert.Contains("if (lastRead_ == null)", unread, StringComparison.Ordinal);
         Assert.Contains("value.Text = NotRead;", unread, StringComparison.Ordinal);
         Assert.Contains("F(\"{0}, not available\", label)", unread, StringComparison.Ordinal);
         Assert.Contains("KpiQueueBadge.Visibility = Visibility.Hidden;", unread, StringComparison.Ordinal);
         Assert.Contains("\"TextFillColorSecondaryBrush\"", unread, StringComparison.Ordinal);
         Assert.Contains("Header.Subtitle = F(\"Figures from {0}, the last time the server could be read.\", time);", unread, StringComparison.Ordinal);
         Assert.Contains("private const string NotRead = \"—\";", source, StringComparison.Ordinal);
      }

      /// <summary>Every COM read the checks count as failed also records whether the link went.</summary>
      [Theory]
      [InlineData("Services/ExternalSetupChecks.cs")]
      [InlineData("Views/DnsRecordsView.cs")]
      public void EveryCountedComFailure_RecordsWhetherTheLinkWent(string file)
      {
         string source = ReadControlPanelFile(file);
         int counted = Regex.Matches(source, @"firstError_ \?\?= ServerSession\.DescribeComError\(ex\);").Count;
         int recorded = Regex.Matches(source, @"firstError_ \?\?= ServerSession\.DescribeComError\(ex\);\s*linkLost_ \|= ServerSession\.IsTransportFailure\(ex\);").Count;
         Assert.True(counted > 0, file + " counts no failed COM read any more.");
         Assert.Equal(counted, recorded);
      }

      // ---- helpers ---------------------------------------------------------------

      private static string ReadView(string file) => ReadControlPanelFile(Path.Join("Views", file));

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelPath(relative));

      /// <summary>A file under the ControlPanel project, found from the test binaries.</summary>
      private static string ControlPanelPath(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
