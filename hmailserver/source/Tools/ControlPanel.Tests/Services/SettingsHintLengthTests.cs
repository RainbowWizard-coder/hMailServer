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
   /// The owner's decision 11 (9 October 2026): a hint is two sentences on the page - what
   /// the setting does, and its safe value or what follows from it - and the rest is in the
   /// help; an INI key name is a small tertiary caption, never a sentence of the hint.
   /// SpamAndVirusHintLengthTests held it for the Anti-spam and Anti-virus settings pages
   /// (review finding F147); the final check of Tier 2 found 56 hints of three to five
   /// sentences on the other settings pages, more on Backup & restore, a distribution
   /// list's dialog, Directory authentication and Directory synchronisation, and the
   /// directory page ending each hint with "Stored as [LDAP] key." (g2 defect 3, g3
   /// defect 11). Each keeps two of its sentences, in every catalogue the same two; the
   /// others are in docs/SettingsReasons.md, under the page and the setting's caption, which
   /// the help opens for the pages no other document is about.
   /// </summary>
   public class SettingsHintLengthTests
   {
      private const string Literal = @"""((?:[^""\\]|\\.)*)""";
      // SpamAndVirusHintLengthTests' rule, with "e.g." and "i.e." ending no sentence.
      private static readonly Regex SentenceEnd = new Regex(@"(?<=[.!?])(?<!e\.g\.)(?<!i\.e\.)\s+(?=[A-Z0-9""'(~])");

      public static TheoryData<string, string, int> Readers => new()
      {
         // file, the pattern whose first group (or last) is a hint, the fewest hints it must read
         { "ServerSettingsView.xaml.cs", @"\bBlurb\s*=\s*L\(" + Literal + @"\)", 200 },
         { "FeatureSettingsView.xaml.cs", @"\b(?:Blurb|Hint)\s*=\s*L\(" + Literal + @"\)", 80 },
         { "BackupView.xaml", @"Hint=""\{loc:L '((?:[^'\\]|\\.)*)'\}""", 6 },
         { "DistributionListDialog.cs", @"WithHint\(L\(" + Literal + @"\)", 4 },
         { "LdapSettingsView.cs", @"Labelled(?:Box|Combo)_\(L\(" + Literal + @"\),\s*\w+,\s*""[\w-]+"",\s*(?://[^\n]*\n\s*)*[LF]\(" + Literal, 10 },
         { "LdapSettingsView.cs", @"LabelledCheck_\(\w+,\s*L\(" + Literal + @"\),\s*""[\w-]+"",\s*[LF]\(" + Literal, 4 },
         { "DirectorySyncView.cs", @"LabelledBox_\(L\(" + Literal + @"\),\s*\w+,\s*""[\w-]+"",\s*(?://[^\n]*\n\s*)*[LF]\(" + Literal, 5 },
      };

      [Theory]
      [MemberData(nameof(Readers))]
      public void EveryHint_IsTwoSentencesAtMost(string file, string pattern, int fewest)
      {
         var hints = Regex.Matches(View(file), pattern).Select(m => m.Groups[m.Groups.Count - 1].Value).ToList();
         Assert.True(hints.Count >= fewest, "Only " + hints.Count + " hints were read from " + file + "; the pattern no longer reads them.");
         var offenders = hints.Where(hint => Sentences(hint) > 2).Select(hint => Sentences(hint) + " sentences: " + hint).ToList();
         Assert.True(offenders.Count == 0, "These hints run past two sentences; keep what the setting does and its safe value, and move the rest to docs/SettingsReasons.md:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// A hint made of two texts, Sentences(L(a), L(b)), is the two together: the Updates
      /// page's Unattended window, the PROXY and XCLIENT upstreams, the smart host's fixed
      /// token, the account-number pattern and SpamAssassin's profile ran to three and four
      /// though each of their texts passed alone.
      /// </summary>
      [Theory]
      [InlineData("ServerSettingsView.xaml.cs", 4)]
      [InlineData("FeatureSettingsView.xaml.cs", 4)]
      public void EveryHintOfTwoTexts_IsTwoSentencesAtMost(string file, int fewest)
      {
         var hints = Regex.Matches(View(file), @"\b(?:Blurb|Hint)\s*=\s*Sentences\(L\(" + Literal + @"\),\s*L\(" + Literal + @"\)\)")
            .Select(m => m.Groups[1].Value + " " + m.Groups[2].Value).ToList();
         Assert.True(hints.Count >= fewest, "Only " + hints.Count + " hints of two texts were read from " + file + ".");
         var offenders = hints.Where(hint => Sentences(hint) > 2).Select(hint => Sentences(hint) + " sentences: " + hint).ToList();
         Assert.True(offenders.Count == 0, "These hints run past two sentences:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// The notes a setting's claim pins (Services/SettingClaims) are hints as well: the
      /// settings pages show each under its setting - the bounce settings, syslog, the
      /// worker thread priority, the caches' sizes, the OpenTelemetry endpoints - through
      /// L(SettingClaims.NoteFor(key)), which the patterns above, reading literals, do not
      /// see, and seven of them ran to three sentences after the final check's fixes.
      /// </summary>
      [Fact]
      public void EveryClaimsNote_IsTwoSentencesAtMost()
      {
         string claims = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "SettingClaims.cs"));
         var notes = Regex.Matches(claims, @"(?:new SettingClaim\(""[^""]+"",\s*ClaimKind\.\w+,\s*|public static readonly string \w+ = )N\(\s*" + Literal + @"\)")
            .Select(m => m.Groups[1].Value).ToList();
         Assert.True(notes.Count >= 15, "Only " + notes.Count + " claim notes were read from SettingClaims.cs; the pattern no longer reads them.");
         var offenders = notes.Where(note => Sentences(note) > 2).Select(note => Sentences(note) + " sentences: " + note).ToList();
         Assert.True(offenders.Count == 0, "These claim notes run past two sentences; keep what the setting does and its safe value, and move the rest to docs/SettingsReasons.md:\n" + string.Join("\n", offenders));
      }

      [Theory]
      [InlineData("Where the search starts, e.g. DC=example,DC=local. Required in search mode.", 2)]
      [InlineData("Off by default. Turn on only on a domain-joined host. A third.", 3)]
      public void TheSentenceCount_ReadsAnAbbreviationAsNoEnd(string hint, int expected)
         => Assert.Equal(expected, Sentences(hint));

      /// <summary>The [LDAP] key is each row's small caption under its editor, not a sentence of its hint.</summary>
      [Fact]
      public void DirectoryAuthentication_SaysEachKeyAsASmallCaption()
      {
         string page = View("LdapSettingsView.cs");
         Assert.DoesNotContain("StoredAs_(", page, StringComparison.Ordinal);
         string keyed = WithheldRowTests.MemberText(page, "private static FieldRow Keyed_(FieldRow row, string key)");
         Assert.True(keyed != null, "LdapSettingsView.Keyed_ is gone.");
         Assert.Contains("row.Applies = F(\"Stored as [LDAP] {0}.\", key);", keyed, StringComparison.Ordinal);
         Assert.Equal(13, Regex.Matches(page, @"\bKeyed_\(Labelled").Count);
         Assert.Contains("Applies = F(\"Stored as [LDAP] {0}.\", \"ServicePassword\")", page, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("Protocols", "a mailbox sitting at 95% is not warned about again on every delivery")]
      [InlineData("TLS versions & ciphers", "which is why the shipped list is shown rather than a blank")]
      [InlineData("Transport security", "The list is not an option of the feature - it is the feature.")]
      [InlineData("Administrative access", "Active Directory accounts are exempt")]
      [InlineData("Directory authentication (LDAP)", "let one wrong password trigger a directory lockout policy")]
      [InlineData("Server limits & expert settings", "a proxy that does not send one will have its connections dropped")]
      [InlineData("Advanced", "is a security decision rather than a convenience")]
      [InlineData("Backup & restore", "so a failed backup never deletes anything")]
      [InlineData("Delivery of e-mail", "says so in the error log once an hour")]
      [InlineData("Logging", "this sends a copy, it does not move anything")]
      [InlineData("API & monitoring", "rather than a second tally")]
      [InlineData("Performance", "worse than no box at all")]
      public void TheSentencesTheHintsLost_AreUnderThePagesHeading(string heading, string reason)
      {
         string text = File.ReadAllText(Path.Combine(Docs(), "SettingsReasons.md")).Replace("\r\n", "\n");
         int at = text.IndexOf("\n## " + heading + "\n", StringComparison.Ordinal);
         Assert.True(at >= 0, "SettingsReasons.md has no heading " + heading);
         int next = text.IndexOf("\n## ", at + 1, StringComparison.Ordinal);
         string section = Regex.Replace(next < 0 ? text.Substring(at) : text.Substring(at, next - at), @"\s+", " ");
         Assert.Contains(reason, section, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("delivery", "Delivery of e-mail")]
      [InlineData("tls", "TLS versions & ciphers")]
      [InlineData("security", "Transport security")]
      [InlineData("webservices", "Web services & autoconfiguration")]
      [InlineData("dns", "DNS resolver")]
      [InlineData("performance", "Performance")]
      [InlineData("advanced", "Advanced")]
      [InlineData("ldap", "Directory authentication (LDAP)")]
      [InlineData("directorysync", "Directory synchronisation")]
      public void ThePagesWithNoOtherGuide_OpenTheirSection(string key, string heading)
         => Assert.Contains("[\"" + key + "\"] = new HelpTarget(\"SettingsReasons.md\", \"" + heading + "\"),",
            ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "HelpMap.cs")), StringComparison.Ordinal);

      private static int Sentences(string hint) => SentenceEnd.Split(hint.Replace("\\\"", "\"").Trim()).Count(part => part.Length > 0);

      private static string View(string file) => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file));

      private static string Docs()
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
