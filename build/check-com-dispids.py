# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""No two members of one COM interface carry the same dispatch id.

MIDL refuses a duplicate - "error MIDL2021: duplicate id" - so this is caught
by any build of the server, eventually. The reason for a checker is WHEN it is
caught: two branches that each add members to the same interface both start at
the next free id, neither is wrong on its own, and the collision exists only in
the tree that folds them. That tree is usually a batch about to be gated, and
the failure arrives twenty minutes into a bench build rather than in a second
here. It happened on 18 September 2026 to `IInterfaceAntiSpam`, where one row
took 47 to 50 and another took 47 to 56.

A property's getter and setter share an id, by design, and so do the members of
different interfaces: the id is only unique within its own interface. So this
counts distinct MEMBER NAMES per (interface, id) and complains when there is
more than one.

Usage:  python build/check-com-dispids.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IDL = os.path.join(ROOT, "hmailserver", "source", "Server", "hMailServer", "hMailServer.idl")

# `interface IInterfaceFoo : IDispatch` opens one; the brace that closes it is
# the first at its own indentation, which in this file is always "};" with the
# same leading spaces as the `interface` line.
OPENS = re.compile(r"^(\s*)interface\s+(\w+)\s*:")
# Anything may sit between the id and the name - the attribute list, and a
# helpstring which is prose and may hold any punctuation, semicolons included.
# One member is one line here, so the pair is unambiguous without excluding
# characters: excluding the semicolon was this checker's own first defect, and
# it silently skipped every member whose help text used one.
MEMBER = re.compile(r"\bid\((\d+)\).*?\bHRESULT\s+(\w+)\s*\(")


def main():
    if not os.path.exists(IDL):
        sys.stdout.write("  FAIL  %s: not found\n" % IDL)
        return 1

    with open(IDL, encoding="utf-8-sig", errors="replace") as f:
        lines = f.read().split("\n")

    seen = {}          # (interface, id) -> {name: first line}
    interface = None
    indent = None
    problems = []
    members = 0

    for number, line in enumerate(lines, 1):
        opening = OPENS.match(line)
        if opening:
            indent, interface = opening.group(1), opening.group(2)
            continue
        if interface is not None and line.strip() == "};" and line[:len(indent)] == indent:
            interface = None
            continue
        if interface is None:
            continue
        found = MEMBER.search(line)
        if not found:
            continue
        members += 1
        identifier, name = int(found.group(1)), found.group(2)
        names = seen.setdefault((interface, identifier), {})
        if name not in names:
            names[name] = number
        if len(names) == 2 and len(names) > 1:
            first = [n for n in names if n != name][0]
            problems.append("%s: id(%d) is %s (line %d) and %s (line %d)"
                            % (interface, identifier, first, names[first], name, number))

    if problems:
        for problem in problems:
            sys.stdout.write("  FAIL  %s\n" % problem)
        sys.stdout.write("\n%d duplicate dispatch id(s). MIDL refuses these, so the server does not build; "
                         "give the members that arrived later the next free ids of their interface.\n" % len(problems))
        return 1

    sys.stdout.write("  OK    %d interface member(s) across %d interface(s): every dispatch id is its interface's own\n"
                     % (members, len(set(key[0] for key in seen))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
