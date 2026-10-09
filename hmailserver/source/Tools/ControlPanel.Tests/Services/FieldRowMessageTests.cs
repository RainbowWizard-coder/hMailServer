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
   /// A field's caption, its message and its editor, as FieldRow draws them
   /// (review findings F146 and F213). The caption was TextCaption - 12 pixels in
   /// the secondary colour - over a 12-pixel tertiary hint four points of alpha
   /// lighter, so that "Void SPF lookups allowed before permerror" could not be
   /// told from the paragraph under it, and ranked below a check box's 14-pixel
   /// caption on the same card. A message sat under the hint, as much as 50
   /// pixels from its box, which kept its neutral border and FocusGlow's violet
   /// ring, since FocusGlow read only Validation.HasError and a FieldRow's message
   /// never set it; and the dialog grew by every message, moving its footer. The
   /// test project has no WPF, so these hold the template and the code to the
   /// shape that draws the field as it should be drawn.
   /// </summary>
   public class FieldRowMessageTests
   {
      [Fact]
      public void TheCaption_IsBodyText_AndTheHintStaysSmall()
      {
         string template = FieldRowTemplate();
         Match label = Regex.Match(template, @"<TextBlock x:Name=""PART_Label""[^>]*>", RegexOptions.Singleline);
         Assert.True(label.Success, "The FieldRow template has no PART_Label.");
         Assert.Contains("Style=\"{StaticResource TextBody}\"", label.Value, StringComparison.Ordinal);

         foreach (string part in new[] { "PART_Hint", "PART_Applies" })
         {
            Match line = Regex.Match(template, @"<TextBlock x:Name=""" + part + @"""[^>]*>", RegexOptions.Singleline);
            Assert.True(line.Success, "The FieldRow template has no " + part + ".");
            Assert.Contains("Style=\"{StaticResource TextCaptionTertiary}\"", line.Value, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void TheMessage_SitsDirectlyUnderTheEditor_BeforeTheAppliesLineAndTheHint()
      {
         string template = FieldRowTemplate();
         int label = template.IndexOf("x:Name=\"PART_Label\"", StringComparison.Ordinal);
         int editor = template.IndexOf("<ContentPresenter", StringComparison.Ordinal);
         int error = template.IndexOf("x:Name=\"PART_ErrorRow\"", StringComparison.Ordinal);
         int applies = template.IndexOf("x:Name=\"PART_Applies\"", StringComparison.Ordinal);
         int hint = template.IndexOf("x:Name=\"PART_Hint\"", StringComparison.Ordinal);
         Assert.True(label >= 0 && editor > label && error > editor && applies > error && hint > applies,
            "The FieldRow's parts are not caption, editor, message, Applies line, hint.");

         // In the grid's rows in that order, so that the order drawn is the order read.
         Assert.Matches(new Regex(@"<ContentPresenter Grid\.Row=""1"""), template);
         Assert.Matches(new Regex(@"x:Name=""PART_ErrorRow"" Grid\.Row=""2"""), template);
         Assert.Matches(new Regex(@"x:Name=""PART_Applies"" Grid\.Row=""3"""), template);
         Assert.Matches(new Regex(@"x:Name=""PART_Hint"" Grid\.Row=""4"""), template);
      }

      [Fact]
      public void AMessage_MarksTheEditor_AndAnEditTakesItAway()
      {
         string row = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FieldRow.cs"));

         // The attached flag FocusGlow reads, set on the editor while a message is
         // set and cleared with it, and the danger colour on the border at rest, by
         // its key, only where the page set no border of its own.
         Assert.Contains("DependencyProperty.RegisterAttached(\n         \"HasError\"", Normalise(row), StringComparison.Ordinal);
         Assert.Contains("Mark_(Error != null ? editor : null);", row, StringComparison.Ordinal);
         Assert.Contains("SetHasError(editor, true);", row, StringComparison.Ordinal);
         Assert.Contains("marked_.ClearValue(HasErrorProperty);", row, StringComparison.Ordinal);
         Assert.Contains("box.SetResourceReference(Control.BorderBrushProperty, \"AppDangerBrush\");", row, StringComparison.Ordinal);
         Assert.Contains("box.ReadLocalValue(Control.BorderBrushProperty) == DependencyProperty.UnsetValue", row, StringComparison.Ordinal);

         // An edit of the value - text, password, a choice - clears the message.
         Assert.Contains("AddHandler(TextBoxBase.TextChangedEvent", row, StringComparison.Ordinal);
         Assert.Contains("AddHandler(PasswordBox.PasswordChangedEvent", row, StringComparison.Ordinal);
         Assert.Contains("AddHandler(Selector.SelectionChangedEvent", row, StringComparison.Ordinal);
         Assert.Contains("Error = null;", row, StringComparison.Ordinal);
      }

      [Fact]
      public void FocusGlow_RingsARefusedFieldInTheDangerColour()
      {
         string glow = ReadControlPanelFile(Path.Join("Services", "FocusGlow.cs"));
         Assert.Contains("Validation.GetHasError(field) || Views.Scaffold.FieldRow.GetHasError(field)", glow, StringComparison.Ordinal);
         Assert.Contains("Color ring = Refused_(field)", glow, StringComparison.Ordinal);

         // Redrawn as the mark comes and goes while the field has the keyboard.
         Assert.Contains("RefusedDescriptor.AddValueChanged(field, OnHasErrorChanged_);", glow, StringComparison.Ordinal);
         Assert.Contains("RefusedDescriptor.RemoveValueChanged(field, OnHasErrorChanged_);", glow, StringComparison.Ordinal);
      }

      [Fact]
      public void FieldsOnOneLine_ShareTheirCaptionsHeight()
      {
         string template = FieldRowTemplate();
         Assert.Contains("<Grid x:Name=\"PART_Lines\">", template, StringComparison.Ordinal);

         string row = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FieldRow.cs"));
         Assert.Contains("lines.RowDefinitions[0].SharedSizeGroup = LabelGroup;", row, StringComparison.Ordinal);
      }

      /// <summary>
      /// The three dialogs whose renders showed the footer moved by a message keep
      /// a line of room for it: the body's floor is raised by it once the window
      /// has sized itself, and the window stays centred as it was placed.
      /// </summary>
      [Theory]
      [InlineData("DomainPolicyListView.cs")]
      [InlineData("ServerSettingsView.NeverBanNames.cs")]
      [InlineData("SuppressionListView.cs")]
      public void ADialogWithARefusableField_KeepsRoomForItsMessage(string view)
      {
         string source = ReadControlPanelFile(Path.Join("Views", view));
         Assert.Contains("dialog.KeepRoomForFieldErrors();", source, StringComparison.Ordinal);

         string window = Normalise(ReadControlPanelFile(Path.Join("Views", "FluentDialogWindow.cs")));
         Assert.Contains("public void KeepRoomForFieldErrors(int lines = 1)", window, StringComparison.Ordinal);
         Assert.Contains("double room = lines * FieldRow.ErrorLineHeight(this);", window, StringComparison.Ordinal);
         Assert.Contains("body.MinHeight = body.ActualHeight + room;", window, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static string FieldRowTemplate()
      {
         string scaffold = Normalise(ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml")));
         int start = scaffold.IndexOf("<Style TargetType=\"{x:Type scaffold:FieldRow}\">", StringComparison.Ordinal);
         Assert.True(start >= 0, "Scaffold.xaml has no FieldRow style.");
         int end = scaffold.IndexOf("</Style>", start, StringComparison.Ordinal);
         return scaffold.Substring(start, end - start);
      }

      private static string Normalise(string text) => text.Replace("\r\n", "\n");

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
