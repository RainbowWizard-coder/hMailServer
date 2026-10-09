// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Anti-spam settings and Anti-virus settings pages' hints say what a setting
   /// does and what its safe value or its consequence is, in two sentences at most,
   /// and the reasons are in the page's help topic: docs/SmtpPolicy.md for the one,
   /// docs/SealedMail.md's "The virus scanner" for the other (review finding F147; the
   /// owner's decision on hints). The void-lookup, rsa-sha1, DMARC tree walk and ARC
   /// trusted-sealer hints ran to six lines and more with RFC and project history, and
   /// the scanner-failure card said its default three times.
   /// </summary>
   public class SpamAndVirusHintLengthTests
   {
      private const string Literal = @"L\(""((?:[^""\\]|\\.)*)""\)";

      // A sentence ends at . ! or ? before a space and a capital, a digit, a quote,
      // a bracket or the tilde of "~all": "RFC 7208 4.6.4." ends one, "4.4.3," does not.
      private static readonly Regex SentenceEnd = new Regex(@"(?<=[.!?])\s+(?=[A-Z0-9""'(~])");

      [Fact]
      public void EveryHintOnTheTwoPages_IsTwoSentencesAtMost()
      {
         var hints = Hints().ToList();
         Assert.True(hints.Count > 30, "Only " + hints.Count + " hints were read from BuildAntiSpam and BuildAntiVirus; the pattern no longer reads them.");

         var offenders = hints.Where(hint => Sentences(hint) > 2).Select(hint => Sentences(hint) + " sentences: " + hint).ToList();
         Assert.True(offenders.Count == 0, "These hints run past two sentences; keep what the setting does and its safe value, and move the rest to the page's help topic:\n" + string.Join("\n", offenders));
      }

      [Theory]
      [InlineData("~all: the domain says the client is probably not authorised. A score refuses it.", 2)]
      [InlineData("RFC 7208 4.6.4. A void lookup is a term. Two is what the RFC says.", 3)]
      [InlineData("On, it is answered 451 4.4.3, so the sender tries again. Off, it is scored.", 2)]
      [InlineData("1440 by default; the ban is an expiring IP range. Never an incoming relay.", 2)]
      public void TheSentenceCount_ReadsTheHintsAsAReaderWould(string hint, int expected)
      {
         Assert.Equal(expected, Sentences(hint));
      }

      [Fact]
      public void TheScannerFailureCard_StatesItsDefaultOnce_InTheOption()
      {
         string source = Source();
         int card = source.IndexOf("Card(L(\"When a scanner cannot run\")", StringComparison.Ordinal);
         int end = source.IndexOf("Cards.Add(failure)", card, StringComparison.Ordinal);
         Assert.True(card > 0 && end > card, "The anti-virus page's card When a scanner cannot run was not found.");

         var texts = Regex.Matches(source.Substring(card, end - card), Literal).Select(m => m.Groups[1].Value).ToList();
         Assert.DoesNotContain(texts, text => text.Contains("default", StringComparison.OrdinalIgnoreCase) || text.Contains("always done", StringComparison.Ordinal));
         Assert.Contains("(0, L(\"Deliver it (default)\"))", source, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("SmtpPolicy.md", "## The reasons behind the Anti-spam settings page", "Two is what the RFC says.")]
      [InlineData("SmtpPolicy.md", "## The reasons behind the Anti-spam settings page", "SHA-1 collisions have been practical since 2020")]
      [InlineData("SmtpPolicy.md", "## The reasons behind the Anti-spam settings page", "a resolver outage must not quietly turn relaxed alignment into strict")]
      [InlineData("SmtpPolicy.md", "## The reasons behind the Anti-spam settings page", "The list is not an option of the feature - it is the feature.")]
      [InlineData("SealedMail.md", "### When a scanner cannot run", "the stricter choice and the one to make deliberately")]
      [InlineData("SealedMail.md", "### When a scanner cannot run", "a service restart gives a held message a fresh budget")]
      public void TheReasonsTheHintsLostAreInThePagesHelpTopic(string document, string heading, string reason)
      {
         string text = File.ReadAllText(Path.Combine(FindDocs(), document)).Replace("\r\n", "\n");
         int at = text.IndexOf("\n" + heading + "\n", StringComparison.Ordinal);
         Assert.True(at >= 0, document + " has no heading " + heading);
         // The document is wrapped at its own width; the reason is read across its lines.
         Assert.Contains(reason, Regex.Replace(text.Substring(at), @"\s+", " "), StringComparison.Ordinal);
      }

      [Fact]
      public void TheScannerFailureReasons_AreInsideTheSectionTheAntiVirusPageOpens()
      {
         // HelpMap opens the Anti-virus settings page's help at SealedMail.md's "The virus scanner".
         string text = File.ReadAllText(Path.Combine(FindDocs(), "SealedMail.md")).Replace("\r\n", "\n");
         int section = text.IndexOf("\n## The virus scanner\n", StringComparison.Ordinal);
         int failure = text.IndexOf("\n### When a scanner cannot run\n", StringComparison.Ordinal);
         int next = text.IndexOf("\n## ", section + 1, StringComparison.Ordinal);
         Assert.True(section >= 0 && failure > section && (next < 0 || failure < next),
            "SealedMail.md's When a scanner cannot run is not inside its The virus scanner section.");
      }

      private static int Sentences(string hint) => SentenceEnd.Split(hint.Replace("\\\"", "\"").Trim()).Count(part => part.Length > 0);

      private static IEnumerable<string> Hints()
      {
         string source = Source();
         foreach (var (from, to) in new[] { ("private void BuildAntiSpam()", "private void BuildAntiVirus()"), ("private void BuildAntiVirus()", "private void BuildTls()") })
         {
            int start = source.IndexOf(from, StringComparison.Ordinal);
            int end = source.IndexOf(to, start + from.Length, StringComparison.Ordinal);
            Assert.True(start > 0 && end > start, from + " was not found in ServerSettingsView.xaml.cs.");
            foreach (Match match in Regex.Matches(source.Substring(start, end - start), @"\bBlurb\s*=\s*" + Literal))
               yield return match.Groups[1].Value;
         }
      }

      private static string Source() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));

      private static string FindDocs()
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Combine(at.FullName, "hmailserver", "docs");
            if (Directory.Exists(candidate))
               return candidate;
         }

         throw new DirectoryNotFoundException("the documentation is not above " + AppContext.BaseDirectory);
      }
   }
}
