# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Exactly one comma between every two OpenAPI blocks, and every block joined.

/api/v1/openapi.json is built in RestApiServer.cpp by appending the text of
one OpenApi<Feature>Paths_() function per feature, in a list. Each function's
text is a run of `"/api/v1/...":{...}` path objects; the comma between one
block's last path and the next block's first is carried by one of the two -
some blocks begin with it, some end with it - and the joining code adds none.
A block written as the last of its own branch, after one that ended with a
comma, carries none itself; the fold that puts a different block before it
makes a document that is not JSON, which is what the full gate of
18 September 2026 found twice at once - the shared address books block and
the OpenPGP keys block each following another block's closing brace as
`}"/api/`, and six fixtures that parse the document refusing it. A build
cannot catch this; only reading the document can, and by then it is a bench
twenty minutes in.

So this reads the join list in RestApiServer.cpp and, for every two blocks
that are joined one after the other, the end of the first's text and the start
of the second's, and fails unless exactly one of them carries the comma. A
block whose text is built by a helper rather than written as literals cannot
be read here and is counted as unread, so the number is honest. It also fails
when a block is defined and not joined, or joined and not defined.

Usage:  python build/check-openapi-blocks.py
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# The tree to read; the environment names another for the checker's own
# negative control (RELEASE.md step 3).
UTIL = os.environ.get("HM_OPENAPI_BLOCKS_DIR") or os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util")

DEFINE = re.compile(r"RestApiServer::(OpenApi\w+Paths_)\s*\(\)\s*\n\s*\{", re.S)
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
JOIN = re.compile(r"openApiJson\s*\+=\s*(OpenApi\w+Paths_)\s*\(\s*\)")
COMMENT = re.compile(r"//[^\n]*")


def literal_run(body):
    """(first, last) of a block written as one run of literals after `return`
    or `static const char *paths =`, or None when it is built another way."""
    text = COMMENT.sub("", body)
    m = re.search(r"(?:return|static\s+const\s+char\s*\*\s*\w+\s*=)\s*(\"(?:[^\"\\]|\\.)*\"(?:\s*\"(?:[^\"\\]|\\.)*\")*)\s*;", text, re.S)
    if not m:
        return None
    literals = LITERAL.findall(m.group(1))
    if not literals:
        return None
    return literals[0], literals[-1]


def main():
    problems = []
    unread = []
    blocks = {}
    for path in sorted(glob.glob(os.path.join(UTIL, "RestApi*.cpp"))):
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            text = f.read()
        for m in DEFINE.finditer(text):
            name = m.group(1)
            end = text.find("\n   }", m.end())
            body = text[m.end():end if end > 0 else m.end() + 200000]
            relative = os.path.relpath(path, ROOT).replace(os.sep, "/")
            where = "%s:%d" % (relative, text.count("\n", 0, m.start()) + 1)
            blocks[name] = (where, literal_run(body))

    server = os.path.join(UTIL, "RestApiServer.cpp")
    joined = []
    if os.path.exists(server):
        with open(server, encoding="utf-8-sig", errors="replace") as f:
            joined = JOIN.findall(f.read())

    for name in blocks:
        if name not in joined:
            problems.append("%s %s is defined and never joined into the document" % (blocks[name][0], name))
    for name in joined:
        if name not in blocks:
            problems.append("RestApiServer.cpp joins %s, which no file defines" % name)

    pairs = 0
    for before, after in zip(joined, joined[1:]):
        if before not in blocks or after not in blocks:
            continue
        run_before, run_after = blocks[before][1], blocks[after][1]
        if run_before is None or run_after is None:
            unread.append("%s -> %s" % (before, after))
            continue
        pairs += 1
        trailing = run_before[1].rstrip().endswith(",")
        leading = run_after[0].lstrip().startswith(",")
        if trailing == leading:
            problems.append("%s %s ends %s and %s (%s) begins %s: %s"
                            % (blocks[after][0], before, "with a comma" if trailing else "without a comma",
                               after, blocks[after][0].split(":")[0], "with one" if leading else "without one",
                               "two commas make an empty member" if trailing else "no comma makes `}\"/api/`, which is not JSON"))

    if problems:
        for problem in problems:
            sys.stdout.write("  FAIL  %s\n" % problem)
        sys.stdout.write("\n%d problem(s): a block without its comma makes /api/v1/openapi.json not JSON the moment "
                         "another block is joined before it.\n" % len(problems))
        return 1
    sys.stdout.write("  OK    %d OpenAPI block(s) joined in order, %d join(s) read with exactly one comma between the two "
                     "(%d built by a helper, not read)\n" % (len(joined), pairs, len(unread)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
