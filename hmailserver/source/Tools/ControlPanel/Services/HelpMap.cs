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

      /// <summary>The document's file name, extension included: "Backup.md".</summary>
      public string Doc { get; }

      /// <summary>The document's name without the extension: what the routes and the wiki take.</summary>
      public string Name => Doc.EndsWith(".md", StringComparison.OrdinalIgnoreCase) ? Doc.Substring(0, Doc.Length - 3) : Doc;

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
   /// A page whose subject has no document of its own is mapped to the nearest
   /// one - an entry that named nothing would be a pane that opens on nothing -
   /// and when a document is written for it, its entry is the one line that
   /// changes. The documents no page of any surface maps to are listed in
   /// <see cref="ReferenceOnly"/>, which the checker reads: a document that is
   /// in neither is a document the help cannot reach, and that fails the check.
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
            ["messagetrace"] = new HelpTarget("FollowConnection.md"),
            ["suppressions"] = new HelpTarget("BounceSuppression.md", "Looking at the list, and changing it"),
            ["journaling"] = new HelpTarget("Journaling.md", "Where it is edited"),
            ["audit"] = new HelpTarget("AlertsAndAuditTrail.md", "The audit trail"),
            ["logs"] = new HelpTarget("DiagnosingStalledMail.md", "Turn on debug logging first"),
            ["diagnostics"] = new HelpTarget("DiagnosingStalledMail.md", "What to include in a report"),
            ["mxquery"] = new HelpTarget("DiagnosingStalledMail.md", "Delivering: the message is accepted but never leaves"),
            ["externalsetup"] = new HelpTarget("SetupChecklist.md", "The document"),
            ["dnsrecords"] = new HelpTarget("SetupChecklist.md", "The document"),
            ["logging"] = new HelpTarget("Syslog.md"),
            ["api"] = new HelpTarget("CommandLine.md", "Credentials"),
            ["apikeys"] = new HelpTarget("CommandLine.md", "Credentials"),

            // Domains and accounts.
            ["domains"] = new HelpTarget("DomainTransforms.md"),
            ["publicfolders"] = new HelpTarget("Backup.md", "In the archive"),
            ["groups"] = new HelpTarget("Backup.md", "In the archive"),
            ["roles"] = new HelpTarget("Roles.md", "Giving somebody a role"),

            // Mail flow.
            ["delivery"] = new HelpTarget("SmtpPolicy.md"),
            ["routes"] = new HelpTarget("RemoteDomains.md", "A policy is not a route"),
            ["remotedomains"] = new HelpTarget("RemoteDomains.md"),
            ["relays"] = new HelpTarget("SmtpPolicy.md"),
            ["rules"] = new HelpTarget("MailFlowRules.md"),
            ["servermessages"] = new HelpTarget("BounceSuppression.md", "Delivery reports"),
            ["sendout"] = new HelpTarget("DistributionLists.md"),

            // Spam and virus filtering.
            ["spamoverview"] = new HelpTarget("SmtpPolicy.md"),
            ["quarantine"] = new HelpTarget("SenderListsAndDigests.md", "The quarantine digest"),
            ["antispam"] = new HelpTarget("SmtpPolicy.md"),
            ["surbl"] = new HelpTarget("SmtpPolicy.md"),
            ["dnsbl"] = new HelpTarget("ReputationWatch.md", "Which lists"),
            ["spamwhitelist"] = new HelpTarget("SenderListsAndDigests.md", "The list"),
            ["blockedsenders"] = new HelpTarget("SenderListsAndDigests.md", "The list"),
            ["senderlist"] = new HelpTarget("SenderListsAndDigests.md", "The list"),
            ["greylistwhitelist"] = new HelpTarget("Greylisting.md", "The bypass list (#284)"),
            ["greylistbypass"] = new HelpTarget("Greylisting.md", "The bypass list (#284)"),
            ["domainpolicylist"] = new HelpTarget("SmtpPolicy.md", "The domain white and black list (#285)"),
            ["virusoverview"] = new HelpTarget("SealedMail.md", "The virus scanner"),
            ["antivirus"] = new HelpTarget("SealedMail.md", "The virus scanner"),
            ["blockedattachments"] = new HelpTarget("SealedMail.md", "The blocked-attachment list"),

            // Listeners, TLS and certificates.
            ["protocols"] = new HelpTarget("DiagnosingStalledMail.md", "Settings that bound each stage"),
            ["ports"] = new HelpTarget("SetupChecklist.md", "The document"),
            ["webservices"] = new HelpTarget("Calendar.md"),
            ["dns"] = new HelpTarget("DiagnosingStalledMail.md", "Delivering: the message is accepted but never leaves"),
            ["certs"] = new HelpTarget("RemoteDomains.md", "Outbound TLS"),
            ["acme"] = new HelpTarget("RemoteDomains.md", "Outbound TLS"),
            ["tlsoverview"] = new HelpTarget("RemoteDomains.md", "Outbound TLS"),
            ["tls"] = new HelpTarget("RemoteDomains.md", "Outbound TLS"),
            ["security"] = new HelpTarget("RemoteDomains.md", "Outbound TLS"),

            // Authentication and access.
            ["authentication"] = new HelpTarget("PasskeysAndSingleSignOn.md"),
            ["ldap"] = new HelpTarget("PasskeysAndSingleSignOn.md", "Single sign-on (OpenID Connect)"),
            ["directorysync"] = new HelpTarget("Directory.md"),
            ["adminaccess"] = new HelpTarget("Roles.md"),
            ["autoban"] = new HelpTarget("CompromisedAccounts.md", "The four signals"),
            ["ipranges"] = new HelpTarget("CountryBlocking.md"),

            // The server itself.
            ["profiles"] = new HelpTarget("Profiles.md"),
            ["backup"] = new HelpTarget("Backup.md"),
            ["performance"] = new HelpTarget("DiagnosingStalledMail.md", "Settings that bound each stage"),
            ["advanced"] = new HelpTarget("ChangedFromStock.md"),
            ["scripts"] = new HelpTarget("Webhooks.md", "What a webhook carries, and what it does not"),
            ["hardening"] = new HelpTarget("ChangedFromStock.md", "A value the file is forcing"),
            ["about"] = new HelpTarget("Upgrading.md"),
         });

      /// <summary>
      /// The documents no page of any surface maps to: reached from every pane's
      /// "All documentation" list and nowhere else. Each is about the project or
      /// a procedure rather than a page - a runbook, a build, a census - which
      /// is why no page owns it. build/check-help-map.py fails when a document
      /// is in neither this list nor any map, and when one is in both.
      /// </summary>
      public static readonly IReadOnlyList<string> ReferenceOnly = new[]
      {
         "AccountRecovery.md",
         "BrandLogos.md",
         "Complaints.md",
         "ControlPanelDesign.md",
         "DkimKeyRotation.md",
         "DmarcReports.md",
         "EnvironmentConfiguration.md",
         "Fuzzing.md",
         "Help.md",
         "HighAvailabilityRunbook.md",
         "ImpersonationProtection.md",
         "MailboxMove.md",
         "MessageRecall.md",
         "MigratingDatabaseBackend.md",
         "OutboundContentPolicy.md",
         "OutboundShaping.md",
         "PackageManagers.md",
         "PersonalData.md",
         "RegressionEnvironment.md",
         "RegulatoryScope.md",
         "RelocatingAnInstallation.md",
         "ResponseCompression.md",
         "RestrictedDelivery.md",
         "SampleDomain.md",
         "SendAs.md",
         "SettingLevels.md",
         "SettingsApply.md",
         "SharedAddressBooks.md",
         "Signatures.md",
         "ThirdPartyBinaries.md",
         "WarmStandby.md",
         "ZeroAccess.md",
      };

      /// <summary>The help for a page, or null for a key that is not a page.</summary>
      public static HelpTarget For(string key)
         => key != null && Pages.TryGetValue(key, out HelpTarget target) ? target : null;
   }
}
