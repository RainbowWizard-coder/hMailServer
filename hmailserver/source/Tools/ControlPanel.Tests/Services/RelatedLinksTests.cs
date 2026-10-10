// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The breadcrumb's Related links (review finding F271). The bar drew the
   /// first three of a page's list whatever the room, so the pairs the map lists
   /// on purpose further down were dropped - the queue's stalled mail, LDAP's
   /// directory synchronisation, the greylisting exemptions' bypass list, SSL/TLS's
   /// Auto-ban, the block lists' IP ranges - and twelve pages were the drawn link
   /// of none. Now as many are drawn as fit, the rest in a drop-down; these hold
   /// the map so that every page is reached by another's links and a pair's two
   /// pages list each other first.
   /// </summary>
   public class RelatedLinksTests
   {
      /// <summary>
      /// Every page is the Related link of at least one other page: drawn beside
      /// the breadcrumb, or in its drop-down where it does not fit. The two
      /// top-level pages are the exceptions: Welcome is the rail's first glyph,
      /// About the administrator menu's last entry.
      /// </summary>
      [Fact]
      public void EveryPage_IsTheRelatedLinkOfAnotherPage()
      {
         var reached = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
         foreach (NavNode page in NavigationMap.Pages)
         {
            foreach (string target in page.SeeAlso)
            {
               if (!string.Equals(target, page.Key, StringComparison.OrdinalIgnoreCase))
                  reached.Add(target);
            }
         }

         List<string> unreached = NavigationMap.Pages
            .Where(p => p.Parent != null && !reached.Contains(p.Key))
            .Select(p => p.Key)
            .ToList();

         Assert.True(unreached.Count == 0, "Pages no other page's Related links lead to: " + string.Join(", ", unreached));
      }

      /// <summary>
      /// The pairs the map's comments put side by side on purpose, and the ones
      /// the review found dropped by the cut: each lists the other among its
      /// first three. A page's partners come first, so they are the links drawn
      /// first wherever there is room - beside the trail of every page at 1280
      /// in English - and in a narrow window, where one link or none fits, the
      /// first that its drop-down, More (n) or Related pages (n), lists.
      /// </summary>
      [Theory]
      [InlineData("tls", "autoban")]
      [InlineData("greylistwhitelist", "greylistbypass")]
      [InlineData("blockedsenders", "senderlist")]
      [InlineData("routes", "remotedomains")]
      [InlineData("ldap", "directorysync")]
      [InlineData("queue", "stalledmail")]
      [InlineData("queue", "messagetrace")]
      [InlineData("messagetrace", "suppressions")]
      [InlineData("audit", "journaling")]
      [InlineData("audit", "discovery")]
      [InlineData("externalsetup", "dnsrecords")]
      [InlineData("api", "apikeys")]
      [InlineData("advanced", "scripts")]
      [InlineData("antispam", "spamoverview")]
      [InlineData("antispam", "quarantine")]
      [InlineData("antivirus", "virusoverview")]
      [InlineData("logging", "logs")]
      [InlineData("dnsbl", "ipranges")]
      [InlineData("adminaccess", "roles")]
      public void APair_ListsEachOtherFirst(string one, string other)
      {
         AssertAmongTheFirst(one, other, 3);
         AssertAmongTheFirst(other, one, 3);
      }

      /// <summary>The three lists the review names, which begin with their partners.</summary>
      [Theory]
      [InlineData("antispam", "spamoverview|quarantine|dnsbl")]
      [InlineData("logging", "logs|messagetrace|audit")]
      [InlineData("tls", "tlsoverview|security|autoban")]
      public void AList_BeginsWithItsPartners(string key, string first)
      {
         Assert.Equal(first.Split('|'), NavigationMap.Find(key).SeeAlso.Take(3).ToArray());
      }

      [Fact]
      public void Everything_IsDrawnWhenItFits()
      {
         Assert.Equal(4, RelatedLinksFit.Count(new[] { 100.0, 120, 90, 110 }, 420, 70));
         Assert.Equal(1, RelatedLinksFit.Count(new[] { 100.0 }, 100, 70));
      }

      /// <summary>As many as fit beside the drop-down, from the first; none when not even the first does.</summary>
      [Fact]
      public void TheRest_GoBehindTheDropDown()
      {
         Assert.Equal(2, RelatedLinksFit.Count(new[] { 100.0, 120, 90, 110 }, 300, 70));
         Assert.Equal(1, RelatedLinksFit.Count(new[] { 100.0, 120, 90, 110 }, 190, 70));
         Assert.Equal(0, RelatedLinksFit.Count(new[] { 100.0, 120 }, 150, 70));
         Assert.Equal(0, RelatedLinksFit.Count(Array.Empty<double>(), 1000, 70));
         Assert.Equal(0, RelatedLinksFit.Count(null, 1000, 70));
      }

      /// <summary>
      /// The window builds every link and hides those that do not fit, again as
      /// its width changes, with the drop-down after the links drawn - More (n),
      /// or Related pages (n) where none fits; the cut at three is gone.
      /// </summary>
      [Fact]
      public void TheWindow_DrawsAsManyAsFit()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string breadcrumb = WithheldRowTests.MemberText(shell, "private void UpdateBreadcrumb(string key)");
         Assert.True(breadcrumb != null, "MainWindow.UpdateBreadcrumb is gone.");
         Assert.Contains("BuildRelated_(", breadcrumb, StringComparison.Ordinal);
         Assert.DoesNotContain(">= 3", breadcrumb, StringComparison.Ordinal);

         string related = ReadControlPanelFile("MainWindow.Related.cs");
         Assert.Contains("RelatedLinksFit.Count(widths, available, moreWidth)", related, StringComparison.Ordinal);
         Assert.Contains("e.WidthChanged", related, StringComparison.Ordinal);
         Assert.Contains("F(\"More ({0})\", hidden)", related, StringComparison.Ordinal);
         Assert.Contains("F(\"Related pages ({0})\", hidden)", related, StringComparison.Ordinal);
         Assert.Contains("item.Click += (s, e) => NavigateTo(key);", related, StringComparison.Ordinal);
      }

      private static void AssertAmongTheFirst(string page, string target, int count)
      {
         NavNode node = NavigationMap.Find(page);
         Assert.True(node != null, page + " is not a page.");
         List<string> first = node.SeeAlso.Take(count).ToList();
         Assert.True(first.Contains(target, StringComparer.OrdinalIgnoreCase),
            page + "'s first " + count + " Related links (" + string.Join(", ", first) + ") leave out its partner " + target + ".");
      }

      private static string ReadControlPanelFile(string relativePath)
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Join(at.FullName, "ControlPanel", relativePath);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(at.FullName, "ControlPanel", "Views")))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("the Control Panel's " + relativePath + " is not above " + AppContext.BaseDirectory);
      }
   }
}
