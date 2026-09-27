// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Welcome page's starting points: every one lands on a page that exists,
   /// the list is a starting page rather than a directory, and the route the
   /// roadmap asked for - into the stall-diagnosis path - is on it.
   /// </summary>
   public class WelcomeIntentsTests
   {
      [Fact]
      public void EveryStartingPointLandsOnAPageTheNavigationKnows()
      {
         var known = new HashSet<string>(NavigationMap.Pages.Select(p => p.Key), StringComparer.OrdinalIgnoreCase);
         List<string> missing = WelcomeIntents.Entries.Where(i => !known.Contains(i.Page)).Select(i => i.Heading + " -> " + i.Page).ToList();
         Assert.True(missing.Count == 0, "These starting points go nowhere: " + string.Join(", ", missing));
      }

      [Fact]
      public void TheStallDiagnosisPathIsTheFirstStartingPoint()
      {
         // The reason the row existed: the information was in a document nothing
         // in the product pointed at. It is the first troubleshooting path because
         // it is the commonest reason the application gets opened in anger; since
         // the navigation was ordered for a new administrator (27 September 2026)
         // it follows only "Set up this server", the setup checklist, which is the
         // reason a new installation is opened at all.
         Assert.Equal("externalsetup", WelcomeIntents.Entries[0].Page);
         Assert.Equal("stalledmail", WelcomeIntents.Entries[1].Page);
         Assert.Contains("stalledmail", NavigationMap.Pages.Select(p => p.Key));
      }

      [Fact]
      public void ItIsAStartingPageNotADirectory()
      {
         Assert.InRange(WelcomeIntents.Entries.Count, 8, 16);
         Assert.Equal(WelcomeIntents.Entries.Count, WelcomeIntents.Entries.Select(i => i.Heading).Distinct(StringComparer.OrdinalIgnoreCase).Count());
         Assert.All(WelcomeIntents.Entries, i =>
         {
            Assert.False(string.IsNullOrWhiteSpace(i.Heading));
            Assert.False(string.IsNullOrWhiteSpace(i.Blurb));
            Assert.DoesNotContain(i.Heading, NavigationMap.Pages.Select(p => p.Title));   // a task, not a page name
         });
      }
   }
}
