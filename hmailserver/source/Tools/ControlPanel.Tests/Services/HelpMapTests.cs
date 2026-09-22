// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The help map holds every page of the navigation to a document, and every
   /// entry to a document and a heading that exist. build/check-help-map.py
   /// holds the same table, and the Deck's and the webmail's, to the documents
   /// from outside; this holds it from inside, where a page added to
   /// NavigationMap without an entry here fails a build of the tests rather
   /// than opening a pane on nothing.
   /// </summary>
   public class HelpMapTests
   {
      [Fact]
      public void EveryNavigationPageHasAnEntryAndNothingElseDoes()
      {
         var pages = NavigationMap.Pages.Select(p => p.Key).ToHashSet(StringComparer.OrdinalIgnoreCase);
         var mapped = HelpMap.Pages.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase);

         Assert.Empty(pages.Except(mapped, StringComparer.OrdinalIgnoreCase));
         Assert.Empty(mapped.Except(pages, StringComparer.OrdinalIgnoreCase));
      }

      /// <summary>Three lookups: a document alone, a document with a heading, and the dashboard's.</summary>
      [Theory]
      [InlineData("backup", "Backup.md", null)]
      [InlineData("rules", "MailFlowRules.md", null)]
      [InlineData("dashboard", "SetupChecklist.md", "Where it is")]
      public void ThePageIsMappedToItsDocument(string key, string doc, string heading)
      {
         HelpTarget target = HelpMap.For(key);

         Assert.NotNull(target);
         Assert.Equal(doc, target.Doc);
         Assert.Equal(heading, target.Heading);
         Assert.Equal(doc.Substring(0, doc.Length - 3), target.Name);
      }

      [Fact]
      public void AKeyThatIsNotAPageHasNoHelp()
      {
         Assert.Null(HelpMap.For("no-such-page"));
         Assert.Null(HelpMap.For(null));
      }

      [Fact]
      public void EveryEntryNamesADocumentThatExistsAndAHeadingItHas()
      {
         string docs = FindDocs();
         foreach (KeyValuePair<string, HelpTarget> entry in HelpMap.Pages)
         {
            string path = Path.Combine(docs, entry.Value.Doc);
            Assert.True(File.Exists(path), entry.Key + " names " + entry.Value.Doc + ", which is not in " + docs);

            if (entry.Value.Heading == null)
               continue;

            IReadOnlyList<MdHeading> headings = MarkdownBlocks.Headings(File.ReadAllText(path));
            Assert.Contains(headings, h => h.Text == entry.Value.Heading);
         }
      }

      [Fact]
      public void AReferenceOnlyDocumentExistsAndIsMappedByNoPage()
      {
         string docs = FindDocs();
         var named = HelpMap.Pages.Values.Select(t => t.Doc).ToHashSet(StringComparer.Ordinal);
         foreach (string doc in HelpMap.ReferenceOnly)
         {
            Assert.True(File.Exists(Path.Combine(docs, doc)), doc + " is listed as reference-only but is not a document");
            Assert.DoesNotContain(doc, named);
         }
      }

      /// <summary>
      /// The program's embedded copy of the documentation is the directory: the
      /// same files but README.md and DeckParity.md, in ordinal order as the
      /// server lists them, each the file's bytes with CRLF made LF. A broken
      /// Exclude or LogicalName in the project file shows here and nowhere else.
      /// </summary>
      [Fact]
      public void TheEmbeddedCopyIsTheDocumentsDirectory()
      {
         string docs = FindDocs();
         string[] expected = Directory.GetFiles(docs, "*.md")
            .Select(Path.GetFileName)
            .Where(f => f != "README.md" && f != "DeckParity.md")
            .Select(f => f.Substring(0, f.Length - 3))
            .OrderBy(n => n, StringComparer.Ordinal)
            .ToArray();

         Assert.NotEmpty(expected);
         Assert.Equal(expected, HelpPages.Names.ToArray());
         Assert.False(HelpPages.Has("README"));
         Assert.False(HelpPages.Has("DeckParity"));

         foreach (string name in expected)
         {
            string file = File.ReadAllText(Path.Combine(docs, name + ".md")).Replace("\r\n", "\n");
            Assert.Equal(file, HelpPages.Read(name));
         }

         Assert.Equal("Backup", HelpPages.Find("backup"));
         Assert.Null(HelpPages.Read("NoSuchDocument"));
      }

      /// <summary>
      /// The wiki base URL the pane hands to the shell is kept only when it is
      /// a web address: the rule HelpPane's setter applies, held here because the
      /// setter itself is WPF and the test project has no window.
      /// </summary>
      [Theory]
      [InlineData("https://gitlab.com/Progressiverobot/hmailserver/-/wikis/", "https://gitlab.com/Progressiverobot/hmailserver/-/wikis/")]
      [InlineData("  HTTP://wiki.example.test/  ", "HTTP://wiki.example.test/")]
      [InlineData("", "")]
      [InlineData(null, "")]
      [InlineData("C:\\Windows\\System32\\calc.exe", "")]
      [InlineData("file:///C:/x", "")]
      [InlineData("ftp://wiki.example.test/", "")]
      [InlineData("wiki.example.test", "")]
      [InlineData("javascript:alert(1)", "")]
      public void TheWikiBaseUrlIsKeptOnlyWhenItIsAWebAddress(string value, string expected)
      {
         Assert.Equal(expected, HelpPages.WikiBaseUrlOrEmpty(value));
      }

      private static string FindDocs()
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
