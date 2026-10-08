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
                  chromium, with PLAYWRIGHT_BROWSERS_PATH set to a folder of
                  its own: the install removes the browsers of other
                  Playwright versions from the folder it installs into, which
                  is otherwise the user's profile): what a reader waits for,
                  rendering included.
  --mode both     the one, then the other.

What each operation is, in both modes:

  open    the folder's first page listed: the first time and then warm, the
          median of --repeat more. HTTP mode asks for the folder tree first,
          as the page does, and times it too, the first time and warm, with
          the change probe the page polls beside it. After a server start the
          folder tree's first request is the one that reads the folder from
          the database - it counts each folder - so the cold read is in
          "folder tree, first time", and the first page after it is warm.
  scroll  the end of the folder: the Oldest page in one step, and every page
          walked with Older (before_uid) - all of them in HTTP mode, or
          --pages of them; --pages in the browser.
  search  a word that is in the body of every 1000th message (seed-folder's
          "quokka"), in the folder and across every folder, followed to the
          end through next_before_uid / next_before_id; and a word in every
          100th subject ("invoice"). Hits, requests and the time of each,
          and the message files the server opened for them.
  select  select-all over the whole folder and one change made to every
          message by the server (POST /api/v1/me/folders/{id}/bulk), stepped
          to the end. HTTP mode adds and then removes a keyword ($PerfMark),
          so the folder is left as it was; --bulk-flag flagged or seen does
          that flag instead, set and then cleared. The browser presses the
          bulk bar's Mark as read, the one flag the bar has - which leaves the
          folder read. For the inbox, the whole browser run has the account's
          inbox in one list (the "inbox" preference, put back afterwards): in
          tabs, the default, the box selects the open tab's messages alone.
          Putting it back signs in again when the run's own session has ended
          while the browser ran, and says so on standard error if it cannot.

What it does about the server between the measures, so that a run reaches its
end and every time in it is the server's work alone:

  429     the REST API refuses a credential's 201st request in ten seconds
          with 429 and Retry-After (RestApiServer.cpp), and the HTTP pass's
          Older walk - 500 requests of some 35 ms on the bench - passes that.
          The request is asked again after Retry-After; neither the refused
          request nor the wait is in any time, a wall time included. Standard
          error says each wait, the JSON counts them ("waits") and the
          Markdown says so under its table.
  401     a request whose session has ended - the service restarted, the
          idle time passed - signs in again once and is asked again,
          untimed, and is counted in "waits" as well. The browser signs in
          again when the page shows its sign-in form after a failed search.
  failed  a search whose request fails - an answer cut short, a status
          other than 200, a timeout - is recorded in its place, in the JSON
          ("failure": which request, after how long, why) and in the table,
          and the run goes on to the next measure; standard error says so,
          and the tool ends with 1 once it has written everything. A request
          is asked again only when a kept-alive connection the server had
          closed failed before any answer, never after part of one: a search
          cut short is not timed twice. Any other request that fails still
          ends the run.

Not in CI: it needs a running server with a seeded folder. The result is one
JSON document (--json) and a Markdown table of one run (--markdown), whose rows
are Performance.md's; both also go to standard output when no file is named.
--self-test, which CI runs, puts HTTP mode against a REST listener of its own
on the loopback that refuses one request with 429, ends the session once and
cuts one search's answer short, and checks what the run recorded.

The account's password comes from the environment, HMPERF_PASSWORD, as the
load generator's do (build/perf/load/loadgen.py): a process's arguments are in
every user's process list. --password still takes one, for a throwaway account.

Usage:
  HMPERF_PASSWORD=SECRET python build/perf/webmail-timing.py --user perf@example.test
      [--base http://127.0.0.1:9530] [--insecure] [--folder INBOX]
      [--mode http|browser|both] [--repeat 5] [--pages 0]
      [--needle quokka] [--subject-needle invoice] [--bulk-flag keyword|flagged|seen]
      [--no-bulk] [--label "bench, SQL CE, 100k"] [--json out.json] [--markdown out.md]
  python build/perf/webmail-timing.py --self-test
"""

import argparse
import base64
import datetime
import http.client
import json
import os
import platform
import ssl
import statistics
import sys
import time
import urllib.parse


SESSION_PATH = "/api/v1/session"

# A run that is refused this many times in all stops waiting and takes the
# 429 as the answer: a server that refuses everything is not waited on for ever.
MOST_REFUSALS = 1000


def describe(error):
    """An exception, short enough for a table's cell."""
    return repr(error)[:200]


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
        self.credentials = None
        # What the measures leave out: requests refused with 429 and the waits
        # after them, sessions signed in again, and the seconds all of it took.
        self.refused = 0
        self.waited_seconds = 0.0
        self.signed_in_again = 0
        self.excluded = 0.0

    def clock(self):
        """A clock that stops while a refusal is waited out or a session signed
        in again: a wall time read from it holds the server's work alone."""
        return time.perf_counter() - self.excluded

    def waits(self):
        return {"refused_429": self.refused, "seconds_waited": round(self.waited_seconds, 1),
                "signed_in_again": self.signed_in_again,
                "note": "each 429 asked again after Retry-After and each ended session signed in again; "
                        "neither the refused request, the wait nor the sign-in is in any time"}

    def _connect(self):
        if self.https:
            return http.client.HTTPSConnection(self.host, self.port, timeout=self.timeout, context=self.context)
        return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)

    def _drop(self):
        if self.connection is not None:
            self.connection.close()
        self.connection = None

    def request(self, method, path, body=None, headers=None):
        """(status, parsed JSON or None, seconds, bytes) of the request as it
        was answered. A 429 is asked again after its Retry-After, and a 401 on
        a session that has ended signs in again and is asked again, once;
        neither the refused request nor what followed it is timed."""
        signed_in_again = False
        while True:
            status, data, seconds, size, retry_after = self._once(method, path, body, headers)
            if status == 429 and self.refused < MOST_REFUSALS:
                wait = retry_after if retry_after > 0 else 10.0
                self.refused += 1
                self.waited_seconds += wait
                sys.stderr.write("webmail-timing: 429 on %s %s: waiting %.1f s (Retry-After) and asking again, "
                                 "untimed\n" % (method, path.split("?")[0], wait))
                paused = time.perf_counter()
                time.sleep(wait + 0.25)
                self.excluded += seconds + (time.perf_counter() - paused)
                continue
            if status == 401 and self.cookie and self.credentials and not signed_in_again and path != SESSION_PATH:
                signed_in_again = True
                before = self.excluded
                began = time.perf_counter()
                self.cookie = ""
                self.sign_in(*self.credentials)
                self.signed_in_again += 1
                self.excluded = before + seconds + (time.perf_counter() - began)
                sys.stderr.write("webmail-timing: %s %s answered 401, the session having ended: signed in again "
                                 "and asked again, untimed\n" % (method, path.split("?")[0]))
                continue
            return status, data, seconds, size

    def _once(self, method, path, body, headers):
        """One request: (status, JSON or None, seconds, bytes, Retry-After).
        One keep-alive connection, opened again when the server has closed it.
        A kept-alive connection that fails before any answer is the server
        having closed it while idle, and the request goes once more on a new
        one; an answer cut short is raised, not asked for again."""
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
            reused = self.connection is not None
            if not reused:
                self.connection = self._connect()
            started = time.perf_counter()
            try:
                self.connection.request(method, path, body=payload, headers=h)
                response = self.connection.getresponse()
            except (ConnectionError, http.client.BadStatusLine):
                self._drop()
                if attempt or not reused:
                    raise
                self.excluded += time.perf_counter() - started
                continue
            except (http.client.HTTPException, OSError):
                self._drop()
                raise
            try:
                raw = response.read()
            except (http.client.HTTPException, OSError):
                self._drop()
                raise
            seconds = time.perf_counter() - started
            for name, value in response.getheaders():
                if name.lower() == "set-cookie" and not self.cookie:
                    self.cookie = value.split(";", 1)[0]
            try:
                retry_after = float(response.getheader("Retry-After", "0") or 0)
            except ValueError:
                retry_after = 0.0
            if response.getheader("Connection", "").lower() == "close":
                self._drop()
            try:
                data = json.loads(raw.decode("utf-8")) if raw else None
            except ValueError:
                data = None
            return response.status, data, seconds, len(raw), retry_after
        raise RuntimeError("unreachable")

    def sign_in(self, user, password):
        token = base64.b64encode((user + ":" + password).encode("utf-8")).decode("ascii")
        status, data, _, _ = self.request("POST", SESSION_PATH, headers={"Authorization": "Basic " + token})
        if status not in (200, 201) or not self.cookie:
            raise SystemExit("could not sign in as %s: %s %s" % (user, status, data))
        self.credentials = (user, password)


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


def http_mode(args, failures):
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
    started = client.clock()
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
    scroll["walk_wall_ms"] = ms(client.clock() - started)
    scroll["walk_reached_end"] = not args.pages or len(pages) < args.pages
    result["scroll"] = scroll

    # ---- search
    def follow(name, first_path, next_key, param):
        calls = []
        hits = 0
        scanned = 0
        opened = 0
        path = first_path
        answer = None
        failure = None
        while len(calls) < args.search_requests:
            began = client.clock()
            try:
                status, answer, seconds, _ = client.request("GET", path)
            except (http.client.HTTPException, OSError) as error:
                failure = {"request": len(calls) + 1, "after_ms": ms(client.clock() - began), "error": describe(error)}
                answer = None
                break
            if status != 200 or answer is None:
                failure = {"request": len(calls) + 1, "after_ms": ms(seconds),
                           "error": "HTTP %s: %s" % (status, json.dumps(answer)[:160] if answer is not None else "no JSON")}
                answer = None
                break
            calls.append(seconds)
            hits += len(answer.get("messages") or [])
            scanned += answer.get("scanned") or 0
            # The message files the server read, of the scanned: with the full-text
            # index caught up, the hits alone (Performance.md, the probe).
            opened += answer.get("opened") or 0
            nxt = answer.get(next_key) or 0
            if answer.get("complete", True) or not nxt:
                break
            path = "%s&%s=%d" % (first_path, param, nxt)
        found = {"requests": len(calls), "first_ms": ms(calls[0]) if calls else None, "all": summary(calls),
                 "hits": hits, "scanned": scanned, "opened": opened,
                 "complete": bool(answer and answer.get("complete", True))}
        if failure:
            found["failure"] = failure
            failures.append("HTTP search %s: request %d failed after %s ms: %s" % (
                name, failure["request"], failure["after_ms"], failure["error"]))
            sys.stderr.write("webmail-timing: the search %s failed at its request %d after %s ms (%s); recorded, "
                             "and the run goes on\n" % (name, failure["request"], failure["after_ms"], failure["error"]))
        return found

    needle = urllib.parse.quote(args.needle)
    result["search"] = {
        "body_word_in_folder": follow("body_word_in_folder", "%s?q=%s" % (listing_path, needle), "next_before_uid", "before_uid"),
        "body_word_everywhere": follow("body_word_everywhere", "/api/v1/me/search?q=%s&limit=200" % needle, "next_before_id", "before_id"),
        "subject_word_in_folder": follow("subject_word_in_folder", "%s?q=%s" % (listing_path, urllib.parse.quote(args.subject_needle)),
                                         "next_before_uid", "before_uid"),
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
            started = client.clock()
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
            return {"wall_ms": ms(client.clock() - started), "requests": len(calls), "per_request": summary(calls),
                    "done": done, "unchanged": unchanged, "failed": failed}

        if args.bulk_flag == "keyword":
            on, off = {"keywords_add": ["$PerfMark"]}, {"keywords_remove": ["$PerfMark"]}
        else:
            on, off = {args.bulk_flag: True}, {args.bulk_flag: False}
        result["select_all"] = {"flag": args.bulk_flag, "max_uid": top, "set": run(on), "cleared": run(off)}

    result["waits"] = client.waits()
    return result


# ------------------------------------------------------------- browser mode

# The page's "Search older messages" button.
SEARCH_OLDER = "#list-foot .listfoot button"


def browser_mode(args, failures):
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

    # The inbox in one list for the run. In tabs, an account's default, the page
    # shows the open tab alone, and ticking its box selects that tab's messages:
    # Mark as read would change the Primary tab's (3 in 5 at seed 49), not the
    # folder's that HTTP mode changes. The account's own choice is put back at the
    # end, whatever happens in between.
    one_list = folder.get("path", "").lower() == "inbox"
    previous_view = None
    if one_list:
        status, prefs, _, _ = client.request("GET", "/api/v1/me/preferences")
        previous_view = (expect(status, prefs, "the preferences").get("preferences") or {}).get("inbox")
        expect(*client.request("PUT", "/api/v1/me/preferences", body={"inbox": "all"})[:2], what="the inbox view")
    result["view"] = "one list" if one_list else "the folder"
    try:
        return _browser_run(args, result, tree, folder_id, long_wait, sync_playwright, failures)
    finally:
        if one_list:
            _put_back_view(args, client, previous_view)
        result["waits"] = client.waits()


def _put_back_view(args, client, previous_view):
    """The account's inbox view as it was before the run (None: the key was not
    set, and a null removes it). The session this tool signed in with sits idle
    while the browser runs, and may have ended by then - a session ends after
    the server's idle time, 30 minutes unless the server or the account's domain
    sets another - so a refusal signs in once more (Client.request does, on a
    401). What still cannot be put back is said, never passed over, and never
    raised: the run's result, or its own error, is what the caller gets."""
    body = {"inbox": previous_view}
    try:
        status = client.request("PUT", "/api/v1/me/preferences", body=body)[0]
        if status != 200:
            fresh = Client(args.base, args.insecure, args.timeout)
            fresh.sign_in(args.user, args.password)
            status = fresh.request("PUT", "/api/v1/me/preferences", body=body)[0]
    except (SystemExit, OSError, http.client.HTTPException) as error:
        status = error
    if status != 200:
        sys.stderr.write("webmail-timing: the account's inbox is left in one list (%s); set the webmail's "
                         "Settings, Inbox, back to %s\n" % (status, previous_view or "tabs, the default"))


def _fill_sign_in(page, args, long_wait):
    page.fill("#address", args.user)
    page.fill("#password", args.password)
    page.click("#signin-form button[type=submit]")
    page.wait_for_selector("#signin", state="hidden", timeout=long_wait)


def _load_again(page, args, long_wait, result):
    """The page loaded afresh, signed in again if it settles on its sign-in
    form. After a failed search the page is left waiting on an answer that
    never came whole, and on 7 October 2026 it showed the sign-in form when it
    was loaded again (Performance.md)."""
    page.goto(args.base.rstrip("/") + "/portal", timeout=long_wait)
    try:
        # The mailbox, when the session still stands: Portal.js shows #account
        # and hides #signin once it has read the session.
        page.wait_for_selector("#account:not([hidden])", timeout=15000)
        return
    except Exception:
        pass
    page.wait_for_selector("#signin:not([hidden])", timeout=long_wait)
    _fill_sign_in(page, args, long_wait)
    result["signed_in_again"] = result.get("signed_in_again", 0) + 1
    sys.stderr.write("webmail-timing: the page showed its sign-in form when loaded again: signed in again\n")


def _browser_run(args, result, tree, folder_id, long_wait, sync_playwright, failures):
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(viewport={"width": 1440, "height": 900}, ignore_https_errors=args.insecure)
        page = context.new_page()

        page.goto(args.base.rstrip("/") + "/portal", timeout=long_wait)
        page.wait_for_selector("#signin:not([hidden])", timeout=long_wait)
        _fill_sign_in(page, args, long_wait)

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

        # ---- search in the folder, followed to the end. A search that fails -
        # its answer cut short, or never drawn within --timeout - is recorded,
        # and the page is loaded again for the select-all.
        navigate("#/f/%d" % folder_id)
        rows_drawn()
        stale()
        started = time.perf_counter()
        presses = 0
        first_answer = None
        try:
            page.fill("#mail-search", args.needle)
            page.press("#mail-search", "Enter")
            page.wait_for_function("q => location.hash.indexOf('q=' + encodeURIComponent(q)) >= 0", arg=args.needle, timeout=long_wait)
            rows_drawn()
            first_answer = time.perf_counter() - started
            while presses < args.search_requests:
                # Search older messages: the note under the list, #list-foot, beside
                # #message-list and not in it since 6.3.5 (Portal.js, renderSearchNote:
                # a listbox holds options and nothing else).
                more = page.query_selector(SEARCH_OLDER)
                if not more:
                    break
                before = page.evaluate("() => location.hash")
                stale()
                more.click()
                page.wait_for_function("h => location.hash !== h", arg=before, timeout=long_wait)
                rows_drawn()
                presses += 1
            result["search"] = {"first_answer_ms": ms(first_answer), "to_the_end_ms": ms(time.perf_counter() - started),
                                "continuations": presses}
        except Exception as error:  # Playwright's own errors and timeouts alike
            failure = {"after_ms": ms(time.perf_counter() - started), "first_answer_ms": ms(first_answer) if first_answer else None,
                       "continuations": presses, "error": describe(error)}
            result["search"] = {"failure": failure}
            failures.append("browser search: failed after %s ms: %s" % (failure["after_ms"], failure["error"]))
            sys.stderr.write("webmail-timing: the browser's search failed after %s ms (%s); recorded, and the run "
                             "goes on\n" % (failure["after_ms"], failure["error"]))
            _load_again(page, args, long_wait, result)

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
    notes = []

    def row(op, mode, measure, value):
        lines.append("| %s | %s | %s | %s |" % (op, mode, measure, "-" if value is None else str(value).replace("|", "/")))

    def waited(mode, waits):
        if waits and (waits.get("refused_429") or waits.get("signed_in_again")):
            notes.append("%s: requests refused with 429 and asked again after Retry-After: %d (%s s waited); sessions "
                         "signed in again: %d. Neither a refused request, a wait nor a sign-in is in any time above."
                         % (mode, waits.get("refused_429", 0), waits.get("seconds_waited", 0), waits.get("signed_in_again", 0)))

    h = document.get("http")
    if h:
        o = h["open"]
        row("Open", "HTTP", "folder tree, first time (ms)", o.get("folder_tree_first_ms"))
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
            failure = q.get("failure")
            if failure:
                row("Search", "HTTP", "%s: first answer (ms)" % label, q["first_ms"] if q["first_ms"] is not None else
                    "failed after %s ms: %s" % (failure["after_ms"], failure["error"][:100]))
                row("Search", "HTTP", "%s: to the end - requests / total (ms) / hits" % label,
                    "failed at request %s after %s ms (%s / %s / %s before it)" % (
                        failure["request"], failure["after_ms"], q["requests"], q["all"].get("total_ms", 0), q["hits"]))
                continue
            row("Search", "HTTP", "%s: first answer (ms)" % label, q["first_ms"])
            row("Search", "HTTP", "%s: to the end - requests / total (ms) / hits" % label,
                "%s / %s / %s" % (q["requests"], q["all"].get("total_ms"), q["hits"]))
        b = h.get("select_all")
        if b:
            row("Select-all + flag", "HTTP", "%s set: requests / wall (ms) / changed" % b["flag"],
                "%s / %s / %s" % (b["set"]["requests"], b["set"]["wall_ms"], b["set"]["done"]))
            row("Select-all + flag", "HTTP", "%s cleared: requests / wall (ms)" % b["flag"],
                "%s / %s" % (b["cleared"]["requests"], b["cleared"]["wall_ms"]))
        waited("HTTP", h.get("waits"))
    w = document.get("browser")
    if w:
        row("Open", "Browser", "rows drawn, first time (ms)", w["open"]["first_ms"])
        row("Open", "Browser", "rows drawn, warm median (ms)", w["open"]["warm"].get("median_ms"))
        row("Scroll", "Browser", "list to its foot (ms)", w["scroll"]["list_to_foot_ms"])
        row("Scroll", "Browser", "Oldest pressed to rows drawn (ms)", w["scroll"]["oldest_ms"])
        row("Scroll", "Browser", "one Older press, median (ms)", w["scroll"]["older_presses"].get("median_ms"))
        failure = w["search"].get("failure")
        if failure:
            row("Search", "Browser", "first answer drawn (ms)", failure["first_answer_ms"] if failure["first_answer_ms"] is not None
                else "failed after %s ms: %s" % (failure["after_ms"], failure["error"][:100]))
            row("Search", "Browser", "to the end (ms) / continuations",
                "failed after %s ms, %s continuations before it" % (failure["after_ms"], failure["continuations"]))
        else:
            row("Search", "Browser", "first answer drawn (ms)", w["search"]["first_answer_ms"])
            row("Search", "Browser", "to the end (ms) / continuations", "%s / %s" % (w["search"]["to_the_end_ms"], w["search"]["continuations"]))
        if w.get("select_all"):
            row("Select-all + flag", "Browser", "Mark as read over the folder, wall (ms)", w["select_all"]["wall_ms"])
        waited("Browser, the tool's own requests", w.get("waits"))
        if w.get("signed_in_again"):
            notes.append("Browser: the page signed in again %d times after a failed search." % w["signed_in_again"])
    if notes:
        lines.append("")
        lines.extend(notes)
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------- self-test

def self_test():
    """HTTP mode against a REST listener of this tool's own on the loopback: a
    folder of 1,000 messages, one request refused with 429 in the Older walk,
    the session ended once in the search of every folder, and the subject
    search's answer cut short. What the run recorded is checked; the exit code
    is the number of checks that failed."""
    import http.server
    import shutil
    import tempfile
    import threading

    failed = []

    def check(name, condition, detail=""):
        sys.stderr.write("%s  %s%s\n" % ("PASS" if condition else "FAIL", name, (" - " + detail) if detail else ""))
        if not condition:
            failed.append(name)

    user, password = "perf@selftest.test", "selftest-secret"
    total, page_size, window = 1000, 200, 250
    state = {"sign_ins": 0, "sessions": set(), "refused": 0, "ended": 0, "cut": 0, "subject_asked": 0}
    lock = threading.Lock()

    class Fake(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *arguments):
            pass

        def send_json(self, status, body, extra=None):
            data = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            for name, value in (extra or {}).items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(data)

        def handle_any(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8")) if length else None
            parts = urllib.parse.urlsplit(self.path)
            query = dict(urllib.parse.parse_qsl(parts.query))
            if self.command == "POST" and parts.path == SESSION_PATH:
                wanted = "Basic " + base64.b64encode((user + ":" + password).encode("utf-8")).decode("ascii")
                if self.headers.get("Authorization") != wanted:
                    return self.send_json(401, {"error": "not signed in"})
                with lock:
                    state["sign_ins"] += 1
                    token = "session%d" % state["sign_ins"]
                    state["sessions"].add(token)
                return self.send_json(201, {"address": user}, {"Set-Cookie": "hmailsession=%s; HttpOnly; SameSite=Strict" % token})
            token = (self.headers.get("Cookie") or "").partition("=")[2]
            with lock:
                # The Older walk's second page is refused once, as the 201st
                # request in ten seconds is; the search of every folder's
                # second request finds its session ended, once.
                if parts.path.endswith("/messages") and query.get("before_uid") == "601" and "q" not in query and not state["refused"]:
                    state["refused"] += 1
                    refuse = True
                else:
                    refuse = False
                if parts.path == "/api/v1/me/search" and "before_id" in query and not state["ended"]:
                    state["ended"] += 1
                    state["sessions"].discard(token)
                known = token in state["sessions"]
            if refuse:
                return self.send_json(429, {"error": "too many requests"}, {"Retry-After": "1"})
            if not known:
                return self.send_json(401, {"error": "not signed in"})
            if parts.path == "/api/v1/me/folders":
                return self.send_json(200, {"folders": [{"id": 5, "path": "INBOX", "subfolders": [
                    {"id": 6, "path": "INBOX/Receipts", "subfolders": []}]}]})
            if parts.path == "/api/v1/me/changes":
                return self.send_json(200, {"changes": []})
            if parts.path == "/api/v1/me/folders/5/messages" and query.get("q") == "invoice":
                # An answer cut short: the head promises more than is sent, and the
                # connection is closed behind it.
                with lock:
                    state["subject_asked"] += 1
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", "5000")
                self.end_headers()
                self.wfile.write(b'{"messages": [' + b'{"uid": 1}, ' * 8)
                self.wfile.flush()
                self.close_connection = True
                return None
            if parts.path in ("/api/v1/me/folders/5/messages", "/api/v1/me/search") and query.get("q") == "quokka":
                key = "before_uid" if parts.path.endswith("/messages") else "before_id"
                below = int(query.get(key) or total + 1)
                low = max(1, below - window)
                hits = [{"uid": n, "id": n} for n in range(below - 1, low - 1, -1) if n % 100 == 0]
                complete = low == 1
                answer = {"messages": hits, "scanned": below - low, "opened": len(hits), "complete": complete}
                if not complete:
                    answer["next_" + key] = low
                return self.send_json(200, answer)
            if parts.path == "/api/v1/me/folders/5/messages":
                below = int(query.get("before_uid") or total + 1)
                uids = list(range(below - 1, max(0, below - 1 - page_size), -1))
                return self.send_json(200, {"total": total, "messages": [{"uid": n} for n in uids], "newest_uid": total,
                                            "oldest_before_uid": page_size + 1})
            if self.command == "POST" and parts.path == "/api/v1/me/folders/5/bulk":
                below = int((body or {}).get("before_uid") or (body or {}).get("max_uid", total) + 1)
                low = max(1, below - 500)
                answer = {"done": below - low, "unchanged": 0, "failed": 0, "complete": low == 1}
                if low > 1:
                    answer["next_before_uid"] = low
                return self.send_json(200, answer)
            return self.send_json(404, {"error": "no such route in the self-test"})

        do_GET = do_POST = do_PUT = do_DELETE = handle_any

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    work = tempfile.mkdtemp(prefix="webmail-timing-selftest-")
    out, table = os.path.join(work, "run.json"), os.path.join(work, "run.md")
    try:
        code = main(["--base", "http://127.0.0.1:%d" % server.server_address[1], "--user", user, "--password", password,
                     "--repeat", "2", "--timeout", "30", "--label", "self-test", "--json", out, "--markdown", table])
    except SystemExit as stopped:
        # The run stopped where it should have gone on: what it said is the detail.
        code = "stopped: %s" % stopped
    finally:
        server.shutdown()
        server.server_close()
    try:
        with open(out, encoding="utf-8") as handle:
            document = json.load(handle)
        with open(table, encoding="utf-8") as handle:
            text = handle.read()
    except (OSError, ValueError) as error:
        check("the run wrote its JSON and its table", False, "%s; the run: %s" % (describe(error), code))
        shutil.rmtree(work, ignore_errors=True)
        return len(failed)
    shutil.rmtree(work, ignore_errors=True)

    h = document.get("http") or {}
    waits = h.get("waits") or {}
    search = h.get("search") or {}
    scroll = h.get("scroll") or {}
    check("the run ends with 1, a failure recorded, after writing everything", code == 1 and document.get("failures"),
          "exit %s, failures %s" % (code, document.get("failures")))
    check("the 429 asked again after its Retry-After", waits.get("refused_429") == 1 and waits.get("seconds_waited") == 1.0
          and state["refused"] == 1, json.dumps(waits))
    check("the refusal and the wait are in no time: the Older walk's total and its wall time",
          scroll.get("walk", {}).get("n") == 5 and scroll.get("walk_rows") == total
          and scroll["walk"].get("total_ms", 1e9) < 1000 and scroll.get("walk_wall_ms", 1e9) < 1000,
          "walk %s, wall %s ms" % (json.dumps(scroll.get("walk")), scroll.get("walk_wall_ms")))
    check("the ended session signed in again, once", waits.get("signed_in_again") == 1 and state["sign_ins"] == 2,
          "signed in again %s, sign-ins seen %d" % (waits.get("signed_in_again"), state["sign_ins"]))
    body_folder, everywhere = search.get("body_word_in_folder") or {}, search.get("body_word_everywhere") or {}
    check("the searches around them followed to the end", body_folder.get("complete") and body_folder.get("hits") == 10
          and body_folder.get("requests") == 4 and body_folder.get("opened") == 10
          and everywhere.get("complete") and everywhere.get("hits") == 10 and everywhere.get("requests") == 4,
          "%s / %s" % (json.dumps(body_folder)[:160], json.dumps(everywhere)[:160]))
    subject = search.get("subject_word_in_folder") or {}
    check("the search cut short recorded, not asked again, and the run gone on to the select-all",
          (subject.get("failure") or {}).get("request") == 1 and not subject.get("complete")
          and "IncompleteRead" in (subject.get("failure") or {}).get("error", "") and state["subject_asked"] == 1
          and (h.get("select_all") or {}).get("set", {}).get("done") == total
          and (h.get("select_all") or {}).get("cleared", {}).get("done") == total,
          "%s; asked %d times" % (json.dumps(subject)[:200], state["subject_asked"]))
    check("the table: the folder tree's first time, the failure and the waits",
          "| Open | HTTP | folder tree, first time (ms) |" in text and "failed at request 1 after" in text
          and "HTTP: requests refused with 429 and asked again after Retry-After: 1 (1.0 s waited)" in text
          and "sessions signed in again: 1." in text, text[-400:])
    sys.stderr.write("%d failed\n" % len(failed) if failed else "all passed\n")
    return len(failed)


def main(argv):
    parser = argparse.ArgumentParser(description="Time the webmail's open, scroll, search and select-all on a large folder.")
    parser.add_argument("--base", default="http://127.0.0.1:9530", help="the REST listener the webmail is served from")
    parser.add_argument("--insecure", action="store_true", help="accept the listener's own certificate")
    parser.add_argument("--user", help="the account (required but for --self-test)")
    parser.add_argument("--password", help="the account's password; better HMPERF_PASSWORD in the environment")
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
    parser.add_argument("--self-test", action="store_true",
                        help="run HTTP mode against a REST listener of this tool's own and check what it recorded")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.user:
        parser.error("--user: the account to sign in as")
    if not args.password:
        args.password = os.environ.get("HMPERF_PASSWORD", "")
    if not args.password:
        parser.error("the account's password: HMPERF_PASSWORD in the environment, or --password")

    document = {
        "label": args.label,
        "when": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "base": args.base,
        "folder": args.folder,
        "client": platform.platform(),
    }
    failures = []
    if args.mode in ("http", "both"):
        document["http"] = http_mode(args, failures)
    if args.mode in ("browser", "both"):
        document["browser"] = browser_mode(args, failures)
    if failures:
        document["failures"] = failures

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
    if failures:
        sys.stderr.write("webmail-timing: %d measure(s) failed and are recorded as such: %s\n" % (len(failures), "; ".join(failures)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
