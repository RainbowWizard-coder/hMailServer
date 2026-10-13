// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sidebar's groups as review findings F152 and F153 left them, with the
   /// owner's decision of 12 October 2026 on the two new groups. REST API keys is
   /// under Sign-in &amp; access, directly after Administrative access: its keys are
   /// credentials that are not the administrator password, which is the group's
   /// subject, where Monitoring &amp; troubleshooting - "Is mail flowing, and if it is
   /// not, where has it stopped?" - held it as its last row. The audit trail,
   /// journaling and eDiscovery are Compliance, after Monitoring &amp; troubleshooting,
   /// and the three virus pages Virus scanning, after the spam group, which is Spam
   /// filtering now and holds spam alone. Every page keeps its key, so the palette's
   /// entries, Related links, tours and the nav-&lt;key&gt; automation ids still arrive.
   /// </summary>
   public class SidebarStructureTests
   {
      [Fact]
      public void RestApiKeysIsUnderSignInAndAccessAfterAdministrativeAccess()
      {
         Assert.Equal("Sign-in & access", NavigationMap.GroupOf("apikeys"));
         Assert.Equal("Sign-in & access > REST API keys", NavigationMap.LocationOf("apikeys"));

         NavNode signIn = NavigationMap.Groups.Single(g => g.Title == "Sign-in & access");
         var keys = signIn.Children.Select(c => c.Key).ToList();
         Assert.Equal(keys.IndexOf("adminaccess") + 1, keys.IndexOf("apikeys"));
         Assert.Equal(7, keys.Count);
      }

      [Fact]
      public void MonitoringEndsWithApiAndMonitoringAndHoldsNeitherKeysNorTheComplianceThree()
      {
         NavNode monitoring = NavigationMap.Groups.Single(g => g.Title == "Monitoring & troubleshooting");
         var keys = monitoring.Children.Select(c => c.Key).ToList();

         Assert.DoesNotContain("apikeys", keys);
         foreach (string key in new[] { "audit", "journaling", "discovery" })
            Assert.DoesNotContain(key, keys);
         Assert.Equal("api", keys[^1]);
      }

      [Fact]
      public void TheMoveKeepsThePagesKeyAndItsRelatedLinks()
      {
         NavNode page = NavigationMap.Find("apikeys");
         Assert.NotNull(page);
         Assert.Equal("REST API keys", page.Title);
         // A key's role is one of the Administrator roles page's (t34a, review finding F089).
         Assert.Equal(new[] { "api", "adminaccess", "roles" }, page.SeeAlso.ToArray());
         Assert.Contains("apikeys", NavigationMap.Find("api").SeeAlso);
      }

      /// <summary>
      /// Compliance follows Monitoring &amp; troubleshooting and holds the audit trail,
      /// journaling and eDiscovery, in that order, under the gavel; Virus scanning
      /// follows the spam group and holds the three virus pages, the overview first,
      /// under the bug (F152, F153).
      /// </summary>
      [Fact]
      public void ComplianceAndVirusScanningAreGroupsOfTheirOwnInTheirPlaces()
      {
         string[] groups = NavigationMap.Groups.Select(g => g.Title).ToArray();
         Assert.Equal(Array.IndexOf(groups, "Monitoring & troubleshooting") + 1, Array.IndexOf(groups, "Compliance"));
         Assert.Equal(Array.IndexOf(groups, "Spam filtering") + 1, Array.IndexOf(groups, "Virus scanning"));
         Assert.DoesNotContain("Spam & virus filtering", groups);

         NavNode compliance = NavigationMap.Groups.Single(g => g.Title == "Compliance");
         Assert.Equal("Gavel24", compliance.Icon);
         Assert.Equal(new[] { "audit", "journaling", "discovery" }, compliance.Children.Select(c => c.Key).ToArray());
         Assert.Equal("Compliance", NavigationMap.GroupOf("discovery"));

         NavNode virus = NavigationMap.Groups.Single(g => g.Title == "Virus scanning");
         Assert.Equal("Bug24", virus.Icon);
         Assert.Equal(new[] { "virusoverview", "antivirus", "blockedattachments" }, virus.Children.Select(c => c.Key).ToArray());
         Assert.Equal("Virus scanning", NavigationMap.GroupOf("virusoverview"));
      }

      /// <summary>
      /// The spam group says spam alone - its title, its purpose and its aliases - and
      /// its old heading is an alias of both groups, so that a note naming it reaches
      /// whichever half its reader wanted.
      /// </summary>
      [Fact]
      public void TheSpamGroupSaysSpamAloneAndTheOldHeadingIsAnAliasOfBothGroups()
      {
         NavNode spam = NavigationMap.Groups.Single(g => g.Title == "Spam filtering");
         Assert.DoesNotContain("virus", spam.Purpose, StringComparison.OrdinalIgnoreCase);
         Assert.DoesNotContain(spam.Aliases, alias => alias.Contains("virus", StringComparison.OrdinalIgnoreCase) && alias != "Spam & virus filtering");
         Assert.DoesNotContain(spam.Children, page => page.Key is "virusoverview" or "antivirus" or "blockedattachments");

         Assert.Contains("Spam & virus filtering", spam.Aliases);
         Assert.Contains("Spam & virus filtering", NavigationMap.Groups.Single(g => g.Title == "Virus scanning").Aliases);
      }

      /// <summary>
      /// The built-in auditor holds view_discovery and not administer_server: its
      /// sidebar is Compliance with eDiscovery alone, where it was Monitoring &amp;
      /// troubleshooting with eDiscovery as its only row (F152).
      /// </summary>
      [Fact]
      public void TheAuditorSeesComplianceWithEDiscovery()
      {
         static bool Auditor(string needs) => string.IsNullOrEmpty(needs) || needs == "view_discovery";

         var groups = NavigationMap.VisibleRoots(Auditor).Where(node => !node.IsPage).ToList();
         Assert.Single(groups);
         Assert.Equal("Compliance", groups[0].Title);
         Assert.Equal(new[] { "discovery" }, groups[0].Children.Select(c => c.Key).ToArray());
      }

      private static readonly string[] AllowAndBlockLists =
         { "spamwhitelist", "blockedsenders", "senderlist", "greylistwhitelist", "greylistbypass", "domainpolicylist" };

      /// <summary>
      /// The spam group's six allow and block lists - five ways through and three to
      /// refuse - are one run under the sub-heading Lists, after the checks and their
      /// block lists, and no other page of the map has a sub-heading (review finding
      /// F269, the owner's decision of 12 October 2026).
      /// </summary>
      [Fact]
      public void TheAllowAndBlockListsAreOneRunUnderLists()
      {
         NavNode spam = NavigationMap.Groups.Single(g => g.Title == "Spam filtering");
         var keys = spam.Children.Select(c => c.Key).ToList();
         int first = keys.IndexOf("spamwhitelist");
         Assert.Equal(AllowAndBlockLists, keys.Skip(first).Take(AllowAndBlockLists.Length).ToArray());
         Assert.Equal("dnsbl", keys[first - 1]);

         foreach (NavNode page in NavigationMap.Pages)
            Assert.Equal(AllowAndBlockLists.Contains(page.Key) ? "Lists" : null, page.Section);

         var drawn = Enumerable.Range(0, spam.Children.Count).Select(i => NavigationMap.SectionBefore(spam.Children, i)).ToList();
         Assert.Equal("Lists", drawn[first]);
         Assert.Single(drawn, section => section != null);
      }

      /// <summary>
      /// A role that cannot open the run's first page still sees the sub-heading, over
      /// the first list it can open: the sub-heading is decided on the pages as the
      /// credential sees them, not on the map's.
      /// </summary>
      [Fact]
      public void TheSubHeadingStandsOverTheFirstListTheCredentialSees()
      {
         static bool NoExemptions(string needs) => true;
         NavNode spam = NavigationMap.VisibleRoots(NoExemptions).Single(n => n.Title == "Spam filtering");
         var withoutFirst = spam.Children.Where(c => c.Key != "spamwhitelist").ToList();

         int blocked = withoutFirst.FindIndex(c => c.Key == "blockedsenders");
         Assert.Equal("Lists", NavigationMap.SectionBefore(withoutFirst, blocked));
         Assert.Null(NavigationMap.SectionBefore(withoutFirst, blocked + 1));
         Assert.Null(NavigationMap.SectionBefore(withoutFirst, -1));
         Assert.Null(NavigationMap.SectionBefore(withoutFirst, withoutFirst.Count));
      }

      /// <summary>
      /// The sub-heading's row in the tree and in the rail's flyout is a caption that
      /// nothing lands on - not focusable, not hit-testable, no page's key in its Tag -
      /// and a level-3 heading to a screen reader, so that the arrow keys go from the
      /// page above it to the page below and a click on it opens nothing.
      /// </summary>
      [Fact]
      public void TheSubHeadingRowIsACaptionNothingLandsOn()
      {
         string shell = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string tree = WithheldRowTests.MemberText(shell, "private static TreeViewItem BuildNavSubHeading_(NavNode group, string section)");
         Assert.True(tree != null, "MainWindow.BuildNavSubHeading_ is gone.");
         string flyoutSource = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.NavFlyout.cs");
         string flyout = WithheldRowTests.MemberText(flyoutSource, "private static ListBoxItem FlyoutSubHeading_(NavNode group, string section)");
         Assert.True(flyout != null, "MainWindow.FlyoutSubHeading_ is gone.");

         foreach (string row in new[] { tree, flyout })
         {
            Assert.Contains("Focusable = false,", row, StringComparison.Ordinal);
            Assert.Contains("IsHitTestVisible = false", row, StringComparison.Ordinal);
            Assert.Contains("AutomationHeadingLevel.Level3", row, StringComparison.Ordinal);
            Assert.Contains("FontSize = Typography.Caption,", row, StringComparison.Ordinal);
         }

         Assert.Contains("Tag = NavSubHeadingTag,", tree, StringComparison.Ordinal);
         Assert.DoesNotContain("Tag =", flyout, StringComparison.Ordinal);
         Assert.Contains("string section = NavigationMap.SectionBefore(node.Children, i);", shell, StringComparison.Ordinal);
         Assert.Contains("string section = NavigationMap.SectionBefore(pages, i);", flyoutSource, StringComparison.Ordinal);
         Assert.Contains("FirstOrDefault(row => row.Focusable)", flyoutSource, StringComparison.Ordinal);
      }
   }
}
