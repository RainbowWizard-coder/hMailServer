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
   /// The dashboard's chart range (review finding F244). Live, 24 hours, 7 days
   /// and 30 days were four buttons that marked the chosen range by its Primary
   /// colour alone: a screen reader heard "Show Live" and three more with none
   /// chosen, and pressing the chosen range read the history again. They are a
   /// radio group named Chart range now, the chosen one drawn as the primary
   /// button was. The test project has no WPF, so the page is held to its source.
   /// </summary>
   public class ChartRangeGroupTests
   {
      [Fact]
      public void TheRangesAreARadioGroupNamedChartRange()
      {
         string page = ReadView("DashboardView.xaml.cs");

         // One group of radio buttons, Live chosen, named by the caption beside it.
         Assert.Contains("Text = L(\"Chart range\"),", page, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(group, L(\"Chart range\"));", page, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"new RadioButton\s*\{\s*Content = label,\s*GroupName = ""dashboard-chart-range"", // no-loc\s*IsChecked = key == ""live"","), page);

         // A group a screen reader names: its peer is a Group, not a bare panel's
         // nothing.
         Assert.Contains("protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Group;", page, StringComparison.Ordinal);
         Assert.Contains("protected override bool IsControlElementCore() => true;", page, StringComparison.Ordinal);
         // "2 of 4", which a panel of radio buttons does not count by itself.
         Assert.Contains("AutomationProperties.SetPositionInSet(choice, group.Children.Count + 1);", page, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetSizeOfSet(choice, ranges.Length);", page, StringComparison.Ordinal);

         // The chosen range drawn as the primary button: WPF-UI's toggle button,
         // whose checked state is the accent fill.
         Assert.Contains("new Style(typeof(RadioButton), TryFindResource(\"DefaultToggleButtonStyle\") as Style)", page, StringComparison.Ordinal);
         Assert.DoesNotContain("F(\"Show {0}\", label)", page, StringComparison.Ordinal);
      }

      [Fact]
      public void ARangeIsReadWhenItIsChosen_NotWhenItIsPressedAgain()
      {
         string page = ReadView("DashboardView.xaml.cs");

         // Checked fires when a range becomes the chosen one, and a chosen radio
         // button pressed again stays chosen and raises nothing.
         Assert.Contains("choice.Checked += (s, e) => ShowRange_(key);", page, StringComparison.Ordinal);
         Assert.DoesNotContain("button.Click += (s, e) => ShowRange_(key);", page, StringComparison.Ordinal);
         Assert.DoesNotContain("ControlAppearance.Primary : Wpf.Ui.Controls.ControlAppearance.Secondary", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheGroupIsOneTabStop_AndTheArrowKeysMoveTheChoice()
      {
         string page = ReadView("DashboardView.xaml.cs");

         Assert.Contains("KeyboardNavigation.SetTabNavigation(group, KeyboardNavigationMode.Once);", page, StringComparison.Ordinal);
         Assert.Contains("group.PreviewKeyDown += (s, e) => MoveRangeChoice_(group, e);", page, StringComparison.Ordinal);
         Assert.Contains("int step = e.Key is Key.Right or Key.Down ? 1 : e.Key is Key.Left or Key.Up ? -1 : 0;", page, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"next\.IsChecked = true;\s*next\.Focus\(\);\s*e\.Handled = true;"), page);
      }

      /// <summary>
      /// The note on a range - one point per minute, from the history the server
      /// keeps - is under the choice and wraps at the page's width: beside it, in
      /// a row of unbounded width, it ran past the window's edge.
      /// </summary>
      [Fact]
      public void TheRangeNoteIsUnderTheChoice_AndGoneWhileItSaysNothing()
      {
         string page = ReadView("DashboardView.xaml.cs");

         Assert.Contains("private readonly StackPanel rangeBar_ = new() { Margin = new Thickness(0, 0, 0, 8) };", page, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"row\.Children\.Add\(group\);\s*rangeBar_\.Children\.Add\(row\);"), page);
         Assert.Contains("Property = TextBlock.TextProperty, Value = \"\", Setters = { new Setter(VisibilityProperty, Visibility.Collapsed) }", page, StringComparison.Ordinal);
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
