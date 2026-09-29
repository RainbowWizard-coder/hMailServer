#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The 1,042 words the legacy Blowfish scheme starts from are pi, and nothing else.

Blowfish's P-array (18 words) and its four S-boxes (4 x 256 words) start as the
fractional part of pi in hexadecimal, in that order: 3.243F6A88 85A308D3 ... The
description of the cipher says so (B. Schneier, Fast Software Encryption 1993), and
that is what makes the tables facts rather than anybody's code. The server's own
implementation, hmailserver/source/Server/Common/Util/LegacyBlowfish.cpp, carries
them as PiP and PiS. This computes pi's first 8,336 hexadecimal digits from Machin's
formula, pi = 16 arctan(1/5) - 4 arctan(1/239), in integer arithmetic, and compares
them with the file's words one by one, so that a reviewer - a distribution's, or
anybody's - need not take the tables on trust, and a word changed by accident in an
edit is found before the known-answer tests would be run.

Run from the repository root: python build/check-blowfish-constants.py
Pass --self-test to run it against mutated copies of the file and prove it can fail.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util", "LegacyBlowfish.cpp")

P_WORDS = 18
S_WORDS = 4 * 256
WORDS = P_WORDS + S_WORDS


def arctan_inverse(x, one):
    """arctan(1/x) * one by its Taylor series, in integers."""
    total = term = one // x
    x2 = x * x
    n = 1
    sign = -1
    while term:
        term //= x2
        total += sign * (term // (2 * n + 1))
        sign = -sign
        n += 1
    return total


def pi_words(count=WORDS):
    """The first count 32-bit words of pi's fractional part in hexadecimal."""
    digits = count * 8
    guard = 64
    one = 1 << (digits * 4 + guard)
    scaled = (16 * arctan_inverse(5, one) - 4 * arctan_inverse(239, one)) >> guard
    fraction = scaled - (3 << (digits * 4))
    text = "%0*x" % (digits, fraction)
    return [int(text[i:i + 8], 16) for i in range(0, digits, 8)]


def table_words(source):
    """PiP's words, then PiS's, as the file writes them; None for a table not found."""
    p = re.search(r"const\s+uint32_t\s+PiP\s*\[\s*18\s*\]\s*=\s*\{(.*?)\};", source, re.S)
    s = re.search(r"const\s+uint32_t\s+PiS\s*\[\s*4\s*\]\s*\[\s*256\s*\]\s*=\s*\{(.*?)\n\s*\};", source, re.S)
    if not p or not s:
        return None, None

    def words(body):
        body = re.sub(r"//[^\n]*", "", body)
        return [int(w, 16) for w in re.findall(r"\b0x([0-9A-Fa-f]{1,8})\b", body)]

    return words(p.group(1)), words(s.group(1))


def check(source, expected):
    problems = []
    p, s = table_words(source)
    if p is None:
        return ["LegacyBlowfish.cpp has no PiP[18] and PiS[4][256] tables this check can read: it has stopped "
                "seeing the source"]
    if len(p) != P_WORDS:
        problems.append("PiP holds %d words, not %d" % (len(p), P_WORDS))
    if len(s) != S_WORDS:
        problems.append("PiS holds %d words, not %d" % (len(s), S_WORDS))
    found = p + s
    for index, (have, want) in enumerate(zip(found, expected)):
        if have != want:
            where = "P%d" % (index + 1) if index < P_WORDS else "S%d[%d]" % ((index - P_WORDS) // 256 + 1, (index - P_WORDS) % 256)
            problems.append("%s is 0x%08x; pi's word there is 0x%08x" % (where, have, want))
            if len(problems) > 20:
                problems.append("and more")
                break
    return problems


def self_test(source, expected):
    failures = []

    def expect(label, mutated, needle):
        found = check(mutated, expected)
        if not any(needle in problem for problem in found):
            failures.append("%s was not reported (got %r)" % (label, found[:3]))

    if check(source, expected):
        failures.append("the tree's file passes: %s" % check(source, expected)[:3])
    if expected[:4] != [0x243F6A88, 0x85A308D3, 0x13198A2E, 0x03707344]:
        failures.append("the computed words do not begin 243F6A88 85A308D3 13198A2E 03707344, as the description "
                        "of the cipher says pi's do")
    if expected[-1] != 0x3AC372E6:
        failures.append("the computed last word is 0x%08x, not the last word of S4 (0x3ac372e6)" % expected[-1])

    first = "0x243f6a88"
    last = "0x3ac372e6"
    if first not in source or last not in source:
        failures.append("the file does not hold P1 and S4[255] as the mutations expect")
    else:
        expect("a changed P1", source.replace(first, "0x243f6a89", 1), "P1 is 0x243f6a89")
        expect("a changed S4[255]", source.replace(last, "0x3ac372e7", 1), "S4[255] is 0x3ac372e7")
        # Two words swapped: each is reported where the other belongs.
        swapped = source.replace("0x85a308d3, 0x13198a2e", "0x13198a2e, 0x85a308d3", 1)
        expect("two words swapped", swapped, "P2 is 0x13198a2e")
        expect("a word missing", source.replace(", " + last, "", 1), "PiS holds 1023 words")
    expect("a check that has stopped seeing the source", "int x;", "stopped seeing the source")

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    with open(SOURCE, encoding="utf-8") as f:
        source = f.read()
    expected = pi_words()
    if "--self-test" in sys.argv:
        return self_test(source, expected)

    problems = check(source, expected)
    if problems:
        print("LegacyBlowfish.cpp's tables are not the hexadecimal digits of pi:")
        for problem in problems:
            print("  " + problem)
        return 1

    print("check-blowfish-constants: P1-P18 and S1-S4, %d words, are pi's first %d hexadecimal digits"
          % (WORDS, WORDS * 8))
    return 0


if __name__ == "__main__":
    sys.exit(main())
