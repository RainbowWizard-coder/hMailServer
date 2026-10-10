#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Control Panel's margins and paddings stay on the spacing scale.

docs/ControlPanelDesign.md gives the Control Panel one spacing scale - 4, 8, 12, 16,
24 and 32 pixels (DesignTokens.Space) - and the named Thickness tokens built from it
(AppPagePadding, AppCardPadding, AppListCardPadding, AppCardGap...). The project's own
review of the Control Panel (finding F295, 10 October 2026) counted 189 of 682 literal
margins and paddings off that scale, the scale's own tokens with no consumer in XAML,
and the page and card paddings themselves on a seventh step, 20; a page that needs a
seventh step is a page whose layout has gone wrong, and nothing said so.

Every Margin and Padding written as numbers in the Control Panel's XAML and C# is
read: a XAML attribute (Margin="0,0,8,0"), a style's Setter of either
(<Setter Property="Padding" Value="24,20,24,20" />, as DialogFrame's inset was), and
a C# `Margin = new Thickness(...)` or
`Padding = new Thickness(...)` whose arguments include a number literal
(DesignTokens.Space.Md and the other names are on the scale by definition). A number
off the scale - not 0 and not 4, 8, 12, 16, 24 or 32, either way round - fails,
unless the baseline holds it.

The baseline, build/spacing-baseline.txt, is the off-scale literals the tree held when
this check came in, file by file and literal by literal with how many times each is
written: the optical nudges that align a mark with a line of text (Margin="0,3,6,0"),
a pill's inner padding, and the rest still to be put on the scale. It may only
shrink. A literal the baseline does not hold, or more of one than it holds, fails;
one the tree no longer has as often as the baseline says fails too, so that the
baseline is lowered with the change that removed it - run with --write, which lowers
and removes entries and never adds one. A new off-scale value that is needed is
added to the baseline by hand, in the commit that needs it, where a reviewer sees it.

Branches written before the check came in, folded into one, bring off-scale literals
the baseline has never seen and take away some it holds. --rebaseline writes the
baseline as the tree has it, adding as well as removing: the one who folds runs it and
reads the baseline's diff, each added line an off-scale value to accept or to send
back to its branch. It is the fold's tool, never a change's.

  python build/check-spacing.py [--write | --rebaseline] [--self-test]
"""
import os
import re
import sys
from collections import Counter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SOURCE = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel")
BASELINE = os.path.join(ROOT, "build", "spacing-baseline.txt")
SCALE = {0, 4, 8, 12, 16, 24, 32}

XAML_ATTR = re.compile(r'\b(Margin|Padding)="([^"{}]+)"')
XAML_SETTER = re.compile(r'<Setter\b[^>]*?\bProperty="(Margin|Padding)"[^>]*?\bValue="([^"{}]+)"')
CS_THICKNESS = re.compile(r'\b(Margin|Padding)\s*=\s*new\s+Thickness\(([^()]*)\)')
NUMBER = re.compile(r'(?<![\w.])-?\d+(?:\.\d+)?(?![\w.])')
SKIP_DIRS = {"bin", "obj", "Properties", "Resources"}

HEADER = """# build/check-spacing.py's baseline: the Control Panel's margins and paddings written off the spacing
# scale (4, 8, 12, 16, 24, 32 and 0), as the tree held them when the check came in on 10 October 2026 - the
# optical nudges that align a mark with a line of text, a pill's inner padding, and the rest still to be put
# on the scale. One line per file and literal: the path under hmailserver/source/Tools/ControlPanel, the
# literal as written, and how many times the file writes it. It may only shrink: `--write` lowers and removes
# entries; a new one is added by hand, with the change that needs it.
"""


def off_scale(values):
    return [v for v in values if abs(v) not in SCALE]


def literals(name, text):
    """(kind, literal as written, off-scale numbers) for every Margin and Padding written as numbers."""
    found = []
    if name.endswith(".xaml"):
        for pattern, kind in ((XAML_ATTR, ""), (XAML_SETTER, "Setter ")):
            for m in pattern.finditer(text):
                raw = m.group(2).strip()
                parts = [p for p in re.split(r"[ ,]+", raw) if p]
                try:
                    values = [float(p) for p in parts]
                except ValueError:
                    continue
                found.append((kind + m.group(1) + "=" + raw, off_scale(values)))
    else:
        for m in CS_THICKNESS.finditer(text):
            args = " ".join(m.group(2).split())
            values = [float(n) for n in NUMBER.findall(args)]
            if not values:
                continue
            found.append((m.group(1) + "=" + args, off_scale(values)))
    return [(literal, bad) for literal, bad in found if bad]


def scan(source):
    counts = Counter()
    for directory, dirs, files in os.walk(source):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            if not name.endswith((".xaml", ".cs")) or name.endswith((".g.cs", ".g.i.cs")):
                continue
            path = os.path.join(directory, name)
            rel = os.path.relpath(path, source).replace(os.sep, "/")
            with open(path, encoding="utf-8-sig") as handle:
                text = handle.read()
            for literal, _ in literals(name, text):
                counts[(rel, literal)] += 1
    return counts


def read_baseline(path):
    held = Counter()
    if not os.path.exists(path):
        return held
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            rel, literal, count = line.split("\t")
            held[(rel, literal)] = int(count)
    return held


def write_baseline(path, held):
    # The ending the file has - CRLF in a Windows checkout (.gitattributes), LF on Linux.
    ending = "\n"
    if os.path.exists(path):
        with open(path, "rb") as handle:
            if b"\r\n" in handle.read():
                ending = "\r\n"
    with open(path, "w", encoding="utf-8", newline=ending) as handle:
        handle.write(HEADER)
        for (rel, literal), count in sorted(held.items()):
            if count > 0:
                handle.write(f"{rel}\t{literal}\t{count}\n")


def problems_of(tree, held):
    out = []
    for key in sorted(set(tree) | set(held)):
        have, may = tree.get(key, 0), held.get(key, 0)
        rel, literal = key
        if have > may:
            out.append(f"{rel}: {literal} is off the spacing scale (4, 8, 12, 16, 24, 32) and written {have} time(s), "
                       f"where the baseline holds {may}: use a token (AppCardGap, DesignTokens.Space.Md...) or a value "
                       f"on the scale, or, for an optical nudge that must stay, add it to build/spacing-baseline.txt")
        elif have < may:
            out.append(f"{rel}: {literal} is written {have} time(s), where the baseline holds {may}: lower the baseline "
                       f"(python build/check-spacing.py --write)")
    return out


def self_test():
    import tempfile
    failures = 0
    with tempfile.TemporaryDirectory() as temp:
        os.makedirs(os.path.join(temp, "Views"))
        with open(os.path.join(temp, "Views", "Page.xaml"), "w", encoding="utf-8") as handle:
            handle.write('<Grid Margin="0,0,8,0" Padding="{StaticResource AppCardPadding}">\n'
                         '  <TextBlock Margin="0,3,6,0" />\n  <Border Padding="24" Margin="-4,0" />\n</Grid>\n'
                         '<Style><Setter Property="Padding" Value="24,20,24,20" />\n'
                         '  <Setter TargetName="Bd" Property="Margin" Value="0,12" />\n'
                         '  <Setter Property="Padding" Value="{StaticResource AppDialogPadding}" /></Style>\n')
        with open(os.path.join(temp, "Views", "Page.cs"), "w", encoding="utf-8") as handle:
            handle.write("var a = new Border { Margin = new Thickness(0, DesignTokens.Space.Sm, 10, 0) };\n"
                         "b.Padding = new Thickness(DesignTokens.Space.Md);\n"
                         "c.Margin = new Thickness(12, 0, 0, 16);\n")
        tree = scan(temp)
        expect = {("Views/Page.xaml", "Margin=0,3,6,0"): 1, ("Views/Page.xaml", "Setter Padding=24,20,24,20"): 1,
                  ("Views/Page.cs", "Margin=0, DesignTokens.Space.Sm, 10, 0"): 1}
        if dict(tree) != expect:
            print("self-test: the scan read", dict(tree), "where", expect, "was written")
            failures += 1
        if not problems_of(tree, Counter()):
            print("self-test: three off-scale literals and an empty baseline passed")
            failures += 1
        if problems_of(tree, Counter(expect)):
            print("self-test: the literals the baseline holds failed")
            failures += 1
        lowered = Counter(expect)
        lowered[("Views/Page.xaml", "Margin=0,3,6,0")] = 2
        if not problems_of(tree, lowered):
            print("self-test: a baseline holding more than the tree has passed")
            failures += 1
        grown = Counter(expect)
        grown[("Views/Page.cs", "Margin=0, DesignTokens.Space.Sm, 10, 0")] = 0
        if not problems_of(tree, grown):
            print("self-test: a literal written more often than the baseline holds passed")
            failures += 1
        # --rebaseline's write, read back: the tree's literals exactly, and the file's CRLF kept.
        baseline = os.path.join(temp, "baseline.txt")
        with open(baseline, "wb") as handle:
            handle.write(b"# old\r\nViews/Gone.xaml\tMargin=0,3,0,0\t2\r\n")
        write_baseline(baseline, tree)
        if read_baseline(baseline) != tree or problems_of(tree, read_baseline(baseline)):
            print("self-test: a rebaselined file read back", dict(read_baseline(baseline)), "where", dict(tree), "was written")
            failures += 1
        with open(baseline, "rb") as handle:
            written = handle.read()
        if b"\r\n" not in written or written.replace(b"\r\n", b"").count(b"\n"):
            print("self-test: the baseline's CRLF was not kept")
            failures += 1
    print(f"self-test: {failures} failure(s)")
    return 1 if failures else 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if "--self-test" in sys.argv[1:]:
        return self_test()
    tree = scan(SOURCE)
    held = read_baseline(BASELINE)
    if "--rebaseline" in sys.argv[1:]:
        write_baseline(BASELINE, tree)
        held = read_baseline(BASELINE)
    elif "--write" in sys.argv[1:]:
        lowered = Counter({key: min(count, tree.get(key, 0)) for key, count in held.items()})
        write_baseline(BASELINE, lowered)
        held = lowered
    problems = problems_of(tree, held)
    for problem in problems:
        print("FAIL  " + problem)
    total = sum(tree.values())
    if problems:
        print(f"FAIL: {len(problems)} problem(s)")
        return 1
    print(f"OK    every Margin and Padding written as numbers is on the spacing scale or in the baseline "
          f"({total} off-scale literal(s) in {len({rel for rel, _ in tree})} file(s), none new)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
