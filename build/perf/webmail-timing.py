#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Time the webmail on a large folder: open, scroll to the end, search, and
select-all with a flag. hmailserver/docs/Performance.md is the method and the
table these numbers go into; build/perf/seed-folder.py makes the folder.

Two ways to measure, the same four operations:

  --mode http     the REST calls the page makes, in the order it makes them,
                  timed from here with nothing but the standard library. What
                  the server costs, with no browser in the way. The default,
                  and what runs where Playwright is not installed.
  --mode browser  the page itself in headless Chromium through Playwright
                  (pip install playwright; python -m playwright install
                  chromium): what a reader waits for, rendering included.
  --mode both     the one, then the other.

What each operation is, in both modes:

  open    the folder's first page listed: the first time (after a server
          start that is the folder read from the database) and then warm,
          the median of --repeat more. HTTP mode times the folder tree and the
          change probe too, which the page asks for beside it.
  scroll  the end of the folder: the Oldest page in one step, and every page
          walked with Older (before_uid) - all of them in HTTP mode, or
          --pages of them; --pages in the browser.
  search  a word that is in the body of every 1000th message (seed-folder's
          "quokka"), in the folder and across every folder, followed to the
          end through next_before_uid / next_before_id; and a word in every
          100th subject ("invoice"). Hits, requests and the time of each.
  select  select-all over the whole folder and one change made to every
          message by the server (POST /api/v1/me/folders/{id}/bulk), stepped
          to the end. HTTP mode adds and then removes a keyword ($PerfMark),
          so the folder is left as it was; --bulk-flag flagged or seen does
          that flag instead, set and then cleared. The browser presses the
          bulk bar's Mark as read, the one flag the bar has - which leaves the
          folder read.

Not in CI: it needs a running server with a seeded folder. The result is one
JSON document (--json) and a Markdown table in the shape of Performance.md's
(--markdown); both also go to standard output when no file is named.

Usage:
  python build/perf/webmail-timing.py --user perf@example.test --password SECRET
      [--base http://127.0.0.1:9530] [--insecure] [--folder INBOX]
      [--mode http|browser|both] [--repeat 5] [--pages 0]
      [--needle quokka] [--subject-needle invoice] [--bulk-flag keyword|flagged|seen]
      [--no-bulk] [--label "bench, SQL CE, 100k"] [--json out.json] [--markdown out.md]
"""

import argparse
import base64
import datetime
import http.client
import json
import platform
import ssl
import statistics
import sys
import time
import urllib.parse


# ---------------------------------------------------------------- HTTP mode

class Client(object):
    """The page's own session: a cookie from POST /api/v1/session, and
    X-Requested-With on every write, as Portal.js sends it."""

    def __init__(self, base, insecure, timeout):
        parts = urllib.parse.urlsplit(base)
        self.https = parts.scheme == "https"
        self.host = parts.hostname
        self.port = parts.port or (443 if self.https else 80)
        self.context = None
        if self.https:
            self.context = ssl.create_default_context()
            if insecure:
                self.context.check_hostname = False
                self.context.verify_mode = ssl.CERT_NONE
        self.timeout = timeout
        self.cookie = ""
        self.connection = None

    def _connect(self):
        if self.https:
            return http.client.HTTPSConnection(self.host, self.port, timeout=self.timeout, context=self.context)
        return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)

    def request(self, method, path, body=None, headers=None):
        """(status, parsed JSON or None, seconds, bytes). One keep-alive
        connection, opened again when the server has closed it."""
        h = {"Accept": "application/json"}
        if self.cookie:
            h["Cookie"] = self.cookie
        if method != "GET":
            h["X-Requested-With"] = "hMailServer"
        payload = None
        if body is not None:
            payload = json.dumps(body).encode("utf-8")
            h["Content-Type"] = "application/json"
        if headers:
            h.update(headers)
        for attempt in (0, 1):
            if self.connection is None:
                self.connection = self._connect()
            started = time.perf_counter()
            try:
                self.connection.request(method, path, body=payload, headers=h)
                response = self.connection.getresponse()
                raw = response.read()
            except (http.client.HTTPException, ConnectionError, OSError):
                self.connection.close()
                self.connection = None
                if attempt:
                    raise
                continue
            seconds = time.perf_counter() - started
            for name, value in response.getheaders():
                if name.lower() == "set-cookie" and not self.cookie:
                    self.cookie = value.split(";", 1)[0]
            if response.getheader("Connection", "").lower() == "close":
                self.connection.close()
                self.connection = None
            try:
                data = json.loads(raw.decode("utf-8")) if raw else None
            except ValueError:
                data = None
            return response.status, data, seconds, len(raw)
        raise RuntimeError("unreachable")

    def sign_in(self, user, password):
        token = base64.b64encode((user + ":" + password).encode("utf-8")).decode("ascii")
        status, data, _, _ = self.request("POST", "/api/v1/session", headers={"Authorization": "Basic " + token})
        if status not in (200, 201) or not self.cookie:
            raise SystemExit("could not sign in as %s: %s %s" % (user, status, data))


def ms(seconds):
    return round(seconds * 1000.0, 1)


def summary(values):
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    p95 = ordered[min(len(ordered) - 1, int(round(0.95 * (len(ordered) - 1))))]
    return {"n": len(values), "total_ms": ms(sum(values)), "median_ms": ms(statistics.median(values)),
            "p95_ms": ms(p95), "max_ms": ms(ordered[-1])}


def find_folder(tree, wanted):
    for folder in tree:
        if folder.get("path", "").lower() == wanted.lower():
            return folder
        found = find_folder(folder.get("subfolders") or [], wanted)
        if found:
            return found
    return None


def expect(status, data, what):
    if status != 200 or data is None:
        raise SystemExit("%s answered %s: %s" % (what, status, data))
    return data


def http_mode(args):
    client = Client(args.base, args.insecure, args.timeout)
    client.sign_in(args.user, args.password)
    result = {}

    # ---- open
    status, tree, tree_seconds, _ = client.request("GET", "/api/v1/me/folders")
    tree = expect(status, tree, "the folder tree")
    folder = find_folder(tree.get("folders") or [], args.folder)
    if not folder:
        raise SystemExit("no folder %s in the account" % args.folder)
    folder_id = folder["id"]
    listing_path = "/api/v1/me/folders/%d/messages" % folder_id

    status, first, first_seconds, first_bytes = client.request("GET", listing_path)
    first = expect(status, first, "the listing")
    warm = []
    for _ in range(args.repeat):
        status, again, seconds, _ = client.request("GET", listing_path)
        expect(status, again, "the listing")
        warm.append(seconds)
    trees = []
    probes = []
    for _ in range(args.repeat):
        trees.append(client.request("GET", "/api/v1/me/folders")[2])
        probes.append(client.request("GET", "/api/v1/me/changes")[2])
    result["open"] = {
        "folder_id": folder_id, "total": first.get("total"), "page_rows": len(first.get("messages") or []),
        "page_bytes": first_bytes, "first_ms": ms(first_seconds), "warm": summary(warm),
        "folder_tree_first_ms": ms(tree_seconds), "folder_tree": summary(trees), "change_probe": summary(probes),
    }

    # ---- scroll: the oldest page in one step, then every page by the cursor
    scroll = {}
    oldest = first.get("oldest_before_uid") or 0
    if oldest:
        status, page, seconds, _ = client.request("GET", "%s?before_uid=%d" % (listing_path, oldest))
        expect(status, page, "the oldest page")
        scroll["oldest_ms"] = ms(seconds)
        scroll["oldest_rows"] = len(page.get("messages") or [])
    else:
        scroll["oldest_ms"] = None
    pages = []
    rows = len(first.get("messages") or [])
    cursor = first["messages"][-1]["uid"] if first.get("messages") else 0
    started = time.perf_counter()
    while cursor and (not args.pages or len(pages) < args.pages):
        status, page, seconds, _ = client.request("GET", "%s?before_uid=%d" % (listing_path, cursor))
        page = expect(status, page, "a page")
        pages.append(seconds)
        messages = page.get("messages") or []
        if not messages:
            break
        rows += len(messages)
        cursor = messages[-1]["uid"]
    scroll["walk"] = summary(pages)
    scroll["walk_rows"] = rows
    scroll["walk_wall_ms"] = ms(time.perf_counter() - started)
    scroll["walk_reached_end"] = not args.pages or len(pages) < args.pages
    result["scroll"] = scroll

    # ---- search
    def follow(first_path, next_key, param):
        calls = []
        hits = 0
        scanned = 0
        path = first_path
        answer = None
        while len(calls) < args.search_requests:
            status, answer, seconds, _ = client.request("GET", path)
            answer = expect(status, answer, "a search")
            calls.append(seconds)
            hits += len(answer.get("messages") or [])
            scanned += answer.get("scanned") or 0
            nxt = answer.get(next_key) or 0
            if answer.get("complete", True) or not nxt:
                break
            path = "%s&%s=%d" % (first_path, param, nxt)
        return {"requests": len(calls), "first_ms": ms(calls[0]) if calls else None, "all": summary(calls),
                "hits": hits, "scanned": scanned, "complete": bool(answer and answer.get("complete", True))}

    needle = urllib.parse.quote(args.needle)
    result["search"] = {
        "body_word_in_folder": follow("%s?q=%s" % (listing_path, needle), "next_before_uid", "before_uid"),
        "body_word_everywhere": follow("/api/v1/me/search?q=%s&limit=200" % needle, "next_before_id", "before_id"),
        "subject_word_in_folder": follow("%s?q=%s" % (listing_path, urllib.parse.quote(args.subject_needle)), "next_before_uid", "before_uid"),
    }

    # ---- select-all and one change to every message, then undone
    if not args.no_bulk:
        # max_uid as the page takes it: the newest UID the listing read in
        # the same pass as its rows, else the newest row.
        top = max(int(first.get("newest_uid") or 0), max([m["uid"] for m in first.get("messages") or []] or [0]))

        def run(change):
            calls = []
            done = unchanged = failed = 0
            body = {"action": "flags", "all": True, "max_uid": top}
            body.update(change)
            started = time.perf_counter()
            while True:
                status, answer, seconds, _ = client.request("POST", "/api/v1/me/folders/%d/bulk" % folder_id, body=body)
                answer = expect(status, answer, "the bulk route")
                calls.append(seconds)
                done += answer.get("done", 0)
                unchanged += answer.get("unchanged", 0)
                failed += answer.get("failed", 0)
                if answer.get("complete") or not answer.get("next_before_uid"):
                    break
                body["before_uid"] = answer["next_before_uid"]
            return {"wall_ms": ms(time.perf_counter() - started), "requests": len(calls), "per_request": summary(calls),
                    "done": done, "unchanged": unchanged, "failed": failed}

        if args.bulk_flag == "keyword":
            on, off = {"keywords_add": ["$PerfMark"]}, {"keywords_remove": ["$PerfMark"]}
        else:
            on, off = {args.bulk_flag: True}, {args.bulk_flag: False}
        result["select_all"] = {"flag": args.bulk_flag, "max_uid": top, "set": run(on), "cleared": run(off)}

    return result


# ------------------------------------------------------------- browser mode

def browser_mode(args):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        raise SystemExit("--mode browser needs Playwright: pip install playwright; python -m playwright install chromium")

    result = {}
    client = Client(args.base, args.insecure, args.timeout)
    client.sign_in(args.user, args.password)
    status, tree, _, _ = client.request("GET", "/api/v1/me/folders")
    tree = expect(status, tree, "the folder tree")
    folder = find_folder(tree.get("folders") or [], args.folder)
    if not folder:
        raise SystemExit("no folder %s in the account" % args.folder)
    folder_id = folder["id"]
    long_wait = args.timeout * 1000

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(viewport={"width": 1440, "height": 900}, ignore_https_errors=args.insecure)
        page = context.new_page()

        page.goto(args.base.rstrip("/") + "/portal", timeout=long_wait)
        page.wait_for_selector("#signin:not([hidden])", timeout=long_wait)
        page.fill("#address", args.user)
        page.fill("#password", args.password)
        page.click("#signin-form button[type=submit]")
        page.wait_for_selector("#signin", state="hidden", timeout=long_wait)

        # The rows on screen are marked before each action and the wait is for
        # a list with none of them left: the list the action asked for has been
        # drawn, not the one it replaced.
        def stale():
            page.evaluate("() => { for (const c of document.getElementById('message-list').children) { c.setAttribute('data-perf-stale', '1'); } }")

        def rows_drawn():
            page.wait_for_function("() => { const l = document.getElementById('message-list');"
                                   " return l.children.length > 0 && !l.querySelector('[data-perf-stale]') && !l.classList.contains('skeleton'); }",
                                   timeout=long_wait)

        def navigate(hash_):
            stale()
            page.evaluate("h => { location.hash = h; }", hash_)

        # ---- open: from another folder to this one, until its rows are drawn.
        # Through another folder rather than the settings, because the page
        # keeps the last listing it drew and draws it again at once on the
        # way back - which is quick, and is not the folder being opened.
        others = []

        def collect(folders):
            for f in folders:
                if f.get("id") != folder_id:
                    others.append(f["id"])
                collect(f.get("subfolders") or [])

        collect(tree.get("folders") or [])
        opens = []
        for _ in range(1 + args.repeat):
            if others:
                navigate("#/f/%d" % others[0])
                rows_drawn()
            else:
                navigate("#/settings")
                page.wait_for_selector("#mail-section", state="hidden", timeout=long_wait)
            started = time.perf_counter()
            navigate("#/f/%d" % folder_id)
            rows_drawn()
            opens.append(time.perf_counter() - started)
        drawn = page.evaluate("() => document.querySelectorAll('#message-list .msg').length")
        result["open"] = {"first_ms": ms(opens[0]), "warm": summary(opens[1:]), "rows_drawn": drawn}

        # ---- scroll: the list to its foot, then the folder's end
        scroll = {}
        scroll["list_to_foot_ms"] = page.evaluate(
            "() => new Promise(done => { const l = document.getElementById('message-list'); const t = performance.now();"
            " l.scrollTop = l.scrollHeight; requestAnimationFrame(() => requestAnimationFrame(() => done(Math.round((performance.now() - t) * 10) / 10))); })")
        if page.is_visible("#page-oldest"):
            stale()
            started = time.perf_counter()
            page.click("#page-oldest")
            page.wait_for_function("() => location.hash.indexOf('before=') >= 0", timeout=long_wait)
            rows_drawn()
            scroll["oldest_ms"] = ms(time.perf_counter() - started)
        else:
            scroll["oldest_ms"] = None
        navigate("#/f/%d" % folder_id)
        rows_drawn()
        olders = []
        for _ in range(args.pages or 10):
            if not page.is_visible("#page-older"):
                break
            before = page.evaluate("() => location.hash")
            stale()
            started = time.perf_counter()
            page.click("#page-older")
            page.wait_for_function("h => location.hash !== h", arg=before, timeout=long_wait)
            rows_drawn()
            olders.append(time.perf_counter() - started)
        scroll["older_presses"] = summary(olders)
        result["scroll"] = scroll

        # ---- search in the folder, followed to the end
        navigate("#/f/%d" % folder_id)
        rows_drawn()
        stale()
        started = time.perf_counter()
        page.fill("#mail-search", args.needle)
        page.press("#mail-search", "Enter")
        page.wait_for_function("q => location.hash.indexOf('q=' + encodeURIComponent(q)) >= 0", arg=args.needle, timeout=long_wait)
        rows_drawn()
        first_answer = time.perf_counter() - started
        presses = 0
        while presses < args.search_requests:
            more = page.query_selector("#message-list .listfoot button")
            if not more:
                break
            before = page.evaluate("() => location.hash")
            stale()
            more.click()
            page.wait_for_function("h => location.hash !== h", arg=before, timeout=long_wait)
            rows_drawn()
            presses += 1
        result["search"] = {"first_answer_ms": ms(first_answer), "to_the_end_ms": ms(time.perf_counter() - started), "continuations": presses}

        # ---- select-all over the folder and Mark as read
        if not args.no_bulk:
            navigate("#/f/%d" % folder_id)
            rows_drawn()
            page.check("#select-all")
            page.wait_for_selector("#select-note:not([hidden])", timeout=long_wait)
            page.click("#select-folder")
            started = time.perf_counter()
            page.click("#bulk-read")
            page.wait_for_selector("#bulk-bar", state="hidden", timeout=long_wait)
            result["select_all"] = {"flag": "seen (Mark as read)", "wall_ms": ms(time.perf_counter() - started)}

        browser.close()
    return result


# ------------------------------------------------------------------ report

def markdown(document):
    lines = ["| Operation | Mode | Measure | Result |", "|---|---|---|---|"]

    def row(op, mode, measure, value):
        lines.append("| %s | %s | %s | %s |" % (op, mode, measure, "-" if value is None else value))

    h = document.get("http")
    if h:
        o = h["open"]
        row("Open", "HTTP", "first page, first time (ms)", o["first_ms"])
        row("Open", "HTTP", "first page, warm median (ms)", o["warm"].get("median_ms"))
        row("Open", "HTTP", "folder tree, warm median (ms)", o["folder_tree"].get("median_ms"))
        s = h["scroll"]
        row("Scroll", "HTTP", "Oldest page in one step (ms)", s.get("oldest_ms"))
        row("Scroll", "HTTP", "every page by Older: pages / total (ms)", "%s / %s" % (s["walk"].get("n"), s["walk"].get("total_ms")))
        row("Scroll", "HTTP", "one Older page, median / p95 (ms)", "%s / %s" % (s["walk"].get("median_ms"), s["walk"].get("p95_ms")))
        for key, label in (("body_word_in_folder", "body word, folder"), ("body_word_everywhere", "body word, every folder"),
                           ("subject_word_in_folder", "subject word, folder")):
            q = h["search"][key]
            row("Search", "HTTP", "%s: first answer (ms)" % label, q["first_ms"])
            row("Search", "HTTP", "%s: to the end - requests / total (ms) / hits" % label,
                "%s / %s / %s" % (q["requests"], q["all"].get("total_ms"), q["hits"]))
        b = h.get("select_all")
        if b:
            row("Select-all + flag", "HTTP", "%s set: requests / wall (ms) / changed" % b["flag"],
                "%s / %s / %s" % (b["set"]["requests"], b["set"]["wall_ms"], b["set"]["done"]))
            row("Select-all + flag", "HTTP", "%s cleared: requests / wall (ms)" % b["flag"],
                "%s / %s" % (b["cleared"]["requests"], b["cleared"]["wall_ms"]))
    w = document.get("browser")
    if w:
        row("Open", "Browser", "rows drawn, first time (ms)", w["open"]["first_ms"])
        row("Open", "Browser", "rows drawn, warm median (ms)", w["open"]["warm"].get("median_ms"))
        row("Scroll", "Browser", "list to its foot (ms)", w["scroll"]["list_to_foot_ms"])
        row("Scroll", "Browser", "Oldest pressed to rows drawn (ms)", w["scroll"]["oldest_ms"])
        row("Scroll", "Browser", "one Older press, median (ms)", w["scroll"]["older_presses"].get("median_ms"))
        row("Search", "Browser", "first answer drawn (ms)", w["search"]["first_answer_ms"])
        row("Search", "Browser", "to the end (ms) / continuations", "%s / %s" % (w["search"]["to_the_end_ms"], w["search"]["continuations"]))
        if w.get("select_all"):
            row("Select-all + flag", "Browser", "Mark as read over the folder, wall (ms)", w["select_all"]["wall_ms"])
    return "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(description="Time the webmail's open, scroll, search and select-all on a large folder.")
    parser.add_argument("--base", default="http://127.0.0.1:9530", help="the REST listener the webmail is served from")
    parser.add_argument("--insecure", action="store_true", help="accept the listener's own certificate")
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--folder", default="INBOX", help="the folder's path, as the folder tree names it")
    parser.add_argument("--mode", choices=("http", "browser", "both"), default="http")
    parser.add_argument("--repeat", type=int, default=5, help="warm repetitions of open")
    parser.add_argument("--pages", type=int, default=0, help="Older pages to walk: 0 is every page in HTTP mode, ten in the browser")
    parser.add_argument("--needle", default="quokka")
    parser.add_argument("--subject-needle", default="invoice")
    parser.add_argument("--search-requests", type=int, default=500, help="the most continuations a search is followed through")
    parser.add_argument("--bulk-flag", choices=("keyword", "flagged", "seen"), default="keyword")
    parser.add_argument("--no-bulk", action="store_true", help="leave select-all out")
    parser.add_argument("--timeout", type=float, default=1800.0, help="seconds one request or one wait may take")
    parser.add_argument("--label", default="", help="what was measured: the machine, the database, the folder")
    parser.add_argument("--json", help="write the result here")
    parser.add_argument("--markdown", help="write the table here")
    args = parser.parse_args(argv)

    document = {
        "label": args.label,
        "when": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "base": args.base,
        "folder": args.folder,
        "client": platform.platform(),
    }
    if args.mode in ("http", "both"):
        document["http"] = http_mode(args)
    if args.mode in ("browser", "both"):
        document["browser"] = browser_mode(args)

    text = json.dumps(document, indent=2)
    table = markdown(document)
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as out:
            out.write(text + "\n")
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8", newline="\n") as out:
            out.write(table)
    if not args.json and not args.markdown:
        print(text)
        print(table)
    else:
        print(table)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
