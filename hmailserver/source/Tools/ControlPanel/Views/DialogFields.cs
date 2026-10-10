// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// What the code-built dialogs share now that they sit on the standard frame:
   /// a field is a <see cref="FieldRow"/> around its editor, a note is a caption
   /// in the type ramp, a divider is the token hairline, and a message that only
   /// says a field is wrong goes on the field rather than into a message box.
   ///
   /// Seven dialogs carried a private copy of the same Label, Input, Combo,
   /// SelectCombo and ComboValue helpers; they now keep a one-line
   /// <c>Label</c> that forwards to <see cref="Field"/> - kept by that name
   /// because build/check-mnemonics.py reads every call of a caption-taking
   /// helper by that name for the Alt key it carries, and a caption the checker
   /// cannot see is a caption whose key can collide unnoticed.
   ///
   /// Two things here are a workaround for what the dialog base does not offer,
   /// and belong in it: <see cref="FitTabs"/>, because a window sized to its
   /// content resizes on every tab switch unless the tab control is sized to
   /// its tallest tab first; and <see cref="FramedDialog"/>, because
   /// <see cref="FluentDialogWindow.UseFrame"/> is protected and the static
   /// prompt helpers build a bare window.
   /// </summary>
   internal static class DialogFields
   {
      /// <summary>A field: the caption (with its Alt key) above the editor, the blurb under it.</summary>
      public static FieldRow Field(string caption, FrameworkElement editor, string hint = null)
         => new() { Label = caption, Content = editor, Hint = hint };

      /// <summary>
      /// A row whose value is read, not edited - the account's last sign-in, whether its
      /// mailbox is zero-access: the caption over the value with no Alt key, since there is
      /// no editor for a key to put the keyboard in (review finding F292).
      /// build/check-mnemonics.py holds such a caption, and its translations, to carrying none.
      /// </summary>
      public static FieldRow ReadOnlyField(string caption, FrameworkElement value, string hint = null)
         => new() { Label = caption, Content = value, Hint = hint };

      /// <summary>Adds the line under the editor after the row was built by a dialog's own <c>Label</c>.</summary>
      public static FieldRow WithHint(this FieldRow row, string hint)
      {
         row.Hint = hint;
         return row;
      }

      /// <summary>
      /// A check box in a run of them. A check box captions itself, so it needs
      /// no <see cref="FieldRow"/> around it; it is spaced by the small step
      /// rather than the field gap, so that four related boxes read as one group
      /// and not as four separate fields.
      /// </summary>
      public static CheckBox Check(CheckBox box)
      {
         box.Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm);
         return box;
      }

      /// <summary>A caption with no editor of its own - the heading over a run of check boxes.</summary>
      public static TextBlock Caption(string text)
      {
         var block = new TextBlock { Text = text, Margin = new Thickness(0, 0, 0, DesignTokens.Space.Xs) };
         block.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
         return block;
      }

      /// <summary>A sentence under a control saying what the setting actually does.</summary>
      public static TextBlock Note(string text)
      {
         var block = new TextBlock { Text = text, Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md) };
         block.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
         return block;
      }

      /// <summary>Body text in the ramp - a dialog's explanation, a status line.</summary>
      public static TextBlock Body(string text)
      {
         var block = new TextBlock { Text = text, Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md) };
         block.SetResourceReference(FrameworkElement.StyleProperty, "TextBody");
         return block;
      }

      /// <summary>The token hairline between two groups of fields.</summary>
      public static Border Separator()
      {
         var divider = new Border { Height = 1, Margin = new Thickness(0, DesignTokens.Space.Xs, 0, DesignTokens.Space.Lg) };
         divider.SetResourceReference(Border.BackgroundProperty, "AppDividerBrush");
         return divider;
      }

      /// <summary>The stack a tab's fields go in, set off from the tab strip by one step.</summary>
      public static StackPanel TabPanel() => new() { Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };

      /// <summary>
      /// A tab's content: the fields, scrolling when the tab is taller than the
      /// dialog allows, and kept clear of the bar while it shows - the bar is
      /// drawn over the content, and covered the right border of every box and
      /// the end of every hint on the domain dialog's Limits and DKIM tabs
      /// (Services/ScrollBarInset).
      /// </summary>
      public static ScrollViewer Scroll(UIElement content)
      {
         var viewer = new ScrollViewer
         {
            Content = content,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Focusable = false
         };
         ScrollBarInset.SetIsEnabled(viewer, true);
         return viewer;
      }

      /// <summary>A collapsed notice, shown by <see cref="Show"/> when there is something to say.</summary>
      public static InlineNotice Notice() => new() { Visibility = Visibility.Collapsed };

      public static void Show(this InlineNotice notice, StatusLevel level, string text)
      {
         notice.Level = level;
         notice.Text = text;
         notice.Visibility = Visibility.Visible;
      }

      public static void Hide(this InlineNotice notice) => notice.Visibility = Visibility.Collapsed;

      /// <summary>
      /// Puts a message on the field it is about, brings the tab that holds the
      /// field to the front, puts the keyboard in the editor and brings the field
      /// into view with its message - so a save that stops on a bad number lands
      /// the administrator on the number, where a message box left them on the
      /// Save button with a sentence to remember, and a field below the fold of a
      /// scrolled dialog is scrolled to.
      /// </summary>
      public static void ShowError(FieldRow row, string error)
      {
         row.Error = error;

         for (DependencyObject current = row; current != null; current = LogicalTreeHelper.GetParent(current))
         {
            if (current is TabItem tab && tab.Parent is TabControl tabs)
            {
               tabs.SelectedItem = tab;
               break;
            }
         }

         if (row.Content is not UIElement editor)
            return;

         // After layout: a tab just brought to the front has no visual yet.
         row.Dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(() =>
         {
            if (editor is Control control && control.Focusable)
               control.Focus();
            else
               editor.MoveFocus(new TraversalRequest(FocusNavigationDirection.First));
            row.BringIntoView();
         }));
      }

      public static void ClearErrors(params FieldRow[] rows)
      {
         foreach (FieldRow row in rows)
            row.Error = null;
      }

      /// <summary>
      /// After a form's checks have set the message of every row they refuse, so
      /// that every box at fault is marked on one press: brings the first such row
      /// into view and puts the keyboard in its editor, as <see cref="ShowError"/>
      /// does. Whether any row was refused.
      /// </summary>
      public static bool ShowFirstError(params FieldRow[] rows)
      {
         foreach (FieldRow row in rows)
         {
            if (row.Error != null)
            {
               ShowError(row, row.Error);
               return true;
            }
         }

         return false;
      }

      /// <summary>
      /// Sizes a tab control to its tallest tab, within what the dialog may be.
      /// A window sized to its content takes the height of the tab on screen,
      /// and would grow and shrink with every click on the strip; measured once
      /// here, every tab gets the same height and the shorter ones scroll nothing.
      /// The ceiling comes from <see cref="DialogLayout"/>, past which the tallest
      /// tab scrolls inside its own viewer rather than the dialog's.
      /// </summary>
      public static void FitTabs(TabControl tabs, double contentWidth)
      {
         var available = new Size(contentWidth, double.PositiveInfinity);
         tabs.Height = DialogLayout.TabControlHeight(Needed_(tabs, available),
            DialogLayout.MaxWindowHeight(SystemParameters.WorkArea.Height));
      }

      /// <summary>
      /// <see cref="FitTabs"/> for a dialog of many tabs - the domain and account
      /// dialogs, whose tabs range from three fields to a form of twenty and to
      /// whole embedded lists: as tall as its tallest tab, no taller than
      /// <see cref="DialogLayout.ManyTabsCap"/>, past which that tab scrolls, and
      /// no shorter than <see cref="DialogLayout.ListTabFloor"/>, which an embedded
      /// list needs. They took the ceiling the work area allows without measuring
      /// anything, 1,180 pixels on a large screen, General's three fields above
      /// 780 pixels of nothing and Save at the screen's foot (review finding F194).
      /// </summary>
      public static void FitManyTabs(TabControl tabs, double contentWidth)
      {
         var available = new Size(contentWidth, double.PositiveInfinity);
         tabs.Height = DialogLayout.TabControlHeight(Needed_(tabs, available),
            DialogLayout.MaxWindowHeight(SystemParameters.WorkArea.Height), DialogLayout.ListTabFloor, DialogLayout.ManyTabsCap);
      }

      /// <summary>
      /// What the tab control needs for its tallest tab and its strip, measured as
      /// the dialog will draw them. The control is not in the window yet, so it is
      /// given the window's font first (FluentDialogWindow's, which its tabs
      /// inherit through the logical tree): measured in the system's default font,
      /// the strip's row of headers came out two pixels shorter than it is drawn,
      /// and the tallest tab of the IP range and route dialogs scrolled by those
      /// two pixels under a bar drawn over its fields (review finding F192). Then
      /// rounded up, with four pixels for layout rounding: at 150 % each part of a
      /// tab is rounded up to a device pixel in the window, which the measure here
      /// cannot do, and the IP range dialog's General tab, measured at 282, was
      /// drawn 284 tall and scrolled by those two.
      /// </summary>
      private static double Needed_(TabControl tabs, Size available)
      {
         InDialogFont_(tabs);
         return Math.Ceiling(TallestTab_(tabs, available) + StripHeight_(tabs, available)) + DesignTokens.Space.Xs;
      }

      /// <summary>
      /// The tallest tab's content at the dialog's width. Each tab's content is
      /// measured on its own rather than through the tab control. Selecting a tab
      /// and measuring the control does not work here: the control is not in a tree
      /// yet, and the selected item reaches its content presenter on a later layout
      /// pass, so every measure reports the tab that was showing before - which is
      /// how the route editor's address list came out a third short and scrolled
      /// out of sight. The contents are ordinary elements this file built, and
      /// measuring one applies its template and the templates of the scaffold
      /// controls inside it.
      /// </summary>
      private static double TallestTab_(TabControl tabs, Size available)
      {
         double tallest = 0;
         foreach (object item in tabs.Items)
         {
            if (item is TabItem { Content: FrameworkElement content })
            {
               content.Measure(available);
               tallest = Math.Max(tallest, content.DesiredSize.Height);
            }
         }

         return tallest;
      }

      /// <summary>
      /// The strip above a tab's content: its row of headers and the divider under
      /// it. Taken from the headers themselves - a TabItem's template draws its
      /// header alone - as well as from the control, whose measure outside a tree
      /// can come before the selected tab reaches its content presenter, which
      /// gave a strip of nothing and a tallest tab that scrolled by the strip's
      /// height: the route dialog's Addresses tab and the German IP range dialog's
      /// General tab (review finding F192).
      /// </summary>
      private static double StripHeight_(TabControl tabs, Size available)
      {
         tabs.Measure(available);
         double strip = Math.Max(0, tabs.DesiredSize.Height - TallestSelectedContent_(tabs, available));

         double headers = 0;
         foreach (object item in tabs.Items)
         {
            if (item is TabItem header)
            {
               header.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
               headers = Math.Max(headers, header.DesiredSize.Height);
            }
         }

         // The divider: the strip's one-pixel bottom border in App.xaml.
         return Math.Max(strip, headers + 1);
      }

      private static double TallestSelectedContent_(TabControl tabs, Size available)
      {
         if (tabs.SelectedItem is TabItem { Content: FrameworkElement selected })
         {
            selected.Measure(available);
            return selected.DesiredSize.Height;
         }

         return 0;
      }

      /// <summary>
      /// The width a tabbed dialog asks the frame for so that every header of its
      /// strip is whole: <paramref name="preferred"/>, or wider where the headers in
      /// the language in force need it (Services/DialogLayout.WidthForStrip). Each
      /// header is measured on its own, as <see cref="FitTabs"/> measures the tabs.
      /// </summary>
      public static double WidthForTabs(TabControl tabs, double preferred)
      {
         InDialogFont_(tabs);
         double strip = 0;
         foreach (object item in tabs.Items)
         {
            if (item is TabItem { Visibility: Visibility.Visible } header)
            {
               header.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
               strip += header.DesiredSize.Width;
            }
         }

         return DialogLayout.WidthForStrip(strip, preferred);
      }

      /// <summary>
      /// Gives a tab control that is not in its window yet the window's font
      /// (FluentDialogWindow's), so that what is measured is what will be drawn.
      /// </summary>
      private static void InDialogFont_(TabControl tabs)
      {
         if (tabs.ReadLocalValue(Control.FontFamilyProperty) == DependencyProperty.UnsetValue)
            tabs.FontFamily = new System.Windows.Media.FontFamily(Typography.UiFontFamily);
      }

      /// <summary>The width the body of a dialog gets: the dialog's width less the frame's inset on both sides.</summary>
      public static double BodyWidth(double dialogWidth) => dialogWidth - 2 * DesignTokens.Space.Xl;

      public static ComboBoxItem Combo(string text, int value) => new() { Content = text, Tag = value };

      /// <summary>Selects the item tagged with the value; when none is, the first item if <paramref name="orFirst"/>.</summary>
      public static void SelectCombo(ComboBox combo, int value, bool orFirst = false)
      {
         foreach (ComboBoxItem item in combo.Items)
         {
            if ((int)item.Tag == value)
            {
               combo.SelectedItem = item;
               return;
            }
         }

         if (orFirst && combo.Items.Count > 0)
            combo.SelectedIndex = 0;
      }

      public static int ComboValue(ComboBox combo, int fallback = 0)
         => combo.SelectedItem is ComboBoxItem item ? (int)item.Tag : fallback;

      /// <summary>
      /// Fields that count only while a switch is on follow it: enabled while it is
      /// ticked, dimmed and out of the tab order while it is not, as the switch is
      /// ticked and unticked and as a dialog fills it (review findings F197 and
      /// F106). They were live under an unticked switch - a relay's user name and
      /// password with "The relay requires authentication" off, a domain's account
      /// limit with "Limit number of accounts" off - so that an edit there looked as
      /// if it did something. The values are left as they are and saved as they are,
      /// which is what the server keeps while the switch is off; only fields the
      /// server reads for nothing else follow a switch.
      ///
      /// The rows, not their editors, are disabled, so that what else decides an
      /// editor's IsEnabled - a server without the property, a key the environment
      /// sets - keeps deciding it; WPF enables a child only while its parent is.
      /// </summary>
      public static void EnabledBy(System.Windows.Controls.Primitives.ToggleButton toggle, params UIElement[] dependents)
         => EnabledWhenAll(new[] { toggle }, dependents);

      /// <summary>
      /// The same for fields that need every one of several switches - an auto-reply's
      /// expiry date, which counts only while the reply is on and set to stop.
      /// </summary>
      public static void EnabledWhenAll(System.Windows.Controls.Primitives.ToggleButton[] toggles, params UIElement[] dependents)
      {
         void Apply()
         {
            bool on = Array.TrueForAll(toggles, toggle => toggle.IsChecked == true);
            foreach (UIElement dependent in dependents)
            {
               if (dependent != null)
                  dependent.IsEnabled = on;
            }
         }

         foreach (System.Windows.Controls.Primitives.ToggleButton toggle in toggles)
         {
            toggle.Checked += (s, e) => Apply();
            toggle.Unchecked += (s, e) => Apply();
            toggle.Indeterminate += (s, e) => Apply();
         }

         Apply();
      }

      /// <summary>
      /// One line of text, asked for in a framed dialog. Null when cancelled. The
      /// caption names the box to a screen reader through the field row, which
      /// is what the two hand-built prompts this replaces had to do by hand.
      /// </summary>
      public static string PromptText(Window owner, string title, string caption, string initial = "")
      {
         var dialog = new FramedDialog { Owner = owner, Title = title };
         var box = new Wpf.Ui.Controls.TextBox { Text = initial ?? "" };
         string result = null;

         var ok = new Wpf.Ui.Controls.Button { Content = L("OK"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         ok.Click += (s, e) => { result = box.Text; dialog.DialogResult = true; dialog.Close(); };
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => dialog.Close();

         dialog.Frame(title, Field(caption, box), ok, cancel, width: 420);
         box.Loaded += (s, e) => box.SelectAll();
         return dialog.ShowDialog() == true ? result : null;
      }
   }

   /// <summary>
   /// A dialog whose content is put together outside a subclass - the prompt
   /// helpers - reaching the base class's frame through a public method.
   /// </summary>
   internal sealed class FramedDialog : FluentDialogWindow
   {
      public DialogFrame Frame(string heading, UIElement body, Button primary, Button secondary,
                               string description = null, double width = DesignTokens.Dialog.DefaultWidth)
         => UseFrame(heading, body, primary, secondary, description, width);
   }
}
