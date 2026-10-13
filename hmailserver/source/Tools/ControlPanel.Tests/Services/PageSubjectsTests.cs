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
   /// per-account sending limits and the script timeout on Server limits. Delivery of
   /// e-mail's Rules tab, one sidebar row from the Rules page, is a Rule limits card
   /// under the rules, and the PROXY protocol and XCLIENT, a card of Server limits, a
   /// card under the incoming relays.
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
      [InlineData("relays", "PROXY protocol")]
      [InlineData("relays", "XCLIENT")]
      [InlineData("hardening", "Sending limits per account")]
      [InlineData("hardening", "Script timeout")]
      [InlineData("rules", "Rule loop limit")]
      [InlineData("rules", "Account-number pattern")]
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
      [InlineData("put a proxy in front of the server", "relays")]
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
      [InlineData("hardening", "PROXY protocol")]
      [InlineData("hardening", "XCLIENT")]
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
      [InlineData("relays", "PROXY protocol")]
      [InlineData("relays", "XCLIENT")]
      [InlineData("hardening", "event-script timeout")]
      public void ThePagesDescriptionNamesWhatItHolds(string key, string words)
      {
         Assert.Contains(words, NavigationMap.Find(key).Purpose, StringComparison.Ordinal);
      }

      /// <summary>
      /// The rules' limits are a card on the Rules page, under the rules: Delivery of
      /// e-mail has no Rules tab, the card is the settings pages' RuleLimits section
      /// shown on the page (an embedded section, its Save named in the page's scope and
      /// found by Ctrl+K), and the page asks it whether there is an unsaved change, saves
      /// it, shows its setting for the palette and leaves it out of the account dialog.
      /// </summary>
      [Fact]
      public void TheRuleLimitsAreACardOnTheRulesPage()
      {
         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string delivery = WithheldRowTests.MemberText(server, "private void BuildDelivery()");
         string limits = WithheldRowTests.MemberText(server, "private void BuildRuleLimits()");
         Assert.True(delivery != null && limits != null, "A page's builder is gone.");
         Assert.DoesNotContain("Tab(L(\"Rules\"))", delivery, StringComparison.Ordinal);
         Assert.DoesNotContain("RuleLoopLimit", delivery, StringComparison.Ordinal);
         Assert.Contains("Path = \"RuleLoopLimit\"", limits, StringComparison.Ordinal);
         Assert.Contains("Path = \"DlpAccountNumberPattern\"", limits, StringComparison.Ordinal);
         Assert.Contains("Tab(L(\"Rule limits\")).Cards.Add(limits);", limits, StringComparison.Ordinal);
         Assert.Contains("case Section.RuleLimits: BuildRuleLimits(); break;", server, StringComparison.Ordinal);

         string rules = ReadControlPanelFile(Path.Join("Views", "RulesView.xaml.cs"));
         Assert.Contains("public partial class RulesView : UserControl, IPageLifecycle, IRefreshablePage, IRevealSetting", rules, StringComparison.Ordinal);
         Assert.Contains("new ServerSettingsView(ServerSettingsView.Section.RuleLimits, embedded: true) { SaveCaption = L(\"Sa_ve rule limits\") }", rules, StringComparison.Ordinal);
         Assert.Contains("public bool HasUnsavedChanges => limits_?.HasUnsavedChanges == true;", rules, StringComparison.Ordinal);
         Assert.Contains("public bool SaveChanges() => limits_ == null || limits_.SaveChanges();", rules, StringComparison.Ordinal);
         Assert.Contains("public bool RevealSetting(string key, string label) => limits_ != null && limits_.RevealSetting(key, label);", rules, StringComparison.Ordinal);
         Assert.Contains("limits_?.RefreshUnlessEdited();", rules, StringComparison.Ordinal);
         string configure = WithheldRowTests.MemberText(rules, "public void ConfigureForRules(Func<dynamic> rulesProvider, bool serverLevel, bool embedded)");
         Assert.Contains("LimitsHost.Visibility = Visibility.Collapsed;", configure, StringComparison.Ordinal);
         Assert.Contains("<ContentControl x:Name=\"LimitsHost\"", ReadControlPanelFile(Path.Join("Views", "RulesView.xaml")), StringComparison.Ordinal);

         Assert.Contains("rules", NavigationMap.Find("delivery").SeeAlso);
         Assert.Contains("delivery", NavigationMap.Find("rules").SeeAlso);
      }

      /// <summary>
      /// The PROXY protocol and XCLIENT are a card under the incoming relays, which are
      /// what they are about: the settings pages' FrontEndProxies section shown on the
      /// page with its own Save, and Server limits &amp; expert settings says where they
      /// went among the settings that used to be on it.
      /// </summary>
      [Fact]
      public void TheFrontEndProxiesAreACardOnTheIncomingRelaysPage()
      {
         string server = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string proxies = WithheldRowTests.MemberText(server, "private void BuildFrontEndProxies()");
         Assert.True(proxies != null, "ServerSettingsView.BuildFrontEndProxies is gone.");
         foreach (string key in new[] { "SMTPProxyProtocolEnabled", "SMTPProxyProtocolTrustedIPs", "SMTPXClientEnabled", "SMTPXClientTrustedIPs" })
            Assert.Contains("Path = \"" + key + "\"", proxies, StringComparison.Ordinal);
         Assert.Contains("case Section.FrontEndProxies: BuildFrontEndProxies(); break;", server, StringComparison.Ordinal);

         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.DoesNotContain("Key = \"SMTPProxyProtocolEnabled\"", feature, StringComparison.Ordinal);
         Assert.Contains("new ElsewhereSetting(\"relays\",", feature, StringComparison.Ordinal);

         string relays = ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         Assert.Contains("public class IncomingRelaysView : UserControl, IPageLifecycle, IRefreshablePage, IRevealSetting", relays, StringComparison.Ordinal);
         Assert.Contains("new ServerSettingsView(ServerSettingsView.Section.FrontEndProxies, embedded: true)", relays, StringComparison.Ordinal);
         Assert.Contains("SaveCaption = L(\"Save pro_xy settings\")", relays, StringComparison.Ordinal);
         Assert.Contains("public bool HasUnsavedChanges => proxies_.HasUnsavedChanges;", relays, StringComparison.Ordinal);
         Assert.Contains("proxies_.RefreshUnlessEdited();", relays, StringComparison.Ordinal);
         Assert.Contains("parts.Children.Add(proxies_);", relays, StringComparison.Ordinal);
      }

      /// <summary>
      /// A section shown on another page draws no frame of its own: no header, no
      /// padding, no gap and no width of its own, no Reload, no scroll viewer that would
      /// take the mouse wheel from the page's; and its Save, with the pill beside it, is
      /// its last card's own, in the card's footer - on a page whose cards answer their
      /// own access keys, the key is then the card's - and Ctrl+S knows it by its
      /// command whatever the host names it.
      /// </summary>
      [Fact]
      public void AnEmbeddedSectionDrawsNoFrameOfItsOwn()
      {
         string embedded = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.Embedded.cs"));
         Assert.Contains("Header.Visibility = Visibility.Collapsed;", embedded, StringComparison.Ordinal);
         Assert.Contains("frame.Margin = new Thickness(0);", embedded, StringComparison.Ordinal);
         Assert.Contains("== \"settings-reload\"", embedded, StringComparison.Ordinal);
         Assert.Contains("PageCommands.SetCommand(SaveButton, PageCommand.Save);", embedded, StringComparison.Ordinal);
         Assert.Contains("footer.Children.Add(HeaderActions);", embedded, StringComparison.Ordinal);
         Assert.Contains("SettingsTabs.Padding = new Thickness(0);", embedded, StringComparison.Ordinal);
         Assert.DoesNotContain("frame.Children.Add(footer);", embedded, StringComparison.Ordinal);

         string place = WithheldRowTests.MemberText(embedded, "private void PlaceEmbeddedFooter_()");
         Assert.True(place != null, "ServerSettingsView.PlaceEmbeddedFooter_ is gone.");
         Assert.Contains(".SelectMany(panel => panel.Children.OfType<Scaffold.Card>())", place, StringComparison.Ordinal);
         Assert.Contains(".LastOrDefault();", place, StringComparison.Ordinal);
         int leave = place.IndexOf("embeddedFooterCard_.Footer = null;", StringComparison.Ordinal);
         int enter = place.IndexOf("last.Footer = embeddedFooter_;", StringComparison.Ordinal);
         Assert.True(leave > 0 && enter > leave, "The footer does not leave the card the drawing replaced before it goes into the new one.");

         string build = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs")), "private void BuildUi()");
         Assert.Matches(new System.Text.RegularExpressions.Regex(@"object content = embedded_\s*\?\s*panel\s*:\s*new ScrollViewer"), build);
         Assert.Contains("MaxWidth = embedded_ ? double.PositiveInfinity : DesignTokens.FormWidth,", build, StringComparison.Ordinal);
         Assert.Contains("Margin = embedded_ ? new Thickness(0) :", build, StringComparison.Ordinal);
         int placed = build.IndexOf("PlaceEmbeddedFooter_();", StringComparison.Ordinal);
         Assert.True(placed > build.IndexOf("SettingsTabs.Items.Add(", StringComparison.Ordinal), "The footer is placed before the cards are drawn.");
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
