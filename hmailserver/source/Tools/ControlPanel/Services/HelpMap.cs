// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Where a page's help is: one of the operator documents
   /// (hmailserver/docs/*.md, embedded in this program as Help.&lt;Name&gt;.md -
   /// see <see cref="HelpPages"/>) and, optionally, one of its level-one or
   /// level-two headings, which the pane scrolls to.
   /// </summary>
   public sealed class HelpTarget
   {
      public HelpTarget(string doc, string heading = null)
      {
         Doc = doc;
         Heading = heading;
      }

      /// <summary>
      /// A page no document is about: the pane shows the page's purpose, says
      /// that it has no guide yet and lists every document, rather than open a
      /// document about something else (review finding F270).
      /// </summary>
      public static readonly HelpTarget NoGuide = new HelpTarget(null);

      /// <summary>Whether a document is about the page; false for <see cref="NoGuide"/>.</summary>
      public bool IsGuide => Doc != null;

      /// <summary>The document's file name, extension included: "Backup.md"; null for <see cref="NoGuide"/>.</summary>
      public string Doc { get; }

      /// <summary>
      /// The document's name without the extension: what the routes take, and what
      /// <see cref="HelpPages.WikiPageOf"/> looks the wiki's page for it up by - the
      /// wiki names its pages its own way.
      /// </summary>
      public string Name => Doc == null ? null : Doc.EndsWith(".md", StringComparison.OrdinalIgnoreCase) ? Doc.Substring(0, Doc.Length - 3) : Doc;

      /// <summary>The heading to open on, or null for the top of the document.</summary>
      public string Heading { get; }
   }

   /// <summary>
   /// Every page of the navigation, mapped to its document. The keys are the
   /// navigation keys of <see cref="NavigationMap"/>, which is what the pane is
   /// told when the page changes; build/check-help-map.py holds this table to
   /// the documents (every page has an entry, every entry names a document
   /// that exists, every heading is in its document) and HelpMapTests holds
   /// it to the navigation. The Deck's HELP_MAP (WebAdmin/index.html) and the
   /// webmail's (Portal.js) are the same table for their pages.
   ///
   /// A page whose subject has no document of its own is mapped to
   /// <see cref="HelpTarget.NoGuide"/>, and the pane says so beside the page's
   /// purpose and the list of every document; it was mapped to "the nearest"
   /// document until October 2026, which on some twenty pages was a document
   /// about something else - the groups and public folders opened what a backup
   /// carries, the certificates the remote domains' outbound TLS, LDAP the
   /// webmail's OpenID Connect (review finding F270). When a document is written
   /// for such a page its entry is the one line that changes. A page whose tabs
   /// have documents of their own maps them in <see cref="Tabs"/>, by the tab's
   /// English header, and the pane follows the tab. The documents no page of any
   /// surface maps to are listed in <see cref="ReferenceOnly"/>, which the checker
   /// reads: a document that is in neither is a document the help cannot reach,
   /// and that fails the check - and so does one listed there whose title or
   /// name meets a tab's or a card's heading, the significant words of one among
   /// the other's, unless the document that page opens links it or the checker
   /// says why it is about something else.
   /// </summary>
   public static class HelpMap
   {
      public static readonly IReadOnlyDictionary<string, HelpTarget> Pages =
         new ReadOnlyDictionary<string, HelpTarget>(new Dictionary<string, HelpTarget>(StringComparer.OrdinalIgnoreCase)
         {
            // Getting started, and the dashboard.
            ["welcome"] = new HelpTarget("SetupChecklist.md"),
            ["dashboard"] = new HelpTarget("SetupChecklist.md", "Where it is"),

            // Monitoring and troubleshooting.
            ["status"] = new HelpTarget("DiagnosingStalledMail.md", "First: which half is stuck?"),
            ["queue"] = new HelpTarget("DiagnosingStalledMail.md", "Delivering: the message is accepted but never leaves"),
            ["stalledmail"] = new HelpTarget("DiagnosingStalledMail.md"),
            // FollowConnection.md is the Control Deck's live view of one session's
            // log lines, not the trace of a message.
            ["messagetrace"] = HelpTarget.NoGuide,
            ["suppressions"] = new HelpTarget("BounceSuppression.md", "Looking at the list, and changing it"),
            ["journaling"] = new HelpTarget("Journaling.md", "Where it is edited"),
            ["audit"] = new HelpTarget("AlertsAndAuditTrail.md", "The audit trail"),
            ["discovery"] = new HelpTarget("Discovery.md", "Where it is used"),
            ["logs"] = new HelpTarget("DiagnosingStalledMail.md", "Turn on debug logging first"),
            ["diagnostics"] = new HelpTarget("DiagnosingStalledMail.md", "What to include in a report"),
            ["mxquery"] = new HelpTarget("DiagnosingStalledMail.md", "Delivering: the message is accepted but never leaves"),
            // The checklist itself, not "The document", which is the JSON the route answers.
            ["externalsetup"] = new HelpTarget("SetupChecklist.md"),
            ["dnsrecords"] = new HelpTarget("SetupChecklist.md"),
            ["logging"] = new HelpTarget("Syslog.md"),
            ["api"] = new HelpTarget("CommandLine.md", "Credentials"),
            ["apikeys"] = new HelpTarget("CommandLine.md", "Credentials"),

            // Domains and accounts.
            ["domains"] = new HelpTarget("DomainsAndAccounts.md"),
            ["publicfolders"] = new HelpTarget("PublicFoldersAndGroups.md"),
            ["groups"] = new HelpTarget("PublicFoldersAndGroups.md", "Groups"),
            ["roles"] = new HelpTarget("Roles.md", "Giving somebody a role"),

            // Mail flow. The delivery page's own guides are by tab (Tabs).
            ["delivery"] = new HelpTarget("SettingsReasons.md", "Delivery of e-mail"),
            ["routes"] = new HelpTarget("RemoteDomains.md", "A policy is not a route"),
            ["remotedomains"] = new HelpTarget("RemoteDomains.md"),
            ["relays"] = new HelpTarget("SettingsReasons.md", "Incoming relays"),
            ["rules"] = new HelpTarget("MailFlowRules.md"),
            ["servermessages"] = HelpTarget.NoGuide,
            ["sendout"] = HelpTarget.NoGuide,

            // Spam and virus filtering. SmtpPolicy.md, which the overview, the
            // anti-spam settings and SURBL opened, never mentions SURBL and is the
            // policy the score cannot express; ReputationWatch.md, which the block
            // lists opened, watches this server's own listing, in a syntax the
            // page does not take; SealedMail.md, which the virus pages opened, is
            // what an encrypted message gets.
            ["spamoverview"] = HelpTarget.NoGuide,
            ["quarantine"] = new HelpTarget("SenderListsAndDigests.md", "Reviewing the quarantine"),
            ["antispam"] = HelpTarget.NoGuide,
            ["surbl"] = HelpTarget.NoGuide,
            ["dnsbl"] = HelpTarget.NoGuide,
            ["spamwhitelist"] = HelpTarget.NoGuide,
            // Blocked senders is in the sender lists' document since its subtitle's
            // warning moved there - the list matches the address the sender claims,
            // so it is no defence against spoofing - beside the table of how the
            // lists differ, both under The list.
            ["blockedsenders"] = new HelpTarget("SenderListsAndDigests.md", "The list"),
            ["senderlist"] = new HelpTarget("SenderListsAndDigests.md", "The list"),
            ["greylistwhitelist"] = new HelpTarget("Greylisting.md", "What greylisting does"),
            ["greylistbypass"] = new HelpTarget("Greylisting.md", "The bypass list (#284)"),
            ["domainpolicylist"] = new HelpTarget("SmtpPolicy.md", "The domain white and black list (#285)"),
            ["virusoverview"] = HelpTarget.NoGuide,
            ["antivirus"] = HelpTarget.NoGuide,
            ["blockedattachments"] = HelpTarget.NoGuide,

            // Listeners, TLS and certificates. RemoteDomains.md's Outbound TLS,
            // which all five TLS pages opened, is one remote domain's policy.
            // The page's settings say what each does in two sentences; the reasons
            // they gave at length are its document's (review finding F310).
            ["protocols"] = new HelpTarget("Protocols.md"),
            ["ports"] = new HelpTarget("SecureDefaults.md", "What a new installation starts with"),
            ["webservices"] = new HelpTarget("SettingsReasons.md", "Web services & autoconfiguration"),
            ["webmail"] = new HelpTarget("PasskeysAndSingleSignOn.md"),
            ["dns"] = new HelpTarget("SettingsReasons.md", "DNS resolver"),
            ["certs"] = new HelpTarget("SecureDefaults.md", "The certificate"),
            ["acme"] = new HelpTarget("SecureDefaults.md", "The certificate"),
            ["tlsoverview"] = new HelpTarget("SecureDefaults.md", "What a new installation starts with"),
            ["tls"] = new HelpTarget("SettingsReasons.md", "TLS versions & ciphers"),
            ["security"] = new HelpTarget("SettingsReasons.md", "Transport security"),

            // Authentication and access. Directory.md is the organisation's
            // address list, not LDAP; CountryBlocking.md is the auto-ban page's
            // Countries tab, not the IP ranges.
            ["authentication"] = new HelpTarget("PasskeysAndSingleSignOn.md"),
            ["ldap"] = new HelpTarget("SettingsReasons.md", "Directory authentication (LDAP)"),
            ["directorysync"] = new HelpTarget("SettingsReasons.md", "Directory synchronisation"),
            ["adminaccess"] = new HelpTarget("SecretsAtRest.md", "The administrator password"),
            ["autoban"] = new HelpTarget("AutoBan.md"),
            ["ipranges"] = HelpTarget.NoGuide,

            // The server itself. The stage timeouts DiagnosingStalledMail.md
            // tabulates are on Server limits, where ServiceAccount.md opened.
            ["profiles"] = new HelpTarget("Profiles.md"),
            ["backup"] = new HelpTarget("Backup.md"),
            ["updates"] = new HelpTarget("UpdateFeed.md"),
            ["performance"] = new HelpTarget("SettingsReasons.md", "Performance"),
            ["advanced"] = new HelpTarget("SettingsReasons.md", "Advanced"),
            ["scripts"] = new HelpTarget("EventScripts.md", "Which language, where"),
            ["hardening"] = new HelpTarget("DiagnosingStalledMail.md", "Settings that bound each stage"),
            ["about"] = new HelpTarget("Upgrading.md"),
         });

      /// <summary>
      /// The tabs of a page that have documents of their own, keyed
      /// "page#Tab" by the page's navigation key and the tab's English header, as
      /// the page builds it (Tab(L("SNMP")) on the Monitoring &amp; alerts page): the pane opens
      /// the tab's document while the tab is the one showing, and the page's own
      /// entry on its other tabs. A tab whose subject the page's entry does not
      /// cover, and no document does, is <see cref="HelpTarget.NoGuide"/> here.
      /// build/check-help-map.py and HelpMapTests hold every entry to a tab its
      /// page builds.
      /// </summary>
      public static readonly IReadOnlyDictionary<string, HelpTarget> Tabs =
         new ReadOnlyDictionary<string, HelpTarget>(new Dictionary<string, HelpTarget>(StringComparer.OrdinalIgnoreCase)
         {
            ["delivery#Delivery"] = new HelpTarget("BounceSuppression.md", "Turning it on"),
            ["delivery#Feedback"] = new HelpTarget("Complaints.md"),
            ["delivery#Reputation"] = new HelpTarget("ReputationWatch.md"),
            ["delivery#Mailing lists"] = new HelpTarget("MailingLists.md"),

            ["protocols#SMTP"] = new HelpTarget("Protocols.md", "SMTP"),
            ["protocols#ManageSieve"] = new HelpTarget("SettingsReasons.md", "Protocols"),

            ["antispam#Sender auth"] = new HelpTarget("SmtpPolicy.md", "SPF's other results (#286)"),
            ["antispam#Host checks"] = new HelpTarget("SmtpPolicy.md"),
            ["antispam#Greylisting"] = new HelpTarget("Greylisting.md", "What greylisting does"),
            ["antispam#Spam traps"] = new HelpTarget("SpamTraps.md"),

            ["antivirus#Sandbox"] = new HelpTarget("Sandbox.md"),

            ["autoban#Countries"] = new HelpTarget("CountryBlocking.md"),
            ["autoban#Compromised accounts"] = new HelpTarget("CompromisedAccounts.md"),

            ["logging#Audit trail"] = new HelpTarget("AlertsAndAuditTrail.md", "The audit trail"),

            ["api#Metrics"] = new HelpTarget("SettingsReasons.md", "Monitoring & alerts"),
            ["api#Alerts & webhooks"] = new HelpTarget("AlertsAndAuditTrail.md", "Alerts"),
            ["api#SNMP"] = new HelpTarget("Snmp.md"),
            ["api#Shutdown"] = new HelpTarget("HighAvailabilityRunbook.md", "5. Planned failover (maintenance)"),

            ["performance#Message store"] = new HelpTarget("MessageStore.md"),

            ["advanced#Scripting"] = new HelpTarget("EventScripts.md", "The engine"),

            ["webmail#Browser sessions"] = new HelpTarget("DevicesAndSessions.md", "The session policies"),
            ["webmail#What people may do"] = new HelpTarget("Directory.md", "The two switches"),
            ["webmail#Encryption"] = new HelpTarget("OpenPGP.md"),
            ["webmail#Notifications"] = HelpTarget.NoGuide,
            ["webmail#Import"] = new HelpTarget("Migration.md", "From the webmail, by the person moving"),

            ["adminaccess#Two-factor"] = HelpTarget.NoGuide,
         });

      /// <summary>
      /// The documents no page of any surface maps to: reached from every pane's
      /// "All documentation" list, and from the documents that link them. Most
      /// are about the project or a procedure rather than a page - a runbook, a
      /// build, a census - which is why no page owns them; a few are about one
      /// card of a page and are linked from the document that page opens (the
      /// checker's REACHED_FROM: the service account's from the stalled-mail
      /// document Server limits opens, the received reports' from the Feedback
      /// tab's complaints document). build/check-help-map.py fails when a
      /// document is in neither this list nor any map, when one is in both, and
      /// when one listed here meets a tab's or a card's heading with neither such
      /// a link nor a stated reason.
      /// </summary>
      public static readonly IReadOnlyList<string> ReferenceOnly = new[]
      {
         "AccountRecovery.md",
         "ActiveActive.md",
         "Alpine.md",
         "BrandLogos.md",
         "Channels.md",
         "CommandLinePackages.md",
         "ContainerImage.md",
         "ContinuousIntegration.md",
         "ControlDeckFramework.md",
         "ControlPanelDesign.md",
         "DatabaseTls.md",
         "DkimKeyRotation.md",
         "DmarcReports.md",
         "EncryptionAtRest.md",
         "EnvironmentConfiguration.md",
         "EvidencePack.md",
         "FollowConnection.md",
         "ForkAndOriginal.md",
         "Fuzzing.md",
         "Help.md",
         "ImpersonationProtection.md",
         "IpWarmup.md",
         "LiveUpdateKinds.md",
         "MailboxMove.md",
         "MessageRecall.md",
         "MigratingDatabaseBackend.md",
         "Mirroring.md",
         "Nix.md",
         "ObjectStorage.md",
         "OutboundContentPolicy.md",
         "OutboundShaping.md",
         "PackageManagers.md",
         "PackagesAppImage.md",
         "PackagesDeb.md",
         "PackagesRpm.md",
         "PackagesWindows.md",
         "Performance.md",
         "PersonalData.md",
         "PublishedNumbers.md",
         "RegressionEnvironment.md",
         "RegulatoryScope.md",
         "RelocatingAnInstallation.md",
         "ReproducibleBuilds.md",
         "ResponseCompression.md",
         "RestrictedDelivery.md",
         "RoomsAndEquipment.md",
         "SampleDomain.md",
         "Scim.md",
         "SealedMail.md",
         "SendAs.md",
         "SendApi.md",
         "ServiceAccount.md",
         "SettingLevels.md",
         "SettingsApply.md",
         "SharedAddressBooks.md",
         "Signatures.md",
         "SmimeAuthority.md",
         "Snap.md",
         "TestLab.md",
         "SourcePackages.md",
         "SourceTarball.md",
         "ThirdPartyBinaries.md",
         "WarmStandby.md",
         "WindowsOnArm.md",
         "ZeroAccess.md",
      };

      /// <summary>The help for a page, or null for a key that is not a page.</summary>
      public static HelpTarget For(string key)
         => key != null && Pages.TryGetValue(key, out HelpTarget target) ? target : null;

      /// <summary>
      /// The help for a page on one of its tabs, by the tab's English header: the
      /// tab's own entry where it has one, else the page's; null for a key that is
      /// not a page.
      /// </summary>
      public static HelpTarget For(string key, string tab)
      {
         if (key != null && !string.IsNullOrEmpty(tab) && Tabs.TryGetValue(key + "#" + tab, out HelpTarget onTab))
            return onTab;
         return For(key);
      }

      /// <summary>The English headers of the page's tabs that have entries of their own, in the table's order.</summary>
      public static IReadOnlyList<string> TabsOf(string key)
      {
         var tabs = new List<string>();
         if (string.IsNullOrEmpty(key))
            return tabs;

         string prefix = key + "#";
         foreach (string entry in Tabs.Keys)
         {
            if (entry.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
               tabs.Add(entry.Substring(prefix.Length));
         }
         return tabs;
      }
   }
}
