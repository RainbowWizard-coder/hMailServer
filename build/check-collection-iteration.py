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
  carries the comment "// owned by this call" and the reason no other thread
  can touch that collection while the call runs (a local loaded above, a
  message's own recipients, a collection a getter builds new for every
  caller). The marker is in the comment of the call's own line, or it is the
  comment of the line before, which then holds nothing but that comment. One
  marker covers one call: a marked line with two calls on it fails. A call
  inside a #define fails whatever it carries, because every use of the macro
  would escape the check. The one exception is Collection.h, the class whose
  vector it is. Anything else fails here with the file, the line and why, and
  the fix is GetSnapshot() or the comment with its reason - never the comment
  alone because it is shorter.

  A marker on a forwarding accessor - a function that hands GetVector() on to
  its own callers - is refused in review, for the reason the #define is refused
  here: it would exempt every caller behind one line.

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

UNMARKED = "unmarked"
SHARED_MARKER = "one marker, more than one call on the line"
IN_DEFINE = "inside a #define"


def blank(text):
    """Two copies of text, every newline and every length kept, so that an
    offset in either is the same offset in the source: the code, with comments
    and string and character literals blanked to spaces; and the comments, with
    everything that is not a comment blanked."""
    code = []
    comments = []
    i = 0
    n = len(text)

    def spaces(chunk):
        return re.sub(r"[^\n]", " ", chunk)

    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            code.append(" " * (j - i))
            comments.append(text[i:j])
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            code.append(spaces(text[i:j]))
            comments.append(text[i:j])
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            code.append(c + spaces(text[i + 1:j]) + text[j])
            comments.append(spaces(text[i:j + 1]))
            i = j + 1
        else:
            code.append(c)
            comments.append("\n" if c == "\n" else " ")
            i += 1
    return "".join(code), "".join(comments)


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


def directive_lines(code_lines, raw_lines):
    """The 1-based numbers of the lines that belong to a preprocessor directive,
    continuation lines included."""
    inside = set()
    continuing = False
    for index, line in enumerate(code_lines):
        if continuing or line.lstrip().startswith("#"):
            inside.add(index + 1)
            continuing = raw_lines[index].rstrip().endswith("\\")
        else:
            continuing = False
    return inside


def check_file(rel, text):
    """The calls in one file that break the rule, as (rel, line number, why,
    source line), and the number of calls found."""
    if rel == EXEMPT:
        return [], 0

    code, comments = blank(text)
    raw_lines = text.split("\n")
    code_lines = code.split("\n")
    comment_lines = comments.split("\n")
    directives = directive_lines(code_lines, raw_lines)

    found = []
    for match in CALL.finditer(code):
        if is_declaration(code, match):
            continue
        found.append(code.count("\n", 0, match.start()) + 1)

    per_line = {}
    for number in found:
        per_line[number] = per_line.get(number, 0) + 1

    failures = []
    for number in found:
        here = raw_lines[number - 1].strip()

        if number in directives:
            failures.append((rel, number, IN_DEFINE, here))
            continue

        marked_here = MARKER in comment_lines[number - 1]
        marked_before = (number >= 2 and
                         code_lines[number - 2].strip() == "" and
                         MARKER in comment_lines[number - 2])

        if not (marked_here or marked_before):
            failures.append((rel, number, UNMARKED, here))
        elif per_line[number] > 1:
            failures.append((rel, number, SHARED_MARKER, here))

    return failures, len(found)


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
    # A marker at the end of a call's line does not cover the call on the next.
    "SameLineThenNext.cpp":
        "void f()\n"
        "{\n"
        "   auto a = local.GetVector(); // owned by this call: local is loaded above\n"
        "   for (auto r : shared->GetVector()) {}\n"
        "}\n",
    # One marker does not cover two calls on the line below it.
    "TwoOnALine.cpp":
        "// owned by this call: one of the two\n"
        "auto a = x.GetVector(); auto b = y.GetConstVector();\n",
    # A marker in a string on the call's own line is not a comment.
    "MarkerInString.cpp":
        "Log(\"// owned by this call\"); for (auto r : shared->GetVector()) {}\n",
    # Nor is one in a string on the line above.
    "MarkerStringAbove.cpp":
        "const char *s = \"// owned by this call\";\n"
        "for (auto r : shared->GetVector()) {}\n",
    # A call in a #define fails however it is marked, on its first line or on
    # a continuation line.
    "Macro.cpp":
        "#define WALK(c) c->GetVector() // owned by this call: says who?\n"
        "for (auto r : WALK(shared)) {}\n"
        "#define WALK_TOO(c) \\\n"
        "   c->GetConstVector()\n",
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
    ("Unmarked.cpp", 3): UNMARKED,
    ("Unmarked.cpp", 5): UNMARKED,
    ("TooFar.cpp", 5): UNMARKED,
    ("SameLineThenNext.cpp", 4): UNMARKED,
    ("TwoOnALine.cpp", 2): SHARED_MARKER,
    ("MarkerInString.cpp", 1): UNMARKED,
    ("MarkerStringAbove.cpp", 2): UNMARKED,
    ("Macro.cpp", 1): IN_DEFINE,
    ("Macro.cpp", 4): IN_DEFINE,
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
        reported = {}
        for rel, number, why, _line in failures:
            reported.setdefault((rel, number), set()).add(why)

        for (rel, number), why in sorted(SELF_TEST_EXPECTED.items()):
            if why in reported.get((rel, number), set()):
                print("self-test: %s:%d is reported, %s" % (rel, number, why))
            else:
                print("SELF-TEST FAILED: %s:%d was not reported as %s (got %s)" %
                      (rel, number, why, sorted(reported.get((rel, number), set())) or "nothing"))
                ok = False

        for key in sorted(set(reported) - set(SELF_TEST_EXPECTED)):
            print("SELF-TEST FAILED: %s:%d was reported (%s) and should not be" % (key[0], key[1], ", ".join(sorted(reported[key]))))
            ok = False

        for rel, label in (("Marked.cpp", "a call marked on its line and one marked on the line before"),
                           ("SameLineThenNext.cpp", "the call its own line's marker covers"),
                           ("Comments.cpp", "the word in a comment and in a string"),
                           ("Declares.h", "an accessor declared by a class of its own"),
                           ("Common/BO/Collection.h", "a call inside Collection.h")):
            wrongly = [key for key in reported if key[0] == rel and key not in SELF_TEST_EXPECTED]
            if wrongly:
                ok = False
            else:
                print("self-test: %s is not reported" % label)

    return ok


def main():
    if "--self-test" in sys.argv:
        return 0 if self_test() else 1

    failures, calls = check(ROOT)
    if failures:
        print("Every GetVector() or GetConstVector() call outside Collection.h carries \"%s\" and its reason," % MARKER)
        print("in the comment of its own line or as the only thing on the line before, one marker to one call and")
        print("none in a #define - or iterates GetSnapshot() instead, which is the locked copy (ARCHITECTURE.md says")
        print("which is which):")
        for rel, number, why, line in failures:
            print("  %s:%d: %s: %s" % (rel, number, why, line))
        return 1

    print("check-collection-iteration: %d calls of GetVector() and GetConstVector() outside Collection.h, each marked as owned by its call" % calls)
    return 0


if __name__ == "__main__":
    sys.exit(main())
