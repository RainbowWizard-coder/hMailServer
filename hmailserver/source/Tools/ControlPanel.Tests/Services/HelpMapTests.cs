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
