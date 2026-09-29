#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The update feed's Atom feed of releases, releases.atom, held to RFC 4287 and to the feed.

build/generate-update-feed.py writes releases.atom beside releases.json on the
update feed's site (https://updates.progressiverobot.com/releases.atom), for a
feed reader: an entry for every release the feed lists, newest first. This
checks such a file:

  - it is well-formed XML in UTF-8, with no document type declaration (so no
    entity can be declared, let alone expanded), and its root is atom:feed in
    the Atom namespace, http://www.w3.org/2005/Atom;
  - the feed and each entry hold what RFC 4287 sections 4.1.1 and 4.1.2 require
    of them: exactly one atom:id, atom:title and atom:updated each; an author,
    in the feed or in every entry; at most one of each element the RFC allows
    once; an entry without atom:content has an alternate link, and one whose
    content has a src has a summary; no two alternate links with the same type
    and hreflang; no element of the Atom namespace the RFC does not define
    there; every link with an href, every category with a term, every person
    with one name; a text construct's type text, html or xhtml, and a text one
    without child elements;
  - every date is an RFC 3339 date-time with an upper-case T and Z (RFC 4287
    section 3.3), a real day and time;
  - every id is an absolute IRI, no two alike; the feed's is the one the
    generator mints, tag:updates.progressiverobot.com,2026:releases, and each
    entry's the same with "/<tag>" after it, the release's tag: made from the
    tag alone, an id does not change when the feed is made again, when a release
    is edited, or when the project moves and every release page's address with
    it;
  - the entries are newest first, and the feed's updated time is no older than
    its newest entry's; its self link is the site's /releases.atom;
  - held to the feed's releases.json (a directory or the site): an entry for
    each release it lists, in its order, each with that release's id, name,
    page and time, and a category saying whether it is a pre-release, so the
    newest entry is the newest release the feed lists; and the release
    releases/latest names, the newest stable version, has its entry;
  - held to an earlier copy (--previous): every id in it is still here, so no
    id was changed and no release removed.

Usage, from the repository root:
  python3 build/check-atom-feed.py
      the feed generate-update-feed.py renders from releases held here, with no
      network: what the checkers job runs
  python3 build/check-atom-feed.py public [--previous FILE|URL]
  python3 build/check-atom-feed.py public/releases.atom [--previous FILE|URL]
      a written feed, held to the releases.json and releases/latest beside it
  python3 build/check-atom-feed.py --site https://updates.progressiverobot.com [--previous FILE|URL]
      the deployed feed, held to the site's releases.json and releases/latest
  python3 build/check-atom-feed.py --self-test
      a good feed passes and each of the ways to break it is refused

generate-update-feed.py --self-test and --serve-check run the same checks, so the
pages job runs them on the canned feed and on the one it publishes.
"""
import argparse
import datetime
import importlib.util
import json
import os
import re
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ATOM_NS = "http://www.w3.org/2005/Atom"
SITE = "https://updates.progressiverobot.com"
SELF_URL = SITE + "/releases.atom"
FEED_ID = "tag:updates.progressiverobot.com,2026:releases"
ENTRY_ID_PREFIX = FEED_ID + "/"
# A server release's tag, as the feed takes it (generate-update-feed.py's SERVER_TAG).
SERVER_TAG = re.compile(r"^v[0-9][A-Za-z0-9._-]{0,62}$")
# An absolute IRI: a scheme, a colon, and no white space (RFC 3987, RFC 4287 4.2.6).
ABSOLUTE_IRI = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:[^\s<>\"{}|\\^`]+$")
# RFC 3339's date-time, with RFC 4287 3.3's upper-case T and Z.
DATE_TIME = re.compile(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(\.\d+)?(Z|[+-](\d{2}):(\d{2}))$")

# The children RFC 4287 defines, each with how many times it may appear: 1 exactly
# once, "?" at most once, "*" any number.
FEED_CHILDREN = {"author": "*", "category": "*", "contributor": "*", "generator": "?", "icon": "?", "id": 1, "link": "*",
                 "logo": "?", "rights": "?", "subtitle": "?", "title": 1, "updated": 1, "entry": "*"}
ENTRY_CHILDREN = {"author": "*", "category": "*", "content": "?", "contributor": "*", "id": 1, "link": "*",
                  "published": "?", "rights": "?", "source": "?", "summary": "?", "title": 1, "updated": 1}
TEXT_CONSTRUCTS = ("title", "subtitle", "rights", "summary")
XHTML_NS = "http://www.w3.org/1999/xhtml"


def atom(name):
    return "{%s}%s" % (ATOM_NS, name)


# What XML 1.0 cannot carry (generate-update-feed.py leaves it out of the feed), and
# the line ends a parser reads as a line feed (XML 1.0, 2.11).
XML_UNUSABLE = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ud800-\udfff\ufffe\uffff]")


def as_xml_reads(text):
    """text as a parser reads it back out of the feed."""
    return XML_UNUSABLE.sub("", text).replace("\r\n", "\n").replace("\r", "\n")


def local(tag):
    """(namespace, name) of an element's tag."""
    if tag.startswith("{"):
        namespace, _, name = tag[1:].partition("}")
        return namespace, name
    return "", tag


def parse_date(text):
    """The UTC datetime an RFC 3339 date-time names, or None when it is not one."""
    match = DATE_TIME.match(text or "")
    if not match:
        return None
    year, month, day, hour, minute, second = (int(match.group(i)) for i in range(1, 7))
    if second == 60:
        # A leap second is a date-time RFC 3339 allows; Python's datetime does not.
        second = 59
    try:
        offset = datetime.timedelta(0)
        if match.group(8) != "Z":
            hours, minutes = int(match.group(9)), int(match.group(10))
            if hours > 23 or minutes > 59:
                return None
            offset = datetime.timedelta(hours=hours, minutes=minutes) * (1 if match.group(8)[0] == "+" else -1)
        moment = datetime.datetime(year, month, day, hour, minute, second, tzinfo=datetime.timezone(offset))
    except ValueError:
        return None
    return moment.astimezone(datetime.timezone.utc)


def parse(content):
    """(root element, problem): the XML parsed, or why it could not be."""
    if not isinstance(content, bytes):
        return None, "the feed is not bytes"
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError as e:
        return None, "the feed is not UTF-8: %s" % e
    declaration = re.match(r"^\ufeff?<\?xml[^?]*\?>", text)
    if declaration:
        encoding = re.search(r"encoding\s*=\s*[\"']([^\"']+)[\"']", declaration.group(0))
        if encoding and encoding.group(1).lower().replace("_", "-") not in ("utf-8", "utf8"):
            return None, "the XML declaration says encoding %s; the feed is written in UTF-8" % encoding.group(1)
    # No document type declaration: an Atom feed has none, and without one no
    # entity can be declared, so none can be expanded, however large.
    if re.search(r"<!DOCTYPE|<!ENTITY", text, re.IGNORECASE):
        return None, "the feed has a document type declaration; an Atom feed needs none, and it could declare entities"
    try:
        return ET.fromstring(content), None
    except ET.ParseError as e:
        return None, "the feed is not well-formed XML: %s" % e


def entry_ids(content):
    """The entries' ids in their order, or [] when the feed cannot be read."""
    root, _ = parse(content)
    if root is None:
        return []
    return [(entry.findtext(atom("id")) or "").strip() for entry in root.findall(atom("entry"))]


def check_children(element, allowed, where, problems):
    counts = {}
    for child in element:
        namespace, name = local(child.tag)
        if namespace != ATOM_NS:
            continue  # foreign markup (RFC 4287 6.4) is allowed anywhere
        if name not in allowed:
            problems.append("%s holds atom:%s, which RFC 4287 does not define there" % (where, name))
            continue
        counts[name] = counts.get(name, 0) + 1
    for name, rule in sorted(allowed.items()):
        count = counts.get(name, 0)
        if rule == 1 and count != 1:
            problems.append("%s has %d atom:%s; RFC 4287 requires exactly one" % (where, count, name))
        elif rule == "?" and count > 1:
            problems.append("%s has %d atom:%s; RFC 4287 allows one at most" % (where, count, name))


def check_text_construct(element, where, problems):
    kind = element.get("type", "text")
    if kind not in ("text", "html", "xhtml"):
        problems.append("%s has type %r; a text construct's is text, html or xhtml" % (where, kind))
    elif kind in ("text", "html") and len(element):
        problems.append("%s is %s but holds child elements; markup must be escaped" % (where, kind))
    elif kind == "xhtml" and not (len(element) == 1 and element[0].tag == "{%s}div" % XHTML_NS):
        problems.append("%s is xhtml but is not one xhtml:div" % where)


def check_person(element, where, problems):
    names = element.findall(atom("name"))
    if len(names) != 1 or not (names[0].text or "").strip():
        problems.append("%s has %d atom:name; a person construct has exactly one, not empty" % (where, len(names)))
    for name in ("uri", "email"):
        if len(element.findall(atom(name))) > 1:
            problems.append("%s has more than one atom:%s" % (where, name))


def check_links(element, where, problems):
    """Returns {rel: [link elements]}."""
    by_rel = {}
    alternates = set()
    for link in element.findall(atom("link")):
        href = link.get("href")
        rel = link.get("rel", "alternate")
        if not href:
            problems.append("%s has an atom:link with no href" % where)
            continue
        by_rel.setdefault(rel, []).append(link)
        if rel == "alternate":
            key = (link.get("type"), link.get("hreflang"))
            if key in alternates:
                problems.append("%s has two alternate links of type %s and hreflang %s" % (where, key[0], key[1]))
            alternates.add(key)
    return by_rel


def check_common(element, where, problems):
    """What the feed and an entry share: the id, the dates, the text constructs,
    the people and the categories. Returns (id, updated as UTC or None)."""
    for name in TEXT_CONSTRUCTS:
        for child in element.findall(atom(name)):
            check_text_construct(child, "%s's atom:%s" % (where, name), problems)
    for name in ("author", "contributor"):
        for child in element.findall(atom(name)):
            check_person(child, "%s's atom:%s" % (where, name), problems)
    for category in element.findall(atom("category")):
        if not category.get("term"):
            problems.append("%s has an atom:category with no term" % where)
    identity = (element.findtext(atom("id")) or "").strip()
    if identity and not ABSOLUTE_IRI.match(identity):
        problems.append("%s's id %r is not an absolute IRI" % (where, identity))
    moments = {}
    for name in ("updated", "published"):
        for child in element.findall(atom(name)):
            text = (child.text or "").strip()
            moment = parse_date(text)
            if moment is None:
                problems.append("%s's atom:%s %r is not an RFC 3339 date-time with an upper-case T and Z (RFC 4287 3.3)"
                                % (where, name, text))
            moments[name] = moment
    title = element.find(atom("title"))
    if title is not None and not "".join(title.itertext()).strip():
        problems.append("%s's atom:title is empty" % where)
    return identity, moments.get("updated")


def check_atom(content, listing=None, latest=None, previous=None):
    """The problems of releases.atom (bytes), none when it is sound. listing is the
    feed's releases.json, latest its releases/latest, previous an earlier copy of
    releases.atom (bytes); each is checked against only when given."""
    root, problem = parse(content)
    if root is None:
        return [problem]
    if root.tag != atom("feed"):
        namespace, name = local(root.tag)
        return ["the root element is %s in the namespace %r, not feed in %s" % (name, namespace, ATOM_NS)]

    problems = []
    check_children(root, FEED_CHILDREN, "the feed", problems)
    feed_id, feed_updated = check_common(root, "the feed", problems)
    if feed_id and feed_id != FEED_ID:
        problems.append("the feed's id is %r, not %s: a reader would take it for another feed" % (feed_id, FEED_ID))
    links = check_links(root, "the feed", problems)
    selves = links.get("self", [])
    if len(selves) != 1 or selves[0].get("href") != SELF_URL:
        problems.append("the feed's self link is %s, not %s alone" % (", ".join(l.get("href") for l in selves) or "missing", SELF_URL))
    feed_has_author = bool(root.findall(atom("author")))

    entries = root.findall(atom("entry"))
    ids = []
    updates = []
    for number, entry in enumerate(entries, 1):
        identity = (entry.findtext(atom("id")) or "").strip()
        where = "entry %d (%s)" % (number, identity or "no id")
        check_children(entry, ENTRY_CHILDREN, where, problems)
        identity, updated = check_common(entry, where, problems)
        entry_links = check_links(entry, where, problems)
        content_element = entry.find(atom("content"))
        if content_element is None and not entry_links.get("alternate"):
            problems.append("%s has no atom:content and no alternate link (RFC 4287 4.1.2)" % where)
        if content_element is not None and content_element.get("src") is not None:
            if entry.find(atom("summary")) is None:
                problems.append("%s's content has a src and the entry has no atom:summary (RFC 4287 4.1.2)" % where)
            if (content_element.text or "").strip() or len(content_element):
                problems.append("%s's content has a src and is not empty (RFC 4287 4.1.3.2)" % where)
        if not feed_has_author and not entry.findall(atom("author")):
            source = entry.find(atom("source"))
            if source is None or not source.findall(atom("author")):
                problems.append("%s has no author, and nor has the feed (RFC 4287 4.1.1)" % where)
        if identity:
            tag = identity[len(ENTRY_ID_PREFIX):] if identity.startswith(ENTRY_ID_PREFIX) else ""
            if not SERVER_TAG.match(tag):
                problems.append("%s's id is not %s<tag>, made from the release's tag alone" % (where, ENTRY_ID_PREFIX))
        ids.append(identity)
        updates.append(updated)

    seen = set()
    for identity in ids:
        if identity and identity in seen:
            problems.append("two entries have the id %s" % identity)
        if identity and identity == feed_id:
            problems.append("an entry has the feed's own id")
        seen.add(identity)
    for number in range(1, len(updates)):
        if updates[number - 1] is not None and updates[number] is not None and updates[number] > updates[number - 1]:
            problems.append("entry %d (%s) is newer than entry %d before it: the entries are not newest first"
                            % (number + 1, ids[number], number))
    known = [u for u in updates if u is not None]
    if feed_updated is not None and known and max(known) > feed_updated:
        problems.append("the feed's updated time is older than its newest entry's")

    if listing is not None:
        problems.extend(check_against_listing(entries, ids, listing, latest))
    if previous is not None:
        earlier = entry_ids(previous)
        if not earlier:
            problems.append("the earlier copy of the feed could not be read, or has no entry")
        for identity in earlier:
            if identity not in seen:
                problems.append("the earlier copy's entry %s is not here: its id changed, or the release was removed" % identity)
    return problems


def check_against_listing(entries, ids, listing, latest):
    """The entries against releases.json (a list of release objects) and
    releases/latest (one release object, or None)."""
    problems = []
    if not isinstance(listing, list):
        return ["releases.json is not a list of releases"]
    tags = [r.get("tag_name") if isinstance(r, dict) else None for r in listing]
    if len(entries) != len(listing):
        problems.append("the feed has %d entries and releases.json lists %d releases" % (len(entries), len(listing)))
    if listing and entries and ids[0] != ENTRY_ID_PREFIX + str(tags[0]):
        problems.append("the newest entry is %s, not the newest release releases.json lists, %s" % (ids[0], tags[0]))
    for number, (entry, release) in enumerate(zip(entries, listing), 1):
        if not isinstance(release, dict):
            problems.append("releases.json's release %d is not an object" % number)
            continue
        tag = release.get("tag_name")
        where = "entry %d (%s)" % (number, tag)
        if ids[number - 1] != ENTRY_ID_PREFIX + str(tag):
            problems.append("%s has the id %s, not %s%s" % (where, ids[number - 1], ENTRY_ID_PREFIX, tag))
        # The release's name, or its tag when the name is blanks (or what XML
        # cannot carry): generate-update-feed.py's atom_title.
        name = as_xml_reads("%s" % release.get("name"))
        name = name if name.strip() else as_xml_reads("%s" % tag)
        title = entry.find(atom("title"))
        if title is None or "".join(title.itertext()) != name:
            problems.append("%s's title is not the release's name, %r" % (where, release.get("name")))
        alternates = [l.get("href") for l in entry.findall(atom("link")) if l.get("rel", "alternate") == "alternate"]
        if alternates != [release.get("html_url")]:
            problems.append("%s's alternate link is %s, not the release's page %s" % (where, alternates, release.get("html_url")))
        updated = parse_date((entry.findtext(atom("updated")) or "").strip())
        released = parse_date(release.get("published_at") or "")
        if updated is None or updated != released:
            problems.append("%s's updated time is not the release's, %s" % (where, release.get("published_at")))
        terms = [c.get("term") for c in entry.findall(atom("category"))]
        want = "pre-release" if release.get("prerelease") else "release"
        if want not in terms or ({"release", "pre-release"} - {want}) & set(terms):
            problems.append("%s's category is %s, not %s" % (where, terms, want))
    if latest is not None:
        tag = latest.get("tag_name") if isinstance(latest, dict) else None
        if ENTRY_ID_PREFIX + str(tag) not in ids:
            problems.append("releases/latest is %s, and the feed has no entry for it" % tag)
    return problems


# ---------------------------------------------------------------- reading a feed

def read_url(url):
    request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-check-atom-feed"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def read_source(source):
    """A file's bytes, or a URL's."""
    if re.match(r"^https?://", source):
        return read_url(source)
    with open(source, "rb") as f:
        return f.read()


def read_json(path):
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8"))


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_update_feed", os.path.join(ROOT, "build", "generate-update-feed.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def canned_tree(feed, project_url=None, extra=None):
    """The tree generate-update-feed.py renders from five releases held here: a
    stable one, its release candidate before it, the one before, one re-hosted
    from GitHub whose name carries markup, and a maintenance release of an older
    line published last. project_url stands in for the project's address (its
    move); extra adds (tag, name, released, notes) releases."""
    project_url = project_url or feed.PROJECT_URL
    registry = feed.API + "/packages/generic/hmailserver/"
    files = {}

    def release(tag, name, released, notes):
        version = tag[1:]
        names = ["hMailServer-%s-x64.exe" % version, "hMailServer-%s-x64.exe.cosign.bundle" % version]
        files[("hmailserver", version)] = dict(
            (n, {"id": i + 1, "file_name": n, "size": 1000 + i, "file_sha256": "%064x" % (i + 1)}) for i, n in enumerate(names))
        return {"tag_name": tag, "name": name, "released_at": released, "upcoming_release": False,
                "_links": {"self": project_url + "/-/releases/" + tag},
                "assets": {"count": len(names), "sources": [], "links": [{"name": n, "url": registry + version + "/" + n} for n in names]},
                "description": notes}

    releases = [
        release("v6.2.29", "hMailServer 6.2.29", "2026-10-03T09:00:00.000Z", "A maintenance release of the 6.2 line."),
        release("v6.3.6", "hMailServer 6.3.6", "2026-10-02T12:00:00.000+01:00",
                "# hMailServer 6.3.6\n\nThe last release for a while: **packages** for every distribution, the `.deb` and "
                "[the image](https://example.com/image).\n\n## Before you upgrade\n\n- Back up."),
        release("v6.3.6-rc1", "Pre-release: hMailServer 6.3.6 RC 1", "2026-10-01T08:00:00.000Z", ""),
        release("v6.3.5", "hMailServer 6.3.5", "2026-09-28T20:00:00.000Z", None),
        release("v6.3.3", "hMailServer 6.3.3 <script>alert(1)</script> & co", "2026-09-15T01:02:52.000Z",
                "*Re-published on GitLab on 24 September 2026 from the GitHub release of 15 September 2026.*\n\n"
                "# hMailServer 6.3.3\n\nA message larger than 60,000 bytes sent with CHUNKING stalled \\- and does not now; "
                "see <https://example.com/notes> & the \"docs\".\n\n| a | b |\n|---|---|\n"),
    ]
    for tag, name, released, notes in extra or []:
        releases.append(release(tag, name, released, notes))
    return feed.render(releases, files)


# ---------------------------------------------------------------- the self-test

def self_test():
    """The failures, as a list: none when a good feed passes and every broken one
    is refused for its own reason."""
    feed = load_generator()
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    tree, latest, warnings = canned_tree(feed)
    good = tree["releases.atom"]
    listing = json.loads(tree["releases.json"].decode("utf-8"))
    expect(not warnings, "the canned releases render without a warning: %s" % warnings)
    expect(latest["tag_name"] == "v6.3.6", "releases/latest is 6.3.6, not 6.2.29, published after it")
    problems = check_atom(good, listing, latest)
    expect(problems == [], "the rendered feed passes, held to releases.json and releases/latest: %s" % problems)
    expect(entry_ids(good) == [ENTRY_ID_PREFIX + r["tag_name"] for r in listing],
           "an entry for every release, in releases.json's order: %s" % entry_ids(good))
    expect(entry_ids(good)[0] == ENTRY_ID_PREFIX + "v6.2.29", "the newest entry is the newest release by date")
    expect(b"<script" not in good and b"&lt;script&gt;alert(1)&lt;/script&gt; &amp; co" in good,
           "markup in a release's name is escaped")
    expect(b"<summary>The last release for a while: packages for every distribution, the .deb and the image.</summary>" in good,
           "the summary is the notes' first paragraph as plain text: %s" % re.findall(rb"<summary>[^<]*6\.3\.6[^<]*</summary>|<summary>The last[^<]*</summary>", good))
    expect(b"<summary>A message larger than 60,000 bytes sent with CHUNKING stalled - and does not now; see https://example.com/notes &amp; the &quot;docs&quot;.</summary>" in good,
           "a re-hosted release's summary skips the re-hosting note: %s" % re.findall(rb"<summary>A message[^<]*</summary>", good))
    expect(b"<summary>hMailServer 6.3.5</summary>" in good and b"<summary>Pre-release: hMailServer 6.3.6 RC 1</summary>" in good,
           "a release without notes is summarised by its name")
    expect(check_atom(good, listing, latest, previous=good) == [], "a feed is its own earlier copy")

    # Stable: the same releases give the same bytes, and a move of the project,
    # an edited name and notes, and a new release change no id.
    again = canned_tree(feed)[0]["releases.atom"]
    expect(again == good, "the same releases render the same bytes")
    moved = canned_tree(feed, project_url="https://gitlab.com/elsewhere/hmailserver",
                        extra=[("v6.3.7", "hMailServer 6.3.7", "2026-11-01T00:00:00.000Z", "Next.")])
    moved_listing = json.loads(moved[0]["releases.json"].decode("utf-8"))
    problems = check_atom(moved[0]["releases.atom"], moved_listing, moved[1], previous=good)
    expect(problems == [], "after the move and a new release every earlier id is still there: %s" % problems)
    expect(b"https://gitlab.com/elsewhere/hmailserver/-/releases/v6.3.6" in moved[0]["releases.atom"],
           "and the links follow the move")

    def refused(content, why, words, feed_listing=listing, feed_latest=latest, previous=None):
        found = check_atom(content, feed_listing, feed_latest, previous)
        expect(any(words in p for p in found), "%s is refused, saying %r: %s" % (why, words, found))

    def edited(old, new, count=1):
        expect(old in good, "the canned feed holds %r" % old)
        return good.replace(old, new, count)

    first_entry = good[good.index(b" <entry>"):good.index(b" </entry>") + len(b" </entry>\n")]

    refused(good[:-40], "a feed cut short", "not well-formed")
    refused(good.decode("utf-8").encode("utf-16"), "a feed in UTF-16", "not UTF-8")
    refused(edited(b'encoding="utf-8"', b'encoding="iso-8859-1"'), "a feed that says it is Latin-1", "encoding iso-8859-1")
    refused(edited(b"<feed ", b'<!DOCTYPE feed [<!ENTITY a "aaaaaaaa">]>\n<feed '), "a document type declaration", "document type")
    refused(edited(b'xmlns="http://www.w3.org/2005/Atom"', b'xmlns="http://purl.org/atom/ns#"'), "the Atom 0.3 namespace",
            "not feed in")
    refused(edited(b" <id>%s</id>\n" % FEED_ID.encode(), b""), "a feed without an id", "the feed has 0 atom:id")
    refused(edited(b" <title>hMailServer releases</title>\n", b" <title>hMailServer releases</title>\n <title>again</title>\n"),
            "a feed with two titles", "the feed has 2 atom:title")
    refused(edited(b"<author><name>The hMailServer maintainers</name>", b"<contributor><name>The hMailServer maintainers</name>")
            .replace(b"</uri></author>", b"</uri></contributor>", 1), "no author anywhere", "has no author")
    refused(edited(b"<author><name>The hMailServer maintainers</name>", b"<author>"), "an author without a name", "atom:name")
    refused(edited(b" <generator>", b" <version>1</version>\n <generator>"), "an element the RFC does not define",
            "does not define there")
    refused(edited(b" <id>%s</id>" % FEED_ID.encode(), b" <id>https://updates.progressiverobot.com/releases.atom</id>"),
            "another feed id", "take it for another feed")
    refused(edited(b'rel="self" type="application/atom+xml" href="%s"' % SELF_URL.encode(),
                   b'rel="self" type="application/atom+xml" href="https://example.com/releases.atom"'), "a wrong self link", "self link")
    refused(edited(b'<link rel="alternate" type="text/html" href="https://gitlab.com',
                   b'<link rel="alternate" type="text/html" href="https://example.com/"/>\n <link rel="alternate" type="text/html" href="https://gitlab.com'),
            "two alternate links of one type", "two alternate links")
    refused(edited(b'<link rel="self" type="application/atom+xml" href', b'<link rel="self" type="application/atom+xml" hreff'),
            "a link without an href", "no href")
    refused(edited(b"<summary>hMailServer 6.3.5</summary>", b"<summary>hMailServer <b>6.3.5</b></summary>"),
            "markup in a text summary", "holds child elements")
    refused(edited(b"<summary>hMailServer 6.3.5</summary>", b'<summary type="markdown">hMailServer 6.3.5</summary>'),
            "a text construct of another type", "type 'markdown'")
    refused(edited(b'<category term="pre-release" label="Pre-release"/>', b'<category label="Pre-release"/>'),
            "a category without a term", "no term")

    # Ids.
    refused(edited(b"<id>%sv6.3.5</id>" % ENTRY_ID_PREFIX.encode(), b"<id>%sv6.3.6</id>" % ENTRY_ID_PREFIX.encode()),
            "two entries with one id", "two entries have the id")
    refused(edited(b"<id>%sv6.3.5</id>" % ENTRY_ID_PREFIX.encode(), b"<id>https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.5</id>"),
            "an entry id made from the release's page", "made from the release's tag alone")
    refused(edited(b"<id>%sv6.3.5</id>" % ENTRY_ID_PREFIX.encode(), b"<id>v6.3.5 release</id>"), "an id that is no IRI",
            "not an absolute IRI")
    refused(edited(b"<id>%sv6.3.5</id>" % ENTRY_ID_PREFIX.encode(), b""), "an entry without an id", "has 0 atom:id")
    refused(good, "an earlier copy with an id since changed", "its id changed",
            previous=good.replace(ENTRY_ID_PREFIX.encode() + b"v6.3.3", b"tag:example.com,2026:v6.3.3"))
    refused(good, "an earlier copy that cannot be read", "could not be read", previous=b"<feed")

    # Dates.
    refused(edited(b"<updated>2026-09-28T20:00:00Z</updated>", b"<updated>2026-09-28 20:00:00Z</updated>"),
            "a date with a space for the T", "not an RFC 3339 date-time")
    refused(edited(b"<updated>2026-09-28T20:00:00Z</updated>", b"<updated>2026-09-28t20:00:00z</updated>"),
            "a date with a lower-case t and z", "upper-case T and Z")
    refused(edited(b"<published>2026-09-28T20:00:00Z</published>", b"<published>2026-02-30T20:00:00Z</published>"),
            "the thirtieth of February", "not an RFC 3339 date-time")
    refused(edited(b"<updated>2026-09-28T20:00:00Z</updated>", b"<updated>2026-09-28T20:00:00+25:00</updated>"),
            "an offset of 25 hours", "not an RFC 3339 date-time")
    refused(edited(b"<updated>2026-09-28T20:00:00Z</updated>", b"<updated>2026-10-28T20:00:00Z</updated>"),
            "an entry newer than the one before it", "not newest first")
    refused(edited(b" <updated>2026-10-03T09:00:00Z</updated>", b" <updated>2026-01-01T00:00:00Z</updated>"),
            "a feed older than its newest entry", "older than its newest entry's")
    expect(parse_date("2026-12-31T23:59:60Z") is not None and parse_date("2026-09-28T21:00:00+01:00") == parse_date("2026-09-28T20:00:00Z"),
           "a leap second and an offset are RFC 3339 date-times")

    # An entry's content.
    no_link = edited(b'  <link rel="alternate" type="text/html" href="%s/-/releases/v6.3.5"/>\n' % feed.PROJECT_URL.encode(), b"")
    refused(no_link, "an entry with no content and no alternate link", "no alternate link")
    refused(no_link.replace(b"<summary>hMailServer 6.3.5</summary>", b'<content src="https://example.com/notes"/>'), "content by src with no summary",
            "has no atom:summary")

    # Against releases.json and releases/latest.
    refused(good.replace(first_entry, b""), "a feed without the newest release", "not the newest release releases.json lists")
    refused(good, "a release releases.json lists and the feed has not", "the feed has 5 entries and releases.json lists 6",
            feed_listing=listing + [dict(listing[-1], tag_name="v6.0.0")])
    swapped = list(listing)
    swapped[1], swapped[2] = swapped[2], swapped[1]
    refused(good, "entries in another order than releases.json's", "has the id", feed_listing=swapped)
    refused(good, "a title other than the release's name", "title is not the release's name",
            feed_listing=[dict(r, name=r["name"] + " (edited)") if r["tag_name"] == "v6.3.5" else r for r in listing])
    refused(good, "a link other than the release's page", "not the release's page",
            feed_listing=[dict(r, html_url="https://example.com/") if r["tag_name"] == "v6.3.5" else r for r in listing])
    refused(good, "a time other than the release's", "updated time is not the release's",
            feed_listing=[dict(r, published_at="2026-09-28T20:00:01Z") if r["tag_name"] == "v6.3.5" else r for r in listing])
    refused(good, "a pre-release filed as a release", "category is",
            feed_listing=[dict(r, prerelease=True) if r["tag_name"] == "v6.3.5" else r for r in listing])
    refused(good, "a releases/latest the feed has no entry for", "has no entry for it", feed_latest=dict(latest, tag_name="v6.4.0"))
    return failures


# ---------------------------------------------------------------- main

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("path", nargs="?", help="a feed tree (public/) or a releases.atom")
    parser.add_argument("--site", help="the deployed site, instead of a path")
    parser.add_argument("--previous", help="an earlier releases.atom, a file or a URL, whose ids must all still be here")
    parser.add_argument("--self-test", action="store_true", help="the negative controls, with no network")
    args = parser.parse_args(argv)

    if args.self_test:
        failures = self_test()
        for failure in failures:
            print("FAILED: " + failure)
        print("self-test: %d failure(s)" % len(failures))
        return 1 if failures else 0

    try:
        previous = read_source(args.previous) if args.previous else None
        if args.site:
            site = args.site.rstrip("/")
            where = site + "/releases.atom"
            content = read_url(where)
            listing = json.loads(read_url(site + "/releases.json").decode("utf-8"))
            latest = json.loads(read_url(site + "/releases/latest").decode("utf-8"))
        elif args.path:
            directory = args.path if os.path.isdir(args.path) else os.path.dirname(os.path.abspath(args.path))
            where = os.path.join(directory, "releases.atom") if os.path.isdir(args.path) else args.path
            with open(where, "rb") as f:
                content = f.read()
            listing = read_json(os.path.join(directory, "releases.json"))
            latest = read_json(os.path.join(directory, "releases", "latest"))
            if listing is None:
                print("NOTE: %s has no releases.json beside it; the feed is checked on its own." % where)
        else:
            where = "the feed build/generate-update-feed.py renders from the releases held here"
            tree, latest, _ = canned_tree(load_generator())
            content = tree["releases.atom"]
            listing = json.loads(tree["releases.json"].decode("utf-8"))
    except (OSError, ValueError, urllib.error.URLError) as e:
        print("check-atom-feed: %s" % e)
        return 1

    problems = check_atom(content, listing, latest, previous)
    for problem in problems:
        print("PROBLEM: " + problem)
    if problems:
        print("check-atom-feed: %s: %d problem(s)" % (where, len(problems)))
        return 1
    ids = entry_ids(content)
    print("check-atom-feed: %s is a valid Atom feed of %d releases, the newest %s%s." % (
        where, len(ids), ids[0][len(ENTRY_ID_PREFIX):] if ids else "none",
        ", held to its releases.json" if listing is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
