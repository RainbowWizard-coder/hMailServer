// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Windows;
using System.Windows.Documents;
using System.Windows.Media;
using Typography = hMailServer.ControlPanel.Services.Typography;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The blocks MarkdownBlocks read, as a FlowDocument for the help pane:
   /// headings, paragraphs, emphasis, inline code, code blocks, bulleted and
   /// numbered lists, links, block quotes, rules and tables as a grid; nothing
   /// else, and anything else as its text. Every character of a document
   /// becomes a Run, so the text can carry nothing the program runs; a link is
   /// followed only when it is http, https or mailto (opened outside, by the
   /// shell) or names another document of the set, which the pane opens in
   /// place. The block a heading became is kept by its text, so the pane can
   /// scroll to the heading its page is mapped to.
   /// </summary>
   public static class MarkdownFlow
   {
      public sealed class Built
      {
         public FlowDocument Document { get; internal set; }

         /// <summary>The block each level-one or level-two heading became, by its text.</summary>
         public IReadOnlyDictionary<string, Block> Headings { get; internal set; }
      }

      /// <summary>
      /// Builds the document. openDocument is called with the name and the
      /// heading of another document of the set when a link to one is
      /// followed; null leaves such links as text.
      /// </summary>
      public static Built Build(IReadOnlyList<MdBlock> blocks, Action<string, string> openDocument)
      {
         var headings = new Dictionary<string, Block>(StringComparer.Ordinal);
         FlowDocument document = NewDocument();

         foreach (Block block in Blocks(blocks, openDocument, headings, 0))
            document.Blocks.Add(block);

         return new Built { Document = document, Headings = headings };
      }

      /// <summary>
      /// An empty document in the help pane's face, size and colour, ragged right
      /// and unhyphenated: every document the pane shows starts here, the ones it
      /// renders and the ones it writes itself - the list of documents and the
      /// line that says a document is missing. A FlowDocument justifies by
      /// default, which spread a wrapped title's words across the pane in All
      /// documentation ("Account  recovery:  a  recovery  address"), and the
      /// missing document's line was in the viewer's own face and size (review
      /// finding F026).
      /// </summary>
      public static FlowDocument NewDocument()
      {
         var document = new FlowDocument
         {
            FontFamily = new FontFamily(Typography.UiFontFamily),
            FontSize = Typography.Body,
            PagePadding = new Thickness(0),
            TextAlignment = TextAlignment.Left,
            IsHyphenationEnabled = false
         };
         document.SetResourceReference(FlowDocument.ForegroundProperty, "TextFillColorPrimaryBrush");
         return document;
      }

      private static IEnumerable<Block> Blocks(IReadOnlyList<MdBlock> blocks, Action<string, string> openDocument, Dictionary<string, Block> headings, int depth)
      {
         foreach (MdBlock block in blocks)
         {
            switch (block)
            {
               case MdHeading heading:
                  var head = new Paragraph
                  {
                     FontSize = heading.Level == 1 ? 20 : heading.Level == 2 ? 16 : Typography.Body,
                     FontWeight = FontWeights.SemiBold,
                     Margin = new Thickness(0, heading.Level == 1 ? 4 : 16, 0, 6),
                     KeepWithNext = true
                  };
                  Inlines(head.Inlines, MarkdownBlocks.Inlines(heading.Text), openDocument);
                  if (heading.Level <= 2 && !headings.ContainsKey(heading.Text))
                     headings[heading.Text] = head;
                  yield return head;
                  break;

               case MdParagraph paragraph:
                  var p = new Paragraph { Margin = new Thickness(0, 0, 0, 10) };
                  Inlines(p.Inlines, MarkdownBlocks.Inlines(paragraph.Text), openDocument);
                  yield return p;
                  break;

               case MdCode code:
                  var pre = new Paragraph(new Run(code.Text))
                  {
                     FontFamily = new FontFamily(Typography.MonoFontFamily),
                     FontSize = Typography.Caption,
                     Padding = new Thickness(10, 8, 10, 8),
                     Margin = new Thickness(0, 0, 0, 10)
                  };
                  pre.SetResourceReference(Block.BackgroundProperty, "ControlFillColorDefaultBrush");
                  yield return pre;
                  break;

               case MdList list:
                  var flowList = new List
                  {
                     MarkerStyle = list.Ordered ? TextMarkerStyle.Decimal : depth % 2 == 0 ? TextMarkerStyle.Disc : TextMarkerStyle.Circle,
                     // A FlowDocument numbers from 1 at the least; a list that
                     // starts at 0 is numbered from 1 rather than not shown.
                     StartIndex = list.Ordered ? Math.Max(1, list.Start) : 1,
                     Margin = new Thickness(0, 0, 0, 10),
                     MarkerOffset = 8
                  };
                  foreach (List<MdBlock> item in list.Items)
                  {
                     var listItem = new ListItem();
                     foreach (Block inner in Blocks(item, openDocument, headings, depth + 1))
                     {
                        if (inner is Paragraph innerParagraph)
                           innerParagraph.Margin = new Thickness(0, 0, 0, 2);
                        if (inner is List innerList)
                           innerList.Margin = new Thickness(0, 0, 0, 2);
                        listItem.Blocks.Add(inner);
                     }
                     flowList.ListItems.Add(listItem);
                  }
                  yield return flowList;
                  break;

               case MdQuote quote:
                  var section = new Section
                  {
                     BorderThickness = new Thickness(3, 0, 0, 0),
                     Padding = new Thickness(10, 0, 0, 0),
                     Margin = new Thickness(0, 0, 0, 10)
                  };
                  section.SetResourceReference(Block.BorderBrushProperty, "ControlStrokeColorDefaultBrush");
                  section.SetResourceReference(Block.ForegroundProperty, "TextFillColorSecondaryBrush");
                  foreach (Block inner in Blocks(quote.Blocks, openDocument, headings, depth + 1))
                     section.Blocks.Add(inner);
                  yield return section;
                  break;

               case MdTable table:
                  yield return Table(table, openDocument);
                  break;

               case MdRule _:
                  var rule = new Paragraph
                  {
                     Margin = new Thickness(0, 4, 0, 12),
                     BorderThickness = new Thickness(0, 0, 0, 1),
                     FontSize = 2
                  };
                  rule.SetResourceReference(Block.BorderBrushProperty, "ControlStrokeColorDefaultBrush");
                  yield return rule;
                  break;
            }
         }
      }

      private static Table Table(MdTable table, Action<string, string> openDocument)
      {
         var flowTable = new Table
         {
            CellSpacing = 0,
            Margin = new Thickness(0, 0, 0, 10),
            FontSize = Typography.Caption
         };
         int columns = Math.Max(1, table.Header.Count);
         for (int c = 0; c < columns; c++)
            flowTable.Columns.Add(new TableColumn());

         var group = new TableRowGroup();
         var headRow = new TableRow { FontWeight = FontWeights.SemiBold };
         foreach (string cell in table.Header)
            headRow.Cells.Add(Cell(cell, openDocument, true));
         group.Rows.Add(headRow);

         foreach (List<string> row in table.Rows)
         {
            var flowRow = new TableRow();
            for (int c = 0; c < columns; c++)
               flowRow.Cells.Add(Cell(c < row.Count ? row[c] : "", openDocument, false));
            group.Rows.Add(flowRow);
         }
         flowTable.RowGroups.Add(group);
         return flowTable;
      }

      private static TableCell Cell(string text, Action<string, string> openDocument, bool header)
      {
         var paragraph = new Paragraph { Margin = new Thickness(0) };
         Inlines(paragraph.Inlines, MarkdownBlocks.Inlines(text), openDocument);
         var cell = new TableCell(paragraph)
         {
            Padding = new Thickness(6, 4, 6, 4),
            BorderThickness = new Thickness(0, 0, 0, 1)
         };
         cell.SetResourceReference(TableCell.BorderBrushProperty, header ? "ControlStrokeColorSecondaryBrush" : "ControlStrokeColorDefaultBrush");
         return cell;
      }

      private static void Inlines(InlineCollection into, IReadOnlyList<MdInline> inlines, Action<string, string> openDocument)
      {
         foreach (MdInline inline in inlines)
         {
            switch (inline.Kind)
            {
               case MdInlineKind.Text:
                  into.Add(new Run(inline.Text));
                  break;

               case MdInlineKind.Code:
                  var code = new Run(inline.Text) { FontFamily = new FontFamily(Typography.MonoFontFamily) };
                  code.SetResourceReference(TextElement.BackgroundProperty, "ControlFillColorDefaultBrush");
                  into.Add(code);
                  break;

               case MdInlineKind.Strong:
                  var strong = new Bold();
                  Inlines(strong.Inlines, inline.Children, openDocument);
                  into.Add(strong);
                  break;

               case MdInlineKind.Emphasis:
                  var em = new Italic();
                  Inlines(em.Inlines, inline.Children, openDocument);
                  into.Add(em);
                  break;

               case MdInlineKind.Link:
                  MdLinkTarget target = inline.Target;
                  if (target.Doc != null && openDocument == null)
                  {
                     Inlines(into, inline.Children, openDocument);
                     break;
                  }
                  var link = new Hyperlink();
                  Inlines(link.Inlines, inline.Children, openDocument);
                  link.ToolTip = target.Doc != null ? target.Doc + ".md" : target.Href;
                  link.Click += (s, e) =>
                  {
                     if (target.Doc != null)
                     {
                        openDocument(target.Doc, target.Heading);
                        return;
                     }
                     if (target.Href.StartsWith("#", StringComparison.Ordinal))
                        return;
                     try
                     {
                        Process.Start(new ProcessStartInfo(target.Href) { UseShellExecute = true });
                     }
                     catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                     {
                        // A link the shell could not open is a link that did nothing.
                     }
                  };
                  into.Add(link);
                  break;
            }
         }
      }
   }
}
