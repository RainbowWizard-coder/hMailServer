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
   /// One name for one thing, as docs/ControlPanelDesign.md's Words section lists them
   /// (review findings F143 and F301, and the owner's decision on signing in: "Sign in"
   /// everywhere, "logon" only for Windows logon). The group called Sign-in &amp; access
   /// said "keeps failing to log on", "Max invalid logon attempts", "Logon tarpit",
   /// "Minimum seconds between logins" and "log on with their domain password" - eight
   /// forms across the catalogue; the Delivery of e-mail page called its smart host a
   /// relayer, a relay and a smart host; and one message said Couldn't, two said "client
   /// id" and "key id", three wrote a server error's number without the server.
   /// </summary>
   public class GlossaryTests
   {
      // The logon words, and the contexts in which one is right: Windows' own logon (its
      // API, an account's logon restrictions, the service's right and the account it runs
      // as), a protocol mechanism (AUTH LOGIN), a product's label (Entra ID's Login URL),
      // a database login, and the log the server writes ("the backup log on the Live logs
      // page", "logged in full").
      private static readonly Regex LogonWord = new Regex(@"\b(?:log ?on|logons?|log ?in|logins?|logged (?:on|in)|logs (?:on|in))\b", RegexOptions.IgnoreCase);

      private static readonly string[] WindowsLogon =
      {
         "Windows logon", "LogonUser", "PLAIN/LOGIN", "Login URL", "needs a login for", "Log on as a service",
         "registered to log on as", "service logs on as", "service that cannot log on", "log on the Live logs page",
         "logged in full", "logon from this computer is not permitted", "logon is not permitted at this time of day",
         "before the next logon",
      };

      [Fact]
      public void NoEnglishText_SaysLogonForSigningIn()
      {
         var offenders = new List<string>();
         foreach (string text in EnglishTexts())
         {
            string rest = WindowsLogon.Aggregate(text, (s, allowed) => s.Replace(allowed, "", StringComparison.Ordinal));
            if (LogonWord.IsMatch(rest))
               offenders.Add(text);
         }

         Assert.True(offenders.Count == 0, "Sign in, sign-in: logon is Windows' word alone:\n" + string.Join("\n", offenders));
      }

      [Theory]
      [InlineData("Failed sign-ins before an address is banned (0 = off)")]
      [InlineData("Sign-in tarpit")]
      [InlineData("Delay per failed sign-in (seconds; 0 = off)")]
      [InlineData("Minimum seconds between sign-ins to one account (0 = no limit)")]
      [InlineData("Default domain (for sign-ins without a domain)")]
      [InlineData("Reject sign-ins using a weaker stored hash than")]
      [InlineData("Server-wide smart host")]
      [InlineData("Smart host")]
      public void TheSettingsSay_TheGlossarysWord(string caption)
      {
         Assert.Contains(caption, EnglishTexts());
      }

      [Fact]
      public void ThePortuguese_SaysLoginWhereTheEnglishSaysSignIn()
      {
         Dictionary<string, string> portuguese = Catalogue("Strings.pt-BR.resx");
         var signIn = new Regex(@"\bsign(?:s|ed|ing)?[ -]ins?\b", RegexOptions.IgnoreCase);
         var found = portuguese.Where(pair => signIn.IsMatch(pair.Key) && !pair.Key.Contains("Windows logon", StringComparison.Ordinal)
                                              && Regex.IsMatch(pair.Value, @"\blogons?\b", RegexOptions.IgnoreCase))
            .Select(pair => pair.Key + " = " + pair.Value).ToList();
         Assert.True(found.Count == 0, "Sign-in is login in Brazilian Portuguese, logon Windows':\n" + string.Join("\n", found));
      }

      [Fact]
      public void TheJapanese_SaysSignInForAPluralSignInToo()
      {
         // JapaneseSignInWordTests reads "sign in" and "sign-in"; "sign-ins" is the same word.
         Dictionary<string, string> japanese = Catalogue("Strings.ja.resx");
         var found = japanese.Where(pair => pair.Key.Contains("sign-ins", StringComparison.OrdinalIgnoreCase)
                                            && (pair.Value.Contains("ログオン", StringComparison.Ordinal) || pair.Value.Contains("ログイン", StringComparison.Ordinal)))
            .Select(pair => pair.Key + " = " + pair.Value).ToList();
         Assert.True(found.Count == 0, "Sign-in is サインイン in Japanese:\n" + string.Join("\n", found));
      }

      [Fact]
      public void NoEnglishText_UsesTheGlossarysOtherWords()
      {
         var rules = new (Regex Pattern, string Say)[]
         {
            (new Regex(@"\bCouldn't\b"), "Could not"),
            (new Regex(@"\b(?:client|key) id\b"), "client ID, key ID"),
            (new Regex(@"\brelayer\b", RegexOptions.IgnoreCase), "smart host"),
            (new Regex(@"(?<!server )\(error \d{4}"), "server error NNNN"),
            (new Regex(@"hMailServer\.INI"), "hMailServer.ini"),
            // e-mail in prose and for the field; the page keeps its title until the page
            // names are decided again, and the texts that name it say its title
            (new Regex(@"\b[Ee]-mail\b(?!'s Rules)(?<!Delivery of e-mail)"), "mail, message or email address"),
         };

         var offenders = new List<string>();
         foreach (string text in EnglishTexts())
         {
            foreach ((Regex pattern, string say) in rules)
            {
               if (pattern.IsMatch(text) && !IsARuleActionName_(text))
                  offenders.Add(say + ": " + text);
            }
         }

         Assert.True(offenders.Count == 0, "These use a word the glossary replaces:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void TheTcpIpPortsGrid_NamesItsColumnConnectionSecurity()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "TcpIpPortsView.xaml"));
         Assert.Contains("<DataGridTextColumn x:Name=\"SecurityColumn\" Header=\"{loc:L 'Connection security'}\"", page, StringComparison.Ordinal);
      }

      [Fact]
      public void NoFieldCaption_EndsInAColon()
      {
         var caption = new Regex(@"\b(?:DialogFields\.)?Field\(L\(""([^""]*)""\)|\bLabel = L\(""([^""]*)""\)|InputDialog\.Prompt\([^,]+, L\(""[^""]*""\), L\(""([^""]*)""\)");
         var offenders = new List<string>();
         foreach ((string relative, string source) in ConfirmationPatternTests.SourceFiles())
         {
            // the DNS records page's captions are rewritten with the page (review finding F306)
            if (relative.EndsWith("DnsRecordsView.cs", StringComparison.Ordinal))
               continue;
            foreach (Match match in caption.Matches(source))
            {
               string text = match.Groups[1].Value + match.Groups[2].Value + match.Groups[3].Value;
               if (text.EndsWith(':'))
                  offenders.Add(relative + ": " + text);
            }
         }

         Assert.True(offenders.Count == 0, "A caption needs no colon:\n" + string.Join("\n", offenders));
      }

      // The rule actions Delete e-mail and Forward e-mail are the Rules page's, rewritten with
      // its descriptions (review finding F309).
      private static bool IsARuleActionName_(string text) => text is "Delete e-mail" or "Forward e-mail";

      private static IEnumerable<string> EnglishTexts()
      {
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"));
         return XDocument.Load(path).Root.Elements("data").Select(d => (string)d.Attribute("name")).ToList();
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
