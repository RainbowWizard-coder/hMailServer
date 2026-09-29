// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Media3D;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The words and the values a page shows can be selected with the mouse and
   /// copied: dragged over, double-clicked, copied with Ctrl+C or from a right-click
   /// menu. Issue #48: nothing on Server status, the setup checklist, DNS records,
   /// Logging, API &amp; monitoring or REST API keys could be copied, so a version
   /// number, a host name or a paragraph of advice had to be typed out again by
   /// whoever wanted it in a ticket or a search box.
   ///
   /// Every one of those is a TextBlock, which WPF does not let anybody select, and
   /// the Control Panel makes them in close to two hundred places - each page's own
   /// small helpers, the scaffold's templates, XAML. Turning each into a read-only
   /// TextBox would change how each looks (a text box has a border, padding and its
   /// own template), how a screen reader announces it (as an edit field) and the
   /// tab order (a stop per line of text). So the TextBlocks themselves are made
   /// selectable, here, by class handlers registered once at start-up: every page
   /// and every dialog has it, including the ones written after this.
   ///
   /// What is left out is text inside something that is clicked or typed into:
   /// buttons (and so toggle switches, check boxes, radio buttons and the cards that
   /// act as buttons), text boxes, list, tree, tab and menu items, data grid cells
   /// and headers, tool tips, icons and the title bar. A press there has to stay a
   /// press on the control. A list is copied by its rows: a data grid already copies
   /// its selected rows with Ctrl+C, the live log copies its selected lines
   /// (LogsView), and every other list its selected rows (<see cref="SelectableList"/>).
   /// A container can opt its text and lists out with <see cref="IsEnabledProperty"/>.
   ///
   /// The selecting is WPF's own text editor, the one behind every read-only TextBox,
   /// attached to the TextBlock read-only. It is internal to WPF and reached by
   /// reflection (TextEditor, its constructor, IsReadOnly, TextView, Selection,
   /// OnDetach and RegisterCommandHandlers, and TextBlock.TextContainer), all of them
   /// checked at start-up and present in .NET 10's WPF (10.0.11 when this was
   /// written). If any is missing the text cannot be dragged over, and the
   /// right-click Copy still copies a whole piece of text - the feature narrows, and
   /// nothing throws. A press on a link inside a text stays the link's.
   ///
   /// Nothing changes about a piece of text until the pointer is on it: it keeps its
   /// layout, its automation peer (a TextBlock is announced as text, with the same
   /// name, before and after) and its place out of the tab order. The pointer turns
   /// to an I-beam over it; the first press makes it focusable, so that Ctrl+C can
   /// reach it, but never a tab stop, and attaches the editor. The editor is removed
   /// again when the text leaves the screen, because each one registers for the
   /// dispatcher's shutdown and a page rebuilt on every visit would pile them up.
   /// </summary>
   public static class SelectableText
   {
      /// <summary>
      /// False on an element turns selecting off for every piece of text inside it,
      /// and copying rows off for every list inside it (SelectableList). Inherited,
      /// so one setting covers a whole panel.
      /// </summary>
      public static readonly DependencyProperty IsEnabledProperty = DependencyProperty.RegisterAttached(
         "IsEnabled", typeof(bool), typeof(SelectableText),
         new FrameworkPropertyMetadata(true, FrameworkPropertyMetadataOptions.Inherits));

      public static bool GetIsEnabled(DependencyObject element) => (bool)element.GetValue(IsEnabledProperty);

      public static void SetIsEnabled(DependencyObject element, bool value) => element.SetValue(IsEnabledProperty, value);

      private static bool installed_;
      private static ContextMenu menu_;
      private static MenuItem menuCopy_;
      private static MenuItem menuSelectAll_;
      private static WeakReference<TextBlock> menuFor_;

      /// <summary>
      /// Registers the handlers for every TextBlock in the application. Called once,
      /// from App.OnStartup, before the first window; safe to call again.
      /// </summary>
      public static void Install()
      {
         if (installed_)
            return;
         installed_ = true;

         // These three first: WPF runs a class's handlers in the order they were
         // registered, and each of them has to come before the text editor's own.
         // The editor's Copy would also put the text on the clipboard as XAML and
         // RTF, with the theme's colours in it - pasted into a light document that
         // is white text on white. Its context menu is Cut, Copy and Paste, two of
         // them for ever disabled here, in the runtime's own words, which six of the
         // Control Panel's seventeen languages do not have. And its pointer is an
         // I-beam only over the glyphs, where this one covers the whole block.
         CommandManager.RegisterClassCommandBinding(typeof(TextBlock),
            new CommandBinding(ApplicationCommands.Copy, OnCopy, OnCanCopy));
         EventManager.RegisterClassHandler(typeof(TextBlock), FrameworkElement.ContextMenuOpeningEvent,
            new ContextMenuEventHandler(OnContextMenuOpening));
         EventManager.RegisterClassHandler(typeof(TextBlock), Mouse.QueryCursorEvent,
            new QueryCursorEventHandler(OnQueryCursor));

         // The editor is registered for every TextBlock, and two of its parts act on
         // a TextBlock that has no editor at all. Its keys (arrows, Page Up/Down,
         // Home/End, Ctrl+A) are class input bindings: pressed on a link inside a
         // paragraph - the About page's - the paragraph turned each into an editor
         // command that could not run, and a command that cannot run swallows its
         // key, so the page no longer scrolled from the keyboard there. And its drop
         // handlers refuse any drag over any text, handled: text dragged into an
         // empty text box over its placeholder was refused. The keys pass on unless
         // this text itself has the keyboard and an editor; a drag over text is the
         // text's parent's to take, as it was - the editor's drop handlers are
         // stepped over (marked handled just before them, put back just after).
         EventManager.RegisterClassHandler(typeof(TextBlock), CommandManager.PreviewCanExecuteEvent,
            new CanExecuteRoutedEventHandler(OnPreviewCanExecute));
         foreach (RoutedEvent drag in DropTargetEvents)
            EventManager.RegisterClassHandler(typeof(TextBlock), drag, new DragEventHandler(OnDragBeforeEditor), true);

         TextEditorAccess.Register();

         foreach (RoutedEvent drag in DropTargetEvents)
            EventManager.RegisterClassHandler(typeof(TextBlock), drag, new DragEventHandler(OnDragAfterEditor), true);

         EventManager.RegisterClassHandler(typeof(TextBlock), UIElement.PreviewMouseDownEvent,
            new MouseButtonEventHandler(OnPreviewMouseDown));
         EventManager.RegisterClassHandler(typeof(TextBlock), UIElement.PreviewKeyDownEvent,
            new KeyEventHandler(OnPreviewKeyDown));
         EventManager.RegisterClassHandler(typeof(TextBlock), FrameworkElement.RequestBringIntoViewEvent,
            new RequestBringIntoViewEventHandler(OnRequestBringIntoView));

         // A selection can be dragged out of its text, as out of a read-only text
         // box, and the editor builds the drag's data as it would for its own Copy:
         // with RTF and XAML beside the text, in the theme's colours (measured: white
         // text in the RTF on the dark theme). Plain text only, as Copy puts it.
         EventManager.RegisterClassHandler(typeof(TextBlock), DataObject.SettingDataEvent,
            new DataObjectSettingDataEventHandler(OnSettingData));
      }

      /// <summary>
      /// Whether this piece of text can be selected and copied: not switched off,
      /// able to take the pointer, and not inside anything that is clicked or typed
      /// into (see the class summary). Decided again at each use rather than once,
      /// so that a block moved elsewhere is judged where it is now.
      /// </summary>
      public static bool IsSelectable(TextBlock block)
      {
         if (block == null || !GetIsEnabled(block) || !block.IsHitTestVisible)
            return false;

         // A block holding controls (an InlineUIContainer) is a small form rather
         // than text; a press on one of those controls has to reach it. Such a
         // control is the block's visual child, and plain text has none - asked
         // that way because reading Inlines would convert every block the pointer
         // passes over to the form an editor needs.
         if (VisualTreeHelper.GetChildrenCount(block) > 0)
            return false;

         for (DependencyObject node = ParentOf_(block); node != null; node = ParentOf_(node))
         {
            if (IsInteractive_(node))
               return false;

            // Somebody else's menu is on this text already.
            if (node is FrameworkElement element && element.ContextMenu != null)
               return false;
         }

         return block.ContextMenu == null;
      }

      /// <summary>
      /// What Copy puts on the clipboard for this block: the selection when there is
      /// one, otherwise the whole text - so a right-click on a value copies the value
      /// without having to select it first. Plain text, exactly as shown, line
      /// breaks as CR LF.
      /// </summary>
      public static string TextToCopy(TextBlock block)
      {
         if (block == null)
            return "";

         TextRange selection = TextEditorAccess.SelectionOf(block);
         if (selection != null && !selection.IsEmpty)
            return selection.Text;

         // A block built from runs keeps its words in them and an empty Text; one
         // given only a Text keeps them there, and reading its content as a range
         // would convert it to the form an editor needs.
         return TextEditorAccess.HoldsOnlyText(block) ? block.Text ?? "" : new TextRange(block.ContentStart, block.ContentEnd).Text;
      }

      /// <summary>
      /// Puts text on the clipboard, as plain text, through <see cref="ClipboardWriter"/>
      /// (which says why in two steps). A copy that fails outright is said in a toast
      /// rather than left as a Copy that silently did nothing.
      /// </summary>
      public static void SetClipboard(string text)
      {
         if (!ClipboardWriter.TrySetText(text, out string error))
            Toast.Failure(L("Could not copy: ") + error);
      }

      /// <summary>
      /// The Copy and Select all menu, for a control that copies something of its
      /// own - the live log's lines, a list's rows (SelectableList). The same words
      /// and the same keys as the menu on a piece of text; no Select all without
      /// <paramref name="selectAll"/>.
      /// </summary>
      public static ContextMenu CopyMenu(Action copy, Func<bool> canCopy, Action selectAll)
      {
         ContextMenu menu = NewMenu_();
         MenuItem copyItem = CopyItem_();
         copyItem.Click += (s, e) => copy();
         menu.Items.Add(copyItem);

         if (selectAll != null)
         {
            MenuItem selectAllItem = SelectAllItem_();
            selectAllItem.Click += (s, e) => selectAll();
            menu.Items.Add(selectAllItem);
         }

         menu.Opened += (s, e) => copyItem.IsEnabled = canCopy == null || canCopy();
         return menu;
      }

      // ---- the handlers ------------------------------------------------------------

      private static readonly RoutedEvent[] DropTargetEvents =
      {
         DragDrop.DragEnterEvent, DragDrop.DragOverEvent, DragDrop.DragLeaveEvent, DragDrop.DropEvent
      };

      // The one drag event being stepped over, between the two handlers either side
      // of the editor's on the same text; nothing runs between them.
      private static DragEventArgs dragArgs_;
      private static DragDropEffects dragEffects_;
      private static bool dragHandled_;

      /// <summary>
      /// A key reaches a TextBlock's class bindings whenever the keyboard is on
      /// something inside the text - a link - and the editor's commands cannot run
      /// there without an editor that has the keyboard. Such a command is let past
      /// (ContinueRouting): the key goes on to the page, which scrolls, as it did
      /// before any text had an editor. Copy stays: on a selectable text it copies
      /// the text, whatever inside it has the keyboard.
      /// </summary>
      private static void OnPreviewCanExecute(object sender, CanExecuteRoutedEventArgs e)
      {
         if (e.Handled || sender is not TextBlock block || !ReferenceEquals(e.OriginalSource, block))
            return;

         bool editing = block.IsKeyboardFocused && TextEditorAccess.IsAttached(block);
         if (editing)
            return;

         bool editorCommand = e.Command == ApplicationCommands.SelectAll
            || (e.Command is RoutedCommand routed && routed.OwnerType == typeof(EditingCommands));
         bool ownCopy = e.Command == ApplicationCommands.Copy && (TextEditorAccess.IsAttached(block) || IsSelectable(block));
         if (!editorCommand && (e.Command != ApplicationCommands.Copy || ownCopy))
            return;

         e.CanExecute = false;
         e.ContinueRouting = true;
         e.Handled = true;
      }

      private static void OnDragBeforeEditor(object sender, DragEventArgs e)
      {
         dragArgs_ = e;
         dragEffects_ = e.Effects;
         dragHandled_ = e.Handled;
         e.Handled = true;   // the editor's handlers do not take handled events
      }

      private static void OnDragAfterEditor(object sender, DragEventArgs e)
      {
         if (!ReferenceEquals(dragArgs_, e))
            return;

         e.Effects = dragEffects_;
         e.Handled = dragHandled_;
         dragArgs_ = null;
      }

      private static void OnSettingData(object sender, DataObjectSettingDataEventArgs e)
      {
         // This text's own editor, not a text box inside the text (which puts plain text anyway).
         if (sender is TextBlock block && ReferenceEquals(e.OriginalSource, block) && TextEditorAccess.IsAttached(block)
             && e.Format != DataFormats.UnicodeText && e.Format != DataFormats.Text)
         {
            e.CancelCommand();
         }
      }

      private static void OnCanCopy(object sender, CanExecuteRoutedEventArgs e)
      {
         if (sender is TextBlock block && (TextEditorAccess.IsAttached(block) || IsSelectable(block)))
         {
            e.CanExecute = TextToCopy(block).Length > 0;
            e.Handled = true;
         }
      }

      private static void OnCopy(object sender, ExecutedRoutedEventArgs e)
      {
         if (sender is TextBlock block && (TextEditorAccess.IsAttached(block) || IsSelectable(block)))
         {
            SetClipboard(TextToCopy(block));
            e.Handled = true;
         }
      }

      private static void OnQueryCursor(object sender, QueryCursorEventArgs e)
      {
         if (e.Handled || sender is not TextBlock block || block.Cursor != null || !IsSelectable(block))
            return;

         e.Cursor = Cursors.IBeam;
         e.Handled = true;
      }

      /// <summary>
      /// The first press on a piece of text: it becomes focusable, so that the
      /// keyboard can copy what the mouse selects, and gets its editor, whose own
      /// handler for this same press then starts the selection. Never a tab stop:
      /// the keyboard reaches controls, and a stop on every line of text would bury
      /// them.
      /// </summary>
      private static void OnPreviewMouseDown(object sender, MouseButtonEventArgs e)
      {
         if (sender is not TextBlock block)
            return;

         // A press on a link inside the text is the link's. The editor would take it
         // too - it moves its caret on a press it has been told is handled - so a
         // text that has one lets it go and gets it back at the next press on words.
         if (InLink_(e.OriginalSource as DependencyObject, block))
         {
            block.Unloaded -= OnUnloaded;
            TextEditorAccess.Detach(block);
            return;
         }

         if (TextEditorAccess.IsAttached(block) || !IsSelectable(block))
            return;

         if (!TextEditorAccess.Attach(block))
            return;
         block.Unloaded += OnUnloaded;

         if (!block.Focusable)
         {
            KeyboardNavigation.SetIsTabStop(block, false);
            // The selection shows where the keyboard is; a focus ring around a
            // paragraph somebody has just clicked says nothing more.
            block.FocusVisualStyle = null;
            block.Focusable = true;
         }

         // The editor finds the position under the pointer through the block's
         // layout, which attaching it has just invalidated.
         block.UpdateLayout();
      }

      private static void OnUnloaded(object sender, RoutedEventArgs e)
      {
         if (sender is TextBlock block)
         {
            block.Unloaded -= OnUnloaded;
            TextEditorAccess.Detach(block);
         }
      }

      private static void OnContextMenuOpening(object sender, ContextMenuEventArgs e)
      {
         if (e.Handled || sender is not TextBlock block || !IsSelectable(block))
            return;

         menu_ ??= BuildMenu_();
         if (menuFor_ != null && menuFor_.TryGetTarget(out TextBlock previous))
            previous.ClearValue(TextBoxBase.IsInactiveSelectionHighlightEnabledProperty);
         menuFor_ = new WeakReference<TextBlock>(block);
         menuCopy_.IsEnabled = TextToCopy(block).Length > 0;
         // The menu takes the keyboard, and a selection is drawn only while its text
         // has it: without this, what Copy is about to copy disappeared as the menu
         // offering it opened. A text box keeps its selection on show the same way.
         block.SetValue(TextBoxBase.IsInactiveSelectionHighlightEnabledProperty, true);
         // Select all needs the editor, which a press attaches; without one (a WPF
         // that does not have it) Copy still copies the whole text.
         menuSelectAll_.Visibility = TextEditorAccess.IsAttached(block) ? Visibility.Visible : Visibility.Collapsed;

         menu_.PlacementTarget = block;
         // From the keyboard (the menu key, Shift+F10) there is no pointer to open at.
         menu_.Placement = e.CursorLeft < 0 ? PlacementMode.Bottom : PlacementMode.MousePoint;
         menu_.IsOpen = true;
         e.Handled = true;
      }

      /// <summary>
      /// With the text focused, the keys that scroll a page still scroll it. The
      /// editor would take them to move a caret nobody can see, and a page that
      /// stopped scrolling from the keyboard because somebody had clicked on its
      /// text would be a regression. Shift with them is the editor's: it extends
      /// the selection. The mapping is the ScrollViewer's own (ScrollInDirection).
      /// </summary>
      private static void OnPreviewKeyDown(object sender, KeyEventArgs e)
      {
         // The text itself, not a link inside it that has the keyboard.
         if (e.Handled || sender is not TextBlock block || !block.IsKeyboardFocused || !TextEditorAccess.IsAttached(block))
            return;

         ModifierKeys modifiers = Keyboard.Modifiers;
         if ((modifiers & (ModifierKeys.Shift | ModifierKeys.Alt)) != 0)
            return;

         ScrollViewer scroller = ScrollerOf_(block);
         if (scroller == null)
            return;

         bool control = (modifiers & ModifierKeys.Control) != 0;
         bool mirrored = scroller.FlowDirection == FlowDirection.RightToLeft;

         switch (e.Key)
         {
            case Key.Left:
               if (mirrored) scroller.LineRight(); else scroller.LineLeft();
               break;
            case Key.Right:
               if (mirrored) scroller.LineLeft(); else scroller.LineRight();
               break;
            case Key.Up:
               scroller.LineUp();
               break;
            case Key.Down:
               scroller.LineDown();
               break;
            case Key.PageUp:
               scroller.PageUp();
               break;
            case Key.PageDown:
               scroller.PageDown();
               break;
            case Key.Home:
               if (control) scroller.ScrollToTop(); else scroller.ScrollToLeftEnd();
               break;
            case Key.End:
               if (control) scroller.ScrollToBottom(); else scroller.ScrollToRightEnd();
               break;
            default:
               return;
         }

         e.Handled = true;
      }

      /// <summary>
      /// A press on a paragraph taller than the view, or half scrolled out of it,
      /// focuses it, and an element that takes focus asks to be brought into view -
      /// which scrolled the page under the pointer at the moment a selection began.
      /// That request, and only that one, is declined: the editor's own requests,
      /// to follow a selection dragged past the edge, name a rectangle.
      /// </summary>
      private static void OnRequestBringIntoView(object sender, RequestBringIntoViewEventArgs e)
      {
         if (sender is TextBlock block && ReferenceEquals(e.TargetObject, block) && e.TargetRect.IsEmpty
             && TextEditorAccess.IsAttached(block)
             && (Mouse.LeftButton == MouseButtonState.Pressed || Mouse.RightButton == MouseButtonState.Pressed))
         {
            e.Handled = true;
         }
      }

      // ---- the menu ----------------------------------------------------------------

      private static ContextMenu BuildMenu_()
      {
         ContextMenu menu = NewMenu_();

         // The block is held weakly and only until the next opening, so a page the
         // menu was last opened on can still go.
         menuCopy_ = CopyItem_();
         menuCopy_.Click += (s, e) =>
         {
            if (menuFor_ != null && menuFor_.TryGetTarget(out TextBlock block))
               SetClipboard(TextToCopy(block));
         };
         menu.Items.Add(menuCopy_);

         menuSelectAll_ = SelectAllItem_();
         menuSelectAll_.Click += (s, e) =>
         {
            if (menuFor_ != null && menuFor_.TryGetTarget(out TextBlock block))
               TextEditorAccess.SelectAll(block);
         };
         menu.Items.Add(menuSelectAll_);

         menu.Closed += (s, e) =>
         {
            // The popup reports its closing a moment later: a right-click on another
            // text while the menu was open has opened it again, on that text, by now.
            if (menu.IsOpen)
               return;

            if (menuFor_ != null && menuFor_.TryGetTarget(out TextBlock block))
               block.ClearValue(TextBoxBase.IsInactiveSelectionHighlightEnabledProperty);
            // PlacementTarget is a strong reference, and this menu lives as long as
            // the program: left set, it kept the last page the menu was opened on -
            // the whole page, its view and its data - alive after it was left.
            menu.ClearValue(ContextMenu.PlacementTargetProperty);
         };

         return menu;
      }

      /// <summary>
      /// The menu in the interface's own type. A menu set as a control's ContextMenu
      /// inherits that control's font, and the live log's is a monospaced 12.
      /// </summary>
      private static ContextMenu NewMenu_() => new()
      {
         FontFamily = new FontFamily(Typography.UiFontFamily),
         FontSize = Typography.Control
      };

      private static MenuItem CopyItem_()
         => new() { Header = L("_Copy"), InputGestureText = GestureText_(ApplicationCommands.Copy) };

      private static MenuItem SelectAllItem_()
         => new() { Header = L("Select _all"), InputGestureText = GestureText_(ApplicationCommands.SelectAll) };

      /// <summary>The command's key as WPF shows it in its own menus (Ctrl+C).</summary>
      private static string GestureText_(RoutedCommand command)
      {
         foreach (InputGesture gesture in command.InputGestures)
         {
            if (gesture is KeyGesture key)
               return key.GetDisplayStringForCulture(CultureInfo.CurrentUICulture);
         }

         return null;
      }

      // ---- the tree ------------------------------------------------------------------

      private static bool IsInteractive_(DependencyObject node)
      {
         return node is ButtonBase                          // buttons, toggles, check boxes, radio buttons, acting cards
             || node is TextBoxBase || node is PasswordBox  // their own selection, and placeholders
             || node is ComboBox
             || node is ListBoxItem                         // list, list view and combo box items
             || node is TreeViewItem || node is TabItem || node is MenuItem
             || node is DataGridCell || node is DataGridColumnHeader || node is DataGridRowHeader
             || node is ToolTip || node is ContextMenu
             || node is Thumb || node is ScrollBar || node is Slider
             || node is Hyperlink
             || node is Wpf.Ui.Controls.IconElement         // a glyph in a symbol font is not text
             || node is Wpf.Ui.Controls.TitleBar;           // pressing there moves the window
      }

      private static bool InLink_(DependencyObject source, TextBlock block)
      {
         for (DependencyObject node = source; node != null && node != block; node = LogicalTreeHelper.GetParent(node))
         {
            if (node is Hyperlink)
               return true;
         }

         return false;
      }

      /// <summary>
      /// The visual parent, and the logical one where there is none: a content
      /// element (a Hyperlink, a Run) has only the latter, and so does the top of a
      /// popup's content.
      /// </summary>
      private static DependencyObject ParentOf_(DependencyObject node)
      {
         DependencyObject parent = node is Visual || node is Visual3D ? VisualTreeHelper.GetParent(node) : null;
         return parent ?? LogicalTreeHelper.GetParent(node);
      }

      private static ScrollViewer ScrollerOf_(DependencyObject node)
      {
         for (DependencyObject current = ParentOf_(node); current != null; current = ParentOf_(current))
         {
            if (current is ScrollViewer scroller)
               return scroller;
         }

         return null;
      }

      // ---- WPF's text editor -----------------------------------------------------------

      /// <summary>
      /// The internal members of WPF's text editor that a read-only TextBox uses,
      /// reached by reflection and checked all at once: either every one of them is
      /// there, or none is used.
      /// </summary>
      private static class TextEditorAccess
      {
         private const BindingFlags Internal = BindingFlags.Instance | BindingFlags.NonPublic;

         private static ConstructorInfo create_;
         private static PropertyInfo readOnly_;
         private static PropertyInfo view_;
         private static PropertyInfo selection_;
         private static MethodInfo detach_;
         private static PropertyInfo blockContainer_;
         private static PropertyInfo containerView_;
         private static PropertyInfo complex_;
         private static bool available_;

         private static readonly ConditionalWeakTable<TextBlock, object> editors_ = new();

         public static void Register()
         {
            try
            {
               Assembly framework = typeof(TextBlock).Assembly;
               Type editor = framework.GetType("System.Windows.Documents.TextEditor", false);
               Type container = framework.GetType("System.Windows.Documents.ITextContainer", false);
               if (editor == null || container == null)
                  return;

               create_ = editor.GetConstructor(Internal, null, new[] { container, typeof(FrameworkElement), typeof(bool) }, null);
               readOnly_ = editor.GetProperty("IsReadOnly", Internal);
               view_ = editor.GetProperty("TextView", Internal);
               selection_ = editor.GetProperty("Selection", Internal);
               detach_ = editor.GetMethod("OnDetach", Internal, null, Type.EmptyTypes, null);
               blockContainer_ = typeof(TextBlock).GetProperty("TextContainer", Internal);
               containerView_ = container.GetProperty("TextView");
               // Optional: without it a whole text is read as a range, which is right,
               // only not free.
               complex_ = typeof(TextBlock).GetProperty("HasComplexContent", Internal);
               MethodInfo register = editor.GetMethod("RegisterCommandHandlers", BindingFlags.Static | BindingFlags.NonPublic,
                  null, new[] { typeof(Type), typeof(bool), typeof(bool), typeof(bool) }, null);

               if (create_ == null || readOnly_ == null || view_ == null || selection_ == null || detach_ == null
                   || blockContainer_ == null || containerView_ == null || register == null)
               {
                  return;
               }

               // Plain content, read-only, with the mouse and keyboard handlers. Read-only
               // twice over, here and on each editor (Attach): typing, Backspace, Delete,
               // Cut, Paste and Undo leave a text as it was.
               register.Invoke(null, new object[] { typeof(TextBlock), false, true, true });
               available_ = true;
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               available_ = false;
            }
         }

         public static bool IsAttached(TextBlock block) => editors_.TryGetValue(block, out _);

         /// <summary>True for a block that has only ever been given a Text - no runs,
         /// no editor. False when that cannot be told.</summary>
         public static bool HoldsOnlyText(TextBlock block)
         {
            try
            {
               return complex_ != null && complex_.GetValue(block) is false;
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               return false;
            }
         }

         public static bool Attach(TextBlock block)
         {
            if (!available_ || IsAttached(block))
               return false;

            object editor = null;
            try
            {
               object container = blockContainer_.GetValue(block);
               editor = create_.Invoke(new[] { container, block, (object)false });
               readOnly_.SetValue(editor, true);
               view_.SetValue(editor, containerView_.GetValue(container));
               editors_.AddOrUpdate(block, editor);
               return true;
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               if (editor != null)
                  Release_(editor);
               return false;
            }
         }

         public static void Detach(TextBlock block)
         {
            if (!editors_.TryGetValue(block, out object editor))
               return;

            editors_.Remove(block);
            Release_(editor);
         }

         public static TextRange SelectionOf(TextBlock block)
         {
            if (!editors_.TryGetValue(block, out object editor))
               return null;

            try
            {
               return selection_.GetValue(editor) as TextRange;
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               return null;
            }
         }

         public static void SelectAll(TextBlock block)
         {
            TextRange selection = SelectionOf(block);
            if (selection == null)
               return;

            if (!block.IsKeyboardFocused)
               block.Focus();
            selection.Select(block.ContentStart, block.ContentEnd);
         }

         private static void Release_(object editor)
         {
            try
            {
               detach_.Invoke(editor, null);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               // Deliberately ignored: the editor is already out of the table, so
               // nothing reaches it again; this only returns what it registered.
            }
         }
      }
   }
}
