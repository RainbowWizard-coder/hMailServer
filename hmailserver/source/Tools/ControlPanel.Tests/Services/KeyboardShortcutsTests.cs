// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The keys the Control Panel's window answers on every page (review finding
   /// F014). Only Ctrl+K and F1 existed: F5, Ctrl+F, Ctrl+S and F6 did nothing on
   /// any page, the toolbar's FocusSearch was never called, the help pane closed
   /// only by its button or a second F1, and Refresh was Alt+R on some pages and
   /// Alt+F on others. The table and the order F6 moves in are held here as data;
   /// the window that answers them cannot be made in a test, so its source is read
   /// for the wiring, as the tour's and the flyout's tests read theirs.
   /// </summary>
   public class KeyboardShortcutsTests
   {
      [Theory]
      [InlineData("K", true, false, false, ShellCommand.Palette)]
      [InlineData("F1", false, false, false, ShellCommand.Help)]
      [InlineData("F5", false, false, false, ShellCommand.Refresh)]
      [InlineData("F", true, false, false, ShellCommand.Search)]
      [InlineData("S", true, false, false, ShellCommand.Save)]
      [InlineData("F6", false, false, false, ShellCommand.NextPart)]
      [InlineData("F6", false, true, false, ShellCommand.PreviousPart)]
      [InlineData("Left", false, false, true, ShellCommand.Back)]
      [InlineData("Right", false, false, true, ShellCommand.Forward)]
      [InlineData("BrowserBack", false, false, false, ShellCommand.Back)]
      [InlineData("BrowserForward", false, false, false, ShellCommand.Forward)]
      public void EachKey_IsItsCommand(string key, bool control, bool shift, bool alt, ShellCommand expected)
      {
         Assert.Equal(expected, KeyboardShortcuts.Resolve(key, control, shift, alt));
      }

      /// <summary>
      /// A key with a modifier it does not take is somebody else's: Alt with a
      /// letter is the access keys', Ctrl+F5 and Ctrl+Shift+S are not ours, a bare
      /// K, F or S is typing, and a bare arrow moves in the control that has it.
      /// </summary>
      [Theory]
      [InlineData("K", false, false, false)]
      [InlineData("F", false, false, false)]
      [InlineData("S", false, false, false)]
      [InlineData("S", true, true, false)]
      [InlineData("F5", true, false, false)]
      [InlineData("F5", false, true, false)]
      [InlineData("F1", true, false, false)]
      [InlineData("F6", true, false, false)]
      [InlineData("K", true, false, true)]
      [InlineData("F6", false, false, true)]
      [InlineData("B", false, false, true)]
      [InlineData("Left", false, false, false)]
      [InlineData("Right", false, false, false)]
      [InlineData("Left", true, false, true)]
      [InlineData("Right", false, true, true)]
      [InlineData("BrowserBack", true, false, false)]
      [InlineData("Enter", false, false, false)]
      [InlineData("", true, false, false)]
      [InlineData(null, true, false, false)]
      public void OtherKeys_AreNoCommand(string key, bool control, bool shift, bool alt)
      {
         Assert.Equal(ShellCommand.None, KeyboardShortcuts.Resolve(key, control, shift, alt));
      }

      [Fact]
      public void F6_GoesRoundTheOpenParts_InReadingOrder()
      {
         var all = new[] { ShellPart.Page, ShellPart.Help, ShellPart.CommandBar, ShellPart.Navigation };

         Assert.Equal(ShellPart.Navigation, KeyboardShortcuts.NextPart(all, ShellPart.CommandBar, false));
         Assert.Equal(ShellPart.Page, KeyboardShortcuts.NextPart(all, ShellPart.Navigation, false));
         Assert.Equal(ShellPart.Help, KeyboardShortcuts.NextPart(all, ShellPart.Page, false));
         Assert.Equal(ShellPart.CommandBar, KeyboardShortcuts.NextPart(all, ShellPart.Help, false));

         Assert.Equal(ShellPart.Help, KeyboardShortcuts.NextPart(all, ShellPart.CommandBar, true));
         Assert.Equal(ShellPart.Navigation, KeyboardShortcuts.NextPart(all, ShellPart.Page, true));
      }

      /// <summary>A part that is not on screen - the help pane closed, the pending bar empty - is passed over.</summary>
      [Fact]
      public void F6_PassesOverWhatIsNotOpen()
      {
         var open = new[] { ShellPart.CommandBar, ShellPart.Navigation, ShellPart.Page };

         Assert.Equal(ShellPart.CommandBar, KeyboardShortcuts.NextPart(open, ShellPart.Page, false));
         Assert.Equal(ShellPart.Page, KeyboardShortcuts.NextPart(open, ShellPart.CommandBar, true));

         // Before sign-in there is no navigation to move to.
         var signIn = new[] { ShellPart.CommandBar, ShellPart.Page };
         Assert.Equal(ShellPart.Page, KeyboardShortcuts.NextPart(signIn, ShellPart.CommandBar, false));
         Assert.Equal(ShellPart.CommandBar, KeyboardShortcuts.NextPart(signIn, ShellPart.Page, false));
      }

      /// <summary>From nowhere - nothing focused, or a tour's card - F6 starts at the top and Shift+F6 at the foot.</summary>
      [Fact]
      public void F6_FromNowhere_StartsAtAnEnd()
      {
         var open = new[] { ShellPart.CommandBar, ShellPart.Navigation, ShellPart.Page, ShellPart.PendingBar };

         Assert.Equal(ShellPart.CommandBar, KeyboardShortcuts.NextPart(open, null, false));
         Assert.Equal(ShellPart.PendingBar, KeyboardShortcuts.NextPart(open, null, true));
         Assert.Equal(ShellPart.CommandBar, KeyboardShortcuts.NextPart(open, ShellPart.Help, false));
         Assert.Null(KeyboardShortcuts.NextPart(Array.Empty<ShellPart>(), ShellPart.Page, false));
         Assert.Equal(ShellPart.Page, KeyboardShortcuts.NextPart(new[] { ShellPart.Page }, ShellPart.Page, false));
      }

      /// <summary>
      /// The list the palette shows names every key the window answers, so that
      /// the two cannot drift apart: each command of the table is in it.
      /// </summary>
      [Fact]
      public void TheList_NamesEveryKeyTheWindowAnswers()
      {
         string keys = string.Join("\n", KeyboardShortcuts.Listed.Select(s => s.Keys));
         foreach (string key in new[] { "Ctrl+K", "F1", "F5", "Ctrl+F", "Ctrl+S", "F6, Shift+F6", "Alt+Left, Alt+Right", "Esc" })
            Assert.Contains(key, keys, StringComparison.Ordinal);

         Assert.All(KeyboardShortcuts.Listed, s =>
         {
            Assert.False(string.IsNullOrWhiteSpace(s.Keys));
            Assert.False(string.IsNullOrWhiteSpace(s.Does));
         });
      }

      /// <summary>
      /// The palette offers the list: typing "keyboard" or "shortcuts" finds a row
      /// that opens it, and the overview has it with nothing typed. It carries no
      /// page and no tour, so choosing it moves the reader nowhere.
      /// </summary>
      [Theory]
      [InlineData("keyboard")]
      [InlineData("shortcuts")]
      [InlineData("keys")]
      public void ThePalette_OffersTheList(string query)
      {
         PaletteRow row = PaletteSearch.Query(query, null, null).FirstOrDefault(r => r.Kind == PaletteRowKind.Shortcuts);
         Assert.NotNull(row);
         Assert.Null(row.Page);
         Assert.Null(row.Tour);
         Assert.Equal(PaletteSearch.HelpSection, row.Section);

         Assert.Contains(PaletteSearch.Query("", null, null), r => r.Kind == PaletteRowKind.Shortcuts);
      }

      /// <summary>Each command reaches what it should, and the window asks the table rather than spelling the keys out again.</summary>
      [Fact]
      public void TheWindow_AnswersEveryCommand()
      {
         string keys = ReadControlPanelFile("MainWindow.Keys.cs");

         Assert.Contains("KeyboardShortcuts.Resolve(", keys, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"case ShellCommand\.Palette:[\s\S]{0,120}?ShowPalette\(\);"), keys);
         Assert.Matches(new Regex(@"case ShellCommand\.Help:[\s\S]{0,120}?ToggleHelp_\(\);"), keys);
         Assert.Matches(new Regex(@"case ShellCommand\.Refresh:[\s\S]{0,200}?PageCommands\.Refresh\(ContentHost\.Content as DependencyObject\);"), keys);
         Assert.Matches(new Regex(@"case ShellCommand\.Save:[\s\S]{0,160}?PageCommands\.Press\([^;]*PageCommand\.Save\)"), keys);
         Assert.Matches(new Regex(@"case ShellCommand\.Search:[\s\S]{0,400}?PageCommands\.FocusSearch\([\s\S]{0,200}?ShowPalette\(\);"), keys);
         Assert.Matches(new Regex(@"case ShellCommand\.NextPart:\s*case ShellCommand\.PreviousPart:\s*return MoveToPart_\("), keys);

         // Escape in the help pane closes it, which puts the keyboard back on
         // the Help button; bubbling, so that a control in the pane keeps its own.
         Assert.Matches(new Regex(@"HelpPanel\.KeyDown \+=[\s\S]{0,300}?Key\.Escape[\s\S]{0,300}?CloseHelp_\(\);"), keys);

         string window = ReadControlPanelFile("MainWindow.xaml.cs");
         Assert.Contains("HandleTourKey_(e) || HandleShellKey_(e)", window, StringComparison.Ordinal);
         Assert.Contains("WireShellKeys_();", window, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"private void CloseHelp_\(\)[\s\S]{0,800}?HelpButton\.Focus\(\);"), window);

         // A running tour's F6 reaches its card from anywhere but the card,
         // from which F6 goes on round the window.
         string tour = ReadControlPanelFile("MainWindow.Tour.cs");
         Assert.Contains("e.Key == Key.F6 && Keyboard.Modifiers == ModifierKeys.None && !TourLayer.HasFocusWithin", tour, StringComparison.Ordinal);
      }

      /// <summary>
      /// F5 and Ctrl+S press the page's own button, found by its caption, so
      /// every button that re-reads a page says Refresh (a list page) or Reload
      /// (a settings page) with the one access key each, and no page keeps the
      /// Re_fresh that made the key Alt+F on six pages and Alt+R on the rest.
      /// </summary>
      [Fact]
      public void EveryReRead_IsRefreshOrReload_WithItsOneKey()
      {
         string views = ControlPanelDirectory("Views");
         var captions = new Regex(@"(?:L\(""|\{loc:L ')((?:R_?e_?-?_?f_?r_?e_?s_?h|R_?e_?l_?o_?a_?d|_R_?e_?f_?r_?e_?s_?h|_R_?e_?l_?o_?a_?d))(?:""\)|'\})");
         var bad = new List<string>();
         foreach (string file in Directory.EnumerateFiles(views, "*.*", SearchOption.AllDirectories).Where(f => f.EndsWith(".cs", StringComparison.Ordinal) || f.EndsWith(".xaml", StringComparison.Ordinal)))
         {
            foreach (Match m in captions.Matches(File.ReadAllText(file)))
            {
               string caption = m.Groups[1].Value;
               if (caption != "_Refresh" && caption != "_Reload")
                  bad.Add(Path.GetFileName(file) + ": " + caption);
            }
         }

         Assert.True(bad.Count == 0, "Re-read with another key: " + string.Join("; ", bad));
      }

      /// <summary>
      /// Reload is the settings pages' verb, and asks first when edits would be
      /// lost (the owner's decision of 9 October 2026): a page that says Reload
      /// is one that can hold unsaved changes. A list page says Refresh.
      /// </summary>
      [Fact]
      public void Reload_IsSaidOnlyByAPageThatCanHoldUnsavedChanges()
      {
         string views = ControlPanelDirectory("Views");
         var reload = new Regex(@"(?:L\(""_Reload""\)|\{loc:L '_Reload'\})");
         foreach (string file in Directory.EnumerateFiles(views, "*.*", SearchOption.AllDirectories).Where(f => f.EndsWith(".cs", StringComparison.Ordinal) || f.EndsWith(".xaml", StringComparison.Ordinal)))
         {
            if (!reload.IsMatch(File.ReadAllText(file)))
               continue;

            string code = file.EndsWith(".xaml", StringComparison.Ordinal) ? file + ".cs" : file;
            Assert.True(File.Exists(code) && File.ReadAllText(code).Contains("public bool HasUnsavedChanges", StringComparison.Ordinal),
               Path.GetFileName(file) + " says Reload but cannot hold an unsaved change; a list page says Refresh.");
         }
      }

      /// <summary>The captions PageCommands finds a page's buttons by are the ones the pages use.</summary>
      [Fact]
      public void PageCommands_FindsTheButtonsByTheCaptionsThePagesUse()
      {
         string commands = ReadControlPanelFile(Path.Join("Views", "PageCommands.cs"));
         Assert.Contains("RefreshCaptions = new[] { N(\"_Refresh\"), N(\"_Reload\") }", commands, StringComparison.Ordinal);
         Assert.Contains("SaveCaptions = new[] { N(\"_Save changes\") }", commands, StringComparison.Ordinal);

         // The scripts page's two buttons say more than their verbs and are marked.
         string scripts = ReadControlPanelFile(Path.Join("Views", "ScriptsView.cs"));
         Assert.Contains("PageCommands.SetCommand(save, PageCommand.Save);", scripts, StringComparison.Ordinal);
         Assert.Contains("PageCommands.SetCommand(reload, PageCommand.Refresh);", scripts, StringComparison.Ordinal);
      }

      /// <summary>
      /// F5 and Ctrl+S press the button as a click does: the keyboard moves to
      /// the button before it is pressed, so that a number typed in a box and not
      /// yet taken in - a NumberBox takes its text into its Value only as it loses
      /// the keyboard - is what Save writes and what makes Reload ask. Pressed
      /// through the button's automation peer, which leaves the keyboard where it
      /// is, Ctrl+S saved the old value and said Saved, and F5 threw the typing
      /// away without asking. The key presses no disabled button, and the
      /// keyboard goes back where it was unless the button put it elsewhere.
      /// </summary>
      [Fact]
      public void PageCommands_MovesTheKeyboardToTheButtonBeforePressingIt()
      {
         string commands = ReadControlPanelFile(Path.Join("Views", "PageCommands.cs"));
         Match press = Regex.Match(commands, @"public static bool Press\(DependencyObject page, PageCommand command\)\s*\{(?<body>[\s\S]*?)\n      \}");
         Assert.True(press.Success, "PageCommands.Press was not found.");
         string body = press.Groups["body"].Value;

         int focus = body.IndexOf("Keyboard.Focus(button)", StringComparison.Ordinal);
         int pressed = body.IndexOf("GridKeys.Press(button)", StringComparison.Ordinal);
         int back = body.IndexOf("Keyboard.Focus(had)", StringComparison.Ordinal);
         Assert.True(focus >= 0 && pressed > focus, "The keyboard must move to the button before it is pressed.");
         Assert.True(back > pressed, "The keyboard goes back where it was after the press.");
         Assert.DoesNotContain("IInvokeProvider", commands, StringComparison.Ordinal);

         // GridKeys.Press leaves a disabled or hidden button alone.
         string keys = ReadControlPanelFile(Path.Join("Views", "GridKeys.cs"));
         Assert.Contains("if (button == null || !button.IsEnabled || !button.IsVisible)", keys, StringComparison.Ordinal);
      }

      /// <summary>
      /// F5 reaches every page that reads the server as it is opened, and Ctrl+S
      /// every page that can hold an unsaved change. Each key found only a button
      /// captioned exactly Refresh, Reload or Save changes, so F5 did nothing on
      /// the domains, routes, rules, incoming relays, TCP/IP ports, SSL
      /// certificates, IP ranges, live logs, stalled mail and message trace pages
      /// and the dashboard, and Ctrl+S nothing on IP ranges ("Save permissions")
      /// or Backup &amp; restore ("Save settings", "Save schedule") - while the
      /// list of keys promised both on every page. A page answers F5 by a Refresh
      /// or Reload button, a button marked PageCommand.Refresh, or as an
      /// IRefreshablePage; Ctrl+S by a Save changes button or one marked
      /// PageCommand.Save.
      /// </summary>
      [Fact]
      public void EveryPageReadAsItOpens_AnswersF5_AndEveryPageThatCanHoldAnEdit_AnswersCtrlS()
      {
         string views = ControlPanelDirectory("Views");
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string specs = File.ReadAllText(Path.Join(views, "CollectionSpecs.cs"));

         // The pages, as the shell makes them.
         var pages = new SortedSet<string>(StringComparer.Ordinal);
         foreach (Match m in Regex.Matches(shell, @"pageFactories_\[""\w+""\] = \(\) => (?:new (?<page>\w+)\(|CollectionSpecs\.(?<spec>\w+)\(\))"))
         {
            if (m.Groups["page"].Success)
            {
               pages.Add(m.Groups["page"].Value);
               continue;
            }

            // A collection page is the CollectionEditorView, or the class a spec makes around one.
            Match spec = Regex.Match(specs, @"public static (?<type>\w+) " + m.Groups["spec"].Value + @"\(\) => new\s*(?<made>\w+)?\s*\(");
            Assert.True(spec.Success, "CollectionSpecs." + m.Groups["spec"].Value + " was not found.");
            pages.Add(spec.Groups["made"].Success ? spec.Groups["made"].Value : spec.Groups["type"].Value);
         }

         Assert.Contains("DomainsView", pages);
         Assert.Contains("CollectionEditorView", pages);
         Assert.True(pages.Count > 40, "The shell's pages were not found: " + string.Join(", ", pages));

         // A page that has no Reload, and whose settings reading again would throw
         // away unasked, has no F5: the backup page's own cards keep no unsaved state
         // (its migrations' card, an embedded section, does, and saves on Ctrl+S).
         var noRefresh = new Dictionary<string, string>(StringComparer.Ordinal)
         {
            ["BackupView"] = "a settings page with no Reload, whose own cards do not know what is unsaved",
         };

         var refresh = new Regex(@"L\(""_(?:Refresh|Reload)""\)|\{loc:L '_(?:Refresh|Reload)'\}|PageCommand\.Refresh\)|\bIRefreshablePage\b|CollectionEditorView");
         // A page whose edit is a settings section shown on it (an embedded ServerSettingsView,
         // the Rules page's Rule limits) is reached through the section's Save, which
         // ServerSettingsView.Embedded marks with PageCommand.Save whatever the page names it.
         var save = new Regex(@"L\(""_Save changes""\)|\{loc:L '_Save changes'\}|PageCommand\.Save\)|new ServerSettingsView\(ServerSettingsView\.Section\.\w+, embedded: true\)");
         Assert.Contains("PageCommands.SetCommand(SaveButton, PageCommand.Save);", File.ReadAllText(Path.Join(views, "ServerSettingsView.Embedded.cs")), StringComparison.Ordinal);
         var bad = new List<string>();
         foreach (string page in pages)
         {
            string source = ClassSource_(views, page);
            Assert.False(string.IsNullOrEmpty(source), page + " was not found under Views.");

            Match enter = Regex.Match(source, @"public void OnEnter\(\)\s*(?:=>\s*(?<expr>[^;]+);|\{(?<body>[\s\S]*?)\n      \})");
            bool readsAsItOpens = enter.Success && (enter.Groups["expr"].Success || enter.Groups["body"].Value.Trim().Length > 0);
            if (readsAsItOpens && !noRefresh.ContainsKey(page) && !refresh.IsMatch(source))
               bad.Add(page + " is read as it is opened, and F5 does not reach it");

            if (source.Contains("public bool HasUnsavedChanges", StringComparison.Ordinal) && !save.IsMatch(source))
               bad.Add(page + " can hold an unsaved change, and Ctrl+S does not reach it");
         }

         Assert.True(bad.Count == 0, string.Join("\n", bad));

         // The pages with no button of their own read themselves again.
         foreach (string page in new[] { "DomainsView", "RoutesView", "TcpIpPortsView", "SslCertificatesView", "RulesView", "IncomingRelaysView", "LogsView", "MessageTraceView", "IPRangesView", "StalledMailView", "DashboardView" })
            Assert.Matches(new Regex(@"class " + page + @"\b[^{]*\bIRefreshablePage\b[\s\S]*void IRefreshablePage\.Refresh\(\)"), ClassSource_(views, page));

         // The window's F5 reaches an IRefreshablePage where Find finds no button.
         string commands = File.ReadAllText(Path.Join(views, "PageCommands.cs"));
         Assert.Matches(new Regex(@"public static bool Refresh\(DependencyObject page\)\s*\{\s*if \(Find\(page, PageCommand\.Refresh, [^)]*\) != null\)\s*return Press\(page, PageCommand\.Refresh\);\s*if \(page is not IRefreshablePage refreshable\)\s*return false;"), commands);

         // Of several Save buttons, the one in the card the keyboard is in.
         Assert.Contains("PageCommands.SetCommand(SaveScheduleButton, PageCommand.Save);", File.ReadAllText(Path.Join(views, "BackupView.xaml.cs")), StringComparison.Ordinal);
         Assert.Contains("PageCommands.SetCommand(button, PageCommand.Save);", File.ReadAllText(Path.Join(views, "BackupView.xaml.cs")), StringComparison.Ordinal);
         Assert.Contains("PageCommands.SetCommand(SavePermsButton, PageCommand.Save);", File.ReadAllText(Path.Join(views, "IPRangesView.xaml.cs")), StringComparison.Ordinal);
         Assert.Matches(new Regex(@"foreach \(DependencyObject container in Ancestors_\(focused\)\)\s*\{\s*List<ButtonBase> near = found\.Where\(b => Ancestors_\(b\)\.Contains\(container\)\)"), commands);
      }

      /// <summary>The source of a page class: every part of it under Views, and the XAML of a XAML page.</summary>
      private static string ClassSource_(string views, string name)
      {
         var parts = new List<string>();
         foreach (string file in Directory.EnumerateFiles(views, "*.cs", SearchOption.AllDirectories))
         {
            string text = File.ReadAllText(file);
            Match declared = Regex.Match(text, @"\n   public (?:sealed |partial )*class " + name + @"\b");
            if (!declared.Success)
               continue;

            // The class runs to the next type at the namespace's level, or the end.
            int start = declared.Index;
            Match next = new Regex(@"\n   (?:public|internal) (?:sealed |static |partial |abstract )*(?:class|interface|enum|record|struct) ").Match(text, start + declared.Length);
            parts.Add(next.Success ? text.Substring(start, next.Index - start) : text.Substring(start));

            string xaml = file.EndsWith(".xaml.cs", StringComparison.Ordinal) ? file.Substring(0, file.Length - 3) : null;
            if (xaml != null && File.Exists(xaml))
               parts.Add(File.ReadAllText(xaml));
         }

         return string.Join("\n", parts);
      }

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(Path.Join(ControlPanelDirectory(""), relative));

      private static string ControlPanelDirectory(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel");
            if (File.Exists(Path.Join(candidate, "MainWindow.xaml")))
               return relative.Length == 0 ? candidate : Path.Join(candidate, relative);
            directory = directory.Parent;
         }

         Assert.Fail("The ControlPanel sources could not be found from the test binaries.");
         return null;
      }
   }
}
