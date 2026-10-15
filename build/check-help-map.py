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
            the wiki, at the branch the wiki serves (--wiki, below): its
            pages are exactly the table's, and each names in its text every
            document the table lists under it - so a page the wiki renamed
            or dropped, and a document a page no longer covers, fail here
            rather than in a pane as GitLab's "page does not exist", which
            every document but two opened while the link was made of the
            document's name.

What it reads the pages from, and so what it would not notice: a Control
Panel page is a Page("key" call in NavigationMap.cs (HelpMapTests holds the
same table from inside, against NavigationMap.Pages); a Deck view is a
data-view="..." attribute in index.html, so a section drawn by the script
rather than written in the markup would be missed; a webmail page is an
if (head === '...') test in Portal.js's route(), so a route written as a
case label or looked up in a table would be missed. Keep the three spelled
as they are, or teach this file the new spelling in the same change.

Usage:  python build/check-help-map.py [--self-test] [--wiki <clone of hmailserver.wiki> [--ref <ref>]]
  --self-test runs the same check over a copy of the maps with an entry
  naming a heading that is not there, one naming a document that is not, a
  tab its page does not build, a reference-only document named as a tab is,
  and the service account's document (whose title is not the card's word for
  word) without the link that reaches it, over copies of the wiki table and
  of a wiki with a document that is not one, a document under two pages, a
  page the wiki does not have, a page of the wiki the table does not list
  and a page that does not name a document listed under it, and over wikis
  it makes with git in a temporary folder - a page pushed to master alone
  while the wiki serves main, a remote whose HEAD names another branch than
  the one its clone recorded, a clone behind its remote and a clone with no
  remote to ask - and fails unless each is refused, and unless a plain
  clone of a wiki whose two branches agree is accepted: the negative
  controls.
  --wiki also holds the wiki table to the wiki's repository. By default it
  is held to the branch the wiki serves - the one its remote's HEAD names,
  which GitLab renders /-/wikis/<page> from - and, while that is another
  branch, to master as well, where the wiki's edits are pushed, so that a
  page added or renamed there fails here before it is served rather than
  opening "page does not exist" in every pane once it is (on 15 October 2026
  GitLab served main, eight commits behind master). The remote is the
  clone's gitlab, or origin in a plain clone, and it is asked (git ls-remote
  --symref), since the clone's own record of the remote's HEAD is written
  when the clone is made and no fetch moves it; each branch is read from the
  clone's remote-tracking branch, which must be where the remote has it, so
  fetch first. --ref <ref> holds the table to that one ref of the clone
  instead, read as the clone has it, with nothing asked of the remote.
"""
import importlib.util
import io
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

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


def wiki_problems(rows, wiki, at=None):
    """What is wrong with the wiki table's rows against the wiki itself, given as {page: text} for every .md
    file at the top of its repository: the table must list exactly the wiki's pages, and each page must name
    in its text - by file name, Backup.md - every document listed under it, which is how a page says it was
    made from one, whether in its opening note or in the section that links it. at, when given, is the ref
    of the wiki's repository the pages were read at, which each problem names."""
    problems = []
    there = "" if at is None else " at %s" % at
    listed = set(page for page, docs, number in rows)
    for page in sorted(set(wiki) - listed):
        problems.append("wiki: the wiki%s has a page %s, which build/help-wiki-pages.txt does not list" % (there, page))
    for page, docs, number in rows:
        where = "build/help-wiki-pages.txt:%d" % number
        if page not in wiki:
            problems.append("%s: the wiki%s has no page %s, so the documents listed under it would open GitLab's "
                            "\"page does not exist\"" % (where, there, page))
            continue
        for doc in docs:
            if doc not in wiki[page]:
                problems.append("%s: the wiki's page %s%s does not name %s, which the table says it was made from"
                                % (where, page, there, doc))
    return problems


# The branch the project pushes its edits of the wiki to. While the wiki serves another one - GitLab served main
# on 15 October 2026, and every edit since 27 September 2026 was on master alone - the table is held to both:
# to the branch served, whose pages the panes open, and to this one, whose pages they open once it is served.
EDIT_BRANCH = "master"


def git_text(clone, args, env=None, timeout=None):
    """git -C <clone> <args>: what it wrote, as text. Raises OSError (no git), CalledProcessError or
    TimeoutExpired."""
    return subprocess.run(["git", "-C", clone] + list(args), capture_output=True, check=True, env=env,
                          timeout=timeout).stdout.decode("utf-8", "replace")


def why(error):
    """What a git command that failed said, on one line, or the error itself."""
    said = (getattr(error, "stderr", None) or b"").decode("utf-8", "replace").strip()
    return " ".join(said.split()) or str(error)


def read_wiki(clone, ref, env=None):
    """{page: text} for every .md file at the top of the wiki's repository at ref, read with git; or a problem
    saying why it could not be read."""
    try:
        listed = git_text(clone, ["ls-tree", "-z", "--name-only", ref], env).split("\0")
        names = [name for name in listed if name.endswith(".md")]
        return dict((name[:-3], git_text(clone, ["show", "%s:%s" % (ref, name)], env)) for name in names), None
    except (OSError, subprocess.CalledProcessError) as error:
        return None, "wiki: %s at %s could not be read: %s" % (clone, ref, why(error))


def served_branches(remote, advertised):
    """The wiki's branches to hold the table to, from what git ls-remote --symref <remote> wrote: the branch its
    HEAD names, which is the one GitLab renders the pages from, and, while that is another, EDIT_BRANCH - each with
    the commit the remote has it at, [(branch, commit)]; or a problem when HEAD names no branch the remote has."""
    served, heads = None, {}
    for line in advertised.splitlines():
        if line.startswith("ref: ") and line.endswith("\tHEAD"):
            target = line[len("ref: "):-len("\tHEAD")]
            if target.startswith("refs/heads/"):
                served = target[len("refs/heads/"):]
            continue
        commit, _, name = line.partition("\t")
        if name.startswith("refs/heads/"):
            heads[name[len("refs/heads/"):]] = commit.strip()
    if served is None or served not in heads:
        return None, ("wiki: %s does not say which branch the wiki serves - its HEAD names no branch it has; name the "
                      "ref to hold the table to with --ref" % remote)
    branches = [served] + ([EDIT_BRANCH] if served != EDIT_BRANCH and EDIT_BRANCH in heads else [])
    return [(branch, heads[branch]) for branch in branches], None


def wiki_refs(clone, env=None):
    """The refs of the clone to hold the table to when --ref names none, [(ref, what it is)] - the remote-tracking
    branches of served_branches - and the problems that keep it from being held to them, [problem]: one of the two
    lists is empty. The remote is the clone's gitlab, as the project's clone names the group's wiki, or else
    origin, a plain clone's; it is asked which branch it serves, since refs/remotes/<remote>/HEAD, the clone's own
    record of that, is written when the clone is made and no fetch moves it (on 15 October 2026 the maintainer's
    clone recorded master for its origin, which served main); and each branch must be in the clone where the
    remote has it, so that what is read is what GitLab serves now and not what the clone last fetched."""
    try:
        remotes = git_text(clone, ["remote"], env).split()
    except (OSError, subprocess.CalledProcessError) as error:
        return [], ["wiki: %s cannot be read as a clone: %s" % (clone, why(error))]
    remote = next((name for name in ("gitlab", "origin") if name in remotes), None)
    if remote is None:
        return [], ["wiki: %s has no remote named gitlab or origin to ask which branch the wiki serves (it has %s); "
                    "name the ref to hold the table to with --ref" % (clone, ", ".join(remotes) or "none")]
    try:
        advertised = git_text(clone, ["ls-remote", "--symref", remote], env, timeout=120)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        return [], ["wiki: %s could not be asked which branch the wiki serves: %s; with no way to it, name the ref to "
                    "hold the table to with --ref" % (remote, why(error))]
    branches, unknown = served_branches(remote, advertised)
    if unknown:
        return [], [unknown]
    refs, problems = [], []
    for branch, commit in branches:
        ref = "%s/%s" % (remote, branch)
        try:
            have = git_text(clone, ["rev-parse", "--verify", "--quiet", "refs/remotes/%s^{commit}" % ref], env).strip()
        except (OSError, subprocess.CalledProcessError):
            have = ""
        if have != commit:
            problems.append("wiki: the clone's %s is %s, and the wiki's %s is at %s: fetch (git -C %s fetch %s) and "
                            "run this again" % (ref, "at " + have[:12] if have else "not there", branch, commit[:12],
                                                clone, remote))
        refs.append((ref, "the branch the wiki serves" if not refs else "the branch its edits are pushed to"))
    return ([], problems) if problems else (refs, [])


def wikis_problems(rows, wikis):
    """What is wrong with the table against each branch of the wiki read, [(ref, what it is, {page: text})], every
    problem naming its ref; and, when the branch served and the edits' branch hold different pages, that no table
    holds for both - the panes open the served branch's, and the other's once it is served."""
    problems = []
    for ref, what, wiki in wikis:
        problems += wiki_problems(rows, wiki, ref)
    if len(wikis) == 2:
        (served_ref, _, served), (other_ref, _, other) = wikis
        only = ((served_ref, sorted(set(served) - set(other))), (other_ref, sorted(set(other) - set(served))))
        differ = ["only %s has %s" % (ref, ", ".join(pages)) for ref, pages in only if pages]
        if differ:
            problems.append("wiki: %s, the branch the wiki serves, and %s, the branch its edits are pushed to, hold "
                            "different pages (%s): the panes open what %s holds, so no table holds for both until the "
                            "two branches hold the same pages" % (served_ref, other_ref, "; ".join(differ), served_ref))
    return problems


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


def remove_tree(path):
    """shutil.rmtree, through the read-only files git leaves on Windows."""
    def again(function, name, *ignored):
        os.chmod(name, stat.S_IWRITE)
        function(name)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=again)
    else:
        shutil.rmtree(path, onerror=again)


def wiki_branch_self_test():
    """The negative controls of the branch --wiki holds the table to, over wikis made with git in a temporary
    folder, each a bare repository standing for GitLab's with a clone of it: a page pushed to master alone while
    the wiki serves main, and listed in the table, is refused by the branch served - the default of up to October
    2026, gitlab/master, accepted it, though every pane would have opened "page does not exist" for it; a remote
    whose HEAD has moved to master since its clone was made is held at master, not at the main the clone
    recorded; a clone behind its remote, and a clone with no remote gitlab or origin, are refused; and a plain
    clone, whose remote is origin, of a wiki whose two branches hold the same pages is accepted - which that
    default refused as "Not a valid object name gitlab/master" - so a check that refused every clone fails too."""
    failures = []
    work = tempfile.mkdtemp(prefix="check-help-map-")
    try:
        # git as nobody in particular: no system or user configuration (no signing, no hooks, no rewritten
        # addresses), a fixed identity and date.
        config = os.path.join(work, "gitconfig")
        io.open(config, "w").close()
        env = dict(os.environ, GIT_CONFIG_GLOBAL=config, GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0",
                   GIT_AUTHOR_NAME="check-help-map", GIT_AUTHOR_EMAIL="check-help-map@example.invalid",
                   GIT_AUTHOR_DATE="2026-10-15T12:00:00Z", GIT_COMMITTER_NAME="check-help-map",
                   GIT_COMMITTER_EMAIL="check-help-map@example.invalid", GIT_COMMITTER_DATE="2026-10-15T12:00:00Z")

        def git(*args, **given):
            return subprocess.run(["git"] + list(args), capture_output=True, check=True, env=env,
                                  **given).stdout.decode("utf-8", "replace").strip()

        pages = {"Home": "The wiki's home page.",
                 "Backup-and-Restore": "Brought up against hmailserver/docs/Backup.md."}
        rows = [("Backup-and-Restore", ["Backup.md"], 1), ("Home", [], 2)]
        added = {"Setup-Checklist": "Written from hmailserver/docs/SetupChecklist.md."}
        rows_added = rows + [("Setup-Checklist", ["SetupChecklist.md"], 3)]

        def commit(bare, branch, tree_pages, parent=None):
            entries = "".join("100644 blob %s\t%s.md\n" % (git("-C", bare, "hash-object", "-w", "--stdin",
                                                               input=text.encode("utf-8")), name)
                              for name, text in sorted(tree_pages.items()))
            tree = git("-C", bare, "mktree", input=entries.encode("utf-8"))
            made = git("-C", bare, "commit-tree", tree, "-m", "The wiki's pages", *(["-p", parent] if parent else []))
            git("-C", bare, "update-ref", "refs/heads/" + branch, made)
            return made

        def remote(name, master_adds=None):
            """GitLab's repository of a wiki: main, and master with the pages added, HEAD naming main."""
            bare = os.path.join(work, name + ".git")
            git("init", "-q", "--bare", bare)
            main = commit(bare, "main", pages)
            commit(bare, "master", dict(pages, **(master_adds or {})), main)
            git("-C", bare, "symbolic-ref", "HEAD", "refs/heads/main")
            return bare

        def clone(bare, name, origin="origin"):
            path = os.path.join(work, name)
            git("clone", "-q", "--no-checkout", "-o", origin, bare, path)
            return path

        def held(path, table):
            """The refs the default holds the table to, and what it finds wrong, as main() reads them."""
            refs, problems = wiki_refs(path, env)
            wikis = []
            for ref, what in refs:
                wiki, unreadable = read_wiki(path, ref, env)
                if unreadable:
                    problems.append(unreadable)
                else:
                    wikis.append((ref, what, wiki))
            return [ref for ref, what in refs], problems + wikis_problems(table, wikis)

        # A page pushed to master alone, as the wiki's edits are, and listed in the table; GitLab serves main.
        ahead = clone(remote("ahead", added), "ahead", "gitlab")
        if wiki_problems(rows_added, read_wiki(ahead, "gitlab/master", env)[0] or {}):
            failures.append("the wiki made for the page on master alone is not one gitlab/master alone accepts")
        refs, problems = held(ahead, rows_added)
        if refs != ["gitlab/main", "gitlab/master"]:
            failures.append("a clone whose remote gitlab serves main was held to %s, not to gitlab/main and "
                            "gitlab/master: %s" % (refs, "; ".join(problems)))
        if not any("the wiki at gitlab/main has no page Setup-Checklist," in p for p in problems) or \
                not any("hold different pages (only gitlab/master has Setup-Checklist)" in p for p in problems):
            failures.append("a page pushed to master alone, which the wiki does not serve, was accepted: %s"
                            % ("; ".join(problems) or "no problem found"))

        # A plain clone of a wiki whose two branches hold the same pages.
        same = remote("same")
        refs, problems = held(clone(same, "plain"), rows)
        if refs != ["origin/main", "origin/master"] or problems:
            failures.append("a plain clone of a wiki whose branches agree was held to %s and refused: %s"
                            % (refs, "; ".join(problems) or "no problem found"))

        # The remote's HEAD moved to master after the clone recorded main.
        moved = remote("moved")
        recorded = clone(moved, "moved")
        git("-C", moved, "symbolic-ref", "HEAD", "refs/heads/master")
        if git("-C", recorded, "symbolic-ref", "refs/remotes/origin/HEAD") != "refs/remotes/origin/main":
            failures.append("the clone made before the remote's HEAD moved does not record main")
        refs, problems = held(recorded, rows)
        if refs != ["origin/master"] or problems:
            failures.append("a clone whose remote now serves master was held to %s, as the clone recorded it, not "
                            "to origin/master: %s" % (refs, "; ".join(problems) or "no problem found"))

        # A clone behind its remote: main moved on after it fetched.
        behind_remote = remote("behind")
        behind = clone(behind_remote, "behind", "gitlab")
        commit(behind_remote, "main", dict(pages, **{"Late-Page": "Pushed after the clone fetched."}),
               git("-C", behind_remote, "rev-parse", "refs/heads/main"))
        refs, problems = held(behind, rows + [("Late-Page", [], 3)])
        if refs or not any("the clone's gitlab/main is at" in p and "fetch" in p for p in problems):
            failures.append("a clone behind its remote was read as the wiki: held to %s, %s"
                            % (refs, "; ".join(problems) or "no problem found"))

        # A clone whose only remote has another name: nothing to ask.
        renamed = clone(same, "renamed")
        git("-C", renamed, "remote", "rename", "origin", "upstream")
        refs, problems = held(renamed, rows)
        if refs or not any("has no remote named gitlab or origin" in p for p in problems):
            failures.append("a clone with no remote gitlab or origin was held to %s: %s"
                            % (refs, "; ".join(problems) or "no problem found"))
    except (OSError, subprocess.CalledProcessError) as error:
        failures.append("the wikis of the default branch's negative controls could not be made with git: %s"
                        % why(error))
    finally:
        remove_tree(work)
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
        failures = (self_test(maps, pages, reference, generator, cp) + wiki_self_test(generator)
                    + wiki_branch_self_test())
        for failure in failures:
            print("  FAIL  " + failure)
        if failures:
            return 1
        print("check-help-map self-test: the negative controls are refused")
        return 0
    rows = generator.read_wiki_table()
    problems = check(maps, pages, reference, generator, cp) + generator.wiki_table_problems(rows, generator.page_names())
    clone = option("--wiki")
    ref = option("--ref")
    held = ""
    if clone is not None:
        # The ref named, read as the clone has it; or the branch the wiki serves, and while that is another the
        # branch its edits are pushed to, each where the remote has it.
        refs, unusable = ([(ref, "the ref named")], []) if ref is not None else wiki_refs(clone)
        problems += unusable
        wikis = []
        for at, what in refs:
            wiki, unreadable = read_wiki(clone, at)
            if unreadable:
                problems.append(unreadable)
            else:
                wikis.append((at, what, wiki))
        problems += wikis_problems(rows, wikis)
        held = " (held to %s at %s)" % (clone, " and ".join("%s (%s)" % (at, what) for at, what in refs))
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
              held))
    return 0


if __name__ == "__main__":
    sys.exit(main())
