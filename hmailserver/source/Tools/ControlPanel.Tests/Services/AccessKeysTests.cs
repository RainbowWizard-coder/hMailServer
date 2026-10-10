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
      [InlineData("ActiveDirectoryPickerDialog.cs", "Content = L(\"_Search\"),")]
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
         // And its negative controls.
         Assert.Contains("def self_test():", checker, StringComparison.Ordinal);
         Assert.Contains("localisation mnemonics mysql-charset", ReadRepositoryFile(".gitlab-ci.yml"), StringComparison.Ordinal);
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
