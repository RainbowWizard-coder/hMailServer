// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Markdown for the help pane, into DOM nodes and never into innerHTML: the
// subset the operator documentation (hmailserver/docs/*.md) is written in -
// headings (ATX and setext), paragraphs, emphasis and strong, inline code,
// fenced code blocks, bulleted and numbered lists (nested by indentation),
// links, block quotes, rules and tables - and anything else as its text. Every
// character of the document reaches the page through createTextNode, so the
// text can carry nothing the page runs; a link is followed only when it is
// http, https or mailto (opened in a new tab) or names another document
// (Backup.md, or Backup.md#a-heading), which the pane opens in place.
//
// Served as /portal-markdown.js and loaded by the webmail beside its S/MIME
// module; also loadable in Node (module.exports), which is how
// build/portal-script-test.js runs it. The Control Deck carries a copy of the
// same code inline in hmailserver/installation/WebAdmin/index.html, because
// its page's policy allows no script but its own inline one; the two are kept
// the same by hand, and a change here is a change there. The Control Panel's
// Services/MarkdownBlocks.cs reads the same subset the same way into a
// FlowDocument.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) { module.exports = factory(); }
  else { root.hmMarkdown = factory(); }
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var FENCE = /^ {0,3}(```|~~~)\s*([\w.+-]*)\s*$/;
  var ATX = /^ {0,3}(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$/;
  var SETEXT_ONE = /^ {0,3}=+[ \t]*$/;
  var SETEXT_TWO = /^ {0,3}-+[ \t]*$/;
  var RULE = /^ {0,3}([-*_])[ \t]*(?:\1[ \t]*){2,}$/;
  var BULLET = /^( *)([-*+])[ \t]+(.*)$/;
  var NUMBERED = /^( *)(\d{1,9})[.)][ \t]+(.*)$/;
  var QUOTE = /^ {0,3}>[ ]?(.*)$/;
  var TABLE_DELIMITER = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/;

  var isBlank = function (line) { return !line || !line.trim(); };
  var isListStart = function (line) { return BULLET.test(line) || NUMBERED.test(line); };
  var isTableRow = function (line) { return line.indexOf('|') >= 0; };

  // The cells of a table row: the outer pipes dropped, a pipe inside a code
  // span kept as the character it is.
  var splitRow = function (line) {
    var cells = [];
    var current = '';
    var inCode = false;
    var text = line.trim();
    if (text.charAt(0) === '|') { text = text.slice(1); }
    if (text.charAt(text.length - 1) === '|' && text.charAt(text.length - 2) !== '\\') { text = text.slice(0, -1); }
    for (var i = 0; i < text.length; i++) {
      var ch = text.charAt(i);
      if (ch === '`') { inCode = !inCode; current += ch; }
      else if (ch === '\\' && text.charAt(i + 1) === '|') { current += '|'; i++; }
      else if (ch === '|' && !inCode) { cells.push(current.trim()); current = ''; }
      else { current += ch; }
    }
    cells.push(current.trim());
    return cells;
  };

  // ---- Blocks ---------------------------------------------------------------
  // parse(text) -> an array of blocks, each with the line it starts on:
  //   {type:'heading', level, text, line}
  //   {type:'paragraph', text, line}
  //   {type:'code', text, language, line}
  //   {type:'list', ordered, start, items:[[block...]], line}
  //   {type:'quote', blocks:[block...], line}
  //   {type:'table', header:[text...], rows:[[text...]], line}
  //   {type:'rule', line}
  var parseBlocks = function (lines, offset) {
    var blocks = [];
    var i = 0;
    var n = lines.length;
    var at = function () { return offset + i; };
    while (i < n) {
      var line = lines[i];
      if (isBlank(line)) { i++; continue; }

      var fence = FENCE.exec(line);
      if (fence) {
        var start = at();
        var mark = fence[1];
        var language = fence[2] || '';
        var code = [];
        i++;
        while (i < n) {
          var close = FENCE.exec(lines[i]);
          if (close && close[1] === mark && !close[2]) { i++; break; }
          code.push(lines[i]);
          i++;
        }
        blocks.push({ type: 'code', text: code.join('\n'), language: language, line: start });
        continue;
      }

      var atx = ATX.exec(line);
      if (atx) {
        blocks.push({ type: 'heading', level: atx[1].length, text: atx[2], line: at() });
        i++;
        continue;
      }

      if (RULE.test(line)) {
        // A run of dashes under a paragraph line is a setext heading, read
        // below when the paragraph is gathered; here the line stands alone.
        blocks.push({ type: 'rule', line: at() });
        i++;
        continue;
      }

      var quote = QUOTE.exec(line);
      if (quote) {
        var quoteStart = at();
        var inner = [];
        while (i < n) {
          var q = QUOTE.exec(lines[i]);
          if (q) { inner.push(q[1]); i++; }
          else if (!isBlank(lines[i]) && inner.length && !isListStart(lines[i]) && !FENCE.test(lines[i]) && !ATX.test(lines[i])) { inner.push(lines[i]); i++; }
          else { break; }
        }
        blocks.push({ type: 'quote', blocks: parseBlocks(inner, quoteStart), line: quoteStart });
        continue;
      }

      if (isListStart(line)) {
        var listStart = at();
        var first = BULLET.exec(line) || NUMBERED.exec(line);
        var ordered = !!NUMBERED.exec(line);
        var indent = first[1].length;
        var items = [];
        var item = null;
        var itemIndent = 0;
        while (i < n) {
          var current = lines[i];
          var open = BULLET.exec(current) || NUMBERED.exec(current);
          if (open && open[1].length <= indent) {
            if (open[1].length < indent) { break; }
            if ((ordered && !NUMBERED.exec(current)) || (!ordered && !BULLET.exec(current))) { break; }
            if (item) { items.push(item); }
            // The item's own text is indented to where its text began, so a
            // line under it that is indented that far belongs to it.
            itemIndent = current.length - open[3].length;
            item = { lines: [open[3]], start: at() };
            i++;
            continue;
          }
          if (!item) { break; }
          if (isBlank(current)) {
            // A blank line ends the list unless the next line is indented
            // under the item (a second paragraph, a nested list) or another
            // item of this list.
            var next = i + 1 < n ? lines[i + 1] : '';
            var nextOpen = BULLET.exec(next) || NUMBERED.exec(next);
            var continues = (!isBlank(next) && next.length - next.replace(/^ */, '').length >= itemIndent) ||
              (nextOpen && nextOpen[1].length === indent && ((ordered && NUMBERED.exec(next)) || (!ordered && BULLET.exec(next))));
            if (!continues) { break; }
            item.lines.push('');
            i++;
            continue;
          }
          var lead = current.length - current.replace(/^ */, '').length;
          if (lead >= itemIndent) { item.lines.push(current.slice(itemIndent)); i++; continue; }
          if (open) { break; }
          if (FENCE.test(current) || ATX.test(current) || (isTableRow(current) && i + 1 < n && isTableRow(lines[i + 1]) && TABLE_DELIMITER.test(lines[i + 1]))) { break; }
          // A lazy continuation of the item's paragraph.
          item.lines.push(current.trim());
          i++;
        }
        if (item) { items.push(item); }
        blocks.push({
          type: 'list', ordered: ordered, start: ordered ? parseInt(first[2], 10) : 1,
          items: items.map(function (entry) { return parseBlocks(entry.lines, entry.start); }),
          line: listStart
        });
        continue;
      }

      if (isTableRow(line) && i + 1 < n && TABLE_DELIMITER.test(lines[i + 1]) && isTableRow(lines[i + 1])) {
        var tableStart = at();
        var header = splitRow(line);
        var rows = [];
        i += 2;
        while (i < n && !isBlank(lines[i]) && isTableRow(lines[i])) { rows.push(splitRow(lines[i])); i++; }
        blocks.push({ type: 'table', header: header, rows: rows, line: tableStart });
        continue;
      }

      // A paragraph: lines until a blank one or the start of something else.
      // A setext underline under it makes the line above it a heading, and
      // the lines before that a paragraph of their own.
      var paragraphStart = at();
      var text = [];
      while (i < n) {
        var candidate = lines[i];
        if (isBlank(candidate)) { break; }
        if (text.length && (SETEXT_ONE.test(candidate) || SETEXT_TWO.test(candidate))) {
          var headingText = text.pop();
          if (text.length) { blocks.push({ type: 'paragraph', text: text.join(' '), line: paragraphStart }); }
          blocks.push({ type: 'heading', level: SETEXT_ONE.test(candidate) ? 1 : 2, text: headingText, line: at() - 1 });
          text = [];
          i++;
          paragraphStart = at();
          break;
        }
        if (text.length && (FENCE.test(candidate) || ATX.test(candidate) || isListStart(candidate) || QUOTE.test(candidate) || RULE.test(candidate))) { break; }
        if (text.length && isTableRow(candidate) && i + 1 < n && isTableRow(lines[i + 1]) && TABLE_DELIMITER.test(lines[i + 1])) { break; }
        text.push(candidate.trim());
        i++;
      }
      if (text.length) { blocks.push({ type: 'paragraph', text: text.join(' '), line: paragraphStart }); }
    }
    return blocks;
  };

  var parse = function (text) {
    return parseBlocks(String(text || '').replace(/\r\n?/g, '\n').split('\n'), 0);
  };

  // The level-one and level-two headings, as the server lists them.
  var headings = function (text) {
    var found = [];
    parse(text).forEach(function (block) {
      if (block.type === 'heading' && block.level <= 2) { found.push({ text: block.text, level: block.level, line: block.line }); }
    });
    return found;
  };

  // ---- Inlines --------------------------------------------------------------
  // Code spans first, since nothing inside one is markup; then links, strong,
  // emphasis and autolinks, left to right; an image is its alternative text.
  var inlinesInto = function (doc, parent, text, linkOf) {
    var rest = String(text || '');
    var buffer = '';
    var flush = function () { if (buffer) { parent.appendChild(doc.createTextNode(buffer)); buffer = ''; } };
    var pattern = /(`+)([\s\S]*?[^`])\1(?!`)|!\[([^\]]*)\]\(([^)\s]*)(?:\s+"[^"]*")?\)|\[([^\]]+)\]\(([^)\s]*)(?:\s+"[^"]*")?\)|<((?:https?|mailto):[^>\s]+)>|(\*\*|__)(?=\S)([\s\S]*?\S)\8|(\*|_)(?=\S)([\s\S]*?\S)\10/;
    while (rest.length) {
      var m = pattern.exec(rest);
      if (!m) { buffer += rest; break; }
      // An underscore inside a word is a character, not emphasis: setting_name.
      if (m[10] === '_' && m.index > 0 && /\w/.test(rest.charAt(m.index - 1))) {
        buffer += rest.slice(0, m.index + 1);
        rest = rest.slice(m.index + 1);
        continue;
      }
      buffer += rest.slice(0, m.index);
      flush();
      if (m[1]) {
        var code = doc.createElement('code');
        code.appendChild(doc.createTextNode(m[2].replace(/^ (.+) $/, '$1')));
        parent.appendChild(code);
      } else if (m[3] !== undefined) {
        buffer += m[3];
      } else if (m[5] !== undefined) {
        var target = linkOf(m[6]);
        if (target) {
          var a = doc.createElement('a');
          a.setAttribute('href', target.href);
          if (target.doc) { a.setAttribute('data-doc', target.doc); if (target.heading) { a.setAttribute('data-heading', target.heading); } }
          else { a.setAttribute('target', '_blank'); a.setAttribute('rel', 'noopener noreferrer'); }
          inlinesInto(doc, a, m[5], linkOf);
          parent.appendChild(a);
        } else {
          inlinesInto(doc, parent, m[5], linkOf);
        }
      } else if (m[7]) {
        var auto = linkOf(m[7]);
        if (auto) {
          var link = doc.createElement('a');
          link.setAttribute('href', auto.href);
          link.setAttribute('target', '_blank');
          link.setAttribute('rel', 'noopener noreferrer');
          link.appendChild(doc.createTextNode(m[7]));
          parent.appendChild(link);
        } else { buffer += m[7]; }
      } else if (m[8]) {
        var strong = doc.createElement('strong');
        inlinesInto(doc, strong, m[9], linkOf);
        parent.appendChild(strong);
      } else if (m[10]) {
        var em = doc.createElement('em');
        inlinesInto(doc, em, m[11], linkOf);
        parent.appendChild(em);
      }
      rest = rest.slice(m.index + m[0].length);
    }
    flush();
  };

  // Where a link may go. http, https and mailto are followed as they are;
  // Name.md and Name.md#fragment name another document of the set, which the
  // pane opens; a fragment alone names a heading of this one; anything else -
  // a path, a script, a protocol the pane does not know - is not a link.
  var defaultLink = function (href) {
    var target = String(href || '').trim();
    if (/^(https?|mailto):/i.test(target)) { return { href: target }; }
    var doc = /^([A-Za-z0-9]+)\.md(?:#(.*))?$/.exec(target);
    if (doc) { return { href: '#', doc: doc[1], heading: doc[2] || '' }; }
    if (/^#[\w-]+$/.test(target)) { return { href: target }; }
    return null;
  };

  var slug = function (text) {
    return String(text || '').toLowerCase().replace(/`/g, '').replace(/[^\wÀ-￿]+/g, '-').replace(/^-+|-+$/g, '');
  };

  // ---- Rendering ------------------------------------------------------------
  var renderBlocks = function (doc, parent, blocks, linkOf) {
    blocks.forEach(function (block) {
      var node;
      if (block.type === 'heading') {
        node = doc.createElement('h' + Math.min(6, block.level + 1));
        node.setAttribute('id', 'help-' + slug(block.text));
        node.setAttribute('data-line', String(block.line));
        node.setAttribute('data-level', String(block.level));
        inlinesInto(doc, node, block.text, linkOf);
      } else if (block.type === 'paragraph') {
        node = doc.createElement('p');
        inlinesInto(doc, node, block.text, linkOf);
      } else if (block.type === 'code') {
        node = doc.createElement('pre');
        var code = doc.createElement('code');
        if (block.language) { code.setAttribute('data-language', block.language); }
        code.appendChild(doc.createTextNode(block.text));
        node.appendChild(code);
      } else if (block.type === 'list') {
        node = doc.createElement(block.ordered ? 'ol' : 'ul');
        if (block.ordered && block.start !== 1) { node.setAttribute('start', String(block.start)); }
        block.items.forEach(function (item) {
          var li = doc.createElement('li');
          // One paragraph in an item is the item's text; more keeps its blocks.
          if (item.length === 1 && item[0].type === 'paragraph') { inlinesInto(doc, li, item[0].text, linkOf); }
          else { renderBlocks(doc, li, item, linkOf); }
          node.appendChild(li);
        });
      } else if (block.type === 'quote') {
        node = doc.createElement('blockquote');
        renderBlocks(doc, node, block.blocks, linkOf);
      } else if (block.type === 'table') {
        node = doc.createElement('table');
        var thead = doc.createElement('thead');
        var headRow = doc.createElement('tr');
        block.header.forEach(function (cell) { var th = doc.createElement('th'); inlinesInto(doc, th, cell, linkOf); headRow.appendChild(th); });
        thead.appendChild(headRow);
        node.appendChild(thead);
        var tbody = doc.createElement('tbody');
        block.rows.forEach(function (row) {
          var tr = doc.createElement('tr');
          for (var c = 0; c < block.header.length; c++) { var td = doc.createElement('td'); inlinesInto(doc, td, row[c] || '', linkOf); tr.appendChild(td); }
          tbody.appendChild(tr);
        });
        node.appendChild(tbody);
      } else if (block.type === 'rule') {
        node = doc.createElement('hr');
      }
      if (node) {
        if (block.type !== 'heading') { node.setAttribute('data-line', String(block.line)); }
        parent.appendChild(node);
      }
    });
  };

  // render(text, doc, linkOf) -> a fragment of doc holding the document.
  var render = function (text, doc, linkOf) {
    var fragment = doc.createDocumentFragment ? doc.createDocumentFragment() : doc.createElement('div');
    renderBlocks(doc, fragment, parse(text), linkOf || defaultLink);
    return fragment;
  };

  return { parse: parse, headings: headings, render: render, slug: slug, link: defaultLink, splitRow: splitRow };
}));
