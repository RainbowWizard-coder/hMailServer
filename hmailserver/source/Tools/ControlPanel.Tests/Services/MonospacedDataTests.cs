// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Data - a cipher list, the key exchange groups, an LDAP search base and DN
   /// template, a log line, a script, a token, a header, a path - is set in the
   /// monospaced face, in a box or a block of its own at one size, AppFontSizeMono
   /// (Typography.Mono, 13), and inside a sentence as a run of its own (review
   /// findings F135, F304). The cipher list editors and the LDAP boxes were in the
   /// UI face, the transport encryption overview's cipher list broke inside a suite
   /// name, REST API keys' header was marked off by two spaces in its sentence, the
   /// stalled-mail guide's log lines were body text in SemiBold, and monospaced text
   /// was 12 pixels in the live log, the Label size in the diagnostics and 14 in the
   /// event script.
   /// </summary>
   public class MonospacedDataTests
   {
      /// <summary>
      /// A cipher list breaks after a colon and never at a hyphen: each hyphen is the
      /// ASCII one, which Cascadia Mono and Consolas both draw, held to the letter
      /// after it by a word joiner. The non-breaking hyphen (U+2011) it was is not in
      /// Consolas, so that where Cascadia Mono is missing every hyphen of the list came
      /// from the UI face, narrower than the letters beside it.
      /// </summary>
      [Fact]
      public void AColonSeparatedListBreaksOnlyAfterItsColons()
      {
         string shown = DataText.BreakAfterColons("ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384");

         Assert.Equal("ECDHE-\u2060ECDSA-\u2060AES128-\u2060GCM-\u2060SHA256:\u200BECDHE-\u2060RSA-\u2060AES256-\u2060GCM-\u2060SHA384", shown);
         Assert.DoesNotContain('\u2011', shown);
         Assert.Equal("", DataText.BreakAfterColons(null));
         Assert.Equal("AEAD-\u2060ONLY", DataText.BreakAfterColons("AEAD-ONLY"));
         // The characters the list is shown with are the ones it holds, the joiners aside.
         Assert.Equal("ECDHE-RSA:AES", DataText.BreakAfterColons("ECDHE-RSA:AES").Replace("\u2060", "", StringComparison.Ordinal).Replace("\u200B", "", StringComparison.Ordinal));
      }

      [Fact]
      public void ALongPathIsCutInItsMiddleAndAShortOneIsWhole()
      {
         const string Long = @"C:\Program Files\hMailServer\Bin\Configuration\Keys\Current\hMailServerApiKeys.ini";
         Assert.Equal(@"C:\Program Files\" + "\u2026" + @"\Current\hMailServerApiKeys.ini", DataText.MiddleOfPath(Long));
         Assert.Equal(@"C:\hMailServer\Bin\ApiKeys.ini", DataText.MiddleOfPath(@"C:\hMailServer\Bin\ApiKeys.ini"));
         Assert.Equal("", DataText.MiddleOfPath(null));
         string fourParts = @"C:\" + new string('a', 70) + @"\b\c.ini";
         Assert.Equal(fourParts, DataText.MiddleOfPath(fourParts));
      }

      [Fact]
      public void TheMonoSizeIsOneTokenSaidInCodeAndInXaml()
      {
         Assert.Equal(13, Typography.Mono);
         string tokens = Read("Views", "Scaffold", "Tokens.xaml");
         Assert.Contains("<sys:Double x:Key=\"AppFontSizeMono\">13</sys:Double>", tokens, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCipherListAndGroupEditorsAreMonospaced()
      {
         string settings = Read("Views", "ServerSettingsView.xaml.cs");
         foreach (string path in new[] { "TlsKeyExchangeGroups", "SslCipherList", "TlsCipherSuites13" })
            // After the Label, which build/generate-settings-index.ps1 reads beside the Path.
            Assert.Matches(new Regex(@"Path = """ + path + @""",\s*Label = L\(""[^""]*""\),\s*Monospaced = true,"), settings);

         string editor = WithheldRowTests.MemberText(settings, "protected TextBox TextEditor(string text, string placeholder = null)");
         Assert.True(editor != null, "ServerSettingsView has no TextEditor.");
         Assert.Contains("FontSize = Monospaced ? Typography.Mono : Typography.Body", editor, StringComparison.Ordinal);
         Assert.Contains("box.FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily);", editor, StringComparison.Ordinal);
      }

      [Fact]
      public void TheOverviewsCipherListIsARunOfItsOwn()
      {
         string overview = Read("Views", "TlsOverviewView.cs");
         Assert.Contains("DataTextBlock.Fill(cipherLine, L(\"Cipher list for TLS 1.2 and below: {0}.", overview, StringComparison.Ordinal);
         Assert.Contains("config.CipherList, breakAfterColons: true);", overview, StringComparison.Ordinal);

         string block = Read("Services", "DataTextBlock.cs");
         Assert.Contains("new Run(shown) { FontFamily = new FontFamily(Typography.MonoFontFamily) }", block, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDirectoryPagesNamesAndFiltersAreMonospaced()
      {
         string ldap = Read("Views", "LdapSettingsView.cs");
         Assert.Contains("foreach (Wpf.Ui.Controls.TextBox box in new[] { searchBase_, userDnTemplate_ })", ldap, StringComparison.Ordinal);
         Assert.Contains("LdapFilterField.ShowWhole(userSearchFilter_);", ldap, StringComparison.Ordinal);

         string field = Read("Views", "LdapFilterField.cs");
         Assert.Contains("box.FontSize = Typography.Mono;", field, StringComparison.Ordinal);
      }

      [Fact]
      public void TheBearerHeaderIsALineOfItsOwnAndTheStorePathIsCut()
      {
         string keys = Read("Views", "ApiKeysView.cs");
         Assert.DoesNotContain("Authorization: Bearer hmapi_...  to", keys, StringComparison.Ordinal);
         Assert.Contains("Text = \"Authorization: Bearer hmapi_...\"", keys, StringComparison.Ordinal);
         Assert.Contains("DataText.MiddleOfPath(path)", keys, StringComparison.Ordinal);
         Assert.Contains("storePath_.ToolTip = path;", keys, StringComparison.Ordinal);
      }

      /// <summary>
      /// The stalled-mail guide's log rows set what the log writes, and only that, in
      /// the monospaced face: each row is a sentence whose placeholders are the log's
      /// words, passed apart and never translated. The whole cell was monospaced, so
      /// that "with a large", "slow", "then silence" and "replies" read as text the
      /// log writes, and in Japanese the whole translated phrase did.
      /// </summary>
      [Fact]
      public void TheStalledMailGuidesLogRowsSetOnlyTheLogsWordsMonospaced()
      {
         string guide = Read("Views", "StalledMailView.cs");
         foreach (string row in new[]
         {
            "(L(\"{0} with a large {1}\"), new[] { \"SpamTestSpamAssassin\", \"Time:\" },",
            "(L(\"{0}, {1} or {2} slow\"), new[] { \"SpamTestDNSBlackLists\", \"SpamTestSURBL\", \"SpamTestSPF\" },",
            "(L(\"{0}, then silence\"), new[] { \"done spam-protection\" },",
            "(L(\"{0} slow\"), new[] { \"script/save\" },",
            "(L(\"Nothing between {0} and silence\"), new[] { \"354\" },",
            "(L(\"{0} replies\"), new[] { \"451 4.3.1\" },",
         })
            Assert.Contains(row, guide, StringComparison.Ordinal);

         int start = guide.IndexOf("private static Grid LogTable((string sentence, string[] logText, string cause)[] rows)", StringComparison.Ordinal);
         int end = guide.IndexOf("private static Grid Table(int count", StringComparison.Ordinal);
         Assert.True(start >= 0 && end > start, "StalledMailView has no LogTable before its Table.");
         string table = guide.Substring(start, end - start);
         Assert.Contains("DataTextBlock.FillPlaceholders(what, rows[i].sentence, rows[i].logText);", table, StringComparison.Ordinal);
         Assert.DoesNotContain("MonoFontFamily", table, StringComparison.Ordinal);

         string block = Read("Services", "DataTextBlock.cs");
         Assert.Contains("isData ? new Run(text) { FontFamily = new FontFamily(Typography.MonoFontFamily) } : new Run(text)", block, StringComparison.Ordinal);
      }

      /// <summary>A sentence cut at its placeholders, in the order its language puts them.</summary>
      [Fact]
      public void ASentenceIsCutAtItsPlaceholders()
      {
         Assert.Equal(new[] { ("SpamTestSpamAssassin", true), (" with a large ", false), ("Time:", true) },
            DataText.Pieces("{0} with a large {1}", new[] { "SpamTestSpamAssassin", "Time:" }));
         // The Japanese puts the field before the test.
         Assert.Equal(new[] { ("Time:", true), (" が大きい ", false), ("SpamTestSpamAssassin", true) },
            DataText.Pieces("{1} が大きい {0}", new[] { "SpamTestSpamAssassin", "Time:" }));
         Assert.Equal(new[] { ("A", true), (", ", false), ("B", true), (" or ", false), ("C", true), (" slow", false) },
            DataText.Pieces("{0}, {1} or {2} slow", new[] { "A", "B", "C" }));
         // A doubled brace is a brace, and a placeholder with no data stays as it is written.
         Assert.Equal(new[] { ("{0} ", false), ("x", true) }, DataText.Pieces("{{0}} {0}", new[] { "x" }));
         Assert.Equal(new[] { ("{1} and ", false), ("a", true) }, DataText.Pieces("{1} and {0}", new[] { "a" }));
         Assert.Empty(DataText.Pieces(null, new[] { "a" }));
      }

      /// <summary>
      /// Every box or block of monospaced text is the one size, read off every source
      /// file of the Control Panel: a site that sets the face sets Typography.Mono
      /// (AppFontSizeMono in XAML) beside it, a run inside a sentence sets no size and
      /// takes the sentence's, and the three one-time code boxes, set large for six
      /// digits typed off a phone, are the only exception. A site that sets no size
      /// fails, where the test this replaces read seven named files and passed over
      /// one: the TOTP setup key in its three places, the generated password, the
      /// quarantine's headers and a setting's name were 14 pixels beside the 13 of
      /// the API key's token, and the help pane's code blocks were 12 (review finding
      /// F304).
      /// </summary>
      [Fact]
      public void EveryMonospacedBoxAndBlockIsTheMonoSize()
      {
         var codeBoxes = new HashSet<string>(StringComparer.Ordinal)
         {
            "TotpPromptDialog.cs code_", "TotpSetupDialog.cs code_", "AdministratorTwoFactorDialog.cs code_",
         };
         var codeBoxesSeen = new HashSet<string>(StringComparer.Ordinal);
         var problems = new List<string>();
         int sites = 0, runs = 0;
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");

         IEnumerable<string> code = new[] { "Views", "Services" }
            .SelectMany(folder => Directory.GetFiles(Path.Join(controlPanel, folder), "*.cs", SearchOption.AllDirectories))
            .Where(file => !file.EndsWith(".g.cs", StringComparison.OrdinalIgnoreCase));
         foreach (string file in code)
         {
            string name = Path.GetFileName(file);
            string source = File.ReadAllText(file);
            if (name != "Typography.cs")
               Assert.False(Regex.IsMatch(source, @"""[^""\n]*(?:Cascadia|Consolas)"), name + " names a monospaced face of its own; use Typography.MonoFontFamily.");

            // A face kept in a field: "static readonly FontFamily Mono = new(Typography.MonoFontFamily);".
            string face = @"(?:new\s*(?:[\w.]*FontFamily)?\(\s*Typography\.MonoFontFamily\s*\)"
               + string.Concat(Regex.Matches(source, @"\bFontFamily\s+(\w+)\s*=\s*new\s*(?:[\w.]*FontFamily)?\(\s*Typography\.MonoFontFamily\s*\)")
                  .Select(alias => @"|\b" + alias.Groups[1].Value + @"\b"))
               + ")";

            // In an object initialiser: "{ ..., FontFamily = <the face>, ... }".
            foreach (Match member in Regex.Matches(source, @"(?<![\w.])FontFamily\s*=\s*" + face))
            {
               (string type, string variable, string body) = Initialiser(source, member.Index);
               string where = name + " " + (variable ?? type ?? "?");
               if (type == null)
               {
                  problems.Add(where + ": a monospaced face set outside an object initialiser this test can read");
                  continue;
               }
               if (type == "Run")
               {
                  runs++;
                  if (Regex.IsMatch(body, @"(?<![\w.])FontSize\s*="))
                     problems.Add(where + ": a run inside a sentence sets a size of its own; it takes the sentence's");
                  continue;
               }

               sites++;
               string key = name + " " + variable;
               if (codeBoxes.Contains(key))
               {
                  codeBoxesSeen.Add(key);
                  if (!Regex.IsMatch(body, @"(?<![\w.])FontSize\s*=\s*(?:28\b|Typography\.Title\b)"))
                     problems.Add(where + ": a one-time code box that is not the 28 pixels the others are");
                  continue;
               }
               if (!Regex.IsMatch(body, @"(?<![\w.])FontSize\s*=[^,;}]*\bTypography\.Mono\b") && !(variable != null && SizedLater(source, variable, member.Index)))
                  problems.Add(where + ": monospaced at a size other than Typography.Mono, or at none");
            }

            // In a statement: "x.FontFamily = <the face>;" (or a choice that may be it).
            foreach (Match statement in Regex.Matches(source, @"\b(\w+)\.FontFamily\s*=\s*[^;]*?" + face + "[^;]*;"))
            {
               sites++;
               string variable = statement.Groups[1].Value;
               if (!SizedLater(source, variable, statement.Index) && !SizedInItsInitialiser(source, variable, statement.Index))
                  problems.Add(name + " " + variable + ": monospaced at a size other than Typography.Mono, or at none");
            }
         }

         // XAML: an element in the face carries the size beside it.
         IEnumerable<string> markup = Directory.GetFiles(Path.Join(controlPanel, "Views"), "*.xaml", SearchOption.AllDirectories)
            .Append(Path.Join(controlPanel, "App.xaml"));
         foreach (string file in markup)
         {
            foreach (Match element in Regex.Matches(File.ReadAllText(file), @"<[\w:.]+\b[^<>]*?FontFamily=""[^""]*(?:Cascadia|Consolas|MonoFontFamily)[^""]*""[^<>]*>", RegexOptions.Singleline))
            {
               sites++;
               if (!Regex.IsMatch(element.Value, @"FontSize=""\{(?:Static|Dynamic)Resource AppFontSizeMono\}"""))
                  problems.Add(Path.GetFileName(file) + ": " + element.Value.Split('\n')[0].Trim() + " is not at AppFontSizeMono");
            }
         }

         Assert.True(sites >= 25, "Fewer monospaced boxes and blocks than the Control Panel has: " + sites);
         Assert.True(runs >= 3, "Fewer monospaced runs than the Control Panel has: " + runs);
         Assert.True(codeBoxesSeen.SetEquals(codeBoxes), "A one-time code box named as the exception was not found: " + string.Join(", ", codeBoxes.Except(codeBoxesSeen)));
         Assert.True(problems.Count == 0, "Monospaced text must be one size, Typography.Mono:\n" + string.Join("\n", problems));
      }

      /// <summary>
      /// The object initialiser that holds the member at <paramref name="at"/>: the type
      /// it constructs (its last name), the variable or field it is assigned to (null for
      /// an argument) and its body - or a null type when the member is not in one.
      /// </summary>
      private static (string Type, string Variable, string Body) Initialiser(string source, int at)
      {
         int depth = 0, open = -1;
         for (int i = at - 1; i >= 0; i--)
         {
            if (source[i] == '}')
               depth++;
            else if (source[i] == '{' && depth-- == 0)
            {
               open = i;
               break;
            }
         }
         if (open < 0)
            return (null, null, null);

         int close = open;
         for (int i = open, level = 0; i < source.Length; i++)
         {
            if (source[i] == '{')
               level++;
            else if (source[i] == '}' && --level == 0)
            {
               close = i;
               break;
            }
         }
         string body = source.Substring(open, close - open + 1);

         // What comes before the brace: "new Type(arguments)", "new Type" or "new()".
         int end = open - 1;
         while (end >= 0 && char.IsWhiteSpace(source[end]))
            end--;
         if (end >= 0 && source[end] == ')')
         {
            for (int level = 0; end >= 0; end--)
            {
               if (source[end] == ')')
                  level++;
               else if (source[end] == '(' && --level == 0)
                  break;
            }
            end--;
         }
         Match constructed = Regex.Match(source.Substring(0, end + 1), @"\bnew\s*([\w.]*)\s*$");
         if (!constructed.Success)
            return (null, null, null);
         string type = constructed.Groups[1].Value.Length == 0 ? "" : constructed.Groups[1].Value.Split('.')[^1];
         Match assigned = Regex.Match(source.Substring(0, constructed.Index), @"(\w+)\s*=\s*$");
         return (type, assigned.Success ? assigned.Groups[1].Value : null, body);
      }

      /// <summary>Whether <paramref name="variable"/> is given the Mono size near <paramref name="at"/>, in a statement.</summary>
      private static bool SizedLater(string source, string variable, int at)
      {
         int from = Math.Max(0, at - 2000);
         string near = source.Substring(from, Math.Min(source.Length, at + 2000) - from);
         return Regex.IsMatch(near, @"\b" + variable + @"\.FontSize\s*=[^;]*\bTypography\.Mono\b")
            || near.Contains(variable + ".SetResourceReference(FontSizeProperty, \"AppFontSizeMono\")", StringComparison.Ordinal);
      }

      /// <summary>Whether the initialiser <paramref name="variable"/> was made with, before <paramref name="at"/>, gives it the Mono size.</summary>
      private static bool SizedInItsInitialiser(string source, string variable, int at)
      {
         int from = Math.Max(0, at - 2000);
         string before = source.Substring(from, at - from);
         Match made = Regex.Matches(before, @"\b" + variable + @"\s*=\s*new\b[^;{]*\{([^{}]*)\}").LastOrDefault();
         return made != null && Regex.IsMatch(made.Groups[1].Value, @"(?<![\w.])FontSize\s*=[^,;}]*\bTypography\.Mono\b");
      }

      [Fact]
      public void TheLiveLogAndTheMessageViewerAreTheMonoSize()
      {
         Assert.Contains("FontFamily=\"Cascadia Mono, Consolas\" FontSize=\"{DynamicResource AppFontSizeMono}\"", Read("Views", "LogsView.xaml"), StringComparison.Ordinal);
         Assert.Contains("content.SetResourceReference(FontSizeProperty, \"AppFontSizeMono\");", Read("Views", "MessageViewerDialog.cs"), StringComparison.Ordinal);
      }

      private static string Read(params string[] parts)
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         return File.ReadAllText(Path.Join(controlPanel, Path.Join(parts)));
      }
   }
}
