// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The words WPF shows by itself - Copy, Cut and Paste on a text box's menu, a scroll
   /// bar's menu - in the Control Panel's language (Services/FrameworkWords.cs). WPF has
   /// none in Danish, Finnish, Norwegian, Dutch, Swedish or Ukrainian, so in those both
   /// menus were English; measured in Swedish before this, "Copy, Cut, Paste" and
   /// "Scroll Here, Top, Bottom, Page Up, Page Down, Scroll Up, Scroll Down".
   ///
   /// FrameworkWords is WPF and this assembly is not, so what is held is what it rests
   /// on: it is installed after the language is chosen, only for a language the
   /// catalogue has; its scroll bar menu is WPF's item for item, as WPF's own was read
   /// out of a running Control Panel; and every language names each item differently -
   /// WPF's German names both Top and Page Up "Nach oben".
   /// </summary>
   public class FrameworkWordsTests
   {
      // WPF's scroll bar menus as .NET 10.0.11's WPF builds them (read from a right-click
      // in the Control Panel, 1 October 2026): header, automation id and command, with a
      // separator after the first item and after each pair.
      private static readonly (string Header, string Id, string Command)[] Vertical =
      {
         ("Scroll Here", "ScrollHere", "ScrollHereCommand"),
         ("Top", "Top", "ScrollToTopCommand"),
         ("Bottom", "Bottom", "ScrollToBottomCommand"),
         ("Page Up", "PageUp", "PageUpCommand"),
         ("Page Down", "PageDown", "PageDownCommand"),
         ("Scroll Up", "ScrollUp", "LineUpCommand"),
         ("Scroll Down", "ScrollDown", "LineDownCommand"),
      };

      private static readonly (string Header, string Id, string Command)[] Horizontal =
      {
         ("Scroll Here", "ScrollHere", "ScrollHereCommand"),
         ("Left Edge", "LeftEdge", "ScrollToLeftEndCommand"),
         ("Right Edge", "RightEdge", "ScrollToRightEndCommand"),
         ("Page Left", "PageLeft", "PageLeftCommand"),
         ("Page Right", "PageRight", "PageRightCommand"),
         ("Scroll Left", "ScrollLeft", "LineLeftCommand"),
         ("Scroll Right", "ScrollRight", "LineRightCommand"),
      };

      [Fact]
      public void TheApplication_InstallsFrameworkWords_AfterTheLanguageAndBeforeTheFirstWindow()
      {
         string app = ReadControlPanelFile("App.xaml.cs");
         string onStartup = WithheldRowTests.MemberText(app, "protected override void OnStartup(StartupEventArgs e)");
         Assert.True(onStartup != null, "App.OnStartup is gone.");

         int language = onStartup.IndexOf("LanguageChoice.Apply();", StringComparison.Ordinal);
         int words = onStartup.IndexOf("FrameworkWords.Install();", StringComparison.Ordinal);
         Assert.True(words >= 0, "App.OnStartup no longer installs FrameworkWords, so WPF's menus are English in six languages.");
         Assert.True(language >= 0 && words > language, "FrameworkWords is installed before the language is chosen, and reads English.");
      }

      [Fact]
      public void OnlyALanguageWithACatalogue_TakesTheCataloguesWords()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "FrameworkWords.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "FrameworkWords.Install is gone.");

         int guard = install.IndexOf("if (!Translated)", StringComparison.Ordinal);
         Assert.True(guard >= 0, "FrameworkWords no longer leaves WPF's words alone in English and in a language with no catalogue.");
         foreach (string set in new[] { "ApplicationCommands.Copy.Text = L(\"Copy\");", "ApplicationCommands.Cut.Text = L(\"Cut\");",
                                         "ApplicationCommands.Paste.Text = L(\"Paste\");", "typeof(ScrollBar), FrameworkElement.ContextMenuOpeningEvent" })
         {
            int at = install.IndexOf(set, StringComparison.Ordinal);
            Assert.True(at > guard, set + " is gone from FrameworkWords.Install, or comes before the language is asked.");
         }

         // A scroll bar with a menu of its own (or ours, from an earlier right-click) keeps it.
         string opening = WithheldRowTests.MemberText(source, "private static void OnScrollBarMenuOpening(object sender, ContextMenuEventArgs e)");
         Assert.True(opening != null, "FrameworkWords.OnScrollBarMenuOpening is gone.");
         Assert.Contains("BaseValueSource != BaseValueSource.Default", opening);
      }

      [Fact]
      public void TheScrollBarMenu_IsWpfsItemForItem()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "FrameworkWords.cs"));
         string menu = WithheldRowTests.MemberText(source, "public static ContextMenu ScrollBarMenu(Orientation orientation)");
         Assert.True(menu != null, "FrameworkWords.ScrollBarMenu is gone.");

         int split = menu.IndexOf("if (orientation == Orientation.Vertical)", StringComparison.Ordinal);
         int otherwise = menu.IndexOf("else", split, StringComparison.Ordinal);
         Assert.True(split > 0 && otherwise > split, "ScrollBarMenu no longer builds a menu for each orientation.");

         string first = menu[..split];
         string vertical = menu[split..otherwise];
         string horizontal = menu[otherwise..];
         Assert.Equal(Vertical, Items_(first).Concat(Items_(vertical)).ToArray());
         Assert.Equal(Horizontal, Items_(first).Concat(Items_(horizontal)).ToArray());
         Assert.Equal(1, Separators_(first));
         Assert.Equal(2, Separators_(vertical));
         Assert.Equal(2, Separators_(horizontal));

         // Each item's command goes to the scroll bar the menu was opened on, as WPF's do.
         string item = WithheldRowTests.MemberText(source, "private static MenuItem Item_(string header, string automationId, RoutedCommand command)");
         Assert.True(item != null, "FrameworkWords.Item_ is gone.");
         Assert.Contains("AutomationProperties.SetAutomationId(item, automationId);", item);
         Assert.Contains("Path = new PropertyPath(ContextMenu.PlacementTargetProperty)", item);
         Assert.Contains("new RelativeSource(RelativeSourceMode.FindAncestor, typeof(ContextMenu), 1)", item);
         Assert.Contains("item.SetBinding(MenuItem.CommandTargetProperty", item);
      }

      /// <summary>
      /// One menu, so in every language each of its items has its own name: WPF's German
      /// calls Top and Page Up both "Nach oben", and Bottom "Unten" beside "Nach unten".
      /// The text box's three commands likewise.
      /// </summary>
      [Fact]
      public void EveryLanguage_NamesEachItemOfAMenuDifferently()
      {
         string resources = ControlPanelDirectory("Resources");
         Assert.True(resources != null, "ControlPanel/Resources could not be found from the test binaries.");

         string[] catalogues = Directory.GetFiles(resources, "Strings.*.resx");
         Assert.True(catalogues.Length >= 17, "Expected seventeen languages, found " + catalogues.Length + ".");

         foreach (string path in catalogues)
         {
            Dictionary<string, string> strings = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "");
            string name = Path.GetFileName(path);

            foreach (string[] menu in new[]
                     {
                        Vertical.Select(i => i.Header).ToArray(),
                        Horizontal.Select(i => i.Header).ToArray(),
                        new[] { "Copy", "Cut", "Paste" },
                     })
            {
               var words = menu.Select(english => strings.TryGetValue(english, out string word) ? word.Trim() : "").ToList();
               Assert.True(words.All(w => w.Length > 0), name + " lacks a word of the menu " + string.Join(", ", menu) + ".");
               var twice = words.GroupBy(w => w, StringComparer.CurrentCultureIgnoreCase).Where(g => g.Count() > 1).Select(g => g.Key).ToList();
               Assert.True(twice.Count == 0, name + " names two items of one menu " + string.Join(" and ", twice) + ".");
            }
         }
      }

      // The calendar's two paging buttons, by view: (view, previous, next).
      private static readonly (string View, string Previous, string Next)[] CalendarButtons =
      {
         ("Month", "Previous month", "Next month"),
         ("Year", "Previous year", "Next year"),
         ("Decade", "Previous decade", "Next decade"),
      };

      /// <summary>
      /// The theme names a date picker calendar's two paging buttons "Previous" and "Next"
      /// for a screen reader, in English in every language and every view (measured in
      /// English, Swedish, German, Japanese and Korean). They are named from the catalogue
      /// in every language, English too - the theme's are English everywhere, so there is
      /// nothing of WPF's to keep - by what they page in the view shown, through a binding
      /// to the calendar's view, so a switch to years or decades renames them at once.
      /// </summary>
      [Fact]
      public void TheCalendarsPagingButtons_AreNamedByWhatTheyPage_InEveryLanguage()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "FrameworkWords.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "FrameworkWords.Install is gone.");
         // SizeChanged, which a new button raises as it is first laid out and a class
         // handler hears; Loaded's class handler is not called on the calendar (measured).
         int calendar = install.IndexOf("RegisterClassHandler(typeof(ButtonBase), FrameworkElement.SizeChangedEvent", StringComparison.Ordinal);
         int guard = install.IndexOf("if (!Translated)", StringComparison.Ordinal);
         Assert.True(calendar >= 0, "FrameworkWords no longer names the calendar's paging buttons as they are laid out.");
         Assert.True(guard > calendar, "The calendar's buttons are named only in a translated language, so English and Korean keep the theme's \"Previous\" and \"Next\" for every view.");
         Assert.DoesNotContain("LoadedEvent", install);

         string sized = WithheldRowTests.MemberText(source, "private static void OnButtonSized(object sender, SizeChangedEventArgs e)");
         Assert.True(sized != null, "FrameworkWords.OnButtonSized is gone.");
         Assert.Contains("button.TemplatedParent is not CalendarItem item || item.TemplatedParent is not Calendar calendar", sized);
         Assert.Matches(@"if \(button\.Name == ""PART_PreviousButton""\)\s*NameCalendarButton_\(button, calendar, PreviousName\);", sized);
         Assert.Matches(@"else if \(button\.Name == ""PART_NextButton""\)\s*NameCalendarButton_\(button, calendar, NextName\);", sized);
         Assert.Matches(@"PreviousName = new\(next: false\);", source);
         Assert.Matches(@"NextName = new\(next: true\);", source);

         string bind = WithheldRowTests.MemberText(source, "private static void NameCalendarButton_(ButtonBase button, Calendar calendar, CalendarButtonName name)");
         Assert.True(bind != null, "FrameworkWords.NameCalendarButton_ is gone.");
         Assert.Contains("button.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(Calendar.DisplayMode))", bind);
         Assert.Contains("Source = calendar", bind);
         Assert.Contains("Converter = name", bind);

         string names = WithheldRowTests.MemberText(source, "public static string CalendarButton(CalendarMode view, bool next)");
         Assert.True(names != null, "FrameworkWords.CalendarButton is gone.");
         var found = new List<(string, string, string)>();
         foreach (Match m in Regex.Matches(names, @"case CalendarMode\.(\w+):\s*return next \? L\(""([^""]+)""\) : L\(""([^""]+)""\);"))
            found.Add((m.Groups[1].Value, m.Groups[3].Value, m.Groups[2].Value));
         Match month = Regex.Match(names, @"default:\s*return next \? L\(""([^""]+)""\) : L\(""([^""]+)""\);");
         Assert.True(month.Success, "CalendarButton no longer names the month view's buttons by default.");
         found.Insert(0, ("Month", month.Groups[2].Value, month.Groups[1].Value));
         Assert.Equal(CalendarButtons, found.ToArray());

         int converter = source.IndexOf("class CalendarButtonName", StringComparison.Ordinal);
         Assert.True(converter >= 0, "The calendar button's converter is gone.");
         string convert = WithheldRowTests.MemberText(source[converter..], "public object Convert(object value, System.Type targetType, object parameter, System.Globalization.CultureInfo culture)");
         Assert.True(convert != null, "The calendar button's converter has no Convert.");
         Assert.Contains("CalendarButton(value is CalendarMode view ? view : CalendarMode.Month, next_)", convert);
      }

      /// <summary>
      /// The theme gives the window's own buttons in the title bar no name: Minimise,
      /// Maximise and Close were each just "button" to a screen reader, in every language
      /// (measured through UI Automation's client). They are named in every language by
      /// the button's type, through a binding, since the title bar switches its middle
      /// button between Maximise and Restore down as the window is maximised and restored.
      /// </summary>
      [Fact]
      public void TheWindowsOwnButtons_AreNamedByTheirType_InEveryLanguage()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "FrameworkWords.cs"));
         string install = WithheldRowTests.MemberText(source, "public static void Install()");
         Assert.True(install != null, "FrameworkWords.Install is gone.");
         int sized = install.IndexOf("RegisterClassHandler(typeof(ButtonBase), FrameworkElement.SizeChangedEvent", StringComparison.Ordinal);
         Assert.True(sized >= 0 && install.IndexOf("if (!Translated)", StringComparison.Ordinal) > sized,
            "The title bar's buttons are no longer named in every language.");

         string handler = WithheldRowTests.MemberText(source, "private static void OnButtonSized(object sender, SizeChangedEventArgs e)");
         Assert.True(handler != null, "FrameworkWords.OnButtonSized is gone.");
         Assert.Contains("if (sender is Wpf.Ui.Controls.TitleBarButton windowButton)", handler);
         Assert.Contains("windowButton.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(Wpf.Ui.Controls.TitleBarButton.ButtonType))", handler);
         Assert.Contains("Source = windowButton", handler);
         Assert.Contains("Converter = WindowButtonNames", handler);
         Assert.Contains("if (Unnamed_(windowButton))", handler);

         string names = WithheldRowTests.MemberText(source, "public static string WindowButton(Wpf.Ui.Controls.TitleBarButtonType type)");
         Assert.True(names != null, "FrameworkWords.WindowButton is gone.");
         var found = Regex.Matches(names, @"case Wpf\.Ui\.Controls\.TitleBarButtonType\.(\w+):\s*return L\(""([^""]+)""\);")
            .Select(m => (m.Groups[1].Value, m.Groups[2].Value)).ToArray();
         Assert.Equal(new[] { ("Minimize", "Minimise"), ("Maximize", "Maximise"), ("Restore", "Restore down"), ("Close", "Close"), ("Help", "Help") }, found);

         int converter = source.IndexOf("class WindowButtonName", StringComparison.Ordinal);
         Assert.True(converter >= 0, "The title bar button's converter is gone.");
         string convert = WithheldRowTests.MemberText(source[converter..],
            "public object Convert(object value, System.Type targetType, object parameter, System.Globalization.CultureInfo culture)");
         Assert.True(convert != null, "The title bar button's converter has no Convert.");
         Assert.Contains("value is Wpf.Ui.Controls.TitleBarButtonType type ? WindowButton(type)", convert);
      }

      /// <summary>
      /// Every catalogue names the title bar's buttons, each differently: "Restore down" is
      /// a key of its own, not the backup page's "Restore", which German says
      /// "Wiederherstellen" and its own title bar "Verkleinern".
      /// </summary>
      [Fact]
      public void EveryLanguage_NamesTheWindowsButtonsDifferently()
      {
         string resources = ControlPanelDirectory("Resources");
         Assert.True(resources != null, "ControlPanel/Resources could not be found from the test binaries.");

         string[] catalogues = Directory.GetFiles(resources, "Strings.*.resx");
         Assert.True(catalogues.Length >= 17, "Expected seventeen languages, found " + catalogues.Length + ".");
         foreach (string path in catalogues)
         {
            Dictionary<string, string> strings = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "");
            string name = Path.GetFileName(path);
            var words = new[] { "Minimise", "Maximise", "Restore down", "Close" }
               .Select(e => strings.TryGetValue(e, out string word) ? word.Trim() : "").ToList();
            Assert.True(words.All(w => w.Length > 0), name + " lacks a name of the title bar's buttons.");
            var twice = words.GroupBy(w => w, StringComparer.CurrentCultureIgnoreCase).Where(g => g.Count() > 1).Select(g => g.Key).ToList();
            Assert.True(twice.Count == 0, name + " gives two of the title bar's buttons the name " + string.Join(" and ", twice) + ".");
         }
      }

      /// <summary>The six names are in every catalogue, and no two of them are the same.</summary>
      [Fact]
      public void EveryLanguage_NamesTheCalendarsButtonsDifferently()
      {
         string resources = ControlPanelDirectory("Resources");
         Assert.True(resources != null, "ControlPanel/Resources could not be found from the test binaries.");

         string[] catalogues = Directory.GetFiles(resources, "Strings.*.resx");
         Assert.True(catalogues.Length >= 17, "Expected seventeen languages, found " + catalogues.Length + ".");
         string[] english = CalendarButtons.SelectMany(b => new[] { b.Previous, b.Next }).ToArray();

         foreach (string path in catalogues)
         {
            Dictionary<string, string> strings = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "");
            string name = Path.GetFileName(path);
            var words = english.Select(e => strings.TryGetValue(e, out string word) ? word.Trim() : "").ToList();
            Assert.True(words.All(w => w.Length > 0), name + " lacks a name of the calendar's buttons.");
            var twice = words.GroupBy(w => w, StringComparer.CurrentCultureIgnoreCase).Where(g => g.Count() > 1).Select(g => g.Key).ToList();
            Assert.True(twice.Count == 0, name + " gives two of the calendar's buttons the name " + string.Join(" and ", twice) + ".");
         }
      }

      /// <summary>
      /// The navigation's chevrons and a password box's eye had no name to a screen
      /// reader in any language (measured through UI Automation's client: nine buttons
      /// "Expander" and one "RevealButton", each named ''). Each is named as it is first
      /// laid out, by a binding that follows its state: the chevron by its group's own
      /// name and whether the group is open, the eye by whether the password shows.
      /// </summary>
      [Fact]
      public void TheNavigationsChevronsAndAPasswordsEye_AreNamedByWhatTheyDo()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "FrameworkWords.cs"));
         string handler = WithheldRowTests.MemberText(source, "private static void OnButtonSized(object sender, SizeChangedEventArgs e)");
         Assert.True(handler != null, "FrameworkWords.OnButtonSized is gone.");

         Assert.Contains("if (sender is ToggleButton expander && expander.Name == \"Expander\" && expander.TemplatedParent is TreeViewItem group)", handler);
         Assert.Contains("if (Unnamed_(expander))", handler);
         Assert.Contains("new MultiBinding { Converter = ExpanderButtonNames, Mode = BindingMode.OneWay }", handler);
         Assert.Contains("name.Bindings.Add(new Binding(nameof(TreeViewItem.IsExpanded)) { Source = group, Mode = BindingMode.OneWay });", handler);
         Assert.Contains("Path = new PropertyPath(AutomationProperties.NameProperty)", handler);
         Assert.Contains("expander.SetBinding(AutomationProperties.NameProperty, name);", handler);

         Assert.Contains("if (sender is ButtonBase reveal && reveal.Name == \"RevealButton\" && reveal.TemplatedParent is Wpf.Ui.Controls.PasswordBox passwordBox)", handler);
         Assert.Contains("if (Unnamed_(reveal))", handler);
         Assert.Contains("reveal.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(Wpf.Ui.Controls.PasswordBox.IsPasswordRevealed))", handler);
         Assert.Contains("Converter = RevealButtonNames", handler);

         // Both before the calendar's test, which returns for any other button.
         int calendar = handler.IndexOf("button.TemplatedParent is not CalendarItem item", StringComparison.Ordinal);
         Assert.True(calendar > handler.IndexOf("expander.Name == \"Expander\"", StringComparison.Ordinal) &&
                     calendar > handler.IndexOf("reveal.Name == \"RevealButton\"", StringComparison.Ordinal),
            "The chevron or the eye is tested after the calendar's buttons, whose test leaves every other button.");

         // The template still names its chevron so (MainWindow.xaml), and WPF-UI 4.3.0's
         // password box its eye (Wpf.Ui.dll's template).
         Assert.Contains("<ToggleButton x:Name=\"Expander\"", ReadControlPanelFile("MainWindow.xaml"));

         string expander = WithheldRowTests.MemberText(source, "public static string ExpanderButton(bool expanded, string group)");
         Assert.True(expander != null, "FrameworkWords.ExpanderButton is gone.");
         Assert.Contains("expanded ? F(\"Collapse {0}\", group ?? \"\") : F(\"Expand {0}\", group ?? \"\")", expander);
         string reveal = WithheldRowTests.MemberText(source, "public static string RevealButton(bool revealed)");
         Assert.True(reveal != null, "FrameworkWords.RevealButton is gone.");
         Assert.Contains("revealed ? L(\"Hide password\") : L(\"Show password\")", reveal);
      }

      /// <summary>Every catalogue has the four, the two of each pair different.</summary>
      [Fact]
      public void EveryLanguage_NamesTheChevronsAndTheEyeEachWay()
      {
         string resources = ControlPanelDirectory("Resources");
         Assert.True(resources != null, "ControlPanel/Resources could not be found from the test binaries.");

         string[] catalogues = Directory.GetFiles(resources, "Strings.*.resx");
         Assert.True(catalogues.Length >= 17, "Expected seventeen languages, found " + catalogues.Length + ".");
         foreach (string path in catalogues)
         {
            Dictionary<string, string> strings = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "");
            string name = Path.GetFileName(path);
            string Word(string english) => strings.TryGetValue(english, out string word) ? word.Trim() : "";
            Assert.True(Word("Expand {0}").Contains("{0}") && Word("Collapse {0}").Contains("{0}"), name + " lacks the chevron's names, or their group.");
            Assert.True(Word("Show password").Length > 0 && Word("Hide password").Length > 0, name + " lacks the eye's names.");
            Assert.True(!string.Equals(Word("Expand {0}"), Word("Collapse {0}"), StringComparison.CurrentCultureIgnoreCase), name + " names both of the chevron's states alike.");
            Assert.True(!string.Equals(Word("Show password"), Word("Hide password"), StringComparison.CurrentCultureIgnoreCase), name + " names both of the eye's states alike.");
         }
      }

      private static IEnumerable<(string, string, string)> Items_(string code)
         => Regex.Matches(code, @"Item_\(L\(""([^""]+)""\), ""(\w+)"", ScrollBar\.(\w+)\)")
            .Select(m => (m.Groups[1].Value, m.Groups[2].Value, m.Groups[3].Value));

      private static int Separators_(string code) => Regex.Matches(code, @"new Separator\(\)").Count;

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
   }
}
