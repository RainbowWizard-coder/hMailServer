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
         Assert.Equal("_Windows-Domäne (leer = ein lokales Windows-Konto)", German["Windows _domain (empty = a local Windows account)"]);
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
         Assert.Equal("Läuft am {0} ab.", German["Expires {0}."]);
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
         Assert.Equal("Aktualisierung", German["Update status"]);
         Assert.StartsWith("Eigene Vertrauensanker", German["Trust anchor override (tag alg digesttype hex; ...)"], StringComparison.Ordinal);
      }

      [Fact]
      public void TheScoreIsABewertungAndTheShippedDefaultIsWerkseitig()
      {
         Assert.DoesNotContain(German.Values, value => value.Contains("Punktzahl", StringComparison.Ordinal));
         Assert.DoesNotContain(German.Values, value => Regex.IsMatch(value, "ausgelieferte(n)? (Vorgabe|Verhalten|Filter|Einstellungen)"));
         Assert.Equal("SPF-Softfail-Bewertung (0 = nicht bewertet)", German["SPF softfail score (0 = not scored)"]);
      }

      /// <summary>
      /// Every text that says what this version ships with says werkseitig in German, the
      /// button with its access key as well: "A_usgelieferte Einstellungen behalten" stood
      /// beside "Die werkseitigen Einstellungen bleiben" in one flow on the Setting profiles
      /// page, and a case-sensitive match on the value with its underscore still in saw
      /// neither it nor "Ausgelieferte". A text that also says a policy is served keeps
      /// ausliefern for the serving, and must say werkseitig for what is shipped.
      /// </summary>
      [Fact]
      public void NoTextThatSaysShippedSaysAusgeliefert()
      {
         var shipped = new Regex(@"\bship(s|ped)?\b", RegexOptions.IgnoreCase);
         var served = new Regex(@"\bserved?\b", RegexOptions.IgnoreCase);
         var ausliefern = new Regex("ausgeliefert|ausliefer", RegexOptions.IgnoreCase);
         List<string> found = German
            .Where(pair => shipped.IsMatch(WithoutAccessKey(pair.Key)))
            .Where(pair => served.IsMatch(pair.Key)
               ? !WithoutAccessKey(pair.Value).Contains("werkseitig", StringComparison.OrdinalIgnoreCase)
               : ausliefern.IsMatch(WithoutAccessKey(pair.Value)))
            .Select(pair => pair.Key + " = " + pair.Value)
            .ToList();
         Assert.True(found.Count == 0, "shipped as ausgeliefert:" + Environment.NewLine + string.Join(Environment.NewLine, found));

         Assert.Equal("_Werkseitige Einstellungen behalten", German["Keep the _shipped settings"]);
         Assert.Equal("Werkseitige Einstellungen behalten", German["Keep the shipped settings"]);
         Assert.StartsWith("Die werkseitigen Einstellungen bleiben", German["The shipped settings are kept; no profile is applied."], StringComparison.Ordinal);
      }

      private static string WithoutAccessKey(string text)
      {
         int at = text.IndexOf('_', StringComparison.Ordinal);
         return at < 0 ? text : text.Remove(at, 1);
      }

      [Fact]
      public void AnAutoBanIsABann()
      {
         Assert.Equal("Auto-Bann", German["Auto-ban"]);
         Assert.DoesNotContain(German.Values, value => value.Contains("Nie-bannen-Liste", StringComparison.Ordinal)
                                                     || value.Contains("automatische Sperre", StringComparison.OrdinalIgnoreCase)
                                                     || value.Contains("Auto-Ban-", StringComparison.Ordinal));
         foreach (string key in new[] { "Names that are never auto-banned", "Localhost is currently banned in the IP ranges.", "Ban for (hours, 1 to 8,760)", "Failed sign-ins before an address is banned (0 = off)" })
            Assert.Matches("[Bb]ann", German[key]);
      }

      /// <summary>
      /// Every German text whose English bans says Bann or bannen, and every Dutch one verbannen or
      /// ban: the Auto-ban caption said "bevor eine Adresse gesperrt wird" directly under "Auto-Bann",
      /// and in Dutch "voordat een adres wordt geblokkeerd" - a Sperre, in German, is the block list.
      /// The IP range's name the server writes, "Auto-ban: &lt;user&gt;", is its own.
      /// </summary>
      [Fact]
      public void ABanIsABannInGermanAndAVerbanningInDutch()
      {
         Dictionary<string, string> dutch = ReadResx("Strings.nl.resx");
         var ban = new Regex(@"\bban(?:s|ned|ning)?\b", RegexOptions.IgnoreCase);
         var offenders = new List<string>();
         foreach ((string key, string value) in German)
         {
            if (!ban.IsMatch(key.Replace("Auto-ban: <user>", "", StringComparison.Ordinal)))
               continue;
            if (!Regex.IsMatch(value, "[Bb]ann"))
               offenders.Add("de: " + key + " = " + value);
            if (dutch.TryGetValue(key, out string nl) && !Regex.IsMatch(nl, "[Bb]an"))
               offenders.Add("nl: " + key + " = " + nl);
         }

         Assert.True(offenders.Count == 0, "A ban is a Bann in German and verbannen in Dutch:\n" + string.Join("\n", offenders));
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
