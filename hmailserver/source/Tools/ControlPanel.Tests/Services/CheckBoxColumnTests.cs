// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A check box stands on the text column (review finding F294): the square stood 10
   /// to 11 pixels right of the column its card's captions, boxes and hints share - 336
   /// against 325 on a settings page, 35 against 25 in a dialog - because WPF-UI's
   /// padding, 11,5,11,6, is the bullet's margin; the hint under a box started at the
   /// row's edge, under neither the box nor its caption; the backup page's two options
   /// under Back up messages stood 22 pixels in, under neither its box nor its caption;
   /// and a permission whose caption wrapped on the IP ranges page pushed its box off
   /// its row's line. The caption starts 28 pixels in from the box - the box and its gap
   /// - and the lines under a box start there.
   /// </summary>
   public class CheckBoxColumnTests
   {
      [Fact]
      public void TheImplicitCheckBoxStyle_PutsTheBoxOnTheTextColumn_AndTheCaptionAtTheTop()
      {
         string app = SaveStateModelTests.ReadControlPanelFile("App.xaml");
         Match style = Regex.Match(app, "<Style TargetType=\"CheckBox\" BasedOn=\"\\{StaticResource DefaultCheckBoxStyle\\}\">(.*?)</Style>", RegexOptions.Singleline);
         Assert.True(style.Success, "App.xaml no longer has the implicit CheckBox style based on WPF-UI's.");
         Assert.Contains("<Setter Property=\"Padding\" Value=\"0,5,11,6\" />", style.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"VerticalContentAlignment\" Value=\"Top\" />", style.Groups[1].Value, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCaptionInset_IsTheBoxAndItsGap_InCodeAndInTokensXaml()
      {
         // WPF-UI's CheckBoxWidth (20) and CheckBoxContentMargin (8,0,0,0); its radio
         // button's circle is 20 with the same margin.
         Assert.Equal(28, DesignTokens.CheckBoxCaptionInset);

         string tokens = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/Tokens.xaml");
         Assert.Contains("<Thickness x:Key=\"AppCheckBoxIndent\">28,0,0,0</Thickness>", tokens, StringComparison.Ordinal);
      }

      /// <summary>A FieldRow whose editor is a check box starts its message, Applies line and hint under the caption.</summary>
      [Fact]
      public void AFieldRowOfACheckBox_StartsItsLinesUnderTheCaption()
      {
         string row = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/FieldRow.cs");
         Assert.Contains("editor is ToggleButton { Content: not null } toggle && (toggle is CheckBox || toggle is RadioButton)", row, StringComparison.Ordinal);
         Assert.Contains("double inset = CaptionsItself(Content) ? DesignTokens.CheckBoxCaptionInset : 0;", row, StringComparison.Ordinal);
         Assert.Contains("new[] { \"PART_ErrorRow\", \"PART_Applies\", \"PART_Hint\" }", row, StringComparison.Ordinal);
         Assert.Contains("line.Margin = new Thickness(inset, DesignTokens.Space.Xs, 0, 0);", row, StringComparison.Ordinal);

         // Every time the row is applied, so that a new editor moves its lines.
         int apply = row.IndexOf("private void Apply_()", StringComparison.Ordinal);
         Assert.True(apply >= 0 && row.IndexOf("IndentLines_();", apply, StringComparison.Ordinal) - apply < 80, "Apply_ no longer indents the lines first.");
      }

      /// <summary>A dialog's note laid under a check box starts under its caption.</summary>
      [Fact]
      public void ADialogsNoteUnderACheckBox_StartsUnderItsCaption()
      {
         string fields = SaveStateModelTests.ReadControlPanelFile("Views/DialogFields.cs");
         Assert.Contains("block.Loaded += (_, _) => UnderItsCheckBox_(block);", fields, StringComparison.Ordinal);
         Assert.Contains("? box.Margin.Left + DesignTokens.CheckBoxCaptionInset", fields, StringComparison.Ordinal);
      }

      /// <summary>The options and lines laid under a box by hand use the inset, never a number of their own.</summary>
      [Fact]
      public void TheLinesLaidUnderABoxByHand_TakeTheInset()
      {
         string backup = SaveStateModelTests.ReadControlPanelFile("Views/BackupView.xaml");
         Assert.Contains("<StackPanel Margin=\"{StaticResource AppCheckBoxIndent}\">", backup, StringComparison.Ordinal);
         Assert.DoesNotContain("Margin=\"22,0,0,12\"", backup, StringComparison.Ordinal);

         string sync = SaveStateModelTests.ReadControlPanelFile("Views/DirectorySyncView.cs");
         Assert.Contains("Margin = new Thickness(DesignTokens.CheckBoxCaptionInset, 0, 0, 16)", sync, StringComparison.Ordinal);

         string levels = SaveStateModelTests.ReadControlPanelFile("Views/SettingLevelsPanel.cs");
         Assert.Contains("Margin = new Thickness(DesignTokens.CheckBoxCaptionInset, 2, 0, 2)", levels, StringComparison.Ordinal);
         Assert.Contains("effect.Margin = new Thickness(DesignTokens.CheckBoxCaptionInset, 0, 0, 0);", levels, StringComparison.Ordinal);
         Assert.DoesNotContain("new Thickness(24, ", levels, StringComparison.Ordinal);
      }

      [Fact]
      public void TheIpRangesPermissions_KeepTheirBoxesOnTheirRowsLine()
      {
         string ranges = SaveStateModelTests.ReadControlPanelFile("Views/IPRangesView.xaml");
         Match style = Regex.Match(ranges, "<UniformGrid.Resources>(.*?)</UniformGrid.Resources>", RegexOptions.Singleline);
         Assert.True(style.Success, "IPRangesView.xaml no longer styles the permission boxes in the grid's resources.");
         Assert.Contains("<Setter Property=\"VerticalAlignment\" Value=\"Top\" />", style.Groups[1].Value, StringComparison.Ordinal);
      }
   }
}
