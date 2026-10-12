// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Dashboard's attention card is short, carries the configuration checks and
   /// leads on from its figures (review finding F242). It listed every open step of
   /// the setup checklist - eleven to fourteen rows, 330 to 400 pixels - so that the
   /// figures began at 577 to 650 pixels and both charts were below the fold at
   /// 1280 by 800; the Server status page's warnings - an open relay at Critical, no
   /// host name, localhost banned - never reached it; and the five figures led
   /// nowhere. Now: the configuration warnings, the most severe first, each with the
   /// page that puts it right; the checklist's next step and two more; one line for
   /// the rest, which opens the checklist; and each figure a button to its page.
   /// </summary>
   public class DashboardAttentionTests
   {
      [Theory]
      [InlineData(0, 0, 0)]
      [InlineData(1, 1, 0)]
      [InlineData(3, 3, 0)]
      [InlineData(4, 3, 1)]
      [InlineData(14, 3, 11)]
      public void TheCardListsTheNextStepAndTwoMore_AndLeavesTheRestToTheChecklist(int open, int shown, int left)
      {
         Assert.Equal(shown, DashboardAttention.StepsToShow(open));
         Assert.Equal(left, DashboardAttention.StepsLeftToTheChecklist(open));
         Assert.Equal(open, DashboardAttention.StepsToShow(open) + DashboardAttention.StepsLeftToTheChecklist(open));
      }

      [Fact]
      public void TheWarnings_AreMostSevereFirst_EachLevelInTheOrderFound()
      {
         var hostName = new ConfigurationWarning { Level = StatusLevel.Warning, Text = "host", Page = "protocols" };
         var bounces = new ConfigurationWarning { Level = StatusLevel.Warning, Text = "bounces", Page = "protocols" };
         var relay = new ConfigurationWarning { Level = StatusLevel.Critical, Text = "relay", Page = "ipranges" };
         var localhost = new ConfigurationWarning { Level = StatusLevel.Warning, Text = "localhost", Page = "ipranges" };

         var ordered = DashboardAttention.MostSevereFirst(new[] { hostName, bounces, relay, localhost });
         Assert.Equal(new[] { "relay", "host", "bounces", "localhost" }, ordered.Select(w => w.Text));
         Assert.Empty(DashboardAttention.MostSevereFirst(null));
      }

      [Fact]
      public void AnOpenRelay_MakesTheCardCritical_AndAWarningAtLeastAWarning()
      {
         var relay = new ConfigurationWarning { Level = StatusLevel.Critical, Text = "relay" };
         var host = new ConfigurationWarning { Level = StatusLevel.Warning, Text = "host" };

         Assert.Equal(StatusLevel.Critical, DashboardAttention.CardLevel(StatusLevel.Warning, new[] { host, relay }));
         Assert.Equal(StatusLevel.Warning, DashboardAttention.CardLevel(StatusLevel.Good, new[] { host }));
         Assert.Equal(StatusLevel.Good, DashboardAttention.CardLevel(StatusLevel.Good, Array.Empty<ConfigurationWarning>()));
         Assert.Equal(StatusLevel.Warning, DashboardAttention.CardLevel(StatusLevel.Warning, null));
      }

      /// <summary>
      /// No host name is a configuration warning and an open step of the setup
      /// checklist at once, on every new server: the card listed "No host name is
      /// set..." above step 1, "The host name - the name this server gives other
      /// servers", and counted both, 15 things where the checklist said 14. While the
      /// step is open the card lists the step alone and counts it once; once the step
      /// is done, or cannot be told, a warning of the same subject is listed.
      /// </summary>
      [Fact]
      public void AnEmptyHostName_AndItsOpenStep_AreOneLine_CountedOnce()
      {
         var hostName = new ConfigurationWarning { Level = StatusLevel.Warning, Text = "host", Page = "protocols", Key = DashboardAttention.HostNameKey };
         var bounces = new ConfigurationWarning { Level = StatusLevel.Warning, Text = "bounces", Page = "protocols" };

         // The host name's step alone: one line, counted once.
         string[] open = { DashboardAttention.HostNameKey };
         var listed = DashboardAttention.WithoutOpenSteps(new[] { hostName }, open);
         Assert.Empty(listed);
         Assert.Equal(1, listed.Count + DashboardAttention.StepsToShow(open.Length));
         Assert.Equal(1, listed.Count + open.Length);

         // Among other steps and warnings, the rest are kept in their order.
         string[] steps = { null, DashboardAttention.HostNameKey, null, null };
         Assert.Equal(new[] { "bounces" }, DashboardAttention.WithoutOpenSteps(new[] { hostName, bounces }, steps).Select(w => w.Text));
         Assert.Equal(new[] { "bounces", "host" }, DashboardAttention.WithoutOpenSteps(new[] { bounces, hostName }, new string[] { null }).Select(w => w.Text));

         // A step that is not open takes nothing away.
         Assert.Single(DashboardAttention.WithoutOpenSteps(new[] { hostName }, Array.Empty<string>()));
         Assert.Single(DashboardAttention.WithoutOpenSteps(new[] { hostName }, null));
         Assert.Empty(DashboardAttention.WithoutOpenSteps(null, open));
      }

      [Fact]
      public void TheHostNameWarning_AndTheHostNameStep_ShareTheirKey_AndTheCardListsAndCountsThemOnce()
      {
         string checks = Read("Services", "ConfigurationChecks.cs");
         int warning = checks.IndexOf("L(\"No host name is set:", StringComparison.Ordinal);
         Assert.True(warning > 0, "ConfigurationChecks no longer warns of an empty host name.");
         string line = checks.Substring(warning, checks.IndexOf('\n', warning) - warning);
         Assert.Contains("\"protocols\", DashboardAttention.HostNameKey);", line, StringComparison.Ordinal);

         string item = WithheldRowTests.MemberText(Read("Services", "ExternalSetupChecks.cs"), "private SetupItem HostNameItem()");
         Assert.True(item != null, "ExternalSetupChecks.HostNameItem is gone.");
         Assert.Contains("Key = DashboardAttention.HostNameKey", item, StringComparison.Ordinal);

         string render = WithheldRowTests.MemberText(Read("Views", "DashboardView.xaml.cs"), "private void RenderAttention_(ExternalSetupChecks checks, int pendingDnsLookups)");
         Assert.True(render != null, "DashboardView.RenderAttention_ is gone.");
         Assert.DoesNotContain("warnings = configurationWarnings_;", render, StringComparison.Ordinal);
         int filtered = render.IndexOf("IReadOnlyList<ConfigurationWarning> warnings = DashboardAttention.WithoutOpenSteps(configurationWarnings_, steps.Select(step => step.Key));", StringComparison.Ordinal);
         int counted = render.IndexOf("SummaryText_(action + warnings.Count,", StringComparison.Ordinal);
         int listed = render.IndexOf("foreach (ConfigurationWarning warning in warnings)", StringComparison.Ordinal);
         Assert.True(filtered > 0 && counted > filtered && listed > filtered,
            "The card does not list and count the warnings less those the checklist holds as an open step.");
      }

      [Fact]
      public void TheCard_ReadsTheStatusPagesChecks_ByTheCodeThatPageReadsThemWith()
      {
         string dashboard = Read("Views", "DashboardView.xaml.cs");
         Assert.Contains("ConfigurationChecks.Read((object)session.Application, out _)", dashboard, StringComparison.Ordinal);
         Assert.Contains("DashboardAttention.MostSevereFirst(", dashboard, StringComparison.Ordinal);

         string status = Read("Views", "StatusView.xaml.cs");
         Assert.Contains("ConfigurationChecks.Read((object)app, out int autoban)", status, StringComparison.Ordinal);

         string render = WithheldRowTests.MemberText(dashboard, "private void RenderAttention_(ExternalSetupChecks checks, int pendingDnsLookups)");
         Assert.True(render != null, "DashboardView.RenderAttention_ is gone.");
         int warnings = render.IndexOf("WarningLine_(warning", StringComparison.Ordinal);
         int steps = render.IndexOf("AttentionLine_(steps[index], index, isNext:", StringComparison.Ordinal);
         int more = render.IndexOf("MoreStepsLine_(more)", StringComparison.Ordinal);
         Assert.True(warnings > 0 && steps > warnings && more > steps,
            "The card does not list the warnings, then the steps it shows, then the line for the rest.");
         Assert.Contains("DashboardAttention.StepsToShow(steps.Count)", render, StringComparison.Ordinal);
         // Every open step was a line of the card.
         Assert.DoesNotContain("foreach (SetupItem item in checks.Items.Where(i => i.State == SetupItemState.ActionNeeded))", render, StringComparison.Ordinal);
      }

      [Fact]
      public void EachFigure_IsAButtonToThePageItIsReadOn()
      {
         XNamespace wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XDocument page = XDocument.Parse(Read("Views", "DashboardView.xaml"));

         var expected = new (string value, string page)[]
         {
            ("KpiUptime", "status"), ("KpiProcessed", "status"), ("KpiQueue", "queue"),
            ("KpiSpam", "spamoverview"), ("KpiViruses", "virusoverview")
         };
         foreach ((string value, string target) in expected)
         {
            XElement figure = page.Descendants().Single(e => (string)e.Attribute(x + "Name") == value);
            XElement button = figure.Ancestors(wpf + "Button").FirstOrDefault();
            Assert.True(button != null, value + " is not inside a button.");
            Assert.Equal(target, (string)button.Attribute("Tag"));
            Assert.Equal("KpiCard_Click", (string)button.Attribute("Click"));
            Assert.Equal("{StaticResource KpiCardButton}", (string)button.Attribute("Style"));
            Assert.NotNull(NavigationMap.Find(target));
         }

         // Named as the figure is, the label first.
         string code = Read("Views", "DashboardView.xaml.cs");
         Assert.Contains("AutomationProperties.SetName(card, AutomationProperties.GetName(value));", code, StringComparison.Ordinal);
      }

      private static string Read(params string[] parts)
      {
         string relative = Path.Join(parts);
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
