// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The help pane's own controls (review finding F027; T3.3 work item 9): All
   /// documentation stayed pressable on its own list and rebuilt it, the links to a
   /// document's headings were about 20 pixels tall, under the 24 that WCAG 2.5.8
   /// asks for, and a search that matched nothing said nothing. The WPF-free test
   /// project cannot draw the pane, so these hold its source to the fix.
   /// </summary>
   public class HelpPaneControlsTests
   {
      private static string Pane_() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "HelpPane.cs"));

      [Fact]
      public void AllDocumentation_CannotBePressed_WhereTheListIsWhatItShows()
      {
         string pane = Pane_();
         string all = WithheldRowTests.MemberText(pane, "public void ShowAll()");
         string none = WithheldRowTests.MemberText(pane, "private void ShowNoGuide_(string navigationKey)");
         string document = WithheldRowTests.MemberText(pane, "public void ShowDocument(string name, string heading)");
         Assert.True(all != null && none != null && document != null, "ShowAll, ShowNoGuide_ or ShowDocument is gone.");
         Assert.Contains("allButton_.IsEnabled = false;", all, StringComparison.Ordinal);
         Assert.Contains("allButton_.IsEnabled = false;", none, StringComparison.Ordinal);
         Assert.Contains("allButton_.IsEnabled = true;", document, StringComparison.Ordinal);

         // Pressed from the keyboard, the keyboard goes to the search rather than
         // being left on a button that has just been disabled.
         Assert.Contains("bool hadKeyboard = allButton_.IsKeyboardFocused;", pane, StringComparison.Ordinal);
         Assert.Contains("search_.Focus();", pane, StringComparison.Ordinal);
      }

      [Fact]
      public void AHeadingsLink_IsTwentyFourPixelsTallAtLeast()
      {
         string link = WithheldRowTests.MemberText(Pane_(), "private FrameworkElement HeadingLink_(MdHeading heading)");
         Assert.True(link != null, "HelpPane.HeadingLink_ is gone.");
         Assert.Contains("Padding = new Thickness(heading.Level == 2 ? 14 : 4, 4, 4, 4),", link, StringComparison.Ordinal);
         Assert.Contains("MinHeight = 24,", link, StringComparison.Ordinal);
      }

      /// <summary>The search says what it found under its box, and that it found nothing, as a polite live region named by its text.</summary>
      [Fact]
      public void TheSearch_SaysWhatItFound_AndThatItFoundNothing()
      {
         string pane = Pane_();
         string find = WithheldRowTests.MemberText(pane, "private void Find_(bool next)");
         Assert.True(find != null, "HelpPane.Find_ is gone.");
         Assert.Contains("ShowFindStatus_(F(\"No match for \\\"{0}\\\" in this document.\", term));", find, StringComparison.Ordinal);
         Assert.Contains("ShowFindStatus_(F(\"Match {0} of {1} - press Enter for the next.\",", find, StringComparison.Ordinal);
         // A term too short to search clears the line, as a new document's empty box does.
         Assert.Contains("ShowFindStatus_(null);", find, StringComparison.Ordinal);

         Assert.Contains("AutomationProperties.SetLiveSetting(findStatus_, AutomationLiveSetting.Polite);", pane, StringComparison.Ordinal);
         Assert.Contains("Announce.Request(findStatus_);", pane, StringComparison.Ordinal);
         Assert.DoesNotContain("AutomationProperties.SetName(findStatus_", pane, StringComparison.Ordinal);
         // Under the box, in the pane's search row.
         Assert.Contains("searchBlock.Children.Add(search_);", pane, StringComparison.Ordinal);
         Assert.Contains("searchBlock.Children.Add(findStatus_);", pane, StringComparison.Ordinal);
      }
   }
}
