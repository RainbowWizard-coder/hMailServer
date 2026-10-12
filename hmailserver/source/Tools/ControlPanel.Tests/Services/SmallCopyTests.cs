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
   /// Three small copy fixes of the Control Panel review (T3.1 item 3), each held so
   /// that it does not come back: a chart card's title says its unit once (F035),
   /// no page's purpose - which the Ctrl+K palette shows as a row - tells its reader
   /// to press Ctrl+K (F039), and a field's caption ends in no colon (F043).
   /// </summary>
   public class SmallCopyTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheSessionsCard_SaysActiveSessions_AndTheThroughputCardItsUnit()
      {
         Loc.Reset();
         Assert.Equal("Active sessions", ChartCatalog.DashboardSessions.CardTitle);
         Assert.Equal("Delivery throughput (messages / minute)", ChartCatalog.DashboardThroughput.CardTitle);

         // The table's headers and the spoken words keep the unit.
         Assert.Contains("SMTP (sessions)", ChartCatalog.DashboardSessions.TableHeaders);
      }

      [Theory]
      [InlineData("Active sessions", "sessions", false, "Active sessions")]
      [InlineData("Aktive Sitzungen", "Sitzungen", false, "Aktive Sitzungen")]
      [InlineData("アクティブなセッション", "セッション", false, "アクティブなセッション")]
      [InlineData("Aktiiviset istunnot", "istuntoa", true, "Aktiiviset istunnot")]
      [InlineData("Aktiiviset istunnot", "istuntoa", false, "Aktiiviset istunnot (istuntoa)")]
      [InlineData("Delivery throughput", "messages / minute", false, "Delivery throughput (messages / minute)")]
      [InlineData("Queue length", "", false, "Queue length")]
      public void ACardsTitle_NamesItsUnitOnce(string title, string unit, bool titleNamesUnit, string expected)
      {
         var chart = new ChartDefinition("test.chart", title, "d", unit, "N0", "A") { TitleNamesUnit = titleNamesUnit };
         Assert.Equal(expected, chart.CardTitle);
      }

      [Fact]
      public void TheChartCard_ShowsTheCardTitle()
      {
         string card = ReadControlPanelFile(Path.Join("Views", "AccessibleChartCard.cs"));
         Assert.Contains("titleText_.Text = definition_.CardTitle;", card, StringComparison.Ordinal);
         Assert.DoesNotContain("definition_.Title + \" (\" + definition_.Unit + \")\"", card, StringComparison.Ordinal);
      }

      /// <summary>
      /// A page's purpose is the palette's row for the page and the sidebar's tool
      /// tip: Welcome's said "press Ctrl+K and describe what you want to do" to a
      /// reader who was already in the palette.
      /// </summary>
      [Fact]
      public void NoPagesPurpose_TellsItsReaderToPressCtrlK()
      {
         List<string> found = NavigationMap.Pages.Where(p => p.Purpose.Contains("Ctrl+K", StringComparison.OrdinalIgnoreCase))
            .Select(p => p.Key + ": " + p.Purpose).ToList();
         Assert.True(found.Count == 0, string.Join(Environment.NewLine, found));

         NavNode welcome = NavigationMap.Pages.Single(p => p.Key == "welcome");
         Assert.Equal("Start here: jump straight to a common task, or describe what you want to do in the search.", welcome.Purpose);
      }

      /// <summary>
      /// The helpers that build a FieldRow from a caption given first - DialogFields'
      /// Field and ReadOnlyField, the dialogs' one-line Label that forwards to Field,
      /// and the pages' own - whose calls <see cref="NoFieldRowCaption_EndsInAColon"/>
      /// reads a caption from.
      /// </summary>
      private static readonly string[] CaptionHelpers = { "Field", "ReadOnlyField", "Label", "LabelledBox_", "LabelledCombo_", "FormField_", "Captioned" };

      private const string Literal = @"""((?:[^""\\]|\\.)*)""";

      /// <summary>
      /// A FieldRow's caption names its field and ends in no colon: "Domain:" above
      /// the DNS records page's domain list and "Folder name:" in the new public
      /// folder's prompt were the two the review found, where Server, User name,
      /// Password, Address and the Domains page's Domain do not. The rule read
      /// only <c>Label = L("...")</c> and XAML's <c>Label=</c>, and passed beside
      /// three more that a helper builds: the second factor's "Enter the 6-digit
      /// code from your authenticator app:" (DialogFields.Field), the domain's
      /// "Publish this DNS TXT record at your DNS provider, then enable DKIM:" and
      /// the account's new IMAP folder's "Folder name (...):" (PromptText). It
      /// reads a caption wherever the Control Panel gives one as a literal: a
      /// Label set in code or in XAML, the first argument of every helper that
      /// builds a FieldRow from a caption, and a text prompt's caption, its third.
      /// </summary>
      [Fact]
      public void NoFieldRowCaption_EndsInAColon()
      {
         string root = Path.GetDirectoryName(Path.GetDirectoryName(ControlPanelPath(Path.Join("Views", "ConnectView.xaml"))));
         var readers = new Dictionary<string, Regex>
         {
            ["Label ="] = new Regex(@"\bLabel\s*=\s*[LF]\(\s*" + Literal),
            ["helper"] = new Regex(@"(?<![\w.])(?:DialogFields\.)?(" + string.Join("|", CaptionHelpers) + @")\(\s*L\(\s*" + Literal + @"\s*\)\s*,"),
            ["prompt"] = new Regex(@"(?<![\w.])(?:DialogFields\.PromptText|PromptText|InputDialog\.Prompt)\(\s*[^,;]*?,\s*L\(\s*""(?:[^""\\]|\\.)*""\s*\)\s*,\s*L\(\s*" + Literal + @"\s*\)"),
         };
         var xaml = new Regex(@"\bLabel=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");
         var declaration = new Regex(@"\b(?:Scaffold\.)?FieldRow\s+(\w+)\s*\(\s*string\s+\w+");
         var read = new Dictionary<string, int>(StringComparer.Ordinal);
         var declared = new SortedSet<string>(StringComparer.Ordinal);
         var found = new List<string>();
         foreach (string path in Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
                     .Where(p => p.EndsWith(".cs", StringComparison.Ordinal) || p.EndsWith(".xaml", StringComparison.Ordinal)))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.Split(Path.DirectorySeparatorChar)[0] is "bin" or "obj")
               continue;

            string text = File.ReadAllText(path);
            IEnumerable<(string Reader, Match Match)> captions = path.EndsWith(".xaml", StringComparison.Ordinal)
               ? xaml.Matches(text).Select(m => ("XAML", m))
               : readers.SelectMany(r => r.Value.Matches(text).Select(m => (r.Key == "helper" ? m.Groups[1].Value : r.Key, m)));
            foreach ((string reader, Match m) in captions)
            {
               read[reader] = read.GetValueOrDefault(reader) + 1;
               string caption = m.Groups[m.Groups.Count - 1].Value;
               if (caption.TrimEnd().EndsWith(':'))
                  found.Add(relative + " (" + reader + "): " + caption);
            }

            foreach (Match m in declaration.Matches(text))
               declared.Add(m.Groups[1].Value);
         }

         Assert.True(found.Count == 0, "Captions that end in a colon:" + Environment.NewLine + string.Join(Environment.NewLine, found));

         // A helper the rule does not know is a caption it does not read; and each
         // reader, each helper among them, reads some caption, so that a call shape
         // it no longer matches cannot pass for no colon.
         Assert.True(declared.IsSubsetOf(CaptionHelpers), "A FieldRow helper that takes its caption first and that NoFieldRowCaption_EndsInAColon does not read: "
            + string.Join(", ", declared.Except(CaptionHelpers)));
         foreach (string reader in CaptionHelpers.Concat(new[] { "Label =", "prompt", "XAML" }))
            Assert.True(read.GetValueOrDefault(reader) > 0, "No caption read through " + reader + "; the rule's pattern for it no longer matches the source.");

         // A caption a page builds as a TextBlock of its own - the DNS records page's
         // Label(...) over each value box, and its report mailbox's - ends in no colon
         // either: five did, two of them the very examples review finding F301 gave
         // ("Report mailbox:", "TXT value:"), which the rule above did not read (the
         // final check of Tier 2, g4 defect 4).
         var label = new Regex(@"(?<![\w.])Label\((?<args>[^;]*?)\)\)?;");
         var literal = new Regex(@"\bL\(\s*(?<text>""(?:[^""\\]|\\.)*"")\s*\)");
         var labelled = new List<string>();
         int labels = 0;
         foreach (string path in Directory.EnumerateFiles(Path.Join(root, "Views"), "*.cs", SearchOption.AllDirectories))
         {
            foreach (Match call in label.Matches(File.ReadAllText(path)))
            {
               foreach (Match text in literal.Matches(call.Groups["args"].Value))
               {
                  labels++;
                  if (text.Groups["text"].Value.TrimEnd('"').TrimEnd().EndsWith(':'))
                     labelled.Add(Path.GetFileName(path) + ": " + text.Groups["text"].Value);
               }
            }
         }
         Assert.True(labels > 0, "No caption read through Label(...); the pattern no longer matches the source.");
         Assert.True(labelled.Count == 0, "Captions that end in a colon:" + Environment.NewLine + string.Join(Environment.NewLine, labelled));
         Assert.DoesNotMatch(new Regex(@"\bL\(""(?:Report mailbox|TXT value|Policy file the server will serve[^""]*|Host name this server announces[^""]*|MX value[^""]*):""\)"),
            ReadControlPanelFile(Path.Join("Views", "DnsRecordsView.cs")));

         // t213b's dialog asks for the public folder's name (review finding F206).
         string folders = ReadControlPanelFile(Path.Join("Views", "PublicFolderNameDialog.cs"));
         Assert.Contains("nameRow_ = DialogFields.Field(L(\"_Folder name\"), name_, PublicFolderTree.Hint(delimiter));", folders, StringComparison.Ordinal);
         string dns = ReadControlPanelFile(Path.Join("Views", "DnsRecordsView.cs"));
         Assert.Contains("new FieldRow { Label = L(\"_Domain\"), Content = domainCombo_ }", dns, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelPath(relative));

      private static string ControlPanelPath(string relative)
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
