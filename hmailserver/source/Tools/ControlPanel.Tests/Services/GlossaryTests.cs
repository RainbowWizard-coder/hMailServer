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
   /// id" and "key id", and nine wrote a server error's number without the server -
   /// "(error 5921)", "error HM5113", "the server reports HM5508".
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
      [InlineData("Smart host name (empty = direct delivery)")]
      [InlineData("The smart host requires authentication")]
      [InlineData("OAuth2 for the smart host (Microsoft 365)")]
      [InlineData("Outbound delivery, retries and the smart host.")]
      public void TheSettingsSay_TheGlossarysWord(string caption)
      {
         Assert.Contains(caption, EnglishTexts());
      }

      /// <summary>
      /// The server-wide smart host is the smart host wherever it is named: on its tab, in the
      /// settings search index's rows for that tab, in the page's subtitle and map description
      /// and in every text that sends the reader to it on the Delivery of e-mail page - where the
      /// tab called it the smart host in its card's title, a relay host and a relay in its
      /// captions and the OAuth2 card, and smart-host relaying in the subtitle. A domain's own
      /// relay host and a route are relays to a host of their own, and a palette search for the
      /// old names still finds the page.
      /// </summary>
      [Fact]
      public void TheServerWideSmartHost_IsNeverCalledARelay()
      {
         var relay = new Regex(@"\brelay(?:s|ed|ing|er)?\b", RegexOptions.IgnoreCase);
         string Relay(string text) => relay.IsMatch(text.Replace("a domain's relay host", "", StringComparison.Ordinal)) ? text : null;
         var offenders = new List<string>();

         List<hMailServer.ControlPanel.Services.SettingEntry> tab = hMailServer.ControlPanel.Services.SettingsSearchIndex.Entries
            .Where(entry => entry.Page == "delivery" && entry.Tab == "Smart host").ToList();
         Assert.True(tab.Count >= 12, "The search index holds " + tab.Count + " settings on the Smart host tab.");
         offenders.AddRange(tab.Select(entry => Relay(entry.Label + " / " + entry.Card)).Where(text => text != null).Select(text => "search index: " + text));

         string source = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         int start = source.IndexOf("var relay = Card(L(\"Server-wide smart host\")", StringComparison.Ordinal);
         int end = source.LastIndexOf("Tab(L(\"Smart host\")).Cards.Add(", StringComparison.Ordinal);
         Assert.True(start > 0 && end > start, "The Smart host tab's cards are no longer where this test looks for them.");
         var literal = new Regex(@"\bL\(""((?:[^""\\]|\\.)*)""\)");
         offenders.AddRange(literal.Matches(source[start..end]).Select(m => Relay(m.Groups[1].Value)).Where(text => text != null).Select(text => "Smart host tab: " + text));
         int delivery = source.IndexOf("Header.PageKey = \"delivery\";", StringComparison.Ordinal);
         Match subtitle = new Regex(@"Header\.Subtitle = L\(""((?:[^""\\]|\\.)*)""\);").Match(source, delivery);
         if (delivery < 0 || !subtitle.Success || Relay(subtitle.Groups[1].Value) != null)
            offenders.Add("subtitle: " + subtitle.Groups[1].Value);

         hMailServer.ControlPanel.Services.NavNode page = hMailServer.ControlPanel.Services.NavigationMap.Find("delivery");
         if (Relay(page.Purpose) != null)
            offenders.Add("navigation map: " + page.Purpose);

         offenders.AddRange(EnglishTexts().Where(text => text.Contains("on the Delivery of e-mail page", StringComparison.Ordinal))
            .Select(Relay).Where(text => text != null).Select(text => "names the page: " + text));

         Assert.True(offenders.Count == 0, "The server-wide host is the smart host:\n" + string.Join("\n", offenders));
         Assert.Contains("SMTP relayer", page.Aliases);
         Assert.Contains("Relay host", page.Aliases);
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
            (new Regex(@"(?<!server )\berror (?:HM)?\d{4}\b"), "server error NNNN"),
            (new Regex(@"\bHM\d{4}\b"), "server error NNNN"),
            (new Regex(@"hMailServer\.INI"), "hMailServer.ini"),
            // e-mail in prose and for the field; the page keeps its title until the page
            // names are decided again, and the texts that name it say its title
            (new Regex(@"\b[Ee]-mail\b(?!'s Rules)(?<!Delivery of e-mail)"), "mail, message or email address"),
         };

         var offenders = new List<string>();
         foreach (string text in EnglishTexts())
         {
            // Windows' own error number, for a service that cannot log on, is not the server's.
            string judged = text.Replace("reports error 1069 in the Windows event log", "", StringComparison.Ordinal);
            foreach ((Regex pattern, string say) in rules)
            {
               if (pattern.IsMatch(judged) && !IsARuleActionName_(text))
                  offenders.Add(say + ": " + text);
            }
         }

         Assert.True(offenders.Count == 0, "These use a word the glossary replaces:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// TLS in prose: SSL is left for SSL/TLS, the connection security that encrypts from the
      /// first byte, and for OpenSSL's name (review finding F301). The two pages that carried it
      /// in their titles are TLS certificates and TLS versions &amp; ciphers since the owner's
      /// decision of 12 October 2026, with the certificates the first lists and a port's TLS
      /// certificate box, and the old titles are their aliases.
      /// </summary>
      [Fact]
      public void NoEnglishText_SaysSslButThePagesTitlesAndOpenSsl()
      {
         var allowed = new Regex(@"OpenSSL|SSL/TLS", RegexOptions.IgnoreCase);
         List<string> offenders = EnglishTexts().Where(text => Regex.IsMatch(allowed.Replace(text.Replace("_", "", StringComparison.Ordinal), ""), @"\bSSL")).ToList();
         Assert.True(offenders.Count == 0, "TLS in prose; SSL is SSL/TLS and OpenSSL alone:\n" + string.Join("\n", offenders));
         var certs = hMailServer.ControlPanel.Services.NavigationMap.Find("certs");
         Assert.Equal("TLS certificates", certs.Title);
         Assert.Contains("SSL certificates", certs.Aliases);
         var tls = hMailServer.ControlPanel.Services.NavigationMap.Find("tls");
         Assert.Equal("TLS versions & ciphers", tls.Title);
         Assert.Contains("SSL/TLS", tls.Aliases);
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
