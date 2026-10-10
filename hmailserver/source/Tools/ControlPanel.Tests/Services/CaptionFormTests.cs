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
   /// Every field caption says its unit, its range and what an edge value means in one form,
   /// "&lt;what&gt; (&lt;unit&gt;, &lt;range&gt;; 0 = &lt;meaning&gt;)" - "Ban the sending address for (minutes,
   /// up to 525,600; 0 = no ban)" (review findings F148 and F300). The Anti-spam and Anti-virus
   /// pages alone spelt them six ways - "(KB, 0 = unlimited)", "(0 never refuses)", "Record
   /// expiration interval, minutes", "(0-100)", "Minutes the sending address is banned (0 = no
   /// ban; up to 525600)", "Max" beside "Maximum" in one card - and the settings pages had twelve
   /// ways to say what 0 means, with "(s)", "in minutes", "N days" and numbers without their
   /// thousands separated.
   ///
   /// The form, past CaptionWordingTests' rule for what a bracket may hold: "no limit", never
   /// "unlimited"; "Maximum", never "Max" or "max"; "off", never "disabled", for what a value
   /// means; the unit in the brackets, never "in seconds", ", minutes" or "(s)" around them; no "N"
   /// for the number; a range "a to b", never "a-b"; a number of four digits or more with its
   /// thousands grouped (a port's or an RFC's number is a name, not a quantity); "0 = ...", never
   /// "0 never refuses", and "empty = ...", never "empty asks the domain's MX hosts" or "leave it
   /// empty for ..."; the unit and the range before what values mean, after "; "; "comma
   /// separated"; and in every catalogue what a number means as "&lt;n&gt; =", as the English writes it.
   /// </summary>
   public class CaptionFormTests
   {
      private static readonly Regex CaptionInXaml = new Regex(
         @"<scaffold:FieldRow\b[^>]*?\bLabel=""\{loc:L '((?:[^'\\]|\\.)*)'\}""", RegexOptions.Singleline);

      // Its bracket is a DNSSEC trust anchor's own syntax; t31c rewrites it (review finding F311).
      private static readonly string[] NotYet = { "Trust anchor override (" };

      private static readonly (string Name, Func<string, string, string, bool> Breaks)[] Checks =
      {
         ("unlimited", (caption, before, inner) => Regex.IsMatch(caption, @"\bunlimited\b", RegexOptions.IgnoreCase)),
         // In either case: the Performance page's "Domain cache max size" passed a rule that read
         // "Max" alone. MTA-STS's policy field is named by its own name, max_age (RFC 8461).
         ("Max", (caption, before, inner) => Regex.IsMatch(caption.Replace("Policy max age", "", StringComparison.Ordinal), @"\bmax\b", RegexOptions.IgnoreCase)),
         ("empty not written empty =", (caption, before, inner) => inner != null && inner.Split("; ").Any(EmptyInProse)),
         ("disabled", (caption, before, inner) => Regex.IsMatch(caption, @"= disabled\b")),
         ("unit outside the brackets", (caption, before, inner) =>
            Regex.IsMatch(before, @"(?:,|\bin) (?:seconds|minutes|hours|days|milliseconds|KB|MB|bytes)\b(?! to\b)") || caption.Contains("(s)", StringComparison.Ordinal)),
         ("N for the number", (caption, before, inner) => Regex.IsMatch(caption, @"\bN\b(?! *=)")),
         ("a-b range", (caption, before, inner) => inner != null && Regex.IsMatch(inner, @"(?<![\w:])\d+-\d+(?![\w:])")),
         ("ungrouped thousands", (caption, before, inner) => inner != null
            && !Regex.IsMatch(caption, @"\b(?:port|RFC)\b", RegexOptions.IgnoreCase)
            && Regex.IsMatch(inner, @"(?<![\w,.])\d{4,}(?![\w,])")),
         ("0 not written 0 =", (caption, before, inner) => inner != null && Regex.IsMatch(inner, @"(?:^|; |, )0 (?!=|to\b|or\b)")),
         ("a meaning after a comma", (caption, before, inner) => inner != null && Regex.IsMatch(inner, @"^[^=;]*, (?:-?\d+|empty|\*) = ")),
         ("comma-separated", (caption, before, inner) => caption.Contains("comma-separated", StringComparison.Ordinal)),
         ("what values mean before the unit or range", (caption, before, inner) => inner != null && MeaningComesFirst(inner)),
      };

      /// <summary>The checks a caption breaks, none when it keeps the form.</summary>
      internal static List<string> Problems(string caption)
      {
         var found = new List<string>();
         if (NotYet.Any(start => caption.StartsWith(start, StringComparison.Ordinal)))
            return found;

         string text = RemoveFirst(caption, "_");
         Match last = Regex.Match(text, @"\(([^()]*)\)\s*$");
         string before = last.Success ? text[..last.Index] : text;
         string inner = last.Success ? last.Groups[1].Value : null;
         string rule = CaptionWordingTests.Problem(caption);
         if (rule != null)
            found.Add(rule);
         foreach ((string name, Func<string, string, string, bool> breaks) in Checks)
         {
            if (breaks(text, before, inner))
               found.Add(name);
         }

         return found;
      }

      /// <summary>
      /// A part of a bracket that says what an empty box means in prose - "empty asks the domain's
      /// MX hosts", "leave it empty for one named after today's date", "empty: the one it was in" -
      /// where the form is "empty = the domain's MX hosts"; the password boxes' shared "leave empty
      /// to keep current" says what to do, and is one text everywhere.
      /// </summary>
      internal static bool EmptyInProse(string part) =>
         Regex.IsMatch(part.Replace("_", ""), @"^(?:leave (?:it )?|left )?empty\b")
         && !part.Replace("_", "").StartsWith("empty = ", StringComparison.Ordinal)
         && part != "leave empty to keep current";

      private static bool MeaningComesFirst(string inner)
      {
         bool meaning = false;
         foreach (string part in inner.Split("; "))
         {
            if (CaptionWordingTests.ValuePart.IsMatch(part))
               meaning = true;
            else if (meaning && CaptionWordingTests.UnitPart.IsMatch(part))
               return true;
         }

         return false;
      }

      private static string RemoveFirst(string text, string what)
      {
         int at = text.IndexOf(what, StringComparison.Ordinal);
         return at < 0 ? text : text.Remove(at, what.Length);
      }

      [Theory]
      [InlineData("Ban the sending address for (minutes, up to 525,600; 0 = no ban)")]
      [InlineData("Largest message to spam-scan (KB; 0 = no limit)")]
      [InlineData("Score at which suspicious counts as malicious (0 to 100)")]
      [InlineData("PBKDF2 iterations for new hashes (10,000 to 10,000,000; 0 = the built-in 210,000)")]
      [InlineData("Forward proxy for this server's web requests (host:port; empty = direct)")]
      [InlineData("Daily backup t_ime (24-hour HH:MM; empty = off)")]
      [InlineData("Digest _hour (0 to 23)")]
      [InlineData("UDP port (1 to 65535)")]
      [InlineData("Server-side session cache size (0 = OpenSSL's default of 20,480, -1 = no cache)")]
      [InlineData("Maximum characters per log line (at least 100)")]
      [InlineData("Policy max age (seconds, 86,400 to 31,557,600)")]
      [InlineData("Domain cache maximum size (KB, resets at service restart)")]
      [InlineData("New _password (leave empty to keep current)")]
      [InlineData("Into this folder (empt_y = the one it was in)")]
      public void ACaption_InTheOneForm_KeepsIt(string caption)
      {
         Assert.Empty(Problems(caption));
      }

      [Theory]
      [InlineData("Max message size to spam-scan (KB, 0 = unlimited)", "unlimited")]
      [InlineData("Max message size to spam-scan (KB, 0 = unlimited)", "Max")]
      [InlineData("Max message size to spam-scan (KB, 0 = unlimited)", "a meaning after a comma")]
      [InlineData("Refuse with 550 after this many retries before the window opens (0 never refuses)", "0 not written 0 =")]
      [InlineData("Record expiration interval, minutes", "unit outside the brackets")]
      [InlineData("SMTP server minimum timeout (s)", "unit outside the brackets")]
      [InlineData("Backup interva_l in minutes (0 = off)", "unit outside the brackets")]
      [InlineData("Minutes the sending address is banned (0 = no ban; up to 525600)", "what values mean before the unit or range")]
      [InlineData("Minutes the sending address is banned (0 = no ban; up to 525600)", "ungrouped thousands")]
      [InlineData("Score at which suspicious counts as malicious (0-100)", "a-b range")]
      [InlineData("Delete logs older than N days (0 = keep everything)", "N for the number")]
      [InlineData("Port (0 = disabled)", "disabled")]
      [InlineData("Only archive mail for these domains (comma-separated; empty = every message)", "comma-separated")]
      [InlineData("Domain cache max size (KB, resets at service restart)", "Max")]
      [InlineData("The server to ask (empty asks the domain's MX hosts)", "empty not written empty =")]
      [InlineData("Wiki the \"Open on the wiki\" link points at (empty hides the link)", "empty not written empty =")]
      [InlineData("Selector for the new key (leave it empty for one named after today's date)", "empty not written empty =")]
      [InlineData("Into this folder (empt_y: the one it was in)", "empty not written empty =")]
      public void ACaption_OutOfTheForm_BreaksIt(string caption, string check)
      {
         Assert.Contains(check, Problems(caption));
      }

      [Fact]
      public void EveryCaption_SaysItsUnitRangeAndEdgeValuesInTheOneForm()
      {
         var offenders = new List<string>();
         int read = 0;
         foreach ((string relative, string source) in ConfirmationPatternTests.SourceFiles())
         {
            foreach (Match match in CaptionWordingTests.CaptionInCode.Matches(source))
            {
               read++;
               Judge(relative, match.Groups[1].Value.Replace("\\\"", "\"").Replace("\\\\", "\\"), offenders);
            }
         }

         string root = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile("App.xaml.cs"));
         foreach (string path in Directory.EnumerateFiles(root, "*.xaml", SearchOption.AllDirectories))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.Split(Path.DirectorySeparatorChar)[0] is "bin" or "obj")
               continue;
            foreach (Match match in CaptionInXaml.Matches(File.ReadAllText(path)))
            {
               read++;
               Judge(relative, match.Groups[1].Value.Replace("\\'", "'"), offenders);
            }
         }

         Assert.True(read > 600, "Only " + read + " captions were read; the patterns no longer find them.");
         Assert.True(offenders.Count == 0, "These captions say a unit, a range or what a value means out of the one form:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// What an empty box means is "empty = ..." in every text that ends with a bracket, an input
      /// prompt's caption as well as a field's: the DKIM rotation's "Selector for the new key (leave
      /// it empty for one named after today's date)" is a prompt's, which the caption positions
      /// above do not read.
      /// </summary>
      [Fact]
      public void EveryText_SaysWhatAnEmptyBoxMeansInTheOneForm()
      {
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"));
         var offenders = new List<string>();
         foreach (string text in XDocument.Load(path).Root.Elements("data").Select(d => (string)d.Attribute("name")))
         {
            Match last = Regex.Match(text, @"\(([^()]*)\)\s*$");
            // "(empty)" alone is a value shown, not a caption
            if (last.Success && last.Index > 0 && last.Groups[1].Value.Split("; ").Any(EmptyInProse))
               offenders.Add(text);
         }

         Assert.True(offenders.Count == 0, "Say what an empty box means as \"empty = ...\":\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// What a number means is "&lt;n&gt; = ..." in every catalogue wherever the English writes it so: the
      /// Russian wrote "(0 — без ограничения)" in 29 captions beside "(0 = без ограничения)" in 85, two
      /// of them captions c00bb6212 had written, and the Ukrainian once "(0 — немає)", where the other
      /// fifteen catalogues write "=" in all of them.
      /// </summary>
      [Fact]
      public void EveryTranslation_SaysWhatANumberMeansAsTheEnglishDoes()
      {
         string folder = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var meaning = new Regex(@"(?<![\w,.-])(-?\d+) = ");
         var offenders = new List<string>();
         int read = 0;
         foreach (string path in Directory.EnumerateFiles(folder, "Strings.*.resx"))
         {
            foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            {
               string key = (string)data.Attribute("name");
               // the Alt key's underscore may stand in the number: "(-_1 = ...)"
               string value = (data.Element("value")?.Value ?? "").Replace("_", "", StringComparison.Ordinal);
               if (value.Length == 0)
                  continue;
               foreach (Match number in meaning.Matches(key))
               {
                  read++;
                  if (!Regex.IsMatch(value, @"(?<![\d.,])" + Regex.Escape(number.Groups[1].Value) + " ="))
                     offenders.Add(Path.GetFileName(path) + ": " + key + " = " + value);
               }
            }
         }

         Assert.True(read > 1500, "Only " + read + " numbers' meanings were read from the catalogues.");
         Assert.True(offenders.Count == 0, "Write what a number means as the English does, \"0 = ...\":\n" + string.Join("\n", offenders));
      }

      private static void Judge(string relative, string caption, List<string> offenders)
      {
         List<string> problems = Problems(caption);
         if (problems.Count > 0)
            offenders.Add(relative + ": " + caption + " - " + string.Join(", ", problems));
      }

      [Fact]
      public void TheAntiSpamAndAntiVirusCaptions_TakeTheReviewsRewrites()
      {
         string source = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));

         foreach (string caption in new[]
         {
            "Largest message to spam-scan (KB; 0 = no limit)",
            "Largest message to virus-scan (KB; 0 = no limit)",
            "Refuse with 550 after this many retries before the window opens (0 = never)",
            "Keep greylisting active during the record expiration window",
            "Record expiration interval (minutes)",
            "Ban the sending address for (minutes, up to 525,600; 0 = no ban)",
            "Block the envelope sender for (days, up to 3,650; 0 = no block)",
            "Keep the record of trap hits for (days, 1 to 3,650)",
            "Score at which suspicious counts as malicious (0 to 100)",
         })
         {
            Assert.Contains("L(\"" + caption + "\")", source, StringComparison.Ordinal);
         }
      }

      /// <summary>
      /// The card of server-wide timeouts says that each of its captions says what 0 means, and
      /// four did not; and what moved out of a caption is the setting's hint now.
      /// </summary>
      [Fact]
      public void WhatACaptionNoLongerSays_IsItsHint_AndEveryTimeoutSaysWhat0Means()
      {
         string feature = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         string server = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));

         foreach (string caption in new[]
         {
            "Absolute lifetime of one client session (seconds; 0 = no time limit)",
            "Wait for a free database connection (seconds; 0 = wait for ever)",
            "Event script execution timeout (seconds; 0 = no time limit)",
            "External process timeout (seconds; 0 = no time limit)",
            "Report the async work queue as stalled after (seconds; 0 = never)",
            "Threads reserved so the async queue cannot be starved (0 = none)",
         })
         {
            Assert.Contains("L(\"" + caption + "\")", feature, StringComparison.Ordinal);
         }

         Assert.Contains("Label = L(\"External process timeout (seconds; 0 = no time limit)\"), Blurb = L(\"A command-line virus scanner is one.\")", feature, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Update feed URL (empty = the built-in feed)\"), Blurb = L(\"Set it to use a mirror.\")", feature, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Expunge records to keep (0 = keep everything)\"), Blurb = L(\"Keeping everything also skips the prune at start-up.\")", server, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Maximum characters per log line (at least 100)\"), Blurb = L(\"Only applied at log level 2 or lower.\")", server, StringComparison.Ordinal);
      }

      /// <summary>
      /// MXTriesFactor bounds how many of a domain's MX hosts each delivery attempt tries - the
      /// attempt's number times the factor, every host when it is 0 (ExternalDelivery) - where its
      /// caption said "Extra delivery attempts per additional MX host (0 = default)" and its hint
      /// that it multiplied the retry count.
      /// </summary>
      [Fact]
      public void TheMxTriesFactor_SaysWhatItDoes_AndWhat0Means()
      {
         string server = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string feature = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));

         Assert.Contains("Label = L(\"MX hosts tried per attempt, times the attempt number (0 = all)\")", server, StringComparison.Ordinal);
         Assert.Contains("The first attempt tries at most this many of a domain's MX hosts, the second at most twice as many", server, StringComparison.Ordinal);
         Assert.DoesNotContain("Extra delivery attempts per additional MX host", server + feature, StringComparison.Ordinal);
         Assert.DoesNotContain("multiplies the retry count", server, StringComparison.Ordinal);
         Assert.Contains("new ElsewhereSetting(\"delivery\", L(\"MX hosts tried per attempt, times the attempt number\"))", feature, StringComparison.Ordinal);
      }
   }
}
