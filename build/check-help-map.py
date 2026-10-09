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
CS_NO_GUIDE = re.compile(r'\["([A-Za-z0-9_-]+)"\]\s*=\s*HelpTarget\.NoGuide\b')
CS_TAB_ENTRY = re.compile(r'\["([A-Za-z0-9_-]+)#([^"]+)"\]\s*=\s*(?:new HelpTarget\("([^"]+)"(?:,\s*"([^"]*)")?\)|HelpTarget\.(NoGuide)\b)')
CS_REFERENCE = re.compile(r"ReferenceOnly\s*=\s*new\[\]\s*\{(.*?)\};", re.S)
CS_NAVIGATION_KEY = re.compile(r'\bPage\("([A-Za-z0-9_-]+)"')

# The Control Panel's tabs and cards, read from the views that build them, for the
# tab entries (each names a tab its page builds) and for the reference-only rule
# (a document whose title or name is a tab's or a card's heading belongs to that
# page's help, not to the list of documents no page opens).
VIEWS = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Views")
MAIN_WINDOW_CS = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "MainWindow.xaml.cs")
SERVER_SETTINGS_CS = os.path.join(VIEWS, "ServerSettingsView.xaml.cs")
FEATURE_SETTINGS_CS = os.path.join(VIEWS, "FeatureSettingsView.xaml.cs")
PAGE_SECTION = re.compile(r'pageFactories_\["([A-Za-z0-9_-]+)"\]\s*=\s*\(\)\s*=>\s*new ServerSettingsView\(ServerSettingsView\.Section\.(\w+)\)')
SECTION_BUILDER = re.compile(r'case Section\.(\w+):\s*(Build\w+)\(\);')
BUILDER = re.compile(r'private void (Build\w+)\(\)')
TAB_HEADER = re.compile(r'\bTab\(L\("((?:[^"\\]|\\.)*)"\)\)')
CARD_TITLE = re.compile(r'\bCard\(L\("((?:[^"\\]|\\.)*)"')
FEATURE_CARD_TITLE = re.compile(r'\bTitle = L\("((?:[^"\\]|\\.)*)"\)')

# A reference-only document whose title or name matches a heading of a page, but
# whose subject is another: the reason it stays reference-only, said where the rule
# is. Mirroring.md is the project's own repository mirror, not the server's
# mailboxes; nothing here is the server's.
NOT_THE_PAGES = {
    "Mirroring.md": "the project's push mirror and weekly export of its repository, not the server's mail",
}

# A reference-only document that is about one card of a tab whose own document
# links it, so that the pane reaches it from the tab in one step: the document
# that links it, which the checker holds to being mapped and to carrying the link.
# Account recovery is the fourth card of the webmail page's Sign-in tab, whose
# document is the passkeys and single sign-on one.
REACHED_FROM = {
    "AccountRecovery.md": "PasskeysAndSingleSignOn.md",
}
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
    """{surface: {key: (doc, heading)}}, {surface: set(pages)}, [reference-only docs], the Control Panel's
    tabs and cards. A Control Panel page with no guide maps to (None, None)."""
    cs = read(HELP_MAP_CS)
    maps = {"Control Panel": {}, "Deck": {}, "webmail": {}}
    for key, doc, heading in CS_ENTRY.findall(cs):
        maps["Control Panel"][key] = (doc, heading or None)
    for key in CS_NO_GUIDE.findall(cs):
        maps["Control Panel"][key] = (None, None)
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
    return maps, pages, reference, parse_control_panel(cs)


def parse_control_panel(help_map_cs):
    """The Control Panel's tab entries {(page, tab): (doc, heading)}, the tabs each settings page builds
    {page: set(tab)}, and every tab's and card's heading the views build."""
    tabs = {}
    for page, tab, doc, heading, no_guide in CS_TAB_ENTRY.findall(help_map_cs):
        tabs[(page, tab)] = (None, None) if no_guide else (doc, heading or None)

    view = read(SERVER_SETTINGS_CS)
    builders = {}
    marks = [(m.start(), m.group(1)) for m in BUILDER.finditer(view)] + [(len(view), None)]
    for (start, name), (end, _) in zip(marks, marks[1:]):
        builders[name] = view[start:end]
    section_builder = dict(SECTION_BUILDER.findall(view))
    page_tabs = {}
    for page, section in PAGE_SECTION.findall(read(MAIN_WINDOW_CS)):
        body = builders.get(section_builder.get(section, ""), "")
        page_tabs[page] = set(TAB_HEADER.findall(body))

    headings = set(TAB_HEADER.findall(view)) | set(CARD_TITLE.findall(view)) | set(FEATURE_CARD_TITLE.findall(read(FEATURE_SETTINGS_CS)))
    return {"tabs": tabs, "page_tabs": page_tabs, "headings": headings}


def words(text):
    """A title or a heading as words to compare: lower case, letters and digits, one space between."""
    return " ".join(re.findall(r"[a-z0-9]+", text.lower()))


def file_words(doc):
    """A document's name as its words: SmtpAuthOnDeliveryPorts.md is "smtp auth on delivery ports"."""
    return words(re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", doc[:-3]))


def check(maps, pages, reference, generator, cp):
    problems = []
    docs = set(name + ".md" for name in generator.page_names())
    headings_of = {}

    def headings(doc):
        if doc not in headings_of:
            headings_of[doc] = set(text for text, level, line in generator.headings(generator.read_page(doc[:-3])))
        return headings_of[doc]

    def check_target(surface, key, doc, heading):
        if doc is None:
            return
        if doc not in docs:
            problems.append("%s: %r names %s, which is not a document the generator embeds" % (surface, key, doc))
            return
        if heading is not None and heading not in headings(doc):
            problems.append("%s: %r names the heading %r, which is not in %s" % (surface, key, heading, doc))

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
            check_target(surface, key, doc, heading)

    # A tab's entry names a page of the Control Panel and a tab that page builds.
    for (page, tab), (doc, heading) in sorted(cp["tabs"].items()):
        if page not in pages["Control Panel"]:
            problems.append("Control Panel: the tab entry %r names no page" % (page + "#" + tab))
            continue
        if tab not in cp["page_tabs"].get(page, set()):
            problems.append("Control Panel: the tab entry %r names a tab the page does not build (its tabs: %s)" % (
                page + "#" + tab, ", ".join(sorted(cp["page_tabs"].get(page, set()))) or "none"))
        check_target("Control Panel", page + "#" + tab, doc, heading)

    named = set(doc for entries in maps.values() for doc, heading in entries.values() if doc)
    named |= set(doc for doc, heading in cp["tabs"].values() if doc)
    for doc in sorted(docs):
        if doc not in named and doc not in reference:
            problems.append("reach: %s is mapped by no surface and is not listed as reference-only in HelpMap.cs" % doc)
        if doc in named and doc in reference:
            problems.append("reach: %s is listed as reference-only in HelpMap.cs but a page maps it" % doc)
    for doc in reference:
        if doc not in docs:
            problems.append("reach: the reference-only list names %s, which is not a document" % doc)

    # A reference-only document that is about a tab or a card of a page - its title, or its name, is the
    # tab's or the card's heading - is that page's help, which the pane cannot reach from it.
    by_words = {}
    for heading in cp["headings"]:
        by_words.setdefault(words(heading), heading)
    for doc, via in sorted(REACHED_FROM.items()):
        if via not in named:
            problems.append("reach: %s is reached through %s, which no page maps" % (doc, via))
        elif via in docs and "](" + doc not in generator.read_page(via[:-3]):
            problems.append("reach: %s is reached through %s, which no longer links it" % (doc, via))
    for doc in reference:
        if doc not in docs or doc in NOT_THE_PAGES or doc in REACHED_FROM:
            continue
        found = generator.headings(generator.read_page(doc[:-3]))
        title = found[0][0] if found and found[0][1] == 1 else ""
        for candidate in (words(title), file_words(doc)):
            if candidate and candidate in by_words:
                problems.append("reach: %s is listed as reference-only, but %r is the heading of a tab or a card of a "
                                "Control Panel page: map that tab to it in HelpMap.Tabs" % (doc, by_words[candidate]))
                break
    return problems


def self_test(maps, pages, reference, generator, cp):
    """The negative control: a map with a heading that is not there, one with a document that is not, a tab
    entry naming a tab its page does not build, and a reference-only document whose name is a tab's
    heading - each refused by name."""
    failures = []
    surface = "Deck"
    key = sorted(maps[surface])[0]
    doc, heading = maps[surface][key]
    wrong = {s: dict(e) for s, e in maps.items()}
    wrong[surface][key] = (doc, "A heading that is not there")
    found = [p for p in check(wrong, pages, reference, generator, cp) if "A heading that is not there" in p]
    if not found:
        failures.append("an entry naming a heading that is not there was accepted")
    wrong = {s: dict(e) for s, e in maps.items()}
    wrong[surface][key] = ("NoSuchDocument.md", None)
    found = [p for p in check(wrong, pages, reference, generator, cp) if "NoSuchDocument.md" in p]
    if not found:
        failures.append("an entry naming a document that does not exist was accepted")

    wrong_cp = dict(cp, tabs=dict(cp["tabs"]))
    wrong_cp["tabs"][("logging", "A tab that is not there")] = ("Snmp.md", None)
    found = [p for p in check(maps, pages, reference, generator, wrong_cp) if "A tab that is not there" in p]
    if not found:
        failures.append("a tab entry naming a tab its page does not build was accepted")

    # Snmp.md listed as reference-only again, with the SNMP tab's entry gone: its name is the tab's heading.
    wrong_cp = dict(cp, tabs={k: v for k, v in cp["tabs"].items() if v[0] != "Snmp.md"})
    found = [p for p in check(maps, pages, reference + ["Snmp.md"], generator, wrong_cp)
             if "Snmp.md is listed as reference-only, but" in p]
    if not found:
        failures.append("a reference-only document named as a tab is named was accepted")
    return failures


def main():
    generator = load_generator()
    maps, pages, reference, cp = parse_maps()
    if "--self-test" in sys.argv:
        failures = self_test(maps, pages, reference, generator, cp)
        for failure in failures:
            print("  FAIL  " + failure)
        if failures:
            return 1
        print("check-help-map self-test: the negative controls are refused")
        return 0
    problems = check(maps, pages, reference, generator, cp)
    for problem in problems:
        print("  FAIL  " + problem)
    if problems:
        print("%d problem(s)" % len(problems))
        return 1
    print("help map: %d Control Panel pages (%d with no guide yet) and %d of their tabs, %d Deck views, %d webmail pages "
          "mapped; %d documents, %d reference-only" % (
              len(maps["Control Panel"]), sum(1 for d, h in maps["Control Panel"].values() if d is None), len(cp["tabs"]),
              len(maps["Deck"]), len(maps["webmail"]), len(generator.page_names()), len(reference)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
