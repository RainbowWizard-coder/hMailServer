#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The counter at the top of Roadmap2.md and of Roadmap3.md says what the rows say.

Roadmap2.md is the plan after 6.3.2. Roadmap3.md is the plan for getting back,
on GitLab, everything the project had on GitHub, and for reaching every other
channel people install mail servers from. Each opens with one line that counts
its rows by state - done, in motion, not started, paused - so that anybody
opening the file sees where the programme stands without scrolling. A counter
that is maintained by hand drifts the first time a row is flipped and the line
is not, which is why every fold and every branch that flips a row runs this.
The rows are the truth; the line is a summary of them.

The script keeps the name it had while Roadmap2.md was the only such file,
because the contributing guide, the merge request template and the roadmaps
themselves name it. It holds every file in ROADMAPS, and fails when one of
them is missing, so a roadmap cannot drop out of the check unnoticed.

The line has a fixed shape, found by its opening words:

    **Where it stands:** 52 done, 31 in motion, 88 not started, 4 paused.

A row is counted by the mark in its first cell: the check mark is done, the
arrows are in motion, the white square is not started, and the pause sign is
paused. Rows in any table anywhere in the file count; nothing else does.

Usage:
  python build/check-roadmap2.py              # every roadmap; exits 1 with the disagreement
  python build/check-roadmap2.py Roadmap3.md  # the one named
  python build/check-roadmap2.py --write      # rewrites each line to match its rows
  python build/check-roadmap2.py --self-test  # the negative controls, in a scratch directory
"""

import io
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Every roadmap that opens with a counter line, by its name at the repository root.
ROADMAPS = ["Roadmap2.md", "Roadmap3.md"]

# Written as code points so that the script behaves the same whichever encoding
# the host thinks the file has. The pause sign carries U+FE0F in the document.
MARKS = [
    ("done", "✅"),
    ("in motion", "\U0001F504"),
    ("not started", "⬜"),
    ("paused", "⏸️"),
]

PREFIX = "**Where it stands:**"
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
    return "%s %d done, %d in motion, %d not started, %d paused." % ((PREFIX,) + tuple(counts))


def check_one(root, name, write, say):
    """0 when the file's counter says what its rows say (or --write made it so), 1 otherwise."""
    path = os.path.join(root, name)
    if not os.path.isfile(path):
        say("%s: not found at %s; every roadmap in ROADMAPS must exist." % (name, path))
        return 1
    with io.open(path, encoding="utf-8", newline="") as handle:
        text = handle.read()
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    wanted = render(counts_of(text.replace("\r\n", "\n")))

    found = [i for i, line in enumerate(lines) if line.startswith(PREFIX)]
    if len(found) != 1:
        say("%s must carry exactly one line beginning '%s' near the top; found %d." % (name, PREFIX, len(found)))
        return 1

    current = lines[found[0]]
    if current == wanted:
        say("%s: %s" % (name, wanted.replace("**", "")))
        return 0

    if write:
        lines[found[0]] = wanted
        with io.open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(newline.join(lines))
        say("%s: the counter now says what the rows say - %s" % (name, wanted.replace("**", "")))
        return 0

    say("%s: the counter disagrees with the rows." % name)
    say("  the line says: %s" % current.replace("**", ""))
    say("  the rows say:  %s" % wanted.replace("**", ""))
    if not LINE.match(current):
        say("  (the line is not in the expected shape; run with --write to rewrite it)")
    else:
        say("  run: python build/check-roadmap2.py --write")
    return 1


def run(arguments, root=ROOT, say=print):
    write = "--write" in arguments
    names = [a for a in arguments if not a.startswith("--")]
    unknown = [a for a in arguments if a.startswith("--") and a != "--write"]
    if unknown:
        say("unknown option %s" % " ".join(unknown))
        return 2
    for name in names:
        if name not in ROADMAPS:
            say("%s is not a roadmap this script holds; it holds %s." % (name, ", ".join(ROADMAPS)))
            return 2
    failed = 0
    for name in names or ROADMAPS:
        failed |= check_one(root, name, write, say)
    return failed


# --- the negative controls ---------------------------------------------------

def _document(counter, rows, newline="\n"):
    """A roadmap in the files' shape: a heading, the counter, prose and one table."""
    lines = ["# A roadmap", "", "The legend: ✅ done, \U0001F504 underway.", ""]
    if counter is not None:
        lines += [counter, ""]
    lines += ["| | Item | Detail |", "|---|---|---|"]
    lines += ["| %s | **Row %d** | Detail with a ✅ in it, which is not the first cell. |" % (mark, n)
              for n, mark in enumerate(rows)]
    lines += ["", "✅ a mark at the start of prose, which is no row.", ""]
    return newline.join(lines)


def self_test():
    failures = []

    def expect(condition, what):
        print("  %s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    done, moving, todo = "✅", "\U0001F504", "⬜"
    paused, paused_bare = "⏸️", "⏸"
    rows2 = [done, done, moving, todo, paused]
    rows3 = [done, moving, moving, todo, todo, todo, paused_bare]
    right2 = render([2, 1, 1, 1])
    right3 = render([1, 2, 3, 1])

    expect(counts_of(_document(right2, rows2)) == [2, 1, 1, 1],
           "the rows are counted by their first cell only, the pause sign with its U+FE0F")
    expect(counts_of(_document(right3, rows3)) == [1, 2, 3, 1],
           "a pause sign without U+FE0F counts as paused too")
    expect(counts_of("| Item | ✅ |\n✅ prose\n  | ✅ | indented |\n") == [0, 0, 0, 0],
           "a mark outside a row's first cell, in prose or in an indented line, is not a row")

    scratch = tempfile.mkdtemp(prefix="check-roadmap2-")
    try:
        def lay(text2, text3):
            for name, text in (("Roadmap2.md", text2), ("Roadmap3.md", text3)):
                path = os.path.join(scratch, name)
                if text is None:
                    if os.path.exists(path):
                        os.remove(path)
                    continue
                with io.open(path, "w", encoding="utf-8", newline="") as handle:
                    handle.write(text)

        def read(name):
            with io.open(os.path.join(scratch, name), encoding="utf-8", newline="") as handle:
                return handle.read()

        def outcome(*arguments):
            said = []
            code = run(list(arguments), root=scratch, say=said.append)
            return code, "\n".join(said)

        expect("Roadmap3.md" in ROADMAPS and "Roadmap2.md" in ROADMAPS,
               "the script holds Roadmap2.md and Roadmap3.md")

        lay(_document(right2, rows2), _document(right3, rows3))
        code, said = outcome()
        expect(code == 0 and "Roadmap2.md" in said and "Roadmap3.md" in said,
               "both counters right: passes, and names both files")

        # The control this script exists for: Roadmap3.md's counter is wrong and
        # Roadmap2.md's right. A check that read Roadmap2.md alone would pass.
        lay(_document(right2, rows2), _document(render([1, 2, 2, 1]), rows3))
        code, said = outcome()
        expect(code == 1 and "Roadmap3.md: the counter disagrees" in said,
               "Roadmap3.md's counter one row short: fails, naming Roadmap3.md")
        code, said = outcome("Roadmap2.md")
        expect(code == 0, "the same tree, Roadmap2.md alone named: passes")
        code, said = outcome("Roadmap3.md")
        expect(code == 1, "the same tree, Roadmap3.md alone named: fails")

        lay(_document(render([3, 1, 1, 1]), rows2), _document(right3, rows3))
        code, said = outcome()
        expect(code == 1 and "Roadmap2.md: the counter disagrees" in said,
               "Roadmap2.md's counter wrong: fails, naming Roadmap2.md")

        # A row flipped without the line: the check sees the flip.
        lay(_document(right2, rows2), _document(right3, [done] + rows3[1:-1] + [done]))
        code, said = outcome()
        expect(code == 1, "a row flipped from paused to done without the counter: fails")

        # --write rewrites the one line, keeps CRLF, and touches nothing else.
        before3 = _document(render([9, 9, 9, 9]), rows3, "\r\n")
        lay(_document("**Where it stands:** some rows.", rows2, "\r\n"), before3)
        code, said = outcome()
        expect(code == 1 and "not in the expected shape" in said,
               "a counter line in another shape: fails and says so")
        code, said = outcome("--write")
        after2, after3 = read("Roadmap2.md"), read("Roadmap3.md")
        expect(code == 0 and right2 in after2 and right3 in after3, "--write puts the rows' counts in both lines")
        expect("\n" not in after3.replace("\r\n", ""), "--write keeps CRLF line endings")
        expect(after3 == before3.replace(render([9, 9, 9, 9]), right3), "--write changes the counter line and nothing else")
        code, said = outcome()
        expect(code == 0, "after --write the check passes")

        lay(_document(right2, rows2), _document(None, rows3))
        code, said = outcome("--write")
        expect(code == 1 and "found 0" in said, "no counter line: fails, even with --write")
        lay(_document(right2, rows2), _document(right3, rows3).replace(right3, right3 + "\n" + right3))
        code, said = outcome("--write")
        expect(code == 1 and "found 2" in said, "two counter lines: fails, even with --write")

        lay(_document(right2, rows2), None)
        code, said = outcome()
        expect(code == 1 and "Roadmap3.md: not found" in said, "Roadmap3.md missing: fails")

        code, said = outcome("Roadmap4.md")
        expect(code == 2, "a file the script does not hold is refused")
        code, said = outcome("--wirte")
        expect(code == 2, "a mistyped option is refused, not taken as a check")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print("check-roadmap2.py --self-test: %d of its controls failed." % len(failures))
        return 1
    print("check-roadmap2.py --self-test: every control passed.")
    return 0


def main():
    arguments = sys.argv[1:]
    if "--self-test" in arguments:
        return self_test()
    return run(arguments)


if __name__ == "__main__":
    sys.exit(main())
