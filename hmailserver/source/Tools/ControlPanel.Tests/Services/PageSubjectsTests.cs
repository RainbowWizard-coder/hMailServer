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
   /// A subject filed on a page whose name does not describe it is found from that
   /// page's description and aliases, and the pages an administrator would try link
   /// to it (review finding F273). Logging holds the alerts, outbound webhooks and
   /// SNMP; API &amp; monitoring the Windows Event Log, the ManageSieve listener and the
   /// graceful shutdown, where its Webhooks alias outranked the real webhook settings
   /// and the Sieve alias sent the reader to the Rules page; Auto-ban the countries
   /// and the compromised accounts; Advanced the default domain, IPv6, the disk
   /// space, the migrations and the PST import; Server limits the PROXY protocol,
   /// XCLIENT, the per-account sending limits and the script timeout; and Delivery
   /// of e-mail a Rules tab one sidebar row from the Rules page, neither linking the
   /// other.
   ///
   /// The cards stay where they are: moving them to the pages named for them was
   /// the owner's to decide with the new groups (the decision of 9 October 2026 on
   /// moves made the Updates page and the Authentication card order now).
   /// </summary>
   [Collection("Loc")]
   public class PageSubjectsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData("logging", "Webhooks")]
      [InlineData("logging", "Outbound webhooks")]
      [InlineData("logging", "Alerts")]
      [InlineData("logging", "SNMP")]
      [InlineData("api", "Windows Event Log")]
      [InlineData("api", "ManageSieve")]
      [InlineData("api", "Sieve")]
      [InlineData("api", "Graceful shutdown")]
      [InlineData("autoban", "Countries")]
      [InlineData("autoban", "GeoIP")]
      [InlineData("autoban", "MaxMind")]
      [InlineData("autoban", "Compromised accounts")]
      [InlineData("advanced", "Default domain")]
      [InlineData("advanced", "Prefer IPv6")]
      [InlineData("advanced", "Disk space")]
      [InlineData("advanced", "Migrations")]
      [InlineData("advanced", "PST import")]
      [InlineData("hardening", "PROXY protocol")]
      [InlineData("hardening", "XCLIENT")]
      [InlineData("hardening", "Sending limits per account")]
      [InlineData("hardening", "Script timeout")]
      [InlineData("delivery", "Rule loop limit")]
      public void ASubjectAPageHoldsFindsThatPageFirst(string key, string subject)
      {
         NavNode page = NavigationMap.Find(key);
         Assert.Contains(subject, page.Aliases);

         List<PaletteRow> pages = PaletteSearch.Query(subject, null, null).Where(r => r.Kind == PaletteRowKind.Page).ToList();
         Assert.True(pages.Count > 0, "'" + subject + "' finds no page.");
         Assert.Equal(key, pages[0].Page);
      }

      /// <summary>The tasks an administrator types for those subjects open the pages that hold them.</summary>
      [Theory]
      [InlineData("block mail from a country", "autoban")]
      [InlineData("import a pst file", "advanced")]
      [InlineData("send an alert to another system", "logging")]
      [InlineData("put a proxy in front of the server", "hardening")]
      public void TheTaskForASubjectOpensThePageThatHoldsIt(string phrase, string key)
      {
         Assert.Contains(IntentIndex.Entries, intent => intent.Phrase == phrase && intent.Page == key);
         PaletteRow first = PaletteSearch.Query(phrase, null, null).First(r => r.Kind == PaletteRowKind.Task);
         Assert.Equal(key, first.Page);
      }

      /// <summary>The aliases that sent the reader to the wrong page are gone from it.</summary>
      [Theory]
      [InlineData("api", "Webhooks")]
      [InlineData("rules", "Sieve")]
      public void AnAliasForASubjectThePageDoesNotHoldIsGone(string key, string alias)
      {
         Assert.DoesNotContain(alias, NavigationMap.Find(key).Aliases);
      }

      /// <summary>The description - the sidebar's tool tip and the palette's line - names what the page holds.</summary>
      [Theory]
      [InlineData("logging", "webhooks")]
      [InlineData("logging", "SNMP")]
      [InlineData("logging", "audit trail")]
      [InlineData("api", "Windows Event Log")]
      [InlineData("api", "ManageSieve")]
      [InlineData("autoban", "countries")]
      [InlineData("autoban", "taken over")]
      [InlineData("advanced", "PST import")]
      [InlineData("advanced", "migrations")]
      [InlineData("advanced", "IPv6")]
      [InlineData("hardening", "PROXY protocol")]
      [InlineData("hardening", "XCLIENT")]
      [InlineData("hardening", "event-script timeout")]
      public void ThePagesDescriptionNamesWhatItHolds(string key, string words)
      {
         Assert.Contains(words, NavigationMap.Find(key).Purpose, StringComparison.Ordinal);
      }

      /// <summary>
      /// Delivery of e-mail's Rules tab and the Rules page link to each other: the
      /// tab says what it limits and opens the Rules page, and the Rules page and
      /// Incoming relays signpost the pages that hold what they do not.
      /// </summary>
      [Fact]
      public void ThePagesASubjectIsLookedForOnLinkToIt()
      {
         // Among the first three: a page's partners come first, as the breadcrumb's
         // Related bar draws them first wherever there is room and lists them first
         // in its drop-down (RelatedLinksFit); Delivery of e-mail's link to Rules was
         // its fourth, when the bar drew three.
         Assert.Contains("delivery", NavigationMap.Find("rules").SeeAlso.Take(3));
         Assert.Contains("rules", NavigationMap.Find("delivery").SeeAlso.Take(3));
         Assert.Contains("hardening", NavigationMap.Find("relays").SeeAlso.Take(3));

         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         int loopLimit = server.IndexOf("Path = \"RuleLoopLimit\"", StringComparison.Ordinal);
         int notice = server.IndexOf("The rules themselves - what each one matches and what it does - are on the Rules page.", StringComparison.Ordinal);
         int opens = server.IndexOf("Page = \"rules\"", StringComparison.Ordinal);
         int tab = server.IndexOf("Tab(L(\"Rules\")).Cards.Add(rules);", StringComparison.Ordinal);
         Assert.True(loopLimit > 0 && notice > loopLimit && opens > notice && tab > opens,
            "Delivery of e-mail's Rules tab does not say where the rules are, with the way there.");
         Assert.Contains("Content = Page == null ? null : NoticeActions.OpenPage(Page, \"settings-open-\" + Page)", server, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relativePath)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relativePath);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relativePath + " could not be found from the test binaries.");
         return null;
      }
   }
}
