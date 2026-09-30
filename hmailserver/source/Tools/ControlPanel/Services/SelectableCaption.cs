// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Media3D;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The words on the controls themselves can be copied: a check box's or a radio
   /// button's caption, a button's (and so a toggle's, a link button's, a card that
   /// acts as a button, a grid's column header), the value a drop-down shows, a tab's
   /// name, a page's name in the navigation tree and a label's words. A right-click on
   /// one opens a menu with Copy, the same "Copy" as the menu on a piece of text, and
   /// Ctrl+C on one that has the keyboard copies the same; either puts the caption on
   /// the clipboard as plain text, as it is shown (an access key's underscore is not
   /// shown, so it is not copied). Issue #48's third round: most of Logging's and API
   /// &amp; monitoring's settings are check boxes, and <see cref="SelectableText"/>
   /// leaves the words of anything that is clicked to the control, so a press stays a
   /// press - the caption of nearly every setting on those pages could still not be
   /// copied.
   ///
   /// So the caption is copied whole, from a menu, rather than selected: a press on a
   /// check box has to toggle it, and a drag over a button's words has to stay a press
   /// on the button. The left button and every key the control has today do what they
   /// did - a right-click presses, toggles, selects and opens nothing in WPF - and
   /// nothing is added to a control, so the automation tree (names, types, focus, tab
   /// order) is what it was; the menu is WPF's own, so the Menu key and Shift+F10 open
   /// it on the control that has the keyboard and a screen reader announces its item.
   /// Registered once at start-up by class handlers, as SelectableText and
   /// SelectableList are, for every ButtonBase, ComboBox, TabItem, TreeViewItem and
   /// Label, so every page and dialogue has it, including the ones written after this.
   /// A label is here because its words are drawn by an AccessText: a press on them
   /// reaches the AccessText, never the text block inside it, so SelectableText, which
   /// makes text blocks selectable, never sees one (measured: no menu on a label).
   ///
   /// Ctrl+C is the key itself (Copy's own gestures, Ctrl+C and Ctrl+Insert) pressed on
   /// the control, not the Copy command: a command-bound Copy is also what a menu's
   /// Copy sends to whatever has the keyboard, and the drop-downs' menu below would
   /// then have copied a focused button's caption. It is taken only where nothing
   /// around the control would copy: a button in a data grid's row or a list's row
   /// leaves Ctrl+C to the grid or the list, which copies its selected rows as before.
   ///
   /// Left alone: a control with nothing to copy (an icon, a glyph - no menu, and the
   /// right-click goes on as before); one with a menu of its own or inside something
   /// that has one; one switched off with <see cref="SelectableText.IsEnabledProperty"/>;
   /// a button whose command is Copy (it copies something else); an editable drop-down's
   /// text box, which has its own menu and its own Copy; a drop-down while its list is
   /// open; anything inside a menu, a tool tip, the title bar or a popup that closes
   /// when the pointer leaves it. The theme (Wpf.Ui) gives every drop-down the menu it
   /// gives a text box - Copy, Cut and Paste, in the runtime's words (English in the
   /// six of the seventeen languages it has none for), sending the commands to
   /// whatever has the keyboard - and on a drop-down that cannot be typed into none of
   /// the three can ever run (measured: all three disabled, with the drop-down focused
   /// or not); that menu is not the drop-down's own, and Copy replaces it there. Menu
   /// items are not covered: a right-click on one chooses it.
   /// </summary>
   public static class SelectableCaption
   {
      private static bool installed_;

      private static readonly ICommand[] TextEditingCommands =
      {
         ApplicationCommands.Copy, ApplicationCommands.Cut, ApplicationCommands.Paste, ApplicationCommands.SelectAll,
         ApplicationCommands.Undo, ApplicationCommands.Redo, ApplicationCommands.Delete
      };

      /// <summary>
      /// Registers the handlers for every control whose caption can be copied. Called
      /// once, from App.OnStartup, before the first window; safe to call again.
      /// </summary>
      public static void Install()
      {
         if (installed_)
            return;
         installed_ = true;

         foreach (Type kind in new[] { typeof(ButtonBase), typeof(ComboBox), typeof(TabItem), typeof(TreeViewItem), typeof(Label) })
         {
            EventManager.RegisterClassHandler(kind, FrameworkElement.ContextMenuOpeningEvent,
               new ContextMenuEventHandler(OnContextMenuOpening));
            EventManager.RegisterClassHandler(kind, UIElement.KeyDownEvent, new KeyEventHandler(OnKeyDown));
         }
      }

      /// <summary>
      /// Whether this control's caption is copied here: not switched off, no menu of
      /// its own or of anything it is in, not a button that copies something else, not
      /// a part of a drop-down, not a drop-down whose list is open, and not inside a
      /// menu, a tool tip, the title bar or a popup that closes when the pointer leaves
      /// it (a menu opening over one would close it).
      /// </summary>
      public static bool IsCopyable(Control control)
      {
         if (control == null || !SelectableText.GetIsEnabled(control) || !ContextMenuService.GetIsEnabled(control)
             || (control.ContextMenu != null && !IsThemeTextMenuOnADropDown_(control)))
         {
            return false;
         }

         if (control is ICommandSource source && ReferenceEquals(source.Command, ApplicationCommands.Copy))
            return false;
         if (control is ButtonBase && control.TemplatedParent is ComboBox)
            return false;   // the drop-down's own button: the drop-down copies what it shows
         if (control is ComboBox { IsDropDownOpen: true })
            return false;

         for (DependencyObject node = ParentOf_(control); node != null; node = ParentOf_(node))
         {
            if (node is FrameworkElement element && element.ContextMenu != null)
               return false;
            if (node is MenuItem || node is ContextMenu || node is ToolTip || node is Wpf.Ui.Controls.TitleBar)
               return false;
            if (node is Popup { StaysOpen: false })
               return false;
         }

         return true;
      }

      /// <summary>
      /// The control's caption as it is shown: a drop-down's value (an editable one's
      /// text), a tab's or a tree item's header, and any other control's content - the
      /// text on the screen, read from what the control's template shows it with, so
      /// that an access key's underscore, a template's own words and an icon are not
      /// in it. Pieces of text on one line are joined by a space, lines by CR LF. Empty
      /// when there is no text: an icon, a glyph, nothing chosen.
      /// </summary>
      public static string CaptionOf(Control control)
      {
         switch (control)
         {
            case ComboBox combo:
               return ComboText_(combo);
            case HeaderedItemsControl tree:       // a tree item: its header, not its children
               return ShownText_(tree, tree.Header, false);
            case HeaderedContentControl tab:      // a tab: its name, not the page under it
               return ShownText_(tab, tab.Header, true);
            case ContentControl content:          // a button, a check box, a radio button, a toggle, a label
               return ShownText_(content, content.Content, true);
            default:
               return "";
         }
      }

      /// <summary>
      /// The menu the theme gives a text box, on a drop-down that cannot be typed into:
      /// set by a style, not by the page, and nothing on it but the text commands (Copy,
      /// Cut, Paste and their kind), none of which a drop-down without a text box runs.
      /// </summary>
      private static bool IsThemeTextMenuOnADropDown_(Control control)
      {
         if (control is not ComboBox { IsEditable: false } combo || combo.ContextMenu is not ContextMenu menu)
            return false;
         if (DependencyPropertyHelper.GetValueSource(combo, FrameworkElement.ContextMenuProperty).BaseValueSource == BaseValueSource.Local)
            return false;

         bool any = false;
         foreach (object item in menu.Items)
         {
            if (item is Separator)
               continue;
            if (item is not MenuItem { Command: ICommand command } || Array.IndexOf(TextEditingCommands, command) < 0)
               return false;
            any = true;
         }

         return any;
      }

      // ---- the handlers ------------------------------------------------------------

      /// <summary>
      /// Ctrl+C (or Ctrl+Insert) on the control itself - not on a text box inside it (an
      /// editable drop-down's), whose Copy is its own - that has a caption, where nothing
      /// it is in would copy. The key is the control's before the page's: a class handler
      /// of the control runs before the one that turns the key into a command.
      /// </summary>
      private static void OnKeyDown(object sender, KeyEventArgs e)
      {
         if (e.Handled || sender is not Control control || !ReferenceEquals(e.OriginalSource, control) || !IsCopyGesture_(control, e))
            return;
         if (control is ComboBox { IsEditable: true } || !IsCopyable(control))
            return;

         string caption = CaptionOf(control);
         if (caption.Length == 0 || AncestorCopies_(control))
            return;   // the key goes on, as it did before a caption could be copied

         SelectableText.SetClipboard(caption);
         e.Handled = true;
      }

      private static bool IsCopyGesture_(Control control, KeyEventArgs e)
      {
         foreach (InputGesture gesture in ApplicationCommands.Copy.InputGestures)
         {
            if (gesture.Matches(control, e))
               return true;
         }

         return false;
      }

      /// <summary>
      /// Whether something the control is in would take Copy now - a data grid or a list
      /// with rows chosen, a text with a selection: the command asked of the control's
      /// parent, which is where the key goes when the control leaves it.
      /// </summary>
      private static bool AncestorCopies_(Control control)
      {
         DependencyObject node = ParentOf_(control);
         while (node != null && node is not IInputElement)
            node = ParentOf_(node);

         return node is IInputElement parent && ApplicationCommands.Copy.CanExecute(null, parent);
      }

      /// <summary>
      /// Whether Ctrl+C with the keyboard on this control copies its caption, as
      /// <see cref="OnKeyDown"/> decides: it can have the keyboard, it is not an editable
      /// drop-down (whose text box copies its own selection), and nothing it is in would
      /// take Copy first.
      /// </summary>
      private static bool KeyCopiesCaption_(Control control)
         => control.Focusable && control.IsEnabled && control is not ComboBox { IsEditable: true } && !AncestorCopies_(control);

      private static void OnContextMenuOpening(object sender, ContextMenuEventArgs e)
      {
         if (e.Handled || sender is not Control control || !FromTheControl_(control, e.OriginalSource as DependencyObject)
             || !IsCopyable(control))
         {
            return;
         }

         string caption = CaptionOf(control);
         if (caption.Length == 0)
         {
            // Nothing to copy: no menu with nothing on it, and the event goes on - but
            // not to the theme's text menu on a drop-down, none of whose items can run
            // there (a drop-down with nothing chosen).
            if (IsThemeTextMenuOnADropDown_(control))
               e.Handled = true;
            return;
         }

         ContextMenu menu = SelectableText.CopyMenu(() => SelectableText.SetClipboard(caption), null, null);

         // Copy's key is shown, and named to a screen reader, only where the key copies
         // the same: a label, a grid's column header and a disabled control never have
         // the keyboard, and on a button in a data grid's or a list's chosen row Ctrl+C
         // copies the rows. There the key would have copied something else.
         if (!KeyCopiesCaption_(control) && menu.Items.Count > 0 && menu.Items[0] is MenuItem copyItem)
            copyItem.InputGestureText = "";

         // From the keyboard (the menu key, Shift+F10) there is no pointer to open at:
         // the menu opens under the control, under a tree item's own row.
         if (e.CursorLeft < 0)
         {
            menu.PlacementTarget = control is HeaderedItemsControl tree
               ? PresenterOf_(tree, tree.Header) ?? (UIElement)tree
               : control;
            menu.Placement = PlacementMode.Bottom;
         }
         else
         {
            menu.PlacementTarget = control;
            menu.Placement = PlacementMode.MousePoint;
         }

         menu.IsOpen = true;
         e.Handled = true;
      }

      /// <summary>
      /// A right-click on the control's own surface: not in a text box inside it (an
      /// editable drop-down's, with its own Cut, Copy and Paste), not in a drop-down's
      /// open list, which is in a popup of its own, and not on another control inside
      /// it (an icon button on a tab, a tree item under this one) that had nothing to
      /// copy - that right-click was on it. A part of this control's own template (a
      /// drop-down's arrow, a tree item's chevron) is this control's.
      /// </summary>
      private static bool FromTheControl_(Control control, DependencyObject source)
      {
         DependencyObject node = source;
         while (node != null && node != control)
         {
            if (node is TextBoxBase || node is PasswordBox || node is Popup)
               return false;
            if (IsCaptioned_(node) && !ReferenceEquals(((FrameworkElement)node).TemplatedParent, control))
               return false;
            node = ParentOf_(node);
         }

         return node == control;
      }

      private static bool IsCaptioned_(DependencyObject node)
         => node is ButtonBase || node is ComboBox || node is TabItem || node is TreeViewItem || node is Label;

      // ---- the caption -------------------------------------------------------------------

      /// <summary>
      /// What a drop-down shows: an editable one's text; otherwise its chosen item as
      /// its selection box draws it (through DisplayMemberPath, the item template or
      /// ToString, as the drop-down does). An item that is an element - a
      /// ComboBoxItem whose content is a panel - is drawn there as a picture of
      /// itself, so its own words are read instead.
      /// </summary>
      private static string ComboText_(ComboBox combo)
      {
         if (combo.IsEditable)
            return combo.Text ?? "";

         object item = combo.SelectedItem;
         if (item == null)
            return "";

         ContentPresenter box = PresenterOf_(combo, combo.SelectionBoxItem);
         if (box != null)
         {
            string shown = TextsIn_(box);
            if (shown.Length > 0)
               return shown;
         }

         // Not drawn yet, or drawn as a picture.
         object content = item is ContentControl container ? container.Content : item;
         if (content is string text)
            return text;
         if (content is Visual || content is Visual3D)
            return TextsIn_((DependencyObject)content);
         return combo.Text ?? "";
      }

      /// <summary>
      /// A content or a header as the control shows it: the words inside the presenter
      /// its template shows it through. A control not yet drawn has no presenter; its
      /// caption is then the string itself (without the access key's underscore where
      /// the control takes one) or the words of the element it is.
      /// </summary>
      private static string ShownText_(Control control, object value, bool accessKeys)
      {
         if (value == null)
            return "";

         ContentPresenter presenter = PresenterOf_(control, value);
         if (presenter != null)
            return TextsIn_(presenter);

         if (value is string text)
            return accessKeys ? MnemonicText.AccessTextShown(text) : text;
         if (value is Visual || value is Visual3D)
            return TextsIn_((DependencyObject)value);
         return "";
      }

      /// <summary>
      /// The presenter in the control's own template that shows <paramref name="value"/>
      /// and is on show - not one of a control further in, whose template is its own.
      /// </summary>
      private static ContentPresenter PresenterOf_(Control control, object value)
      {
         if (value == null)
            return null;

         var pending = new Stack<DependencyObject>();
         pending.Push(control);
         while (pending.Count > 0)
         {
            DependencyObject node = pending.Pop();
            if (node is UIElement element && element.Visibility != Visibility.Visible)
               continue;

            if (node is ContentPresenter presenter && ReferenceEquals(presenter.TemplatedParent, control)
                && (ReferenceEquals(presenter.Content, value) || (value.GetType().IsValueType && value.Equals(presenter.Content))))
            {
               return presenter;
            }

            int count = node is Visual || node is Visual3D ? VisualTreeHelper.GetChildrenCount(node) : 0;
            for (int i = count - 1; i >= 0; i--)
               pending.Push(VisualTreeHelper.GetChild(node, i));
         }

         return null;
      }

      /// <summary>
      /// The words under <paramref name="root"/> as they are shown: every visible piece
      /// of text in order, without an icon's glyph or the words of a control further in.
      /// A piece that starts below the one before it starts a new line.
      /// </summary>
      private static string TextsIn_(DependencyObject root)
      {
         var pieces = new List<(UIElement Element, string Text)>();
         Collect_(root, root, pieces);

         var caption = new StringBuilder();
         Rect previous = Rect.Empty;
         foreach ((UIElement element, string text) in pieces)
         {
            Rect bounds = BoundsIn_(element, root as Visual);
            if (caption.Length > 0)
            {
               bool sameLine = !previous.IsEmpty && !bounds.IsEmpty && bounds.Top < previous.Bottom - 0.5;
               if (!sameLine)
                  caption.Append("\r\n");
               else if (!char.IsWhiteSpace(caption[caption.Length - 1]) && !char.IsWhiteSpace(text[0]))
                  caption.Append(' ');
            }

            caption.Append(text);
            previous = bounds;
         }

         return caption.ToString();
      }

      private static void Collect_(DependencyObject node, DependencyObject root, List<(UIElement, string)> pieces)
      {
         if (node is UIElement element && element.Visibility != Visibility.Visible)
            return;
         // An icon's glyph is not text, and a control inside the caption has its own.
         if (node is Wpf.Ui.Controls.IconElement)
            return;
         if (node != root && (node is ButtonBase || node is TextBoxBase || node is PasswordBox || node is ComboBox
                              || node is Thumb || node is ScrollBar || node is Slider))
         {
            return;
         }

         if (node is AccessText access)
         {
            // What it shows is its text without the access key's underscore; the text
            // block it draws with still holds the underscore (measured: .NET 10's WPF),
            // so the words are read from the caption by WPF's own rule, as its automation
            // name is - not MnemonicText.Strip's, which keeps an underscore before a
            // space that WPF takes as the marker ("odd_ one" is drawn "odd one").
            Add_(access, MnemonicText.AccessTextShown(access.Text), pieces);
            return;
         }

         if (node is TextBlock block)
         {
            Add_(block, SelectableText.TextToCopy(block), pieces);
            return;
         }

         int count = node is Visual || node is Visual3D ? VisualTreeHelper.GetChildrenCount(node) : 0;
         for (int i = 0; i < count; i++)
            Collect_(VisualTreeHelper.GetChild(node, i), root, pieces);
      }

      private static void Add_(UIElement element, string text, List<(UIElement, string)> pieces)
      {
         if (!IsGlyphOnly_(text))
            pieces.Add((element, text));
      }

      /// <summary>
      /// Nothing but white space and characters of the Private Use Area, which is where
      /// the symbol fonts (Segoe Fluent Icons, Segoe MDL2 Assets) keep their glyphs: an
      /// icon drawn as text, not words.
      /// </summary>
      private static bool IsGlyphOnly_(string text)
      {
         if (string.IsNullOrEmpty(text))
            return true;

         for (int i = 0; i < text.Length; i++)
         {
            char c = text[i];
            if (char.IsWhiteSpace(c) || (c >= '\uE000' && c <= '\uF8FF'))
               continue;
            if (char.IsHighSurrogate(c) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1])
                && char.ConvertToUtf32(c, text[i + 1]) >= 0xF0000)
            {
               i++;
               continue;
            }

            return false;
         }

         return true;
      }

      private static Rect BoundsIn_(UIElement element, Visual root)
      {
         if (root == null || !element.IsArrangeValid || element.RenderSize.IsEmpty)
            return Rect.Empty;

         try
         {
            return element.TransformToAncestor(root).TransformBounds(new Rect(element.RenderSize));
         }
         catch (InvalidOperationException)
         {
            return Rect.Empty;   // not under the root after all
         }
      }

      /// <summary>
      /// The visual parent, and the logical one where there is none: a content element
      /// has only the latter, and so does the top of a popup's content.
      /// </summary>
      private static DependencyObject ParentOf_(DependencyObject node)
      {
         DependencyObject parent = node is Visual || node is Visual3D ? VisualTreeHelper.GetParent(node) : null;
         return parent ?? LogicalTreeHelper.GetParent(node);
      }
   }
}
