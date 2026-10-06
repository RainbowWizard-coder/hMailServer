#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The update feed an installed server reads, rendered from the GitLab releases.

A server's live update (Common/Util/UpdateChecker.cpp) reads a release feed in
the GitHub Releases API's shape. It read GitHub's own until GitHub suspended the
project's account on 18 September 2026; since then it reads
https://updates.progressiverobot.com, which GitLab Pages serves from this
project, and which this script makes from the project's GitLab releases (the
public API, no token: the project is public) and the files they link to in the
generic package registry. The files it writes, under public/:

  releases/latest         the newest release that is not a pre-release, as one
                          release object: the stable channel's default
  releases.json           every release, newest first, as a list: the
                          pre-release channel's default, and the one value of
                          UpdateFeedUrl that serves both channels
                          (docs/UpdateFeed.md)
  releases/index.html     a page showing that list, for a browser that follows
                          Pages' 302 from /releases to /releases/; HTML, with
                          the list escaped, so nothing in a release's name runs
                          on the updates domain
  releases/tags/<tag>     one release object per release: the rollback's lookup
  releases.atom           the same releases, newest first, as an Atom feed
                          (RFC 4287) for a feed reader, below; no server reads it
  index.html              a page for a person who opens the address
  _redirects              GitLab Pages' redirect rules: the stable download
                          links, below; for people, never a server

The Atom feed. releases.atom has an entry for every release releases.json
lists, in its order: the release's name as the title, its page on GitLab as the
link, the time it was released as the entry's updated and published times, a
category saying release or pre-release, and a summary: the first paragraph of
the release's own notes, the description GitLab holds, as plain text (the note a
re-hosted release opens with, headings, tables and code left out; at most
SUMMARY_MAX characters). The notes rather than CHANGELOG.md's section because the
feed is made from the releases alone, whichever commit runs this - a CHANGELOG
is the commit's, it holds no section for a release re-hosted from GitHub or
made by hand, and --check --site compares with what the API says now - and
because a release's notes are what its page shows. The feed's updated time is
the newest entry's, so a render changes the file only when a release does.
Every id is a tag URI made from the release's tag alone (ATOM_ID_PREFIX), so it
stays the same across renders, edits of a release, and a move of the project,
such as its move into the group hmailserver on 29 September 2026, which changed
every release page's address: a reader remembers entries by id, and an id made
from the page would have shown every release again after the move. build/check-atom-feed.py holds the feed to RFC 4287 and
to releases.json; --self-test and --serve-check run it.

The stable download links. /download/<name> answers a 302 to the file of the
release releases/latest is - the newest stable VERSION - in the package
registry: /download/hMailServer-x64.exe, /download/hmailserver_amd64.deb,
/download/hmailserver.x86_64.rpm and the name of every other kind of
installation installer_names() knows, the version taken out as a release's own
permanent link takes it (release-chain.py's permalink_name), each with its
.cosign.bundle beside it. A name gets a rule only when that release carries the
file, so a missing package is a 404, never another release's. /download/ goes to
that release's page. GitLab's own permalink/latest follows released_at and has no
pre-release flag, so a pre-release or the build-inputs release published after
a stable one takes it over; these do not move until a stable release with a
higher version is published. Pages serves a file before a rule, answers only
301, 302 and 200 (and an absolute target only with a 301 or a 302), ignores a
trailing slash when it matches, and applies the rules to the custom domain's
paths; the gitlab.io address would need the project's path in front of them.

Every URL the server derives is a FILE there, answered 200 with no redirect,
because HttpsClient::Request follows none. GitLab Pages serves a path that is a
file as it is; answers a directory's path without the slash with a 302 to the
path with it, and with the slash with its index.html; and tries <path>.html
for a path that is neither. It ignores the query string. So /releases/latest,
/releases.json and /releases/tags/v6.3.3 are 200, and the bare /releases -
a directory here, since releases/latest is in it - and /releases?per_page=10,
GitHub's list address, are a 302 a server would report as "The update feed
answered HTTP 302."; neither is a default or in the documentation.

What a release object holds is exactly what UpdateChecker::ReadRelease_ reads
and nothing else: tag_name, name, html_url, published_at, draft, prerelease and
assets, each asset its name, browser_download_url, size and digest. The URL is
the release link's own, a file of the project's generic package registry
(.../packages/generic/hmailserver/<version>/<file>, anonymous for a public
project), and the convention every release follows is held: the link is named
after the file it points to, and the package's version is the tag without its
v. The size and the "sha256:<hex>" digest are the registry's own record of the
file, which the server holds the download to. A Sigstore bundle is an asset like
any other, named <artefact>.cosign.bundle, which is how the server finds it.
GitLab has no pre-release flag: a release is one when its tag has a suffix -
anything after the numbers but digits and dots, v6.2.23-alpha1 or v6.3.5rc1 -
which is how every pre-release so far was published, and the mark a server reads
whatever the feed says. A release whose name says "Pre-release" but whose tag
has no suffix breaks that rule and is not published. A release whose tag is not v<digit>... is not
a server release (build-inputs-1 carries build inputs) and is left out; so is
one GitLab calls upcoming, whose date has not come.

A release with a link that breaks those rules is left out of the feed with a
warning, so that one old release cannot hold every later one back - except the
newest stable release, which a feed without it would misstate, and which stops
the render instead. The JSON escapes <, > and & so that no file of the site
carries markup from a release's name.

Usage, from the repository root:
  python3 build/generate-update-feed.py [--output DIR]
      write the tree (default public/); refuses a non-empty DIR that is not one
  python3 build/generate-update-feed.py --check [--output DIR | --site URL]
      exit 1 when DIR, or the site at URL, is not what the API says now
  python3 build/generate-update-feed.py --serve-check DIR
      serve DIR as GitLab Pages would and read every URL a server derives,
      both channels and a rollback, with a copy of the server's parser, and
      the Atom feed with build/check-atom-feed.py
  python3 build/generate-update-feed.py --self-test
      the above against canned API answers, with no network
"""
import argparse
import datetime
import html
import html.entities
import http.server
import json
import os
import re
import shutil
import socketserver
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER_SOURCE = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util", "UpdateChecker.cpp")

PROJECT_ID = "86730559"
PROJECT_URL = "https://gitlab.com/hmailserver/hmailserver"
API = "https://gitlab.com/api/v4/projects/" + PROJECT_ID
SITE = "https://updates.progressiverobot.com"
DOCUMENT_URL = PROJECT_URL + "/-/blob/master/hmailserver/docs/UpdateFeed.md"

# Releases published with an installer and no Sigstore bundle on purpose, and why.
# A server offered one says the release "carries no Sigstore bundle for its
# installer, so it cannot be verified" and downloads nothing
# (UpdateDownloader.cpp), which is what is wanted: it is installed by hand. The
# serve check notes such a release as the stable one instead of failing on it;
# any other stable release without its bundle still fails.
UNSIGNED_BY_DESIGN = {
    "6.3.4": "built and Authenticode-signed by hand after GitHub's suspension; the GitLab CI signing job arrives with 6.3.5",
}

# A link of a release is a file of this project's generic package registry.
REGISTRY_LINK = re.compile(r"^https://gitlab\.com/api/v4/projects/" + PROJECT_ID + r"/packages/generic/([^/?#]+)/([^/?#]+)/([^/?#]+)$")
# A server release's tag: v, a digit, then what the server accepts in a tag.
SERVER_TAG = re.compile(r"^v[0-9][A-Za-z0-9._-]{0,62}$")
TIMESTAMP = re.compile(r"^(\d{4}-\d{2}-\d{2})T(\d{2}:\d{2}:\d{2})(?:\.\d+)?(Z|[+-]\d{2}:\d{2})$")

RELEASE_KEYS = ["tag_name", "name", "html_url", "published_at", "draft", "prerelease", "assets"]
ASSET_KEYS = ["name", "size", "digest", "browser_download_url"]

# The Atom feed (releases.atom). Its ids are tag URIs (RFC 4151) on the feed's own
# name, minted in 2026, and an entry's is the release's tag after "releases/":
# nothing in them moves with the project's path. build/check-atom-feed.py holds
# a feed to exactly these.
ATOM_PATH = "releases.atom"
ATOM_NS = "http://www.w3.org/2005/Atom"
ATOM_ID_PREFIX = "tag:updates.progressiverobot.com,2026:"
ATOM_FEED_ID = ATOM_ID_PREFIX + "releases"
# The longest summary, in characters; a longer first paragraph is cut at a
# sentence, or else a word, before it.
SUMMARY_MAX = 1000


class FeedError(Exception):
    pass


# ---------------------------------------------------------------- the API

def http_get(url, attempts=5):
    """GET url anonymously: (body bytes, headers). Retries a 429 or a 5xx."""
    request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-update-feed", "Accept": "application/json"})
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.read(), dict((k.lower(), v) for k, v in response.headers.items())
        except urllib.error.HTTPError as e:
            if (e.code == 429 or e.code >= 500) and attempt < attempts - 1:
                wait = e.headers.get("Retry-After", "")
                time.sleep(int(wait) if wait.isdigit() else 5 * (attempt + 1))
                continue
            raise FeedError("%s answered HTTP %d" % (url, e.code))
        except (urllib.error.URLError, OSError) as e:
            if attempt < attempts - 1:
                time.sleep(5 * (attempt + 1))
                continue
            raise FeedError("%s could not be read: %s" % (url, e))
    raise FeedError("%s could not be read" % url)


def get_all(get, path):
    """Every item of a paginated API list, following X-Next-Page."""
    items = []
    page = "1"
    for _ in range(100):
        separator = "&" if "?" in path else "?"
        body, headers = get(API + path + separator + "per_page=100&page=" + page)
        chunk = json.loads(body.decode("utf-8"))
        if not isinstance(chunk, list):
            raise FeedError("%s is not a list" % path)
        items.extend(chunk)
        page = (headers.get("x-next-page") or "").strip()
        if not page:
            return items
    raise FeedError("%s has more than a hundred pages" % path)


def load(get):
    """The releases, and the registry's record of every file they link to."""
    releases = get_all(get, "/releases?order_by=released_at&sort=desc")

    wanted = set()
    for release in releases:
        for link in (release.get("assets") or {}).get("links") or []:
            match = REGISTRY_LINK.match(link.get("url") or "")
            if match:
                wanted.add((urllib.parse.unquote(match.group(1)), urllib.parse.unquote(match.group(2))))

    packages = get_all(get, "/packages?package_type=generic&status=default")
    ids = {}
    for package in packages:
        key = (package.get("name"), package.get("version"))
        if key in wanted:
            ids.setdefault(key, []).append(package["id"])

    files = {}
    for key in sorted(ids):
        record = {}
        for package_id in sorted(ids[key]):
            for entry in get_all(get, "/packages/%d/package_files" % package_id):
                # A file uploaded twice is served as the later upload.
                name = entry.get("file_name")
                if name not in record or entry.get("id", 0) > record[name].get("id", 0):
                    record[name] = entry
        files[key] = record
    return releases, files


# ---------------------------------------------------------------- the feed

def parse_version(text):
    """UpdateChecker.cpp's ParseVersion_: four numbers, from after an optional v
    to the first character that is neither a digit nor a dot."""
    components = [0, 0, 0, 0]
    position = 1 if text[:1] in ("v", "V") else 0
    index = 0
    while position < len(text) and index < 4:
        c = text[position]
        if "0" <= c <= "9":
            if components[index] < 100000000:
                components[index] = components[index] * 10 + ord(c) - ord("0")
            position += 1
        elif c == ".":
            index += 1
            position += 1
        else:
            break
    return components


def compare_versions(left, right):
    a, b = parse_version(left), parse_version(right)
    return (a > b) - (a < b)


def utc_timestamp(text):
    """GitLab's released_at as GitHub wrote published_at: 2026-09-15T01:02:52Z."""
    match = TIMESTAMP.match(text or "")
    if not match:
        raise FeedError("released_at %r is not an ISO 8601 time" % text)
    moment = datetime.datetime.fromisoformat(match.group(1) + "T" + match.group(2) + (
        "+00:00" if match.group(3) == "Z" else match.group(3)))
    return moment.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def has_suffix(version):
    """UpdateChecker's HasSuffix_: anything in a version but digits and dots, after
    an optional v - "-rc1", "rc1", "_rc1", ".rc1". The version comparison stops at
    the first such character, so 6.3.5rc1 would compare as 6.3.5 if a dash were
    the only mark."""
    if version[:1] in ("v", "V"):
        version = version[1:]
    return any(c not in "0123456789." for c in version)


def is_prerelease(release):
    """A suffixed tag (v6.3.5-rc1, v6.3.5rc1). A server tells a pre-release by its
    version's suffix whatever the feed's flag says, so the tag is the only mark a
    feed that was tampered with cannot take off; a release named as a pre-release
    whose tag has no suffix is refused in render_release."""
    return has_suffix(release["tag_name"])


def render_release(release, files, errors):
    tag = release["tag_name"]
    if not has_suffix(tag) and (release.get("name") or "").strip().lower().startswith("pre-release"):
        errors.append("%s: the name says it is a pre-release but the tag has no suffix, so a server could not tell it from the "
                      "final release; tag a pre-release v<version>-<suffix>" % tag)
    assets = []
    seen = set()
    for link in sorted((release.get("assets") or {}).get("links") or [], key=lambda l: l.get("name") or ""):
        name = link.get("name") or ""
        url = link.get("url") or ""
        match = REGISTRY_LINK.match(url)
        if not match:
            errors.append("%s: the link %r goes to %s, which is not this project's package registry" % (tag, name, url))
            continue
        package = (urllib.parse.unquote(match.group(1)), urllib.parse.unquote(match.group(2)))
        file_name = urllib.parse.unquote(match.group(3))
        entry = files.get(package, {}).get(file_name)
        if entry is None:
            errors.append("%s: the link %r names %s %s/%s, which the registry does not have" % (tag, name, file_name, package[0], package[1]))
            continue
        # The link is the file it names, of this release's own package: a link
        # named for one release's installer that points at another's would have a
        # server install the other and find the "newer" release again next time.
        if file_name != name:
            errors.append("%s: the link %r points at the file %s, which has another name" % (tag, name, file_name))
            continue
        if package != ("hmailserver", tag[1:]):
            errors.append("%s: the link %r is a file of the package %s %s, not of hmailserver %s" % (tag, name, package[0], package[1], tag[1:]))
            continue
        if name in seen:
            errors.append("%s: two links are named %r" % (tag, name))
            continue
        seen.add(name)
        digest = (entry.get("file_sha256") or "").lower()
        if not re.match(r"^[0-9a-f]{64}$", digest):
            errors.append("%s: the registry has no SHA-256 for %s" % (tag, file_name))
            continue
        assets.append({
            "name": name,
            "size": int(entry.get("size") or 0),
            "digest": "sha256:" + digest,
            "browser_download_url": url,
        })
    links = release.get("_links") or {}
    return {
        "tag_name": tag,
        "name": release.get("name") or tag,
        "html_url": links.get("self") or (PROJECT_URL + "/-/releases/" + urllib.parse.quote(tag)),
        "published_at": utc_timestamp(release.get("released_at")),
        "draft": False,
        "prerelease": is_prerelease(release),
        "assets": assets,
    }


def feed_bytes(value):
    text = json.dumps(value, ensure_ascii=False, indent=1)
    # No file of the site carries markup, whatever a release's name holds; every
    # JSON reader, the server's among them, reads the escapes back as the three
    # characters.
    text = text.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    return (text + "\n").encode("utf-8")


# ---------------------------------------------------------------- the Atom feed

def atom_entry_id(tag):
    """An entry's id: the release's tag, which a protected tag keeps for good."""
    return ATOM_ID_PREFIX + "releases/" + tag


# What XML 1.0 cannot carry at all (the C0 controls but tab, line feed and
# carriage return, a lone surrogate, U+FFFE and U+FFFF), and so is left out.
XML_UNUSABLE = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ud800-\udfff\ufffe\uffff]")


def xml_text(value):
    """value as XML character data or an attribute's value in double quotes."""
    value = XML_UNUSABLE.sub("", "%s" % value)
    return value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def atom_title(name, tag):
    """An entry's title: the release's name as XML carries it, or its tag when
    that leaves nothing to show. An empty atom:title fails check-atom-feed.py,
    and with it the pages job, which would keep a release's name of blanks from
    reaching the servers too; build/check-atom-feed.py expects the same."""
    text = XML_UNUSABLE.sub("", "%s" % name)
    return text if text.strip() else tag


# The note every release re-hosted from GitHub opens with: "*Re-published on
# GitLab on 24 September 2026 from the GitHub release of ...*". It is about the
# page, not the release, so the summary is the paragraph after it.
REHOSTED_NOTE = re.compile(r"^[*_]*Re-published on GitLab\b")
MARKDOWN_HEADING = re.compile(r"^#{1,6}(\s|$)")
MARKDOWN_RULE = re.compile(r"^([-*_])( *\1){2,} *$")
MARKDOWN_FENCE = re.compile(r"^(```|~~~)")
MARKDOWN_LIST_ITEM = re.compile(r"^([-*+]|\d{1,9}[.)])\s+")
CODE_SPAN = re.compile(r"(`+)(.+?)\1")
# A link's destination and title: (url), (url "title"), (<url with spaces>), and a
# url with one level of parentheses in it (https://host/a_(b)), as CommonMark reads them.
LINK_DESTINATION = r"\((?:\s*<[^<>]*>|(?:[^()\s]|\([^()\s]*\))*)(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^()]*\)))?\s*\)"
# Raw HTML inline, which a page renders as markup and never shows: a tag, open or
# closing, and a comment.
HTML_INLINE = re.compile(r"<!--.*?-->|</?[A-Za-z][A-Za-z0-9-]*"
                         r"(?:\s+[A-Za-z_:][A-Za-z0-9_.:-]*(?:\s*=\s*(?:[^\s\"'=<>`]+|'[^']*'|\"[^\"]*\"))?)*\s*/?>")
# A character reference, which a page shows as its character: CommonMark's, with
# the semicolon.
CHARACTER_REFERENCE = re.compile(r"&(?:#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});")


def plain_markdown(text):
    """One paragraph of Markdown as the text it shows: links and images as their
    text, raw HTML's tags gone and its character references as their characters,
    emphasis and backslash escapes gone, code spans as their code."""
    # A code span stands aside as a mark of the private use area while the rest
    # is read, so that emphasis around one (**after `354`, the fix.**) is still
    # seen as emphasis and nothing inside one is.
    code = []

    def aside(match):
        code.append(match.group(2).strip())
        return "\ue000%d\ue001" % (len(code) - 1)

    text = CODE_SPAN.sub(aside, text.replace("\ue000", "").replace("\ue001", ""))
    # GitHub's alert, "> [!WARNING]" and the quote under it.
    text = re.sub(r"^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*", lambda m: m.group(1).capitalize() + ": ", text)
    text = re.sub(r"!\[([^\]]*)\]" + LINK_DESTINATION, r"\1", text)
    text = re.sub(r"\[([^\]]+)\]" + LINK_DESTINATION, r"\1", text)
    text = re.sub(r"<(https?://[^>\s]+)>", r"\1", text)
    text = HTML_INLINE.sub("", text)
    text = re.sub(r"\*\*(?=\S)(.+?)(?<=\S)\*\*", r"\1", text)
    text = re.sub(r"(?<!\w)__(?=\S)(.+?)(?<=\S)__(?!\w)", r"\1", text)
    text = re.sub(r"(?<![\w*])\*(?=\S)(.+?)(?<=\S)\*(?![\w*])", r"\1", text)
    text = re.sub(r"(?<![\w_])_(?=\S)(.+?)(?<=\S)_(?![\w_])", r"\1", text)
    text = re.sub(r"\\([!-/:-@\[-`{-~])", r"\1", text)
    # Last, so that what a reference stands for is text and never markup; a
    # reference to the marks a code span stands aside with is left out, and a
    # name HTML does not define (&notaname;) is text as it stands.
    def character(match):
        reference = match.group(0)
        if not reference.startswith("&#") and reference[1:] not in html.entities.html5:
            return reference
        return html.unescape(reference).replace("\ue000", "").replace("\ue001", "")

    text = CHARACTER_REFERENCE.sub(character, text)
    text = re.sub("\ue000(\\d+)\ue001", lambda m: code[int(m.group(1))] if int(m.group(1)) < len(code) else "", text)
    return re.sub(r"\s+", " ", text).strip()


def release_summary(description, fallback):
    """The Atom summary of a release: the first paragraph of its notes that is
    prose - not the re-hosting note, a heading, a rule, a table or code - as
    plain text, cut to SUMMARY_MAX characters; fallback when there is none. The
    Download table build/ci/release-chain.py begins a release's notes with (from
    the release after 6.3.6, and 6.3.6's by hand) is a heading and a table, so
    the summary is still the first paragraph of the CHANGELOG section after it."""
    paragraphs = []
    current = []
    fenced = False
    for line in (description or "").replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        stripped = line.strip()
        if MARKDOWN_FENCE.match(stripped):
            fenced = not fenced
            stripped = ""
        elif fenced:
            continue
        if not stripped or MARKDOWN_HEADING.match(stripped) or stripped.startswith("|") or MARKDOWN_RULE.match(stripped):
            if current:
                paragraphs.append(current)
            current = []
            continue
        # A hard line break's backslash, and a list item's marker: the items of a
        # list read as one paragraph.
        stripped = MARKDOWN_LIST_ITEM.sub("", stripped.lstrip("> ")).rstrip("\\").rstrip()
        current.append(stripped)
    if current:
        paragraphs.append(current)
    for paragraph in paragraphs:
        text = " ".join(paragraph)
        if REHOSTED_NOTE.match(text):
            continue
        # Read no more of a paragraph than a summary can show, so that no release's
        # notes can make the markup's patterns work through a whole book.
        text = plain_markdown(text[:4 * SUMMARY_MAX])
        if not text:
            continue
        if len(text) > SUMMARY_MAX:
            cut = text[:SUMMARY_MAX - 4]
            sentence = cut.rfind(". ")
            word = cut.rfind(" ")
            if sentence >= SUMMARY_MAX // 2:
                text = cut[:sentence + 1] + " ..."
            elif word > 0:
                text = cut[:word].rstrip(",;:") + " ..."
            else:
                text = cut + " ..."
        return text
    return fallback


def atom_bytes(rendered, summaries):
    """releases.atom (RFC 4287): an entry for every release of rendered, the
    release objects of releases.json in its order, newest first. summaries maps a
    tag to its entry's summary. The same releases make the same bytes: the feed's
    updated time is its newest entry's, not the time of the render."""
    e = xml_text
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<feed xmlns="%s" xml:lang="en">' % ATOM_NS,
        ' <id>%s</id>' % e(ATOM_FEED_ID),
        ' <title>hMailServer releases</title>',
        ' <subtitle>Every release of hMailServer, newest first: the releases the update feed at %s gives a server, '
        'each with the first paragraph of its notes.</subtitle>' % e(SITE),
        ' <updated>%s</updated>' % e(max(r["published_at"] for r in rendered)),
        ' <link rel="self" type="application/atom+xml" href="%s"/>' % e(SITE + "/" + ATOM_PATH),
        ' <link rel="alternate" type="text/html" href="%s"/>' % e(PROJECT_URL + "/-/releases"),
        ' <author><name>The hMailServer maintainers</name><uri>%s</uri></author>' % e(PROJECT_URL),
        ' <generator>build/generate-update-feed.py</generator>',
    ]
    for release in rendered:
        tag = release["tag_name"]
        term, label = ("pre-release", "Pre-release") if release["prerelease"] else ("release", "Release")
        lines += [
            ' <entry>',
            '  <id>%s</id>' % e(atom_entry_id(tag)),
            '  <title>%s</title>' % e(atom_title(release["name"], tag)),
            '  <link rel="alternate" type="text/html" href="%s"/>' % e(release["html_url"]),
            '  <updated>%s</updated>' % e(release["published_at"]),
            '  <published>%s</published>' % e(release["published_at"]),
            '  <category term="%s" label="%s"/>' % (term, label),
            '  <summary>%s</summary>' % e(summaries.get(tag) or release["name"]),
            ' </entry>',
        ]
    lines.append('</feed>')
    return ("\n".join(lines) + "\n").encode("utf-8")


_ATOM_CHECKER = []


def atom_checker():
    """build/check-atom-feed.py, loaded once: what holds releases.atom to RFC 4287
    and to releases.json."""
    if not _ATOM_CHECKER:
        import importlib.util
        spec = importlib.util.spec_from_file_location("check_atom_feed", os.path.join(ROOT, "build", "check-atom-feed.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _ATOM_CHECKER.append(module)
    return _ATOM_CHECKER[0]


def render(releases, files):
    """The tree, as {relative path: bytes}, the release releases/latest is, and a
    warning for each release left out. A release that cannot be published as it
    is - a link outside the registry, to a file it lacks, or that breaks the
    naming convention - is left out, unless it is the newest stable release: a
    feed without that one would tell every stable server something false, so it
    stops the render (FeedError) and the site keeps what it had."""
    candidates = [r for r in releases if SERVER_TAG.match(r.get("tag_name") or "") and not r.get("upcoming_release")]
    if not candidates:
        raise FeedError("there is no server release to publish")

    newest_stable = None
    for release in candidates:
        if is_prerelease(release):
            continue
        if newest_stable is None or compare_versions(release["tag_name"], newest_stable["tag_name"]) > 0:
            newest_stable = release

    rendered = []
    warnings = []
    for release in candidates:
        errors = []
        try:
            item = render_release(release, files, errors)
        except FeedError as e:
            errors.append("%s: %s" % (release["tag_name"], e))
        if errors:
            if release is newest_stable:
                raise FeedError("the newest stable release, %s, cannot be published as it is:\n  %s" % (
                    release["tag_name"], "\n  ".join(errors)))
            warnings.append("%s is left out of the feed:\n  %s" % (release["tag_name"], "\n  ".join(errors)))
            continue
        rendered.append(item)

    tags = [r["tag_name"] for r in rendered]
    if len(set(tags)) != len(tags):
        raise FeedError("a tag is published twice: %s" % ", ".join(sorted(t for t in set(tags) if tags.count(t) > 1)))

    # Newest first, as GitHub listed them; equal dates by version, then by tag.
    rendered.sort(key=lambda r: (r["published_at"], parse_version(r["tag_name"]), r["tag_name"]), reverse=True)

    stable = [r for r in rendered if not r["prerelease"]]
    if not stable:
        raise FeedError("every release is a pre-release, so releases/latest would have nothing to say")
    # The newest stable VERSION, whatever its date: a maintenance release of an
    # older line published after a newer one must not become every server's latest.
    latest = stable[0]
    for candidate in stable[1:]:
        if compare_versions(candidate["tag_name"], latest["tag_name"]) > 0:
            latest = candidate

    # Each release's summary from its own notes, which the release objects do not
    # carry: a server reads exactly RELEASE_KEYS.
    summaries = dict((r["tag_name"], release_summary(r.get("description"), r.get("name") or r["tag_name"])) for r in candidates)

    tree = {
        "releases/latest": feed_bytes(latest),
        "releases.json": feed_bytes(rendered),
        "releases/index.html": releases_page(rendered).encode("utf-8"),
        ATOM_PATH: atom_bytes(rendered, summaries),
        "index.html": index_page(latest, rendered).encode("utf-8"),
        "_redirects": redirects_bytes(redirect_rules(latest)),
    }
    for release in rendered:
        tree["releases/tags/" + release["tag_name"]] = feed_bytes(release)
    return tree, latest, warnings


def releases_page(rendered):
    """/releases/: the list as a page, for a browser. No server reads it."""
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>hMailServer releases</title>
<link rel="alternate" type="application/atom+xml" title="hMailServer releases" href="../releases.atom">
<style>body{{font:16px/1.5 system-ui,sans-serif;max-width:60rem;margin:2rem auto;padding:0 16px}}pre{{white-space:pre-wrap;word-break:break-all}}</style>
</head>
<body>
<h1>hMailServer releases</h1>
<p>Every release in the update feed, newest first, as <a href="../releases.json"><code>releases.json</code></a> gives it to a server.
To follow them in a feed reader, subscribe to <a href="../releases.atom"><code>releases.atom</code></a>.</p>
<pre>{listing}</pre>
</body>
</html>
""".format(listing=html.escape(feed_bytes(rendered).decode("utf-8")))


def index_page(latest, rendered):
    e = html.escape
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>hMailServer update feed</title>
<link rel="alternate" type="application/atom+xml" title="hMailServer releases" href="releases.atom">
<style>body{{font:16px/1.5 system-ui,sans-serif;max-width:46rem;margin:2rem auto;padding:0 16px}}code{{word-break:break-all}}</style>
</head>
<body>
<h1>hMailServer update feed</h1>
<p>The release feed an installed hMailServer reads to find out whether a newer release exists.
It is made from the <a href="{project}/-/releases">project's releases on GitLab</a>, and the files it names are downloaded from the project's package registry.</p>
<p>The newest release that is not a pre-release is <a href="{latest_url}">{latest_name}</a>, published {latest_date}. The feed lists {count} releases.</p>
<ul>
<li><a href="releases/latest"><code>releases/latest</code></a>: the newest release that is not a pre-release, which a server on the stable channel reads.</li>
<li><a href="releases.json"><code>releases.json</code></a>: every release, which a server on the pre-release channel reads.</li>
<li><code>releases/tags/v&lt;version&gt;</code>: one release, which a server reads to roll back to the version it runs.</li>
<li><a href="releases.atom"><code>releases.atom</code></a>: every release as an Atom feed, newest first, for a feed reader. No server reads it.</li>
</ul>
<p>A server from 6.2.28 to 6.3.3 still looks for its updates on GitHub. One line in its <code>hMailServer.ini</code> points it here:
<code>UpdateFeedUrl={site}/releases.json</code>. <a href="{document}">How, and what it changes</a>.</p>
<h2>Download</h2>
<p>These links always lead to the newest release that is not a pre-release, now {latest_name}:</p>
<ul>
{downloads}
</ul>
</body>
</html>
""".format(project=e(PROJECT_URL), latest_url=e(latest["html_url"]), latest_name=e(latest["name"]),
           latest_date=e(latest["published_at"][:10]), count=len(rendered), site=e(SITE), document=e(DOCUMENT_URL),
           downloads="\n".join('<li><a href="{0}"><code>{0}</code></a></li>'.format(e(source))
                               for source, _, _ in redirect_rules(latest) if not source.endswith(".cosign.bundle")))


# ---------------------------------------------------------------- the download links

DOWNLOAD = "/download/"

# gitlab-pages' limits on _redirects (internal/redirects/redirects.go).
REDIRECTS_MAX_BYTES = 64 * 1024
REDIRECTS_MAX_RULES = 1000


def alias_name(name, version):
    """A package's name with its version taken out, as release-chain.py's
    permalink_name makes a release's permanent download name: hMailServer-x64.exe,
    hmailserver_amd64.deb, hmailserver.x86_64.rpm."""
    for marker, replacement in (("-%s-1." % version, "."), ("-%s" % version, ""), ("_%s" % version, "")):
        if marker in name:
            return name.replace(marker, replacement, 1)
    return name


def download_aliases():
    """{kind of installation: /download/<name>}, one for every kind installer_names knows."""
    return dict((platform, DOWNLOAD + alias_name(name, "9.9.9")) for platform, name in installer_names("9.9.9").items())


def redirect_rules(latest):
    """public/_redirects as (from, to, status), from the release object releases/latest
    is: /download/ to its page, and each download name to its file - the installer
    and its bundle - where it carries it."""
    version = latest["tag_name"][1:]
    urls = dict((a["name"], a["browser_download_url"]) for a in latest["assets"])
    rules = [(DOWNLOAD, latest["html_url"], 302)]
    aliases = download_aliases()
    for platform, name in sorted(installer_names(version).items()):
        for suffix in ("", ".cosign.bundle"):
            if name + suffix in urls:
                rules.append((aliases[platform] + suffix, urls[name + suffix], 302))
    return rules


def redirects_bytes(rules):
    return "".join("%s %s %d\n" % rule for rule in rules).encode("utf-8")


def parse_redirects(content):
    """(rules, errors) for a _redirects file, as gitlab-pages reads and validates it
    (internal/redirects/validations.go): each line `from to [status]`, 301 when no
    status is given; from a path from /; to a path from / or an http(s) URL; the
    status 200, 301 or 302, and an absolute target never with 200; no query
    parameters, no force (!). A rule it would refuse is an error here, and so is a
    splat or a placeholder, which this emulation does not follow."""
    errors = []
    if len(content) > REDIRECTS_MAX_BYTES:
        return [], ["_redirects is %d bytes; Pages reads none of a file over %d" % (len(content), REDIRECTS_MAX_BYTES)]
    rules = []
    for number, line in enumerate(content.decode("utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) not in (2, 3):
            errors.append("_redirects line %d is not `from to [status]`: %s" % (number, line))
            continue
        source, target = fields[0], fields[1]
        status_text = fields[2] if len(fields) == 3 else "301"
        if not status_text.isdigit():
            errors.append("_redirects line %d: %s is not a status Pages takes (a ! or a parameter is not supported)" % (number, status_text))
            continue
        status = int(status_text)
        absolute = target.startswith(("http://", "https://"))
        if not source.startswith("/") or source.startswith(("//", "/\\")) or "?" in source:
            errors.append("_redirects line %d: %s is not a path from /" % (number, source))
        elif not (absolute or (target.startswith("/") and not target.startswith(("//", "/\\")))):
            errors.append("_redirects line %d: %s is neither a path from / nor an http(s) URL" % (number, target))
        elif status not in (200, 301, 302):
            errors.append("_redirects line %d: Pages answers only 200, 301 and 302, not %d" % (number, status))
        elif absolute and status == 200:
            errors.append("_redirects line %d: Pages rewrites (200) only to a path of its own, not to %s" % (number, target))
        elif "*" in source + target or re.search(r"/:[A-Za-z]", source + urllib.parse.urlparse(target).path):
            errors.append("_redirects line %d: a splat or a placeholder, which this emulation does not follow" % number)
        else:
            rules.append((source, target, status))
    if len(rules) > REDIRECTS_MAX_RULES:
        errors.append("_redirects has %d rules; Pages reads the first %d" % (len(rules), REDIRECTS_MAX_RULES))
    return rules, errors


def redirect_path(path):
    """gitlab-pages' normalizePath (internal/redirects/helpers.go): a trailing slash
    does not matter."""
    return (path[:-1] if path.endswith("/") else path) + "/"


# ---------------------------------------------------------------- writing and checking

def read_tree(directory):
    tree = {}
    for base, _, names in os.walk(directory):
        for name in names:
            path = os.path.join(base, name)
            relative = os.path.relpath(path, directory).replace(os.sep, "/")
            with open(path, "rb") as f:
                tree[relative] = f.read()
    return tree


def write_tree(tree, directory):
    if os.path.isdir(directory) and os.listdir(directory):
        if not os.path.isfile(os.path.join(directory, "releases.json")):
            raise FeedError("%s is not empty and is not an update feed; nothing was written" % directory)
        shutil.rmtree(directory)
    for relative in sorted(tree):
        path = os.path.join(directory, *relative.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(tree[relative])


def site_path(relative):
    """The URL path Pages answers a file at: index.html at its directory's."""
    if relative == "index.html":
        return "/"
    if relative.endswith("/index.html"):
        return "/" + relative[:-len("index.html")]
    return "/" + relative


class NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def fetch_no_redirect(url):
    """(status, body, headers) for one GET, a redirect answered as it is."""
    handlers = [NoRedirects]
    if urllib.parse.urlparse(url).hostname == "127.0.0.1":
        # The loopback server is this process's own; no proxy in the environment
        # can reach it.
        handlers.append(urllib.request.ProxyHandler({}))
    opener = urllib.request.build_opener(*handlers)
    request = urllib.request.Request(url, headers={"User-Agent": "hMailServer", "Accept": "application/json"})
    try:
        with opener.open(request, timeout=30) as response:
            return response.status, response.read(), dict((k.lower(), v) for k, v in response.headers.items())
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict((k.lower(), v) for k, v in e.headers.items())


def differences(expected, actual, actual_name):
    problems = []
    for relative in sorted(expected):
        if relative not in actual:
            problems.append("missing from %s: %s" % (actual_name, relative))
        elif actual[relative] != expected[relative]:
            problems.append("differs in %s: %s" % (actual_name, relative))
    for relative in sorted(set(actual) - set(expected)):
        problems.append("in %s but not in the releases: %s" % (actual_name, relative))
    return problems


def check_site(tree, site):
    problems = []
    for relative in sorted(tree):
        # Pages answers /_redirects with its own page of the rules and whether
        # each is valid, not with the file: the rules are checked by what they do.
        if relative == "_redirects":
            continue
        url = site.rstrip("/") + site_path(relative)
        status, body, _ = fetch_no_redirect(url)
        if status != 200:
            problems.append("%s answered HTTP %d" % (url, status))
        elif body != tree[relative]:
            problems.append("%s is not what the releases say" % url)
    if "_redirects" in tree:
        latest = json.loads(tree["releases/latest"].decode("utf-8"))

        def get(path):
            status, _, headers = fetch_no_redirect(site.rstrip("/") + path)
            return status, headers
        problems.extend(check_downloads(latest, get, site.rstrip("/")))
    return problems


def check_downloads(latest, get, where, out=lambda line: None):
    """The download links, read through get(path) -> (status, headers) and not
    followed: every name the release object latest carries answers a 302 to that
    release's own file, /download/ (with its slash or without) a 302 to its page,
    and a name it does not carry is not sent anywhere. Returns the problems."""
    problems = []
    if has_suffix(latest.get("tag_name") or ""):
        problems.append("the download links would lead to %s, a pre-release" % latest.get("tag_name"))
    expected = dict((source, target) for source, target, _ in redirect_rules(latest))
    paths = [DOWNLOAD, DOWNLOAD[:-1]]
    for alias in sorted(download_aliases().values()):
        paths += [alias, alias + ".cosign.bundle"]
    for path in paths:
        want = expected.get(redirect_path(path)) if path == DOWNLOAD[:-1] else expected.get(path)
        status, headers = get(path)
        location = headers.get("location", "")
        if want is not None:
            if status != 302 or location != want:
                problems.append("%s%s answered %d%s, not a 302 to %s" % (where, path, status, " to " + location if location else "", want))
            else:
                out("  302 %s%s -> %s" % (where, path, want))
        elif status in (301, 302, 303, 307, 308):
            problems.append("%s%s answered %d to %s, though %s does not carry that file" % (where, path, status, location, latest.get("tag_name")))
    return problems


# ---------------------------------------------------------------- the server's side of it

def server_constants(source=None):
    """The compiled feed addresses, read from UpdateChecker.cpp itself."""
    if source is None:
        with open(CHECKER_SOURCE, encoding="utf-8") as f:
            source = f.read()
    constants = {}
    for name in ("STABLE_FEED", "PRERELEASE_FEED", "RELEASES_BASE"):
        match = re.search(r"const char \*" + name + r' = "([^"]+)";', source)
        if not match:
            raise FeedError("UpdateChecker.cpp has no %s" % name)
        constants[name] = match.group(1)
    return constants


def feed_url(constants, configured, prerelease_channel):
    """UpdateChecker::FeedUrl."""
    if configured:
        return configured
    return constants["PRERELEASE_FEED"] if prerelease_channel else constants["STABLE_FEED"]


def by_tag_url(constants, configured, version):
    """UpdateChecker::ReleaseByTagUrl, the same in every release from 6.2.28."""
    tag = "/tags/v" + version
    if not configured:
        return constants["RELEASES_BASE"] + tag
    at = configured.find("/releases")
    if at < 0:
        return configured + tag
    return configured[:at + 9] + tag


def installer_names(tag):
    """UpdateChecker.cpp's PlatformInstallerName_, for each kind of installation."""
    return {
        "windows": "hMailServer-%s-x64.exe" % tag,
        "windows-arm64": "hMailServer-%s-arm64.exe" % tag,
        "deb-amd64": "hmailserver_%s_amd64.deb" % tag,
        "deb-arm64": "hmailserver_%s_arm64.deb" % tag,
        "rpm-x86_64": "hmailserver-%s-1.x86_64.rpm" % tag,
        "rpm-aarch64": "hmailserver-%s-1.aarch64.rpm" % tag,
        "appimage-x86_64": "hMailServer-%s-x86_64.AppImage" % tag,
        "appimage-aarch64": "hMailServer-%s-aarch64.AppImage" % tag,
    }


def text_member(value, key):
    member = value.get(key)
    return member if isinstance(member, str) else ""


def read_release(value, platform):
    """UpdateChecker::ReadRelease_, field for field: None where it returns false."""
    if not isinstance(value, dict):
        return None
    tag = text_member(value, "tag_name")
    if not tag:
        return None
    if tag[0] in ("v", "V"):
        tag = tag[1:]
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")
    if len(tag) > 64 or any(c not in allowed for c in tag) or tag[:1] in (".", "-"):
        return None
    release = {
        "version": tag,
        "name": text_member(value, "name"),
        "url": text_member(value, "html_url"),
        "published_at": text_member(value, "published_at"),
        "draft": value.get("draft") is True,
        "prerelease": value.get("prerelease") is True,
        "installer_name": "", "installer_url": "", "installer_size": 0, "installer_digest": "", "bundle_url": "",
    }
    installer = installer_names(tag)[platform]
    assets = value.get("assets")
    if isinstance(assets, list):
        for asset in assets:
            if not isinstance(asset, dict):
                continue
            name = text_member(asset, "name")
            if name == installer:
                size = asset.get("size")
                release["installer_name"] = name
                release["installer_url"] = text_member(asset, "browser_download_url")
                release["installer_size"] = int(size) if isinstance(size, (int, float)) and not isinstance(size, bool) else 0
                release["installer_digest"] = text_member(asset, "digest")
            elif name == installer + ".cosign.bundle":
                release["bundle_url"] = text_member(asset, "browser_download_url")
    return release


def select_release(document, prerelease_channel, platform):
    """UpdateChecker::SelectRelease_: (release or None, error or None)."""
    if isinstance(document, dict):
        candidates = [document]
    elif isinstance(document, list):
        candidates = document
    else:
        return None, "The update feed is neither a release nor a list of releases."
    any_release = False
    best = None
    for candidate in candidates:
        release = read_release(candidate, platform)
        if release is None:
            continue
        any_release = True
        if release["draft"] or ((release["prerelease"] or has_suffix(release["version"])) and not prerelease_channel):
            continue
        if best is None or compare_versions(release["version"], best["version"]) > 0:
            best = release
    if not any_release:
        return None, "The update feed has no release in it."
    return best, None


# ---------------------------------------------------------------- GitLab Pages, locally

def pages_resolve(root, url_path, follow_rules=True):
    """gitlab-pages' reader (internal/serving/disk/reader.go, tryFile) for one
    path: (status, file or None, location or None). The query is not its business."""
    path = urllib.parse.unquote(url_path.split("?", 1)[0])
    if ".." in path.split("/"):
        return 404, None, None
    full = os.path.join(root, *[p for p in path.split("/") if p])
    if os.path.isdir(full):
        if not path.endswith("/"):
            query = url_path[len(url_path.split("?", 1)[0]):]
            return 302, None, path + "/" + query
        index = os.path.join(full, "index.html")
        return (200, index, None) if os.path.isfile(index) else (404, None, None)
    if os.path.isfile(full):
        return 200, full, None
    if not path.endswith("/") and os.path.isfile(full + ".html"):
        return 200, full + ".html", None
    # No file: the first rule of _redirects whose from is the path - a file comes
    # first, as in gitlab-pages, and a rule it would refuse is skipped.
    redirects = os.path.join(root, "_redirects")
    if follow_rules and os.path.isfile(redirects):
        with open(redirects, "rb") as f:
            rules, _ = parse_redirects(f.read())
        for source, target, status in rules:
            if redirect_path(source) == redirect_path(path):
                if status == 200:
                    return pages_resolve(root, target, follow_rules=False)
                return status, None, target
    return 404, None, None


class PagesServer(object):
    """A loopback HTTP server that answers as GitLab Pages would from root."""

    def __init__(self, root):
        tree_root = root

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                status, path, location = pages_resolve(tree_root, self.path)
                self.send_response(status)
                if location:
                    self.send_header("Location", location)
                body = b""
                if path:
                    with open(path, "rb") as f:
                        body = f.read()
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()


def serve_check_atom(base, out):
    """/releases.atom as Pages serves it from base, held by build/check-atom-feed.py
    to RFC 4287 and to the /releases.json and /releases/latest served beside it.
    Returns the problems."""
    url = SITE + "/" + ATOM_PATH
    status, body, _ = fetch_no_redirect(base + "/" + ATOM_PATH)
    if status != 200:
        out("  %-3d %s" % (status, url))
        return ["%s answered HTTP %d" % (url, status)]
    documents = {}
    for path in ("/releases.json", "/releases/latest"):
        answer, content, _ = fetch_no_redirect(base + path)
        try:
            documents[path] = json.loads(content.decode("utf-8")) if answer == 200 else None
        except ValueError:
            documents[path] = None
    checker = atom_checker()
    found = checker.check_atom(body, documents["/releases.json"], documents["/releases/latest"])
    if documents["/releases.json"] is None:
        found.append("%s/releases.json could not be read, so the Atom feed was not held to it" % SITE)
    entries = checker.entry_ids(body)
    out("  200 %s -> %d entries, the newest %s" % (url, len(entries), entries[0] if entries else "none"))
    return ["%s: %s" % (url, problem) for problem in found]


def serve_check(directory, constants, out=print):
    """Every URL a server derives, read from directory served as Pages serves it,
    and each answer read as the server reads it. Returns the problems found."""
    problems = []
    with PagesServer(directory) as pages:
        base = "http://127.0.0.1:%d" % pages.port

        def local(url):
            return url.replace(SITE, base, 1) if url.startswith(SITE) else url

        def read(url, prerelease_channel, platform="windows", by_tag=False):
            status, body, _ = fetch_no_redirect(local(url))
            if status != 200:
                problems.append("%s answered HTTP %d" % (url, status))
                out("  %-3d %s" % (status, url))
                return None
            try:
                document = json.loads(body.decode("utf-8"))
            except ValueError as e:
                problems.append("%s is not JSON: %s" % (url, e))
                return None
            if by_tag:
                release = read_release(document, platform)
                error = None if release else "not a release"
            else:
                release, error = select_release(document, prerelease_channel, platform)
            if error:
                problems.append("%s: %s" % (url, error))
                return None
            if release is None:
                # Every server on that channel would call itself up to date.
                problems.append("%s gives the %s channel no release it takes" % (url, "pre-release" if prerelease_channel else "stable"))
                out("  200 %s -> no release the channel takes" % url)
                return None
            out("  200 %s -> %s %s, installer %s (%d bytes, %s), bundle %s" % (
                url, "pre-release" if release["prerelease"] else "release", release["version"],
                release["installer_name"] or "none", release["installer_size"],
                release["installer_digest"][:15] + "..." if release["installer_digest"] else "no digest",
                "yes" if release["bundle_url"] else "none"))
            return release

        configured = SITE + "/releases.json"
        out("the compiled defaults:")
        stable = read(feed_url(constants, "", False), False)
        preview = read(feed_url(constants, "", True), True)
        if stable and stable["installer_url"] and not stable["bundle_url"] and stable["version"] in UNSIGNED_BY_DESIGN:
            out("NOTE %s, the stable channel's release, carries no Sigstore bundle by design (%s): servers are told so and install it by hand" % (stable["version"], UNSIGNED_BY_DESIGN[stable["version"]]))
        elif stable and not (stable["installer_url"] and stable["bundle_url"]):
            problems.append("%s, the stable channel's release, carries no Windows installer with its Sigstore bundle, so no Windows server could install it" % stable["version"])
        # Every other kind of server the stable release carries a file for - the Debian
        # and RPM packages on x86-64 and, from 6.3.6, on AArch64 - finds it with its
        # bundle, or is offered a file it cannot verify and installs nothing.
        if stable:
            status, body, _ = fetch_no_redirect(local(feed_url(constants, "", False)))
            try:
                document = json.loads(body.decode("utf-8")) if status == 200 else None
            except ValueError:
                document = None
            carried = ["windows"] if stable["installer_url"] else []
            for platform in sorted(installer_names(stable["version"])):
                if platform == "windows" or document is None:
                    continue
                offered, _ = select_release(document, False, platform)
                if not offered or not offered["installer_url"]:
                    continue
                carried.append(platform)
                if not offered["bundle_url"] and offered["version"] not in UNSIGNED_BY_DESIGN:
                    problems.append("%s, the stable channel's release, carries %s without its Sigstore bundle, so no %s server "
                                    "could install it" % (offered["version"], offered["installer_name"], platform))
            out("  %s carries a file for: %s" % (stable["version"], ", ".join(carried) or "no kind of server"))
        out("UpdateFeedUrl=%s, the value docs/UpdateFeed.md gives:" % configured)
        stable_configured = read(feed_url(constants, configured, False), False)
        preview_configured = read(feed_url(constants, configured, True), True)
        if stable and stable_configured and stable["version"] != stable_configured["version"]:
            problems.append("the stable channel finds %s by default and %s through UpdateFeedUrl" % (stable["version"], stable_configured["version"]))
        if preview and preview_configured and preview["version"] != preview_configured["version"]:
            problems.append("the pre-release channel finds %s by default and %s through UpdateFeedUrl" % (preview["version"], preview_configured["version"]))

        out("the rollback's lookup, for every release, by default and through UpdateFeedUrl:")
        tags = sorted(n[len("releases/tags/"):] for n in read_tree(directory) if n.startswith("releases/tags/"))
        looked_up = 0
        for tag in tags:
            version = tag[1:]
            for configured_value in ("", configured):
                url = by_tag_url(constants, configured_value, version)
                status, body, _ = fetch_no_redirect(local(url))
                release = read_release(json.loads(body.decode("utf-8")), "windows") if status == 200 else None
                if release is None or release["version"] != version:
                    problems.append("%s does not answer release %s" % (url, version))
                else:
                    looked_up += 1
        out("  %d of %d lookups answered with the release asked for" % (looked_up, 2 * len(tags)))
        if stable:
            for configured_value in ("", configured):
                read(by_tag_url(constants, configured_value, stable["version"]), False, by_tag=True)

        out("the Atom feed of the releases, for a feed reader (build/check-atom-feed.py):")
        problems.extend(serve_check_atom(base, out))

        out("the download links, which follow releases/latest (_redirects):")
        redirects = os.path.join(directory, "_redirects")
        if not os.path.isfile(redirects):
            problems.append("there is no _redirects, so every /download/ link answers 404")
        else:
            with open(redirects, "rb") as f:
                problems.extend(parse_redirects(f.read())[1])
        try:
            with open(os.path.join(directory, "releases", "latest"), "rb") as f:
                latest = json.loads(f.read().decode("utf-8"))
            redirect_rules(latest)
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
            problems.append("releases/latest could not be read for the download links: %r" % e)
            latest = None
        if latest is not None:
            def get(path):
                status, _, headers = fetch_no_redirect(base + path)
                return status, headers
            problems.extend(check_downloads(latest, get, SITE, out))

        out("the addresses a server must not be pointed at:")
        for path in ("/releases", "/releases?per_page=10"):
            status, _, headers = fetch_no_redirect(base + path)
            out("  %-3d %s%s" % (status, SITE + path, " -> " + headers.get("location", "") if status in (301, 302) else ""))
    return problems


# ---------------------------------------------------------------- the self-test

def self_test():
    import tempfile

    registry = API + "/packages/generic/hmailserver/"

    def link(version, name):
        return {"name": name, "url": registry + version + "/" + urllib.parse.quote(name), "link_type": "other"}

    def release(tag, name, released, links, upcoming=False):
        return {"tag_name": tag, "name": name, "released_at": released, "upcoming_release": upcoming,
                "_links": {"self": PROJECT_URL + "/-/releases/" + tag},
                "assets": {"count": len(links), "sources": [], "links": links}, "description": "notes"}

    def files_for(version):
        return [link(version, n) for n in (
            "hMailServer-%s-x64.exe" % version, "hMailServer-%s-x64.exe.cosign.bundle" % version,
            "hmailserver_%s_amd64.deb" % version, "hmailserver_%s_amd64.deb.cosign.bundle" % version)]

    markup = "hMailServer 6.3.3 <script>alert(1)</script> & co"
    releases = [
        release("v6.3.5-rc1", "Pre-release: hMailServer 6.3.5 RC 1", "2026-10-02T09:00:00.000Z", files_for("6.3.5-rc1")),
        release("v6.3.4", "hMailServer 6.3.4", "2026-10-01T12:00:00.000+01:00", files_for("6.3.4") + [
            link("6.3.4", "notes with space+plus.txt"), link("6.3.4", "hmailserver-6.3.4-1.x86_64.rpm")]),
        release("v6.2.29", "hMailServer 6.2.29", "2026-10-01T12:30:00.000Z", files_for("6.2.29")),
        release("build-inputs-1", "build inputs", "2026-09-11T04:26:16.000Z", []),
        release("v6.3.3", markup, "2026-09-15T01:02:52.000Z", files_for("6.3.3")),
        release("v6.9.0", "hMailServer 6.9.0", "2027-01-01T00:00:00.000Z", files_for("6.9.0"), upcoming=True),
        release("v6.2.22-pre1", "Pre-release: hMailServer 6.2.22 pre-release 1", "2026-08-16T22:33:58.000Z", []),
        release("v6.0.0-B4", "Pre-release: hMailServer 6.0.0-B4", "2026-06-12T20:44:23.000Z", []),
    ]
    package_ids = {}
    package_files = {}
    next_id = [1000]
    for r in releases:
        for l in r["assets"]["links"]:
            m = REGISTRY_LINK.match(l["url"])
            version = urllib.parse.unquote(m.group(2))
            package_ids.setdefault(version, 500 + len(package_ids))
            next_id[0] += 1
            name = urllib.parse.unquote(m.group(3))
            package_files.setdefault(package_ids[version], []).append(
                {"id": next_id[0], "file_name": name, "size": len(name) * 1000, "file_sha256": ("%064x" % (len(name) * 7919))})
    # An earlier upload of the same file, which the registry no longer serves.
    package_files[package_ids["6.3.4"]].insert(0, {"id": 1, "file_name": "hMailServer-6.3.4-x64.exe", "size": 1, "file_sha256": "0" * 64})
    packages = [{"id": i, "name": "hmailserver", "version": v} for v, i in package_ids.items()]
    packages.append({"id": 999, "name": "build-inputs", "version": "1"})

    requests = []

    def fake_get(url):
        requests.append(url)
        parsed = urllib.parse.urlparse(url)
        query = urllib.parse.parse_qs(parsed.query)
        page = int(query.get("page", ["1"])[0])
        path = parsed.path[len("/api/v4/projects/" + PROJECT_ID):]
        if path == "/releases":
            items, size = releases, 3          # three to a page: the pagination is exercised
        elif path == "/packages":
            items, size = packages, 100
        else:
            m = re.match(r"^/packages/(\d+)/package_files$", path)
            assert m, url
            items, size = package_files.get(int(m.group(1)), []), 100
        chunk = items[(page - 1) * size:page * size]
        more = page * size < len(items)
        return json.dumps(chunk).encode("utf-8"), {"x-next-page": str(page + 1) if more else ""}

    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    loaded, files = load(fake_get)
    expect(len(loaded) == len(releases), "every page of releases is read")
    expect(sum(1 for u in requests if "/releases?" in u) == 3, "the releases are read three to a page")
    tree, latest, warnings = render(loaded, files)
    expect(warnings == [], "nothing is left out of a feed whose releases are all publishable: %s" % warnings)

    tags = sorted(n for n in tree if n.startswith("releases/tags/"))
    expect(tags == sorted("releases/tags/" + t for t in ("v6.3.5-rc1", "v6.3.4", "v6.2.29", "v6.3.3", "v6.2.22-pre1", "v6.0.0-B4")),
           "a tag file for every server release, none for build-inputs-1 or the upcoming 6.9.0: %s" % tags)
    expect(set(tree) == set(tags) | {"releases/latest", "releases.json", "releases/index.html", ATOM_PATH, "index.html", "_redirects"},
           "the tree is the documented files")
    expect(latest["tag_name"] == "v6.3.4", "releases/latest is the highest stable version, not the latest date (6.2.29 was published after 6.3.4): %s"
           % latest["tag_name"])

    # The download links follow releases/latest: not 6.3.5-rc1, the newest by
    # date, nor 6.2.29, published after 6.3.4, nor build-inputs-1, nor the
    # upcoming 6.9.0 - the releases permalink/latest would follow. A name 6.3.4
    # does not carry (the AArch64 packages, the AppImages, the ARM installer) has
    # no rule, and 6.3.4's other files (the notes) none either.
    rules, errors = parse_redirects(tree["_redirects"])
    expect(errors == [], "the _redirects written is one Pages takes: %s" % errors)
    in_634 = registry + "6.3.4/"
    expect(rules == [
        ("/download/", PROJECT_URL + "/-/releases/v6.3.4", 302),
        ("/download/hmailserver_amd64.deb", in_634 + "hmailserver_6.3.4_amd64.deb", 302),
        ("/download/hmailserver_amd64.deb.cosign.bundle", in_634 + "hmailserver_6.3.4_amd64.deb.cosign.bundle", 302),
        ("/download/hmailserver.x86_64.rpm", in_634 + "hmailserver-6.3.4-1.x86_64.rpm", 302),
        ("/download/hMailServer-x64.exe", in_634 + "hMailServer-6.3.4-x64.exe", 302),
        ("/download/hMailServer-x64.exe.cosign.bundle", in_634 + "hMailServer-6.3.4-x64.exe.cosign.bundle", 302),
    ], "a 302 for each file of 6.3.4 a download name stands for, and /download/ to its page: %s" % rules)
    expect(b'href="/download/hMailServer-x64.exe"' in tree["index.html"] and b".cosign.bundle</code>" not in tree["index.html"],
           "the page lists the download links, without the bundles")
    # From 6.3.6 a release carries the AArch64 .deb and .rpm too, each with its bundle:
    # their download names lead to that release's files as the x86-64 ones do, and an
    # AArch64 server installed from either package finds its file and bundle.
    with_arm = json.loads(json.dumps(loaded))
    arm_names = ["hmailserver_6.3.4_arm64.deb", "hmailserver_6.3.4_arm64.deb.cosign.bundle",
                 "hmailserver-6.3.4-1.aarch64.rpm", "hmailserver-6.3.4-1.aarch64.rpm.cosign.bundle"]
    [r for r in with_arm if r["tag_name"] == "v6.3.4"][0]["assets"]["links"] += [link("6.3.4", n) for n in arm_names]
    arm_files = dict((key, dict(record)) for key, record in files.items())
    for number, name in enumerate(arm_names):
        arm_files[("hmailserver", "6.3.4")][name] = {"id": 9000 + number, "file_name": name, "size": 4000000, "file_sha256": "%064x" % (number + 1)}
    arm_tree, arm_latest, arm_warnings = render(with_arm, arm_files)
    arm_rules = parse_redirects(arm_tree["_redirects"])[0]
    expect(not arm_warnings and all((DOWNLOAD + alias_name(n, "6.3.4"), registry + "6.3.4/" + n, 302) in arm_rules for n in arm_names),
           "the AArch64 packages' download names, and their bundles', lead to the release's files: %s %s" % (arm_warnings, arm_rules))
    for platform in ("deb-arm64", "rpm-aarch64"):
        offered = read_release(json.loads(arm_tree["releases/latest"].decode("utf-8")), platform)
        expect(offered and offered["installer_name"] == installer_names("6.3.4")[platform] and offered["bundle_url"]
               and offered["installer_digest"] and offered["installer_size"] == 4000000,
               "an AArch64 server installed from the %s finds its file, its size, its digest and its bundle: %s" % (platform, offered))
    aliases = download_aliases()
    expect(set(aliases) == set(installer_names("6.3.4")), "a download name for every kind of installation")
    expect([aliases[p] for p in ("windows", "windows-arm64", "deb-amd64", "deb-arm64", "rpm-x86_64", "rpm-aarch64", "appimage-x86_64", "appimage-aarch64")] == [
        "/download/hMailServer-x64.exe", "/download/hMailServer-arm64.exe", "/download/hmailserver_amd64.deb", "/download/hmailserver_arm64.deb",
        "/download/hmailserver.x86_64.rpm", "/download/hmailserver.aarch64.rpm", "/download/hMailServer-x86_64.AppImage",
        "/download/hMailServer-aarch64.AppImage"], "the download names: %s" % aliases)
    # The names a release's own permanent links have (release-chain.py), so that a
    # link that read .../permalink/latest/downloads/<name> reads /download/<name>.
    import importlib.util
    spec = importlib.util.spec_from_file_location("release_chain", os.path.join(ROOT, "build", "ci", "release-chain.py"))
    chain = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(chain)
    for platform, name in sorted(installer_names("6.3.5").items()):
        expect(DOWNLOAD + chain.permalink_name(name, "6.3.5") == aliases[platform],
               "the download name of %s is its release's permanent name: %s" % (name, chain.permalink_name(name, "6.3.5")))

    # The rules Pages refuses, refused here too.
    for text, why in ((b"/a https://example.com/x 200\n", "an absolute rewrite"), (b"/a /b 307\n", "a 307"),
                      (b"a /b 302\n", "a from that is not a path"), (b"/a example.com/b 302\n", "a bare domain"),
                      (b"/a //example.com/b 302\n", "a protocol-relative target"), (b"/a/* /b/:splat 302\n", "a splat"),
                      (b"/a /b 302!\n", "a force"), (b"/a /b 302 x=1 y\n", "a parameter"),
                      (b"/a /b 302\n" * 7000, "a file over 64 KiB"), (b"".join(b"/a%d /b 302\n" % i for i in range(1001)), "a thousand and one rules")):
        expect(parse_redirects(text)[1] != [], "_redirects with %s is refused" % why)
    expect(parse_redirects(b"# a comment\n\n/a /b\n") == ([("/a", "/b", 301)], []), "a rule with no status is a 301, and a comment is skipped")
    expect(tree["releases/index.html"].startswith(b"<!DOCTYPE html>") and b"releases.json" in tree["releases/index.html"],
           "releases/ is a page that shows the list")

    # Markup in a release's name is text everywhere on the site: nothing to run on the domain.
    for relative, content in sorted(tree.items()):
        expect(b"<script" not in content and b"</script" not in content, "%s carries no markup from a release's name" % relative)
    listing = json.loads(tree["releases.json"].decode("utf-8"))
    expect([r["name"] for r in listing if r["tag_name"] == "v6.3.3"] == [markup], "the name reads back as it was written")

    expect([r["tag_name"] for r in listing] == ["v6.3.5-rc1", "v6.2.29", "v6.3.4", "v6.3.3", "v6.2.22-pre1", "v6.0.0-B4"],
           "the list is newest first, by the UTC time: %s" % [r["tag_name"] for r in listing])
    pre_releases = {"v6.3.5-rc1", "v6.2.22-pre1", "v6.0.0-B4"}
    for r in listing:
        expect(list(r) == RELEASE_KEYS, "%s has exactly the keys the server reads: %s" % (r["tag_name"], list(r)))
        expect(r["prerelease"] == (r["tag_name"] in pre_releases),
               "%s is a pre-release exactly when its tag has a suffix" % r["tag_name"])
        expect(r["draft"] is False, "nothing published is a draft")
        for a in r["assets"]:
            expect(list(a) == ASSET_KEYS, "an asset of %s has exactly the keys the server reads: %s" % (r["tag_name"], list(a)))
            expect(re.match(r"^sha256:[0-9a-f]{64}$", a["digest"]) is not None, "a digest is sha256:<hex>")
    v634 = [r for r in listing if r["tag_name"] == "v6.3.4"][0]
    expect(v634["published_at"] == "2026-10-01T11:00:00Z", "released_at is written in UTC as GitHub wrote it: %s" % v634["published_at"])

    # The Atom feed: RFC 4287 and releases.json's releases, in its order, as
    # build/check-atom-feed.py holds it - and that checker refuses each way of
    # breaking one (its own self-test, which the pages job runs this way).
    checker = atom_checker()
    problems = checker.check_atom(tree[ATOM_PATH], listing, latest)
    expect(problems == [], "releases.atom is a valid Atom feed of the releases releases.json lists: %s" % problems)
    expect(checker.entry_ids(tree[ATOM_PATH]) == [atom_entry_id(r["tag_name"]) for r in listing],
           "releases.atom has an entry for each release, newest first, its id made from the tag")
    expect(b"<summary>notes</summary>" in tree[ATOM_PATH] and b"&lt;script&gt;" in tree[ATOM_PATH],
           "each entry's summary is its notes, and a name's markup is text")
    checker_failures = checker.self_test()
    expect(checker_failures == [], "build/check-atom-feed.py refuses every broken feed of its self-test: %s" % checker_failures)
    expect(b'<a href="releases.atom">' in tree["index.html"]
           and b'<link rel="alternate" type="application/atom+xml" title="hMailServer releases" href="releases.atom">' in tree["index.html"]
           and b'<a href="../releases.atom">' in tree["releases/index.html"]
           and b'<link rel="alternate" type="application/atom+xml" title="hMailServer releases" href="../releases.atom">' in tree["releases/index.html"],
           "the site's pages link the Atom feed, for a person and for a reader's discovery")
    rerendered = render(json.loads(json.dumps(loaded)), files)[0]
    expect(rerendered[ATOM_PATH] == tree[ATOM_PATH], "the same releases render the same releases.atom, whenever")
    moved = json.loads(json.dumps(loaded))
    for r in moved:
        r["_links"]["self"] = r["_links"]["self"].replace("/hmailserver/hmailserver/", "/elsewhere/hmailserver/")
        r["name"] = r["name"] + " (edited)"
        r["description"] = "Edited notes."
    moved_atom = render(moved, files)[0][ATOM_PATH]
    expect(checker.entry_ids(moved_atom) == checker.entry_ids(tree[ATOM_PATH]) and b"/elsewhere/hmailserver/-/releases/v6.3.4" in moved_atom,
           "a move of the project and an edited name and notes change the links and no id")
    for description, summary in (
            (None, "fallback"), ("", "fallback"), ("# Heading\n\n---\n", "fallback"),
            ("*Re-published on GitLab on 24 September 2026 from the GitHub release.*\n\n# hMailServer 6.3.3\n\nThe lead.\n\nMore.", "The lead."),
            # The Download table the release chain puts first (build/ci/release-chain.py, download_table).
            ("## Download\n\n| | |\n|---|---|\n| **Windows, x64** | [hMailServer-6.3.7-x64.exe](https://gitlab.com/hmailserver/"
             "hmailserver/-/releases/v6.3.7/downloads/hMailServer-x64.exe): the installer. SHA-256 `2f673913`. |\n"
             "| **Linux** | The `.deb` and `.rpm` packages; `hmailserver-linux-6.3.7-SHA256SUMS.txt` lists them. |\n\n"
             "# hMailServer 6.3.7\n\nThe lead.\n\nMore.", "The lead."),
            ("Line one\\\nline two, with **bold**, *emphasis*, `a_b*c`, [a link](https://x/y) and ![an image](i.png).",
             "Line one line two, with bold, emphasis, a_b*c, a link and an image."),
            ("```\ncode first\n```\n\n| a | b |\n|---|---|\n\n- item one\n- item two", "item one item two"),
            ("hmailserver_6.3.4_amd64.deb and snake_case stay, \\- an escape goes.", "hmailserver_6.3.4_amd64.deb and snake_case stay, - an escape goes."),
            ("#123 is an issue, not a heading.", "#123 is an issue, not a heading."),
            ("**Mail could hang after `354`, and this is the fix.** hm__x__y stays.", "Mail could hang after 354, and this is the fix. hm__x__y stays."),
            ("> [!WARNING]\n> **Fresh installations fail** - use 6.2.20.", "Warning: Fresh installations fail - use 6.2.20."),
            ("See [the docs](https://example.com/a_(b)) and [this](<https://example.com/a b> \"title\") now.", "See the docs and this now."),
            ("Raw <b>HTML</b> goes,<br> <!-- a comment --> x < y > z stays, `<code>` stays.", "Raw HTML goes, x < y > z stays, <code> stays."),
            ("AT&T &amp; R&amp;D&nbsp;x &#x41;&#66; &notanentity; `&amp;` &#xE000;0&#xE001;.", "AT&T & R&D x AB &notanentity; &amp; 0.")):
        got = release_summary(description, "fallback")
        expect(got == summary, "the summary of %r is %r: %r" % (description, summary, got))
    # A name XML cannot carry whole - a control character, a carriage return - is
    # left as a parser reads it, and the feed is still held to releases.json.
    odd = json.loads(json.dumps(loaded))
    of_tag = [r for r in odd if r["tag_name"] == "v6.3.4"][0]
    of_tag["name"] = "hMailServer\x0b 6.3.4\r\nsecond line"
    odd_tree, odd_latest, _ = render(odd, files)
    problems = checker.check_atom(odd_tree[ATOM_PATH], json.loads(odd_tree["releases.json"].decode("utf-8")), odd_latest)
    expect(problems == [], "a release name with a control character and a line break still makes a feed held to releases.json: %s" % problems)
    # A name of blanks, or of what XML cannot carry, is published in releases.json as
    # it is; the entry's title is the tag, so the feed still passes and the pages job
    # still publishes the release to the servers.
    for blank in ("   ", "\x01\x02", "\r\n"):
        of_tag["name"] = blank
        odd_tree, odd_latest, _ = render(odd, files)
        problems = checker.check_atom(odd_tree[ATOM_PATH], json.loads(odd_tree["releases.json"].decode("utf-8")), odd_latest)
        expect(problems == [] and b"<title>v6.3.4</title>" in odd_tree[ATOM_PATH],
               "a release named %r has its tag as the entry's title, and the feed passes: %s" % (blank, problems))
    # Notes that would keep the markup's patterns busy are read no further than a summary needs.
    started = time.time()
    busy = release_summary("**a " * 200000, "fallback")
    expect(time.time() - started < 5 and len(busy) <= SUMMARY_MAX, "a paragraph of 200,000 unmatched ** is summarised at once: %.1f s" % (time.time() - started))
    for busy_text in ("<a b" * 100000, "[a](" * 100000, "[a](b(c" * 100000, "&#" * 100000):
        started = time.time()
        busy = release_summary(busy_text, "fallback")
        expect(time.time() - started < 5 and len(busy) <= SUMMARY_MAX, "a paragraph of %r... is summarised at once: %.1f s" % (busy_text[:8], time.time() - started))
    long_text = ("A sentence of the lead. " * 60).strip()
    cut = release_summary(long_text, "fallback")
    expect(len(cut) <= SUMMARY_MAX and cut.endswith("lead. ...") and long_text.startswith(cut[:-4]),
           "a long lead is cut at a sentence, within %d characters: %d, %r" % (SUMMARY_MAX, len(cut), cut[-30:]))
    expect(xml_text("a\x00b\x1bc\ud800d&<>\"") == "abcd&amp;&lt;&gt;&quot;", "what XML cannot carry is left out, and the rest escaped")
    exe = [a for a in v634["assets"] if a["name"] == "hMailServer-6.3.4-x64.exe"][0]
    expect(exe["size"] == len("hMailServer-6.3.4-x64.exe") * 1000, "a file uploaded twice is described as its later upload")
    spaced = [a for a in v634["assets"] if a["name"] == "notes with space+plus.txt"]
    expect(len(spaced) == 1 and spaced[0]["browser_download_url"].endswith("/notes%20with%20space%2Bplus.txt"),
           "a file name that needs escaping is found in the registry and linked escaped")

    # What the server makes of it: the parser, both channels, every platform.
    stable, error = select_release(json.loads(tree["releases/latest"].decode("utf-8")), False, "windows")
    expect(error is None and stable["version"] == "6.3.4" and stable["installer_url"] and stable["bundle_url"],
           "the stable channel reads 6.3.4 with its installer and bundle from releases/latest")
    expect(stable["installer_digest"].startswith("sha256:") and stable["installer_size"] > 0, "with the size and digest the download is held to")
    listed_stable, _ = select_release(listing, False, "windows")
    expect(listed_stable["version"] == "6.3.4", "the stable channel reading the list passes over 6.3.5-rc1 and takes 6.3.4")
    preview, _ = select_release(listing, True, "deb-amd64")
    expect(preview["version"] == "6.3.5-rc1" and preview["installer_name"] == "hmailserver_6.3.5-rc1_amd64.deb" and preview["bundle_url"],
           "the pre-release channel takes 6.3.5-rc1 and a Debian server finds its package and bundle")

    # The copy of the server's parser passes over a suffixed version on the stable
    # channel even when the feed flags it stable, as the server does.
    flagged = json.loads(json.dumps(listing))
    for r in flagged:
        r["prerelease"] = False
    flagged_stable, _ = select_release(flagged, False, "windows")
    expect(flagged_stable["version"] == "6.3.4", "a suffixed version flagged stable is still a pre-release: %s" % flagged_stable["version"])

    # A suffix is anything but digits and dots, not only a dash: 6.3.9rc1, flagged
    # stable and the highest version in the list, is still a pre-release.
    for version, suffixed in (("6.3.5-rc1", True), ("v6.3.5rc1", True), ("6.3.5_rc1", True), ("6.3.5.rc1", True),
                              ("v6.3.4", False), ("6.3.4.1", False)):
        expect(has_suffix(version) == suffixed, "%s %s a suffix" % (version, "has" if suffixed else "has no"))
    undashed = json.loads(json.dumps(flagged))
    renamed = json.loads(json.dumps([r for r in undashed if r["tag_name"] == "v6.3.4"][0]))
    renamed["tag_name"] = "v6.3.9rc1"
    undashed.append(renamed)
    undashed_stable, _ = select_release(undashed, False, "windows")
    expect(undashed_stable["version"] == "6.3.4", "a version suffixed without a dash, flagged stable, is still a pre-release: %s"
           % undashed_stable["version"])

    def of(release_list, tag):
        return [r for r in release_list if r["tag_name"] == tag][0]

    # An unpublishable link in the newest stable release stops the render: the
    # site keeps what it had rather than tell stable servers something false.
    broken = json.loads(json.dumps(loaded))
    newest = of(broken, "v6.3.4")["assets"]["links"]
    newest.append({"name": "elsewhere.exe", "url": "https://example.com/elsewhere.exe"})
    newest.append(link("6.3.4", "never-uploaded.txt"))
    # Named for one file, pointing at another: a server would install the file and
    # find the "newer" release again next time.
    renamed = link("6.3.4", "hMailServer-6.3.4-x64.exe")
    renamed["name"] = "hMailServer-6.3.5-x64.exe"
    newest.append(renamed)
    # Another release's file, under its own name.
    newest.append(link("6.3.3", "hMailServer-6.3.3-x64.exe"))
    try:
        render(broken, files)
        failures.append("an unpublishable link in the newest stable release stops the render")
    except FeedError as e:
        text = str(e)
        expect("v6.3.4" in text and "example.com" in text and "never-uploaded.txt" in text, "the render names every link it cannot publish: %s" % text)
        expect("hMailServer-6.3.5-x64.exe" in text and "which has another name" in text, "a link named for another file is refused: %s" % text)
        expect("of the package hmailserver 6.3.3, not of hmailserver 6.3.4" in text, "another release's file is refused: %s" % text)

    # A release named as a pre-release whose tag has no suffix: a server could not
    # tell it from the final release. The highest version of all, so it would be
    # the newest stable release, and it stops the render.
    broken = json.loads(json.dumps(loaded))
    named = json.loads(json.dumps(of(broken, "v6.3.4")))
    named.update({"tag_name": "v6.3.6", "name": "Pre-release: hMailServer 6.3.6"})
    named["assets"]["links"] = files_for("6.3.4")[:0]
    broken.append(named)
    try:
        render(broken, files)
        failures.append("a pre-release by its name alone, the highest version, stops the render")
    except FeedError as e:
        expect("v6.3.6" in str(e) and "the tag has no suffix" in str(e), "the render says why: %s" % e)
    named.update({"tag_name": "v6.2.30"})
    left_out = render(broken, files)
    expect("releases/tags/v6.2.30" not in left_out[0] and any("v6.2.30 is left out" in w for w in left_out[2]),
           "an older one is left out with a warning: %s" % left_out[2])

    # In an older release it leaves that release out, with a warning, and the feed
    # is published without it.
    broken = json.loads(json.dumps(loaded))
    of(broken, "v6.3.3")["assets"]["links"].append({"name": "elsewhere.exe", "url": "https://example.com/elsewhere.exe"})
    partial, partial_latest, partial_warnings = render(broken, files)
    expect("releases/tags/v6.3.3" not in partial and partial_latest["tag_name"] == "v6.3.4", "v6.3.3 is left out and the rest is published")
    expect(len(partial_warnings) == 1 and "v6.3.3 is left out" in partial_warnings[0] and "example.com" in partial_warnings[0],
           "the warning names the release and the link: %s" % partial_warnings)

    # The server's compiled addresses are files of this tree, and GitLab Pages
    # answers each without a redirect.
    constants = server_constants()
    for name, value in sorted(constants.items()):
        expect(value.startswith(SITE + "/"), "UpdateChecker.cpp's %s is on %s: %s" % (name, SITE, value))
    with tempfile.TemporaryDirectory() as directory:
        write_tree(tree, directory)
        expect(read_tree(directory) == tree, "the tree written is the tree rendered")
        expect(pages_resolve(directory, "/releases")[0] == 302, "Pages answers the bare /releases with a redirect")
        expect(pages_resolve(directory, "/releases?per_page=10")[2] == "/releases/?per_page=10", "and keeps the query")
        expect(pages_resolve(directory, "/releases/")[1].endswith("index.html"), "and /releases/ with its index.html")
        expect(pages_resolve(directory, "/nothing")[0] == 404, "and a path that is nothing with 404")
        lines = []
        problems = serve_check(directory, constants, out=lines.append)
        expect(not problems, "every URL a server derives answers 200 with a release it reads: %s" % problems)

        # A releases/latest the stable channel would not take - here a
        # pre-release - leaves every stable server saying it is up to date.
        latest_path = os.path.join(directory, "releases", "latest")
        with open(latest_path, "wb") as f:
            f.write(tree["releases/tags/v6.3.5-rc1"])
        problems = serve_check(directory, constants, out=lambda line: None)
        expect(any("gives the stable channel no release it takes" in p for p in problems),
               "serve-check fails when the stable default yields no release: %s" % problems)

        # One without the Windows installer and its bundle leaves every Windows server where it is.
        bare = json.loads(tree["releases/latest"].decode("utf-8"))
        bare["assets"] = [a for a in bare["assets"] if not a["name"].startswith("hMailServer-6.3.4-x64.exe")]
        with open(latest_path, "wb") as f:
            f.write(feed_bytes(bare))
        problems = serve_check(directory, constants, out=lambda line: None)
        expect(any("carries no Windows installer" in p for p in problems),
               "serve-check fails when the stable release has no Windows installer: %s" % problems)

        # One with the installer and no bundle is a note when it is named in
        # UNSIGNED_BY_DESIGN, and a failure when it is not.
        unsigned = json.loads(tree["releases/latest"].decode("utf-8"))
        unsigned["assets"] = [a for a in unsigned["assets"] if a["name"] != "hMailServer-6.3.4-x64.exe.cosign.bundle"]
        with open(latest_path, "wb") as f:
            f.write(feed_bytes(unsigned))
        problems = serve_check(directory, constants, out=lambda line: None)
        expect(not any("carries no Windows installer" in p for p in problems),
               "serve-check notes a release unsigned by design: %s" % problems)
        saved = dict(UNSIGNED_BY_DESIGN)
        UNSIGNED_BY_DESIGN.clear()
        try:
            problems = serve_check(directory, constants, out=lambda line: None)
        finally:
            UNSIGNED_BY_DESIGN.update(saved)
        expect(any("carries no Windows installer" in p for p in problems),
               "serve-check fails on a release with no bundle that is not unsigned by design: %s" % problems)

        # The AArch64 packages, which a release carries from 6.3.6: one without its bundle
        # is a file every AArch64 server on that package manager is offered and cannot
        # verify, and it is a failure unless the release is unsigned by design.
        arm = json.loads(tree["releases/latest"].decode("utf-8"))
        arm["assets"] += [{"name": n, "size": 1000, "digest": "sha256:" + "a" * 64, "browser_download_url": registry + "6.3.4/" + n}
                          for n in ("hmailserver_6.3.4_arm64.deb", "hmailserver_6.3.4_arm64.deb.cosign.bundle", "hmailserver-6.3.4-1.aarch64.rpm")]
        with open(latest_path, "wb") as f:
            f.write(feed_bytes(arm))
        lines = []
        problems = serve_check(directory, constants, out=lines.append)
        expect(not any("aarch64.rpm without its Sigstore bundle" in p for p in problems)
               and any("6.3.4 carries a file for: windows, deb-amd64, deb-arm64, rpm-aarch64, rpm-x86_64" in l for l in lines),
               "a release unsigned by design is not failed for its AArch64 .rpm either, and the kinds it serves are named: %s %s"
               % (problems, [l for l in lines if "carries a file for" in l]))
        saved = dict(UNSIGNED_BY_DESIGN)
        UNSIGNED_BY_DESIGN.clear()
        try:
            problems = serve_check(directory, constants, out=lambda line: None)
        finally:
            UNSIGNED_BY_DESIGN.update(saved)
        expect(any("carries hmailserver-6.3.4-1.aarch64.rpm without its Sigstore bundle, so no rpm-aarch64 server could install it" in p
                   for p in problems) and not any("hmailserver_6.3.4_arm64.deb without" in p for p in problems),
               "serve-check fails on an AArch64 package without its bundle, and not on one with it: %s" % problems)
        with open(latest_path, "wb") as f:
            f.write(tree["releases/latest"])
        try:
            os.makedirs(os.path.join(directory, "stranger"))
            with open(os.path.join(directory, "stranger", "file.txt"), "w") as f:
                f.write("x")
            write_tree(tree, os.path.join(directory, "stranger"))
            failures.append("a directory that is not a feed is not overwritten")
        except FeedError:
            pass

        # The download links, as Pages answers them: a rule where there is no
        # file, a trailing slash that does not matter, and a 404 for a name the
        # release does not carry.
        page = PROJECT_URL + "/-/releases/v6.3.4"
        expect(pages_resolve(directory, "/download/") == (302, None, page), "/download/ goes to 6.3.4's page")
        expect(pages_resolve(directory, "/download") == (302, None, page), "and /download too")
        expect(pages_resolve(directory, "/download/hMailServer-x64.exe") == (302, None, in_634 + "hMailServer-6.3.4-x64.exe"),
               "/download/hMailServer-x64.exe goes to 6.3.4's installer")
        expect(pages_resolve(directory, "/download/hMailServer-arm64.exe")[0] == 404, "a name 6.3.4 does not carry is a 404")
        expect(any("302 %s/download/hMailServer-x64.exe -> %s" % (SITE, in_634 + "hMailServer-6.3.4-x64.exe") in line for line in lines),
               "serve-check reads the download links: %s" % [l for l in lines if "/download/" in l])
        with PagesServer(directory) as pages:
            expect(check_site(tree, "http://127.0.0.1:%d" % pages.port) == [],
                   "--check --site finds the site that was written as the releases make it, the download links by what they do")

        redirects_path = os.path.join(directory, "_redirects")

        def serve_check_with(content):
            with open(redirects_path, "wb") as f:
                f.write(content)
            try:
                return serve_check(directory, constants, out=lambda line: None)
            finally:
                with open(redirects_path, "wb") as f:
                    f.write(tree["_redirects"])

        # The installer's link sent to the pre-release.
        moved = tree["_redirects"].replace(b"6.3.4/hMailServer-6.3.4-x64.exe 302\n", b"6.3.5-rc1/hMailServer-6.3.5-rc1-x64.exe 302\n")
        expect(moved != tree["_redirects"], "the test changes the installer's rule")
        problems = serve_check_with(moved)
        expect(any("/download/hMailServer-x64.exe answered 302" in p and "not a 302 to" in p for p in problems),
               "serve-check fails when a download link leads anywhere but the stable release's file: %s" % problems)
        # A rule for a name the release does not carry.
        problems = serve_check_with(tree["_redirects"] + b"/download/hMailServer-arm64.exe " + in_634.encode() + b"hMailServer-6.3.4-x64.exe 302\n")
        expect(any("does not carry that file" in p for p in problems), "serve-check fails on a link to a file the release does not carry: %s" % problems)
        # A rule Pages refuses.
        problems = serve_check_with(tree["_redirects"] + b"/elsewhere https://example.com/ 200\n")
        expect(any("rewrites (200) only" in p for p in problems), "serve-check fails on a rule Pages refuses: %s" % problems)
        # No _redirects at all.
        os.remove(redirects_path)
        problems = serve_check(directory, constants, out=lambda line: None)
        with open(redirects_path, "wb") as f:
            f.write(tree["_redirects"])
        expect(any("there is no _redirects" in p for p in problems) and any("/download/hMailServer-x64.exe answered 404" in p for p in problems),
               "serve-check fails without _redirects: %s" % problems)
        # A file where a download name is: Pages serves it, and the rule is never read.
        os.makedirs(os.path.join(directory, "download"))
        with open(os.path.join(directory, "download", "hMailServer-x64.exe"), "wb") as f:
            f.write(b"MZ")
        problems = serve_check(directory, constants, out=lambda line: None)
        shutil.rmtree(os.path.join(directory, "download"))
        expect(any("/download/hMailServer-x64.exe answered 200" in p for p in problems),
               "serve-check fails when a file stands where a download link is: %s" % problems)
        # A releases/latest that is a pre-release takes the links with it: failed.
        with open(latest_path, "wb") as f:
            f.write(tree["releases/tags/v6.3.5-rc1"])
        problems = serve_check(directory, constants, out=lambda line: None)
        with open(latest_path, "wb") as f:
            f.write(tree["releases/latest"])
        expect(any("the download links would lead to v6.3.5-rc1, a pre-release" in p for p in problems),
               "serve-check fails when the download links would follow a pre-release: %s" % problems)

        # The Atom feed, read from the site as a reader reads it and held to the
        # releases.json served beside it: missing, and without its newest entry.
        atom_path = os.path.join(directory, ATOM_PATH)
        os.remove(atom_path)
        problems = serve_check(directory, constants, out=lambda line: None)
        expect(problems == ["%s/%s answered HTTP 404" % (SITE, ATOM_PATH)], "serve-check fails without releases.atom: %s" % problems)
        newest = tree[ATOM_PATH].index(b" <entry>")
        with open(atom_path, "wb") as f:
            f.write(tree[ATOM_PATH][:newest] + tree[ATOM_PATH][tree[ATOM_PATH].index(b" </entry>\n") + len(b" </entry>\n"):])
        problems = serve_check(directory, constants, out=lambda line: None)
        expect(any(p.startswith(SITE + "/" + ATOM_PATH + ": ") and "not the newest release releases.json lists" in p for p in problems),
               "serve-check fails on a releases.atom without the newest release: %s" % problems)
        with open(atom_path, "wb") as f:
            f.write(tree[ATOM_PATH])
        lines = []
        expect(serve_check(directory, constants, out=lines.append) == [], "and the site put back passes")
        expect(any("200 %s/%s -> 6 entries, the newest %s" % (SITE, ATOM_PATH, atom_entry_id("v6.3.5-rc1")) in l for l in lines),
               "serve-check reads the Atom feed: %s" % [l for l in lines if ATOM_PATH in l])

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--output", default=os.path.join(ROOT, "public"), help="the tree to write or check (default: public/ at the repository root)")
    parser.add_argument("--check", action="store_true", help="exit 1 when the tree, or the site, is not what the API says")
    parser.add_argument("--site", help="with --check: the deployed feed to compare instead of a directory")
    parser.add_argument("--serve-check", metavar="DIR", help="serve DIR as GitLab Pages would and read it as a server would")
    parser.add_argument("--self-test", action="store_true", help="test this script against canned answers, with no network")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    try:
        if args.serve_check:
            problems = serve_check(args.serve_check, server_constants())
            for problem in problems:
                print("PROBLEM: " + problem)
            print("serve-check: %d problem(s)" % len(problems))
            return 1 if problems else 0

        releases, files = load(http_get)
        tree, latest, warnings = render(releases, files)
        for warning in warnings:
            print("WARNING: " + warning)
        tags = sum(1 for n in tree if n.startswith("releases/tags/"))

        if args.check:
            if args.site:
                problems = check_site(tree, args.site)
                where = args.site
            else:
                if not os.path.isdir(args.output):
                    print("%s does not exist: write it first (no --check), or pass --site to check a deployed feed." % args.output)
                    return 1
                problems = differences(tree, read_tree(args.output), args.output)
                where = args.output
            for problem in problems:
                print(problem)
            if problems:
                print("%s is not the feed the releases make now (%d difference(s)); run this script without --check." % (where, len(problems)))
                return 1
            print("%s is the feed the releases make: %d files, %d releases, latest %s." % (where, len(tree), tags, latest["tag_name"]))
            return 0

        write_tree(tree, args.output)
        for relative in sorted(tree):
            print("%9d  %s" % (len(tree[relative]), relative))
        print("%d files, %d releases, releases/latest is %s, written to %s." % (len(tree), tags, latest["tag_name"], args.output))
        names = installer_names(latest["tag_name"][1:])
        have = set(a["name"] for a in latest["assets"])
        missing = [n for n in (names["windows"], names["windows"] + ".cosign.bundle") if n not in have]
        if missing == [names["windows"] + ".cosign.bundle"] and latest["tag_name"][1:] in UNSIGNED_BY_DESIGN:
            print("NOTE: %s carries no Sigstore bundle by design (%s)." % (latest["tag_name"], UNSIGNED_BY_DESIGN[latest["tag_name"][1:]]))
        elif missing:
            print("WARNING: %s carries no %s, so no Windows server can install it; --serve-check fails on it." % (
                latest["tag_name"], " and no ".join(missing)))
        linked = set(rule[0] for rule in redirect_rules(latest))
        absent = sorted(a for a in download_aliases().values() if a not in linked)
        print("The download links lead to %s%s." % (latest["tag_name"], "; these answer 404, since it does not carry them: " + ", ".join(absent) if absent else ""))
        return 0
    except FeedError as e:
        print("generate-update-feed: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
