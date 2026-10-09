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
   /// The Related links follow the session's role (review finding F079). A
   /// domain administrator's Domains page offered Public folders, Groups and
   /// Rules, which the role cannot open and the sidebar hides; pressing one did
   /// nothing, since NavigateTo found no row and said nothing. The links are now
   /// the ones the credential may open, and a page of the map it may not open
   /// is said to be so when something asks for it.
   /// </summary>
   public class RelatedLinksRoleTests
   {
      /// <summary>ServerSession.May for a credential holding these permissions.</summary>
      private static Func<string, bool> Holding(params string[] permissions)
      {
         var held = new HashSet<string>(permissions, StringComparer.OrdinalIgnoreCase);
         return needs => string.IsNullOrEmpty(needs) || held.Contains("administer_server") || held.Contains(needs);
      }

      public static IEnumerable<object[]> Roles() => new[]
      {
         new object[] { "administer_domain" },
         new object[] { "view_configuration" },
         new object[] { "view_discovery" },
         new object[] { "view_configuration,view_discovery" },
         new object[] { "administer_server" }
      };

      /// <summary>Every page a role sees offers Related links only to pages the role sees.</summary>
      [Theory]
      [MemberData(nameof(Roles))]
      public void EveryLinkARoleIsOffered_LeadsToAPageItMayOpen(string permissions)
      {
         Func<string, bool> may = Holding(permissions.Split(','));
         var visible = NavigationMap.VisiblePages(may).Select(p => p.Key).ToHashSet(StringComparer.OrdinalIgnoreCase);

         var broken = new List<string>();
         foreach (string page in visible)
         {
            foreach (NavNode target in NavigationMap.RelatedOf(page, may))
            {
               if (!visible.Contains(target.Key))
                  broken.Add(page + " -> " + target.Key);
            }
         }

         Assert.True(broken.Count == 0, "Related links to pages the role cannot open: " + string.Join(", ", broken));
      }

      /// <summary>
      /// The domain administrator's two pages lead to each other; the links to
      /// the server administrator's pages are gone from them, and stay for a
      /// server administrator and for a credential the server did not describe.
      /// </summary>
      [Fact]
      public void ADomainAdministrator_IsOfferedTheDomainAdministratorsPages()
      {
         Func<string, bool> domainAdministrator = Holding("administer_domain");

         Assert.Equal(new[] { "suppressions" }, NavigationMap.RelatedOf("domains", domainAdministrator).Select(n => n.Key).ToArray());
         Assert.Equal(new[] { "domains" }, NavigationMap.RelatedOf("suppressions", domainAdministrator).Select(n => n.Key).ToArray());

         Assert.Equal(NavigationMap.Find("domains").SeeAlso.ToArray(), NavigationMap.RelatedOf("domains", null).Select(n => n.Key).ToArray());
         Assert.Equal(NavigationMap.Find("domains").SeeAlso.ToArray(), NavigationMap.RelatedOf("domains", Holding("administer_server")).Select(n => n.Key).ToArray());
         Assert.Empty(NavigationMap.RelatedOf("no-such-page", null));
         Assert.Empty(NavigationMap.RelatedOf(null, null));
      }

      /// <summary>
      /// The window draws the links the session may open, and a jump to a page of
      /// the map that the role's tree leaves out says so in a toast, where it did
      /// nothing at all: a link, a palette row, a Welcome tile or a tour step.
      /// </summary>
      [Fact]
      public void TheWindow_DrawsTheRolesLinksAndSaysWhatItCannotOpen()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string breadcrumb = WithheldRowTests.MemberText(shell, "private void UpdateBreadcrumb(string key)");
         Assert.True(breadcrumb != null, "MainWindow.UpdateBreadcrumb is gone.");
         Assert.Contains("BuildRelated_(NavigationMap.RelatedOf(key, SessionMayUse_()));", breadcrumb, StringComparison.Ordinal);

         string navigate = WithheldRowTests.MemberText(shell, "private void NavigateTo(string key, bool fromSearch)");
         Assert.True(navigate != null, "MainWindow.NavigateTo is gone.");
         int loop = navigate.IndexOf("foreach (TreeViewItem leaf in AllLeaves(NavTree.Items)", StringComparison.Ordinal);
         int said = navigate.IndexOf("F(\"Your role cannot open {0}.\"", StringComparison.Ordinal);
         Assert.True(loop >= 0 && said > loop, "A jump to a page the role cannot open says nothing again.");

         string related = ReadControlPanelFile("MainWindow.Related.cs");
         Assert.Contains("session == null || session.Permissions == null ? null : session.May", related, StringComparison.Ordinal);
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
