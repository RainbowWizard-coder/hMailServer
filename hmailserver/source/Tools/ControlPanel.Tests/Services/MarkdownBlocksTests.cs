// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The help pane's markdown reading, over a fixed document written the way
   /// the operator documentation is: a setext title, an ATX heading, a list
   /// with a nested list, a numbered list, a code block, a table with a pipe
   /// inside a code span, a link to the web and one to another document, a
   /// quote and a rule. What is proved is the block model MarkdownFlow draws
   /// from, so a construct that stopped being read would show here rather
   /// than as a wall of text in the pane.
   /// </summary>
   public class MarkdownBlocksTests
   {
      private const string Document =
         "What the backup carries\n" +
         "=======================\n" +
         "\n" +
         "The built-in backup writes one archive per run: an index, and - when\n" +
         "**Back up messages** is on - the *whole* store beside it. See\n" +
         "[Upgrading](Upgrading.md#the-short-version) and [the site](https://example.test/x).\n" +
         "\n" +
         "In the archive\n" +
         "--------------\n" +
         "\n" +
         "### The server\n" +
         "\n" +
         "* Domains, with `hm_domains`\n" +
         "* Accounts\n" +
         "  * nested_item with an under_score\n" +
         "\n" +
         "1. first\n" +
         "2. second\n" +
         "\n" +
         "| What | Table |\n" +
         "|---|---|\n" +
         "| Folders | `a|b` |\n" +
         "| Rules | hm_rules |\n" +
         "\n" +
         "```sql\n" +
         "select * from hm_settings\n" +
         "# not a heading\n" +
         "```\n" +
         "\n" +
         "> A quoted line.\n" +
         "\n" +
         "---\n" +
         "\n" +
         "Not in the archive\n" +
         "------------------\n" +
         "\n" +
         "The end.\n";

      [Fact]
      public void HeadingsOfBothKindsAreReadWithTheirLevelsAndLines()
      {
         IReadOnlyList<MdHeading> headings = MarkdownBlocks.Headings(Document);

         Assert.Equal(new[] { "What the backup carries", "In the archive", "Not in the archive" }, headings.Select(h => h.Text).ToArray());
         Assert.Equal(new[] { 1, 2, 2 }, headings.Select(h => h.Level).ToArray());
         Assert.Equal(new[] { 0, 7, 33 }, headings.Select(h => h.Line).ToArray());

         // The ### heading is a heading of the document, just not one the list carries.
         Assert.Contains(MarkdownBlocks.Parse(Document), b => b is MdHeading h && h.Level == 3 && h.Text == "The server");
      }

      [Fact]
      public void TheBlocksAreReadInOrder()
      {
         IReadOnlyList<MdBlock> blocks = MarkdownBlocks.Parse(Document);

         Assert.Equal(
            new[] { "MdHeading", "MdParagraph", "MdHeading", "MdHeading", "MdList", "MdList", "MdTable", "MdCode", "MdQuote", "MdRule", "MdHeading", "MdParagraph" },
            blocks.Select(b => b.GetType().Name).ToArray());
      }

      [Fact]
      public void AListNestsByIndentationAndANumberedListKeepsItsStart()
      {
         IReadOnlyList<MdBlock> blocks = MarkdownBlocks.Parse(Document);
         var bullets = (MdList) blocks[4];
         var numbered = (MdList) blocks[5];

         Assert.False(bullets.Ordered);
         Assert.Equal(2, bullets.Items.Count);
         Assert.Equal("Domains, with `hm_domains`", ((MdParagraph) bullets.Items[0][0]).Text);
         Assert.Equal(2, bullets.Items[1].Count);
         var nested = (MdList) bullets.Items[1][1];
         Assert.Equal("nested_item with an under_score", ((MdParagraph) nested.Items[0][0]).Text);

         Assert.True(numbered.Ordered);
         Assert.Equal(1, numbered.Start);
         Assert.Equal(new[] { "first", "second" }, numbered.Items.Select(i => ((MdParagraph) i[0]).Text).ToArray());
      }

      [Fact]
      public void ATableKeepsAPipeInsideACodeSpan()
      {
         var table = (MdTable) MarkdownBlocks.Parse(Document)[6];

         Assert.Equal(new[] { "What", "Table" }, table.Header.ToArray());
         Assert.Equal(2, table.Rows.Count);
         Assert.Equal("`a|b`", table.Rows[0][1]);
      }

      [Fact]
      public void ACodeBlockKeepsItsTextAndHidesAHeadingInsideIt()
      {
         var code = (MdCode) MarkdownBlocks.Parse(Document)[7];

         Assert.Equal("sql", code.Language);
         Assert.Equal("select * from hm_settings\n# not a heading", code.Text);
      }

      [Fact]
      public void InlinesAreReadAsCodeStrongEmphasisAndLinks()
      {
         IReadOnlyList<MdInline> inlines = MarkdownBlocks.Inlines("when **Back up messages** is on - the *whole* store, `hm_x`, [Upgrading](Upgrading.md#the-short-version) and [the site](https://example.test/x) and setting_name.");

         Assert.Equal(
            new[] { MdInlineKind.Text, MdInlineKind.Strong, MdInlineKind.Text, MdInlineKind.Emphasis, MdInlineKind.Text, MdInlineKind.Code, MdInlineKind.Text, MdInlineKind.Link, MdInlineKind.Text, MdInlineKind.Link, MdInlineKind.Text },
            inlines.Select(i => i.Kind).ToArray());
         Assert.Equal("Back up messages", inlines[1].Children[0].Text);
         Assert.Equal("hm_x", inlines[5].Text);
         Assert.Equal("Upgrading", inlines[7].Target.Doc);
         Assert.Equal("the-short-version", inlines[7].Target.Heading);
         Assert.Equal("https://example.test/x", inlines[9].Target.Href);
         Assert.Null(inlines[9].Target.Doc);
         Assert.Equal(" and setting_name.", inlines[10].Text);
      }

      [Theory]
      [InlineData("https://example.test/a", "https://example.test/a", null)]
      [InlineData("mailto:x@example.test", "mailto:x@example.test", null)]
      [InlineData("Backup.md", "#", "Backup")]
      [InlineData("Backup.md#versions", "#", "Backup")]
      public void ALinkGoesToTheWebOrToAnotherDocument(string href, string expectedHref, string expectedDoc)
      {
         MdLinkTarget target = MarkdownBlocks.LinkTarget(href);

         Assert.NotNull(target);
         Assert.Equal(expectedHref, target.Href);
         Assert.Equal(expectedDoc, target.Doc);
      }

      [Theory]
      [InlineData("javascript:alert(1)")]
      [InlineData("../etc/passwd")]
      [InlineData("images/x.png")]
      [InlineData("ftp://example.test")]
      public void AnythingElseIsNotALink(string href)
      {
         Assert.Null(MarkdownBlocks.LinkTarget(href));
      }

      /// <summary>
      /// The pane's scroll target: the block of the heading the page is mapped
      /// to, the top when the document does not have it or none is named.
      /// </summary>
      [Fact]
      public void TheScrollTargetIsTheMappedHeadingsBlock()
      {
         IReadOnlyList<MdBlock> blocks = MarkdownBlocks.Parse(Document);

         Assert.Equal(2, MarkdownBlocks.IndexOfHeading(blocks, "In the archive"));
         Assert.Equal(10, MarkdownBlocks.IndexOfHeading(blocks, "Not in the archive"));
         Assert.Equal(-1, MarkdownBlocks.IndexOfHeading(blocks, "A heading that is not there"));
         Assert.Equal(-1, MarkdownBlocks.IndexOfHeading(blocks, null));
      }

      /// <summary>
      /// A line of prose that wraps to begin with a number and a full stop stays
      /// in its paragraph: a numbered list ends a paragraph only when it starts at
      /// 1, as CommonMark has it. ServiceAccount.md's "... and Yes is a / 0. A
      /// silent installation's move then waits" was read as a list starting at 0,
      /// which the help pane's FlowDocument refused, and the pane threw.
      /// </summary>
      [Fact]
      public void ANumberThatWrapsToTheStartOfALine_StaysInItsParagraph()
      {
         IReadOnlyList<MdBlock> wrapped = MarkdownBlocks.Parse("With --confirm the question names it, and Yes is a\n0. A silent installation's move then waits.\n");
         var paragraph = Assert.IsType<MdParagraph>(Assert.Single(wrapped));
         Assert.Contains("is a 0. A silent", paragraph.Text, StringComparison.Ordinal);

         IReadOnlyList<MdBlock> interrupted = MarkdownBlocks.Parse("The steps:\n1. First\n2. Second\n");
         Assert.IsType<MdParagraph>(interrupted[0]);
         Assert.Equal(2, Assert.IsType<MdList>(interrupted[1]).Items.Count);

         IReadOnlyList<MdBlock> bullets = MarkdownBlocks.Parse("The parts:\n- one\n- two\n");
         Assert.IsType<MdList>(bullets[1]);

         // After a blank line a list may start anywhere, 0 included.
         IReadOnlyList<MdBlock> fromZero = MarkdownBlocks.Parse("Text.\n\n0. Zero\n1. One\n");
         Assert.Equal(0, Assert.IsType<MdList>(fromZero[1]).Start);

         // And every document of the set parses without a list the pane cannot number
         // from inside a paragraph's text.
         foreach (string name in HelpPages.Names)
         {
            foreach (MdBlock block in MarkdownBlocks.Parse(HelpPages.Read(name)))
            {
               if (block is MdList list && list.Ordered && list.Start < 1)
                  Assert.Fail(name + ".md has a numbered list starting at " + list.Start + " on line " + (list.Line + 1) + ".");
            }
         }
      }
   }
}
