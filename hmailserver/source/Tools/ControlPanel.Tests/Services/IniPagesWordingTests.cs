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
   /// How the pages that edit hMailServer.ini name the file, their settings and their
   /// examples (review findings F136, F137 and F138).
   ///
   /// F136: subtitles and card blurbs said "hMailServer.INI" while the line under them
   /// said "Editing C:\...\hMailServer.ini" - the file's own name, as the server makes it
   /// (IniFileSettings) and the installer writes it. F137: the Directory authentication
   /// page's captions carried their keys in brackets, "Port (Port)", "Directory server
   /// host name (Server)" - in Japanese and Chinese with the Alt key after the bracket -
   /// and its state card listed keys, "the section is also missing: Server, SearchBase
   /// (or UserDnTemplate, ...)"; Directory synchronisation, which edits the same section,
   /// named none. The key is now the last sentence of each field's hint, "Stored as
   /// [LDAP] Server.", the small line the owner's decision on hints allows on an expert
   /// page, and what is missing is named by the fields' captions. F138: in the light
   /// theme an example placeholder reads as a value - the host name box showed
   /// "dc01.example.local" under the notice that the host name was missing - so an
   /// example is said to be one ("e.g. dc01.example.local"), and a box whose emptiness
   /// stands for a default says the default in its hint as well as in grey.
   /// </summary>
   public class IniPagesWordingTests
   {
      private static readonly Regex StringLiteral = new Regex(@"@?""(?:[^""\\]|\\.)*""");

      [Fact]
      public void NoTextOrTranslation_SpellsTheFileHMailServerINI()
      {
         string folder = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var offenders = new List<string>();
         foreach (string path in Directory.EnumerateFiles(folder, "Strings*.resx"))
         {
            foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            {
               string key = (string)data.Attribute("name");
               string value = (string)data.Element("value") ?? "";
               if (key.Contains("hMailServer.INI", StringComparison.Ordinal) || value.Contains("hMailServer.INI", StringComparison.Ordinal))
                  offenders.Add(Path.GetFileName(path) + ": " + key);
            }
         }

         Assert.True(offenders.Count == 0, "These spell the file hMailServer.INI; its name is hMailServer.ini:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void NoStringInTheSource_SpellsTheFileHMailServerINI()
      {
         var offenders = new List<string>();
         foreach ((string relative, string source) in ConfirmationPatternTests.SourceFiles())
         {
            string[] lines = source.Split('\n');
            for (int i = 0; i < lines.Length; i++)
            {
               string line = lines[i].TrimStart();
               if (line.StartsWith("//", StringComparison.Ordinal) || line.StartsWith('*'))
                  continue;
               if (StringLiteral.Matches(line).Any(m => m.Value.Contains("hMailServer.INI", StringComparison.Ordinal)))
                  offenders.Add(relative + ":" + (i + 1) + ": " + line.Trim());
            }
         }

         Assert.True(offenders.Count == 0, "These strings spell the file hMailServer.INI:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void DirectoryAuthentication_CaptionsNoKey_AndEachHintEndsWithTheKeyItIsStoredAs()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "LdapSettingsView.cs"));

         var written = Regex.Matches(page, @"\bIniWrite_\(\s*""(\w+)""").Select(m => m.Groups[1].Value).ToHashSet(StringComparer.Ordinal);
         Assert.True(written.Count >= 14, "The page writes only " + written.Count + " keys; the pattern no longer reads them.");

         // No caption carries a key in brackets.
         var captions = Regex.Matches(page, @"\b(?:LabelledBox_|LabelledCombo_)\(L\(""([^""]*)""\)|\bLabelledCheck_\(\w+,\s*L\(""([^""]*)""\)|\bLabel = L\(""([^""]*)""\)")
            .Select(m => m.Groups[1].Value + m.Groups[2].Value + m.Groups[3].Value).ToList();
         Assert.True(captions.Count >= 15, "Only " + captions.Count + " captions were read.");
         foreach (string caption in captions)
         {
            foreach (string key in written)
               Assert.DoesNotContain("(" + key + ")", caption, StringComparison.Ordinal);
         }

         // Each key the page writes is named once, as the last sentence of its field's hint.
         var stored = Regex.Matches(page, @"\), ""(\w+)""\)").Select(m => m.Groups[1].Value).ToList();
         foreach (string key in written)
            Assert.True(stored.Count(k => k == key) == 1, key + " is not the key of exactly one field's StoredAs_ hint.");
         Assert.Contains("=> Sentences(hint, F(\"Stored as [LDAP] {0}.\", key));", page, StringComparison.Ordinal);
      }

      [Fact]
      public void DirectoryAuthentication_NamesWhatIsMissing_ByTheFieldsCaptions()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "LdapSettingsView.cs"));

         Assert.DoesNotContain("F(\"Before enabling, the section is also missing", page, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Server\")", page, StringComparison.Ordinal);
         foreach (string method in new[] { "MissingBeforeEnabling_", "MissingWhileEnabled_" })
         {
            int at = page.IndexOf("private static string " + method, StringComparison.Ordinal);
            Assert.True(at > 0, method + " is gone.");
            string body = page.Substring(at, page.IndexOf(";", at, StringComparison.Ordinal) - at);
            // the captions as the page shows them, without their Alt keys: Directory server
            // _host name, Search bas_e, User _DN template
            Assert.Contains("Directory server host name", body, StringComparison.Ordinal);
            Assert.Contains("Search base", body, StringComparison.Ordinal);
            Assert.Equal(3, Regex.Matches(body, @"\bL\(""").Count);
         }
      }

      [Fact]
      public void TheDirectoryPages_SayAnExampleIsOne_AndADefaultInTheHint()
      {
         string ldap = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "LdapSettingsView.cs"));
         string sync = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         string features = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         string helper = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ExamplePlaceholder.cs"));

         Assert.Contains("public static string Of(string example) => F(\"e.g. {0}\", example);", helper, StringComparison.Ordinal);

         // Directory authentication: every placeholder is an example said to be one, the
         // default filter (named in its hint), the port's "Automatic - ..." or the timeout's
         // default, 10, which its hint's first words state.
         foreach (Match placeholder in Regex.Matches(ldap, @"^\s*placeholder:\s*([^)\r\n]+\)?)", RegexOptions.Multiline))
         {
            string value = placeholder.Groups[1].Value;
            Assert.True(value.StartsWith("ExamplePlaceholder.Of(", StringComparison.Ordinal)
                        || value.StartsWith("DefaultUserSearchFilter", StringComparison.Ordinal)
                        || value.StartsWith("L(\"Automatic", StringComparison.Ordinal)
                        || value.StartsWith("\"10\"", StringComparison.Ordinal),
               "A placeholder on the Directory authentication page reads as a value: " + value);
         }
         Assert.Contains("ExamplePlaceholder.Of(\"dc01.example.local\")", ldap, StringComparison.Ordinal);
         Assert.Contains("ExamplePlaceholder.Of(\"DC=example,DC=local\")", ldap, StringComparison.Ordinal);
         Assert.Contains("ExamplePlaceholder.Of(\"svc-mail@example.local\")", ldap, StringComparison.Ordinal);
         Assert.Contains("L(\"Default 10. The server reads values below 1 as 10", ldap, StringComparison.Ordinal);

         // Authentication: the OAuth2 card's three URLs - the token issuer, the provider's
         // published keys and its introspection endpoint - are examples.
         Assert.Contains("Placeholder = ExamplePlaceholder.Of(\"https://login.microsoftonline.com/<tenant>/v2.0\")", features, StringComparison.Ordinal);
         Assert.Contains("Placeholder = ExamplePlaceholder.Of(\"https://login.microsoftonline.com/<tenant>/discovery/v2.0/keys\")", features, StringComparison.Ordinal);
         Assert.Contains("Placeholder = ExamplePlaceholder.Of(\"https://idp.example.com/oauth2/introspect\")", features, StringComparison.Ordinal);

         // Directory synchronisation: a box whose placeholder is the default its emptiness
         // stands for names that default in its hint too.
         var boxes = Regex.Matches(sync, @"LabelledBox_\(L\(""[^""]*""\), \w+, ""[\w-]+"",\s*(?://[^\n]*\n\s*)*L\(""((?:[^""\\]|\\.)*)""\),\s*placeholder: ""([^""]*)""\)");
         Assert.True(boxes.Count >= 5, "Only " + boxes.Count + " of Directory synchronisation's boxes were read.");
         foreach (Match box in boxes)
         {
            string hint = box.Groups[1].Value;
            string fallback = box.Groups[2].Value;
            // the hint groups a number's thousands, "5,000", and the box takes it as typed, "5000"
            Assert.True(Regex.Replace(hint, @"(?<=\d),(?=\d{3})", "").Contains(fallback, StringComparison.Ordinal),
               "The hint of the box whose empty value is " + fallback + " does not say so: " + hint);
         }
      }
   }
}
