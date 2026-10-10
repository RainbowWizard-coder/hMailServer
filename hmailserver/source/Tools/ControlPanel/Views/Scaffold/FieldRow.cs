// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Shapes;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// One field of a form: the caption above the editor, the editor (the
   /// content), an optional validation message directly under it, and an
   /// optional hint under that. The caption carries an Alt-key mnemonic the way
   /// every caption here does ("_Host" is Alt+H), wired to the editor through
   /// <see cref="Mnemonic"/>; the editor takes its accessible name from the
   /// caption and its help text from the hint and the message, so a screen
   /// reader landing in the editor hears what it is, what it wants and what is
   /// wrong with it - the same three things a sighted user reads around it.
   ///
   /// The caption is body text, 14 pixels in the primary colour, as a check
   /// box's caption beside it is; the hint and the Applies line under the
   /// editor stay small and tertiary. The caption was 12-pixel secondary text
   /// over a 12-pixel hint four points of alpha lighter, so that a field's name
   /// could not be told from the paragraph under it and ranked below a check
   /// box's (review finding F146).
   ///
   /// The message is drawn with the danger colour, a cross and the words
   /// themselves, directly under the editor, where it sat under the hint as
   /// much as 50 pixels from the box; it is an assertive live region, and the
   /// row raises the live-region event on it (<see cref="Announce"/>) as the
   /// message is set, so it is announced when it appears. While a message is
   /// set the editor is marked refused as well (<see cref="HasErrorProperty"/>):
   /// its border at rest is the danger colour, and FocusGlow draws its ring in
   /// that colour, where the box kept its neutral border and the violet ring
   /// because FocusGlow read only Validation.HasError, which a message set here
   /// never sets (review finding F213). An edit of the value takes the message
   /// away - it said what was wrong with the value it was given - typed or put
   /// in by code, as a generated password is, as does setting
   /// <see cref="Error"/> to null.
   ///
   /// <code>
   /// &lt;scaffold:FieldRow Label="{loc:L '_Host'}" Hint="{loc:L 'The name or address of the server.'}"&gt;
   ///    &lt;ui:TextBox x:Name="HostBox" /&gt;
   /// &lt;/scaffold:FieldRow&gt;
   /// </code>
   /// </summary>
   public class FieldRow : ScaffoldContentControl
   {
      static FieldRow()
      {
         DefaultStyleKeyProperty.OverrideMetadata(typeof(FieldRow), new FrameworkPropertyMetadata(typeof(FieldRow)));
      }

      public FieldRow()
      {
         // An edit of the editor's value takes the message away. Routed events
         // from the content bubble through the row: a text box's or a number
         // box's text, a password box's password and a combo box's choice.
         // Every password box here is a PasswordField, WPF-UI's, whose
         // PasswordChanged is its own routed event and not WPF's: it is raised
         // for a password typed and for one put in by code - Generate's on the
         // Domains page - where a password put in by code raises no
         // TextChanged, so that "Enter a password, or press Generate." stayed
         // under a box Generate had filled. WPF's own password box keeps its
         // handler, for a row that holds one.
         AddHandler(TextBoxBase.TextChangedEvent, new TextChangedEventHandler(OnEdited_));
         AddHandler(Wpf.Ui.Controls.PasswordBox.PasswordChangedEvent, new RoutedEventHandler(OnEdited_));
         AddHandler(PasswordBox.PasswordChangedEvent, new RoutedEventHandler(OnEdited_));
         AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler(OnEdited_));
      }

      public static readonly DependencyProperty LabelProperty = DependencyProperty.Register(
         nameof(Label), typeof(string), typeof(FieldRow), new PropertyMetadata(null, OnAnyChanged, NullWhenEmpty));

      public static readonly DependencyProperty HintProperty = DependencyProperty.Register(
         nameof(Hint), typeof(string), typeof(FieldRow), new PropertyMetadata(null, OnAnyChanged, NullWhenEmpty));

      public static readonly DependencyProperty ErrorProperty = DependencyProperty.Register(
         nameof(Error), typeof(string), typeof(FieldRow), new PropertyMetadata(null, OnAnyChanged, NullWhenEmpty));

      public static readonly DependencyProperty AppliesProperty = DependencyProperty.Register(
         nameof(Applies), typeof(string), typeof(FieldRow), new PropertyMetadata(null, OnAnyChanged, NullWhenEmpty));

      public static readonly DependencyProperty LabelGroupProperty = DependencyProperty.Register(
         nameof(LabelGroup), typeof(string), typeof(FieldRow), new PropertyMetadata(null, OnLabelGroupChanged, NullWhenEmpty));

      /// <summary>
      /// Set on an editor while the row around it carries a message: what
      /// FocusGlow reads for the danger ring, beside Validation.HasError, and
      /// what a test or an automation client can read for "this field was
      /// refused". The row sets and clears it; nothing else needs to.
      /// </summary>
      public static readonly DependencyProperty HasErrorProperty = DependencyProperty.RegisterAttached(
         "HasError", typeof(bool), typeof(FieldRow), new PropertyMetadata(false));

      public static bool GetHasError(DependencyObject element) => (bool)element.GetValue(HasErrorProperty);

      public static void SetHasError(DependencyObject element, bool value) => element.SetValue(HasErrorProperty, value);

      // Whether the row put the danger colour on the editor's border, so that it
      // takes away only a colour it set and never one the page gave the box.
      private static readonly DependencyProperty OwnsBorderProperty = DependencyProperty.RegisterAttached(
         "OwnsBorder", typeof(bool), typeof(FieldRow), new PropertyMetadata(false));

      // The editor this row marked refused, so that a cleared message or a new
      // editor unmarks the one that was marked.
      private FrameworkElement marked_;

      /// <summary>The caption, with its Alt key marked by an underscore.</summary>
      public string Label
      {
         get => (string)GetValue(LabelProperty);
         set => SetValue(LabelProperty, value);
      }

      /// <summary>What the field wants, in one line, shown under the editor and its message.</summary>
      public string Hint
      {
         get => (string)GetValue(HintProperty);
         set => SetValue(HintProperty, value);
      }

      /// <summary>Why the value is not acceptable, or null when it is.</summary>
      public string Error
      {
         get => (string)GetValue(ErrorProperty);
         set => SetValue(ErrorProperty, value);
      }

      /// <summary>
      /// When a saved value of this field reaches the running server - "Applies on
      /// save", "After reinitialise" or "After a restart" (Services/SettingApplies) -
      /// for a field kept in the settings store that was hMailServer.ini's [Settings]
      /// section, or null for every other field. Shown small under the editor and
      /// read with its help text, so a screen reader hears it with the field.
      /// </summary>
      public string Applies
      {
         get => (string)GetValue(AppliesProperty);
         set => SetValue(AppliesProperty, value);
      }

      /// <summary>
      /// For fields laid beside each other on one line - an inline Add form: the
      /// name of a group whose captions share one height, so that the editors
      /// line up whatever a caption wraps to, and a message under one editor
      /// moves none of the others. Every row of the line names the same group,
      /// a button beside them sits under a row of that group (a grid row whose
      /// SharedSizeGroup is the name), and the panel around them sets
      /// Grid.IsSharedSizeScope. The rows of such a line sat on its bottom edge,
      /// so a message under one box lifted that box above its neighbours and put
      /// the button beside the message. Null, the caption takes its own height.
      /// </summary>
      public string LabelGroup
      {
         get => (string)GetValue(LabelGroupProperty);
         set => SetValue(LabelGroupProperty, value);
      }

      protected override AutomationControlType ControlType => AutomationControlType.Group;

      /// <summary>
      /// The line the message is drawn in, its cross and its words, while a
      /// message is set; null while none is. For a dialog that keeps room for its
      /// messages (FluentDialogWindow.KeepRoomForFieldErrors), which takes the
      /// body's height without them.
      /// </summary>
      internal FrameworkElement ShownMessage => Error != null ? Part<FrameworkElement>("PART_ErrorRow") : null;

      /// <summary>
      /// The height one line of a message takes under an editor, as the template
      /// draws it - the caption size's line and the gap above it - for a dialog
      /// that keeps room for its messages (FluentDialogWindow.KeepRoomForFieldErrors).
      /// </summary>
      public static double ErrorLineHeight(FrameworkElement scope)
      {
         double size = scope?.TryFindResource("AppFontSizeCaption") is double caption ? caption : Typography.Caption;
         FontFamily family = scope == null ? new FontFamily(Typography.UiFontFamily) : System.Windows.Documents.TextElement.GetFontFamily(scope);
         return Math.Ceiling(size * family.LineSpacing) + DesignTokens.Space.Xs;
      }

      public override void OnApplyTemplate()
      {
         base.OnApplyTemplate();

         // The mark in the shade of the message beside it (Scaffold.xaml): the
         // critical level's fill-text shade, which a message on a card needs -
         // the dark theme's danger colour measured 4.1:1 there.
         StatusPresentation critical = StatusSemantics.For(StatusLevel.Critical);
         if (Part<Path>("PART_ErrorMark") is { } mark)
            ShapeMarkVisuals.ApplyMark(mark, critical.Shape, critical.FillTextBrushKey);

         ApplyLabelGroup_();
         Apply_();
      }

      protected override void OnContentChanged(object oldContent, object newContent)
      {
         base.OnContentChanged(oldContent, newContent);
         Apply_();
      }

      private static void OnAnyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
      {
         var row = (FieldRow)d;
         row.Apply_();

         // The message is the template's own live region: announced as it is
         // set, after the layout that shows it. Clearing it says nothing.
         if (e.Property == ErrorProperty && e.NewValue != null && row.Part<TextBlock>("PART_Error") is { } message)
            Announce.Request(message);
      }

      private static void OnLabelGroupChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => ((FieldRow)d).ApplyLabelGroup_();

      // The caption's line is the first row of the template's grid; in a group
      // it is as tall as the tallest caption of the group's rows in the scope.
      private void ApplyLabelGroup_()
      {
         if (Part<Grid>("PART_Lines") is { } lines && lines.RowDefinitions.Count > 0)
            lines.RowDefinitions[0].SharedSizeGroup = LabelGroup;
      }

      private void OnEdited_(object sender, RoutedEventArgs e)
      {
         if (Error != null && e.OriginalSource is DependencyObject source && IsInContent_(source))
            Error = null;
      }

      // An edit inside this row's own editor - not one of a row nested in it,
      // which clears its own.
      private bool IsInContent_(DependencyObject source)
      {
         for (DependencyObject at = source; at != null; at = at is Visual ? VisualTreeHelper.GetParent(at) ?? LogicalTreeHelper.GetParent(at) : LogicalTreeHelper.GetParent(at))
         {
            if (ReferenceEquals(at, this))
               return true;
            if (at is FieldRow)
               return false;
         }

         return false;
      }

      private void Apply_()
      {
         string label = Label ?? "";
         var editor = Content as FrameworkElement;

         AutomationProperties.SetName(this, MnemonicText.Strip(label));

         if (Part<TextBlock>("PART_Label") is { } caption)
            Mnemonic.Apply(caption, label, editor);

         if (editor == null)
         {
            Mark_(null);
            return;
         }

         // A row of a box and a button beside it - a file and its Browse… - names
         // and describes the box, which a screen reader lands on, where it named
         // the panel around the two, which nothing lands on, and the box had no
         // name but one the page set by hand (Mnemonic.Editor, which the
         // caption's key reaches as well).
         if (Mnemonic.Editor(editor, reachable: false) is FrameworkElement inner)
            editor = inner;

         // The editor's own name and help, unless the page gave it a name of its
         // own - a page that already says something more exact keeps it.
         if (string.IsNullOrEmpty(AutomationProperties.GetName(editor)) && label.Length > 0)
            AutomationProperties.SetName(editor, AccessibleNames.Qualify(MnemonicText.Strip(label), ""));

         string help = Loc.Sentences(Hint, Applies, Error);

         AutomationProperties.SetHelpText(editor, help);

         Mark_(Error != null ? editor : null);
      }

      /// <summary>
      /// Marks the editor refused, and the one marked before not: HasError for
      /// FocusGlow's ring, and the danger colour on the border at rest, by its
      /// resource key so that a change of theme recolours it. A border the page
      /// set on the box itself is left as it is.
      /// </summary>
      private void Mark_(FrameworkElement editor)
      {
         if (ReferenceEquals(editor, marked_))
            return;

         if (marked_ != null)
         {
            marked_.ClearValue(HasErrorProperty);
            if ((bool)marked_.GetValue(OwnsBorderProperty))
            {
               marked_.ClearValue(Control.BorderBrushProperty);
               marked_.ClearValue(OwnsBorderProperty);
            }
         }

         marked_ = editor;
         if (editor == null)
            return;

         SetHasError(editor, true);
         if (editor is Control box && box.ReadLocalValue(Control.BorderBrushProperty) == DependencyProperty.UnsetValue)
         {
            box.SetResourceReference(Control.BorderBrushProperty, "AppDangerBrush");
            box.SetValue(OwnsBorderProperty, true);
         }
      }
   }
}
