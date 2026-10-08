// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A failed read is never shown as a fact. Every list page used to keep what
   /// it had on screen when its read threw - its empty state ("No routes defined
   /// yet."), the rows of its last visit, a count of zero, ticked defaults with
   /// Save enabled - and the only thing that said otherwise was a toast that
   /// went away by itself; on the IP ranges, routes, rules, ports and relays
   /// pages the read had no catch at all, so the toast was the shell's. Every
   /// such page now reads inside a catch that hands the failure to
   /// Services/ReadFailure, which empties the list, hides the empty state, puts
   /// a Critical notice with Try again above the list and disables the writes
   /// until a read works; and every page that shows its own messages on that
   /// notice clears the failure first, so Try again never sits beside a
   /// sentence it has nothing to do with.
   ///
   /// Scanned rather than rendered, as UnreadRowTests is: this assembly has no
   /// WPF. Each test fails against the tree before the fix.
   /// </summary>
   public class ReadFailurePageTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      /// <summary>Every list page, the method that reads its list, and the words its failure leads with.</summary>
      public static TheoryData<string, string, string> Readers => new()
      {
         { "QueueView.xaml.cs", "private void Reload()", "Could not read the queue: " },
         { "IPRangesView.xaml.cs", "private void Reload()", "Could not read the IP ranges: " },
         { "RoutesView.xaml.cs", "private void Reload()", "Could not read the routes: " },
         { "RulesView.xaml.cs", "private void Reload()", "Could not read the rules: " },
         { "TcpIpPortsView.xaml.cs", "private void Reload()", "Could not read the ports: " },
         { "UtilityViews.cs", "private void Reload()", "Could not read the incoming relays: " },
         { "PublicFoldersView.cs", "private void Reload()", "Could not read the public folders: " },
         { "QuarantineView.cs", "private void Reload()", "Could not read the quarantine: " },
         { "DomainPolicyListView.cs", "private void Unread_(string prefix, string why)", "Could not read the list: " },
         { "AuditTrailView.cs", "private void Reload()", "Could not read the audit trail: " },
         { "JournalingView.cs", "private void Unread_(string prefix, string why)", "Could not read the journaling rules: " },
         { "RolesView.cs", "private void Reload_()", "Could not read the roles: " },
         { "CollectionEditorView.cs", "private void Reload()", "Could not read the list: " },
         { "BackupView.xaml.cs", "public void OnEnter()", "Could not read the backup settings: " },
         { "DomainsView.xaml.cs", "private void ReloadDomains(string select = null)", "Could not read the domains: " },
         { "DomainsView.xaml.cs", "private void ReloadAccounts()", "Could not read the accounts: " },
         { "SuppressionListView.cs", "private bool LoadDomains_()", "Could not read the domains: " },
         { "SuppressionListView.cs", "private void Unread_(string prefix, string why)", "Could not read the suppression list: " },
      };

      [Theory]
      [MemberData(nameof(Readers))]
      public void EveryListPage_ReadsInsideACatch_ThatShowsTheFailureTheOneWay(string file, string reader, string prefix)
      {
         string source = ReadView(file);
         string body = WithheldRowTests.MemberText(source, reader);
         Assert.True(body != null, file + " no longer has " + reader + ".");

         Assert.True(source.Contains("L(\"" + prefix + "\")", StringComparison.Ordinal),
            file + " does not lead its failure with \"" + prefix + "\".");

         // The read is caught - on the IP ranges, routes, rules, ports and relays
         // pages it was not, and the only word of a lost link was the shell's toast.
         bool reads = reader.Contains("Unread_", StringComparison.Ordinal);
         Assert.True(reads || body.Contains("catch (Exception", StringComparison.Ordinal),
            reader + " in " + file + " reads with no catch, so a lost link leaves the page as it was.");

         // ... and what is caught goes to the one helper, which empties the list,
         // hides the empty state and shows the notice with Try again.
         Assert.True(body.Contains("ReadFailure.Show(", StringComparison.Ordinal),
            reader + " in " + file + " does not hand a failed read to ReadFailure.Show.");
      }

      /// <summary>
      /// A page that writes its own messages on the notice the failure uses clears
      /// the failure first: otherwise "Select an entry first." would appear beside
      /// a Try again button that has nothing to do with it.
      /// </summary>
      public static TheoryData<string, string> Writers => new()
      {
         { "QueueView.xaml.cs", "private void ShowNotice_(StatusLevel level, string text)" },
         { "PublicFoldersView.cs", "private void Notice_(StatusLevel level, string text)" },
         { "QuarantineView.cs", "private void Status_(StatusLevel level, string text)" },
         { "DomainPolicyListView.cs", "private void Status_(StatusLevel level, string text)" },
         { "AuditTrailView.cs", "private void Status_(StatusLevel level, string text)" },
         { "CollectionEditorView.cs", "private void Say_(StatusLevel level, string text)" },
         { "BackupView.xaml.cs", "private void ShowNotice_(StatusLevel level, string text)" },
         { "SuppressionListView.cs", "private void Status_(StatusLevel level, string text)" },
      };

      [Theory]
      [MemberData(nameof(Writers))]
      public void APageThatWritesItsOwnMessageOnTheNotice_ClearsTheFailureFirst(string file, string writer)
      {
         string body = WithheldRowTests.MemberText(ReadView(file), writer);
         Assert.True(body != null, file + " no longer has " + writer + ".");
         Assert.True(body.Contains("ReadFailure.Clear(", StringComparison.Ordinal),
            writer + " in " + file + " writes over the failure notice without clearing the failure and its Try again button.");
      }

      /// <summary>
      /// The XAML pages that had no notice to say a read failed have one now, and
      /// the controls that write are named so that the code can disable them.
      /// </summary>
      public static TheoryData<string, string, string[]> Notices => new()
      {
         { "IPRangesView.xaml", "Notice", new[] { "AddRangeButton", "RestoreDefaultsButton" } },
         { "RoutesView.xaml", "Notice", new[] { "AddRouteButton" } },
         { "RulesView.xaml", "Notice", new[] { "AddRuleButton" } },
         { "TcpIpPortsView.xaml", "Notice", new[] { "AddPortButton", "RestoreDefaultsButton" } },
         { "DomainsView.xaml", "DomainNotice", new[] { "AddDomainButton", "AccountNotice", "AccountForm" } },
         { "BackupView.xaml", "Notice", new[] { "BackupCard" } },
      };

      [Theory]
      [MemberData(nameof(Notices))]
      public void EveryXamlListPage_HasANoticeForAFailedRead_AndNamesTheControlsThatWrite(string file, string notice, string[] writes)
      {
         XElement page = XDocument.Parse(ReadView(file)).Root;
         XElement found = page.Descendants(Scaffold + "InlineNotice").FirstOrDefault(e => (string)e.Attribute(X + "Name") == notice);
         Assert.True(found != null, file + " has no InlineNotice named " + notice + ", so a failed read has nowhere to be said.");
         Assert.Equal("Collapsed", (string)found.Attribute("Visibility"));

         foreach (string write in writes)
         {
            Assert.True(page.Descendants().Any(e => (string)e.Attribute(X + "Name") == write),
               file + " has no element named " + write + ", so a failed read cannot disable it.");
         }
      }

      /// <summary>
      /// The notice goes above the list, under the header: the collection editor's
      /// sat in the row under the grid, where a failed read put "Could not load"
      /// below three hundred pixels of blank card.
      /// </summary>
      [Fact]
      public void TheCollectionEditorsNotice_IsAboveTheGrid()
      {
         string build = WithheldRowTests.MemberText(ReadView("CollectionEditorView.cs"), "private void Build()");
         Assert.True(build != null, "CollectionEditorView.Build is gone.");

         Match notice = Regex.Match(build, @"Grid\.SetRow\(status_, (\d+)\);");
         Match card = Regex.Match(build, @"Grid\.SetRow\(card, (\d+)\);");
         Assert.True(notice.Success && card.Success, "The collection editor no longer places its notice and its card by row.");
         Assert.True(int.Parse(notice.Groups[1].Value) < int.Parse(card.Groups[1].Value),
            "The collection editor's notice is in a row under its grid.");
      }

      /// <summary>
      /// The helper itself: a failure empties the list, hides the empty state,
      /// disables the writes and shows the notice at Critical with the one Try again
      /// button, and Clear gives the writes back only when the notice is showing a
      /// failure of its own.
      /// </summary>
      [Fact]
      public void TheHelper_EmptiesHidesDisablesAndSaysCritical_AndClearsOnlyItsOwnNotice()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "ReadFailure.cs"));

         string show = WithheldRowTests.MemberText(source,
            "public static void Show(InlineNotice notice, EmptyState empty, ItemsControl list, string prefix, string why,");
         Assert.True(show != null, "ReadFailure.Show is gone.");
         foreach (string step in new[] { "list.ItemsSource = null;", "empty.Visibility = Visibility.Collapsed;", "Enable(false, writes);",
                                         "notice.Level = StatusLevel.Critical;", "TryAgainButton(tryAgain)", "notice.Visibility = Visibility.Visible;" })
            Assert.True(show.Contains(step, StringComparison.Ordinal), "ReadFailure.Show no longer does " + step);

         string clear = WithheldRowTests.MemberText(source, "public static void Clear(InlineNotice notice, params UIElement[] writes)");
         Assert.True(clear != null, "ReadFailure.Clear is gone.");
         Assert.True(clear.Contains("Enable(true, writes);", StringComparison.Ordinal), "ReadFailure.Clear does not give the writes back.");
         Assert.True(clear.Contains("ReferenceEquals(notice.Tag, Marker)", StringComparison.Ordinal),
            "ReadFailure.Clear hides a notice that is not showing a failure of its own.");

         Assert.True(source.Contains("L(\"Try again\")", StringComparison.Ordinal), "The Try again button has lost its caption.");
      }

      /// <summary>
      /// The Try again button is on some twenty pages, and no letter is free of a
      /// collision on every one of them - in English T is Require TLS for auth, Add
      /// action, Take the role away, Restore settings and Test connection - so its
      /// caption carries no Alt key, in English or in any catalogue: WPF, given two
      /// enabled targets for one key, moves the focus between them and presses
      /// neither, and a mnemonic checker that never saw the helper's file called
      /// the key checked. build/check-mnemonics.py scans the helper now, exempts the
      /// caption from coverage, and puts the helper's captions into the scope of
      /// every page that calls it, so a key given back to one collides there.
      /// </summary>
      [Fact]
      public void TheSharedTryAgainCaption_CarriesNoAltKey_InAnyLanguage()
      {
         string helper = ReadControlPanelFile(Path.Join("Services", "ReadFailure.cs"));
         string button = WithheldRowTests.MemberText(helper, "public static Wpf.Ui.Controls.Button TryAgainButton(Action tryAgain)");
         Assert.True(button != null, "ReadFailure.TryAgainButton is gone.");

         MatchCollection captions = Regex.Matches(button, @"L\(""((?:[^""\\]|\\.)*)""\)");
         Assert.True(captions.Count > 0, "ReadFailure.TryAgainButton has no caption that goes through the catalogue.");

         string resources = ControlPanelPath("Resources");
         string[] catalogues = Directory.GetFiles(resources, "Strings*.resx");
         Assert.True(catalogues.Length >= 18, "The catalogues were not found under " + resources + ".");

         foreach (Match caption in captions)
         {
            string key = caption.Groups[1].Value;
            Assert.True(MnemonicText.Parse(key).KeyIndex < 0,
               "The shared caption \"" + key + "\" carries an Alt key, which collides on the pages that show it.");

            foreach (string catalogue in catalogues)
            {
               XElement data = XDocument.Load(catalogue).Root.Elements("data")
                  .FirstOrDefault(d => (string)d.Attribute("name") == key);
               Assert.True(data != null, Path.GetFileName(catalogue) + " has no entry for \"" + key + "\".");

               string value = (string)data.Element("value") ?? "";
               Assert.True(MnemonicText.Parse(value).KeyIndex < 0,
                  Path.GetFileName(catalogue) + " gives the shared caption \"" + key + "\" an Alt key: \"" + value + "\".");
            }
         }

         // The checker's two halves of the rule: the caption is exempt from coverage,
         // and the helper's captions are checked against every page that calls it.
         string checker = ReadRepositoryFile(Path.Join("build", "check-mnemonics.py"));
         Assert.True(checker.Contains("\"Try again\"", StringComparison.Ordinal),
            "build/check-mnemonics.py no longer exempts \"Try again\" from carrying a key.");
         Assert.True(checker.Contains("ReadFailure.cs", StringComparison.Ordinal) && checker.Contains("ReadFailure.Show(", StringComparison.Ordinal),
            "build/check-mnemonics.py no longer puts ReadFailure's captions into the scope of every page that calls ReadFailure.Show.");
      }

      private static string ReadView(string file) => ReadControlPanelFile(Path.Join("Views", file));

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelPath(relative));

      /// <summary>A file or folder under the ControlPanel project, found from the test binaries.</summary>
      private static string ControlPanelPath(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate) || Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }

      /// <summary>A file under the repository root, found from the test binaries.</summary>
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
