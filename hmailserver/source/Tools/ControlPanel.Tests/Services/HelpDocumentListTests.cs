// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The help pane's list of every document (review finding F315): some 120 by
   /// file name in one run, ContinuousIntegration, Fuzzing and Nix among the
   /// operators' guides, a search box over it that said "Search this document",
   /// and no way back from a document it opened. It is grouped as the sidebar is
   /// now, then Reference and Building and packaging, each by title; the box
   /// searches every document while the list shows; Back returns. The dead
   /// AccountRules spec, whose subtitle said rules' criteria could not be edited
   /// here, is gone with its texts.
   /// </summary>
   public class HelpDocumentListTests
   {
      private static IReadOnlyList<HelpDocumentSection> All() => HelpDocumentList.Sections(HelpPages.Names, HelpPages.TitleOf);

      [Fact]
      public void EveryDocumentIsListedOnce_UnderTheSidebarsGroupsThenReferenceThenBuilding()
      {
         IReadOnlyList<HelpDocumentSection> sections = All();
         List<string> listed = sections.SelectMany(s => s.Names).ToList();
         Assert.Equal(HelpPages.Names.OrderBy(n => n, StringComparer.Ordinal), listed.OrderBy(n => n, StringComparer.Ordinal));

         List<string> groups = NavigationMap.Groups.Select(g => g.Title).ToList();
         List<string> headings = sections.Select(s => s.Heading).ToList();
         Assert.Equal(HelpDocumentList.Building, headings[^1]);
         Assert.Equal(HelpDocumentList.Reference, headings[^2]);
         List<string> grouped = headings.Take(headings.Count - 2).ToList();
         Assert.True(grouped.Count >= 5, "Fewer than five of the sidebar's groups list a document.");
         Assert.All(grouped, h => Assert.Contains(h, groups));
         Assert.Equal(grouped.OrderBy(groups.IndexOf), grouped);
      }

      [Fact]
      public void AGroupListsWhatItsPagesOpen_AndTheProjectsOwnDocumentsComeLast()
      {
         IReadOnlyList<HelpDocumentSection> sections = All();
         HelpDocumentSection Of(string name) => sections.Single(s => s.Names.Contains(name, StringComparer.OrdinalIgnoreCase));

         Assert.Equal("Set up this server", Of("SetupChecklist").Heading);
         Assert.Equal("Maintenance", Of("Backup").Heading);
         foreach (string build in new[] { "ContinuousIntegration", "Fuzzing", "Nix", "ReproducibleBuilds" })
            Assert.Equal(HelpDocumentList.Building, Of(build).Heading);
         Assert.Equal(HelpDocumentList.Reference, Of("HighAvailabilityRunbook").Heading);

         // One rule for the documents of the packages, each of which says how its
         // package is installed, how it is built and what has run: the snap stood
         // under Reference while the Nix flake and Alpine, its counterparts, stood
         // under Building and packaging.
         Assert.Equal(Of("Nix").Heading, Of("Snap").Heading);
         Assert.Equal(Of("Nix").Heading, Of("Alpine").Heading);
         foreach (string package in new[] { "Snap", "ContainerImage", "PackageManagers", "CommandLinePackages", "PackagesDeb", "PackagesRpm", "PackagesAppImage", "PackagesWindows", "SourceTarball", "SourcePackages" })
            Assert.Equal(HelpDocumentList.Building, Of(package).Heading);

         Assert.All(sections, s => Assert.Equal(s.Names.OrderBy(HelpPages.TitleOf, StringComparer.CurrentCultureIgnoreCase), s.Names));
         Assert.All(HelpDocumentList.BuildingAndPackaging, name => Assert.True(HelpPages.Has(name), name + " is not a document."));
      }

      [Fact]
      public void TheSearchListsTheDocumentsThatHoldTheWords()
      {
         IReadOnlyList<HelpDocumentSection> found = HelpDocumentList.Sections(HelpPages.Names, HelpPages.TitleOf, HelpPages.Read, "DNSSEC");
         List<string> names = found.SelectMany(s => s.Names).ToList();
         Assert.NotEmpty(names);
         Assert.True(names.Count < HelpPages.Names.Count, "The search left every document in the list.");
         Assert.All(names, n => Assert.True(
            HelpPages.TitleOf(n).Contains("DNSSEC", StringComparison.CurrentCultureIgnoreCase) || HelpPages.Read(n).Contains("DNSSEC", StringComparison.CurrentCultureIgnoreCase),
            n + " does not hold the words searched for."));

         Assert.Empty(HelpDocumentList.Sections(HelpPages.Names, HelpPages.TitleOf, HelpPages.Read, "zqxjkvw"));
         // One character narrows nothing.
         Assert.Equal(HelpPages.Names.Count, HelpDocumentList.Sections(HelpPages.Names, HelpPages.TitleOf, HelpPages.Read, "a").Sum(s => s.Names.Count));
      }

      [Fact]
      public void ThePaneSaysWhatItSearches_AndGoesBack()
      {
         string pane = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "HelpPane.cs"));
         string list = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "HelpPane.List.cs"));
         Assert.Contains("string label = shown_ == null ? L(\"Search all documentation\") : L(\"Search this document\");", list, StringComparison.Ordinal);
         Assert.Contains("search_.TextChanged += (s, e) => SearchChanged_();", pane, StringComparison.Ordinal);
         Assert.Contains("IReadOnlyList<HelpDocumentSection> sections = Sections_(term);", list, StringComparison.Ordinal);
         Assert.Contains("AddDocumentSections_(document);", pane, StringComparison.Ordinal);

         // Back: kept by every link in the pane, started again by a page's help.
         Assert.Contains("AutomationProperties.SetAutomationId(backButton_, \"help-back\");", list, StringComparison.Ordinal);
         string all = pane[pane.IndexOf("allButton_.Click += (s, e) =>", StringComparison.Ordinal)..];
         Assert.Contains("GoTo_(ShowAll);", all[..all.IndexOf("};", StringComparison.Ordinal)], StringComparison.Ordinal);
         Assert.Contains("(doc, fragment) => GoTo_(() => ShowDocument(doc, HeadingForFragment_(doc, fragment)))", pane, StringComparison.Ordinal);
         Assert.Contains("link.Click += (s, e) => GoTo_(() => ShowDocument(opened, null));", list, StringComparison.Ordinal);
         string showPage = WithheldRowTests.MemberText(pane, "public void ShowPage(string navigationKey, string tab)");
         Assert.True(showPage != null && showPage.Contains("ForgetHistory_();", StringComparison.Ordinal), "A page's help keeps the pane's history of another page.");
         Assert.Contains("F(\"Back to {0}\", where)", list, StringComparison.Ordinal);
      }

      /// <summary>
      /// Back collapses when it empties the history; with the keyboard on it, WPF put
      /// the keyboard on the window, outside the pane, and the next Tab started at
      /// the top of the window. The keyboard goes to the search box instead.
      /// </summary>
      [Fact]
      public void Back_LeavesTheKeyboardInThePane_WhenItGoes()
      {
         string back = Member("HelpPane.List.cs", "private void Back_()");
         int noted = back.IndexOf("bool hadKeyboard = backButton_ != null && backButton_.IsKeyboardFocusWithin;", StringComparison.Ordinal);
         int popped = back.IndexOf("back_.Pop()", StringComparison.Ordinal);
         int updated = back.IndexOf("UpdateBack_();", StringComparison.Ordinal);
         int handed = back.IndexOf("if (hadKeyboard && backButton_.Visibility != Visibility.Visible)", StringComparison.Ordinal);
         int focused = back.IndexOf("search_.Focus();", StringComparison.Ordinal);
         Assert.True(noted >= 0 && noted < popped, "Back_ does not note the keyboard before it goes back.");
         Assert.True(updated > popped && handed > updated && focused > handed, "Back_ does not hand the keyboard on once Back is collapsed.");
      }

      /// <summary>
      /// Enter in the list's search opens the first document only while the search
      /// narrows the list: in an empty box, or one of one character, it opened the
      /// first document of the whole list, Profiles under Set up this server.
      /// </summary>
      [Fact]
      public void EnterInTheListsSearch_OpensNothing_BelowTheListsThreshold()
      {
         string enter = Member("HelpPane.List.cs", "private void SearchEnter_()");
         int guard = enter.IndexOf("if (term.Length < HelpDocumentList.ShortestTerm)", StringComparison.Ordinal);
         int open = enter.IndexOf("GoTo_(() => ShowDocument(first, null));", StringComparison.Ordinal);
         Assert.True(guard >= 0 && guard < open, "Enter opens a document with fewer characters than narrow the list.");
         Assert.Contains("foreach (HelpDocumentSection section in Sections_(term))", enter, StringComparison.Ordinal);

         // The threshold is the list's own: below it nothing is narrowed.
         Assert.Equal(2, HelpDocumentList.ShortestTerm);
         Assert.Equal(HelpPages.Names.Count, HelpDocumentList.Sections(HelpPages.Names, HelpPages.TitleOf, HelpPages.Read, new string('D', HelpDocumentList.ShortestTerm - 1)).Sum(s => s.Names.Count));
         Assert.True(HelpDocumentList.Sections(HelpPages.Names, HelpPages.TitleOf, HelpPages.Read, "DNSSEC").Sum(s => s.Names.Count) < HelpPages.Names.Count);
      }

      /// <summary>
      /// What the list's search found is said under the box and to a screen reader:
      /// the list was rebuilt on every key, "No document contains ..." a paragraph in
      /// the viewer, and the number of documents found said nowhere, where the other
      /// lists' searches say their result through Announce.
      /// </summary>
      [Fact]
      public void TheListsSearch_SaysWhatItFound_ToAScreenReaderToo()
      {
         string list = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "HelpPane.List.cs"));
         string pane = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "HelpPane.cs"));

         Assert.Contains("ShowFindStatus_(ListResult_(search_.Text.Trim()));", Member("HelpPane.List.cs", "private void FilterList_()"), StringComparison.Ordinal);
         string result = Member("HelpPane.List.cs", "private static string ListResult_(string term)");
         Assert.Contains("if (term.Length < HelpDocumentList.ShortestTerm)", result, StringComparison.Ordinal);
         Assert.Contains("F(\"No document contains \\\"{0}\\\".\", term)", result, StringComparison.Ordinal);
         Assert.Contains("PluralF(found, \"{0} document contains \\\"{1}\\\".\", \"{0} documents contain \\\"{1}\\\".\", found, term)", result, StringComparison.Ordinal);

         // The one line under the box, t33b's findStatus_, says the list's result as well.
         string show = Member("HelpPane.cs", "private void ShowFindStatus_(string text)");
         Assert.Contains("Announce.Request(findStatus_);", show, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetLiveSetting(findStatus_, AutomationLiveSetting.Polite);", pane, StringComparison.Ordinal);
         Assert.Contains("searchBlock.Children.Add(findStatus_);", pane, StringComparison.Ordinal);
         Assert.DoesNotContain("listStatus_", list, StringComparison.Ordinal);

         // Said once, under the box, and gone with the list.
         Assert.DoesNotContain("No document contains", Member("HelpPane.List.cs", "private void AddDocumentSections_(FlowDocument document)"), StringComparison.Ordinal);
         Assert.Contains("Find_(false);", Member("HelpPane.List.cs", "private void SearchChanged_()"), StringComparison.Ordinal);

         // Both forms are in every catalogue, and the few form where a language has one.
         var english = XDocument.Load(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"))).Root
            .Elements("data").Select(d => (string)d.Attribute("name")).ToHashSet(StringComparer.Ordinal);
         Assert.Contains("{0} document contains \"{1}\".", english);
         Assert.Contains("{0} documents contain \"{1}\".", english);
      }

      private static string Member(string file, string signature)
      {
         string member = WithheldRowTests.MemberText(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file)), signature);
         Assert.True(member != null, file + ": " + signature + " is gone.");
         return member;
      }

      [Fact]
      public void TheDeadAccountRulesSpec_IsGone()
      {
         Assert.DoesNotContain("AccountRules(", ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs")), StringComparison.Ordinal);
         var keys = XDocument.Load(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"))).Root
            .Elements("data").Select(d => (string)d.Attribute("name")).ToHashSet(StringComparer.Ordinal);
         Assert.DoesNotContain("Per-account rules. You can add, rename, enable/disable and delete rules here; matching criteria and actions are managed on the server.", keys);
      }
   }
}
