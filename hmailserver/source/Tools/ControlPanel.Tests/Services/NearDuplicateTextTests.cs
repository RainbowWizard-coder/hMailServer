// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Texts that said one thing two ways, each one more catalogue entry to
   /// translate seventeen times and to drift (review finding F325): the read
   /// failure of the DNS records page and of the three overviews, the directory
   /// synchronisation schedule on its own page and on the LDAP page, a Welcome
   /// starting point and the search answer for the same task, the MX query's
   /// purpose and its search answer, the two answers about mailing lists, and
   /// names given by hand that repeat the caption WPF already names the control
   /// by.
   /// </summary>
   public class NearDuplicateTextTests
   {
      private static readonly string[] Gone =
      {
         "Some values could not be read, so the cards below may be incomplete. Why: {0}",
         "Some values could not be read, so the cards below may be incomplete. The first reason: {0}",
         "Saved {0} - the server picks the selection up within two seconds; the new schedule takes effect when the service restarts.",
         "Saved {0} - the server picks the selection up within two seconds; no service restart is needed.",
         "The new schedule takes effect only when the hMailServer service restarts. Restart the service now? Every client connected to the server is disconnected while it stops and starts again.",
         "Work out which half has stalled - accepting or delivering - turn on the logging that names the cause, and read the lines that do.",
         "Look up the MX records for any domain.",
         "Distribution lists are created inside the domain that owns the list address.",
         "Turn on debug logging now",
         "Also mark accounts inactive when their directory entry has gone",
      };

      [Fact]
      public void TheSecondWordings_AreGoneFromTheCatalogue()
      {
         var keys = XDocument.Load(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"))).Root
            .Elements("data").Select(d => (string)d.Attribute("name")).ToHashSet(StringComparer.Ordinal);
         Assert.True(keys.Count > 5000, "The catalogue was not read.");
         foreach (string gone in Gone)
            Assert.DoesNotContain(gone, keys);
      }

      [Fact]
      public void TheDnsRecordsReadFailure_IsTheOverviewsSentence()
      {
         string dns = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DnsRecordsView.cs"));
         string spam = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SpamOverviewView.cs"));
         foreach (string sentence in new[]
         {
            "\"Some values could not be read, so the sections below may be incomplete. Why: {0}\"",
            "\"Some values could not be read, so the sections below may be incomplete. The first reason: {0}\"",
         })
         {
            Assert.Contains(sentence, dns, StringComparison.Ordinal);
            Assert.Contains(sentence, spam, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void TheDirectorySynchronisationSchedule_IsSaidAsTheLdapPageSaysIt()
      {
         string sync = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         string ldap = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "LdapSettingsView.cs"));
         foreach (string sentence in new[]
         {
            "\"Saved {0} - the server picks this up within two seconds, but the directory synchronisation schedule takes effect when the service restarts.\"",
            "\"Saved {0} - the server picks this up within two seconds. No service restart is needed.\"",
            "\"The directory synchronisation schedule takes effect only when the hMailServer service restarts. Restart the service now? Every client connected to the server is disconnected while it stops and starts again.\"",
         })
         {
            Assert.Contains(sentence, sync, StringComparison.Ordinal);
            Assert.Contains(sentence, ldap, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void ATaskIsDescribedOnce_WhereverItIsOffered()
      {
         Assert.Equal(IntentIndex.Entries.Single(e => e.Phrase == "mail is stalled").Answer,
            WelcomeIntents.Entries.Single(i => i.Page == "stalledmail").Blurb);
         Assert.Equal(NavigationMap.Find("mxquery").Purpose,
            IntentIndex.Entries.Single(e => e.Phrase == "where does mail for a domain go").Answer);
         Assert.Equal(IntentIndex.Entries.Single(e => e.Phrase == "delete a mailing list").Answer,
            IntentIndex.Entries.Single(e => e.Phrase == "set up a mailing list").Answer);
      }

      /// <summary>
      /// A name given by hand that is the caption without its Alt key is the name
      /// the control already has: WPF's, from the words on a button or a check box.
      /// </summary>
      [Fact]
      public void NoControlIsNamedByHandWithItsOwnCaption()
      {
         string stalled = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "StalledMailView.cs"));
         Assert.Contains("Content = L(\"_Turn on debug logging now\"),", stalled, StringComparison.Ordinal);
         Assert.DoesNotContain("AutomationProperties.SetName(enable,", stalled, StringComparison.Ordinal);
         string sync = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         Assert.Contains("disableMissing_.Content = L(\"Also mark accounts _inactive when their directory entry has gone\");", sync, StringComparison.Ordinal);
         Assert.DoesNotContain("AutomationProperties.SetName(disableMissing_,", sync, StringComparison.Ordinal);
      }

      /// <summary>The DKIM key's file filters are the catalogue's, as the DNS records page's are.</summary>
      [Fact]
      public void TheDkimKeysFileFilters_AreTranslated()
      {
         string domain = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DomainDialog.cs"));
         Assert.Contains("Filter = L(\"PEM key files (*.pem)|*.pem|All files (*.*)|*.*\"),", domain, StringComparison.Ordinal);
         Assert.Contains("L(\"PEM/key files (*.pem;*.key)|*.pem;*.key|All files (*.*)|*.*\")", domain, StringComparison.Ordinal);
         Assert.DoesNotContain("Filter = \"PEM", domain, StringComparison.Ordinal);
         Assert.Contains("Filter = L(\"PEM key files (*.pem)|*.pem|All files (*.*)|*.*\")",
            ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DnsRecordsView.cs")), StringComparison.Ordinal);
      }
   }
}
