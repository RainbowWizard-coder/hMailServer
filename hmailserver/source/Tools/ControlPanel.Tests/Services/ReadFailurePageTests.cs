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
      /// below three hundred pixels of blank card. The page is laid out by a
      /// FillPanel, whose parts are in the order they are added, so the notice
      /// is the part added before the filling card.
      /// </summary>
      [Fact]
      public void TheCollectionEditorsNotice_IsAboveTheGrid()
      {
         string build = WithheldRowTests.MemberText(ReadView("CollectionEditorView.cs"), "private void Build()");
         Assert.True(build != null, "CollectionEditorView.Build is gone.");

         int notice = build.IndexOf("parts.Children.Add(status_);", StringComparison.Ordinal);
         int card = build.IndexOf("parts.Children.Add(FillPanel.Filling(card,", StringComparison.Ordinal);
         Assert.True(notice >= 0 && card >= 0, "The collection editor no longer places its notice and its card in the FillPanel.");
         Assert.True(notice < card, "The collection editor's notice is under its grid.");
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
      /// The keyboard focus survives Try again. The button runs the page's read;
      /// when the read fails again - the common case right after a lost link - the
      /// page calls Show again, and a new button in the old one's place took the
      /// focus with it to the window, where a screen reader announced the window
      /// and Tab started again from the top of the page; a read that worked took
      /// the button away with the focus on it and left the focus nowhere. Show
      /// keeps the button a notice already carries and gives it the new read, and
      /// Clear moves the focus to the first control that writes, or on to the next
      /// control in the tab order, before it takes the button out.
      /// </summary>
      [Fact]
      public void TheHelper_KeepsTheButtonAcrossFailures_AndMovesTheFocusOnSuccess()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "ReadFailure.cs"));

         string show = WithheldRowTests.MemberText(source,
            "public static void Show(InlineNotice notice, EmptyState empty, ItemsControl list, string prefix, string why,");
         Assert.True(show != null, "ReadFailure.Show is gone.");
         Assert.True(show.Contains("IsShown(notice) && notice.Content is Wpf.Ui.Controls.Button kept", StringComparison.Ordinal),
            "ReadFailure.Show replaces the Try again button on every failure, which drops the keyboard focus to the window.");
         Assert.True(show.Contains("retry.Run = tryAgain;", StringComparison.Ordinal),
            "ReadFailure.Show keeps the button but not the read it should run.");

         string button = WithheldRowTests.MemberText(source, "public static Wpf.Ui.Controls.Button TryAgainButton(Action tryAgain)");
         Assert.True(button != null && button.Contains("button.Tag = retry;", StringComparison.Ordinal) && button.Contains("retry.Run", StringComparison.Ordinal),
            "The Try again button no longer runs the read its notice last gave it.");

         string clear = WithheldRowTests.MemberText(source, "public static void Clear(InlineNotice notice, params UIElement[] writes)");
         Assert.True(clear != null, "ReadFailure.Clear is gone.");
         Assert.True(clear.Contains("if (notice.IsKeyboardFocusWithin)", StringComparison.Ordinal)
                     && clear.IndexOf("IsKeyboardFocusWithin", StringComparison.Ordinal) < clear.IndexOf("notice.Content = null;", StringComparison.Ordinal),
            "ReadFailure.Clear takes the Try again button out of the tree with the keyboard focus still on it.");

         string move = WithheldRowTests.MemberText(source, "private static void MoveFocusFrom(InlineNotice notice, UIElement[] writes)");
         Assert.True(move != null, "ReadFailure.MoveFocusFrom is gone.");
         Assert.True(move.Contains("write.Focus()", StringComparison.Ordinal) && move.Contains("FocusNavigationDirection.First", StringComparison.Ordinal),
            "ReadFailure.MoveFocusFrom does not move the focus to the first control that writes.");
         Assert.True(move.Contains("FocusNavigationDirection.Next", StringComparison.Ordinal),
            "ReadFailure.MoveFocusFrom leaves the focus nowhere on a page with no control that writes.");
      }

      /// <summary>
      /// The domain white and black list filters its rows on Enter in the search
      /// box, and filtering nothing showed "The list is empty, and an empty list
      /// changes nothing" under the notice that said the list could not be read.
      /// A search while the failure is shown reads again instead, before anything
      /// is presented.
      /// </summary>
      [Fact]
      public void TheDomainPolicyList_ReadsAgainRatherThanFilteringAListItCouldNotRead()
      {
         string present = WithheldRowTests.MemberText(ReadView("DomainPolicyListView.cs"), "private void Present_()");
         Assert.True(present != null, "DomainPolicyListView.Present_ is gone.");

         int asks = present.IndexOf("ReadFailure.IsShown(status_)", StringComparison.Ordinal);
         int shows = present.IndexOf("StatusText.Show(", StringComparison.Ordinal);
         Assert.True(asks >= 0, "DomainPolicyListView.Present_ filters a list that could not be read, and shows its empty state under the failure.");
         Assert.True(shows > asks, "DomainPolicyListView.Present_ shows the empty state before it asks whether the read failed.");
         Assert.True(present.IndexOf("Reload();", StringComparison.Ordinal) > asks && present.IndexOf("Reload();", StringComparison.Ordinal) < shows,
            "DomainPolicyListView.Present_ does not read again when the failure is shown.");
      }

      /// <summary>
      /// The journaling page's list, empty state and row actions sit in a card;
      /// hiding the three and leaving the card put a bare strip between the
      /// failure notice and the form. The card goes with its contents and comes
      /// back with a read that works.
      /// </summary>
      [Fact]
      public void TheJournalingPage_HidesTheListCardWithTheList_WhenTheReadFails()
      {
         string source = ReadView("JournalingView.cs");
         string unread = WithheldRowTests.MemberText(source, "private void Unread_(string prefix, string why)");
         string reload = WithheldRowTests.MemberText(source, "private void Reload_()");
         Assert.True(unread != null && reload != null, "JournalingView.Unread_ or Reload_ is gone.");

         Assert.True(unread.Contains("listCard_.Visibility = Visibility.Collapsed;", StringComparison.Ordinal),
            "JournalingView.Unread_ leaves the list's card on the page with nothing in it.");
         Assert.True(reload.Contains("listCard_.Visibility = Visibility.Visible;", StringComparison.Ordinal)
                     && reload.IndexOf("ReadFailure.Clear(", StringComparison.Ordinal) < reload.IndexOf("listCard_.Visibility = Visibility.Visible;", StringComparison.Ordinal),
            "JournalingView.Reload_ does not bring the list's card back after a read that works.");
      }

      /// <summary>
      /// The backup page's two hMailServer.ini boxes - metadata only, verify each
      /// backup - are read before the settings object is opened, and the catch
      /// cleared every other box but left them: with the link lost they hold the
      /// file's copy or the default, and BackupVerifyRestore's default is on, so
      /// "Verify each backup…" stayed ticked under "could not read the backup
      /// settings". The catch clears them with the rest.
      /// </summary>
      [Fact]
      public void TheBackupPage_ClearsItsIniBoxesToo_WhenTheReadFails()
      {
         string enter = WithheldRowTests.MemberText(ReadView("BackupView.xaml.cs"), "public void OnEnter()");
         Assert.True(enter != null, "BackupView.OnEnter is gone.");

         int caught = enter.IndexOf("catch (Exception ex)", StringComparison.Ordinal);
         Assert.True(caught >= 0, "BackupView.OnEnter no longer catches a failed read.");
         Assert.True(enter.IndexOf("CheckMessagesDbOnly.IsChecked = CheckVerifyRestore.IsChecked = false;", StringComparison.Ordinal) > caught,
            "BackupView.OnEnter leaves the two hMailServer.ini boxes as they were when the backup settings could not be read.");
      }

      /// <summary>
      /// The transport encryption overview resolves each port's certificate id
      /// against the certificates it has read; with the certificates unread the
      /// collection is empty, every TLS listener came back with no certificate,
      /// and TlsPosture judged each one "Will not start" at Critical while the
      /// certificates card said they could not be read. The listener pass marks
      /// the certificate as not read instead, and TlsPosture says so of it
      /// (TlsPostureTests.UnreadCertificates_AreNotNoCertificate).
      /// </summary>
      [Fact]
      public void TheTransportOverview_MarksAListenerWhoseCertificateWasNotRead()
      {
         string read = WithheldRowTests.MemberText(ReadView("TlsOverviewView.cs"), "private void ReadListeners(TlsPostureConfig config, dynamic settings)");
         Assert.True(read != null, "TlsOverviewView.ReadListeners is gone.");
         Assert.True(read.Contains("config.WasRead(nameof(config.Certificates))", StringComparison.Ordinal),
            "TlsOverviewView.ReadListeners never asks whether the certificates were read, so an unread collection is \"no certificate\".");
         Assert.True(read.Contains("CertificateUnread = !certificatesRead", StringComparison.Ordinal),
            "TlsOverviewView.ReadListeners does not mark a listener whose certificate could not be read.");
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
