#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every loop over a shared collection iterates a locked copy.

Collection<T, P> (Common/BO/Collection.h) keeps its items in one vector behind a
mutex. GetCount, GetItem, AddItem, the deletes and the refresh all take that
mutex; GetVector() and GetConstVector() hand out a reference to the vector itself
and take nothing. A range-for over that reference, or a copy of it, while another
thread refreshes the collection (a swap, and the old buffer freed), adds to it (a
push_back that reallocates) or erases from it, is a read of freed memory - which
is what the heap-corruption abort of 15, 22 and 23 September 2026 was: the IMAP
special-use pass walking an account's shared folder tree through GetVector()
while other sessions created and deleted folders in it. GetSnapshot() is the
locked copy, and is what anything that another thread can reach must iterate.

Whether another thread can reach a collection is not something a script can
see, so the decision is written where the call is. The rule:

  Every call of GetVector() or GetConstVector() under hmailserver/source/Server
  carries the comment "// owned by this call" - on its own line, or on the line
  before it - saying why no other thread can touch that collection while the
  call runs (a local loaded above, a message's own recipients, a collection a
  getter builds new for every caller). The one exception is Collection.h, the
  class whose vector it is. Anything else fails here with the file and the line,
  and the fix is GetSnapshot() or the comment with its reason - never the
  comment alone because it is shorter.

What counts as a call: the name followed by an opening parenthesis in code, with
comments and string and character literals blanked first, that is not the
declaration of an accessor of that name. A declaration has a return type before
the name and "()" then an optional const and a brace or a semicolon after it -
MessageRecipients and Attachments are not Collections and declare their own.

The table of every site and its verdict is in ARCHITECTURE.md ("A collection
another thread can change is iterated as a locked copy").

Run from the repository root: python3 build/check-collection-iteration.py
Pass --self-test to run it against a temporary tree with one of each case.
"""
import os
import re
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Server")

MARKER = "// owned by this call"
EXEMPT = "Common/BO/Collection.h"
EXTENSIONS = (".cpp", ".h", ".hpp", ".inl", ".cc", ".cxx")
BUILD_OUTPUT = ("x64", "Win32", "Debug", "Release", ".vs", "obj", "bin", "ipch")

CALL = re.compile(r"\b(GetVector|GetConstVector)\s*\(")
DECLARATION_TAIL = re.compile(r"\s*\)\s*(?:const\s*)?(?:override\s*)?(?:noexcept\s*)?([{;])")
KEYWORDS = {"return", "throw", "else", "do", "case", "co_return", "co_yield", "sizeof", "decltype",
            "and", "or", "not", "new", "delete", "typeid", "alignof"}


def blank(text):
    """Comments, string literals and character literals blanked to spaces, with
    every newline and every length kept, so that an offset in the result is the
    same offset in the source."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j]) + text[j])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def is_declaration(code, match):
    """True when the name at match is being declared or defined, not called."""
    tail = DECLARATION_TAIL.match(code, match.end())
    if not tail:
        return False

    before = code[:match.start()].rstrip()

    # A qualified definition, Foo::GetVector() { ... }: the qualifier is not
    # what decides, the type in front of it is, and only a body makes it one.
    qualified = re.search(r"(?:\b\w+\s*::\s*)+$", before)
    if qualified:
        if tail.group(1) != "{":
            return False
        before = before[:qualified.start()].rstrip()

    if not before:
        return False
    if before.endswith("->") or before.endswith("&&"):
        return False
    if before[-1] in ">&*":
        return True

    word = re.search(r"(\w+)$", before)
    return bool(word) and word.group(1) not in KEYWORDS


def check_file(rel, text):
    """The unmarked calls in one file, as (rel, line number, source line)."""
    if rel == EXEMPT:
        return [], 0

    code = blank(text)
    lines = text.split("\n")
    failures = []
    calls = 0

    for match in CALL.finditer(code):
        if is_declaration(code, match):
            continue
        calls += 1
        number = code.count("\n", 0, match.start()) + 1
        here = lines[number - 1]
        previous = lines[number - 2] if number >= 2 else ""
        if MARKER in here or MARKER in previous:
            continue
        failures.append((rel, number, here.strip()))

    return failures, calls


def check(root):
    failures = []
    calls = 0
    for folder, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in BUILD_OUTPUT]
        for f in files:
            if not f.endswith(EXTENSIONS):
                continue
            path = os.path.join(folder, f)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read().replace("\r\n", "\n")
            found, counted = check_file(rel, text)
            failures.extend(found)
            calls += counted
    return failures, calls


SELF_TEST_FILES = {
    # The class whose vector it is: exempt whatever it does.
    "Common/BO/Collection.h":
        "std::vector<T> &GetVector() { return vecObjects; }\n"
        "void Walk() { for (auto x : other.GetVector()) {} }\n",
    # One unmarked call, and one unmarked GetConstVector().
    "Unmarked.cpp":
        "void f()\n"
        "{\n"
        "   for (auto rule : rules->GetVector())\n"
        "      use(rule);\n"
        "   std::vector<int> copy = list.GetConstVector();\n"
        "}\n",
    # Marked on the same line, and on the line before.
    "Marked.cpp":
        "void f()\n"
        "{\n"
        "   auto a = local.GetVector(); // owned by this call: local is loaded above\n"
        "   // owned by this call: the message this delivery holds\n"
        "   for (auto r : message->GetRecipients()->GetVector())\n"
        "      use(r);\n"
        "}\n",
    # A marker two lines up does not reach.
    "TooFar.cpp":
        "void f()\n"
        "{\n"
        "   // owned by this call: said too early\n"
        "\n"
        "   auto b = local.GetVector();\n"
        "}\n",
    # The word in comments and in literals is not a call.
    "Comments.cpp":
        "// Never walk GetVector() of a shared collection.\n"
        "/* GetConstVector() is the same reference,\n"
        "   and GetVector ( ) too. */\n"
        "const char *text = \"GetVector()\";\n"
        "const char quote = '\"'; int after = GetCount();\n",
    # An accessor of that name declared by a class of its own.
    "Declares.h":
        "class MessageRecipients\n"
        "{\n"
        "   std::vector<int> & GetVector() {return recipients_; }\n"
        "   const std::vector<int> &GetConstVector() const;\n"
        "   std::vector<int> GetVector() { return copy_; }\n"
        "};\n"
        "std::vector<int> &Outside::GetVector() { return v_; }\n",
}

SELF_TEST_EXPECTED = {
    ("Unmarked.cpp", 3),
    ("Unmarked.cpp", 5),
    ("TooFar.cpp", 5),
}


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as root:
        for rel, text in SELF_TEST_FILES.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)

        failures, _calls = check(root)
        reported = {(rel, number) for rel, number, _ in failures}

        for expected in sorted(SELF_TEST_EXPECTED):
            if expected in reported:
                print("self-test: the unmarked call at %s:%d is reported" % expected)
            else:
                print("SELF-TEST FAILED: the unmarked call at %s:%d was not reported" % expected)
                ok = False

        for unexpected in sorted(reported - SELF_TEST_EXPECTED):
            print("SELF-TEST FAILED: %s:%d was reported and is not an unmarked call" % unexpected)
            ok = False

        for rel, label in (("Marked.cpp", "a call marked on its line and one marked on the line before"),
                           ("Comments.cpp", "the word in a comment and in a string"),
                           ("Declares.h", "an accessor declared by a class of its own"),
                           ("Common/BO/Collection.h", "a call inside Collection.h")):
            if any(r == rel for r, _ in reported):
                ok = False
            else:
                print("self-test: %s is not reported" % label)

    return ok


def main():
    if "--self-test" in sys.argv:
        return 0 if self_test() else 1

    failures, calls = check(ROOT)
    if failures:
        print("Every GetVector() or GetConstVector() call outside Collection.h carries \"%s\" on its" % MARKER)
        print("line or the line before, with the reason no other thread can reach the collection - or iterates")
        print("GetSnapshot() instead, which is the locked copy (ARCHITECTURE.md says which is which):")
        for rel, number, line in failures:
            print("  %s:%d: %s" % (rel, number, line))
        return 1

    print("check-collection-iteration: %d calls of GetVector() and GetConstVector() outside Collection.h, each marked as owned by its call" % calls)
    return 0


if __name__ == "__main__":
    sys.exit(main())
