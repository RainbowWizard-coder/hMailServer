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
   /// copied, and the live log's lines too (Services/SelectableText.cs,
   /// Views/LogsView.xaml.cs).
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
      /// The REST API keys page shows a new key once; its Copy button stays exactly as it
      /// was, and nothing on the page can show a stored key (the store holds a digest).
      /// </summary>
      [Fact]
      public void TheNewKey_KeepsItsCopyButton()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "ApiKeysView.cs"));
         Assert.Contains("Content = L(\"Copy to clip_board\")", code);
         Assert.Contains("Clipboard.SetText(newKeyToken_.Text);", code);
         Assert.Contains("newKeyToken_.Text = \"\";", code);
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
