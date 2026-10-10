// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Delivery of e-mail and Protocols pages' headings and density (review
   /// finding F310): the first Delivery card repeated the page's title; the
   /// reputation watch wrote its alert's identifier inline as a word; the eight
   /// idle timeouts stood in one column, the eighth below the fold; the SMTP
   /// tab's blurbs ran to six and seven lines; and "Changes apply after pressing
   /// Save" named a button whose caption is Save changes.
   /// </summary>
   public class ProtocolsAndDeliveryCopyTests
   {
      private static readonly string Page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));

      [Fact]
      public void TheDeliveryCardHasANameOfItsOwn_AndTheAlertIsQuoted()
      {
         Assert.Contains("var del = Card(L(\"Delivery, retries and throttling\"),", Page, StringComparison.Ordinal);
         Assert.DoesNotContain("Card(L(\"Delivery of e-mail\")", Page, StringComparison.Ordinal);
         Assert.Contains("raises the alert 'reputation.listed' when one is.", Page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheServicesBlurbNamesTheButtonByItsCaption()
      {
         Assert.Contains("Changes apply when you press Save changes.", Page, StringComparison.Ordinal);
         Assert.DoesNotContain("after pressing Save.", Page, StringComparison.Ordinal);
      }

      [Fact]
      public void EachTimeoutsMinimumIsBesideItsMaximum()
      {
         Assert.Contains("idle.Pairs = true;", Page, StringComparison.Ordinal);
         Assert.Matches(@"if \(card\.Pairs\)\s+PairRows_\(fields\);", Page);
         // The eight are four pairs, each a minimum then its maximum.
         string idle = Page.Substring(Page.IndexOf("var idle = Card(L(\"Idle timeouts\")", StringComparison.Ordinal));
         idle = idle.Substring(0, idle.IndexOf("Tab(L(\"Timeouts\"))", StringComparison.Ordinal));
         string[] keys = Regex.Matches(idle, "Path = \"([A-Z0-9a-z]+)\"").Select(m => m.Groups[1].Value).ToArray();
         Assert.Equal(new[] { "SMTPDMinTimeout", "SMTPDMaxTimeout", "SMTPCMinTimeout", "SMTPCMaxTimeout", "POP3DMinTimeout", "POP3DMaxTimeout", "POP3CMinTimeout", "POP3CMaxTimeout" }, keys);
      }

      [Fact]
      public void TheSmtpBlurbsAreTwoSentences_AndTheirReasonsAreTheDocuments()
      {
         string smtp = Page.Substring(Page.IndexOf("var smtp = Card(L(\"SMTP\"));", StringComparison.Ordinal));
         smtp = smtp.Substring(0, smtp.IndexOf("Tab(L(\"SMTP\")).Cards.Add(smtp);", StringComparison.Ordinal));
         foreach (string path in new[] { "RejectFullMailboxAtRcpt", "SmtpAuthenticatedSenderCheck", "ArchiveRetentionDays", "MetricsPerDomainEnabled" })
         {
            string setting = smtp.Substring(smtp.IndexOf("Path = \"" + path + "\"", StringComparison.Ordinal));
            string blurb = Regex.Match(setting, "Blurb = L\\(\"([^\"]*)\"\\)").Groups[1].Value;
            int sentences = Regex.Matches(blurb, @"[.!?](\s|$)").Count;
            Assert.True(sentences <= 2, path + "'s blurb has " + sentences + " sentences: " + blurb);
         }

         Assert.Equal("Protocols.md", HelpMap.For("protocols").Doc);
         Assert.Equal("SMTP", HelpMap.For("protocols", "SMTP").Heading);
      }
   }
}
