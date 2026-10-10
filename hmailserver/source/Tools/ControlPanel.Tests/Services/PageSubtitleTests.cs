// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A page's subtitle is a sentence or two on a line of its own under the title and
   /// the actions (review finding F279). It shared the title's column, left of the
   /// actions, and the long ones ran to ten lines there in English and fourteen in
   /// German - the greylisting bypass list, the allow and block list, Blocked senders,
   /// Webmail and TCP/IP ports - so that in the narrowest window those pages showed
   /// their list's header and no rows. The rest of what such a page said is in the
   /// help document its help pane opens, or, for the ports page's Listening column,
   /// in that column's header.
   /// </summary>
   public class PageSubtitleTests
   {
      // The subtitles still longer than two sentences, by their first words, with why
      // they are left for now. Nothing else may be: a new one is cut, the rest of what
      // it would say going to the page's help.
      private static readonly Dictionary<string, string> LongerForNow = new(StringComparer.Ordinal)
      {
         ["Sending organisations decided about by their domain"] = "the domain white and black list, whose subtitle T2.11 item 15 (F266) rewrites with the notice under it",
         ["What this server does when it talks to a named remote domain"] = "Remote domains, whose first sentence alone is three lines and wants new words",
         ["Messages the server would otherwise have refused"] = "the quarantine",
         ["Creates and updates mailboxes to match an LDAP directory"] = "directory synchronisation",
         ["What this server still needs, in the order"] = "the setup checklist",
         ["Edit the server event-handler script"] = "Event scripts, whose last sentence names the page holding the engine's switch, which T2.10 item 8 (F273) may move",
      };

      [Fact]
      public void EveryPageSubtitle_IsTwoSentencesAtMost()
      {
         string root = Path.GetDirectoryName(ControlPanelFile("App.xaml.cs"));
         var offenders = new List<string>();
         int seen = 0;
         foreach ((string file, string subtitle) in Subtitles(root))
         {
            seen++;
            int sentences = Sentences(subtitle);
            if (sentences <= 2 || LongerForNow.Keys.Any(k => subtitle.StartsWith(k, StringComparison.Ordinal)))
               continue;
            offenders.Add(file + ": " + sentences.ToString(CultureInfo.InvariantCulture) + " sentences: " + subtitle);
         }

         Assert.True(seen > 40, "Only " + seen + " page subtitles were found; the patterns no longer match how pages set them.");
         Assert.True(offenders.Count == 0,
            "A page's subtitle runs past two sentences; keep two on the page and put the rest in the page's help:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void TheSubtitlesLeftLongerForNow_AreStillThere()
      {
         // A subtitle cut since stays cut: its entry here goes with it.
         string root = Path.GetDirectoryName(ControlPanelFile("App.xaml.cs"));
         List<string> subtitles = Subtitles(root).Select(s => s.Subtitle).ToList();
         foreach (string start in LongerForNow.Keys)
            Assert.True(subtitles.Any(s => s.StartsWith(start, StringComparison.Ordinal) && Sentences(s) > 2),
               "\"" + start + "...\" is two sentences now, or gone: take it off LongerForNow.");
      }

      [Theory]
      [InlineData("Envelope domains and HELO suffixes let past greylisting")]
      [InlineData("Addresses and whole domains that every account wants, or does not.")]
      [InlineData("Envelope senders refused outright, or scored.")]
      [InlineData("The pages people sign in to in a browser")]
      [InlineData("The ports each protocol listens on")]
      [InlineData("A role is a named set of permissions")]
      public void TheSubtitlesTheReviewFoundLong_AreTwoSentencesAtMost(string start)
      {
         string root = Path.GetDirectoryName(ControlPanelFile("App.xaml.cs"));
         string subtitle = Subtitles(root).Select(s => s.Subtitle).Single(s => s.StartsWith(start, StringComparison.Ordinal));
         Assert.InRange(Sentences(subtitle), 1, 2);
      }

      [Fact]
      public void TheSentenceCount_CountsSentencesNotAbbreviations()
      {
         Assert.Equal(1, Sentences("Look up the mail exchanger records for a domain - where e-mail to that domain is delivered."));
         Assert.Equal(1, Sentences("A host such as mail.example.com, e.g. the relay."));
         Assert.Equal(3, Sentences("Edit the script (OnHELO, ...). Saving writes the file. The switch is elsewhere."));
      }

      [Fact]
      public void TheSubtitle_HasALineOfItsOwn_UnderTheTitleAndTheActions()
      {
         string scaffold = File.ReadAllText(ControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml")));
         int start = scaffold.IndexOf("<Style TargetType=\"{x:Type scaffold:PageHeader}\">", StringComparison.Ordinal);
         Assert.True(start >= 0, "Scaffold.xaml no longer styles PageHeader.");
         string header = scaffold.Substring(start, scaffold.IndexOf("</Style>", start, StringComparison.Ordinal) - start);

         Match subtitle = Regex.Match(header, "<TextBlock x:Name=\"PART_Subtitle\"[^>]*>");
         Assert.True(subtitle.Success, "The PageHeader template has no PART_Subtitle.");
         Assert.Contains("Grid.Row=\"1\"", subtitle.Value, StringComparison.Ordinal);
         Assert.Contains("Grid.ColumnSpan=\"2\"", subtitle.Value, StringComparison.Ordinal);
         Assert.Contains("MaxWidth=\"{StaticResource AppSubtitleWidth}\"", subtitle.Value, StringComparison.Ordinal);

         // The title row holds the title and its pill only.
         Match titleRow = Regex.Match(header, "<WrapPanel x:Name=\"PART_TitleRow\"[^>]*>(.*?)</WrapPanel>", RegexOptions.Singleline);
         Assert.True(titleRow.Success, "The PageHeader template has no PART_TitleRow.");
         Assert.DoesNotContain("PART_Subtitle", titleRow.Groups[1].Value, StringComparison.Ordinal);

         // And the actions that do not fit beside the title go under the subtitle.
         string code = File.ReadAllText(ControlPanelFile(Path.Join("Views", "Scaffold", "PageHeader.cs")));
         Assert.Contains("PageHeaderLayout.ActionsGoUnder(constraint.Width, actions, TitleWidth_())", code, StringComparison.Ordinal);
         Assert.Contains("int row = under ? 2 : 0;", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSubtitleWidth_IsOneTokenInXamlAndInCode()
      {
         string tokens = File.ReadAllText(ControlPanelFile(Path.Join("Views", "Scaffold", "Tokens.xaml")));
         Match width = Regex.Match(tokens, "<sys:Double x:Key=\"AppSubtitleWidth\">([0-9.]+)</sys:Double>");
         Assert.True(width.Success, "Tokens.xaml has no AppSubtitleWidth.");
         Assert.Equal(DesignTokens.SubtitleWidth, double.Parse(width.Groups[1].Value, CultureInfo.InvariantCulture));
      }

      [Theory]
      // A short title beside wide actions in a wide header: beside.
      [InlineData(950, 400, 300, false)]
      // The same title where the actions leave it less than its width: under.
      [InlineData(640, 400, 300, true)]
      // A title exactly as wide as the room the actions leave: beside.
      [InlineData(700, 400, 300, false)]
      // No actions at all, or only the help button: beside, at any width.
      [InlineData(200, 0, 300, false)]
      [InlineData(640, 56, 300, false)]
      // Actions wider than the header: under, whatever the title.
      [InlineData(300, 400, 0, true)]
      public void TheActions_GoUnderTheSubtitle_OnlyWhereTheTitleWouldNotFitBesideThemWhole(double width, double actions, double title, bool under)
         => Assert.Equal(under, PageHeaderLayout.ActionsGoUnder(width, actions, title));

      [Fact]
      public void TheListeningColumn_SaysWhatItReads_InItsHeader()
      {
         string page = File.ReadAllText(ControlPanelFile(Path.Join("Views", "TcpIpPortsView.xaml.cs")));
         Assert.Contains("GridStyles.HeaderCaption(ListeningColumn, L(\"" + ListeningTip + "\"));", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// The sentence sits on the Listening column's header, so it names the column by the
      /// header's own word in every language: the French one said "La colonne Écoute" under
      /// a header that reads "À l'écoute", carried over from the subtitle it was cut from.
      /// </summary>
      [Fact]
      public void TheListeningColumnsToolTip_NamesTheColumnAsItsHeaderReads_InEveryLanguage()
      {
         string resources = Path.GetDirectoryName(ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var problems = new List<string>();
         int seen = 0;
         foreach (string path in Directory.GetFiles(resources, "Strings.*.resx"))
         {
            seen++;
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            var catalogue = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
               catalogue[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";

            if (!catalogue.TryGetValue("Listening", out string header) || !catalogue.TryGetValue(ListeningTip, out string tip))
               problems.Add(tag + ": the Listening header or its tool tip is not translated");
            else if (!tip.Contains(header, StringComparison.Ordinal))
               problems.Add(tag + ": the header reads '" + header + "' and its tool tip says '" + tip + "'");
         }

         Assert.True(seen >= 17, "Only " + seen + " catalogues were found beside Strings.resx.");
         Assert.True(problems.Count == 0, string.Join("\n", problems));
      }

      private const string ListeningTip = "The Listening column is read from this machine's open ports, not from the configuration, so it shows what is actually happening rather than what was asked for.";

      /// <summary>
      /// What the cut subtitles said is in the help document each page's help opens
      /// (HelpMap): Blocked senders' warning, that it matches the address the sender
      /// claims and is no defence against spoofing, had been cut from the page and was in
      /// no document, while the commit that cut it said it was.
      /// </summary>
      [Theory]
      [InlineData("blockedsenders", "matches the envelope address the sender claims")]
      [InlineData("blockedsenders", "no defence against spoofing")]
      [InlineData("senderlist", "Nearest first: the account, then its domain, then the server.")]
      [InlineData("senderlist", "which refuses the message during the SMTP conversation")]
      [InlineData("greylistbypass", "a suffix with a leading dot")]
      [InlineData("greylistbypass", "Greylisting decides at `RCPT TO`")]
      [InlineData("greylistbypass", "privilege granted to a named domain")]
      [InlineData("roles", "The four built-in roles cannot be edited")]
      public void WhatTheCutSubtitlesSaid_IsInTheHelpTheirPagesOpen(string page, string words)
      {
         HelpTarget target = HelpMap.For(page);
         Assert.NotNull(target);
         string docs = Path.GetFullPath(Path.Join(Path.GetDirectoryName(ControlPanelFile("App.xaml.cs")), "..", "..", "..", "docs"));
         string text = Regex.Replace(File.ReadAllText(Path.Join(docs, target.Doc)), @"\s+", " ");
         Assert.Contains(words, text, StringComparison.Ordinal);
      }

      /// <summary>The number of sentences in an English text: a full stop, a question or an exclamation mark, then a space and a capital, a quotation or a bracket, or the end.</summary>
      internal static int Sentences(string text)
         => Regex.Matches(text.Trim(), @"[.!?](?=\s+[A-Z""“(]|\s*$)").Count;

      /// <summary>Every subtitle literal a page sets: in XAML, and in code as a property or on a header.</summary>
      internal static IEnumerable<(string File, string Subtitle)> Subtitles(string root)
      {
         var xaml = new Regex(@"Subtitle=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");
         var code = new Regex(@"Subtitle\s*=\s*[LF]\(\s*""((?:[^""\\]|\\.)*)""");
         foreach (string path in Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
                     .Where(p => p.EndsWith(".cs", StringComparison.Ordinal) || p.EndsWith(".xaml", StringComparison.Ordinal)))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.Split(Path.DirectorySeparatorChar)[0] is "bin" or "obj")
               continue;

            string text = File.ReadAllText(path);
            Regex pattern = path.EndsWith(".xaml", StringComparison.Ordinal) ? xaml : code;
            foreach (Match m in pattern.Matches(text))
               yield return (relative, m.Groups[1].Value.Replace("\\'", "'").Replace("\\\"", "\""));
         }
      }

      private static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
