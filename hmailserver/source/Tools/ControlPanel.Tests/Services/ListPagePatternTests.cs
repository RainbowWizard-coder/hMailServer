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
   /// One list page pattern (review finding F288; docs/ControlPanelDesign.md, List
   /// pages): the count as a Normal pill beside the title, a polite live region
   /// that a screen reader hears as it changes, Refresh in the header,
   /// the list's verbs in the toolbar over it with the Primary last, words and no
   /// glyphs on those buttons, and never a count in an Information notice, a
   /// caption under the list or the subtitle. The pages used four action layouts
   /// and five ways to show a count.
   /// </summary>
   public class ListPagePatternTests
   {
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      public static TheoryData<string> XamlListPages => new()
      {
         "RoutesView.xaml", "TcpIpPortsView.xaml", "IPRangesView.xaml", "SslCertificatesView.xaml", "RulesView.xaml", "QueueView.xaml"
      };

      /// <summary>The XAML list pages: the count pill beside the title, a polite live region, and Refresh in the header.</summary>
      [Theory]
      [MemberData(nameof(XamlListPages))]
      public void AXamlListPage_HasTheCountAndRefreshInItsHeader(string file)
      {
         XElement header = Page(file).Descendants(Scaffold + "PageHeader").Single();

         XElement status = header.Element(Scaffold + "PageHeader.Status");
         Assert.True(status != null, file + ": the header has no count.");
         XElement pill = status.Element(Scaffold + "StatusPill");
         Assert.True(pill != null && (string)pill.Attribute(X + "Name") == "CountPill" && (string)pill.Attribute("Level") == "Normal",
            file + ": the count is not a Normal StatusPill named CountPill.");
         Assert.True((string)pill.Attribute("AutomationProperties.LiveSetting") == "Polite",
            file + ": the count is not a polite live region, so a screen reader never hears it change.");

         XElement actions = header.Element(Scaffold + "PageHeader.Actions");
         Assert.True(actions != null && actions.Descendants(Ui + "Button").Any(b => (string)b.Attribute("Content") == "{loc:L '_Refresh'}"),
            file + ": the header has no Refresh.");

         // The code behind counts with the catalogue's plural forms, and hides the
         // count with a read that failed.
         string code = ReadControlPanelFile(Path.Join("Views", file + ".cs"));
         Assert.Matches(new Regex(@"n => Plural\(n, ""\{0\} [^""]+"", ""\{0\} [^""]+""\)"), code);
         Assert.Contains("CountPill.Visibility = ", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The XAML list pages' search says what it shows (review findings F288 and
      /// F210): the count, the filter and the empty state go through one place,
      /// Services/ListSearchState, after every read that worked and every keystroke
      /// in the search - "Shown: 1 of 2" while a search is typed in, and the no-match
      /// sentence without the add action where it hides every row. These pages
      /// filtered the grid alone: "2 routes" beside a blank card while a search hid
      /// both.
      /// </summary>
      [Theory]
      [MemberData(nameof(XamlListPages))]
      public void AXamlListPage_SaysWhatItsSearchShows(string file)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file + ".cs"));
         Assert.Contains("private void Search_TextChanged(object sender, EventArgs e) => ShowListState_();", code, StringComparison.Ordinal);

         string state = WithheldRowTests.MemberText(code, "private void ShowListState_()");
         Assert.True(state != null, file + ".cs has no ShowListState_.");
         Assert.Contains("ListSearchState.Show(", state, StringComparison.Ordinal);
         Assert.Contains("SearchBar.SearchText, CountPill,", state, StringComparison.Ordinal);
         Assert.Contains("EmptyStatus, ", state, StringComparison.Ordinal);

         // Reload shows the pill and then the state, so that a read that failed -
         // whose pill is hidden - is neither counted nor called empty.
         Assert.True(Regex.IsMatch(code, @"CountPill\.Visibility = [^;]+;\s*ShowListState_\(\);"),
            file + ".cs does not show its list's state after showing its pill.");

         // No one sets the count or filters the grid past the state.
         Assert.DoesNotContain("CountPill.Text =", code, StringComparison.Ordinal);
         Assert.DoesNotContain("ListSearch.Apply(", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The one place: the filter, then the pill and the empty state only while the
      /// pill is shown, the add action taken away for the no-match sentence and
      /// given back, both through Services/ListSearchCount.
      /// </summary>
      [Fact]
      public void TheListSearchState_FiltersAndThenSaysWhatItShows()
      {
         string code = ReadControlPanelFile(Path.Join("Services", "ListSearchState.cs"));
         string show = WithheldRowTests.MemberText(code, "public static void Show(");
         Assert.True(show != null, "ListSearchState.Show is gone.");

         int apply = show.IndexOf("ListSearch.Apply(list, query);", StringComparison.Ordinal);
         int guard = show.IndexOf("pill.Visibility != Visibility.Visible", StringComparison.Ordinal);
         int text = show.IndexOf("pill.Text = ListSearchCount.PillText(query, shown, total, count(total));", StringComparison.Ordinal);
         Assert.True(apply >= 0 && guard > apply && text > guard, "ListSearchState.Show does not filter, then ask whether the read worked, then count.");
         Assert.Contains("if (ListSearchCount.NothingMatches(query, shown, total))", show, StringComparison.Ordinal);
         Assert.Contains("empty.Action = null;", show, StringComparison.Ordinal);
         Assert.Contains("empty.Text = ListSearchCount.NothingMatchesText(query);", show, StringComparison.Ordinal);
         Assert.Contains("empty.Action = held.Action;", show, StringComparison.Ordinal);
      }

      /// <summary>
      /// The pages that filter their rows themselves say the same: the allowed and
      /// blocked domains count what they show and say where nothing matches, which
      /// was a blank card; the public folders and the quarantine count what their
      /// search shows, where they kept the whole list's number.
      /// </summary>
      [Fact]
      public void TheSelfFilteringLists_CountWhatTheirSearchShows()
      {
         string policy = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Views", "DomainPolicyListView.cs")), "private void Present_()");
         Assert.True(policy != null, "DomainPolicyListView.Present_ is gone.");
         Assert.Contains("count_.Text = ListSearchCount.PillText(search, shown.Count, all_.Count,", policy, StringComparison.Ordinal);
         int nothing = policy.IndexOf("if (ListSearchCount.NothingMatches(search, shown.Count, all_.Count))", StringComparison.Ordinal);
         int back = policy.IndexOf("empty_.Action = addFirst_;", StringComparison.Ordinal);
         Assert.True(nothing >= 0 && back > nothing, "DomainPolicyListView.Present_ has no no-match state, or does not give the add action back after it.");
         Assert.Contains("empty_.Text = ListSearchCount.NothingMatchesText(search);", policy, StringComparison.Ordinal);

         string folders = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Views", "PublicFoldersView.cs")), "private void ShowSearch_()");
         Assert.True(folders != null, "PublicFoldersView.ShowSearch_ is gone.");
         Assert.Contains("count_.Text = ListSearchCount.PillText(query, shown, folders.Count,", folders, StringComparison.Ordinal);

         string quarantine = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs")), "private void ShowSearchState_()");
         Assert.True(quarantine != null, "QuarantineView.ShowSearchState_ is gone.");
         int failed = quarantine.IndexOf("if (ReadFailure.IsShown(status_))", StringComparison.Ordinal);
         int counted = quarantine.IndexOf("count_.Text = ListSearchCount.PillText(review_.Filter, review_.Shown.Count, review_.Messages.Count,", StringComparison.Ordinal);
         Assert.True(failed >= 0 && counted > failed, "QuarantineView.ShowSearchState_ does not count what its search shows after asking whether the read worked.");
      }

      public static TheoryData<string, string> CodeListPageRefresh => new()
      {
         { "CollectionEditorView.cs", "Actions = refresh" },
         { "SuppressionListView.cs", "Actions = MakeButton(L(\"_Refresh\")" },
         { "DomainPolicyListView.cs", "Actions = MakeButton(L(\"_Refresh\")" },
         { "AuditTrailView.cs", "Actions = MakeButton(L(\"_Refresh\")" },
         { "MessageTraceView.cs", "Actions = refresh_," },
         { "PublicFoldersView.cs", "header_.Actions = MakeButton(L(\"_Refresh\")" },
      };

      /// <summary>
      /// The code-built list pages have Refresh in their header (the quarantine's,
      /// after Digest settings, is TheQuarantine_PutsItsFilterAndListVerbInTheToolbar's).
      /// The message trace had none and F5 alone: its Refresh runs the last search or
      /// the followed message again, and cannot be pressed before the first.
      /// </summary>
      [Theory]
      [MemberData(nameof(CodeListPageRefresh))]
      public void ACodeListPage_HasRefreshInItsHeader(string file, string actions)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains(actions, code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheMessageTrace_RefreshesTheLastSearch_OnceThereIsOne()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "MessageTraceView.cs"));
         Assert.Contains("refresh_ = MakeButton(L(\"_Refresh\"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => rerun_?.Invoke());", code, StringComparison.Ordinal);
         Assert.Contains("refresh_.IsEnabled = rerun_ != null;", code, StringComparison.Ordinal);

         string remember = WithheldRowTests.MemberText(code, "private void Remember_(Action rerun)");
         Assert.True(remember != null, "MessageTraceView.Remember_ is gone.");
         Assert.Contains("refresh_.IsEnabled = true;", remember, StringComparison.Ordinal);
         Assert.Contains("Remember_(() => Search_(address));", code, StringComparison.Ordinal);
         Assert.Contains("Remember_(() => Follow_(queueId));", code, StringComparison.Ordinal);
         Assert.DoesNotContain("rerun_ = () =>", code, StringComparison.Ordinal);
      }

      /// <summary>No button in a page header or a toolbar carries a glyph: a glyph is a row's button and an empty state's.</summary>
      [Theory]
      [MemberData(nameof(XamlListPages))]
      public void AXamlListPage_HeaderAndToolbarButtons_AreWordsAlone(string file)
      {
         XElement root = Page(file);
         var slots = root.Descendants(Scaffold + "PageHeader.Actions").Concat(root.Descendants(Scaffold + "Toolbar.Actions"));
         foreach (XElement button in slots.SelectMany(s => s.Descendants(Ui + "Button")))
         {
            Assert.True(button.Attribute("Icon") == null && button.Element(Ui + "Button.Icon") == null,
               file + ": " + (string)button.Attribute("Content") + " carries a glyph.");
         }
      }

      public static TheoryData<string, string> CodeListPages => new()
      {
         { "CollectionEditorView.cs", "Status = countBadge_," },
         { "SuppressionListView.cs", "Status = count_," },
         { "DomainPolicyListView.cs", "Status = count_," },
         { "AuditTrailView.cs", "Status = count_," },
         { "MessageTraceView.cs", "Status = count_," },
         { "QuarantineView.cs", "Status = count_," },
         { "PublicFoldersView.cs", "header_.Status = count_;" },
      };

      /// <summary>
      /// The code-built list pages put their count beside the title, as a Normal
      /// pill, and make it a polite live region: the suppression list's, the
      /// quarantine's, the message trace's and the audit trail's counts were
      /// Information notices, which announced themselves, and a pill is silent
      /// unless it is one (Scaffold/StatusPill; WCAG 2.2 SC 4.1.3).
      /// </summary>
      [Theory]
      [MemberData(nameof(CodeListPages))]
      public void ACodeListPage_HasItsCountBesideTheTitle(string file, string status)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains(status, code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"(count_|countBadge_) = new\(\) \{ Level = StatusLevel\.Normal"), code);
         Assert.Matches(new Regex(@"AutomationProperties\.SetLiveSetting\((count_|countBadge_), (System\.Windows\.Automation\.)?AutomationLiveSetting\.Polite\);"), code);
      }

      /// <summary>
      /// A count is never an Information notice, a caption under the list or the
      /// subtitle: "3 addresses listed.", "5 messages held.", "4 public folders.",
      /// "4 messages waiting for delivery.", "4 rules of the server", "1 to 6 of
      /// 1284" and "Entries that match the search: 1 of 4." were all one of those.
      /// A notice that says a list is longer than the page reads is a state, and
      /// stays.
      /// </summary>
      [Fact]
      public void NoListPage_SaysItsCountInANotice()
      {
         string views = ControlPanelDirectory("Views");
         var notice = new Regex(@"Status_\(StatusLevel\.Information, (Plural\(|F\(""\{0\} to \{1\} of \{2\}""|F\(""Entries that match)");
         var subtitle = new Regex(@"Subtitle = [^;]*Plural\(");
         string[] files = { "SuppressionListView.cs", "QuarantineView.cs", "MessageTraceView.cs", "AuditTrailView.cs", "DomainPolicyListView.cs", "PublicFoldersView.cs", "QueueView.xaml.cs", "RulesView.xaml.cs" };
         foreach (string file in files)
         {
            string code = File.ReadAllText(Path.Join(views, file));
            Assert.False(notice.IsMatch(code), file + " says a count in an Information notice.");
            Assert.False(subtitle.IsMatch(code), file + " counts in its subtitle.");
         }

         Assert.DoesNotContain("count_.SetResourceReference(StyleProperty, \"TextCaption\")", File.ReadAllText(Path.Join(views, "PublicFoldersView.cs")), StringComparison.Ordinal);
      }

      /// <summary>
      /// The collection editor's pages: Refresh in the header, Edit, Delete and then
      /// Add, the Primary, in the toolbar with the search; in a dialog's tab, which
      /// has no header, Refresh before Add. None of them carries a glyph.
      /// </summary>
      [Fact]
      public void TheCollectionEditor_FollowsThePattern()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         string build = WithheldRowTests.MemberText(code, "private void Build()");
         Assert.True(build != null, "CollectionEditorView.Build is gone.");

         int edit = build.IndexOf("L(\"_Edit\")", StringComparison.Ordinal);
         int delete = build.IndexOf("L(\"_Delete\")", StringComparison.Ordinal);
         int refresh = build.IndexOf("L(\"_Refresh\")", StringComparison.Ordinal);
         int add = build.IndexOf("L(\"_Add\")", StringComparison.Ordinal);
         Assert.True(edit >= 0 && delete > edit && refresh > delete && add > refresh, "Edit, Delete, Refresh and Add are not built in that order.");
         Assert.Matches(new Regex(@"if \(embedded_\)\s*actions\.Children\.Add\(refresh\);"), build);
         Assert.Contains("Actions = refresh", build, StringComparison.Ordinal);
         Assert.Contains("SearchPlaceholder = L(spec_.SearchPlaceholder), Actions = actions", build, StringComparison.Ordinal);

         string make = WithheldRowTests.MemberText(code, "private static Button MakeButton(string text, ControlAppearance appearance, RoutedEventHandler onClick)");
         Assert.True(make != null, "CollectionEditorView.MakeButton is gone, or takes a glyph again.");
         Assert.DoesNotContain("Icon", make, StringComparison.Ordinal);
      }

      /// <summary>
      /// The quarantine's grouping is a filter beside its search, and Remove expired
      /// acts on the list from the toolbar; the checked messages' four acts are the
      /// bar under it (review finding F260).
      /// </summary>
      [Fact]
      public void TheQuarantine_PutsItsFilterAndListVerbInTheToolbar()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));
         Assert.Contains("search_.Filters = filters;", code, StringComparison.Ordinal);
         Assert.Contains("search_.Actions = removeExpired_;", code, StringComparison.Ordinal);
         // The checked messages' four acts are a bar of their own, under the toolbar.
         Assert.True(code.IndexOf("parts.Children.Add(bulkBar);", StringComparison.Ordinal) > code.IndexOf("parts.Children.Add(search_);", StringComparison.Ordinal),
            "The checked messages' bar is not under the toolbar.");
         Assert.Matches(new Regex(@"L\(""Di_gest settings""\)[\s\S]{0,200}?L\(""_Refresh""\)"), code);
      }

      /// <summary>
      /// The toolbar's actions go on a row of their own once the widest does not fit
      /// beside the search and the filters - the suppression list's in the narrowest
      /// window - and stay beside them otherwise.
      /// </summary>
      [Fact]
      public void TheToolbar_PutsItsActionsUnderWhenTheWidestDoesNotFit()
      {
         // 650 wide, a 220 search and a filter of 400: 18 left, Add… needs 80.
         Assert.True(ToolbarRows.ActionsUnder(650, 220, 400, 80));
         // 1000 wide: 368 left.
         Assert.False(ToolbarRows.ActionsUnder(1000, 220, 400, 80));
         // Exactly the width it needs fits.
         Assert.False(ToolbarRows.ActionsUnder(220 + 400 + ToolbarRows.Gap + 80, 220, 400, 80));
         // No actions, or a toolbar of actions alone: nothing to put under.
         Assert.False(ToolbarRows.ActionsUnder(300, 220, 0, 0));
         Assert.False(ToolbarRows.ActionsUnder(100, 0, 0, 300));

         // The filters go under the search where the two are wider than the
         // toolbar: the German suppression list's at 760 pixels, 220 and 438 in 652.
         Assert.True(ToolbarRows.FiltersUnder(652, 220, 438));
         Assert.False(ToolbarRows.FiltersUnder(652, 220, 400));
         Assert.False(ToolbarRows.FiltersUnder(652, 0, 700));

         string toolbar = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Toolbar.cs"));
         Assert.Contains("protected override Size MeasureOverride(Size constraint)", toolbar, StringComparison.Ordinal);
         Assert.Contains("Services.ToolbarRows.FiltersUnder(constraint.Width, search, filters)", toolbar, StringComparison.Ordinal);
         Assert.Contains("Place_(actions, row,", toolbar, StringComparison.Ordinal);

         // Actions that cannot wrap are as wide as all of them: the message trace's
         // two, in a StackPanel, were cut at 760 pixels in German beside its search.
         Assert.Contains("if (actions is System.Windows.Controls.WrapPanel panel)", toolbar, StringComparison.Ordinal);
      }

      private static XElement Page(string file) => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", file))).Root;

      private static string ControlPanelDirectory(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative + " could not be found from the test binaries.");
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
