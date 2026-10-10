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
   /// The file pickers and the administration level's signpost (review finding
   /// F324): a picker was a 40-pixel "…" on the settings pages, on Backup and on SSL
   /// certificates and Browse… in the dialogs; the account's Administration level
   /// said nothing of the Administrator roles page, which refines it, and Domains
   /// did not link to it.
   /// </summary>
   public class FilePickerCopyTests
   {
      [Theory]
      [InlineData("ServerSettingsView.xaml.cs")]
      [InlineData("FeatureSettingsView.xaml.cs")]
      public void TheSettingsPagesPickers_SayBrowse_AndAreNamedByTheSettingTheyFill(string file)
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains("Content = L(\"Browse…\"),", page, StringComparison.Ordinal);
         Assert.Contains("string setting = LabelInName.Shown(AccessibleName ?? Label ?? L(\"this setting\"));", page, StringComparison.Ordinal);
         Assert.Contains("ControlHelp.NameLink(browse, L(\"Browse…\"), setting);", page, StringComparison.Ordinal);
         Assert.Contains("ControlHelp.Explain(browse, ", page, StringComparison.Ordinal);
         Assert.DoesNotContain("MinWidth = 40", page, StringComparison.Ordinal);
      }

      [Fact]
      public void BackupsAndCertificatesPickers_SayBrowse_AndAreNamedByTheFieldTheyFill()
      {
         string backup = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "BackupView.xaml"));
         Assert.Equal(2, backup.Split("Content=\"{loc:L 'Browse…'}\"").Length - 1);
         string backupCode = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "BackupView.xaml.cs"));
         Assert.Contains("ControlHelp.NameLink(BrowseDestination, L(\"Browse…\"), LabelInName.Shown(L(\"Destination _folder (on the server)\")));", backupCode, StringComparison.Ordinal);
         Assert.Contains("ControlHelp.NameLink(BrowseBackupFile, L(\"Browse…\"), LabelInName.Shown(L(\"Back_up archive (HMBackup*.7z on the server)\")));", backupCode, StringComparison.Ordinal);
         // The captions the names repeat are the rows' own.
         Assert.Contains("Label=\"{loc:L 'Destination _folder (on the server)'}\"", backup, StringComparison.Ordinal);
         Assert.Contains("Label=\"{loc:L 'Back_up archive (HMBackup*.7z on the server)'}\"", backup, StringComparison.Ordinal);

         string certs = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml"));
         Assert.Contains("x:Name=\"BrowseCertButton\" Grid.Column=\"1\" Content=\"{loc:L 'Browse…'}\"", certs, StringComparison.Ordinal);
         Assert.Contains("x:Name=\"BrowseKeyButton\" Grid.Column=\"1\" Content=\"{loc:L 'Browse…'}\"", certs, StringComparison.Ordinal);
         Assert.Contains("Label=\"{loc:L '_Certificate file (PEM)'}\"", certs, StringComparison.Ordinal);
         Assert.Contains("Label=\"{loc:L 'Private key _file (PEM)'}\"", certs, StringComparison.Ordinal);
         string certsCode = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml.cs"));
         Assert.Contains("ControlHelp.NameLink(BrowseCertButton, L(\"Browse…\"), LabelInName.Shown(L(\"_Certificate file (PEM)\")));", certsCode, StringComparison.Ordinal);
         Assert.Contains("ControlHelp.NameLink(BrowseKeyButton, L(\"Browse…\"), LabelInName.Shown(L(\"Private key _file (PEM)\")));", certsCode, StringComparison.Ordinal);
      }

      /// <summary>No button anywhere is captioned by an ellipsis alone.</summary>
      [Fact]
      public void NoButtonIsABareEllipsis()
      {
         var bare = new Regex(@"Content\s*=\s*""(?:…|\\u2026|\.\.\.)""");
         string views = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Views", "BackupView.xaml")));
         var found = Directory.EnumerateFiles(views, "*.*", SearchOption.AllDirectories)
            .Where(f => f.EndsWith(".cs", StringComparison.Ordinal) || f.EndsWith(".xaml", StringComparison.Ordinal))
            .Where(f => bare.IsMatch(File.ReadAllText(f)))
            .Select(Path.GetFileName)
            .ToList();
         Assert.True(found.Count == 0, "A button is captioned \"…\" alone: " + string.Join(", ", found));
      }

      /// <summary>
      /// A picker's name is its caption and then what it fills, in every language,
      /// since the one is the other's start (LabelInName.LinkName).
      /// </summary>
      [Fact]
      public void APickersName_StartsWithItsCaption()
      {
         Assert.Equal("Browse: Database folder", LabelInName.LinkName("Browse…", "Database folder"));
         Assert.True(LabelInName.StartsWithCaption(LabelInName.LinkName("Browse…", "Database folder"), "Browse…"));
      }

      /// <summary>
      /// The field a picker fills is named by the words its caption shows: a
      /// Japanese caption's key suffix, "(_F)", is not read out as "(F)".
      /// </summary>
      [Fact]
      public void ThePickersField_IsNamedByTheWordsItsCaptionShows()
      {
         Assert.Equal("宛先フォルダー (サーバー上)", LabelInName.Shown("宛先フォルダー (サーバー上)(_F)"));
         Assert.Equal("参照: 宛先フォルダー (サーバー上)", LabelInName.LinkName("参照…", LabelInName.Shown("宛先フォルダー (サーバー上)(_F)")));
         Assert.Equal("Durchsuchen: Zielordner (auf dem Server)", LabelInName.LinkName("Durchsuchen…", LabelInName.Shown("Ziel_ordner (auf dem Server)")));
      }

      [Fact]
      public void TheAdministrationLevel_PointsAtTheRolesPage_AndDomainsLinksToIt()
      {
         string account = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "AccountDialog.cs"));
         Assert.Contains("DialogFields.Field(L(\"Ad_ministration level\"), adminLevel_,", account, StringComparison.Ordinal);
         Assert.Contains("L(\"The Administrator roles page can give this account a narrower, named role instead.\")));", account, StringComparison.Ordinal);
         Assert.Contains("roles", NavigationMap.Find("domains").SeeAlso);
         Assert.Contains("roles", NavigationMap.Find("adminaccess").SeeAlso);
      }
   }
}
