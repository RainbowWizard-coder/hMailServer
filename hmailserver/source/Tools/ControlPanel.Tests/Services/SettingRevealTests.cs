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
   /// A setting found by search opens its tab and its field (review finding F274).
   /// The index carried only the label, the key and the page, so "MaxMind country
   /// database" opened Auto-ban on its Auto-ban tab, three tabs from the setting,
   /// with nothing marked, and its row said only "Sign-in &amp; access &gt; Auto-ban".
   /// The index now records each setting's tab and card, the row says
   /// "Sign-in &amp; access &gt; Auto-ban &gt; Countries", and the page it opens is
   /// asked to show the setting: the tab chosen, the row in view, the keyboard in
   /// the editor and a ring round the row once.
   /// </summary>
   [Collection("Loc")]
   public class SettingRevealTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      private static readonly string[] ServerPages =
         { "protocols", "delivery", "antispam", "antivirus", "tls", "logging", "performance", "advanced", "adminaccess", "autoban", "webmail" };

      private static readonly string[] FeaturePages =
         { "security", "acme", "api", "hardening", "authentication", "dns", "webservices", "updates" };

      [Theory]
      [InlineData("GeoIpDatabaseFile", "autoban", "Countries", "Country blocking")]
      [InlineData("CompromiseDetectionMode", "autoban", "Compromised accounts", "Compromised accounts")]
      [InlineData("JsonLogging", "logging", "Logging", "Log categories")]
      [InlineData("MirrorEMailAddress", "advanced", "Copies of mail", "Mirroring")]
      [InlineData("PstImportDirectory", "advanced", "Migrations", "Migrations")]
      [InlineData("DlpAccountNumberPattern", "delivery", "Rules", "Sensitive content in rules")]
      [InlineData("AntiSpam.SpamAssassinHost", "antispam", "SpamAssassin", "SpamAssassin")]
      [InlineData("UpdateWindow", "updates", null, "How this server updates")]
      [InlineData("SMTPProxyProtocolEnabled", "hardening", null, "Front-end proxies (PROXY protocol and XCLIENT)")]
      [InlineData("DkimOversignHeaders", "security", null, "DKIM oversigning")]
      public void TheIndexRecordsWhereOnItsPageASettingIs(string key, string page, string tab, string card)
      {
         SettingEntry entry = SettingsSearchIndex.Entries.Single(e => e.Key == key);
         Assert.Equal(page, entry.Page);
         Assert.Equal(tab, entry.Tab);
         Assert.Equal(card, entry.Card);
      }

      /// <summary>
      /// Every setting on a page of tabs has its tab and its card, and every one on a
      /// page of one scroll its card; and each is a tab header or a card title the
      /// page's source writes, so the index cannot name a tab the page does not have.
      /// </summary>
      [Fact]
      public void EverySettingOfASettingsPageIsPlaced()
      {
         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         var tabs = new HashSet<string>(Regex.Matches(server, "Tab\\(L\\(\"((?:[^\"\\\\]|\\\\.)*)\"\\)\\)").Select(m => Unescape_(m.Groups[1].Value)), StringComparer.Ordinal);
         var cards = new HashSet<string>(Regex.Matches(server, "Card\\(L\\(\"((?:[^\"\\\\]|\\\\.)*)\"\\)").Select(m => Unescape_(m.Groups[1].Value)), StringComparer.Ordinal);
         cards.UnionWith(Regex.Matches(feature, "Title = L\\(\"((?:[^\"\\\\]|\\\\.)*)\"\\)").Select(m => Unescape_(m.Groups[1].Value)));

         var unplaced = new List<string>();
         foreach (SettingEntry entry in SettingsSearchIndex.Entries)
         {
            if (ServerPages.Contains(entry.Page))
            {
               if (entry.Tab == null || !tabs.Contains(entry.Tab) || entry.Card == null || !cards.Contains(entry.Card))
                  unplaced.Add(entry.Page + " " + entry.Key + " (" + entry.Tab + " / " + entry.Card + ")");
            }
            else if (FeaturePages.Contains(entry.Page))
            {
               if (entry.Tab != null || entry.Card == null || !cards.Contains(entry.Card))
                  unplaced.Add(entry.Page + " " + entry.Key + " (" + entry.Tab + " / " + entry.Card + ")");
            }
         }

         Assert.True(unplaced.Count == 0, "Settings the index does not place on their page: " + string.Join(", ", unplaced));
      }

      /// <summary>The row says where the setting is, down to its tab, or its card on a page of one scroll.</summary>
      [Fact]
      public void TheRowSaysTheTab()
      {
         PaletteRow row = PaletteSearch.Query("MaxMind country database", null, null).First(r => r.Kind == PaletteRowKind.Setting);
         Assert.Equal("autoban", row.Page);
         Assert.Equal("GeoIpDatabaseFile", row.Setting);
         Assert.Equal("Sign-in & access > Auto-ban > Countries", row.Location);

         PaletteRow scroll = PaletteSearch.Query("Unattended window", null, null).First(r => r.Kind == PaletteRowKind.Setting);
         Assert.Equal("updates", scroll.Page);
         Assert.Equal("Maintenance > Updates > How this server updates", scroll.Location);

         // Only a setting row carries a setting.
         Assert.All(PaletteSearch.Query("logging", null, null).Where(r => r.Kind != PaletteRowKind.Setting), r => Assert.Null(r.Setting));
      }

      /// <summary>An entry that knows neither its tab nor its card says only the page's trail.</summary>
      [Fact]
      public void AnEntryWithNoPlaceSaysThePage()
      {
         var entry = new SettingEntry("Label", "Key", "backup");
         Assert.Null(entry.Tab);
         Assert.Null(entry.Within);
         Assert.Null(new SettingEntry("Label", "Key", "backup", "", "").Card);
      }

      /// <summary>
      /// The palette hands the setting to the shell, the shell asks the page it has
      /// just opened to show it, and both settings views can.
      /// </summary>
      [Fact]
      public void ThePageOpensAtTheSetting()
      {
         string palette = ReadControlPanelFile(Path.Join("Views", "NavigationPalette.cs"));
         Assert.Contains("SelectedSetting = SelectedPage != null && row.Kind == PaletteRowKind.Setting ? row.Setting : null;", palette, StringComparison.Ordinal);

         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         Assert.Contains("RevealSetting_(palette.SelectedPage, palette.SelectedSetting, palette.SelectedSettingLabel);", shell, StringComparison.Ordinal);
         Assert.Contains("reveal.RevealSetting(setting, label);", shell, StringComparison.Ordinal);
         Assert.Contains("ContentHost?.Content is not IRevealSetting reveal", shell, StringComparison.Ordinal);
         Assert.Contains("!string.Equals(currentPage_, page, StringComparison.OrdinalIgnoreCase)", shell, StringComparison.Ordinal);

         Assert.Contains("public partial class ServerSettingsView : IRevealSetting", ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.Reveal.cs")), StringComparison.Ordinal);
         Assert.Contains("SettingsTabs.SelectedItem = item;", ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.Reveal.cs")), StringComparison.Ordinal);
         Assert.Contains("public partial class FeatureSettingsView : IRevealSetting", ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.Reveal.cs")), StringComparison.Ordinal);

         string reveal = ReadControlPanelFile(Path.Join("Views", "SettingReveal.cs"));
         Assert.Contains("row.BringIntoView();", reveal, StringComparison.Ordinal);
         Assert.Contains("Keyboard.Focus(target);", reveal, StringComparison.Ordinal);
         Assert.Contains("AdornerLayer.GetAdornerLayer(target)", reveal, StringComparison.Ordinal);
         Assert.Contains("SystemParameters.ClientAreaAnimation", reveal, StringComparison.Ordinal);
         Assert.Contains("SystemParameters.HighContrast", reveal, StringComparison.Ordinal);
      }

      /// <summary>
      /// Auto-ban's four CountryPolicy rows - SMTP, IMAP, POP3 and the web - share one key
      /// and so one automation id, and the page took the first row of a key: choosing
      /// IMAP, POP3 or the web in the palette rang and focused the SMTP row. Each palette
      /// row hands the page its own label as well, and the page shows the row of that
      /// label, found by its caption where the key is shared.
      /// </summary>
      [Fact]
      public void ARowThatSharesItsKeyIsShownByItsLabel()
      {
         List<PaletteRow> rows = PaletteSearch.Query("CountryPolicy", null, null)
            .Where(row => row.Kind == PaletteRowKind.Setting && row.Setting == "CountryPolicy").ToList();
         Assert.Equal(new[] { "IMAP connections", "POP3 connections", "SMTP connections", "Webmail, REST API and web services connections" },
            rows.Select(row => row.SettingLabel).OrderBy(label => label, StringComparer.Ordinal).ToArray());
         Assert.All(rows, row => Assert.Equal("autoban", row.Page));
         Assert.All(PaletteSearch.Query("logging", null, null).Where(r => r.Kind != PaletteRowKind.Setting), r => Assert.Null(r.SettingLabel));

         string palette = ReadControlPanelFile(Path.Join("Views", "NavigationPalette.cs"));
         Assert.Contains("SelectedSettingLabel = SelectedSetting != null ? row.SettingLabel : null;", palette, StringComparison.Ordinal);

         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.Reveal.cs"));
         Assert.Contains("public bool RevealSetting(string key, string label)", server, StringComparison.Ordinal);
         Assert.Contains("SettingReveal.SameLabel(row.Setting.Label, label == null ? null : L(label))", server, StringComparison.Ordinal);
         Assert.Contains("SettingReveal.Find(SettingsTabs, null, setting.Label)", server, StringComparison.Ordinal);

         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.Reveal.cs"));
         Assert.Contains("public bool RevealSetting(string key, string label)", feature, StringComparison.Ordinal);
         Assert.Contains("SettingReveal.SameLabel(candidate.Label, label == null ? null : L(label))", feature, StringComparison.Ordinal);
         Assert.Contains("SettingReveal.Find(CardsPanel, null, setting.Label)", feature, StringComparison.Ordinal);
      }

      /// <summary>
      /// A key that more than one row of a page shares tells its rows apart by their
      /// labels alone, so every such row's label is its own on that page.
      /// </summary>
      [Fact]
      public void RowsThatShareAKeyHaveLabelsOfTheirOwn()
      {
         foreach (IGrouping<string, SettingEntry> shared in SettingsSearchIndex.Entries.GroupBy(e => e.Page + " " + e.Key).Where(g => g.Count() > 1))
         {
            List<string> labels = shared.Select(e => e.Label).ToList();
            Assert.True(labels.Distinct(StringComparer.Ordinal).Count() == labels.Count,
               shared.Key + " has rows of one label: " + string.Join(", ", labels));
         }

         Assert.Equal(4, SettingsSearchIndex.Entries.Count(e => e.Page == "autoban" && e.Key == "CountryPolicy"));
      }

      private static string Unescape_(string literal) => literal.Replace("\\\"", "\"").Replace("\\\\", "\\");

      private static string ReadControlPanelFile(string relativePath)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relativePath);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relativePath + " could not be found from the test binaries.");
         return null;
      }
   }
}
