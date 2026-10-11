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
   /// REST API keys is under Sign-in & access, directly after Administrative access
   /// (review finding F152): its keys are credentials that are not the
   /// administrator password, which is the group's subject, where Monitoring &
   /// troubleshooting - "Is mail flowing, and if it is not, where has it stopped?" -
   /// held it as its last row. The move keeps the page's key, so the palette's
   /// entries, Related links, tours and the nav-apikeys automation id still arrive,
   /// and adds no group: the Compliance and Virus scanning groups the review also
   /// proposed each add a rail glyph, and wait on the owner's decision (6 of 9
   /// October 2026). API & monitoring stays Monitoring's last row, its endpoints
   /// being what a monitoring system reads.
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
      public void MonitoringEndsWithApiAndMonitoringAndHoldsNoKeys()
      {
         NavNode monitoring = NavigationMap.Groups.Single(g => g.Title == "Monitoring & troubleshooting");
         var keys = monitoring.Children.Select(c => c.Key).ToList();

         Assert.DoesNotContain("apikeys", keys);
         Assert.Equal("api", keys[^1]);
      }

      [Fact]
      public void TheMoveKeepsThePagesKeyAndItsRelatedLinksAndAddsNoGroup()
      {
         NavNode page = NavigationMap.Find("apikeys");
         Assert.NotNull(page);
         Assert.Equal("REST API keys", page.Title);
         // A key's role is one of the Administrator roles page's (t34a, review finding F089).
         Assert.Equal(new[] { "api", "adminaccess", "roles" }, page.SeeAlso.ToArray());
         Assert.Contains("apikeys", NavigationMap.Find("api").SeeAlso);

         // The groups, in the sidebar's order: none added until the owner decides.
         string[] groups = NavigationMap.Groups.Select(g => g.Title).ToArray();
         Assert.DoesNotContain("Compliance", groups);
         Assert.DoesNotContain("Virus scanning", groups);
      }
   }
}
