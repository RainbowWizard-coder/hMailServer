#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Embed the operator documentation in the server.

Reads every hmailserver/docs/*.md except README.md (the index of the others)
and DeckParity.md (a census build/check-deck-parity.py rewrites) and writes
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

The table ends with a build tag, the first sixteen hex digits of the SHA-256
of every page's text in table order, which the routes serve as the ETag: a
browser that holds a page asks again with If-None-Match and is answered 304
until the documentation changes in the build.

The heading parser here is the one build/check-help-map.py imports to hold
the three surfaces' help maps to the documents, so a heading the checker
accepts is a heading the server lists, by construction.

Usage: generate-help-pages.py [--print | --check | --headings]
  --print     writes the unit to stdout
  --check     exits 1 when the committed unit is not what the documents would make
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
PIECE = 4000

# The index of the others, and a census a checker rewrites: neither is a page
# anybody is helped by.
EXCLUDED = {"README.md", "DeckParity.md"}

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


def build_tag(names):
    digest = hashlib.sha256()
    for name in names:
        digest.update(read_page(name).encode("utf-8"))
    return digest.hexdigest()[:16]


def generate():
    names = page_names()
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
        out.append("      { %s, %d, Page%dPieces, Page%dHeadings }," % (cpp_literal(name), len(text.encode("utf-8")), index, index))
    out.append("      { nullptr, 0, nullptr, nullptr }")
    out.append("   };")
    out.append("")
    out.append('   extern const char *const HelpPagesTag = "%s";' % build_tag(names))
    out.append("}")
    out.append("")
    return "\n".join(out)


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
    if "--check" in sys.argv:
        current = io.open(OUT, "r", encoding="utf-8").read().replace("\r\n", "\n") if os.path.exists(OUT) else ""
        if current != text:
            print("HelpPagesData.cpp is not what the documents would generate; run build/generate-help-pages.py")
            return 1
        print("HelpPagesData.cpp is current")
        return 0
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote " + OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
