#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Control Deck's script, executed rather than grepped.

The Control Deck is one file, hmailserver/installation/WebAdmin/index.html: the
markup, one style block and one inline script, served by the server at / and
installed by the packages. It is the only graphical administration tool a Linux
server has, and until this existed nothing ran its script: a view could draw
nothing from a perfectly good answer, a form could send a key the route refuses,
a refusal could be swallowed, and every check would stay green.

So: this reads the page, lifts the script out, checks the cheap truths about the
markup against the Content-Security-Policy the server sends for the page (read
from RestApiServer.cpp, not copied here) - whose script-src is the SHA-256 of
the page's one inline script, which the server computes from the bytes it
serves (Common/Util/InlineScriptHash.h), and never 'unsafe-inline' or 'self' -
and hands the page and the script to
build/deck-script-test.js, which builds a small DOM from the markup, answers
fetch out of a recorded server whose state the writes change, runs the script in
it and asserts behaviour: a sign-in that keeps no password, each view drawn from
its answers, each form sending the JSON its route takes, each refusal shown in
the server's own words, each change read back after the round trip. The page is
also served with Trusted Types, under the one policy its script makes first:
this holds the script to building none of the elements that policy refuses, and
build/trusted-types-test.js then runs the policy itself.

Run from the repository root:  python3 build/check-deck-script.py
Needs node on PATH (the harnesses job of .gitlab-ci.yml runs in a node image).
--keep names a directory to leave the extracted files in, for looking at by hand.
"""
import argparse
import base64
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "hmailserver", "installation", "WebAdmin", "index.html")
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util", "RestApiServer.cpp")
RUNNER = os.path.join(ROOT, "build", "deck-script-test.js")
TRUSTED_TYPES = os.path.join(ROOT, "build", "trusted-types-test.js")

# The page's Trusted Types policy: the block at the top of its script, and the
# pattern of the elements it refuses, which no view may build.
TRUSTED_TYPES_BLOCK = re.compile(r"/\* ---------- Trusted Types ----------.*?\}\)\(\);", re.S)
NEVER_PATTERN = re.compile(r"const NEVER=/(?P<pattern>.+?)/i;")

# A script element as the HTML tokenizer finds one: "<script" and "</script" in
# any case, each followed by white space, "/" or ">" - so "<scripts>" is not one.
# read() has already made CR LF and a lone CR LF, as the parser does first.
SCRIPT = re.compile(r"<script(?=[\t\n\f />])(?P<attrs>[^>]*)>(?P<body>.*?)</script(?=[\t\n\f />])", re.S | re.I)

# What stands for the script source HandleWebAdminPage_ computes, in the policy
# this check reads from it.
COMPUTED = "<computed-hash>"
# The expression that computes it, and the cache it is asked of.
SCRIPT_SOURCE_CALL = "deckScript_.ScriptSource(body)"
SCRIPT_SOURCE_CACHE = re.compile(r"\bInlineScriptHashCache\s+deckScript_\s*;")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def script_source(script):
    """script-src for the script's text as a browser hashes it: UTF-8, SHA-256, base64."""
    digest = hashlib.sha256(script.encode("utf-8")).digest()
    return "'sha256-%s'" % base64.b64encode(digest).decode("ascii")


def handler_text(source):
    """HandleWebAdminPage_'s body, from its name to the next function's."""
    at = source.find("RestApiServer::HandleWebAdminPage_()")
    if at < 0:
        raise SystemExit("RestApiServer.cpp no longer defines HandleWebAdminPage_; this check reads the page's policy from it")
    end = source.find("\n   HttpResponse\n", at)
    return source[at:end if end > at else len(source)]


def policy_for_page(source):
    """The Content-Security-Policy HandleWebAdminPage_ sends, as {directive: [sources]}.

    The header is two literals around the script source the server computes:
        "Content-Security-Policy: ...; script-src " + deckScript_.ScriptSource(body) + "; ...\\r\\n"
    which reads here as script-src COMPUTED. A policy that is one literal again
    is read as it is, and check_markup says what is wrong with its script-src.
    """
    handler = handler_text(source)
    computed = re.search(r'"Content-Security-Policy: ([^"]*?)"\s*\+\s*([\w.]+\([^()]*\))\s*\+\s*"([^"]*?)\\r\\n"', handler)
    if computed:
        if computed.group(2) != SCRIPT_SOURCE_CALL:
            raise SystemExit("HandleWebAdminPage_ builds its policy around %s; this check knows %s, the page's script hash"
                             % (computed.group(2), SCRIPT_SOURCE_CALL))
        if not SCRIPT_SOURCE_CACHE.search(source):
            raise SystemExit("RestApiServer.cpp no longer declares deckScript_ as an InlineScriptHashCache")
        text = computed.group(1) + COMPUTED + computed.group(3)
    else:
        literal = re.search(r'"Content-Security-Policy: ([^"]*?)\\r\\n"', handler)
        if not literal:
            raise SystemExit("HandleWebAdminPage_ sends no Content-Security-Policy header this check can read")
        text = literal.group(1)
    policy = {}
    for directive in text.split(";"):
        words = directive.split()
        if words:
            policy[words[0]] = words[1:]
    return policy


def check_trusted_types(script):
    """The page makes its default policy before any view draws, and no view builds what it refuses."""
    problems = []
    block = TRUSTED_TYPES_BLOCK.search(script)
    if not block:
        return ["the script makes no Trusted Types policy (the block headed '---------- Trusted Types ----------'); "
                "under require-trusted-types-for 'script' every view would be refused"]
    first_sink = script.find(".innerHTML")
    made = script.find('trustedTypes.createPolicy("default"')
    if made < 0 or made > block.end() or (0 <= first_sink < made):
        problems.append("the script does not make its default policy in its Trusted Types block, before any view draws")
    if script.count("createPolicy(") != 1:
        problems.append("the script makes %d Trusted Types policies; it makes one, named default" % script.count("createPolicy("))
    never = NEVER_PATTERN.search(block.group(0))
    if not never:
        problems.append("the Trusted Types block has no NEVER pattern this check can read")
        return problems
    refused = re.compile(never.group("pattern"), re.I)
    rest = script[:block.start()] + script[block.end():]
    for hit in sorted(set(m.group(0).lower() for m in refused.finditer(rest))):
        problems.append("the script builds %s, which its Trusted Types policy refuses: the view would not be drawn "
                        "in a browser that has Trusted Types" % hit)
    return problems


def check_headers(source):
    """The headers beside the policy that HandleWebAdminPage_ must send."""
    handler = handler_text(source)
    problems = []
    for header, why in (("X-Frame-Options: DENY", "frame-ancestors 'none' for a browser that reads only the older header"),
                        ("Cross-Origin-Opener-Policy: same-origin", "no page of another site keeps a handle on the Deck's window"),
                        ("X-Content-Type-Options: nosniff", "the page is what its type says")):
        if '"%s\\r\\n"' % header not in handler:
            problems.append("HandleWebAdminPage_ no longer sends %s (%s)" % (header, why))
    return problems


def check_markup(page, policy):
    problems = []
    scripts = list(SCRIPT.finditer(page))
    if len(scripts) != 1:
        problems.append("the page has %d script blocks; the Deck is one inline script, and the server allows exactly one "
                        "by its hash ('none' for any other count)" % len(scripts))
    for block in scripts:
        if re.search(r"(?:^|[\s/])src\s*=", block.group("attrs"), re.I):
            problems.append("the page loads a script from a file, which the policy's script-src (%s) does not allow"
                            % " ".join(policy.get("script-src", [])))
        if not block.group("body"):
            problems.append("the page's script block is empty; the server serves such a page under script-src 'none'")
        # A browser leaves script data only at the first "</script" - unless the
        # text holds "<!--" and then "<script", which makes it read on past that
        # "</script". The server's hash would then be of less text than the
        # browser runs, and the Deck would not start.
        if "<!--" in block.group("body"):
            problems.append("the script contains '<!--'; a browser may then not end the script where the server's hash "
                            "does, and refuse the Deck's own script (write it '<!' + '--')")
    script_src = policy.get("script-src", [])
    if "'unsafe-inline'" in script_src:
        problems.append("the policy allows inline script as such (script-src %s); the page's own script is allowed by its "
                        "hash and nothing else may run" % " ".join(script_src))
    if "'self'" in script_src:
        problems.append("the policy's script-src names 'self' (%s); this origin serves files whose type a sender or an "
                        "account chose, so a script file of this origin is not a script of this page" % " ".join(script_src))
    if script_src != [COMPUTED]:
        problems.append("the policy's script-src is not the page's script hash, computed by the server from the page it "
                        "serves (%s); it is %s" % (SCRIPT_SOURCE_CALL, " ".join(script_src) or "missing"))
    if "javascript:" in page.lower():
        problems.append("the page names a javascript: address, which the policy refuses to run")
    if re.search(r"\beval\s*\(|\bnew\s+Function\s*\(", page):
        problems.append("the page calls eval or new Function, which the policy refuses without 'unsafe-eval'")
    if policy.get("require-trusted-types-for") != ["'script'"]:
        problems.append("the policy does not say require-trusted-types-for 'script'; without it the page's Trusted Types "
                        "policy is never asked")
    if policy.get("trusted-types") != ["default"]:
        problems.append("the policy's trusted-types is not default alone (%s); the page makes one policy, named default"
                        % " ".join(policy.get("trusted-types", [])))
    problems += check_trusted_types(scripts[0].group("body") if len(scripts) == 1 else "")
    if "'unsafe-inline'" not in policy.get("style-src", []):
        problems.append("the policy no longer allows the inline style the page is made of")

    markup = SCRIPT.sub("", page)
    if re.search(r"<(link|iframe|frame|object|embed|base)\b", markup, re.I):
        problems.append("the page carries a link, frame, object, embed or base element; the policy allows none of them")
    for remote in re.findall(r"""(?:src|href|action)\s*=\s*["'](?:https?:)?//[^"']*""", markup, re.I):
        problems.append("the page names a remote resource (%s)" % remote)
    for remote in re.findall(r"url\(\s*['\"]?(?:https?:)?//[^)]*\)", markup, re.I):
        problems.append("the style names a remote resource (%s)" % remote)
    if re.search(r"@import\b", markup, re.I):
        problems.append("the style imports another sheet, which style-src forbids")
    for handler in re.findall(r"<[^>]*\s(on\w+)\s*=", markup, re.I):
        problems.append("the markup carries an inline event handler (%s); listeners are attached in the script" % handler)
    if re.search(r"""<form\b[^>]*\baction\s*=""", markup, re.I):
        problems.append("a form names an action; the page posts through fetch to this origin only")

    allowed = set(policy.keys())
    for directive, sources in policy.items():
        for source in sources:
            if source.startswith("http") or source.startswith("//") or source == "*":
                problems.append("the policy allows a remote source (%s %s)" % (directive, source))
    for needed in ("connect-src", "frame-ancestors", "base-uri", "default-src"):
        if needed not in allowed:
            problems.append("the policy names no %s" % needed)
    if policy.get("connect-src") != ["'self'"]:
        problems.append("connect-src is not 'self' alone (%s); the page's API calls are same-origin" % " ".join(policy.get("connect-src", [])))
    if policy.get("frame-ancestors") != ["'none'"]:
        problems.append("frame-ancestors is not 'none'; the page must not be framed")
    if "'none'" not in policy.get("default-src", []):
        problems.append("default-src is not 'none'")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keep", metavar="DIR", help="write the extracted page and script here and keep them")
    parser.add_argument("--extract-only", action="store_true", help="extract, check the markup, and do not run node")
    args = parser.parse_args()

    for path in (PAGE, SERVER, RUNNER, TRUSTED_TYPES):
        if not os.path.exists(path):
            raise SystemExit("not found: %s (run this from the repository root)" % path)

    page = read(PAGE)
    server = read(SERVER)
    policy = policy_for_page(server)
    problems = check_markup(page, policy) + check_headers(server)
    if problems:
        for problem in problems:
            sys.stderr.write("index.html: %s\n" % problem)
        return 1

    script = SCRIPT.search(page).group("body")
    print("index.html: one inline script of %d characters, served under script-src %s"
          % (len(script), script_source(script)), flush=True)

    where = args.keep or tempfile.mkdtemp(prefix="deck-script-")
    os.makedirs(where, exist_ok=True)
    page_copy = os.path.join(where, "index.html")
    code = os.path.join(where, "deck.js")
    with open(page_copy, "w", encoding="utf-8", newline="") as handle:
        handle.write(page)
    with open(code, "w", encoding="utf-8", newline="") as handle:
        handle.write(script)

    if args.extract_only:
        print("extracted %d bytes of page and %d bytes of script into %s" % (len(page), len(script), where))
        return 0

    node = shutil.which("node") or shutil.which("node.exe")
    if not node:
        sys.stderr.write("node is not on PATH; it runs the Deck's script. Install node, or pass --extract-only.\n")
        return 1

    try:
        run = subprocess.run([node, RUNNER, page_copy, code], cwd=ROOT)
        if run.returncode == 0:
            # An argument list and no shell: node as PATH finds it on the machine that runs the
            # check, the checker's own script under build/, a constant and the page's copy in the
            # folder made above. Nothing is parsed as a command line. (Semgrep takes whatever
            # follows "nosemgrep:" as rule ids, so the reason is here.)
            run = subprocess.run([node, TRUSTED_TYPES, "deck", page_copy], cwd=ROOT)  # nosemgrep
    finally:
        if not args.keep:
            shutil.rmtree(where, ignore_errors=True)
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
