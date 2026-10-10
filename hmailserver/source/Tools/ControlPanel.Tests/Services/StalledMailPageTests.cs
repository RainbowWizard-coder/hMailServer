// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Diagnosing stalled mail (review finding F251). The Accepting card's "Test
   /// SpamAssassin and ClamAV" opened Diagnostics, which runs port 25, MX,
   /// backup-folder and IP checks and tests neither scanner; its rows of links were
   /// horizontal stacks, so the row of three timeout links (708 pixels in English,
   /// about 790 in German and Japanese) and the German debug-logging row were cut
   /// in the 760 window; and each step's paragraph was its card's 12-pixel
   /// description at about 180 characters a line, above tables of 14.
   /// </summary>
   public class StalledMailPageTests
   {
      /// <summary>Each scanner's test is opened on its own tab of its own page.</summary>
      [Fact]
      public void TheScannerTests_OpenOnTheirTabs()
      {
         string code = Source_();
         string accepting = WithheldRowTests.MemberText(code, "private Card Accepting()");
         Assert.True(accepting != null, "StalledMailView.Accepting is gone.");

         Assert.Contains("Link(L(\"Anti-spam settings: test SpamAssassin\"), \"antispam\", N(\"Content filters\"))", accepting, StringComparison.Ordinal);
         Assert.Contains("Link(L(\"Anti-virus settings: test ClamAV\"), \"antivirus\", N(\"ClamAV\"))", accepting, StringComparison.Ordinal);
         Assert.DoesNotContain("\"diagnostics\"", accepting, StringComparison.Ordinal);

         // The ClamAV timeouts the Delivering card names are on the ClamAV tab too.
         Assert.Contains("Link(L(\"Anti-virus settings (ClamAV timeouts)\"), \"antivirus\", N(\"ClamAV\"))", code, StringComparison.Ordinal);

         // And the link opens the page on the tab it names.
         string link = WithheldRowTests.MemberText(code, "private static Wpf.Ui.Controls.Button Link(string text, string navKey, string tab = null)");
         Assert.True(link != null, "StalledMailView.Link is gone.");
         Assert.Contains("?.NavigateToTab(navKey, tab);", link, StringComparison.Ordinal);

         // The tabs named hold the tests: each test button is on its card of that tab.
         string settings = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         AssertOnTab_(WithheldRowTests.MemberText(settings, "private void BuildAntiSpam()"), "ButtonText = L(\"Test SpamAssassin connection\")", "Tab(L(\"Content filters\")).Cards.Add(sa);");
         AssertOnTab_(WithheldRowTests.MemberText(settings, "private void BuildAntiVirus()"), "ButtonText = L(\"Test ClamAV connection\")", "Tab(L(\"ClamAV\")).Cards.Add(clamav);");
      }

      /// <summary>Every row of buttons wraps, and none is a horizontal stack.</summary>
      [Fact]
      public void TheRowsOfLinks_Wrap()
      {
         string code = Source_();
         Assert.DoesNotContain("Orientation = Orientation.Horizontal", code, StringComparison.Ordinal);
         Assert.Equal(4, Regex.Matches(code, @"var links = new WrapPanel \{ Margin = new Thickness\(0, \d+, 0, 0\) \};").Count);
         Assert.Contains("var row = new WrapPanel();", code, StringComparison.Ordinal);
      }

      /// <summary>The page is the form's width, and each step's paragraph is body text in its card.</summary>
      [Fact]
      public void ThePage_IsTheFormsWidth_AndItsParagraphsBodyText()
      {
         string code = Source_();
         Assert.Contains("var panel = new FormColumnPanel { MaxWidth = DesignTokens.FormWidth, HorizontalAlignment = HorizontalAlignment.Left };", code, StringComparison.Ordinal);
         Assert.DoesNotContain("MaxWidth = 980", code, StringComparison.Ordinal);

         string section = WithheldRowTests.MemberText(code, "private static Card Section(string heading, string intro)");
         Assert.True(section != null, "StalledMailView.Section is gone.");
         Assert.Contains("paragraph.SetResourceReference(StyleProperty, \"TextBody\");", section, StringComparison.Ordinal);
         Assert.Contains("new Card { Title = heading, Content = content }", section, StringComparison.Ordinal);
         Assert.DoesNotContain("Description = intro", section, StringComparison.Ordinal);
      }

      private static void AssertOnTab_(string build, string what, string tab)
      {
         Assert.True(build != null, "A ServerSettingsView builder is gone.");
         int at = build.IndexOf(what, StringComparison.Ordinal);
         int added = build.IndexOf(tab, StringComparison.Ordinal);
         Assert.True(at >= 0 && added > at, what + " is no longer on the card " + tab + " adds.");

         // Nothing else is added to a tab between the test and its card's line.
         Assert.DoesNotContain("Tab(L(", build.Substring(at, added - at), StringComparison.Ordinal);
      }

      private static string Source_() => ReadControlPanelFile(Path.Join("Views", "StalledMailView.cs"));

      private static string ReadControlPanelFile(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "ControlPanel", "Views")))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("The Control Panel's source was not found above " + AppContext.BaseDirectory, relative);
      }
   }
}
