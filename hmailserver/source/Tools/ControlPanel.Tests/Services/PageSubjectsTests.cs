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
   /// A subject is on the page an administrator would try, and is found from that
   /// page's description and aliases (review finding F273, and the owner's decision
   /// of 12 October 2026 to make every move it proposed). The alerts, the outbound
   /// webhooks and the SNMP agent, which were tabs of Logging, are on Monitoring &amp;
   /// alerts, the page that was API &amp; monitoring, where its Webhooks alias had
   /// outranked the real webhook settings; the Windows Event Log is on Logging, and
   /// ManageSieve a tab of Protocols, where the Sieve alias sends the reader. Where a
   /// card stays, the page's description and aliases name it: the graceful shutdown
   /// on Monitoring &amp; alerts; the countries and the compromised accounts on
   /// Auto-ban; the default domain, IPv6 and the disk space on Advanced; the
   /// per-account sending limits and the script timeout on Server limits.
   /// </summary>
   [Collection("Loc")]
   public class PageSubjectsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData("api", "Webhooks")]
      [InlineData("api", "Outbound webhooks")]
      [InlineData("api", "Alerts")]
      [InlineData("api", "SNMP")]
      [InlineData("logging", "Windows Event Log")]
      [InlineData("protocols", "ManageSieve")]
      [InlineData("protocols", "Sieve")]
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
      [InlineData("send an alert to another system", "api")]
      [InlineData("put a proxy in front of the server", "hardening")]
      public void TheTaskForASubjectOpensThePageThatHoldsIt(string phrase, string key)
      {
         Assert.Contains(IntentIndex.Entries, intent => intent.Phrase == phrase && intent.Page == key);
         PaletteRow first = PaletteSearch.Query(phrase, null, null).First(r => r.Kind == PaletteRowKind.Task);
         Assert.Equal(key, first.Page);
      }

      /// <summary>The aliases that sent the reader to the wrong page are gone from it, and from the pages the cards left.</summary>
      [Theory]
      [InlineData("logging", "Webhooks")]
      [InlineData("logging", "SNMP")]
      [InlineData("logging", "Alerts")]
      [InlineData("api", "Windows Event Log")]
      [InlineData("api", "ManageSieve")]
      [InlineData("api", "Sieve")]
      [InlineData("rules", "Sieve")]
      public void AnAliasForASubjectThePageDoesNotHoldIsGone(string key, string alias)
      {
         Assert.DoesNotContain(alias, NavigationMap.Find(key).Aliases);
      }

      /// <summary>The description - the sidebar's tool tip and the palette's line - names what the page holds.</summary>
      [Theory]
      [InlineData("api", "webhooks")]
      [InlineData("api", "SNMP")]
      [InlineData("api", "alerts")]
      [InlineData("logging", "audit trail")]
      [InlineData("logging", "Windows Event Log")]
      [InlineData("protocols", "ManageSieve")]
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

      /// <summary>
      /// The cards themselves are on the pages named for them: Monitoring &amp; alerts
      /// builds the REST API, metrics, alerts and webhooks, SNMP and shutdown tabs; the
      /// Windows Event Log is a card of Logging's Logging tab, beside syslog, and
      /// ManageSieve a tab of Protocols; and neither Logging nor the INI-only view
      /// holds what left it (review finding F273).
      /// </summary>
      [Fact]
      public void TheCardsAreOnThePagesNamedForThem()
      {
         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string monitoring = WithheldRowTests.MemberText(server, "private void BuildMonitoring()");
         string logging = WithheldRowTests.MemberText(server, "private void BuildLogging()");
         string protocols = WithheldRowTests.MemberText(server, "private void BuildProtocols()");
         Assert.True(monitoring != null && logging != null && protocols != null, "A page's builder is gone.");

         Assert.Contains("Header.PageKey = \"api\";", monitoring, StringComparison.Ordinal);
         foreach (string tab in new[] { "REST API", "Metrics", "Alerts & webhooks", "SNMP", "Shutdown" })
            Assert.Contains("Tab(L(\"" + tab + "\")).Cards.Add(", monitoring, StringComparison.Ordinal);
         foreach (string key in new[] { "RestApiPort", "MetricsServerPort", "AlertsEnabled", "WebhooksEnabled", "SnmpEnabled", "ShutdownDrainSeconds" })
            Assert.Contains("Path = \"" + key + "\"", monitoring, StringComparison.Ordinal);

         foreach (string key in new[] { "AlertsEnabled", "WebhooksEnabled", "SnmpEnabled" })
            Assert.DoesNotContain("Path = \"" + key + "\"", logging, StringComparison.Ordinal);
         int syslog = logging.IndexOf("logging.Cards.Add(syslog);", StringComparison.Ordinal);
         int eventLog = logging.IndexOf("logging.Cards.Add(eventLog);", StringComparison.Ordinal);
         Assert.True(syslog > 0 && eventLog > syslog, "The Windows Event Log is not a card of the Logging tab after syslog.");
         Assert.Contains("Path = \"WindowsEventLogEnabled\"", logging, StringComparison.Ordinal);
         Assert.Contains("Path = \"WindowsEventLogLevel\"", logging, StringComparison.Ordinal);

         Assert.Contains("Tab(L(\"ManageSieve\")).Cards.Add(sieve);", protocols, StringComparison.Ordinal);
         Assert.Contains("Path = \"ManageSieveServerPort\"", protocols, StringComparison.Ordinal);
         Assert.Contains("afterBuildUi_ = () => WireManageSieveTlsState_(sievePort, sieveBind, sieveTls);", protocols, StringComparison.Ordinal);

         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.DoesNotContain("case Section.Integration:", feature, StringComparison.Ordinal);
         foreach (string key in new[] { "RestApiPort", "MetricsServerPort", "WindowsEventLogEnabled", "ManageSieveServerPort", "ShutdownDrainSeconds" })
            Assert.DoesNotContain("Key = \"" + key + "\"", feature, StringComparison.Ordinal);

         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         Assert.Contains("pageFactories_[\"api\"] = () => new ServerSettingsView(ServerSettingsView.Section.Monitoring);", shell, StringComparison.Ordinal);
      }

      /// <summary>
      /// What the INI-only view said of the metrics listener and of ManageSieve is said
      /// on their new pages: the five warnings MetricsServer::Start gives cause for, by
      /// their automation ids, and whether ManageSieve will offer STARTTLS; and a secret
      /// of the metrics listener is never shown, a blank box keeps it, a box removes it,
      /// and ticking the removal empties and holds the box, so that a typed value and a
      /// removal are never both asked of one save.
      /// </summary>
      [Fact]
      public void TheMovedCardsKeepTheirWarningsAndTheirSecrets()
      {
         string monitoring = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.Monitoring.cs"));
         string warnings = WithheldRowTests.MemberText(monitoring, "private void WireMetricsWarnings_(MetricsRows rows)");
         Assert.True(warnings != null, "ServerSettingsView.WireMetricsWarnings_ is gone.");
         foreach (string id in new[] { "MetricsBindAddressInvalidWarning", "MetricsNeedsCredentialWarning", "MetricsHalfBasicWarning", "MetricsHalfTlsWarning", "MetricsClearTextCredentialWarning" })
            Assert.Contains("\"" + id + "\"", warnings, StringComparison.Ordinal);
         Assert.Contains("on && bindParsed && !loopback && !tokenSet && !basicSet", warnings, StringComparison.Ordinal);
         Assert.Contains("on && bindParsed && !loopback && (tokenSet || basicSet) && !(certSet && keySet)", warnings, StringComparison.Ordinal);

         string state = WithheldRowTests.MemberText(monitoring, "private (StatusLevel Level, string Text) ManageSieveTlsState_(string bind)");
         Assert.True(state != null, "ServerSettingsView.ManageSieveTlsState_ is gone.");
         Assert.Contains("the TLS certificates page checks the files themselves.", state, StringComparison.Ordinal);
         Assert.Contains("ServerSession.SessionImap, ServerSession.SessionPop3, ServerSession.SessionSmtp", monitoring, StringComparison.Ordinal);

         string secret = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.IniSecret.cs"));
         Assert.Contains("private sealed class IniSecret : ComPassword, IIniSetting", secret, StringComparison.Ordinal);
         Assert.Matches(new System.Text.RegularExpressions.Regex(@"remove_\.Checked \+= \(s, e\) =>\s*\{\s*box_\.Password = """";\s*box_\.IsEnabled = false;"), secret);
         Assert.Contains("return false;   // blank keeps the stored secret", secret, StringComparison.Ordinal);
         Assert.Contains("IniStore.SettingsReachable", secret, StringComparison.Ordinal);

         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string build = WithheldRowTests.MemberText(server, "private void BuildMonitoring()");
         Assert.Contains("var metricsToken = new IniSecret", build, StringComparison.Ordinal);
         Assert.Contains("afterBuildUi_ = () => WireMetricsWarnings_(rows);", build, StringComparison.Ordinal);
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
