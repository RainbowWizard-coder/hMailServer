#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A literal in the server's C++ holds the same characters on Windows as on Linux.

The Windows build does not pass /utf-8 to MSVC. A source file without a UTF-8
byte-order mark is read in the machine's ANSI code page (1252 on the benches), so a
raw UTF-8 character in one of its literals is read as two or three other
characters: a copyright sign, C2 A9, is "A-circumflex, copyright" there and one
character on Linux, where GCC and clang read UTF-8. A file WITH the mark is read as
UTF-8, but a narrow literal ("...", with no L, u, U or u8 before it and not the
argument of _T) is then converted to the ANSI code page, a byte per character
where Linux keeps UTF-8's two or three, and a character that code page lacks
becomes '?'. Either way the same line of source holds different text on the two
platforms, and nothing says so until a test compares it.

That happened in batch42 (2 October 2026): SignatureHtmlSanitizerTester expected
"<b>(c) (c) (c)</b>" written with raw copyright signs, in a file with no mark, and
failed on Windows alone, which stopped the whole internal suite (ClassTester)
there. A character beyond ASCII is written as an escape - \\u00A9 in a wide
literal, the UTF-8 bytes as \\xC2\\xA9 in a narrow one - and this keeps it so.

Every C and C++ file under hmailserver/source/Server is read with its comments
skipped. A byte beyond ASCII inside a string or character literal fails:

  * in any literal of a file without the mark;
  * in a narrow literal of a file with it.

Run from the repository root: python build/check-source-literals.py
Pass --self-test to run it against made-up files and prove it can fail.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "hmailserver", "source", "Server")
EXTENSIONS = (".cpp", ".h", ".hpp", ".c", ".cc", ".inl")
BOM = b"\xef\xbb\xbf"


def literals(data):
    """Each string or character literal of a C/C++ file as (line, prefix, wide, body bytes).

    prefix is the encoding prefix written before it (L, u, U, u8, or empty), and wide
    says whether it is a wide literal by its prefix or by being the argument of _T.
    Comments are skipped, and so are digit separators (1'000).
    """
    found = []
    i = 0
    n = len(data)
    line = 1
    while i < n:
        c = data[i]
        if c == 0x0A:
            line += 1
            i += 1
            continue
        if data.startswith(b"//", i):
            end = data.find(b"\n", i)
            i = n if end < 0 else end
            continue
        if data.startswith(b"/*", i):
            end = data.find(b"*/", i + 2)
            end = n if end < 0 else end + 2
            line += data.count(b"\n", i, end)
            i = end
            continue
        if c in (0x22, 0x27):
            # The identifier characters right before the quote: its prefix, or a
            # digit separator when they are digits.
            start = i
            while start > 0 and (chr(data[start - 1]).isalnum() or data[start - 1] == 0x5F):
                start -= 1
            word = data[start:i].decode("ascii", "replace")
            if c == 0x27 and word and word[0].isdigit():
                i += 1
                continue
            raw = word.endswith("R") and c == 0x22
            prefix = word[:-1] if raw else word
            if prefix not in ("", "L", "u", "U", "u8"):
                prefix = ""
            if raw:
                open_paren = data.find(b"(", i)
                delimiter = b")" + data[i + 1:open_paren] + b'"'
                end = data.find(delimiter, open_paren)
                end = n if end < 0 else end
                body = data[open_paren + 1:end]
                after = end + len(delimiter)
            else:
                j = i + 1
                while j < n and data[j] != c and data[j] != 0x0A:
                    j += 2 if data[j] == 0x5C else 1
                body = data[i + 1:j]
                after = j + 1
            before = data[max(0, start - 16):start].rstrip()
            wide = prefix in ("L", "u", "U") or before.endswith(b"_T(") or before.endswith(b"_TEXT(")
            found.append((line, prefix, wide, body))
            line += body.count(b"\n")
            i = after
            continue
        i += 1
    return found


def problems_of(path, data):
    problems = []
    marked = data.startswith(BOM)
    for line, prefix, wide, body in literals(data[3:] if marked else data):
        if all(b < 0x80 for b in body):
            continue
        if not marked:
            problems.append("%s:%d: a character beyond ASCII in a literal of a file with no UTF-8 byte-order mark: "
                            "MSVC reads it in the ANSI code page. Write it as an escape (\\uXXXX in a wide "
                            "literal, its UTF-8 bytes as \\xHH in a narrow one)" % (path, line))
        elif not wide and prefix != "u8":
            problems.append("%s:%d: a character beyond ASCII in a narrow literal: MSVC converts it to the ANSI "
                            "code page, Linux keeps UTF-8. Make it wide (L\"\\uXXXX\" or _T(...)), or write its "
                            "UTF-8 bytes as \\xHH" % (path, line))
    return problems


def check(root=SOURCE):
    problems = []
    files = 0
    for directory, _, names in os.walk(root):
        for name in sorted(names):
            if not name.lower().endswith(EXTENSIONS):
                continue
            path = os.path.join(directory, name)
            with open(path, "rb") as f:
                data = f.read()
            files += 1
            problems += problems_of(os.path.relpath(path, ROOT).replace(os.sep, "/"), data)
    return problems, files


def self_test():
    failures = []
    e_acute = "\u00e9".encode("utf-8")

    def expect(label, data, count):
        found = problems_of("sample.cpp", data)
        if len(found) != count:
            failures.append("%s: %d problem(s) reported, not %d: %s" % (label, len(found), count, found))

    # The tree passes, and the walk sees it.
    problems, files = check()
    if problems:
        failures.append("the tree passes: %s" % problems[:3])
    if files < 1000:
        failures.append("only %d files were read: this check has stopped seeing the source" % files)

    # batch42's failure: a raw character in a wide literal of a file with no mark.
    expect("a raw character in _T() of a file with no mark", b'x = _T("<b>' + e_acute + b'</b>");\n', 1)
    expect("a raw character in L\"\" of a file with no mark", b'x = L"' + e_acute + b'";\n', 1)
    expect("a raw character in a narrow literal of a file with no mark", b'x = "' + e_acute + b'";\n', 1)
    expect("a raw character in a character literal", b"wchar_t c = L'" + e_acute + b"';\n", 1)
    expect("a raw character in a raw string", b'x = R"(' + e_acute + b')";\n', 1)
    expect("two literals on two lines", b'a = "' + e_acute + b'";\nb = L"' + e_acute + b'";\n', 2)

    # With the mark: a narrow literal fails, a wide one and u8 pass.
    expect("a raw character in a narrow literal of a marked file", BOM + b'x = "' + e_acute + b'";\n', 1)
    expect("a raw character in _T() of a marked file", BOM + b'x = _T("' + e_acute + b'");\n', 0)
    expect("a raw character in L\"\" of a marked file", BOM + b'x = L"' + e_acute + b'";\n', 0)
    expect("a raw character in u8\"\" of a marked file", BOM + b'x = u8"' + e_acute + b'";\n', 0)

    # Not literals: comments, escapes, a digit separator, a quote in a comment.
    expect("a raw character in a line comment", b"// caf" + e_acute + b'\nx = "a";\n', 0)
    expect("a raw character in a block comment", b"/* caf" + e_acute + b' "q" */ x = "a";\n', 0)
    expect("an escape", b'x = _T("\\u00E9"); y = "\\xC3\\xA9";\n', 0)
    expect("a digit separator before a literal", b"int n = 1'000; x = _T(\"" + e_acute + b"\");\n", 1)
    expect("an escaped quote inside a literal", b'x = "a\\"b' + e_acute + b'";\n', 1)

    # The line is the literal's.
    found = problems_of("sample.cpp", b'a = "x";\n\n// "' + e_acute + b'"\nb = "' + e_acute + b'";\n')
    if len(found) != 1 or ":4:" not in found[0]:
        failures.append("the literal's line is not the one reported: %s" % found)

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    problems, files = check()
    if problems:
        print("A literal holds other characters on Windows than on Linux:")
        for problem in problems:
            print("  " + problem)
        return 1

    print("check-source-literals: %d C and C++ files under hmailserver/source/Server, every literal beyond ASCII "
          "written so that both platforms read the same characters" % files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
