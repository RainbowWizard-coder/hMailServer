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
   /// The page tour and the help pane no longer share a glyph (review findings F024
   /// and F081; T3.3 work item 8). A page a tour visits had a question mark in its
   /// header that started the tour, 250 pixels from the command bar's question mark
   /// that opens the help pane, and Welcome's "Show me around" was the same glyph:
   /// the tool tips told them apart, the glyphs did not. The tour is a light bulb
   /// with "Show me around" beside it where the header has room (the owner's
   /// decision of 9 October 2026), and the question mark is the help pane's alone.
   /// </summary>
   public class TourGlyphTests
   {
      [Fact]
      public void TheQuestionMark_IsTheHelpPanesAlone()
      {
         string root = Path.GetDirectoryName(ControlPanelFile_("MainWindow.xaml"));
         var uses = Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
            .Where(path => (path.EndsWith(".xaml", StringComparison.Ordinal) || path.EndsWith(".cs", StringComparison.Ordinal))
               && !path.Contains(Path.DirectorySeparatorChar + "obj" + Path.DirectorySeparatorChar, StringComparison.Ordinal)
               && !path.Contains(Path.DirectorySeparatorChar + "bin" + Path.DirectorySeparatorChar, StringComparison.Ordinal))
            .Where(path => File.ReadAllText(path).Contains("QuestionCircle", StringComparison.Ordinal))
            .Select(path => Path.GetFileName(path))
            .ToList();
         Assert.Equal(new[] { "MainWindow.xaml" }, uses);

         string shell = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.xaml");
         Assert.Matches(new Regex(@"x:Name=""HelpButton""[^>]*Icon=""\{ui:SymbolIcon QuestionCircle24\}""[^>]*AutomationProperties.AutomationId=""help-toggle"""), shell);
      }

      [Fact]
      public void TheTour_IsALightBulb_OnThePagesAndOnWelcome()
      {
         string template = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml"));
         Match help = Regex.Match(template, @"<ui:Button x:Name=""PART_Help""[^>]*/>", RegexOptions.Singleline);
         Assert.True(help.Success, "Scaffold.xaml has no PART_Help.");
         Assert.Contains("Icon=\"{ui:SymbolIcon Lightbulb24}\"", help.Value, StringComparison.Ordinal);

         string welcome = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "WelcomeView.cs"));
         Assert.Contains("SymbolRegular.Lightbulb24", welcome, StringComparison.Ordinal);
      }

      /// <summary>
      /// The header's button says "Show me around" where there is room, its name
      /// still says which page it shows, and it carries no Alt key: like the tour's
      /// card, it is on whichever page a tour visits.
      /// </summary>
      [Fact]
      public void TheHeadersTourButton_SaysShowMeAround_WhereThereIsRoom_WithNoAltKey()
      {
         string header = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "Scaffold", "PageHeader.cs"));
         Assert.Contains("object content = words ? L(\"Show me around\") : null;", header, StringComparison.Ordinal);
         Assert.DoesNotContain("Sho_w me around", header, StringComparison.Ordinal);
         Assert.Contains("string caption = L(\"Show me around this page\");", header, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(help, caption);", header, StringComparison.Ordinal);

         string measure = WithheldRowTests.MemberText(header, "protected override Size MeasureOverride(Size constraint)");
         Assert.True(measure != null, "PageHeader.MeasureOverride is gone.");
         Assert.Contains("PageHeaderLayout.TourCaptionShows(", measure, StringComparison.Ordinal);
         // The words are decided before the actions are placed beside the title or under it.
         Assert.True(measure.IndexOf("TourCaptionShows(", StringComparison.Ordinal) < measure.IndexOf("ActionsGoUnder(", StringComparison.Ordinal),
            "The header places its actions before it knows how wide the tour button is.");

         // Decided on the actions' width beside the title wherever they are, and, once
         // the words change, on the button measured off the tree: measured again in
         // place the bar answered with its old width, and beside the help pane at 760
         // the header measured for ever, the words coming and going.
         Assert.Contains("double actions = BesideWidth_(bar);", measure, StringComparison.Ordinal);
         Assert.Contains("actions = others + TourButtonWidth_(help, words);", measure, StringComparison.Ordinal);
         Assert.DoesNotContain("help.Measure(", measure, StringComparison.Ordinal);
         Assert.Contains("bar.Margin = under ? new Thickness(0, 12, 0, 0) : new Thickness(BesideGap, 6, 0, 0);", measure, StringComparison.Ordinal);
      }

      [Fact]
      public void TheWords_ShowWhereTheActionsFitBesideTheTitle_TheGlyphWhereOnlyItDoes()
      {
         // Room for the words beside the title.
         Assert.True(PageHeaderLayout.TourCaptionShows(900, 300, 200, 400));
         // The words would push the actions under the subtitle; the glyph keeps them beside the title.
         Assert.False(PageHeaderLayout.TourCaptionShows(650, 300, 200, 400));
         // Not even the glyph keeps them beside the title: under the subtitle, the words where the line holds them.
         Assert.True(PageHeaderLayout.TourCaptionShows(500, 300, 200, 400));
         Assert.False(PageHeaderLayout.TourCaptionShows(250, 300, 200, 400));
      }

      private static string ControlPanelFile_(string relative)
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
