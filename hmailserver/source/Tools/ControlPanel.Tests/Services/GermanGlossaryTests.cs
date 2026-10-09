// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The German catalogue's one glossary (review finding F284). It said Domain and
   /// Domäne on one screen; Sperrliste for the suppression list, the DNS block lists
   /// and the allow and block list; "Läuft ab 2026-10-20", which reads as "runs
   /// from"; Übersicht for the digest as for the overview pages; Aktualisieren, a
   /// verb, as the Update row's label; Vertrauensanker-Ausnahme for one's own trust
   /// anchors; Auto-Bann beside Sperre for the same ban; Punktzahl beside Bewertung
   /// for the score; and "das ausgelieferte Verhalten" - shipped, in a mail server
   /// where ausliefern is what happens to a message.
   /// </summary>
   public class GermanGlossaryTests
   {
      private static readonly Dictionary<string, string> German = ReadResx("Strings.de.resx");

      [Fact]
      public void AMailDomainIsADomainAndOnlyAWindowsDomainIsADomaene()
      {
         var windows = new Regex(@"Active Directory|Windows|domain controller|joined|join|Negotiate|SSPI|domain password|ServiceDomain|%d|domain account|forest|directory|\bAD\b", RegexOptions.IgnoreCase);
         List<string> found = German
            .Where(pair => pair.Value.Contains("omäne", StringComparison.Ordinal) && !windows.IsMatch(pair.Key))
            .Select(pair => pair.Key + " = " + pair.Value)
            .ToList();
         Assert.True(found.Count == 0, "Domäne for a mail domain:" + Environment.NewLine + string.Join(Environment.NewLine, found));

         Assert.Equal("Domains", German["Domains"]);
         Assert.Contains("Domain", German["Domain - {0}"], StringComparison.Ordinal);
         Assert.Equal("_Windows-Domäne (für ein lokales Windows-Konto leer lassen)", German["Windows _domain (leave empty for a local Windows account)"]);
      }

      [Fact]
      public void TheSuppressionListIsAnUnterdrueckungsliste()
      {
         Assert.Equal("Unterdrückungsliste", German["Suppression list"]);
         List<string> found = German
            .Where(pair => pair.Key.Contains("suppression", StringComparison.OrdinalIgnoreCase) && pair.Value.Contains("Sperrliste", StringComparison.Ordinal))
            .Select(pair => pair.Key)
            .ToList();
         Assert.True(found.Count == 0, string.Join(" | ", found));
      }

      [Fact]
      public void AnExpiryRunsOutOnItsDate()
      {
         Assert.Equal("Läuft am {0} ab.", German["Read-only. Expires {0}."].Substring("Schreibgeschützt. ".Length));
         Assert.Equal("Ablauf", German["Expires"]);
         Assert.DoesNotContain(German.Values, value => value.Contains("Läuft ab {", StringComparison.Ordinal));
      }

      [Fact]
      public void TheDigestIsADigestAndTheOverviewsAreUebersichten()
      {
         Assert.Equal("Digest-Einstellungen", German["digest settings"]);
         Assert.Equal("Quarantäne-Digest", German["Quarantine digest"]);
         List<string> found = German
            .Where(pair => pair.Key.Contains("digest", StringComparison.OrdinalIgnoreCase) && pair.Value.Contains("Übersicht", StringComparison.Ordinal))
            .Select(pair => pair.Key)
            .ToList();
         Assert.True(found.Count == 0, string.Join(" | ", found));
      }

      [Fact]
      public void TheUpdateRowAndTheTrustAnchorsAreNamedAsWhatTheyAre()
      {
         Assert.Equal("Aktualisierung", German["Update"]);
         Assert.StartsWith("Eigene Vertrauensanker", German["Trust anchor override (tag alg digesttype hex; ...)"], StringComparison.Ordinal);
      }

      [Fact]
      public void TheScoreIsABewertungAndTheShippedDefaultIsWerkseitig()
      {
         Assert.DoesNotContain(German.Values, value => value.Contains("Punktzahl", StringComparison.Ordinal));
         Assert.DoesNotContain(German.Values, value => Regex.IsMatch(value, "ausgelieferte(n)? (Vorgabe|Verhalten|Filter|Einstellungen)"));
         Assert.Equal("SPF-Softfail-Bewertung (0 = nicht bewertet)", German["SPF softfail score (0 = not scored)"]);
      }

      [Fact]
      public void AnAutoBanIsABann()
      {
         Assert.Equal("Auto-Bann", German["Auto-ban"]);
         Assert.DoesNotContain(German.Values, value => value.Contains("Nie-bannen-Liste", StringComparison.Ordinal)
                                                     || value.Contains("automatische Sperre", StringComparison.OrdinalIgnoreCase)
                                                     || value.Contains("Auto-Ban-", StringComparison.Ordinal));
         foreach (string key in new[] { "Names that are never auto-banned", "Localhost is currently banned in the IP ranges.", "Ban for (hours, 1 to 8760)" })
            Assert.Matches("[Bb]ann", German[key]);
      }

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(Path.Join(ResourcesDirectory(), file)).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }

      private static string ResourcesDirectory()
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 8 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }
   }
}
