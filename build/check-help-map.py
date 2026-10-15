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
            not reference-only; and a reference-only document whose title or
            name meets a Control Panel tab's or card's heading - the
            significant words of one among the other's - is that page's
            help: mapped to the tab, linked from the page's document
            (REACHED_FROM), or another subject by a stated reason
            (NOT_THE_PAGES).
  wiki      build/help-wiki-pages.txt, the wiki's pages and the documents
            each was made from, which the panes' "Open on the wiki" opens:
            every page named once and in a form a wiki page's name takes,
            every document one the help carries and under one page only -
            read by the generator's own reader, which refuses the same. With
            --wiki and a clone of the wiki's repository, it is also held to
            the wiki: its pages are exactly the table's, and each names in
            its text every document the table lists under it - so a page the
            wiki renamed or dropped, and a document a page no longer covers,
            fail here rather than in a pane as GitLab's "page does not
            exist", which every document but two opened while the link was
            made of the document's name.

What it reads the pages from, and so what it would not notice: a Control
Panel page is a Page("key" call in NavigationMap.cs (HelpMapTests holds the
same table from inside, against NavigationMap.Pages); a Deck view is a
data-view="..." attribute in index.html, so a section drawn by the script
rather than written in the markup would be missed; a webmail page is an
if (head === '...') test in Portal.js's route(), so a route written as a
case label or looked up in a table would be missed. Keep the three spelled
as they are, or teach this file the new spelling in the same change.

Usage:  python build/check-help-map.py [--self-test] [--wiki <clone of hmailserver.wiki> [--ref <branch>]]
  --self-test runs the same check over a copy of the maps with an entry
  naming a heading that is not there, one naming a document that is not, a
  tab its page does not build, a reference-only document named as a tab is,
  and the service account's document (whose title is not the card's word for
  word) without the link that reaches it, and over copies of the wiki table
  and of a wiki with a document that is not one, a document under two pages,
  a page the wiki does not have, a page of the wiki the table does not list
  and a page that does not name a document listed under it, and fails unless
  each is refused: the negative controls.
  --wiki also holds the wiki table to the wiki's repository at --ref, by
  default gitlab/master (the remote-tracking branch of a clone whose remote
  is named gitlab; master in a plain clone).
"""
import importlib.util
import io
import os
import re
import subprocess
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

# A reference-only document whose title or name meets a heading of a page, but whose
# subject is another: the reason it stays reference-only, said where the rule is.
NOT_THE_PAGES = {
    "Mirroring.md": "the project's push mirror and weekly export of its repository, not the server's mail",
    "DatabaseTls.md": "the encryption of the server's own connection to its database, which is set in hMailServer.ini's "
                      "[Database] section and which no page edits (the document's 'No editor in the Control Panel or the "
                      "Deck'); the Performance page's Database tab and its Database connections card tune the pool and "
                      "the retries",
    "MigratingDatabaseBackend.md": "moving the server to another database engine by backup, repointing and restore, "
                                   "which Backup.md and Upgrading.md link; the Performance page's Database tab tunes the "
                                   "connections of the engine in use",
    "RestrictedDelivery.md": "the domains a domain's accounts may exchange mail with, on the Restricted delivery tab of "
                             "the domain's own dialog, which the Domains page's DomainsAndAccounts.md links; the "
                             "Delivery of e-mail page's Delivery tab is the retries and the bounces",
}

# A reference-only document that is about one card of a page whose own document - the
# one the pane opens for the page, or for the card's tab - links it, so that the pane
# reaches it from the page in one step: the document that links it, which the checker
# holds to being mapped and to carrying the link.
REACHED_FROM = {
    # The fourth card of the webmail page's Sign-in tab, whose document is the
    # passkeys and single sign-on one.
    "AccountRecovery.md": "PasskeysAndSingleSignOn.md",
    # The Reports your domains receive card of the Delivery of e-mail page's
    # Feedback tab, which opens the complaints document.
    "DmarcReports.md": "Complaints.md",
    # The Windows service account card of Server limits & expert settings, which
    # opens the stalled-mail document on the settings that bound each stage.
    "ServiceAccount.md": "DiagnosingStalledMail.md",
    # The Object storage and Mailboxes encrypted at rest cards of the Performance
    # page's Message store tab, which opens the message store's document.
    "ObjectStorage.md": "MessageStore.md",
    "EncryptionAtRest.md": "MessageStore.md",
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


# The words a title and a heading may differ by and still be the same subject: the
# articles and the possessives ("The service account", "Windows service account";
# "The reports our own domains receive, read", "Reports your domains receive").
NOT_SIGNIFICANT = frozenset(["a", "an", "the", "our", "your", "its", "their", "own", "s"])


def significant(text):
    """The words of a title or a heading that say what it is about, as a set."""
    return frozenset(w for w in words(text).split() if w not in NOT_SIGNIFICANT)


def meets(one, other):
    """Whether a title and a heading are about the same thing: the significant words of one are among the
    other's. Exact matching let through the documents it was written for - the service account's title is "The
    service account", its card "Windows service account"."""
    a, b = significant(one), significant(other)
    return bool(a) and bool(b) and (a <= b or b <= a)


def check(maps, pages, reference, generator, cp, reached_from=None, not_the_pages=None):
    reached_from = REACHED_FROM if reached_from is None else reached_from
    not_the_pages = NOT_THE_PAGES if not_the_pages is None else not_the_pages
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

    # A reference-only document that is about a tab or a card of a page - its title, or its name, meets the
    # tab's or the card's heading - is that page's help, which the pane cannot reach from it.
    for doc, via in sorted(reached_from.items()):
        if via not in named:
            problems.append("reach: %s is reached through %s, which no page maps" % (doc, via))
        elif via in docs and "](" + doc not in generator.read_page(via[:-3]):
            problems.append("reach: %s is reached through %s, which no longer links it" % (doc, via))
    headings = sorted(cp["headings"])
    for doc in reference:
        if doc not in docs or doc in not_the_pages or doc in reached_from:
            continue
        found = generator.headings(generator.read_page(doc[:-3]))
        title = found[0][0] if found and found[0][1] == 1 else ""
        met = next((h for candidate in (title, file_words(doc)) for h in headings if meets(candidate, h)), None)
        if met is not None:
            problems.append("reach: %s is listed as reference-only, but its title or name meets %r, the heading of a "
                            "tab or a card of a Control Panel page: map that tab to it in HelpMap.Tabs, link it from "
                            "the page's document and list it in REACHED_FROM, or say in NOT_THE_PAGES why it is about "
                            "something else" % (doc, met))
    return problems


def wiki_problems(rows, wiki):
    """What is wrong with the wiki table's rows against the wiki itself, given as {page: text} for every .md
    file at the top of its repository: the table must list exactly the wiki's pages, and each page must name
    in its text - by file name, Backup.md - every document listed under it, which is how a page says it was
    made from one, whether in its opening note or in the section that links it."""
    problems = []
    listed = set(page for page, docs, number in rows)
    for page in sorted(set(wiki) - listed):
        problems.append("wiki: the wiki has a page %s, which build/help-wiki-pages.txt does not list" % page)
    for page, docs, number in rows:
        where = "build/help-wiki-pages.txt:%d" % number
        if page not in wiki:
            problems.append("%s: the wiki has no page %s, so the documents listed under it would open GitLab's "
                            "\"page does not exist\"" % (where, page))
            continue
        for doc in docs:
            if doc not in wiki[page]:
                problems.append("%s: the wiki's page %s does not name %s, which the table says it was made from"
                                % (where, page, doc))
    return problems


def read_wiki(clone, ref):
    """{page: text} for every .md file at the top of the wiki's repository at ref, read with git; or a problem
    saying why it could not be read."""
    def git(*args):
        return subprocess.run(["git", "-C", clone] + list(args), capture_output=True, check=True).stdout
    try:
        names = [n for n in git("ls-tree", "-z", "--name-only", ref).decode("utf-8").split("\0") if n.endswith(".md")]
        return dict((name[:-3], git("show", "%s:%s" % (ref, name)).decode("utf-8", "replace")) for name in names), None
    except (OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", b"") or b""
        return None, "wiki: %s at %s could not be read: %s" % (clone, ref, detail.decode("utf-8", "replace").strip() or error)


def wiki_self_test(generator):
    """The wiki table's negative controls: copies of the table and of a wiki made from it, each with one thing
    wrong, every one refused by name - and the wiki made from the table accepted, so a check that refused
    everything would fail too."""
    failures = []
    names = generator.page_names()
    rows = generator.read_wiki_table()
    made = [(page, list(docs), number) for page, docs, number in rows]
    mapped = [(page, docs) for page, docs, number in made if docs]
    unmapped = [page for page, docs, number in made if not docs]
    wiki = dict((page, " ".join(docs)) for page, docs, number in made)

    if generator.wiki_table_problems(made, names) or wiki_problems(made, wiki):
        failures.append("the table as it is, against a wiki that holds it, was refused")

    wrong = made + [(unmapped[0], ["NoSuchDocument.md"], 9001)]
    if not any("NoSuchDocument.md, which is not a document the help carries" in p for p in generator.wiki_table_problems(wrong, names)):
        failures.append("a table naming a document the help does not carry was accepted")

    twice = mapped[0][1][0]
    wrong = [(page, docs + ([twice] if page == mapped[1][0] else []), number) for page, docs, number in made]
    if not any(twice + " is listed under" in p for p in generator.wiki_table_problems(wrong, names)):
        failures.append("a table listing a document under two pages was accepted")

    gone = dict(wiki)
    del gone[mapped[0][0]]
    if not any("the wiki has no page %s," % mapped[0][0] in p for p in wiki_problems(made, gone)):
        failures.append("a page the wiki does not have was accepted")

    more = dict(wiki, **{"A-Page-The-Table-Does-Not-List": ""})
    if not any("A-Page-The-Table-Does-Not-List, which build/help-wiki-pages.txt does not list" in p for p in wiki_problems(made, more)):
        failures.append("a page of the wiki the table does not list was accepted")

    silent = dict(wiki, **{mapped[0][0]: ""})
    if not any("does not name %s" % mapped[0][1][0] in p for p in wiki_problems(made, silent)):
        failures.append("a page that does not name a document listed under it was accepted")

    # The link as it was made up to October 2026: the base URL and the document's own name, as if the wiki had
    # a page of that name for every document. Against the wiki the table describes, nearly every one is refused.
    as_it_was = [(name, [name + ".md"], index + 1) for index, name in enumerate(names)]
    refused = [p for p in wiki_problems(as_it_was, wiki) if "the wiki has no page" in p]
    if len(refused) < len(names) - 2:
        failures.append("the old link, the document's own name, was accepted for %d of %d documents"
                        % (len(names) - len(refused), len(names)))
    return failures


def self_test(maps, pages, reference, generator, cp):
    """The negative control: a map with a heading that is not there, one with a document that is not, a tab
    entry naming a tab its page does not build, a reference-only document whose name is a tab's heading, the
    service account's document - whose title meets its card's heading but is not it word for word - with
    nothing that reaches it, and the same said to be reached through a document that does not link it - each
    refused by name."""
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

    # ServiceAccount.md without its way in: "The service account" is not the card's "Windows service account"
    # word for word, which the exact rule let through.
    without = {k: v for k, v in REACHED_FROM.items() if k != "ServiceAccount.md"}
    found = [p for p in check(maps, pages, reference, generator, cp, reached_from=without)
             if "ServiceAccount.md is listed as reference-only, but" in p and "Windows service account" in p]
    if not found:
        failures.append("the service account's document, whose card is 'Windows service account', was accepted with "
                        "nothing that reaches it")

    # ... and reached through a mapped document that does not link it.
    elsewhere = dict(REACHED_FROM, **{"ServiceAccount.md": "Snmp.md"})
    found = [p for p in check(maps, pages, reference, generator, cp, reached_from=elsewhere)
             if "ServiceAccount.md is reached through Snmp.md, which no longer links it" in p]
    if not found:
        failures.append("a document said to be reached through one that does not link it was accepted")
    return failures


def option(name):
    """The value given after an option, or None when it is not given; a missing value is an error."""
    if name not in sys.argv:
        return None
    at = sys.argv.index(name)
    if at + 1 >= len(sys.argv) or sys.argv[at + 1].startswith("--"):
        sys.exit("%s needs a value" % name)
    return sys.argv[at + 1]


def main():
    generator = load_generator()
    maps, pages, reference, cp = parse_maps()
    if "--self-test" in sys.argv:
        failures = self_test(maps, pages, reference, generator, cp) + wiki_self_test(generator)
        for failure in failures:
            print("  FAIL  " + failure)
        if failures:
            return 1
        print("check-help-map self-test: the negative controls are refused")
        return 0
    rows = generator.read_wiki_table()
    problems = check(maps, pages, reference, generator, cp) + generator.wiki_table_problems(rows, generator.page_names())
    clone = option("--wiki")
    ref = option("--ref") or "gitlab/master"
    if clone is not None:
        wiki, unreadable = read_wiki(clone, ref)
        problems += [unreadable] if unreadable else wiki_problems(rows, wiki)
    for problem in problems:
        print("  FAIL  " + problem)
    if problems:
        print("%d problem(s)" % len(problems))
        return 1
    with_page = sum(len(docs) for page, docs, number in rows)
    print("help map: %d Control Panel pages (%d with no guide yet) and %d of their tabs, %d Deck views, %d webmail pages "
          "mapped; %d documents, %d reference-only; %d of the documents have a wiki page, among the wiki's %d pages%s" % (
              len(maps["Control Panel"]), sum(1 for d, h in maps["Control Panel"].values() if d is None), len(cp["tabs"]),
              len(maps["Deck"]), len(maps["webmail"]), len(generator.page_names()), len(reference), with_page, len(rows),
              " (held to %s at %s)" % (clone, ref) if clone is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
