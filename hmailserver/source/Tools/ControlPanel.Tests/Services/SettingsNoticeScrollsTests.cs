// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The settings pages ServerSettingsView builds pin their header and tab strip
   /// above tabs that scroll, and in the narrowest window (760 by 520) the pinned
   /// title, a two-line subtitle, a two-line notice and the strip left the settings
   /// 140 pixels - one card heading and two lines of text on Auto-ban (review finding
   /// F290). The page's notice now sits above the first card of the tab on screen and
   /// scrolls away with it, and below 600 pixels of page the subtitle is the title's
   /// tool tip; the strip stays pinned.
   /// </summary>
   public class SettingsNoticeScrollsTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Fact]
      public void TheNotice_StartsInASlotUnderTheHeader_ThatItCanMoveFrom()
      {
         XDocument page = XDocument.Parse(Read(Path.Join("Views", "ServerSettingsView.xaml")));
         XElement notice = page.Descendants().Single(e => (string)e.Attribute(X + "Name") == "StatusNotice");
         Assert.Equal(Wpf + "ContentControl", notice.Parent.Name);
         Assert.Equal("PageNoticeSlot", (string)notice.Parent.Attribute(X + "Name"));
         Assert.Equal("settings-status", (string)notice.Attribute("AutomationProperties.AutomationId"));
      }

      [Fact]
      public void EachTab_HasASlotAboveItsFirstCard_AndTheNoticeGoesWithTheTabOnScreen()
      {
         string source = Read(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string build = Member(source, "private void BuildUi()");

         // The tabs that held it go first; then each tab's slot is the first part of
         // its panel, after the tab's own REST notice is put there.
         int detach = build.IndexOf("tabNoticeSlots_.Clear();", StringComparison.Ordinal);
         int clear = build.IndexOf("SettingsTabs.Items.Clear();", StringComparison.Ordinal);
         Assert.True(detach >= 0 && detach < clear, "BuildUi clears the tabs before taking the notice out of them.");
         int rest = build.IndexOf("panel.Children.Insert(0, RestTabNotice_(tabRestProblem));", StringComparison.Ordinal);
         int slot = build.IndexOf("panel.Children.Insert(0, noticeSlot);", StringComparison.Ordinal);
         Assert.True(rest >= 0 && slot > rest, "The notice's slot is not the first part of the tab.");
         Assert.Contains("tabNoticeSlots_.Add(noticeSlot);", build, StringComparison.Ordinal);
         int select = build.IndexOf("SettingsTabs.SelectedIndex = 0;", StringComparison.Ordinal);
         Assert.True(build.IndexOf("PlaceNotice_();", select, StringComparison.Ordinal) > select, "BuildUi does not place the notice in the first tab.");

         // A change of tab moves it; a list on a tab, whose SelectionChanged bubbles, does not.
         Assert.Matches(new Regex(@"SettingsTabs\.SelectionChanged \+= \(s, e\) =>\s*\{\s*(?://[^\n]*\n\s*)*if \(ReferenceEquals\(e\.OriginalSource, SettingsTabs\)\)\s*PlaceNotice_\(\);"), source);

         string place = Member(source, "private void PlaceNotice_()");
         Assert.Contains("tabNoticeSlots_[at] : PageNoticeSlot", place, StringComparison.Ordinal);
         Assert.Contains("from.Content = null;", place, StringComparison.Ordinal);
      }

      [Fact]
      public void ANoticeToActOn_IsScrolledIntoView()
      {
         // Above the tab's first card, a save made far down a long tab said which
         // row it could not write out of sight.
         string show = Member(Read(Path.Join("Views", "ServerSettingsView.xaml.cs")), "private void ShowNotice_(string sentence, StatusLevel level, Action tryAgain = null)");
         Assert.Matches(new Regex(@"if \(level >= StatusLevel\.Warning\)\s*Dispatcher\.BeginInvoke\(DispatcherPriority\.Loaded, new Action\(\(\) => StatusNotice\.BringIntoView\(\)\)\);"), show);
      }

      [Fact]
      public void InAShortWindow_TheSubtitleIsTheTitlesToolTip()
      {
         string source = Read(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("SizeChanged += (s, e) => Header.SubtitleInToolTip = PageHeaderLayout.SubtitleInToolTip(e.NewSize.Height);", source, StringComparison.Ordinal);

         string scaffold = Read(Path.Join("Views", "Scaffold", "Scaffold.xaml"));
         Match trigger = Regex.Match(scaffold, "<Trigger Property=\"SubtitleInToolTip\" Value=\"True\">(.*?)</Trigger>", RegexOptions.Singleline);
         Assert.True(trigger.Success, "The PageHeader template has no SubtitleInToolTip trigger.");
         Assert.Contains("<Setter TargetName=\"PART_Subtitle\" Property=\"Visibility\" Value=\"Collapsed\" />", trigger.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("TargetName=\"PART_TitleRow\" Property=\"ToolTip\"", trigger.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("{Binding Subtitle, RelativeSource={RelativeSource TemplatedParent}}", trigger.Groups[1].Value, StringComparison.Ordinal);
      }

      [Theory]
      // A 760 by 520 window gives a page some 395 pixels; 1280 by 800 some 675.
      [InlineData(395, true)]
      [InlineData(599, true)]
      [InlineData(600, false)]
      [InlineData(675, false)]
      // Not laid out yet: the subtitle stays.
      [InlineData(0, false)]
      public void TheSubtitleGoesIntoTheToolTip_BelowSixHundredPixelsOfPage(double height, bool toolTip)
         => Assert.Equal(toolTip, PageHeaderLayout.SubtitleInToolTip(height));

      private static string Member(string source, string signature)
      {
         int start = source.IndexOf(signature, StringComparison.Ordinal);
         Assert.True(start >= 0, signature + " is gone.");
         int open = source.IndexOf('{', start);
         int depth = 0;
         for (int i = open; i < source.Length; i++)
         {
            if (source[i] == '{')
               depth++;
            else if (source[i] == '}' && --depth == 0)
               return source.Substring(start, i - start + 1);
         }

         return source.Substring(start);
      }

      private static string Read(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
