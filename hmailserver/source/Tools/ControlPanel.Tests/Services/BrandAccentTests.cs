// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// One blue (review finding F045; the owner's choice of 9 October 2026, the brand
   /// over the Windows accent). The shell drew three: the tab underline and the title
   /// bar's glyph in App.xaml's BrandBrush, #2F81F7 in both themes, which
   /// ThemeTokens' in-place recolour never reached; the rail and the selection pills
   /// in AppBrandBrush; and the primary buttons, the ticked check boxes and the
   /// palette's selection in the Windows accent - on a machine whose accent is red or
   /// orange, a colour the Control Panel keeps for danger and warning.
   /// </summary>
   public class BrandAccentTests
   {
      [Theory]
      [InlineData(ChartTheme.Light)]
      [InlineData(ChartTheme.Dark)]
      public void WpfUisAccent_IsTheBrand(ChartTheme theme)
      {
         DesignTokens.AccentArgb accent = DesignTokens.Accent(theme).Value;
         uint brand = DesignTokens.Status(theme, ChartSystemColors.Fallback).Brand;

         // The primary button's fill, the ticked box's and the selection's are the brand.
         Assert.Equal(brand, accent.Fill(theme));
         Assert.Equal(brand, accent.Primary);
         Assert.Equal(brand, accent.System);
      }

      /// <summary>
      /// The words on the primary button, the theme's own (white in the light theme, black
      /// in the dark), clear 4.5:1 on its fill at rest and under the pointer.
      /// </summary>
      [Theory]
      [InlineData(ChartTheme.Light)]
      [InlineData(ChartTheme.Dark)]
      public void TheWordsOnTheAccent_ClearFourPointFiveToOne_AtRestAndUnderThePointer(ChartTheme theme)
      {
         DesignTokens.AccentArgb accent = DesignTokens.Accent(theme).Value;

         Assert.Equal(theme == ChartTheme.Light ? 0xFFFFFFFFu : 0xFF000000u, accent.Text);
         foreach (uint fill in new[] { accent.Fill(theme), accent.Hover, accent.Primary })
         {
            double ratio = ChartPalette.ContrastRatio(accent.Text, fill);
            Assert.True(ratio >= 4.5, theme + ": the words on " + fill.ToString("X8") + " measure " + ratio.ToString("F2") + ":1.");
         }
      }

      /// <summary>
      /// WPF-UI chooses dark or light words for the accent by the secondary's brightness
      /// (its HSV value above 80); the choice must be the theme's own words, or the words
      /// it sets by that rule and the ones the theme's buttons draw would differ.
      /// </summary>
      [Theory]
      [InlineData(ChartTheme.Light, false)]
      [InlineData(ChartTheme.Dark, true)]
      public void TheSecondarysBrightness_ChoosesTheThemesOwnWords(ChartTheme theme, bool darkWords)
      {
         uint secondary = DesignTokens.Accent(theme).Value.Secondary;
         double value = Math.Max((secondary >> 16) & 0xFF, Math.Max((secondary >> 8) & 0xFF, secondary & 0xFF)) / 255.0 * 100;
         Assert.Equal(darkWords, value > 80);
      }

      [Fact]
      public void UnderHighContrast_TheAccentIsTheUsers()
         => Assert.Null(DesignTokens.Accent(ChartTheme.HighContrast));

      [Fact]
      public void ThemeTokens_GivesWpfUiTheAccentAfterEveryThemeChange()
      {
         string tokens = SaveStateModelTests.ReadControlPanelFile("Services/ThemeTokens.cs");
         Assert.Contains("if (DesignTokens.Accent(CurrentChartTheme) is DesignTokens.AccentArgb accent)", tokens, StringComparison.Ordinal);
         Assert.Contains("ApplicationAccentColorManager.Apply(FromArgb(accent.System), FromArgb(accent.Primary),", tokens, StringComparison.Ordinal);
         foreach (string key in new[] { "\"AccentFillColorSecondary\"", "\"AccentFillColorSecondaryBrush\"", "\"AccentFillColorTertiary\"", "\"AccentFillColorTertiaryBrush\"" })
            Assert.Contains(key, tokens, StringComparison.Ordinal);

         // The recolour of App.xaml's BrandBrush, which never took effect, is gone.
         Assert.DoesNotContain("\"BrandBrush\"", tokens, StringComparison.Ordinal);
      }

      /// <summary>
      /// WPF-UI's accent brushes themselves are the brand's, on the application's dictionary:
      /// its theme dictionary's brushes keep the accent colour they were made with, and the
      /// primary button stayed the Windows accent beside a ticked box in the brand. Under High
      /// Contrast they are taken away again; the focused field's line, the combo box's focused
      /// border and the link button's words are the focus and link colours, not the accent.
      /// </summary>
      [Fact]
      public void WpfUisAccentBrushes_AreRepublishedInTheBrand_AndWithdrawnUnderHighContrast()
      {
         var brushes = DesignTokens.AccentBrushes.ToDictionary(b => b.Key, b => b.Part);
         Assert.Equal(DesignTokens.AccentPart.Fill, brushes["AccentButtonBackground"]);
         Assert.Equal(DesignTokens.AccentPart.Hover, brushes["AccentButtonBackgroundPointerOver"]);
         Assert.Equal(DesignTokens.AccentPart.Pressed, brushes["AccentButtonBackgroundPressed"]);
         Assert.Equal(DesignTokens.AccentPart.Primary, brushes["CheckBoxCheckBackgroundFillChecked"]);
         Assert.Equal(DesignTokens.AccentPart.Primary, brushes["ToggleButtonBackgroundChecked"]);
         Assert.Equal(DesignTokens.AccentPart.Primary, brushes["ProgressRingForegroundThemeBrush"]);
         foreach (string theirs in new[] { "TextControlFocusedBorderBrush", "ComboBoxBorderBrushFocused", "HyperlinkButtonForeground" })
            Assert.False(brushes.ContainsKey(theirs), theirs + " is the focus or the link colour, not the accent.");

         DesignTokens.AccentArgb light = DesignTokens.Accent(ChartTheme.Light).Value;
         Assert.Equal(light.Primary, light.Of(DesignTokens.AccentPart.Fill, ChartTheme.Light));
         DesignTokens.AccentArgb dark = DesignTokens.Accent(ChartTheme.Dark).Value;
         Assert.Equal(dark.Secondary, dark.Of(DesignTokens.AccentPart.Fill, ChartTheme.Dark));

         string tokens = SaveStateModelTests.ReadControlPanelFile("Services/ThemeTokens.cs");
         Assert.Contains("Publish(key, FromArgb(accent.Of(part, theme)));", tokens, StringComparison.Ordinal);
         Assert.Contains("Application.Current.Resources.Remove(key);", tokens, StringComparison.Ordinal);
         Assert.Contains("WithdrawBrandAccent_();", tokens, StringComparison.Ordinal);
      }

      /// <summary>No blue but the brand's: no BrandBrush, and the tab underline and the title glyph in AppBrandBrush.</summary>
      [Fact]
      public void TheTabUnderlineAndTheTitleGlyph_AreTheBrand_AndBrandBrushIsGone()
      {
         string app = SaveStateModelTests.ReadControlPanelFile("App.xaml");
         string window = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");

         Assert.DoesNotContain("x:Key=\"BrandBrush\"", app, StringComparison.Ordinal);
         Assert.DoesNotContain("{StaticResource BrandBrush}", app + window, StringComparison.Ordinal);
         Assert.Contains("<Border x:Name=\"Underline\" Height=\"2.5\" CornerRadius=\"2\" Margin=\"2,0,2,-1\"", app, StringComparison.Ordinal);
         Assert.Contains("VerticalAlignment=\"Bottom\" Background=\"{DynamicResource AppBrandBrush}\"", app, StringComparison.Ordinal);
         Assert.Contains("<ui:SymbolIcon Symbol=\"Mail24\" Foreground=\"{DynamicResource AppBrandBrush}\" />", window, StringComparison.Ordinal);
      }
   }
}
