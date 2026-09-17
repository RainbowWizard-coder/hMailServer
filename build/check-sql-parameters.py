# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A placeholder written twice needs two parameters, and in order.

SQL Server and SQL Server Compact substitute parameters by POSITION, not by
name: the provider takes the values in the order they were added and puts them
where the question marks fall. Two consequences, both of which have shipped in
this tree:

  - a statement that writes one placeholder name twice ("where mtsender like
    @ADDRESS or mtrecipient like @ADDRESS") and adds one value leaves the second
    without one - the search answers nothing, or the provider refuses the
    statement (MessageTrace::Search, found 17 September 2026);
  - a statement whose values are added in a different order from the
    placeholders' silently puts each value in the wrong column - an epoch into an
    int column and a score into a text one, which SQL Compact refuses with
    "Requested conversion is not supported" and MySQL, which does substitute by
    name, accepts (CompromiseDetector::WriteShape, found the same evening).

This reads every SQLCommand built with a literal statement and the AddParameter
calls that follow it, and fails when a placeholder is written more often than a
value is added for it, or when the order of the values does not follow the order
of the placeholders. A statement built from pieces at run time, or whose
parameters are added in a loop or behind a condition, is beyond what reading can
decide: those are listed as unread so the number is honest.

Usage:  python build/check-sql-parameters.py
"""
import collections
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")

PLACEHOLDER = re.compile(r"@[A-Z][A-Z_0-9]*")
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
COMMAND = re.compile(r"\bSQLCommand\s+(\w+)\s*\(")
ADD = re.compile(r"(\w+)\.AddParameter\(\s*\"(@[A-Z][A-Z_0-9]*)\"")


def statement_of(text, at):
    """The literal text of the SQLCommand constructor starting at `at`, or None
    when it is built from anything but string literals."""
    depth = 0
    i = at
    while i < len(text):
        c = text[i]
        if c == '"':
            m = STRING.match(text, i)
            if not m:
                return None
            i = m.end()
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[at:i + 1]
        elif c == ";" and depth == 0:
            return None
        i += 1
    return None


def main():
    problems = []
    unread = []
    read = 0

    for dirpath, _, files in os.walk(SERVER):
        for name in sorted(files):
            if not name.endswith(".cpp"):
                continue
            # The substitution engine's own tester writes statements that break
            # this rule on purpose - a name used twice, values added out of
            # order, a token that is not a parameter at all - to prove what
            # SQLStatement does with them. They never reach a provider.
            if name in ("SQLStatement.cpp", "ClassTester.cpp") or name.endswith("Tester.cpp"):
                continue
            path = os.path.join(dirpath, name)
            relative = os.path.relpath(path, ROOT).replace(os.sep, "/")
            text = io.open(path, encoding="utf-8", errors="replace").read()

            for m in COMMAND.finditer(text):
                variable = m.group(1)
                line = text.count("\n", 0, m.start()) + 1
                body = statement_of(text, m.end() - 1)
                if body is None:
                    continue

                # The statement, as the provider will see it: every literal in
                # the constructor, joined. A conditional statement (a ternary of
                # two literals) gives both, and a placeholder that appears in
                # each is then counted twice - so those are read separately.
                literals = STRING.findall(body)
                if not literals:
                    continue
                branches = [ "".join(literals) ]
                if "?" in body and ":" in body and len(literals) > 1:
                    unread.append("%s:%d (%s: a statement chosen at run time)" % (relative, line, variable))
                    continue
                if "+" in body and len(literals) > 1:
                    unread.append("%s:%d (%s: a statement built from pieces)" % (relative, line, variable))
                    continue

                written = PLACEHOLDER.findall(branches[0])
                if not written:
                    continue
                read += 1

                # The AddParameter calls for this variable, in order, up to the
                # next SQLCommand of any name.
                rest = text[m.end():]
                nxt = COMMAND.search(rest)
                region = rest[:nxt.start()] if nxt else rest
                added = [p for v, p in ADD.findall(region) if v == variable]

                if "for (" in region.split("AddParameter")[0][-400:] or "while (" in region.split("AddParameter")[0][-400:]:
                    unread.append("%s:%d (%s: parameters added in a loop)" % (relative, line, variable))
                    continue

                counts_written = collections.Counter(written)
                counts_added = collections.Counter(added)
                for placeholder, n in sorted(counts_written.items()):
                    if counts_added.get(placeholder, 0) < n:
                        problems.append("%s:%d %s writes %s %d time(s) and adds %d value(s) for it - the provider substitutes by position, so the last one gets nothing"
                                        % (relative, line, variable, placeholder, n, counts_added.get(placeholder, 0)))

                if sorted(counts_added.items()) == sorted(counts_written.items()) and added != written:
                    problems.append("%s:%d %s adds its values in the order %s but writes them in the order %s - by position, each value lands in the wrong place"
                                    % (relative, line, variable, " ".join(added), " ".join(written)))

    if problems:
        for p in problems:
            sys.stdout.write("  FAIL  %s\n" % p)
        sys.stdout.write("\n%d problem(s). By position, not by name: a value in the wrong place is a column written with another column's value, and the backends disagree about whether that is an error.\n" % len(problems))
        return 1

    sys.stdout.write("  OK    %d statement(s) read: every placeholder has its own value, added in the order it is written (%d built at run time, not read)\n"
                     % (read, len(unread)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
