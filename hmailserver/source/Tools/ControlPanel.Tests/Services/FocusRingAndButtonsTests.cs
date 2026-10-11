// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// One rule each for the focus, the buttons with nothing round them, the message
   /// boxes and the icons (review findings F296 and F041). The focus was drawn four ways
   /// - the violet ring on the fields, a violet border on the navigation's rows, a
   /// two-tone ring on the grids and the tabs, and WPF-UI's own one-pixel rectangle
   /// elsewhere; Transparent buttons kept a one-pixel outline, so that the breadcrumb and
   /// the Related links read as chips; message boxes stretched a one-line notice into a
   /// block of tint two hundred pixels tall, a question had no mark, and Close was
   /// Primary in one dialog and secondary in others; and icon sizes ran from 12 to 40
   /// with no token.
   /// </summary>
   public class FocusRingAndButtonsTests
   {
      [Fact]
      public void EveryControlsFocusVisual_IsTheOneRing_UnderBothKeys()
      {
         string focus = SaveStateModelTests.ReadControlPanelFile("Services/AccessibleFocus.cs");

         // The focus colour by its key, two pixels wide, two pixels outside.
         Assert.Contains("ring.SetResourceReference(Shape.StrokeProperty, \"AppFocusBrush\");", focus, StringComparison.Ordinal);
         Assert.Contains("ring.SetValue(Shape.StrokeThicknessProperty, DesignTokens.FocusRingThickness);", focus, StringComparison.Ordinal);
         Assert.Contains("ring.SetValue(FrameworkElement.MarginProperty, new Thickness(-outside));", focus, StringComparison.Ordinal);
         Assert.Contains("ring_ = BuildRing_(DesignTokens.FocusRingThickness);", focus, StringComparison.Ordinal);
         Assert.Equal(2, DesignTokens.FocusRingThickness);

         // WPF's key and WPF-UI's, so that WPF-UI's own controls draw it too.
         Assert.Contains("internal const string WpfUiFocusVisualKey = \"DefaultControlFocusVisualStyle\";", focus, StringComparison.Ordinal);
         Assert.Contains("Application.Current.Resources[SystemParameters.FocusVisualStyleKey] = style;", focus, StringComparison.Ordinal);
         Assert.Contains("Application.Current.Resources[WpfUiFocusVisualKey] = style;", focus, StringComparison.Ordinal);

         // Drawn inside a tab, whose strip's scroller cut the ring outside it.
         Assert.Contains("insetRing_ = BuildRing_(0);", focus, StringComparison.Ordinal);
         Assert.Contains("Application.Current.Resources[InsetFocusVisualKey] = highContrast ? insetTwoTone_ : insetRing_;", focus, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"FocusVisualStyle\" Value=\"{DynamicResource AppFocusVisualInset}\" />", SaveStateModelTests.ReadControlPanelFile("App.xaml"), StringComparison.Ordinal);

         // The two-tone ring under High Contrast alone, chosen on every theme change.
         Assert.Contains("Style style = highContrast ? twoTone_ : ring_;", focus, StringComparison.Ordinal);
         string tokens = SaveStateModelTests.ReadControlPanelFile("Services/ThemeTokens.cs");
         Assert.Contains("AccessibleFocus.Apply(CurrentChartTheme == ChartTheme.HighContrast);", tokens, StringComparison.Ordinal);
      }

      /// <summary>The navigation's and the lists' rows draw the same ring inside themselves, two pixels wide.</summary>
      [Fact]
      public void TheRowsFocus_IsTheRingsWidth()
      {
         string app = SaveStateModelTests.ReadControlPanelFile("App.xaml");
         string window = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");

         Assert.DoesNotContain("BorderThickness=\"1.5\"", app + window, StringComparison.Ordinal);
         Assert.DoesNotContain("StrokeThickness\" Value=\"1.5\"", app, StringComparison.Ordinal);
         Assert.Contains("<Setter TargetName=\"Ring\" Property=\"StrokeThickness\" Value=\"2\" />", app, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSubtleButton_HasNothingRoundIt_AndTheShellsGlyphAndLinkButtonsTakeIt()
      {
         string app = SaveStateModelTests.ReadControlPanelFile("App.xaml");
         Match style = Regex.Match(app, "<Style x:Key=\"SubtleButton\"[^>]*>(.*?)</Style>", RegexOptions.Singleline);
         Assert.True(style.Success, "App.xaml has no SubtleButton style.");
         Assert.Contains("<Setter Property=\"Appearance\" Value=\"Transparent\" />", style.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"BorderThickness\" Value=\"0\" />", style.Groups[1].Value, StringComparison.Ordinal);
         // Disabled, no fill either - the dark theme's page showed WPF-UI's: a disabled Forward was a grey box.
         Assert.Contains("<SolidColorBrush x:Key=\"ButtonBackgroundDisabled\" Color=\"Transparent\" />", style.Groups[1].Value, StringComparison.Ordinal);

         string window = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         foreach (string button in new[] { "SidebarToggle", "ThemeButton", "HelpButton", "AdminButton", "BackButton", "ForwardButton" })
            Assert.Matches(new Regex("x:Name=\"" + button + "\"[^>]*Style=\"\\{StaticResource SubtleButton\\}\""), window);
         Assert.DoesNotContain("Appearance=\"Transparent\"", window, StringComparison.Ordinal);

         string code = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs") + SaveStateModelTests.ReadControlPanelFile("MainWindow.Related.cs");
         Assert.Equal(3, Regex.Matches(code, @"SetResourceReference\(StyleProperty, ""SubtleButton""\)").Count);
         Assert.DoesNotContain("ControlAppearance.Transparent", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheIconSizes_AreTokens()
      {
         Assert.Equal(new[] { 16.0, 20, 24, 40 }, new[] { DesignTokens.IconSize.Small, DesignTokens.IconSize.Medium, DesignTokens.IconSize.Large, DesignTokens.IconSize.Hero });

         string tokens = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Tokens.xaml");
         Assert.Contains("<sys:Double x:Key=\"AppIconSizeSm\">16</sys:Double>", tokens, StringComparison.Ordinal);
         Assert.Contains("<sys:Double x:Key=\"AppIconSizeMd\">20</sys:Double>", tokens, StringComparison.Ordinal);
         Assert.Contains("<sys:Double x:Key=\"AppIconSizeLg\">24</sys:Double>", tokens, StringComparison.Ordinal);
         Assert.Contains("<sys:Double x:Key=\"AppIconSizeXl\">40</sys:Double>", tokens, StringComparison.Ordinal);

         string scaffold = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Scaffold.xaml");
         Assert.Contains("FontSize=\"{StaticResource AppIconSizeXl}\"", scaffold, StringComparison.Ordinal);
         string window = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         Assert.Equal(3, Regex.Matches(window, @"FontSize=""\{StaticResource AppIconSizeSm\}""").Count);
      }

      /// <summary>
      /// A message box's notice - a question's at Normal, its mark and no word - stands at
      /// the top of a window as tall as its message.
      /// </summary>
      [Fact]
      public void AMessageBox_IsAsTallAsItsMessage_AndAQuestionHasItsMark()
      {
         string dialogs = SaveStateModelTests.ReadControlPanelFile("Views/Dialogs.cs");
         Assert.Contains("if (level == StatusLevel.Normal && !question)", dialogs, StringComparison.Ordinal);
         Assert.Contains("body.VerticalAlignment = VerticalAlignment.Top;", dialogs, StringComparison.Ordinal);
         Assert.Contains("}, question: true);", dialogs, StringComparison.Ordinal);
         Assert.Contains("question: icon == MessageBoxImage.Question", dialogs, StringComparison.Ordinal);

         // With no heading, the head's gap goes as well.
         string scaffold = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Scaffold.xaml");
         Assert.Contains("<Setter TargetName=\"PART_Head\" Property=\"Visibility\" Value=\"Collapsed\" />", scaffold, StringComparison.Ordinal);

         string frame = SaveStateModelTests.ReadControlPanelFile("Views/FluentDialogWindow.cs");
         int within = frame.IndexOf("public void SizeToContentWithin(double width)", StringComparison.Ordinal);
         Assert.True(within >= 0 && frame.IndexOf("MinHeight = 0;", within, StringComparison.Ordinal) > within, "SizeToContentWithin keeps WPF-UI's 320-pixel floor.");
      }

      /// <summary>A dialog whose footer holds Close alone draws it Primary, its lists' Add secondary, and gives it no Enter.</summary>
      [Theory]
      [InlineData("FolderPermissionsDialog.cs", "var close = new Wpf.Ui.Controls.Button { Content = L(\"Close\")", "var add = new Wpf.Ui.Controls.Button { Content = L(\"_Add…\")")]
      [InlineData("GroupMembersDialog.cs", "var close = new Wpf.Ui.Controls.Button { Content = L(\"Close\")", "Content = L(\"_Add account…\"),")]
      [InlineData("RecipientsDialog.cs", "var closeButton = new Wpf.Ui.Controls.Button { Content = L(\"Close\")", null)]
      public void ALoneClose_IsTheDialogsPrimary(string file, string close, string add)
      {
         string[] lines = SaveStateModelTests.ReadControlPanelFile("Views/" + file).Replace("\r\n", "\n").Split('\n');
         string closeLine = lines.Single(l => l.Contains(close, StringComparison.Ordinal));
         Assert.Contains("Appearance = Wpf.Ui.Controls.ControlAppearance.Primary", closeLine, StringComparison.Ordinal);

         if (add != null)
         {
            int at = Array.FindIndex(lines, l => l.Contains(add, StringComparison.Ordinal));
            Assert.True(at >= 0, file + " no longer has its list's Add.");
            string block = string.Join("\n", lines.Skip(Math.Max(0, at - 2)).Take(5));
            Assert.DoesNotContain("ControlAppearance.Primary", block, StringComparison.Ordinal);
         }

         // Close is the secondary slot's, Escape's, and never takes Enter.
         string all = string.Join("\n", lines);
         Assert.Matches(new Regex(@"UseFrame\([^;]*, null, close(Button)?,", RegexOptions.Singleline), all);
      }
   }
}
