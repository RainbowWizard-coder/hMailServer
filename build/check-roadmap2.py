#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The counter at the top of Roadmap2.md says what the rows say.

Roadmap2.md is the plan after 6.3.2, and it opens with one line that counts its
rows by state - done, in motion, not started, paused - so that anybody opening
the file sees where the programme stands without scrolling. A counter that is
maintained by hand drifts the first time a row is flipped and the line is not,
which is why every fold and every branch that flips a row runs this. The rows
are the truth; the line is a summary of them.

The line has a fixed shape, found by its opening words:

    **Where it stands:** 52 done, 31 in motion, 88 not started, 4 paused.

A row is counted by the mark in its first cell: the check mark is done, the
arrows are in motion, the white square is not started, and the pause sign is
paused. Rows in any table anywhere in the file count; nothing else does.

Usage:
  python build/check-roadmap2.py           # exits 1 with the disagreement
  python build/check-roadmap2.py --write   # rewrites the line to match the rows
"""

import io
import os
import re
import sys

ROADMAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Roadmap2.md")

# Written as code points so that the script behaves the same whichever encoding
# the host thinks the file has. The pause sign carries U+FE0F in the document.
MARKS = [
    ("done", "✅"),
    ("in motion", "\U0001F504"),
    ("not started", "⬜"),
    ("paused", "⏸️"),
]

LINE = re.compile(r"^\*\*Where it stands:\*\* (\d+) done, (\d+) in motion, (\d+) not started, (\d+) paused\.\s*$")


def counts_of(text):
    counts = [0, 0, 0, 0]
    for line in text.split("\n"):
        match = re.match(r"^\|\s*(\S+)\s*\|", line)
        if not match:
            continue
        cell = match.group(1)
        for index, (_, mark) in enumerate(MARKS):
            if cell == mark or cell == mark.rstrip("️"):
                counts[index] += 1
                break
    return counts


def render(counts):
    return "**Where it stands:** %d done, %d in motion, %d not started, %d paused." % tuple(counts)


def main():
    write = "--write" in sys.argv[1:]
    with io.open(ROADMAP, encoding="utf-8", newline="") as handle:
        text = handle.read()
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    counts = counts_of(text.replace("\r\n", "\n"))
    wanted = render(counts)

    found = [i for i, line in enumerate(lines) if line.startswith("**Where it stands:**")]
    if len(found) != 1:
        print("Roadmap2.md must carry exactly one line beginning '**Where it stands:**' near the top; found %d." % len(found))
        return 1

    current = lines[found[0]]
    if current == wanted:
        print("Roadmap2.md: %s" % wanted.replace("**", ""))
        return 0

    if write:
        lines[found[0]] = wanted
        with io.open(ROADMAP, "w", encoding="utf-8", newline="") as handle:
            handle.write(newline.join(lines))
        print("Roadmap2.md: the counter now says what the rows say - %s" % wanted.replace("**", ""))
        return 0

    stated = LINE.match(current)
    print("Roadmap2.md: the counter disagrees with the rows.")
    print("  the line says: %s" % current.replace("**", ""))
    print("  the rows say:  %s" % wanted.replace("**", ""))
    if not stated:
        print("  (the line is not in the expected shape; run with --write to rewrite it)")
    else:
        print("  run: python build/check-roadmap2.py --write")
    return 1


if __name__ == "__main__":
    sys.exit(main())
