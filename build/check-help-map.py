#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every page of the three surfaces has help, and every entry of help is real.

Each surface maps its pages to the operator documentation (hmailserver/docs,
embedded by build/generate-help-pages.py) in a table of its own:

  hmailserver/source/Tools/ControlPanel/Services/HelpMap.cs
      ["key"] = new HelpTarget("Doc.md", "Heading"),  keyed by the navigation
      keys of Services/NavigationMap.cs - the nodes the palette indexes; and
      ReferenceOnly, the documents no page of any surface maps to.
  hmailserver/installation/WebAdmin/index.html
      const HELP_MAP={key:["Doc.md","Heading"],...};  keyed by every
      data-view section of the Deck.
  hmailserver/source/Server/Common/Util/Portal.js
      var HELP_MAP = { key: ['Doc.md', 'Heading'], ... };  keyed by every
      route head the webmail's router distinguishes (head === 'x').

Checked:
  pages     every page of each surface has an entry, and no entry names a
            page the surface does not have;
  files     every entry names a document that exists and is one the
            generator embeds (not README.md, not DeckParity.md);
  headings  every heading named is a level-one or level-two heading of its
            document, read by the generator's own parser, so a heading the
            checker accepts is one the server lists and the panes scroll to;
  reach     every document is named by some surface or listed as reference-
            only, and nothing listed as reference-only is named - a document
            in neither is one the help cannot reach, a document in both is
            not reference-only.

What it reads the pages from, and so what it would not notice: a Control
Panel page is a Page("key" call in NavigationMap.cs (HelpMapTests holds the
same table from inside, against NavigationMap.Pages); a Deck view is a
data-view="..." attribute in index.html, so a section drawn by the script
rather than written in the markup would be missed; a webmail page is an
if (head === '...') test in Portal.js's route(), so a route written as a
case label or looked up in a table would be missed. Keep the three spelled
as they are, or teach this file the new spelling in the same change.

Usage:  python build/check-help-map.py [--self-test]
  --self-test runs the same check over a copy of the maps with an entry
  naming a heading that is not there and one naming a document that is not,
  and fails unless both are refused: the negative control.
"""
import importlib.util
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONTROL_PANEL = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Services")
HELP_MAP_CS = os.path.join(CONTROL_PANEL, "HelpMap.cs")
NAVIGATION_CS = os.path.join(CONTROL_PANEL, "NavigationMap.cs")
DECK = os.path.join(ROOT, "hmailserver", "installation", "WebAdmin", "index.html")
PORTAL = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util", "Portal.js")

CS_ENTRY = re.compile(r'\["([A-Za-z0-9_-]+)"\]\s*=\s*new HelpTarget\("([^"]+)"(?:,\s*"([^"]*)")?\)')
CS_REFERENCE = re.compile(r"ReferenceOnly\s*=\s*new\[\]\s*\{(.*?)\};", re.S)
CS_NAVIGATION_KEY = re.compile(r'\bPage\("([A-Za-z0-9_-]+)"')
DECK_TABLE = re.compile(r"const HELP_MAP=\{(.*?)\};", re.S)
DECK_ENTRY = re.compile(r'([A-Za-z0-9_-]+):\["([^"]+)"(?:,"([^"]*)")?\]')
DECK_VIEW = re.compile(r'data-view="([A-Za-z0-9_-]+)"')
PORTAL_TABLE = re.compile(r"var HELP_MAP = \{(.*?)\};", re.S)
PORTAL_ENTRY = re.compile(r"([A-Za-z0-9_-]+): \['([^']+)'(?:, '([^']*)')?\]")
PORTAL_ROUTE = re.compile(r"head === '([A-Za-z0-9_-]+)'")


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_help_pages", os.path.join(HERE, "generate-help-pages.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(path):
    with io.open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def parse_maps():
    """{surface: {key: (doc, heading)}}, {surface: set(pages)}, [reference-only docs]."""
    cs = read(HELP_MAP_CS)
    maps = {"Control Panel": {}, "Deck": {}, "webmail": {}}
    for key, doc, heading in CS_ENTRY.findall(cs):
        maps["Control Panel"][key] = (doc, heading or None)
    reference = []
    block = CS_REFERENCE.search(cs)
    if block:
        reference = re.findall(r'"([^"]+)"', block.group(1))

    deck = read(DECK)
    table = DECK_TABLE.search(deck)
    if table:
        for key, doc, heading in DECK_ENTRY.findall(table.group(1)):
            maps["Deck"][key] = (doc, heading or None)

    portal = read(PORTAL)
    table = PORTAL_TABLE.search(portal)
    if table:
        for key, doc, heading in PORTAL_ENTRY.findall(table.group(1)):
            maps["webmail"][key] = (doc, heading or None)

    pages = {
        "Control Panel": set(CS_NAVIGATION_KEY.findall(read(NAVIGATION_CS))),
        "Deck": set(DECK_VIEW.findall(deck)),
        "webmail": set(PORTAL_ROUTE.findall(portal)),
    }
    return maps, pages, reference


def check(maps, pages, reference, generator):
    problems = []
    docs = set(name + ".md" for name in generator.page_names())
    headings_of = {}

    def headings(doc):
        if doc not in headings_of:
            headings_of[doc] = set(text for text, level, line in generator.headings(generator.read_page(doc[:-3])))
        return headings_of[doc]

    for surface, entries in maps.items():
        if not entries:
            problems.append("%s: no help map was found" % surface)
            continue
        missing = sorted(pages[surface] - set(entries))
        extra = sorted(set(entries) - pages[surface])
        for key in missing:
            problems.append("%s: the page %r has no help entry" % (surface, key))
        for key in extra:
            problems.append("%s: the help entry %r names no page of the surface" % (surface, key))
        for key, (doc, heading) in sorted(entries.items()):
            if doc not in docs:
                problems.append("%s: %r names %s, which is not a document the generator embeds" % (surface, key, doc))
                continue
            if heading is not None and heading not in headings(doc):
                problems.append("%s: %r names the heading %r, which is not in %s" % (surface, key, heading, doc))

    named = set(doc for entries in maps.values() for doc, heading in entries.values())
    for doc in sorted(docs):
        if doc not in named and doc not in reference:
            problems.append("reach: %s is mapped by no surface and is not listed as reference-only in HelpMap.cs" % doc)
        if doc in named and doc in reference:
            problems.append("reach: %s is listed as reference-only in HelpMap.cs but a page maps it" % doc)
    for doc in reference:
        if doc not in docs:
            problems.append("reach: the reference-only list names %s, which is not a document" % doc)
    return problems


def self_test(maps, pages, reference, generator):
    """The negative control: a map with a heading that is not there, and one
    with a document that is not, each refused by name."""
    failures = []
    surface = "Deck"
    key = sorted(maps[surface])[0]
    doc, heading = maps[surface][key]
    wrong = {s: dict(e) for s, e in maps.items()}
    wrong[surface][key] = (doc, "A heading that is not there")
    found = [p for p in check(wrong, pages, reference, generator) if "A heading that is not there" in p]
    if not found:
        failures.append("an entry naming a heading that is not there was accepted")
    wrong = {s: dict(e) for s, e in maps.items()}
    wrong[surface][key] = ("NoSuchDocument.md", None)
    found = [p for p in check(wrong, pages, reference, generator) if "NoSuchDocument.md" in p]
    if not found:
        failures.append("an entry naming a document that does not exist was accepted")
    return failures


def main():
    generator = load_generator()
    maps, pages, reference = parse_maps()
    if "--self-test" in sys.argv:
        failures = self_test(maps, pages, reference, generator)
        for failure in failures:
            print("  FAIL  " + failure)
        if failures:
            return 1
        print("check-help-map self-test: the negative control is refused")
        return 0
    problems = check(maps, pages, reference, generator)
    for problem in problems:
        print("  FAIL  " + problem)
    if problems:
        print("%d problem(s)" % len(problems))
        return 1
    print("help map: %d Control Panel pages, %d Deck views, %d webmail pages mapped; %d documents, %d reference-only" % (
        len(maps["Control Panel"]), len(maps["Deck"]), len(maps["webmail"]), len(generator.page_names()), len(reference)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
