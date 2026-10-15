#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Embed the operator documentation in the server.

Reads every hmailserver/docs/*.md except README.md (the index of the others),
DeckParity.md (a census build/check-deck-parity.py rewrites),
WebAuthorisationMatrix.md (a table build/generate-auth-matrix.py writes from the
code, which would otherwise change this file whenever a route did) and
WebThreatModel.md (a model of the web surface for those who change and review
it, which names the surface's weak points: a page of the help is served to
anyone who asks, before any credential) and writes
hmailserver/source/Server/Common/Util/HelpPagesData.cpp: each document's text,
CRLF made LF, in pieces of at most four thousand characters (a compiler's limit
on one literal is sixteen thousand bytes; a piece of four thousand characters
is at most that many after the UTF-8 escapes, and the pieces are elements of an
array the server joins on first use), with a table of the file name without its
extension, the byte length of the text, and the document's headings - the
level-one and level-two ones, ATX (`#`, `##`) and setext (a line under `====`
or `----`), each with the line it starts on - so that GET /api/v1/help can list
every page with its headings without joining a byte, and a help pane can scroll
to the heading its page is mapped to. The same arrangement as the webmail's
page and build/generate-portal-page.py.

Each row also names the page of the project's wiki made from the document, or
none: the wiki's pages have names of their own (Backup-and-Restore, not
Backup), and build/help-wiki-pages.txt lists them with the documents each was
made from. GET /api/v1/help answers it as the page's wiki_page, which the
Control Deck's and the webmail's "Open on the wiki" add to the wiki base URL;
the same table is written for the Control Panel, which carries its own copy of
the documents, as Tools/ControlPanel/Services/HelpWikiPages.g.cs. A line of the
table that names a document the server does not carry, a document twice or a
page twice, or a page in a form no wiki name takes, stops the generator, as a
document with a name the server cannot answer does.

The table ends with a build tag, the first sixteen hex digits of the SHA-256
of every page's text and its wiki page in table order, which the routes serve
as the ETag: a browser that holds a page asks again with If-None-Match and is
answered 304 until the documentation, or the wiki page a document opens,
changes in the build.

The heading parser here is the one build/check-help-map.py imports to hold
the three surfaces' help maps to the documents, so a heading the checker
accepts is a heading the server lists, by construction; it reads the wiki
table through this file too.

Usage: generate-help-pages.py [--print | --check | --headings]
  --print     writes the server's unit to stdout
  --check     exits 1 when either committed file is not what the documents and
              the wiki table would make
  --headings  prints every page with its headings and their lines, for writing a map
"""

import hashlib
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "hmailserver", "docs")
OUT = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util", "HelpPagesData.cpp")
WIKI_TABLE = os.path.join(ROOT, "build", "help-wiki-pages.txt")
CS_OUT = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Services", "HelpWikiPages.g.cs")
PIECE = 4000

# A page of the wiki as GitLab names it: the file name at the top of the wiki's
# repository without .md - letters, digits, hyphens and underscores
# (Backup-and-Restore, _Sidebar). Anything else would be a path or need escaping
# in the address the panes make.
WIKI_PAGE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_-]{0,99}$")

# The index of the others, and a census a checker rewrites: neither is a page
# anybody is helped by. The authorisation matrix changes with every route; the
# threat model is for those who change and review the web surface and names its
# weak points, and /api/v1/help/{page} is answered before any credential.
EXCLUDED = {"README.md", "DeckParity.md", "WebAuthorisationMatrix.md", "WebThreatModel.md"}

ATX = re.compile(r"^(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$")
SETEXT_ONE = re.compile(r"^=+[ \t]*$")
SETEXT_TWO = re.compile(r"^-+[ \t]*$")
FENCE = re.compile(r"^[ \t]{0,3}(```|~~~)")
LIST_ITEM = re.compile(r"^[ \t]*(?:[-*+]|\d+[.)])[ \t]+")


# What the server takes for a page name (RestApiHelp.cpp, WellFormedPageName_):
# letters and digits, at most 64. A document named otherwise would be compiled in
# and never answered - GET /api/v1/help/Packages-deb was a 404 on 27 September 2026 -
# so it is refused here, where the generator and its --check both see it.
PAGE_NAME = re.compile(r"^[A-Za-z0-9]{1,64}$")


def page_names():
    """The documents, as names without the extension, in sorted order."""
    names = []
    for file in sorted(os.listdir(DOCS)):
        if file.endswith(".md") and file not in EXCLUDED:
            if not PAGE_NAME.match(file[:-3]):
                sys.exit("hmailserver/docs/%s: a help page's name is letters and digits only, which the server's "
                         "/api/v1/help/{page} answers; rename the document" % file)
            names.append(file[:-3])
    return names


def read_page(name):
    """The document's text with LF line endings."""
    with io.open(os.path.join(DOCS, name + ".md"), "r", encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


def read_wiki_table(path=WIKI_TABLE):
    """Every line of build/help-wiki-pages.txt that is not blank or a comment, in its order, as
    (page, [document file names], line number): a page of the wiki, then the documents it was made
    from, which may be none."""
    rows = []
    with io.open(path, "r", encoding="utf-8") as f:
        for number, line in enumerate(f.read().replace("\r\n", "\n").split("\n"), 1):
            fields = line.split("#", 1)[0].split()
            if fields:
                rows.append((fields[0], fields[1:], number))
    return rows


def wiki_table_problems(rows, names):
    """What is wrong with the wiki table's rows against the documents the help carries (names, without
    .md): a page in a form no wiki name takes, a page listed twice, a document that is not one the help
    carries, a document under two pages. Empty when nothing is."""
    problems = []
    carried = set(names)
    pages = {}
    documents = {}
    for page, docs, number in rows:
        where = "build/help-wiki-pages.txt:%d" % number
        if not WIKI_PAGE.match(page):
            problems.append("%s: %r is not the name of a wiki page (letters, digits, - and _)" % (where, page))
        if page in pages:
            problems.append("%s: the page %s is listed again, first on line %d" % (where, page, pages[page]))
        else:
            pages[page] = number
        for doc in docs:
            if not doc.endswith(".md") or doc[:-3] not in carried:
                problems.append("%s: %s names %s, which is not a document the help carries" % (where, page, doc))
            elif doc in documents:
                problems.append("%s: %s is listed under %s and under %s on line %d; a document has one wiki page"
                                % (where, doc, page, documents[doc][0], documents[doc][1]))
            else:
                documents[doc] = (page, number)
    return problems


def wiki_pages(names):
    """{document name without .md: the wiki page made from it}, for every document that has one. A table
    that is not right stops the generator, naming each line that is wrong."""
    rows = read_wiki_table()
    problems = wiki_table_problems(rows, names)
    if problems:
        sys.exit("build/help-wiki-pages.txt:\n  " + "\n  ".join(problems))
    return dict((doc[:-3], page) for page, docs, number in rows for doc in docs)


def headings(text):
    """(text, level, line) for every level-one and level-two heading, ATX or
    setext, outside fenced code. A setext underline counts only under a line
    that could be a paragraph: not blank, not a list item, not a table row,
    not itself a heading - so a rule after a list, and the |---| row of a
    table, are not headings."""
    found = []
    lines = text.split("\n")
    in_fence = None
    for index, line in enumerate(lines):
        fence = FENCE.match(line)
        if fence:
            if in_fence is None:
                in_fence = fence.group(1)
            elif fence.group(1) == in_fence:
                in_fence = None
            continue
        if in_fence:
            continue
        atx = ATX.match(line)
        if atx:
            level = len(atx.group(1))
            if level <= 2 and atx.group(2):
                found.append((atx.group(2), level, index))
            continue
        if index == 0:
            continue
        underline = 1 if SETEXT_ONE.match(line) else 2 if SETEXT_TWO.match(line) else 0
        if not underline:
            continue
        above = lines[index - 1]
        if not above.strip() or LIST_ITEM.match(above) or above.lstrip().startswith("|") or ATX.match(above):
            continue
        if SETEXT_ONE.match(above) or SETEXT_TWO.match(above) or FENCE.match(above):
            continue
        # A blank line before the candidate, or the file's start, makes it a
        # paragraph of one line; a line under a paragraph of several is the
        # last of them, and the heading is that line, as CommonMark reads it.
        found.append((above.strip(), underline, index - 1))
    return found


def cpp_literal(s):
    out = []
    for ch in s:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            continue
        elif ch == "\t":
            out.append("\\t")
        elif code < 0x20 or code > 0x7e:
            # Non-ASCII goes as its UTF-8 bytes in hex escapes, each followed by
            # a break so a following hex digit is not swallowed by the escape.
            for b in ch.encode("utf-8"):
                out.append("\\x%02x\"\"" % b)
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def build_tag(names, wiki):
    """Sixteen hex digits of the SHA-256 of every page's text and its wiki page, in table order: what the
    list answers changes with either, so the tag does."""
    digest = hashlib.sha256()
    for name in names:
        digest.update(read_page(name).encode("utf-8"))
        digest.update(b"\0" + wiki.get(name, "").encode("utf-8") + b"\0")
    return digest.hexdigest()[:16]


def generate():
    names = page_names()
    wiki = wiki_pages(names)
    out = []
    out.append("// https://www.progressiverobot.com")
    out.append("// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd")
    out.append("// SPDX-License-Identifier: AGPL-3.0-or-later")
    out.append("")
    out.append("// Generated by build/generate-help-pages.py from hmailserver/docs/*.md - do")
    out.append("// not edit; edit the documents and run the generator. Served by")
    out.append("// RestApiHelp.cpp at /api/v1/help and /api/v1/help/{page}.")
    out.append("")
    out.append('#include "StdAfx.h"')
    out.append('#include "HelpPages.h"')
    out.append("")
    out.append("namespace HM")
    out.append("{")
    out.append("   namespace")
    out.append("   {")
    for index, name in enumerate(names):
        text = read_page(name)
        if index:
            out.append("")
        out.append("      const char *const Page%dPieces[] =" % index)
        out.append("      {")
        for i in range(0, len(text), PIECE):
            out.append("         " + cpp_literal(text[i:i + PIECE]) + ",")
        out.append("         nullptr")
        out.append("      };")
        out.append("")
        out.append("      const HelpHeading Page%dHeadings[] =" % index)
        out.append("      {")
        for heading, level, line in headings(text):
            out.append("         { %s, %d, %d }," % (cpp_literal(heading), level, line))
        out.append("         { nullptr, 0, 0 }")
        out.append("      };")
    out.append("   }")
    out.append("")
    out.append("   extern const HelpPage HelpPages[] =")
    out.append("   {")
    for index, name in enumerate(names):
        text = read_page(name)
        out.append("      { %s, %d, Page%dPieces, Page%dHeadings, %s }," % (
            cpp_literal(name), len(text.encode("utf-8")), index, index, cpp_literal(wiki.get(name, ""))))
    out.append("      { nullptr, 0, nullptr, nullptr, nullptr }")
    out.append("   };")
    out.append("")
    out.append('   extern const char *const HelpPagesTag = "%s";' % build_tag(names, wiki))
    out.append("}")
    out.append("")
    return "\n".join(out)


def generate_cs():
    """The wiki table for the Control Panel, which carries its own copy of the documents and so its own
    copy of this: every document with a wiki page, in file-name order."""
    names = page_names()
    wiki = wiki_pages(names)
    out = []
    out.append("// https://www.progressiverobot.com")
    out.append("// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd")
    out.append("// SPDX-License-Identifier: AGPL-3.0-or-later")
    out.append("")
    out.append("// <auto-generated>")
    out.append("//     Generated by build/generate-help-pages.py from build/help-wiki-pages.txt.")
    out.append("//     Do not edit by hand - edit the table and re-run the script instead. The")
    out.append("//     server's HelpPagesData.cpp carries the same table, and GET /api/v1/help")
    out.append("//     answers it to the Control Deck and the webmail as each page's wiki_page.")
    out.append("// </auto-generated>")
    out.append("")
    out.append("using System;")
    out.append("using System.Collections.Generic;")
    out.append("using System.Collections.ObjectModel;")
    out.append("")
    out.append("namespace hMailServer.ControlPanel.Services")
    out.append("{")
    out.append("   /// <summary>")
    out.append("   /// The page of the project's wiki made from each document that has one, keyed by the")
    out.append("   /// document's name without .md: the wiki names its pages its own way")
    out.append("   /// (Backup-and-Restore for Backup), and a document no page was made from is not here,")
    out.append("   /// so it has no \"Open on the wiki\" (<see cref=\"HelpPages.WikiUrl\"/>).")
    out.append("   /// </summary>")
    out.append("   public static class HelpWikiPages")
    out.append("   {")
    out.append("      public static readonly IReadOnlyDictionary<string, string> ByDocument =")
    out.append("         new ReadOnlyDictionary<string, string>(new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)")
    out.append("         {")
    for name in names:
        if name in wiki:
            out.append('            ["%s"] = "%s",' % (name, wiki[name]))
    out.append("         });")
    out.append("   }")
    out.append("}")
    out.append("")
    return "\n".join(out)


def current(path):
    """A committed file's text with LF line endings, as the generator writes it; empty when it is not there."""
    return io.open(path, "r", encoding="utf-8").read().replace("\r\n", "\n") if os.path.exists(path) else ""


def main():
    if "--headings" in sys.argv:
        for name in page_names():
            print(name)
            for heading, level, line in headings(read_page(name)):
                print("   %s %s  (line %d)" % ("#" * level, heading, line))
        return 0
    text = generate()
    if "--print" in sys.argv:
        sys.stdout.write(text)
        return 0
    outputs = ((OUT, text), (CS_OUT, generate_cs()))
    if "--check" in sys.argv:
        stale = [path for path, wanted in outputs if current(path) != wanted]
        for path in stale:
            print("%s is not what the documents and build/help-wiki-pages.txt would generate; run "
                  "build/generate-help-pages.py" % os.path.relpath(path, ROOT).replace(os.sep, "/"))
        if stale:
            return 1
        print("HelpPagesData.cpp and HelpWikiPages.g.cs are current")
        return 0
    for path, wanted in outputs:
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(wanted)
        print("wrote " + path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
