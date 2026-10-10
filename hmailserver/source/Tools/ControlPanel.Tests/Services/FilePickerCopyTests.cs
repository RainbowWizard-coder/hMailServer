// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
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

      /// <summary>
      /// The certificate's two pickers have the form's first line to themselves, each
      /// half of it, the box at least 120 pixels and Browse… beside it; under them the
      /// name, and the key's passphrase and Add, in the same halves. On one line of
      /// five, Browse… took its column's room: the boxes were some 48 pixels wide in
      /// English at 760 pixels and none in German, with Durchsuchen… cut at the right.
      /// </summary>
      [Fact]
      public void TheCertificatesPickers_HaveTheFormsFirstLine_EachHalfOfIt()
      {
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XElement page = XDocument.Parse(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml"))).Root;
         XElement Named(string name) => page.Descendants().Single(e => (string)e.Attribute(x + "Name") == name);
         static string Row(XElement e) => (string)e.Attribute("Grid.Row") ?? "0";
         static string Column(XElement e) => (string)e.Attribute("Grid.Column") ?? "0";
         static List<string> Widths(XElement grid)
            => grid.Elements().Single(e => e.Name.LocalName == "Grid.ColumnDefinitions").Elements().Select(c => (string)c.Attribute("Width")).ToList();

         XElement form = Named("AddCertForm");
         Assert.Equal(new[] { "*", "*" }, Widths(form));

         XElement cert = Named("NewCertFileRow"), key = Named("NewKeyFileRow");
         Assert.Equal(form, cert.Parent);
         Assert.Equal(form, key.Parent);
         Assert.Equal(("0", "0"), (Row(cert), Column(cert)));
         Assert.Equal(("0", "1"), (Row(key), Column(key)));
         Assert.Null(key.Attribute("Grid.ColumnSpan"));
         Assert.Equal((string)cert.Attribute("LabelGroup"), (string)key.Attribute("LabelGroup"));

         // The second line: the name in the first half, the passphrase and Add in the second.
         XElement name = Named("NewCertNameRow"), last = Named("AddCertLastLine");
         Assert.Equal(("1", "0"), (Row(name), Column(name)));
         Assert.Equal(("1", "1"), (Row(last), Column(last)));
         Assert.Equal(new[] { "*", "Auto" }, Widths(last));
         Assert.Equal(last, Named("NewKeyPassphraseRow").Parent);
         Assert.True(Named("AddCertButton").Ancestors().Contains(last), "Add is not on the passphrase's line.");
         Assert.Equal((string)name.Attribute("LabelGroup"), (string)Named("NewKeyPassphraseRow").Attribute("LabelGroup"));
         Assert.NotEqual((string)cert.Attribute("LabelGroup"), (string)name.Attribute("LabelGroup"));
         Assert.Equal(4, form.Elements().Count(e => !e.Name.LocalName.StartsWith("Grid.", StringComparison.Ordinal)));

         foreach ((string box, string browse) in new[] { ("NewCertFile", "BrowseCertButton"), ("NewKeyFile", "BrowseKeyButton") })
         {
            XElement picker = Named(box).Parent;
            Assert.Equal(picker, Named(browse).Parent);
            List<XElement> parts = picker.Elements().Single(e => e.Name.LocalName == "Grid.ColumnDefinitions").Elements().ToList();
            Assert.Equal("*", (string)parts[0].Attribute("Width"));
            Assert.True((double?)parts[0].Attribute("MinWidth") >= 120, box + " may shrink below 120 pixels.");
            Assert.Equal("Auto", (string)parts[1].Attribute("Width"));
            Assert.Equal("1", Column(Named(browse)));
         }

         // In the order the form is read and tabbed: the files, then the name.
         List<string> order = form.Elements().Select(e => (string)e.Attribute(x + "Name")).Where(n => n != null).ToList();
         Assert.True(order.IndexOf("NewKeyFileRow") < order.IndexOf("NewCertNameRow"), "The name comes before the files in the tab order.");
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
