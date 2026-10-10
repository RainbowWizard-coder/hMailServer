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
   /// Spacing on the token scale (review finding F295): 189 of 682 literal margins and
   /// paddings were off the scale, the scale's own XAML tokens had no consumer, the page
   /// and card paddings themselves were on a seventh step, 20; a list card's padding was
   /// 4, 6, 8, 10 or 12 by page and every dialog named a width of its own; the settings
   /// panels were as wide as their widest card, so that the cards' right edges jumped
   /// from tab to tab, and a page's column was 680, 760, 860, 880, 980 or 1000 pixels by
   /// page; and four App.xaml styles were dead. build/check-spacing.py holds every margin
   /// and padding written as numbers; these hold the tokens and the pages.
   /// </summary>
   public class SpacingScaleTests
   {
      private static readonly double[] Scale = { 0, 4, 8, 12, 16, 24, 32 };

      // A dialog's width held as a number in a constant or a variable: "DialogWidth = 560".
      private static readonly Regex HeldDialogWidth = new(@"\b\w*DialogWidth\s*=\s*(\d{3})\b", RegexOptions.IgnoreCase);

      [Theory]
      [InlineData("AppPagePadding", "24")]
      [InlineData("AppCardPadding", "24,16")]
      [InlineData("AppListCardPadding", "8")]
      [InlineData("AppDialogPadding", "24")]
      public void ThePagesTheCardsAndTheDialogsInsets_AreOnTheScale(string key, string value)
      {
         string tokens = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Tokens.xaml");
         Assert.Contains("<Thickness x:Key=\"" + key + "\">" + value + "</Thickness>", tokens, StringComparison.Ordinal);
      }

      [Fact]
      public void EveryThicknessToken_IsOnTheScale_AndTheNumbersAreNotTokens()
      {
         string tokens = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Tokens.xaml");
         foreach (Match thickness in Regex.Matches(tokens, "<Thickness x:Key=\"(\\w+)\">([^<]+)</Thickness>"))
         {
            if (thickness.Groups[1].Value == "AppCheckBoxIndent" || thickness.Groups[1].Value == "AppStatusFillBorderThickness")
               continue;   // the box and its gap; the pill's hairline
            foreach (string part in thickness.Groups[2].Value.Split(','))
               Assert.Contains(double.Parse(part, System.Globalization.CultureInfo.InvariantCulture), Scale);
         }

         // The six steps are C# (DesignTokens.Space); as XAML numbers no Margin could take them, and none did.
         Assert.DoesNotMatch(new Regex("x:Key=\"AppSpace"), tokens);
         Assert.Equal(DesignTokens.Space.Sm, DesignTokens.ListCardPadding);
      }

      [Fact]
      public void TheDeadStyles_AreGone()
      {
         string app = SaveStateModelTests.ReadControlPanelFile("App.xaml");
         foreach (string style in new[] { "PageTitle", "PageSubtitle", "StatusLabel", "StatusValue" })
            Assert.DoesNotContain("x:Key=\"" + style + "\"", app, StringComparison.Ordinal);
      }

      /// <summary>The scaffold's own gaps are on the scale: a card's description and footer, a section's description and fields.</summary>
      [Fact]
      public void TheScaffoldsGaps_AreOnTheScale()
      {
         string scaffold = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Scaffold.xaml");
         Assert.Contains("Style=\"{StaticResource TextCaption}\" Margin=\"0,0,0,12\" />", scaffold, StringComparison.Ordinal);
         Assert.Contains("<Border x:Name=\"PART_Footer\" Grid.Row=\"3\" Margin=\"0,12,0,0\" Padding=\"0,12,0,0\"", scaffold, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"Padding\" Value=\"{StaticResource AppDialogPadding}\" />", scaffold, StringComparison.Ordinal);
         Assert.DoesNotContain("Margin=\"0,0,0,10\"", scaffold, StringComparison.Ordinal);
         Assert.DoesNotContain("Margin=\"0,10,0,0\"", scaffold, StringComparison.Ordinal);
         Assert.DoesNotContain("Margin=\"0,2,0,0\"", scaffold, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDialogWidths_AreFour_OnTheGrid_AndEveryDialogNamesOne()
      {
         Assert.Equal(new[] { 420.0, 520, 640, 760 }, DesignTokens.Dialog.Widths);
         foreach (double width in DesignTokens.Dialog.Widths)
         {
            Assert.Equal(0, width % 4);
            Assert.InRange(width, DesignTokens.Dialog.MinWidth, DesignTokens.Dialog.MaxWidth);
         }

         // The domain and account dialogs' width, for their many tabs, is named too, on the grid.
         Assert.Equal(800, DesignTokens.Dialog.ManyTabsWidth);
         Assert.Equal(0, DesignTokens.Dialog.ManyTabsWidth % 4);
         Assert.InRange(DesignTokens.Dialog.ManyTabsWidth, DesignTokens.Dialog.WidestWidth, DesignTokens.Dialog.MaxWidth);

         // A number in the frame's call, and a number held in a constant or a variable that
         // the call is given: the IP range and route dialogs kept "private const double
         // DialogWidth = 560;" under "width: DialogWidth", and the domain and account
         // dialogs 800, which the call's pattern alone could not see.
         var literal = new Regex(@"(?:UseFrame|\.Frame)\([^;]*?(?:width:\s*|,\s*)(\d{3})\)\s*;", RegexOptions.Singleline);
         var problems = Directory.GetFiles(ViewsDirectory_(), "*.cs")
            .SelectMany(file =>
            {
               string code = File.ReadAllText(file);
               return literal.Matches(code).Concat(HeldDialogWidth.Matches(code)).Select(m => Path.GetFileName(file) + ": " + m.Value);
            })
            .ToList();
         Assert.True(problems.Count == 0, "A dialog names a width of its own, not one of DesignTokens.Dialog's:\n" + string.Join("\n", problems));
      }

      /// <summary>The check above sees a width held in a constant as it sees one in the call.</summary>
      [Theory]
      [InlineData("private const double DialogWidth = 560;", true)]
      [InlineData("double dialogWidth = 600;", true)]
      [InlineData("private const double DialogWidth = DesignTokens.Dialog.WideWidth;", false)]
      [InlineData("private const double CertificateNameWidth = 280;", false)]
      public void AWidthHeldInAConstant_IsSeen(string line, bool seen)
         => Assert.Equal(seen, HeldDialogWidth.IsMatch(line));

      /// <summary>
      /// A card that holds a list or a grid takes the compact inset by its name, where it was
      /// 4, 6, 8, 10 or 12 by page; the other cards keep the card's own (the MX query's answer,
      /// a card of text, keeps 12, which is on the scale).
      /// </summary>
      [Theory]
      [InlineData("AppPasswordsPanel.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding) };")]
      [InlineData("AuditTrailView.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("CollectionEditorView.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding) };")]
      [InlineData("DiscoveryView.cs", "var casesCard = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("DomainPolicyListView.cs", "var listCard = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("MessageTraceView.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("ProfilesView.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = body };")]
      [InlineData("PublicFoldersView.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("QuarantineView.cs", "var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("SuppressionListView.cs", "var listCard = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };")]
      [InlineData("UtilityViews.cs", "var listCard = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = listHost };")]
      [InlineData("IPRangesView.xaml", "<scaffold:Card scaffold:FillPanel.Fill=\"True\" Padding=\"{StaticResource AppListCardPadding}\">")]
      [InlineData("RoutesView.xaml", "<scaffold:Card scaffold:FillPanel.Fill=\"True\" Padding=\"{StaticResource AppListCardPadding}\">")]
      [InlineData("RulesView.xaml", "<scaffold:Card scaffold:FillPanel.Fill=\"True\" Padding=\"{StaticResource AppListCardPadding}\">")]
      [InlineData("SslCertificatesView.xaml", "<scaffold:Card scaffold:FillPanel.Fill=\"True\" Padding=\"{StaticResource AppListCardPadding}\">")]
      [InlineData("TcpIpPortsView.xaml", "<scaffold:Card scaffold:FillPanel.Fill=\"True\" Padding=\"{StaticResource AppListCardPadding}\">")]
      [InlineData("QueueView.xaml", "<scaffold:Card Grid.Row=\"3\" Padding=\"{StaticResource AppListCardPadding}\">")]
      [InlineData("LogsView.xaml", "<scaffold:Card Grid.Row=\"1\" Padding=\"{StaticResource AppListCardPadding}\">")]
      public void AListCardsInset_IsTheToken(string file, string line)
         => Assert.Contains(line, SaveStateModelTests.ReadControlPanelFile("Views/" + file), StringComparison.Ordinal);

      /// <summary>Every settings card of a page and of each of its tabs ends at one edge: the column is as wide as it may be.</summary>
      [Theory]
      [InlineData("Views/ServerSettingsView.xaml.cs", "var panel = new Scaffold.FormColumnPanel")]
      [InlineData("Views/FeatureSettingsView.xaml.cs", "panel = new FormColumnPanel")]
      [InlineData("Views/FeatureSettingsView.xaml", "<scaffold:FormColumnPanel MaxWidth=\"{StaticResource AppFormWidth}\" HorizontalAlignment=\"Left\">")]
      [InlineData("Views/LdapSettingsView.cs", "var cards = new FormColumnPanel { MaxWidth = DesignTokens.FormWidth")]
      [InlineData("Views/DirectorySyncView.cs", "var cards = new FormColumnPanel {")]
      [InlineData("Views/BackupView.xaml", "<scaffold:FormColumnPanel MaxWidth=\"{StaticResource AppFormWidth}\" HorizontalAlignment=\"Left\">")]
      public void TheSettingsPagesCards_StandInTheFormColumn(string file, string line)
         => Assert.Contains(line, SaveStateModelTests.ReadControlPanelFile(file), StringComparison.Ordinal);

      [Fact]
      public void TheFormColumn_MeasuresItselfByFormColumn()
      {
         string panel = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/FormColumnPanel.cs");
         Assert.Contains("public class FormColumnPanel : StackPanel", panel, StringComparison.Ordinal);
         Assert.Contains("desired.Width = FormColumn.Width(desired.Width, constraint.Width);", panel, StringComparison.Ordinal);
      }

      /// <summary>
      /// A page's column is one of two widths, the form's (760) or an overview's (1000),
      /// by name: the forms and the text pages in the form's column, the overviews in a
      /// stack of the overview width. They were 680, 760, 860, 880, 980 and 1000.
      /// </summary>
      [Theory]
      [InlineData("Views/AboutView.cs", "var panel = new FormColumnPanel { MaxWidth = DesignTokens.FormWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/ApiKeysView.cs", "var cards = new FormColumnPanel { Margin = new Thickness(0, 0, 12, 0), MaxWidth = DesignTokens.FormWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/UtilityViews.cs", "var panel = new FormColumnPanel { MaxWidth = DesignTokens.FormWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/StalledMailView.cs", "var panel = new FormColumnPanel { MaxWidth = DesignTokens.FormWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/DnsRecordsView.cs", "var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/ExternalSetupView.cs", "var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/SpamOverviewView.cs", "var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/TlsOverviewView.cs", "var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/VirusOverviewView.cs", "var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      [InlineData("Views/WelcomeView.cs", "var panel = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };")]
      public void APagesColumn_IsTheFormOrTheOverviewWidth(string file, string line)
         => Assert.Contains(line, SaveStateModelTests.ReadControlPanelFile(file), StringComparison.Ordinal);

      /// <summary>No page's column names a width of its own, in code or in XAML.</summary>
      [Fact]
      public void NoPagesColumn_NamesAWidthOfItsOwn()
      {
         Assert.Equal(760, DesignTokens.FormWidth);
         Assert.Equal(1000, DesignTokens.OverviewWidth);

         var code = new Regex(@"new (?:Scaffold\.)?(?:StackPanel|FormColumnPanel)\s*\{[^}]*\bMaxWidth = \d{3,4}\b");
         var xaml = new Regex(@"<(?:StackPanel|scaffold:FormColumnPanel)\b[^>]*\bMaxWidth=""\d{3,4}""");
         var problems = Directory.GetFiles(ViewsDirectory_(), "*.cs", SearchOption.AllDirectories)
            .SelectMany(file => code.Matches(File.ReadAllText(file)).Select(m => Path.GetFileName(file) + ": " + m.Value))
            .Concat(Directory.GetFiles(ViewsDirectory_(), "*.xaml", SearchOption.AllDirectories)
               .SelectMany(file => xaml.Matches(File.ReadAllText(file)).Select(m => Path.GetFileName(file) + ": " + m.Value)))
            .ToList();
         Assert.True(problems.Count == 0, "A page's column names a width of its own, not DesignTokens.FormWidth or OverviewWidth:\n" + string.Join("\n", problems));
      }

      private static string ViewsDirectory_()
         => Path.GetDirectoryName(Path.Join(ControlPanelDirectory_(), "Views", "x"));

      private static string ControlPanelDirectory_()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "MainWindow.xaml");
            if (File.Exists(candidate))
               return Path.Join(directory.FullName, "ControlPanel");
         }

         throw new InvalidOperationException("The Control Panel's sources could not be found from the test binaries.");
      }
   }
}
