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
   /// No text writes an ordinary word in capitals to stress it, in English or in a translation
   /// (review finding F297). About sixty texts did: "This server only CHECKS SPF", "the PTR is
   /// NOT published", "applied to THIS account's copy", "Match ALL criteria (AND)", "READ THIS
   /// FIRST:", "the directory ACCEPTED the credentials", "Signing is currently ON." - and the
   /// catalogues copied the capitals, "den RICHTIGEN Schlüssel", "「正しい」パスフレーズ". They are
   /// written in sentence case now, and the sentence carries the stress.
   ///
   /// A word in capitals is two capital letters or more standing alone - not a part of
   /// hm_accounts, %MACRO_FROM%, X-Spam-Flag, MTA-STS, a path or a placeholder. One is allowed
   /// when it is a name: a protocol, a standard, a product, a format, a record type, a command or
   /// a unit (Names); a few phrases are names as a whole (MAIL FROM, RCPT TO, NT SERVICE, HH:MM);
   /// an operator alone in brackets is the rule editor's own word ("(AND)", "(UND)"); and a text
   /// in capitals as a whole is a label (the rule card's IF and THEN). A translation may keep
   /// every word in capitals its English text has, and add the names its language writes so.
   /// </summary>
   public class EmphasisCapitalsTests
   {
      private static readonly Regex CapitalsWord = new Regex(
         @"(?<![\w%$\-./\\@{<&#])\p{Lu}[\p{Lu}\p{Nd}]*\p{Lu}(?:'s)?(?![\w%\-/\\@}>(]|\.\w)");

      // Names that hold a word of their own, taken out of an English text before it is read.
      private static readonly string[] NamePhrases =
      {
         "MAIL FROM", "RCPT TO", "NT SERVICE", "SEARCH BODY and TEXT", "YYYY-MM-DD", "HH:MM:SS", "HH:MM",
         "[EXTERNAL]", "DOMAIN\\name", "--export-stored-secrets-key FILE", "CAP_NET_ADMIN",
      };

      // The words in capitals that are names. A new one goes here only when it is a name; a
      // word written in capitals for stress is written in sentence case instead.
      private static readonly HashSet<string> Names = new HashSet<string>(StringComparer.Ordinal)
      {
         "AAAA", "ACL", "ACME", "AD", "ADFS", "ADO", "AEAD", "API", "ARC", "ASCII", "AUTH", "AV", "AWS", "BATV",
         "BDAT", "BIMI", "BINARYMIME", "CA", "CBC", "CC", "CE", "CHUNKING", "CIDR", "CLI", "CN", "CNAME", "COM",
         "COMPRESS", "CONDSTORE", "DANE", "DATA", "DC", "DCOM", "DEFLATE", "DELETE", "DHE", "DKIM", "DLP", "DMARC",
         "DMZ", "DN", "DNS", "DNSBL", "DNSSEC", "DPAPI", "ECDHE", "ECDSA", "EHLO", "FETCH", "GB", "GET", "GIF",
         "GNU", "GSSAPI", "GUI", "HELO", "HKLM", "HMAC", "HS", "HTML", "HTTP", "HTTPS", "ID", "IDLE", "IE", "IMAP",
         "INBOX", "INI", "IP", "ISP", "JPEG", "JSON", "JWK", "JWKS", "KB", "LDAP", "LDAPS", "MB", "MIME", "MS",
         "MSVC", "MTA", "MTU", "MX", "NAT", "NCSA", "NIST", "NTFS", "NTLM", "OK", "OTLP", "PEM", "PIPELINING",
         "PKCE", "PKI", "PLAIN", "PNG", "POST", "PROXY", "PSBL", "PST", "PTR", "QR", "QRESYNC", "QUOTA", "RCPT",
         "REST", "RFC", "RPC", "RS", "RSA", "RU", "SAML", "SASL", "SCIM", "SEARCH", "SERVFAIL", "SETACL", "SIEM",
         "SMTP", "SNMP", "SORT", "SPF", "SQL", "SRS", "SSL", "SSPI", "STARTTLS", "STATUS", "SURBL", "TCP", "TELL",
         "TLS", "TLSA", "TTL", "TXT", "UDP", "UI", "UID", "UPN", "URI", "URL", "UTC", "VAPID", "VERP", "XCLIENT",
         "ZEN",
      };

      // Names a translation writes in capitals where its English text has none: a record type
      // or a protocol named where the English says "the record" or "the check".
      private static readonly HashSet<string> NamesAnyTranslationAdds = new HashSet<string>(StringComparer.Ordinal)
      {
         "CBC", "DKIM", "DMARC", "DN", "DNS", "DNSSEC", "EXPUNGE", "IBAN", "ID", "IP", "JSON", "MX", "PST", "RSA",
         "SPF", "SRS", "STARTTLS", "TLS", "TTL", "TXT", "URL",
      };

      // A language's own abbreviations: a CA (fr AC, ru/uk ЦС), an ISP (fr FAI, tr İSS), its
      // units and its time formats (ЧЧ:ММ, TT:MM), software (ru ПО) and the Japanese BAN.
      private static readonly Dictionary<string, string[]> OwnNames = new Dictionary<string, string[]>(StringComparer.Ordinal)
      {
         ["da"] = new[] { "TT" },
         ["de"] = new[] { "WLAN" },
         ["fi"] = new[] { "TT" },
         ["fr"] = new[] { "AC", "FAI" },
         ["ja"] = new[] { "BAN", "OS", "PC" },
         ["nb"] = new[] { "TT" },
         ["nl"] = new[] { "UU" },
         ["pl"] = new[] { "WWW" },
         ["ru"] = new[] { "ГБ", "КБ", "МБ", "ММ", "ПО", "СС", "ЦС", "ЧЧ" },
         ["sv"] = new[] { "DOMÄN" },
         ["tr"] = new[] { "DD", "SS", "ISS" },
         ["uk"] = new[] { "ГБ", "ГГ", "КБ", "МБ", "СС", "ХХ", "ЦС" },
      };

      private static readonly string[] Languages =
      {
         "cs", "da", "de", "es", "fi", "fr", "it", "ja", "nb", "nl", "pl", "pt-BR", "ru", "sv", "tr", "uk", "zh-Hans",
      };

      [Fact]
      public void NoEnglishText_WritesAWordInCapitalsForStress()
      {
         var offenders = new List<string>();
         foreach (string text in Catalogue("Strings.resx").Keys)
         {
            if (IsALabel_(text))
               continue;
            List<string> stressed = Capitals(text, NamePhrases).Where(word => !Names.Contains(word)).ToList();
            if (stressed.Count > 0)
               offenders.Add(string.Join(", ", stressed) + ": " + text);
         }

         Assert.True(offenders.Count == 0,
            "Write these in sentence case, or add a name to EmphasisCapitalsTests.Names:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void NoTranslation_WritesAWordInCapitalsForStress()
      {
         var offenders = new List<string>();
         foreach (string language in Languages)
         {
            var allowed = new HashSet<string>(NamesAnyTranslationAdds, StringComparer.Ordinal);
            if (OwnNames.TryGetValue(language, out string[] own))
               allowed.UnionWith(own);

            foreach ((string key, string value) in Catalogue("Strings." + language + ".resx"))
            {
               string english = key.EndsWith("|few", StringComparison.Ordinal) ? key[..^4] : key;
               if (IsALabel_(english))
                  continue;
               var englishCapitals = new HashSet<string>(Capitals(english, Array.Empty<string>()), StringComparer.Ordinal);
               List<string> stressed = Capitals(value, Array.Empty<string>())
                  .Where(word => !englishCapitals.Contains(word) && !allowed.Contains(word)).ToList();
               if (stressed.Count > 0)
                  offenders.Add(language + ": " + string.Join(", ", stressed) + ": " + value);
            }
         }

         Assert.True(offenders.Count == 0,
            "Write these as the language writes the word, or add the language's own name to EmphasisCapitalsTests.OwnNames:\n"
            + string.Join("\n", offenders));
      }

      [Theory]
      [InlineData("Match ALL criteria (AND)", "ALL")]
      [InlineData("This server only CHECKS SPF on incoming mail", "CHECKS")]
      [InlineData("Signing is currently ON.", "ON")]
      [InlineData("so the key was NOT revoked: {0}", "NOT")]
      [InlineData("applied to THIS account's copy at delivery", "THIS")]
      [InlineData("READ THIS FIRST: IMAP, POP3 and SMTP have no way to change a password", "READ, THIS, FIRST")]
      [InlineData("Where mail FROM this domain leaves through", "FROM")]
      public void AWordInCapitals_IsFound(string text, string words)
      {
         Assert.Equal(words, string.Join(", ", Capitals(text, NamePhrases).Where(word => !Names.Contains(word))));
      }

      [Theory]
      [InlineData("Match all criteria (AND)")]
      [InlineData("Blocked: refused at MAIL FROM.")]
      [InlineData("At RCPT TO, or after DATA for an entry that needs DKIM or DMARC")]
      [InlineData("Move to NT SERVICE\\hMailServer…")]
      [InlineData("Daily backup time (24-hour HH:MM; empty = off)")]
      [InlineData("MTA-STS policy for %MACRO_FROM% in hm_accounts, X-Spam-Flag, ADO's own timeout and POP3")]
      public void ANameInCapitals_IsNotStress(string text)
      {
         Assert.DoesNotContain(Capitals(text, NamePhrases), word => !Names.Contains(word));
      }

      /// <summary>The words in capitals of a text, each without its "'s", the phrases taken out first.</summary>
      internal static List<string> Capitals(string text, IEnumerable<string> phrases)
      {
         string plain = phrases.Aggregate(text, (s, phrase) => s.Replace(phrase, " ", StringComparison.Ordinal)).Replace("_", "", StringComparison.Ordinal);
         var words = new List<string>();
         foreach (Match match in CapitalsWord.Matches(plain))
         {
            // "(AND)", "(UND)", "（OR）": an operator, the rule editor's word
            int after = match.Index + match.Length;
            bool bracketed = match.Index > 0 && (plain[match.Index - 1] is '(' or '（')
                             && after < plain.Length && (plain[after] is ')' or '）');
            if (bracketed)
               continue;
            string word = match.Value.EndsWith("'s", StringComparison.Ordinal) ? match.Value[..^2] : match.Value;
            words.Add(word);
         }

         return words;
      }

      // IF, THEN, OK, SORT: a text in capitals as a whole is a label, and its translation is its own.
      private static bool IsALabel_(string text)
      {
         string plain = text.Replace("_", "", StringComparison.Ordinal);
         return plain.Any(char.IsLetter) && plain == plain.ToUpperInvariant();
      }

      private static Dictionary<string, string> Catalogue(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", file));
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }
   }
}
