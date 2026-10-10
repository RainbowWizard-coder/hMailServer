// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Every list page can be searched, and searches as it is typed in (review
   /// findings F210 and F112): the collection lists in page mode had no search box,
   /// a white list of hundreds of entries could only be scrolled, the Groups page
   /// had none beside the domains page's two, and the allowed and blocked domains,
   /// the suppression list and the audit trail searched only on Enter, which
   /// nothing on the page said.
   /// </summary>
   public class ListSearchPagesTests
   {
      /// <summary>
      /// The collection editor's page mode has a search box over the list, named
      /// for what it searches, that filters live over every column the grid shows,
      /// and a search that hides every row says so without offering a first entry.
      /// </summary>
      [Fact]
      public void TheCollectionPages_SearchEveryShownColumnAsItIsTyped()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         Assert.Contains("public string SearchPlaceholder = N(\"Search this list\");", code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"if \(!embedded_\)\s*\{\s*search_ = new Toolbar \{ SearchPlaceholder = L\(spec_\.SearchPlaceholder\)"), code);
         Assert.Contains("search_.SearchTextChanged += (_, _) => ShowListState_();", code, StringComparison.Ordinal);

         string state = WithheldRowTests.MemberText(code, "private void ShowListState_()");
         Assert.True(state != null, "CollectionEditorView.ShowListState_ is gone.");
         Assert.Contains("ListSearch.Apply(grid_, query, item => item is Row row ? SearchWords_(row) : \"\");", state, StringComparison.Ordinal);
         Assert.Contains("F(\"Shown: {0} of {1}\", shown, rows_.Count)", state, StringComparison.Ordinal);
         Assert.Contains("F(\"No entry in this list has \\\"{0}\\\" in it.\", query)", state, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"empty_\.Action = null;[\s\S]{0,200}?return;[\s\S]{0,900}?empty_\.Action = addFirst_;"), state);

         // The words searched are the cells as the grid shows them - a choice's
         // label, a tick box's words - never the row's database id.
         string words = WithheldRowTests.MemberText(code, "private string SearchWords_(Row row)");
         Assert.True(words != null, "CollectionEditorView.SearchWords_ is gone.");
         Assert.Contains("CellWords_(f, row)", words, StringComparison.Ordinal);
         Assert.DoesNotContain(".Id", words, StringComparison.Ordinal);
      }

      /// <summary>Each collection page names what its search box searches, which is the box's accessible name.</summary>
      [Theory]
      [InlineData("SurblServers", "Search SURBL servers")]
      [InlineData("DnsBlackLists", "Search DNS block lists")]
      [InlineData("SpamWhiteList", "Search spam exemptions")]
      [InlineData("BlockedSenders", "Search blocked senders")]
      [InlineData("SenderList", "Search the allow and block list")]
      [InlineData("GreyListWhiteList", "Search greylisting exemptions")]
      [InlineData("GreyListBypass", "Search the greylisting bypass list")]
      [InlineData("BlockedAttachments", "Search blocked attachment patterns")]
      [InlineData("Groups", "Search security groups")]
      [InlineData("RemoteDomains", "Search remote domains")]
      [InlineData("ServerMessages", "Search server messages")]
      public void EachCollectionPage_NamesWhatItSearches(string spec, string placeholder)
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         int start = specs.IndexOf(" " + spec + "() => new", StringComparison.Ordinal);
         Assert.True(start > 0, "CollectionSpecs." + spec + " is gone.");
         int end = specs.IndexOf("public static", start + 1, StringComparison.Ordinal);
         string body = end > 0 ? specs.Substring(start, end - start) : specs.Substring(start);
         Assert.Contains("SearchPlaceholder = N(\"" + placeholder + "\"),", body, StringComparison.Ordinal);
      }

      /// <summary>The Groups page is the editor's page mode, with its search and count, and its Members row under the list.</summary>
      [Fact]
      public void TheGroupsPage_IsAListPageWithItsSearch()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         Assert.Contains("editor_ = new CollectionEditorView(spec);", specs, StringComparison.Ordinal);
         Assert.Contains("editor_.AddBelowList(membersRow);", specs, StringComparison.Ordinal);
         Assert.DoesNotContain("new CollectionEditorView(spec, embedded: true)", specs, StringComparison.Ordinal);
      }

      /// <summary>The allowed and blocked domains filter the rows already read as they are typed in, and never over a list that could not be read.</summary>
      [Fact]
      public void TheDomainList_FiltersAsItIsTyped()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "DomainPolicyListView.cs"));
         Assert.Contains("toolbar_.SearchTextChanged += (_, _) => { if (!ReadFailure.IsShown(status_)) Present_(); };", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Key.Enter) Present_()", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The two searches that ask the server run once typing pauses, and at once
      /// on Enter; the wait is dropped as the page is left.
      /// </summary>
      [Theory]
      [InlineData("SuppressionListView.cs")]
      [InlineData("AuditTrailView.cs")]
      public void TheServerSearches_RunOnceTypingPauses(string file)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains("toolbar_.SearchTextChanged += (_, _) => searchDelay_.Restart();", code, StringComparison.Ordinal);
         Assert.Contains("Key.Enter) searchDelay_.Now();", code, StringComparison.Ordinal);
         Assert.Contains("public void OnLeave() => searchDelay_.Cancel();", code, StringComparison.Ordinal);

         string delay = ReadControlPanelFile(Path.Join("Services", "SearchDelay.cs"));
         Assert.Contains("public const int DelayMilliseconds = 300;", delay, StringComparison.Ordinal);
      }

      /// <summary>The public folders can be searched by name, and a search that hides every one says so.</summary>
      [Fact]
      public void ThePublicFolders_CanBeSearched()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "PublicFoldersView.cs"));
         Assert.Contains("new() { SearchPlaceholder = L(\"Search public folders\") }", code, StringComparison.Ordinal);
         Assert.Contains("search_.SearchTextChanged += (_, _) => ShowSearch_();", code, StringComparison.Ordinal);
         string show = WithheldRowTests.MemberText(code, "private void ShowSearch_()");
         Assert.True(show != null, "PublicFoldersView.ShowSearch_ is gone.");
         Assert.Contains("ListSearch.Apply(list_, query);", show, StringComparison.Ordinal);
         Assert.Contains("No public folder has", show, StringComparison.Ordinal);
      }

      /// <summary>
      /// No list page's search waits for Enter alone: a search box whose only
      /// handler is a key press reads as a filter that does not work.
      /// </summary>
      [Fact]
      public void NoListPage_SearchesOnEnterAlone()
      {
         string views = ControlPanelDirectory("Views");
         var enterOnly = new Regex(@"toolbar_\.PreviewKeyDown \+= \(_, e\) => \{ if \(e\.Key == System\.Windows\.Input\.Key\.Enter\)");
         string[] waiting = Directory.EnumerateFiles(views, "*.cs", SearchOption.AllDirectories)
            .Where(f => !f.EndsWith("MessageTraceView.cs", StringComparison.Ordinal))
            .Where(f =>
            {
               string text = File.ReadAllText(f);
               return enterOnly.IsMatch(text) && !text.Contains("toolbar_.SearchTextChanged +=", StringComparison.Ordinal);
            })
            .Select(Path.GetFileName)
            .ToArray();

         // The message trace's box is a query form with a Search button beside it.
         Assert.True(waiting.Length == 0, "Searches on Enter alone: " + string.Join(", ", waiting));
      }

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
