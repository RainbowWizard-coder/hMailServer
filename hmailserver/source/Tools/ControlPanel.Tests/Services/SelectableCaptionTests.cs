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
   /// Issue #48, third round: the words on the controls themselves - a check box's or a
   /// radio button's caption, a button's, a drop-down's value, a tab's name, a tree item's,
   /// a label's - are copied from a right-click Copy or Ctrl+C on the control
   /// (Services/SelectableCaption.cs).
   ///
   /// The behaviour is WPF's and is not rendered here - this assembly is WPF-free - so, as
   /// in SelectableTextTests, what is held is the wiring a rendered test would only catch
   /// after the fact, scanned from the source. Each assertion names the regression it
   /// stands for; the WPF harness of the commit that added them measured each.
   /// </summary>
   public class SelectableCaptionTests
   {
      private static string Source => ReadControlPanelFile(Path.Join("Services", "SelectableCaption.cs"));

      private static string Member(string signature)
      {
         string member = WithheldRowTests.MemberText(Source, signature);
         Assert.True(member != null, "SelectableCaption: \"" + signature + "\" is gone.");
         return member;
      }

      [Fact]
      public void TheApplication_InstallsSelectableCaption_BeforeTheFirstWindow()
      {
         string app = ReadControlPanelFile("App.xaml.cs");
         string onStartup = WithheldRowTests.MemberText(app, "protected override void OnStartup(StartupEventArgs e)");
         Assert.True(onStartup != null, "App.OnStartup is gone.");
         Assert.True(onStartup.Contains("SelectableCaption.Install();", StringComparison.Ordinal),
            "App.OnStartup no longer installs SelectableCaption, so no check box's, button's, drop-down's, tab's or label's words can be copied.");
      }

      /// <summary>
      /// Registered for the classes, so every page and dialogue has it, and the ones written
      /// after it: ButtonBase covers buttons, toggles, check boxes, radio buttons, link
      /// buttons, the cards that act as buttons, an expander's header and a grid's column
      /// headers. A label is there because its words are an AccessText's, which a press
      /// reaches without passing the text block SelectableText makes selectable.
      /// </summary>
      [Fact]
      public void EveryKindOfCaption_IsRegisteredForTheMenuAndTheKey()
      {
         string install = Member("public static void Install()");
         foreach (string kind in new[] { "typeof(ButtonBase)", "typeof(ComboBox)", "typeof(TabItem)", "typeof(TreeViewItem)", "typeof(Label)" })
            Assert.True(install.Contains(kind, StringComparison.Ordinal), kind + " is no longer registered, so its caption cannot be copied.");

         Assert.Contains("FrameworkElement.ContextMenuOpeningEvent", install);
         Assert.Contains("UIElement.KeyDownEvent", install);
      }

      /// <summary>
      /// Ctrl+C is the key on the control, not the Copy command. Answering the command made
      /// every control with a caption answer any Copy routed to whatever has the keyboard:
      /// measured, the Copy on a drop-down's menu then copied the caption of the button that
      /// had the keyboard ("_Plain button"), where it had been disabled.
      /// </summary>
      [Fact]
      public void CtrlC_IsTheKeyOnTheControl_NotTheCopyCommand()
      {
         Assert.DoesNotMatch(@"RegisterClassCommandBinding", Source);
         Assert.DoesNotMatch(@"new CommandBinding\(", Source);

         string key = Member("private static void OnKeyDown(object sender, KeyEventArgs e)");
         Assert.Contains("ReferenceEquals(e.OriginalSource, control)", key);
         Assert.Contains("IsCopyGesture_(control, e)", key);

         string gesture = Member("private static bool IsCopyGesture_(Control control, KeyEventArgs e)");
         Assert.Contains("ApplicationCommands.Copy.InputGestures", gesture);
      }

      /// <summary>
      /// Ctrl+C on a control is taken only where nothing around it would copy - measured, a
      /// button in a data grid's chosen row leaves the key to the grid (the row's cells), in
      /// a list's chosen row to the list (the row's words) - and never from an editable
      /// drop-down's text box, whose Copy is its own.
      /// </summary>
      [Fact]
      public void CtrlC_LeavesAGridsAListsAndATextBoxsCopyToThem()
      {
         string key = Member("private static void OnKeyDown(object sender, KeyEventArgs e)");
         Assert.Contains("AncestorCopies_(control)", key);
         Assert.Contains("ComboBox { IsEditable: true }", key);
         Assert.True(key.IndexOf("AncestorCopies_(control)", StringComparison.Ordinal) < key.IndexOf("SelectableText.SetClipboard(", StringComparison.Ordinal),
            "The caption is copied before asking whether a grid or a list around the control would copy.");

         string ancestors = Member("private static bool AncestorCopies_(Control control)");
         Assert.Contains("ApplicationCommands.Copy.CanExecute(null, parent)", ancestors);
      }

      /// <summary>
      /// A control with a menu of its own keeps it, and so does one inside something with a
      /// menu; a container opts out with SelectableText.IsEnabled; a button whose command is
      /// Copy copies something else; a drop-down's own arrow is the drop-down's; a menu
      /// opening over a light-dismiss popup would close it (measured: the popup stays open).
      /// </summary>
      [Fact]
      public void WhatIsLeftAlone_IsLeftAlone()
      {
         string copyable = Member("public static bool IsCopyable(Control control)");
         foreach (string rule in new[]
         {
            "SelectableText.GetIsEnabled(control)", "ContextMenuService.GetIsEnabled(control)", "control.ContextMenu != null",
            "element.ContextMenu != null", "ReferenceEquals(source.Command, ApplicationCommands.Copy)",
            "control.TemplatedParent is ComboBox", "ComboBox { IsDropDownOpen: true }", "Popup { StaysOpen: false }",
            "node is MenuItem", "node is ContextMenu", "node is ToolTip", "Wpf.Ui.Controls.TitleBar"
         })
         {
            Assert.True(copyable.Contains(rule, StringComparison.Ordinal), "IsCopyable no longer checks " + rule + ".");
         }

         string surface = Member("private static bool FromTheControl_(Control control, DependencyObject source)");
         Assert.Contains("node is TextBoxBase", surface);
         Assert.Contains("node is Popup", surface);
         Assert.Contains("TemplatedParent, control", surface);
      }

      /// <summary>
      /// The theme gives every drop-down the menu it gives a text box - Copy, Cut and Paste,
      /// in the runtime's words, sending the commands to whatever has the keyboard - and on a drop-down
      /// that cannot be typed into none of them can run (measured: all three disabled, with
      /// the drop-down focused or not). There, and only there, Copy replaces it: a menu set
      /// on the drop-down itself, or one with anything but the text commands on it, stays.
      /// </summary>
      [Fact]
      public void TheThemesTextMenu_IsReplacedOnlyOnADropDownThatCannotBeTypedInto()
      {
         string theme = Member("private static bool IsThemeTextMenuOnADropDown_(Control control)");
         Assert.Contains("ComboBox { IsEditable: false }", theme);
         Assert.Contains("BaseValueSource.Local", theme);
         Assert.Contains("Array.IndexOf(TextEditingCommands, command) < 0", theme);

         string commands = Member("private static readonly ICommand[] TextEditingCommands =");
         foreach (string command in new[] { "Copy", "Cut", "Paste" })
            Assert.Contains("ApplicationCommands." + command, commands);

         string copyable = Member("public static bool IsCopyable(Control control)");
         Assert.Contains("control.ContextMenu != null && !IsThemeTextMenuOnADropDown_(control)", copyable);
      }

      /// <summary>
      /// No text, no menu: an icon-only button, a glyph, a drop-down with nothing chosen. The
      /// right-click then goes on as it did - but not to the theme's dead text menu.
      /// </summary>
      [Fact]
      public void NothingToCopy_OpensNoMenu()
      {
         string opening = Member("private static void OnContextMenuOpening(object sender, ContextMenuEventArgs e)");
         int empty = opening.IndexOf("if (caption.Length == 0)", StringComparison.Ordinal);
         int menu = opening.IndexOf("SelectableText.CopyMenu(", StringComparison.Ordinal);
         Assert.True(empty >= 0 && menu > empty, "A menu is opened for a control with nothing to copy.");

         string key = Member("private static void OnKeyDown(object sender, KeyEventArgs e)");
         Assert.Contains("caption.Length == 0", key);
      }

      /// <summary>
      /// The menu is the one the text has - SelectableText.CopyMenu, "_Copy" in all seventeen
      /// languages - so this adds no caption to translate; from the keyboard (the Menu key,
      /// Shift+F10) it opens under the control, since there is no pointer to open at.
      /// </summary>
      [Fact]
      public void TheMenu_IsTheTextsCopy_AndOpensFromTheKeyboard()
      {
         Assert.Contains("SelectableText.CopyMenu(", Source);
         Assert.DoesNotMatch(@"\bL\(""", Source);
         Assert.DoesNotMatch(@"\bF\(""", Source);

         string opening = Member("private static void OnContextMenuOpening(object sender, ContextMenuEventArgs e)");
         Assert.Contains("e.CursorLeft < 0", opening);
         Assert.Contains("PlacementMode.Bottom", opening);
         Assert.Contains("e.Handled = true;", opening);
      }

      /// <summary>
      /// Nothing is added to or changed on a control - no menu set on it, nothing made
      /// focusable or a tab stop, no automation property - so the automation tree and the tab
      /// order are what they were (measured: identical dumps before and after).
      /// </summary>
      [Fact]
      public void NothingIsAddedToAControl()
      {
         Assert.DoesNotMatch(@"\.ContextMenu\s*=[^=]", Source);
         Assert.DoesNotMatch(@"Focusable\s*=", Source);
         Assert.DoesNotMatch(@"IsTabStop", Source);
         Assert.DoesNotMatch(@"AutomationProperties", Source);
         Assert.DoesNotMatch(@"\.Focus\(\)", Source);
      }

      /// <summary>
      /// The caption is what the control shows, read from the presenter its template shows it
      /// with: a drop-down's selection box (so DisplayMemberPath, an item template and ToString
      /// all copy as shown), without an icon's glyph (Wpf.Ui's symbols, a symbol font's
      /// private-use characters), and without the access key's underscore - which the text
      /// block an AccessText draws with still holds (measured: .NET 10's WPF copied
      /// "E_nable" for "Enable"). The underscore is taken away by WPF's own rule
      /// (MnemonicText.AccessTextShown), not by MnemonicText.Strip, which keeps one before a
      /// space: WPF draws "odd_ one" as "odd one", and the copy was "odd_ one".
      /// </summary>
      [Fact]
      public void TheCaption_IsWhatTheControlShows()
      {
         string combo = Member("private static string ComboText_(ComboBox combo)");
         Assert.Contains("PresenterOf_(combo, combo.SelectionBoxItem)", combo);
         Assert.Contains("combo.IsEditable", combo);

         string collect = Member("private static void Collect_(DependencyObject node, DependencyObject root, List<(UIElement, string)> pieces)");
         Assert.Contains("Wpf.Ui.Controls.IconElement", collect);
         Assert.Contains("MnemonicText.AccessTextShown(access.Text)", collect);
         Assert.DoesNotContain("MnemonicText.Strip(", Source);
         Assert.True(collect.IndexOf("node is AccessText", StringComparison.Ordinal) < collect.IndexOf("node is TextBlock", StringComparison.Ordinal),
            "An AccessText's own text block is read before its caption, so an access key's underscore is copied.");

         string glyph = Member("private static bool IsGlyphOnly_(string text)");
         Assert.Matches(@"c >= '\\uE000' && c <= '\\uF8FF'", glyph);

         string presenter = Member("private static ContentPresenter PresenterOf_(Control control, object value)");
         Assert.Contains("ReferenceEquals(presenter.TemplatedParent, control)", presenter);
      }

      /// <summary>
      /// The menu names Copy's key (Ctrl+C), to the eye and to a screen reader, only where the
      /// key copies the caption: a label, a grid's column header and a disabled control never
      /// have the keyboard, and on a button in a data grid's or a list's chosen row the key
      /// copies the rows. Measured before: every caption's menu said Ctrl+C, and on a label the
      /// key then copied whatever had the keyboard.
      /// </summary>
      [Fact]
      public void TheMenuShowsCopysKey_OnlyWhereTheKeyCopiesTheCaption()
      {
         string opening = Member("private static void OnContextMenuOpening(object sender, ContextMenuEventArgs e)");
         Assert.Contains("!KeyCopiesCaption_(control)", opening);
         Assert.Contains("InputGestureText = \"\"", opening);

         string key = Member("private static bool KeyCopiesCaption_(Control control)");
         foreach (string rule in new[] { "control.Focusable", "control.IsEnabled", "ComboBox { IsEditable: true }", "!AncestorCopies_(control)" })
            Assert.True(key.Contains(rule, StringComparison.Ordinal), "KeyCopiesCaption_ no longer checks " + rule + ", so the menu names a key that copies something else.");
      }

      // ---- helpers -------------------------------------------------------------------------

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
