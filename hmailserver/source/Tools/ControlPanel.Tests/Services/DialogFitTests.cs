// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// How the tabbed dialogs fit their content, held at the source: a scrolled
   /// tab and a dialog's body keep clear of the scroll bar drawn over them
   /// (review findings F192 and F073); the domain and account dialogs are 800
   /// pixels wide and as tall as their tallest tab within a cap (F193, F194);
   /// and the route and IP range dialogs are as wide as their headers need
   /// (F193).
   /// </summary>
   public class DialogFitTests
   {
      [Fact]
      public void AScrolledTab_KeepsClearOfItsBar()
      {
         string fields = ReadControlPanelFile(Path.Join("Views", "DialogFields.cs"));
         Assert.Matches(new Regex(@"public static ScrollViewer Scroll\(UIElement content\)[\s\S]{0,600}?ScrollBarInset\.SetIsEnabled\(viewer, true\);"), fields);

         string inset = ReadControlPanelFile(Path.Join("Services", "ScrollBarInset.cs"));
         Assert.Contains("ComputedVerticalScrollBarVisibility == Visibility.Visible", inset, StringComparison.Ordinal);
         Assert.Contains("barShowing ? DesignTokens.Space.Md : 0", inset, StringComparison.Ordinal);
      }

      /// <summary>The frame's body, which scrolls past a dialog's height - the distribution list dialog's did.</summary>
      [Fact]
      public void TheDialogFramesBody_KeepsClearOfItsBar()
      {
         string scaffold = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml"));
         int frame = scaffold.IndexOf("TargetType=\"{x:Type scaffold:DialogFrame}\"", StringComparison.Ordinal);
         Assert.True(frame > 0, "Scaffold.xaml has no DialogFrame style.");
         int viewer = scaffold.IndexOf("<ScrollViewer", frame, StringComparison.Ordinal);
         int end = scaffold.IndexOf('>', viewer);
         Assert.Contains("svc:ScrollBarInset.IsEnabled=\"True\"", scaffold.Substring(viewer, end - viewer), StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("DomainDialog.cs")]
      [InlineData("AccountDialog.cs")]
      public void TheTwoBigDialogs_AreWideAndAsTallAsTheirTallestTabWithinACap(string file)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains("private const double DialogWidth = DesignTokens.Dialog.ManyTabsWidth;", code, StringComparison.Ordinal);
         Assert.Equal(800, hMailServer.ControlPanel.Services.DesignTokens.Dialog.ManyTabsWidth);
         Assert.Contains("DialogFields.FitManyTabs(tabs, DialogFields.BodyWidth(DialogWidth));", code, StringComparison.Ordinal);
         Assert.DoesNotContain("FillTabs", code, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("RouteDialog.cs")]
      [InlineData("IPRangeDialog.cs")]
      public void TheFiveTabDialogs_AreAsWideAsTheirHeadersNeed(string file)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains("private const double DialogWidth = DesignTokens.Dialog.WideWidth;", code, StringComparison.Ordinal);
         Assert.Contains("double width = DialogFields.WidthForTabs(tabs, DialogWidth);", code, StringComparison.Ordinal);
         Assert.Contains("DialogFields.FitTabs(tabs, DialogFields.BodyWidth(width));", code, StringComparison.Ordinal);
         Assert.Contains("cancel, width: width);", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// FitTabs counts the strip from the headers themselves, not only from the
      /// tab control's measure outside a tree, which can come before the selected
      /// tab reaches its presenter and so counted a strip of nothing: the tallest
      /// tab then scrolled by the strip's height (F192, the route dialog's
      /// Addresses tab).
      /// </summary>
      [Fact]
      public void FitTabs_CountsTheStripFromItsHeaders()
      {
         string fields = ReadControlPanelFile(Path.Join("Views", "DialogFields.cs"));
         Assert.Matches(new Regex(@"private static double StripHeight_\(TabControl tabs, Size available\)[\s\S]{0,900}?header\.Measure\([\s\S]{0,300}?return Math\.Max\(strip, headers \+ 1\);"), fields);
      }

      /// <summary>
      /// The tab control is measured in the dialog's font, before the window it
      /// will inherit that font from exists, and rounded up: in the system's
      /// default font its row of headers came out two pixels short, and the
      /// tallest tab of the IP range dialog scrolled by those two pixels.
      /// </summary>
      [Fact]
      public void TheTabsAreMeasuredInTheDialogsFontAndRoundedUp()
      {
         string fields = ReadControlPanelFile(Path.Join("Views", "DialogFields.cs"));
         Assert.Matches(new Regex(@"private static double Needed_\(TabControl tabs, Size available\)\s*\{\s*InDialogFont_\(tabs\);\s*return Math\.Ceiling\(TallestTab_\(tabs, available\) \+ StripHeight_\(tabs, available\)\) \+ DesignTokens\.Space\.Xs;"), fields);
         Assert.Contains("tabs.FontFamily = new System.Windows.Media.FontFamily(Typography.UiFontFamily);", fields, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"public static double WidthForTabs\(TabControl tabs, double preferred\)\s*\{\s*InDialogFont_\(tabs\);"), fields);
      }

      /// <summary>No dialog asks for the whole height the screen allows without measuring its tabs any more.</summary>
      [Fact]
      public void NoDialog_TakesTheCeilingUnmeasured()
      {
         string views = ControlPanelDirectory("Views");
         string[] users = Directory.EnumerateFiles(views, "*.cs", SearchOption.AllDirectories)
            .Where(f => File.ReadAllText(f).Contains("FillTabs(", StringComparison.Ordinal))
            .Select(Path.GetFileName)
            .ToArray();
         Assert.True(users.Length == 0, "FillTabs is back in: " + string.Join(", ", users));
      }

      private static string ControlPanelDirectory(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative + " could not be found from the test binaries.");
         return null;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
