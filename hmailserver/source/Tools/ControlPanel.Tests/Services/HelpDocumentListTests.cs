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
         Assert.Contains("allButton_.Click += (s, e) => GoTo_(ShowAll);", pane, StringComparison.Ordinal);
         Assert.Contains("(doc, fragment) => GoTo_(() => ShowDocument(doc, HeadingForFragment_(doc, fragment)))", pane, StringComparison.Ordinal);
         Assert.Contains("link.Click += (s, e) => GoTo_(() => ShowDocument(opened, null));", list, StringComparison.Ordinal);
         string showPage = WithheldRowTests.MemberText(pane, "public void ShowPage(string navigationKey, string tab)");
         Assert.True(showPage != null && showPage.Contains("ForgetHistory_();", StringComparison.Ordinal), "A page's help keeps the pane's history of another page.");
         Assert.Contains("F(\"Back to {0}\", where)", list, StringComparison.Ordinal);
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
