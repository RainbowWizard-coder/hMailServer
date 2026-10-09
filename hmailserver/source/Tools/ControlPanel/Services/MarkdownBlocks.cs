// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One block of a markdown document, and the line it starts on (from 0).</summary>
   public abstract class MdBlock
   {
      public int Line { get; internal set; }
   }

   public sealed class MdHeading : MdBlock
   {
      public int Level { get; internal set; }
      public string Text { get; internal set; }
   }

   public sealed class MdParagraph : MdBlock
   {
      public string Text { get; internal set; }
   }

   public sealed class MdCode : MdBlock
   {
      public string Text { get; internal set; }
      public string Language { get; internal set; }
   }

   public sealed class MdList : MdBlock
   {
      public bool Ordered { get; internal set; }
      public int Start { get; internal set; }
      public List<List<MdBlock>> Items { get; } = new List<List<MdBlock>>();
   }

   public sealed class MdQuote : MdBlock
   {
      public List<MdBlock> Blocks { get; } = new List<MdBlock>();
   }

   public sealed class MdTable : MdBlock
   {
      public List<string> Header { get; } = new List<string>();
      public List<List<string>> Rows { get; } = new List<List<string>>();
   }

   public sealed class MdRule : MdBlock
   {
   }

   public enum MdInlineKind
   {
      Text,
      Code,
      Strong,
      Emphasis,
      Link
   }

   /// <summary>One run of a line: text, a code span, or a span with children.</summary>
   public sealed class MdInline
   {
      public MdInlineKind Kind { get; internal set; }
      public string Text { get; internal set; }
      public MdLinkTarget Target { get; internal set; }
      public List<MdInline> Children { get; } = new List<MdInline>();
   }

   /// <summary>
   /// Where a link goes: the web (Href, opened outside), or another document of
   /// the set (Doc, opened in the pane, at Heading when one is named).
   /// </summary>
   public sealed class MdLinkTarget
   {
      public string Href { get; internal set; }
      public string Doc { get; internal set; }
      public string Heading { get; internal set; }
   }

   /// <summary>
   /// Markdown for the help pane, read into blocks: the subset the operator
   /// documentation (hmailserver/docs/*.md) is written in - headings (ATX and
   /// setext), paragraphs, emphasis and strong, inline code, fenced code
   /// blocks, bulleted and numbered lists nested by indentation, links, block
   /// quotes, rules and tables - and anything else as its text. The same
   /// reading, rule for rule, as the webmail's PortalMarkdown.js and the copy
   /// of it in the Deck, so the three panes show one document one way; the
   /// heading list agrees with build/generate-help-pages.py, which is what
   /// build/check-help-map.py holds the help map's headings to.
   ///
   /// No WPF here: MarkdownFlow turns these blocks into a FlowDocument. This
   /// half is compiled into ControlPanel.Core and tested without a window.
   /// </summary>
   public static class MarkdownBlocks
   {
      private static readonly Regex Fence = new Regex(@"^ {0,3}(```|~~~)\s*([\w.+-]*)\s*$", RegexOptions.Compiled);
      private static readonly Regex Atx = new Regex(@"^ {0,3}(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$", RegexOptions.Compiled);
      private static readonly Regex SetextOne = new Regex(@"^ {0,3}=+[ \t]*$", RegexOptions.Compiled);
      private static readonly Regex SetextTwo = new Regex(@"^ {0,3}-+[ \t]*$", RegexOptions.Compiled);
      private static readonly Regex Rule = new Regex(@"^ {0,3}([-*_])[ \t]*(?:\1[ \t]*){2,}$", RegexOptions.Compiled);
      private static readonly Regex Bullet = new Regex(@"^( *)([-*+])[ \t]+(.*)$", RegexOptions.Compiled);
      private static readonly Regex Numbered = new Regex(@"^( *)(\d{1,9})[.)][ \t]+(.*)$", RegexOptions.Compiled);
      private static readonly Regex Quote = new Regex(@"^ {0,3}>[ ]?(.*)$", RegexOptions.Compiled);
      private static readonly Regex TableDelimiter = new Regex(@"^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$", RegexOptions.Compiled);
      private static readonly Regex LeadingSpaces = new Regex(@"^ *", RegexOptions.Compiled);

      // Code spans first, since nothing inside one is markup; then an image
      // (its alternative text), a link, an autolink, strong and emphasis.
      private static readonly Regex Inline = new Regex(
         @"(`+)(.*?[^`])\1(?!`)" +
         @"|!\[([^\]]*)\]\(([^)\s]*)(?:\s+""[^""]*"")?\)" +
         @"|\[([^\]]+)\]\(([^)\s]*)(?:\s+""[^""]*"")?\)" +
         @"|<((?:https?|mailto):[^>\s]+)>" +
         @"|(\*\*|__)(?=\S)(.*?\S)\8" +
         @"|(\*|_)(?=\S)(.*?\S)\10",
         RegexOptions.Compiled | RegexOptions.Singleline);

      private static readonly Regex DocLink = new Regex(@"^([A-Za-z0-9]+)\.md(?:#(.*))?$", RegexOptions.Compiled);
      private static readonly Regex Fragment = new Regex(@"^#[\w-]+$", RegexOptions.Compiled);

      public static IReadOnlyList<MdBlock> Parse(string text)
      {
         string[] lines = (text ?? "").Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
         return ParseBlocks(lines, 0);
      }

      /// <summary>The level-one and level-two headings, as the server lists them.</summary>
      public static IReadOnlyList<MdHeading> Headings(string text)
      {
         var found = new List<MdHeading>();
         foreach (MdBlock block in Parse(text))
         {
            if (block is MdHeading heading && heading.Level <= 2)
               found.Add(heading);
         }
         return found;
      }

      /// <summary>
      /// The pane's scroll target: the index of the heading with this text, or
      /// -1 for none - and -1 for no heading, which is the top of the document.
      /// </summary>
      public static int IndexOfHeading(IReadOnlyList<MdBlock> blocks, string heading)
      {
         if (string.IsNullOrEmpty(heading))
            return -1;

         for (int i = 0; i < blocks.Count; i++)
         {
            if (blocks[i] is MdHeading h && string.Equals(h.Text, heading, StringComparison.Ordinal))
               return i;
         }
         return -1;
      }

      /// <summary>
      /// Where a link may go. http, https and mailto are followed as they are;
      /// Name.md and Name.md#fragment name another document of the set; a
      /// fragment alone names a heading of this one; anything else - a path, a
      /// script, a protocol the pane does not know - is not a link.
      /// </summary>
      public static MdLinkTarget LinkTarget(string href)
      {
         string target = (href ?? "").Trim();
         if (target.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
             target.StartsWith("https://", StringComparison.OrdinalIgnoreCase) ||
             target.StartsWith("mailto:", StringComparison.OrdinalIgnoreCase))
            return new MdLinkTarget { Href = target };

         Match doc = DocLink.Match(target);
         if (doc.Success)
            return new MdLinkTarget { Href = "#", Doc = doc.Groups[1].Value, Heading = doc.Groups[2].Value };

         if (Fragment.IsMatch(target))
            return new MdLinkTarget { Href = target };

         return null;
      }

      /// <summary>
      /// The text of a line with its inline markup taken off - `What it does
      /// *not* do` as What it does not do - for a label that is a string rather
      /// than rendered runs: the headings list's buttons, the pane's title.
      /// </summary>
      public static string PlainText(string text)
      {
         var plain = new System.Text.StringBuilder();
         void Append(IReadOnlyList<MdInline> inlines)
         {
            foreach (MdInline inline in inlines)
            {
               if (inline.Kind == MdInlineKind.Text || inline.Kind == MdInlineKind.Code)
                  plain.Append(inline.Text);
               else
                  Append(inline.Children);
            }
         }
         Append(Inlines(text));
         return plain.ToString();
      }

      /// <summary>The runs of one line of text.</summary>
      public static IReadOnlyList<MdInline> Inlines(string text)
      {
         var into = new List<MdInline>();
         InlinesInto(into, text ?? "");
         return into;
      }

      private static bool IsBlank(string line) => string.IsNullOrWhiteSpace(line);
      private static bool IsListStart(string line) => Bullet.IsMatch(line) || Numbered.IsMatch(line);

      /// <summary>
      /// Whether a list that starts on this line ends the paragraph above it: a
      /// bulleted one does, and a numbered one only when it starts at 1, as
      /// CommonMark has it - so that a line of prose that wraps to begin with a
      /// number and a full stop, "0. A silent installation's move then waits",
      /// stays in its paragraph. It became a list starting at 0, which a
      /// FlowDocument cannot number, and the help pane threw on ServiceAccount.md
      /// and SourcePackages.md.
      /// </summary>
      private static bool StartsListInParagraph(string line)
      {
         if (Bullet.IsMatch(line))
            return true;

         Match numbered = Numbered.Match(line);
         return numbered.Success && numbered.Groups[2].Value == "1";
      }
      private static bool IsTableRow(string line) => line.IndexOf('|') >= 0;
      private static int Lead(string line) => LeadingSpaces.Match(line).Length;

      /// <summary>The cells of a table row: the outer pipes dropped, a pipe inside a code span kept.</summary>
      public static List<string> SplitRow(string line)
      {
         var cells = new List<string>();
         var current = new System.Text.StringBuilder();
         bool inCode = false;
         string text = line.Trim();
         if (text.StartsWith("|"))
            text = text.Substring(1);
         if (text.EndsWith("|") && !text.EndsWith("\\|"))
            text = text.Substring(0, text.Length - 1);
         for (int i = 0; i < text.Length; i++)
         {
            char ch = text[i];
            if (ch == '`') { inCode = !inCode; current.Append(ch); }
            else if (ch == '\\' && i + 1 < text.Length && text[i + 1] == '|') { current.Append('|'); i++; }
            else if (ch == '|' && !inCode) { cells.Add(current.ToString().Trim()); current.Clear(); }
            else current.Append(ch);
         }
         cells.Add(current.ToString().Trim());
         return cells;
      }

      private static List<MdBlock> ParseBlocks(IList<string> lines, int offset)
      {
         var blocks = new List<MdBlock>();
         int i = 0;
         int n = lines.Count;
         while (i < n)
         {
            string line = lines[i];
            if (IsBlank(line)) { i++; continue; }

            Match fence = Fence.Match(line);
            if (fence.Success)
            {
               int start = offset + i;
               string mark = fence.Groups[1].Value;
               string language = fence.Groups[2].Value;
               var code = new List<string>();
               i++;
               while (i < n)
               {
                  Match close = Fence.Match(lines[i]);
                  if (close.Success && close.Groups[1].Value == mark && close.Groups[2].Value.Length == 0) { i++; break; }
                  code.Add(lines[i]);
                  i++;
               }
               blocks.Add(new MdCode { Text = string.Join("\n", code), Language = language, Line = start });
               continue;
            }

            Match atx = Atx.Match(line);
            if (atx.Success)
            {
               blocks.Add(new MdHeading { Level = atx.Groups[1].Value.Length, Text = atx.Groups[2].Value, Line = offset + i });
               i++;
               continue;
            }

            if (Rule.IsMatch(line))
            {
               blocks.Add(new MdRule { Line = offset + i });
               i++;
               continue;
            }

            if (Quote.IsMatch(line))
            {
               int quoteStart = offset + i;
               var inner = new List<string>();
               while (i < n)
               {
                  Match q = Quote.Match(lines[i]);
                  if (q.Success) { inner.Add(q.Groups[1].Value); i++; }
                  else if (!IsBlank(lines[i]) && inner.Count > 0 && !IsListStart(lines[i]) && !Fence.IsMatch(lines[i]) && !Atx.IsMatch(lines[i])) { inner.Add(lines[i]); i++; }
                  else break;
               }
               var quote = new MdQuote { Line = quoteStart };
               quote.Blocks.AddRange(ParseBlocks(inner, quoteStart));
               blocks.Add(quote);
               continue;
            }

            if (IsListStart(line))
            {
               int listStart = offset + i;
               Match first = Bullet.Match(line);
               bool ordered = false;
               if (!first.Success) { first = Numbered.Match(line); ordered = true; }
               int indent = first.Groups[1].Value.Length;
               var items = new List<(List<string> lines, int start)>();
               List<string> item = null;
               int itemStart = 0;
               int itemIndent = 0;
               while (i < n)
               {
                  string current = lines[i];
                  Match open = Bullet.Match(current);
                  bool openIsNumbered = false;
                  if (!open.Success) { open = Numbered.Match(current); openIsNumbered = open.Success; }
                  if (open.Success && open.Groups[1].Value.Length <= indent)
                  {
                     if (open.Groups[1].Value.Length < indent)
                        break;
                     if (ordered != openIsNumbered)
                        break;
                     if (item != null)
                        items.Add((item, itemStart));
                     itemIndent = current.Length - open.Groups[3].Value.Length;
                     item = new List<string> { open.Groups[3].Value };
                     itemStart = offset + i;
                     i++;
                     continue;
                  }
                  if (item == null)
                     break;
                  if (IsBlank(current))
                  {
                     string next = i + 1 < n ? lines[i + 1] : "";
                     Match nextOpen = Bullet.Match(next);
                     bool nextIsNumbered = false;
                     if (!nextOpen.Success) { nextOpen = Numbered.Match(next); nextIsNumbered = nextOpen.Success; }
                     bool continues = (!IsBlank(next) && Lead(next) >= itemIndent) ||
                        (nextOpen.Success && nextOpen.Groups[1].Value.Length == indent && ordered == nextIsNumbered);
                     if (!continues)
                        break;
                     item.Add("");
                     i++;
                     continue;
                  }
                  int lead = Lead(current);
                  if (lead >= itemIndent) { item.Add(current.Substring(itemIndent)); i++; continue; }
                  if (open.Success)
                     break;
                  if (Fence.IsMatch(current) || Atx.IsMatch(current) || (IsTableRow(current) && i + 1 < n && IsTableRow(lines[i + 1]) && TableDelimiter.IsMatch(lines[i + 1])))
                     break;
                  item.Add(current.Trim());
                  i++;
               }
               if (item != null)
                  items.Add((item, itemStart));
               var list = new MdList { Ordered = ordered, Start = ordered ? int.Parse(first.Groups[2].Value) : 1, Line = listStart };
               foreach ((List<string> entryLines, int entryStart) in items)
                  list.Items.Add(ParseBlocks(entryLines, entryStart));
               blocks.Add(list);
               continue;
            }

            if (IsTableRow(line) && i + 1 < n && TableDelimiter.IsMatch(lines[i + 1]) && IsTableRow(lines[i + 1]))
            {
               var table = new MdTable { Line = offset + i };
               table.Header.AddRange(SplitRow(line));
               i += 2;
               while (i < n && !IsBlank(lines[i]) && IsTableRow(lines[i])) { table.Rows.Add(SplitRow(lines[i])); i++; }
               blocks.Add(table);
               continue;
            }

            // A paragraph: lines until a blank one or the start of something
            // else. A setext underline under it makes the line above it a
            // heading, and the lines before that a paragraph of their own.
            int paragraphStart = offset + i;
            var text = new List<string>();
            while (i < n)
            {
               string candidate = lines[i];
               if (IsBlank(candidate))
                  break;
               if (text.Count > 0 && (SetextOne.IsMatch(candidate) || SetextTwo.IsMatch(candidate)))
               {
                  string headingText = text[text.Count - 1];
                  text.RemoveAt(text.Count - 1);
                  if (text.Count > 0)
                     blocks.Add(new MdParagraph { Text = string.Join(" ", text), Line = paragraphStart });
                  blocks.Add(new MdHeading { Level = SetextOne.IsMatch(candidate) ? 1 : 2, Text = headingText, Line = offset + i - 1 });
                  text.Clear();
                  i++;
                  break;
               }
               if (text.Count > 0 && (Fence.IsMatch(candidate) || Atx.IsMatch(candidate) || StartsListInParagraph(candidate) || Quote.IsMatch(candidate) || Rule.IsMatch(candidate)))
                  break;
               if (text.Count > 0 && IsTableRow(candidate) && i + 1 < n && IsTableRow(lines[i + 1]) && TableDelimiter.IsMatch(lines[i + 1]))
                  break;
               text.Add(candidate.Trim());
               i++;
            }
            if (text.Count > 0)
               blocks.Add(new MdParagraph { Text = string.Join(" ", text), Line = paragraphStart });
         }
         return blocks;
      }

      private static void InlinesInto(List<MdInline> into, string text)
      {
         string rest = text;
         var buffer = new System.Text.StringBuilder();
         void Flush()
         {
            if (buffer.Length > 0)
            {
               into.Add(new MdInline { Kind = MdInlineKind.Text, Text = buffer.ToString() });
               buffer.Clear();
            }
         }

         while (rest.Length > 0)
         {
            Match m = Inline.Match(rest);
            if (!m.Success) { buffer.Append(rest); break; }

            // An underscore inside a word is a character, not emphasis: setting_name.
            if (m.Groups[10].Success && m.Groups[10].Value == "_" && m.Index > 0 && char.IsLetterOrDigit(rest[m.Index - 1]))
            {
               buffer.Append(rest.Substring(0, m.Index + 1));
               rest = rest.Substring(m.Index + 1);
               continue;
            }

            buffer.Append(rest.Substring(0, m.Index));
            Flush();
            if (m.Groups[1].Success)
            {
               string code = m.Groups[2].Value;
               if (code.Length > 2 && code.StartsWith(" ") && code.EndsWith(" "))
                  code = code.Substring(1, code.Length - 2);
               into.Add(new MdInline { Kind = MdInlineKind.Code, Text = code });
            }
            else if (m.Groups[3].Success)
            {
               buffer.Append(m.Groups[3].Value);
            }
            else if (m.Groups[5].Success)
            {
               MdLinkTarget target = LinkTarget(m.Groups[6].Value);
               if (target != null)
               {
                  var link = new MdInline { Kind = MdInlineKind.Link, Target = target };
                  InlinesInto(link.Children, m.Groups[5].Value);
                  into.Add(link);
               }
               else
               {
                  InlinesInto(into, m.Groups[5].Value);
               }
            }
            else if (m.Groups[7].Success)
            {
               MdLinkTarget auto = LinkTarget(m.Groups[7].Value);
               if (auto != null)
               {
                  var link = new MdInline { Kind = MdInlineKind.Link, Target = auto };
                  link.Children.Add(new MdInline { Kind = MdInlineKind.Text, Text = m.Groups[7].Value });
                  into.Add(link);
               }
               else
               {
                  buffer.Append(m.Groups[7].Value);
               }
            }
            else if (m.Groups[8].Success)
            {
               var strong = new MdInline { Kind = MdInlineKind.Strong };
               InlinesInto(strong.Children, m.Groups[9].Value);
               into.Add(strong);
            }
            else if (m.Groups[10].Success)
            {
               var em = new MdInline { Kind = MdInlineKind.Emphasis };
               InlinesInto(em.Children, m.Groups[11].Value);
               into.Add(em);
            }
            rest = rest.Substring(m.Index + m.Length);
         }
         Flush();
      }
   }
}
