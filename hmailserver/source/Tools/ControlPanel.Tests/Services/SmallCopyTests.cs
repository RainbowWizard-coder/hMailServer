// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Three small copy fixes of the Control Panel review (T3.1 item 3), each held so
   /// that it does not come back: a chart card's title says its unit once (F035),
   /// no page's purpose - which the Ctrl+K palette shows as a row - tells its reader
   /// to press Ctrl+K (F039), and a field's caption ends in no colon (F043).
   /// </summary>
   public class SmallCopyTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheSessionsCard_SaysActiveSessions_AndTheThroughputCardItsUnit()
      {
         Loc.Reset();
         Assert.Equal("Active sessions", ChartCatalog.DashboardSessions.CardTitle);
         Assert.Equal("Delivery throughput (messages / minute)", ChartCatalog.DashboardThroughput.CardTitle);

         // The table's headers and the spoken words keep the unit.
         Assert.Contains("SMTP (sessions)", ChartCatalog.DashboardSessions.TableHeaders);
      }

      [Theory]
      [InlineData("Active sessions", "sessions", false, "Active sessions")]
      [InlineData("Aktive Sitzungen", "Sitzungen", false, "Aktive Sitzungen")]
      [InlineData("アクティブなセッション", "セッション", false, "アクティブなセッション")]
      [InlineData("Aktiiviset istunnot", "istuntoa", true, "Aktiiviset istunnot")]
      [InlineData("Aktiiviset istunnot", "istuntoa", false, "Aktiiviset istunnot (istuntoa)")]
      [InlineData("Delivery throughput", "messages / minute", false, "Delivery throughput (messages / minute)")]
      [InlineData("Queue length", "", false, "Queue length")]
      public void ACardsTitle_NamesItsUnitOnce(string title, string unit, bool titleNamesUnit, string expected)
      {
         var chart = new ChartDefinition("test.chart", title, "d", unit, "N0", "A") { TitleNamesUnit = titleNamesUnit };
         Assert.Equal(expected, chart.CardTitle);
      }

      [Fact]
      public void TheChartCard_ShowsTheCardTitle()
      {
         string card = ReadControlPanelFile(Path.Join("Views", "AccessibleChartCard.cs"));
         Assert.Contains("titleText_.Text = definition_.CardTitle;", card, StringComparison.Ordinal);
         Assert.DoesNotContain("definition_.Title + \" (\" + definition_.Unit + \")\"", card, StringComparison.Ordinal);
      }

      /// <summary>
      /// A page's purpose is the palette's row for the page and the sidebar's tool
      /// tip: Welcome's said "press Ctrl+K and describe what you want to do" to a
      /// reader who was already in the palette.
      /// </summary>
      [Fact]
      public void NoPagesPurpose_TellsItsReaderToPressCtrlK()
      {
         List<string> found = NavigationMap.Pages.Where(p => p.Purpose.Contains("Ctrl+K", StringComparison.OrdinalIgnoreCase))
            .Select(p => p.Key + ": " + p.Purpose).ToList();
         Assert.True(found.Count == 0, string.Join(Environment.NewLine, found));

         NavNode welcome = NavigationMap.Pages.Single(p => p.Key == "welcome");
         Assert.Equal("Start here: jump straight to a common task, or describe what you want to do in the search.", welcome.Purpose);
      }

      /// <summary>
      /// A FieldRow's caption names its field and ends in no colon: "Domain:" above
      /// the DNS records page's domain list and "Folder name:" in the new public
      /// folder's prompt were the two that did, where Server, User name, Password,
      /// Address and the Domains page's Domain do not.
      /// </summary>
      [Fact]
      public void NoFieldRowCaption_EndsInAColon()
      {
         string root = Path.GetDirectoryName(Path.GetDirectoryName(ControlPanelPath(Path.Join("Views", "ConnectView.xaml"))));
         var code = new Regex(@"\bLabel\s*=\s*L\(\s*""((?:[^""\\]|\\.)*)""\s*\)");
         var xaml = new Regex(@"\bLabel=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");
         var found = new List<string>();
         foreach (string path in Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
                     .Where(p => p.EndsWith(".cs", StringComparison.Ordinal) || p.EndsWith(".xaml", StringComparison.Ordinal)))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.Split(Path.DirectorySeparatorChar)[0] is "bin" or "obj")
               continue;

            string text = File.ReadAllText(path);
            foreach (Match m in (path.EndsWith(".xaml", StringComparison.Ordinal) ? xaml : code).Matches(text))
            {
               if (m.Groups[1].Value.TrimEnd().EndsWith(':'))
                  found.Add(relative + ": " + m.Groups[1].Value);
            }
         }

         Assert.True(found.Count == 0, "Captions that end in a colon:" + Environment.NewLine + string.Join(Environment.NewLine, found));

         string folders = ReadControlPanelFile(Path.Join("Views", "PublicFoldersView.cs"));
         Assert.Contains("InputDialog.Prompt(Window.GetWindow(this), L(\"New public folder\"), L(\"_Folder name\"));", folders, StringComparison.Ordinal);
         string dns = ReadControlPanelFile(Path.Join("Views", "DnsRecordsView.cs"));
         Assert.Contains("new FieldRow { Label = L(\"_Domain\"), Content = domainCombo_ }", dns, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelPath(relative));

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
