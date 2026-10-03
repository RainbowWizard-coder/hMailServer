#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An IP address in text is read by IPAddress, the same way on Windows and on Linux.

Boost.Asio's make_address (and make_address_v4, make_address_v6 and the older
from_string) reads text with the platform's own parser: inet_pton on Linux, and
WSAStringToAddress on Windows, which reads a good deal more - the forms inet_aton
has read since 4.2BSD (192.0.2 as 192.0.0.2, 3232235777 as 192.168.1.1, 0x7f.1 as
127.0.0.1, 0300.0250.0.1 and 010.0.0.1 octal, as 192.168.0.1 and 8.0.0.1), an
address with a port after it (192.0.2.7:25, [::1]:25) and one in brackets. So the
same text was an address on Windows and not on Linux, or another address.

That was found in batch42 (2 October 2026): FqdnCheck judged a HELO of 192.0.2 "an
IP address" on Windows and "numeric where its top-level domain should be" on Linux,
and its tester stopped the whole internal suite on Windows; an AutoBanNeverBan entry
of 192.168.1.0/24 typed as 192.168.1/24 protected 192.168.0.0/24 on Windows, without
a word. IPAddress::Parse (Common/TCPIP/IPAddress.h) is the one reader now, and its
tester holds both platforms to the same answer for every form.

The C library's inet_addr and inet_aton are the same shape the other way round:
they read those older forms on every platform, so a text one of them takes is not
the address IPAddress::Parse, inet_pton or the other platform's reader of the same
setting takes. The Windows resolver read DNSServer with inet_addr until 6.3.6 while
the DNSSEC resolver and the TLS policy read it with inet_pton: DNSServer=8.8.8 sent
one kind of lookup to 8.8.0.8 and the other to the system's servers.

This fails on any call of make_address, make_address_v4, make_address_v6,
from_string, inet_addr or inet_aton in the server's C and C++
(hmailserver/source/Server) and the fuzz harnesses (fuzz/), comments skipped,
outside IPAddress.cpp. The conversion make_address_v4(boost::asio::ip::v4_mapped,
...) reads no text and is allowed; inet_pton reads the four decimal parts alone, on
both platforms, and is not held to it. A call that must read as the C library does -
where the platforms would otherwise differ - is allowed when its line or one of the
three above it carries "address-parsing-exempt:" and the reason.

Run from the repository root: python build/check-address-parsing.py
Pass --self-test to run it against made-up files and prove it can fail.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREES = [os.path.join(ROOT, "hmailserver", "source", "Server"), os.path.join(ROOT, "fuzz")]
EXTENSIONS = (".cpp", ".h", ".hpp", ".c", ".cc", ".inl")
ALLOWED_FILES = {"hmailserver/source/Server/Common/TCPIP/IPAddress.cpp"}
EXEMPT = "address-parsing-exempt:"
CALL = re.compile(r"\b(make_address(?:_v4|_v6)?|from_string|inet_addr|inet_aton)\s*\(\s*([^,)]*)")


def without_comments(text):
    """The text with comments blanked and string literals emptied, line breaks kept."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
            continue
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append('""')
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def exempt(raw_lines, number):
    """Whether line number (1-based) or one of the three above it carries the marker and a reason."""
    for index in range(max(0, number - 4), number):
        line = raw_lines[index]
        at = line.find(EXEMPT)
        if at >= 0 and line[at + len(EXEMPT):].strip():
            return True
    return False


def problems_of(path, text):
    problems = []
    code = without_comments(text)
    raw_lines = text.split("\n")
    for number, line in enumerate(code.split("\n"), 1):
        if CALL.search(line) and exempt(raw_lines, number):
            continue
        for match in CALL.finditer(line):
            first = match.group(2).strip()
            if match.group(1) == "make_address_v4" and first.endswith("v4_mapped"):
                continue
            if match.group(1).startswith("inet_"):
                problems.append("%s:%d: %s reads the older forms of an IPv4 address (127.1, 010.0.0.1) as addresses, "
                                "which IPAddress::Parse and inet_pton do not; use IPAddress::Parse" % (path, number, match.group(1)))
            else:
                problems.append("%s:%d: %s reads an address with the platform's own parser, which on Windows reads "
                                "forms Linux does not; use IPAddress::Parse" % (path, number, match.group(1)))
    return problems


def check(trees=TREES):
    problems = []
    files = 0
    for tree in trees:
        for directory, _, names in os.walk(tree):
            for name in sorted(names):
                if not name.lower().endswith(EXTENSIONS):
                    continue
                path = os.path.join(directory, name)
                relative = os.path.relpath(path, ROOT).replace(os.sep, "/")
                files += 1
                if relative in ALLOWED_FILES:
                    continue
                with open(path, encoding="utf-8", errors="replace") as f:
                    problems += problems_of(relative, f.read())
    return problems, files


def self_test():
    failures = []

    def expect(label, text, count):
        found = problems_of("sample.cpp", text)
        if len(found) != count:
            failures.append("%s: %d problem(s) reported, not %d: %s" % (label, len(found), count, found))

    problems, files = check()
    if problems:
        failures.append("the tree passes: %s" % problems[:3])
    if files < 1000:
        failures.append("only %d files were read: this check has stopped seeing the source" % files)

    # Each reader, in the forms the tree used before.
    expect("make_address", "auto a = boost::asio::ip::make_address(host, ec);\n", 1)
    expect("make_address on a std::string", "boost::asio::ip::make_address(std::string(h.c_str()), notAddress);\n", 1)
    expect("make_address_v4", "a = boost::asio::ip::make_address_v4(text, error);\n", 1)
    expect("make_address_v6", "a = boost::asio::ip::make_address_v6(text, error);\n", 1)
    expect("from_string", "a = boost::asio::ip::address::from_string(text);\n", 1)
    expect("a call split over two lines", "a =\n   boost::asio::ip::make_address(\n      text, error);\n", 1)
    expect("two calls on two lines", "make_address(a, e);\nmake_address_v6(b, e);\n", 2)
    expect("inet_addr", "addr.sin_addr.s_addr = inet_addr(sCustomDNS.c_str());\n", 1)
    expect("inet_addr with ::", "unsigned long a = ::inet_addr(host);\n", 1)
    expect("inet_aton", "if (inet_aton(text, &address) == 0) return false;\n", 1)

    # Not a reading of text: the v4-mapped conversion, a comment, a string, a name.
    expect("the v4-mapped conversion", "return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, a.to_v6());\n", 0)
    expect("a line comment", "// Boost's make_address(text) reads it\nx = 1;\n", 0)
    expect("a block comment", "/* make_address(text, ec) */ x = 1;\n", 0)
    expect("a string", 'Log("make_address(text) failed");\n', 0)
    expect("IPAddress::Parse", "if (!IPAddress::Parse(text, address)) return false;\n", 0)
    expect("inet_pton", "if (inet_pton(AF_INET, text, &address.sin_addr) != 1) return false;\n", 0)
    expect("a name that ends in inet_addr", "x = my_inet_addr(text);\n", 0)

    # The marker, with a reason, on the line or within the three above it - and not
    # without a reason, nor four lines away.
    expect("exempt above", "// address-parsing-exempt: the C library's answer on both platforms\nx = inet_addr(h);\n", 0)
    expect("exempt on the line", "x = inet_addr(h); // address-parsing-exempt: as before\n", 0)
    expect("exempt with no reason", "// address-parsing-exempt:\nx = inet_addr(h);\n", 1)
    expect("exempt too far above", "// address-parsing-exempt: a reason\n\n\n\nx = inet_addr(h);\n", 1)

    # IPAddress.cpp itself is where the one reader is, and is not held to this.
    found, _ = check()
    if any("IPAddress.cpp" in problem for problem in found):
        failures.append("IPAddress.cpp was held to the rule: %s" % found)

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    problems, files = check()
    if problems:
        print("An address is read with the platform's own parser:")
        for problem in problems:
            print("  " + problem)
        return 1

    print("check-address-parsing: %d C and C++ files, every address in text read by IPAddress::Parse" % files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
