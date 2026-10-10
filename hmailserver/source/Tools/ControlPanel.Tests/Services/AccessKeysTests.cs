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
   /// The access keys (review findings F287 and F071). The page forms' captions
   /// carried no key while every dialog field did, so Alt reached a page's
   /// buttons and none of its boxes; one action had three names and keys -
   /// Refresh was Alt+R, Alt+F or Reload - and Resume and Refresh were both Alt+R
   /// on the server status page; the Backup &amp; restore page shared eight letters
   /// between its cards, so Alt+P moved the focus instead of pressing Put the
   /// message back; and build/check-mnemonics.py, which read neither a field row's
   /// caption nor a caption set in code on a XAML page's button, passed all of it.
   /// The test project has no WPF to press a key in, so the wiring is held to its
   /// source, and the checker - which CI runs with its own negative controls
   /// (--self-test) - to the rules it must hold.
   /// </summary>
   public class AccessKeysTests
   {
      /// <summary>The page forms the finding names: every caption has a key, none a button's.</summary>
      [Theory]
      [InlineData("DomainsView.xaml", "{loc:L 'Do_main'}")]
      [InlineData("DomainsView.xaml", "{loc:L 'Add_ress'}")]
      [InlineData("DomainsView.xaml", "{loc:L '_Password'}")]
      [InlineData("DomainsView.xaml", "{loc:L 'Alias addr_ess'}")]
      [InlineData("DomainsView.xaml", "{loc:L 'Deli_vers to'}")]
      [InlineData("DomainsView.xaml", "{loc:L 'L_ist address'}")]
      [InlineData("RolesView.cs", "FormField_(L(\"_Account\"), address_)")]
      [InlineData("RolesView.cs", "FormField_(L(\"R_ole\"), roleBox)")]
      [InlineData("RolesView.cs", "FormField_(L(\"_Domains\"), domains_)")]
      public void ThePageFormsCaptionsCarryAKey(string view, string caption)
      {
         Assert.Contains(caption, ReadView(view), StringComparison.Ordinal);
      }

      /// <summary>One verb, one key; and Resume is not Refresh's key.</summary>
      [Theory]
      [InlineData("StatusView.xaml.cs", "PauseButton.Content = L(\"R_esume the mail server\");")]
      [InlineData("LogsView.xaml.cs", "PauseButton.Content = paused_ ? L(\"R_esume view\") : L(\"_Pause view\");")]
      [InlineData("TcpIpPortsView.xaml", "Content=\"{loc:L 'Restore de_faults'}\"")]
      [InlineData("IPRangesView.xaml", "Content=\"{loc:L 'Restore de_faults'}\"")]
      [InlineData("AccountTwoFactorPanel.cs", "MakeButton(L(\"Re_move\")")]
      [InlineData("RouteDialog.cs", "Content = L(\"Re_move\")")]
      [InlineData("MessageTraceView.cs", "MakeButton(L(\"Remove _expired\")")]
      [InlineData("ActiveDirectoryPickerDialog.cs", "searchButton_ = new() { Content = L(\"_Search\") }")]
      [InlineData("AppPasswordsPanel.cs", "MakeButton(L(\"_Copy\")")]
      [InlineData("DomainDialog.cs", "var dkimBrowse = new Wpf.Ui.Controls.Button { Content = L(\"_Browse…\")")]
      [InlineData("TcpIpPortDialog.cs", "var caBrowse = new Wpf.Ui.Controls.Button { Content = L(\"_Browse…\")")]
      public void AVerbCarriesOneKey(string view, string caption)
      {
         Assert.Contains(caption, ReadView(view), StringComparison.Ordinal);
      }

      [Fact]
      public void TheBackupPagesCardsAnswerTheirOwnKeys()
      {
         string backup = ReadView("BackupView.xaml");
         Assert.Matches(new Regex(@"<UserControl[^>]*\blocal:AccessKeyScope\.ByCard=""True"""), backup);

         // In one card no two captions share a key: the mailbox card's archive
         // and Merge's "nothing" were both N.
         Assert.Contains("{loc:L '_Archive in the backup destination (name or path)'}", backup, StringComparison.Ordinal);
      }

      [Fact]
      public void AccessKeyScope_SetsTheCardAsTheScope_OnlyForTheCardThatHoldsTheKeyboardAndHasTheKey()
      {
         string scope = ReadView("AccessKeyScope.cs");

         // On the page, for handled events too, and never over a scope set nearer.
         Assert.Contains("page.AddHandler(AccessKeyManager.AccessKeyPressedEvent, Handler, true);", scope, StringComparison.Ordinal);
         Assert.Contains("if (e.Scope != null || sender is not DependencyObject page)", scope, StringComparison.Ordinal);

         // The card of the element the event was raised on, holding the keyboard and
         // answering the key: anything else keeps the window's scope, so a key the
         // card does not have goes to the page as a whole.
         Assert.Contains("Card card = CardOf(e.OriginalSource as DependencyObject, page);", scope, StringComparison.Ordinal);
         Assert.Contains("if (card == null || !card.IsKeyboardFocusWithin || !Answers(card, e.Key))", scope, StringComparison.Ordinal);
         Assert.Contains("e.Scope = card;", scope, StringComparison.Ordinal);

         // A caption that can be pressed: a button's AccessText or a field's
         // editor, visible and enabled.
         Assert.Contains("element.IsVisible && element.IsEnabled", scope, StringComparison.Ordinal);
         Assert.Contains("d is AccessText text", scope, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.GetAccessKey(d)", scope, StringComparison.Ordinal);
      }

      [Fact]
      public void AFieldsKey_PutsTheKeyboardInTheBoxOfARow_AndIsRegisteredOnce()
      {
         string mnemonic = ReadView("Mnemonic.cs");

         // A row of a box and a button takes no focus itself: the key goes to the
         // first control inside it that does.
         Assert.Contains("e.Target = Editor(target, true) ?? target;", mnemonic, StringComparison.Ordinal);
         Assert.Contains("element.Focusable && KeyboardNavigation.GetIsTabStop(element)", mnemonic, StringComparison.Ordinal);
         // Never a button of the row: a key presses a button it reaches, and a
         // field's key only moves the keyboard.
         Assert.Contains("element is not ButtonBase && Editor(element, reachable) is { } editor", mnemonic, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetAccessKey(Editor(target, false) ?? target,", mnemonic, StringComparison.Ordinal);

         // A caption applied again registers its key once, and gives a changed one up.
         Assert.Matches(new Regex(@"if \(registered != key\)\s*\{\s*if \(registered != null\)\s*AccessKeyManager\.Unregister\(registered, target\);\s*AccessKeyManager\.Register\(key, target\);"), mnemonic);
      }

      [Fact]
      public void TheChecker_ReadsEveryCaptionAndHoldsEachRule()
      {
         string checker = ReadRepositoryFile(Path.Join("build", "check-mnemonics.py"));

         // Field rows in XAML and in code, and the helpers that make them.
         Assert.Contains(@"<scaffold:FieldRow\b[^>]*?\bLabel=""([^""]*)""", checker, StringComparison.Ordinal);
         Assert.Contains(@"new\s+(?:Scaffold\.)?FieldRow\b", checker, StringComparison.Ordinal);
         Assert.Contains(@"\b(?:FormField_|Captioned|Field)\(", checker, StringComparison.Ordinal);
         // A caption set in code on a XAML page's named button joins its page.
         Assert.Contains("def onto_xaml_pages(captions):", checker, StringComparison.Ordinal);
         // A tab's key may not be the constructor's: the constructor's captions
         // come first, so the tab's is the one reported.
         Assert.Contains("for c in ctor + items:", checker, StringComparison.Ordinal);
         // One verb, one key, for verbs.
         Assert.Contains("def one_key_per_caption(captions):", checker, StringComparison.Ordinal);
         Assert.Contains("if c.text in EXEMPT or c.key is None or c.via or c.field:", checker, StringComparison.Ordinal);
         // Cards that answer their own keys.
         Assert.Contains(@"AccessKeyScope\.ByCard=""True""", checker, StringComparison.Ordinal);
         // A settings page's row classes on the tab their card is placed on, with the
         // page's header; and a row's MakeButton_ caption is read.
         Assert.Contains("def row_captions(captions, files):", checker, StringComparison.Ordinal);
         Assert.Contains("placed += row_captions(all_captions, files)", checker, StringComparison.Ordinal);
         Assert.Contains(@"\bMakeButton_?\(\s*", checker, StringComparison.Ordinal);
         // And its negative controls.
         Assert.Contains("def self_test():", checker, StringComparison.Ordinal);
         Assert.Contains("localisation mnemonics mysql-charset", ReadRepositoryFile(".gitlab-ci.yml"), StringComparison.Ordinal);
      }

      /// <summary>
      /// The Auto-ban tab draws Banned now, the never-ban table by address and the
      /// never-ban table by reverse name - three classes of ServerSettingsView's own -
      /// at once, under the page header's Reload and Save changes, so no two of their
      /// buttons may share an Alt key in English or in any catalogue: WPF, given two
      /// targets for one key, moves the focus between them and presses neither. Add
      /// address... took Add name...'s Alt+A, and in every language one of Lift ban,
      /// Add address... and Remove address met a key already on the tab - in Russian
      /// Lift ban was Save changes' Alt+С, in Brazilian Portuguese Reload's Alt+R - while
      /// build/check-mnemonics.py gave each class a scope of its own (review finding
      /// F231). The checker now holds every settings tab to it (row_captions); this
      /// holds the tab it was found on, read from the source as the page builds it.
      /// </summary>
      [Fact]
      public void TheAutoBanTabsButtons_ShareNoAccessKey_InEnglishOrAnyCatalogue()
      {
         var captions = new List<string>();
         captions.AddRange(Regex.Matches(ReadView("ServerSettingsView.xaml"), @"<ui:Button\b[^>]*?\bContent=""\{loc:L '([^']*_[^']*)'\}""")
            .Select(m => m.Groups[1].Value));
         foreach ((string file, string rows) in new[]
         {
            ("ServerSettingsView.BannedNow.cs", "BannedNowRows"),
            ("ServerSettingsView.NeverBanAddresses.cs", "NeverBanAddressRows"),
            ("ServerSettingsView.NeverBanNames.cs", "NeverBanNameRows")
         })
         {
            // The row class alone: the add dialogs that follow it in its file are
            // windows of their own, as is the question a Remove asks.
            string source = ReadView(file);
            int start = source.IndexOf("private sealed class " + rows + " ", StringComparison.Ordinal);
            Assert.True(start >= 0, file + " has no class " + rows + ".");
            int end = source.IndexOf("\n      private static class ", start, StringComparison.Ordinal);
            string body = end < 0 ? source.Substring(start) : source.Substring(start, end - start);
            captions.AddRange(Regex.Matches(body, @"(?:\bMakeButton_?\(|\bContent = )L\(""([^""]*_[^""]*)""\)")
               .Select(m => m.Groups[1].Value));
         }

         // Every button of the tab is read, so that the scan cannot go blind.
         Assert.Equal(new[] { "_Reload", "_Save changes", "_Lift ban", "A_dd address…", "Remo_ve address", "_Add name…", "Re_move name", "C_heck an address…" },
            captions);

         string resources = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var offenders = new List<string>();
         var languages = new List<(string Tag, Dictionary<string, string> Catalogue)> { ("en", null) };
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            languages.Add((Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length),
               XDocument.Load(path).Root.Elements("data")
                  .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal)));
         }

         Assert.Equal(18, languages.Count);
         foreach ((string tag, Dictionary<string, string> catalogue) in languages)
         {
            // A text the catalogue leaves empty is shown in English.
            var shown = captions.Select(c => catalogue != null && catalogue.TryGetValue(c, out string text) && text.Length > 0 ? text : c).ToList();
            foreach (IGrouping<char?, string> shared in shown.GroupBy(AccessKeyOf_).Where(g => g.Key == null || g.Count() > 1))
            {
               offenders.Add(tag + ": " + (shared.Key == null
                  ? "no Alt key in " + string.Join(", ", shared.Select(s => "\"" + s + "\""))
                  : "Alt+" + shared.Key + " on " + string.Join(", ", shared.Select(s => "\"" + s + "\""))));
            }
         }

         Assert.True(offenders.Count == 0,
            "Two buttons of the Auto-ban tab share an Alt key, so that neither key presses its button; choose a letter the tab leaves free:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// The key a caption's Alt key is, as WPF's AccessText reads it: the character
      /// after the first underscore that is not doubled, upper-cased as
      /// AccessKeyManager compares keys; null where there is none.
      /// </summary>
      private static char? AccessKeyOf_(string caption)
      {
         for (int i = 0; i < caption.Length - 1; i++)
         {
            if (caption[i] != '_')
               continue;
            if (caption[i + 1] == '_')
            {
               i++;
               continue;
            }

            return char.ToUpperInvariant(caption[i + 1]);
         }

         return null;
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }

      private static string ReadRepositoryFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, relative);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "build")))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail(relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
