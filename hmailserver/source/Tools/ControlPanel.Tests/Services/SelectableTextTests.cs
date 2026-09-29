// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Issue #48: the text and values on the Control Panel's pages can be selected and
   /// copied, the live log's lines and every plain list's rows too, and every Copy
   /// writes the clipboard the one way (Services/SelectableText.cs, SelectableList.cs,
   /// ClipboardWriter.cs, Views/LogsView.xaml.cs).
   ///
   /// The behaviour is WPF's and is not rendered here - this assembly is WPF-free - so
   /// what is held is what the behaviour rests on: the Copy menu's two captions in
   /// every language, and the wiring that a rendered test would only catch after the
   /// fact (scanned, as WithheldRowTests is). Each assertion names the regression it
   /// stands for.
   /// </summary>
   public class SelectableTextTests
   {
      // ---- the menu's captions ---------------------------------------------------------

      /// <summary>
      /// Copy and Select all share one menu, so in every language each carries an
      /// access key and the two keys differ. check-mnemonics.py reads buttons, check
      /// boxes and radio buttons, not menu items, so this is where it is held.
      /// </summary>
      [Fact]
      public void EveryLanguage_GivesCopyAndSelectAllTwoDifferentKeys()
      {
         string resources = ControlPanelDirectory("Resources");
         Assert.True(resources != null, "ControlPanel/Resources could not be found from the test binaries.");

         string[] catalogues = Directory.GetFiles(resources, "Strings*.resx");
         Assert.True(catalogues.Length >= 18, "Expected the English catalogue and seventeen languages, found " + catalogues.Length + ".");

         foreach (string path in catalogues)
         {
            Dictionary<string, string> strings = ReadResx(path);
            string name = Path.GetFileName(path);

            Assert.True(strings.TryGetValue("_Copy", out string copy) && copy.Length > 0, name + " has no \"_Copy\".");
            Assert.True(strings.TryGetValue("Select _all", out string selectAll) && selectAll.Length > 0, name + " has no \"Select _all\".");

            string copyKey = MnemonicText.Key(copy);
            string selectAllKey = MnemonicText.Key(selectAll);
            Assert.True(copyKey != null, name + ": \"" + copy + "\" carries no access key.");
            Assert.True(selectAllKey != null, name + ": \"" + selectAll + "\" carries no access key.");
            Assert.True(!string.Equals(copyKey, selectAllKey, StringComparison.OrdinalIgnoreCase),
               name + ": \"" + copy + "\" and \"" + selectAll + "\" are on one menu and share the key " + copyKey + ".");
         }
      }

      // ---- the wiring --------------------------------------------------------------------

      [Fact]
      public void TheApplication_InstallsSelectableText_BeforeTheFirstWindow()
      {
         string app = ReadControlPanelFile("App.xaml.cs");
         string onStartup = WithheldRowTests.MemberText(app, "protected override void OnStartup(StartupEventArgs e)");
         Assert.True(onStartup != null, "App.OnStartup is gone.");
         Assert.True(onStartup.Contains("SelectableText.Install();", StringComparison.Ordinal),
            "App.OnStartup no longer installs SelectableText, so no page's text can be selected or copied.");
      }

      /// <summary>
      /// WPF runs a class's handlers in the order they were registered. Registered after
      /// the text editor's, Copy would put XAML and RTF with the theme's colours on the
      /// clipboard (white text pasted into a light document), and the right-click menu
      /// would be the editor's Cut/Copy/Paste in the runtime's words.
      /// </summary>
      [Fact]
      public void CopyTheMenuAndThePointer_AreRegisteredBeforeTheTextEditor()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "SelectableText.Install is gone.");

         int editor = install.IndexOf("TextEditorAccess.Register();", StringComparison.Ordinal);
         Assert.True(editor > 0, "Install no longer registers the text editor.");

         foreach (string first in new[] { "ApplicationCommands.Copy", "ContextMenuOpeningEvent", "Mouse.QueryCursorEvent" })
         {
            int at = install.IndexOf(first, StringComparison.Ordinal);
            Assert.True(at >= 0 && at < editor, first + " is registered after the text editor's own handlers, which then win.");
         }
      }

      /// <summary>
      /// The editor's keys are class input bindings on every TextBlock. With the
      /// keyboard on a link inside a paragraph (the About page's), the paragraph turned
      /// Page Down, the arrows, Home and End into editor commands that could not run -
      /// and a command that cannot run swallows its key unless its CanExecute says to
      /// continue routing: measured, the page stopped scrolling from the keyboard there.
      /// </summary>
      [Fact]
      public void KeysPressedInsideAText_ThatNoEditorHas_GoOnToThePage()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "SelectableText.Install is gone.");
         Assert.Contains("CommandManager.PreviewCanExecuteEvent", install);

         string handler = WithheldRowTests.MemberText(source, "private static void OnPreviewCanExecute(object sender, CanExecuteRoutedEventArgs e)");
         Assert.True(handler != null, "SelectableText.OnPreviewCanExecute is gone, so a key pressed on a link inside a text is swallowed.");
         Assert.Contains("e.ContinueRouting = true;", handler);
         Assert.Contains("block.IsKeyboardFocused && TextEditorAccess.IsAttached(block)", handler);
         Assert.Contains("typeof(EditingCommands)", handler);
      }

      /// <summary>
      /// The editor's drop handlers mark every drag over a TextBlock handled with no
      /// effect, editor or not: measured, text dragged into an empty text box over its
      /// placeholder was refused, and a drop zone never heard of a drag over its words.
      /// They are stepped over - marked handled just before, put back just after.
      /// </summary>
      [Fact]
      public void ADragOverText_IsLeftToWhatTheTextIsIn()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "SelectableText.Install is gone.");

         int before = install.IndexOf("OnDragBeforeEditor", StringComparison.Ordinal);
         int editor = install.IndexOf("TextEditorAccess.Register();", StringComparison.Ordinal);
         int after = install.IndexOf("OnDragAfterEditor", StringComparison.Ordinal);
         Assert.True(before >= 0 && editor > before && after > editor,
            "The drag handlers no longer sit either side of the text editor's, so its drop handlers refuse drags over text again.");

         string beforeHandler = WithheldRowTests.MemberText(source, "private static void OnDragBeforeEditor(object sender, DragEventArgs e)");
         string afterHandler = WithheldRowTests.MemberText(source, "private static void OnDragAfterEditor(object sender, DragEventArgs e)");
         Assert.True(beforeHandler != null && afterHandler != null, "A drag handler is gone.");
         Assert.Contains("e.Handled = true;", beforeHandler);
         Assert.Contains("e.Effects = dragEffects_;", afterHandler);
         Assert.Contains("e.Handled = dragHandled_;", afterHandler);
         foreach (string drag in new[] { "DragDrop.DragEnterEvent", "DragDrop.DragOverEvent", "DragDrop.DragLeaveEvent" })
            Assert.Contains(drag, source);
      }

      /// <summary>
      /// The text menu is one menu for the program's life, and PlacementTarget is a
      /// strong reference: left set after it closed, the last page it was opened on -
      /// page, view and data - stayed alive after it was left.
      /// </summary>
      [Fact]
      public void TheTextMenu_LetsGoOfItsTextWhenItCloses()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string build = WithheldRowTests.MemberText(source, "private static ContextMenu BuildMenu_()");
         Assert.True(build != null, "SelectableText.BuildMenu_ is gone.");
         int closed = build.IndexOf("menu.Closed +=", StringComparison.Ordinal);
         Assert.True(closed >= 0, "The text menu no longer does anything when it closes.");
         Assert.True(build.IndexOf("menu.ClearValue(ContextMenu.PlacementTargetProperty);", closed, StringComparison.Ordinal) > closed,
            "The text menu keeps its PlacementTarget after closing, and with it the page it was last opened on.");
      }

      /// <summary>
      /// A press on the words of a button, a list row, a tab or a menu has to stay a
      /// press on that control; a glyph is not text; the title bar moves the window.
      /// </summary>
      [Fact]
      public void TextInsideControls_IsLeftToTheControls()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string interactive = WithheldRowTests.MemberText(source, "private static bool IsInteractive_(DependencyObject node)");
         Assert.True(interactive != null, "SelectableText.IsInteractive_ is gone.");

         foreach (string control in new[]
         {
            "ButtonBase", "TextBoxBase", "PasswordBox", "ComboBox", "ListBoxItem", "TreeViewItem", "TabItem", "MenuItem",
            "DataGridCell", "DataGridColumnHeader", "ToolTip", "ContextMenu", "Thumb", "ScrollBar", "Slider", "Hyperlink",
            "IconElement", "TitleBar"
         })
         {
            Assert.True(Regex.IsMatch(interactive, @"\bnode is (?:[\w.]+\.)?" + control + @"\b"),
               "Text inside a " + control + " is no longer left to it, so a press on its words would start a selection instead.");
         }
      }

      /// <summary>
      /// Selectable text is never a tab stop - the keyboard reaches controls, and a stop
      /// on every line of text would bury them - and it only becomes focusable when the
      /// pointer presses it, so until then a screen reader and the tab order see exactly
      /// what they saw before.
      /// </summary>
      [Fact]
      public void TextBecomesFocusable_OnlyOnAPress_AndNeverATabStop()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string press = WithheldRowTests.MemberText(source, "private static void OnPreviewMouseDown(object sender, MouseButtonEventArgs e)");
         Assert.True(press != null, "SelectableText.OnPreviewMouseDown is gone.");

         int notATabStop = press.IndexOf("KeyboardNavigation.SetIsTabStop(block, false);", StringComparison.Ordinal);
         int focusable = press.IndexOf("block.Focusable = true;", StringComparison.Ordinal);
         Assert.True(notATabStop >= 0 && focusable > notATabStop, "A pressed text is made focusable without first being taken out of the tab order.");

         Assert.Single(Regex.Matches(source, @"\.Focusable = true"));
         Assert.False(source.Contains("LoadedEvent", StringComparison.Ordinal),
            "Text is changed as it loads rather than when it is pressed, so every text on every page would carry an editor.");
      }

      [Fact]
      public void TheLiveLog_SelectsLinesAndCopiesThemInTheLogsOrder()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml"));
         Assert.True(Regex.IsMatch(xaml, @"<ListBox x:Name=""LogList""[^>]*SelectionMode=""Extended"""),
            "The live log's list no longer takes several lines at once.");

         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         Assert.True(code.Contains("new CommandBinding(ApplicationCommands.Copy", StringComparison.Ordinal), "Ctrl+C no longer copies the selected lines.");
         Assert.True(code.Contains("LogList.ContextMenu = SelectableText.CopyMenu(", StringComparison.Ordinal), "The list's Copy menu is gone.");

         string textOf = WithheldRowTests.MemberText(code, "internal static string TextOf(IEnumerable<LogLine> lines, IEnumerable selected)");
         Assert.True(textOf != null, "LogsView.TextOf is gone.");
         // The log's order (walk the lines, keep the chosen), by reference (two lines
         // with the same words are two lines), each line's text as read, CR LF between.
         Assert.Contains("ReferenceEqualityComparer.Instance", textOf);
         Assert.Contains("lines.Where(line => chosen.Contains(line)).Select(line => line.Text)", textOf);
         Assert.Contains("string.Join(\"\\r\\n\",", textOf);

         // Whole lines only: a line the server is part-way through writing is read at
         // the next tick, or it is two rows and a copy of them has a line break the
         // log does not.
         string poll = WithheldRowTests.MemberText(code, "private void Poll()");
         Assert.True(poll != null, "LogsView.Poll is gone.");
         Assert.Contains("Array.LastIndexOf(bytes, (byte)'\\n', count - 1)", poll);
         Assert.Contains("position_ = from + end + 1;", poll);
      }

      /// <summary>
      /// The REST API keys page shows a new key once; its Copy button stays, says so when
      /// the clipboard cannot be written, and nothing on the page can show a stored key
      /// (the store holds a digest).
      /// </summary>
      [Fact]
      public void TheNewKey_KeepsItsCopyButton()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "ApiKeysView.cs"));
         Assert.Contains("Content = L(\"Copy to clip_board\")", code);
         Assert.Contains("if (!ClipboardWriter.TrySetText(newKeyToken_.Text, out string error))", code);
         Assert.Contains("The clipboard could not be written: {0}", code);
         Assert.Contains("newKeyToken_.Text = \"\";", code);
      }

      /// <summary>
      /// A selection dragged out of a text carries plain text only: the editor would add
      /// RTF and XAML in the theme's colours (white text on the dark theme), which Copy
      /// already leaves out.
      /// </summary>
      [Fact]
      public void ADragOutOfText_CarriesPlainTextOnly()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "SelectableText.Install is gone.");
         Assert.Contains("DataObject.SettingDataEvent", install);

         string handler = WithheldRowTests.MemberText(source, "private static void OnSettingData(object sender, DataObjectSettingDataEventArgs e)");
         Assert.True(handler != null, "SelectableText.OnSettingData is gone, so a drag out of a text carries RTF and XAML again.");
         Assert.Contains("e.CancelCommand();", handler);
         Assert.Contains("DataFormats.UnicodeText", handler);
      }

      // ---- the clipboard ---------------------------------------------------------------------

      /// <summary>
      /// Every Copy goes through ClipboardWriter. WPF's Clipboard.SetText flushes at once,
      /// retrying with the UI thread asleep, so a clipboard watcher that asks for the text
      /// in that moment waits on the sleeping thread: measured with a watcher that quick,
      /// it threw CLIPBRD_E_CANT_OPEN after a second on every copy - the chart's Copy showed
      /// no toast and the Control Panel froze for that second - while ClipboardWriter's
      /// copies all landed at once.
      /// </summary>
      [Fact]
      public void EveryCopy_GoesThroughClipboardWriter()
      {
         string root = ControlPanelDirectory("");
         Assert.True(root != null, "ControlPanel could not be found from the test binaries.");

         var offenders = new List<string>();
         foreach (string path in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.StartsWith("bin" + Path.DirectorySeparatorChar, StringComparison.Ordinal)
                || relative.StartsWith("obj" + Path.DirectorySeparatorChar, StringComparison.Ordinal)
                || relative == Path.Join("Services", "ClipboardWriter.cs"))
            {
               continue;
            }

            string code = File.ReadAllText(path);
            foreach (Match m in Regex.Matches(code, @"\bClipboard\s*\.\s*(SetText|SetDataObject|SetData|SetFileDropList|SetImage|SetAudio|Flush)\s*\("))
               offenders.Add(relative + ": " + m.Value);
         }

         Assert.True(offenders.Count == 0, "Written to the clipboard around ClipboardWriter: " + string.Join("; ", offenders));
      }

      /// <summary>
      /// The flush follows the copy on the dispatcher, so a copy made just before the
      /// Control Panel closes is flushed as it exits - measured, without it the text was
      /// gone after the exit, four times in four - and nothing asks Clipboard.IsCurrent,
      /// which retries asleep as SetText does.
      /// </summary>
      [Fact]
      public void ClipboardWriter_FlushesAsTheControlPanelExits()
      {
         string app = ReadControlPanelFile("App.xaml.cs");
         string onExit = WithheldRowTests.MemberText(app, "protected override void OnExit(ExitEventArgs e)");
         Assert.True(onExit != null && onExit.Contains("ClipboardWriter.FlushBeforeExit();", StringComparison.Ordinal),
            "App.OnExit no longer flushes the last copy, so text copied just before the Control Panel closes is lost.");

         string writer = ReadControlPanelFile(Path.Join("Services", "ClipboardWriter.cs"));
         Assert.Contains("Clipboard.SetDataObject(data, false);", writer);
         Assert.DoesNotMatch(@"Clipboard\.IsCurrent\s*\(", writer);
         Assert.DoesNotMatch(@"Clipboard\.SetText\s*\(", writer);
      }

      // ---- lists -------------------------------------------------------------------------

      [Fact]
      public void TheApplication_InstallsSelectableList_BeforeTheFirstWindow()
      {
         string app = ReadControlPanelFile("App.xaml.cs");
         string onStartup = WithheldRowTests.MemberText(app, "protected override void OnStartup(StartupEventArgs e)");
         Assert.True(onStartup != null && onStartup.Contains("SelectableList.Install();", StringComparison.Ordinal),
            "App.OnStartup no longer installs SelectableList, so no list's rows can be copied.");
      }

      /// <summary>
      /// Registered for ListBox, so for every ListView too; a list with its own Copy or
      /// menu (the live log) is left to it, and so is one inside something with a menu.
      /// With nothing chosen, Ctrl+C goes on as it did before a list could copy.
      /// </summary>
      [Fact]
      public void Lists_CopyTheirRows_UnlessTheyCopySomethingOfTheirOwn()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableList.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "SelectableList.Install is gone.");
         Assert.Contains("RegisterClassCommandBinding(typeof(ListBox)", install);
         Assert.Contains("RegisterClassHandler(typeof(ListBox), FrameworkElement.ContextMenuOpeningEvent", install);

         string copyable = WithheldRowTests.MemberText(source, "public static bool IsCopyable(ListBox list)");
         Assert.True(copyable != null, "SelectableList.IsCopyable is gone.");
         Assert.Contains("list.ContextMenu != null", copyable);
         Assert.Contains("binding.Command == ApplicationCommands.Copy", copyable);
         Assert.Contains("SelectableText.GetIsEnabled(list)", copyable);

         string canCopy = WithheldRowTests.MemberText(source, "private static void OnCanCopy(object sender, CanExecuteRoutedEventArgs e)");
         Assert.True(canCopy != null, "SelectableList.OnCanCopy is gone.");
         Assert.Contains("e.ContinueRouting = true;", canCopy);

         // The live log keeps its own Copy and menu.
         string logs = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         Assert.Contains("LogList.CommandBindings.Add(new CommandBinding(ApplicationCommands.Copy", logs);
         Assert.Contains("LogList.ContextMenu = SelectableText.CopyMenu(", logs);
      }

      /// <summary>
      /// The selected rows in the list's own order, one per line, each as shown: the
      /// pieces of text in a row tab-separated as a data grid's cells, without the words
      /// of the row's buttons or an icon's glyph, and a row out of view built again from
      /// the list's template (a list does not keep what it does not show).
      /// </summary>
      [Fact]
      public void ARowIsCopiedAsShown_InTheListsOrder()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableList.cs"));
         string rows = WithheldRowTests.MemberText(source, "public static string SelectedRowsText(ListBox list)");
         Assert.True(rows != null, "SelectableList.SelectedRowsText is gone.");
         Assert.Contains("SameItem.Instance", rows);
         Assert.Contains("foreach (object item in list.Items)", rows);
         Assert.Contains("string.Join(\"\\r\\n\", lines)", rows);

         string row = WithheldRowTests.MemberText(source, "private static string RowText_(ListBox list, object item, OffScreenReader_ reader)");
         Assert.True(row != null, "SelectableList.RowText_ is gone.");
         foreach (string path in new[] { "ContainerFromItem(item)", "view.View is GridView grid", "list.ItemTemplate != null", "list.DisplayMemberPath" })
            Assert.Contains(path, row);

         string interactive = WithheldRowTests.MemberText(source, "private static bool IsInteractive_(DependencyObject node)");
         Assert.True(interactive != null, "SelectableList.IsInteractive_ is gone.");
         foreach (string control in new[] { "ButtonBase", "TextBoxBase", "PasswordBox", "ComboBox", "ScrollBar", "Thumb" })
            Assert.Matches(@"\bnode is " + control + @"\b", interactive);
         Assert.Contains("Wpf.Ui.Controls.IconElement", source);
      }

      /// <summary>
      /// A copy of many rows builds each template once and hands it one row after another:
      /// built afresh for every row out of view, Select all and Ctrl+C on the domains page's
      /// accounts held the Control Panel for 2.5 seconds at 1,000 accounts, 8 at 5,000 and
      /// 24 at 20,000, and 0.1, 0.4 and 1.3 seconds so. The reader lives for one copy, so it
      /// keeps no row (and no page) after it; and a row that is an element itself is read
      /// where it is, never put into a presenter of the reader's, which it would take as
      /// its parent and so refuse the list's own when it came back into view.
      /// </summary>
      [Fact]
      public void ACopyOfManyRows_BuildsEachTemplateOnce()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableList.cs"));
         string rows = WithheldRowTests.MemberText(source, "public static string SelectedRowsText(ListBox list)");
         Assert.True(rows != null, "SelectableList.SelectedRowsText is gone.");
         Assert.Contains("var reader = new OffScreenReader_();", rows);
         Assert.Contains("RowText_(list, item, reader)", rows);

         string built = WithheldRowTests.MemberText(source, "public string Built(object key, object item, DataTemplate template, DataTemplateSelector selector)");
         Assert.True(built != null, "SelectableList.OffScreenReader_.Built is gone.");
         Assert.Contains("presenters_.TryGetValue(key, out ContentPresenter presenter)", built);
         Assert.Contains("presenter.Content = item;", built);

         string bound = WithheldRowTests.MemberText(source, "public string Bound(object key, object item, BindingBase binding)");
         Assert.True(bound != null, "SelectableList.OffScreenReader_.Bound is gone.");
         Assert.Contains("probes_.TryGetValue(key, out TextBlock probe)", bound);

         Assert.DoesNotMatch(@"static\s+OffScreenReader_\s", source);
         string row = WithheldRowTests.MemberText(source, "private static string RowText_(ListBox list, object item, OffScreenReader_ reader)");
         Assert.True(row != null, "SelectableList.RowText_ is gone.");
         Assert.Contains("view.View is GridView grid && item is not Visual && item is not Visual3D", row);
         Assert.True(row.IndexOf("item is Visual", StringComparison.Ordinal) >= 0
                     && row.IndexOf("item is Visual", StringComparison.Ordinal) < row.IndexOf("reader.Built(list,", StringComparison.Ordinal),
            "A row that is an element is put into the reader's presenter again.");
      }

      /// <summary>
      /// A row handed to the reader's template reads as the same row built afresh would. Two
      /// ways it did not, measured with templates of each kind against the old per-row build:
      /// an element further in that builds what it shows when measured (a ContentControl
      /// whose content is an object, or changes kind; an ItemsControl; a control the row
      /// before hid) kept the row before's, because Measure stops at the elements it finds
      /// unchanged and the reader's presenter is in no layout pass; and a row Equal to the
      /// one before (a type compared by a key) kept the one before's words, because WPF
      /// reads a binding again only for a data context that is not Equal to the old one.
      /// No list in the Control Panel had such a template or such rows; every list is
      /// registered, so the next one would have copied other rows' words.
      /// </summary>
      [Fact]
      public void AHandedOverRow_ReadsAsOneBuiltAfresh()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableList.cs"));

         string built = WithheldRowTests.MemberText(source, "public string Built(object key, object item, DataTemplate template, DataTemplateSelector selector)");
         Assert.True(built != null, "SelectableList.OffScreenReader_.Built is gone.");
         Assert.Contains("if (EqualButAnother_(presenter.Content, item))", built);
         Assert.Contains("presenter.Content = null;", built);
         Assert.Contains("MeasureWhole_(presenter);", built);
         Assert.True(built.IndexOf("MeasureWhole_(presenter);", StringComparison.Ordinal) > built.IndexOf("presenter.Content = item;", StringComparison.Ordinal)
                     && built.IndexOf("MeasureWhole_(presenter);", StringComparison.Ordinal) < built.IndexOf("TextsIn_(presenter, presenter)", StringComparison.Ordinal),
            "The row is measured whole after it is handed over and before it is read.");

         string bound = WithheldRowTests.MemberText(source, "public string Bound(object key, object item, BindingBase binding)");
         Assert.True(bound != null, "SelectableList.OffScreenReader_.Bound is gone.");
         Assert.Contains("if (EqualButAnother_(probe.DataContext, item))", bound);
         Assert.Contains("probe.DataContext = null;", bound);

         string equal = WithheldRowTests.MemberText(source, "private static bool EqualButAnother_(object before, object item)");
         Assert.True(equal != null, "SelectableList.OffScreenReader_.EqualButAnother_ is gone.");
         Assert.Contains("!ReferenceEquals(before, item) && Equals(before, item)", equal);

         string whole = WithheldRowTests.MemberText(source, "private void MeasureWhole_(ContentPresenter presenter)");
         Assert.True(whole != null, "SelectableList.OffScreenReader_.MeasureWhole_ is gone.");
         Assert.Contains("Unmeasured_(presenter, presenter, unmeasured_);", whole);
         Assert.Contains("InvalidateMeasure()", whole);

         string unmeasured = WithheldRowTests.MemberText(source, "private static void Unmeasured_(DependencyObject node, DependencyObject row, List<UIElement> found)");
         Assert.True(unmeasured != null, "SelectableList.OffScreenReader_.Unmeasured_ is gone.");
         foreach (string kind in new[] { "!stale.IsMeasureValid", "node is Control", "node is ContentPresenter", "node is ItemsPresenter", "IsItemsHost: true" })
            Assert.Contains(kind, unmeasured);
      }

      /// <summary>
      /// A list view's row is read column by column, in the columns' order, on screen or not.
      /// Read from the screen, a row gave its cells in the order they were made and nothing
      /// for an empty one, so one copy of the AD picker's rows mixed two layouts: a row on
      /// screen with no display name had its e-mail address under Name, and with the E-mail
      /// column dragged to the front the rows on screen kept the old order and those out of
      /// view took the new.
      /// </summary>
      [Fact]
      public void AListViewsRow_IsReadColumnByColumn_OnScreenOrNot()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableList.cs"));
         string row = WithheldRowTests.MemberText(source, "private static string RowText_(ListBox list, object item, OffScreenReader_ reader)");
         Assert.True(row != null, "SelectableList.RowText_ is gone.");
         int grid = row.IndexOf("view.View is GridView grid", StringComparison.Ordinal);
         int screen = row.IndexOf("ContainerFromItem(item)", StringComparison.Ordinal);
         Assert.True(grid >= 0 && screen >= 0 && grid < screen,
            "A list view's row on screen is read from the screen again, not column by column.");
         Assert.Contains("grid.Columns.Select(column => column.DisplayMemberBinding != null", row);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static string ControlPanelDirectory(string child)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", child);
            if (Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

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

      private static Dictionary<string, string> ReadResx(string path)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }
   }
}
