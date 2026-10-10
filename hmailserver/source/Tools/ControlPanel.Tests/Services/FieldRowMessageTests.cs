// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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

      /// <summary>
      /// A password put in by code takes the message away as a typed one does. Every
      /// password box of the Control Panel is a PasswordField, which is WPF-UI's
      /// PasswordBox, and WPF-UI raises its own routed PasswordChanged - not WPF's,
      /// which only WPF's own password box raises - for a password typed and for one
      /// set in code, with no TextChanged for the latter: the row listened for WPF's
      /// alone, so that Generate on the Domains page filled the password and left
      /// "Enter a password, or press Generate." under it, the box still marked refused.
      /// </summary>
      [Fact]
      public void APasswordPutInByCode_TakesTheMessageAway()
      {
         string row = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FieldRow.cs"));
         Assert.Contains("AddHandler(Wpf.Ui.Controls.PasswordBox.PasswordChangedEvent, new RoutedEventHandler(OnEdited_));", row, StringComparison.Ordinal);

         string field = ReadControlPanelFile(Path.Join("Views", "PasswordField.cs"));
         Assert.Contains("public class PasswordField : Wpf.Ui.Controls.PasswordBox", field, StringComparison.Ordinal);

         // Generate puts its password in by code, in the box of a FieldRow.
         string domains = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"));
         Assert.Matches(new Regex(@"<scaffold:FieldRow x:Name=""PasswordRow""[^>]*>\s*<local:PasswordField x:Name=""NewAccountPassword"" />"), domains);
         string code = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         Assert.Contains("NewAccountPassword.Password = pw;", code, StringComparison.Ordinal);
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
      /// The dialogs whose fields can be refused keep room for the messages: a
      /// line for each field a press can refuse, or for each line the longest
      /// message wraps to; the access-control entry for its account or group box
      /// alone, which Anyone collapses.
      /// </summary>
      [Theory]
      [InlineData("DomainPolicyListView.cs", "dialog.KeepRoomForFieldErrors();")]
      [InlineData("ServerSettingsView.NeverBanNames.cs", "dialog.KeepRoomForFieldErrors();")]
      [InlineData("SuppressionListView.cs", "dialog.KeepRoomForFieldErrors();")]
      [InlineData("FieldDialog.cs", "KeepRoomForFieldErrors(Math.Max(1, fields.Count(f => f.Required)));")]
      [InlineData("DistributionListDialog.cs", "KeepRoomForFieldErrors(3);")]
      [InlineData("FolderPermissionsDialog.cs", "KeepRoomForFieldErrors(1, subjectRow_);")]
      [InlineData("TotpSetupDialog.cs", "KeepRoomForFieldErrors(2, codeRow_);")]
      [InlineData("AdministratorTwoFactorDialog.cs", "KeepRoomForFieldErrors(2, codeRow_);")]
      [InlineData("AdministratorPasswordDialog.cs", "KeepRoomForFieldErrors();")]
      [InlineData("ApiKeysView.cs", "dlg.KeepRoomForFieldErrors(2);")]
      [InlineData("TcpIpPortDialog.cs", "KeepRoomForFieldErrors(1, portRow_);")]
      [InlineData("PublicFolderNameDialog.cs", "KeepRoomForFieldErrors(2, nameRow_);")]
      [InlineData("MailboxMoveDialog.cs", "KeepRoomForFieldErrors(1, newAddressRow_);")]
      [InlineData("RuleCriteriaDialog.cs", "KeepRoomForFieldErrors(2);")]
      [InlineData("IncomingRelayDialog.cs", "dialog.KeepRoomForFieldErrors(3);")]
      public void ADialogWithARefusableField_KeepsRoomForItsMessage(string view, string call)
      {
         string source = ReadControlPanelFile(Path.Join("Views", view));
         Assert.Contains(call, source, StringComparison.Ordinal);
      }

      /// <summary>
      /// The rule docs/ControlPanelDesign.md states, held for every dialog on the
      /// frame: one that says a message under a field keeps room for it, or fits
      /// its tabs to a height of their own, which a message inside a tab does not
      /// change. It was applied to the three dialogs whose renders had shown the
      /// footer move and to the unit's own, and the two-factor setup grew from 547
      /// to 583 pixels on a wrong code, Enable two-factor authentication and Close
      /// 36 pixels lower; the administrator's second factor and password, the API
      /// key's Create, the port binding and the mailbox move did the same.
      /// </summary>
      [Fact]
      public void EveryDialogOnTheFrameThatRefusesAField_KeepsRoomForTheMessage()
      {
         var missing = new List<string>();
         foreach (string file in Directory.GetFiles(ControlPanelDirectory("Views"), "*.cs", SearchOption.AllDirectories))
         {
            string name = Path.GetFileName(file);
            if (name is "DialogFields.cs" or "FluentDialogWindow.cs")
               continue;

            string source = File.ReadAllText(file);
            bool framed = source.Contains("UseFrame(", StringComparison.Ordinal) || source.Contains(".Frame(", StringComparison.Ordinal);
            bool refuses = Regex.IsMatch(source, @"\.Error = [LF]\(|DialogFields\.ShowError\(|DialogFields\.ShowFirstError\(");
            bool keeps = source.Contains("KeepRoomForFieldErrors(", StringComparison.Ordinal)
               || source.Contains("DialogFields.FitTabs(", StringComparison.Ordinal)
               || source.Contains("DialogFields.FitManyTabs(", StringComparison.Ordinal)
               || source.Contains("DialogFields.FillTabs(", StringComparison.Ordinal);
            if (framed && refuses && !keeps)
               missing.Add(name);
         }

         Assert.True(missing.Count == 0,
            "These dialogs say a message under a field and keep no room for it, so that the message moves their buttons (FluentDialogWindow.KeepRoomForFieldErrors): "
            + string.Join(", ", missing));
      }

      /// <summary>
      /// The room is the floor of the presenter that holds the body, taken again
      /// whenever the body's height changes, from its height without the messages
      /// shown in it (Services/FieldMessageRoom), so that a row shown or collapsed
      /// grows or shrinks the dialog and only a message is kept from moving the
      /// footer. The body's own floor, raised once at Loaded, kept the
      /// access-control entry as tall as with its account box after Anyone
      /// collapsed it. The window stays centred as it was placed, and lets go of
      /// the layout manager's event when it closes.
      /// </summary>
      [Fact]
      public void TheRoom_FollowsTheBody()
      {
         string window = Normalise(ReadControlPanelFile(Path.Join("Views", "FluentDialogWindow.cs")));
         Assert.Contains("public void KeepRoomForFieldErrors(int lines = 1, params FieldRow[] refusable)", window, StringComparison.Ordinal);
         Assert.Contains("VisualTreeHelper.GetParent(body) is not FrameworkElement holder", window, StringComparison.Ordinal);
         Assert.Contains("lines * FieldRow.ErrorLineHeight(this)", window, StringComparison.Ordinal);
         Assert.Contains("ActualHeight - scroller.ActualHeight", window, StringComparison.Ordinal);
         Assert.Contains("Top -= grows / 2;", window, StringComparison.Ordinal);
         Assert.Contains("body.LayoutUpdated += follow;", window, StringComparison.Ordinal);
         Assert.Contains("Closed += (_, _) => body.LayoutUpdated -= follow;", window, StringComparison.Ordinal);
         Assert.Contains("FieldMessageRoom.Take(height, FieldMessageRoom.Covered(spans), room_, window_.MaxHeight, chrome_, refusableShown)", window, StringComparison.Ordinal);
         Assert.Contains("holder_.MinHeight = floor;", window, StringComparison.Ordinal);
         Assert.DoesNotContain("body.MinHeight", window, StringComparison.Ordinal);

         // A collapsed element's messages and rows are not counted.
         Assert.Contains("if (at is UIElement element && element.Visibility == Visibility.Collapsed)\n               return;", window, StringComparison.Ordinal);

         string row = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FieldRow.cs"));
         Assert.Contains("internal FrameworkElement ShownMessage => Error != null ? Part<FrameworkElement>(\"PART_ErrorRow\") : null;", row, StringComparison.Ordinal);
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

      private static string ControlPanelDirectory(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (Directory.Exists(candidate) && File.Exists(Path.Join(directory.FullName, "ControlPanel", "Views", "FieldDialog.cs")))
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
