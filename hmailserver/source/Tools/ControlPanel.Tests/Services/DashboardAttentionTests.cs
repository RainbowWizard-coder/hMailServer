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
         int steps = render.IndexOf("AttentionLine_(steps[index], index)", StringComparison.Ordinal);
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
