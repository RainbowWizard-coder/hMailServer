#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""HTML's named character references, as C++ the server compiles.

The SURBL test and the sender lists' URL entries read the links of an HTML part
as a browser reads them, its character references first (SURBL.cpp,
DecodeCharacterReferences_): <a href="http://&eopf;vil.example/"> goes to
evil.example, since &eopf; is U+1D556 and UTS 46 maps that to an 'e'. This
writes every named reference HTML defines, out of the HTML standard's own list,
vendored as html.spec.whatwg.org serves it under
hmailserver/source/Server/Common/AntiSpam/whatwg:

  entities.json   each reference's name, with its '&' and, but for the 106 HTML
                  reads without one too, its ';', and the one or two code points
                  it stands for

and writes

  Common/AntiSpam/HtmlReferencesData.cpp   the table (Common/AntiSpam/
                                           HtmlReferences.h says what it holds)

The file is held to the SHA-256 it is pinned by below, and to what the standard
says of the list, before anything is written: a name is '&', ASCII letters and
digits and maybe a ';'; a reference is one or two code points, as its
"characters" says too; and every name without its ';' is there with it, standing
for the same. The standard says the list "is static and will not be expanded or
changed in the future", so the pin is not expected to move; if the file ever
does change, put the new file and its SHA-256 here, run this, run SURBLTester on
both platforms (ClassTester on Windows, public-suffix-tests on Linux), and say in
CHANGELOG.md what changed.

The output is written with LF line endings and compared with CRLF folded to LF,
since the tree checks text out with CRLF.

Usage:
  python build/generate-html-references.py           write the table
  python build/generate-html-references.py --check   exit 1 when the committed
                                                     table is not what this writes
"""
import hashlib
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANTISPAM = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "AntiSpam")
WHATWG = os.path.join(ANTISPAM, "whatwg")
INPUT = os.path.join(WHATWG, "entities.json")
OUTPUT = os.path.join(ANTISPAM, "HtmlReferencesData.cpp")

# The SHA-256 of https://html.spec.whatwg.org/entities.json as the WHATWG serves it
# (LF line endings, Last-Modified 12 November 2025; fetched 13 October 2026). The
# tree keeps it byte for byte: .gitattributes marks whatwg/ -text.
PIN = "d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6"

NAME = re.compile(r"&([A-Za-z0-9]+;?)\Z")
LAST_CODE_POINT = 0x10FFFF


class InputError(Exception):
    pass


def read_references():
    """[(name without its '&', (code points))], sorted by name, byte by byte."""
    if not os.path.isfile(INPUT):
        raise InputError("%s is missing" % INPUT)
    data = open(INPUT, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != PIN:
        if hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest() == PIN:
            raise InputError("%s has CRLF line endings: the file is kept as the WHATWG serves it "
                             "(.gitattributes marks whatwg/ -text); check it out again" % INPUT)
        raise InputError("%s has SHA-256 %s, and %s is pinned: it is not the list this was written for"
                         % (INPUT, digest, PIN))

    entries = json.loads(data.decode("utf-8"))
    if not isinstance(entries, dict) or not entries:
        raise InputError("%s is not an object of references" % INPUT)

    references = {}
    for key, value in entries.items():
        match = NAME.match(key)
        if not match:
            raise InputError("%r is not a reference's name: '&', ASCII letters and digits, maybe a ';'" % key)
        if not isinstance(value, dict) or set(value) != {"codepoints", "characters"}:
            raise InputError("%s: not a \"codepoints\" and a \"characters\"" % key)
        code_points = value["codepoints"]
        if (not isinstance(code_points, list) or not 1 <= len(code_points) <= 2 or
                not all(isinstance(cp, int) and 0 < cp <= LAST_CODE_POINT and not 0xD800 <= cp <= 0xDFFF
                        for cp in code_points)):
            raise InputError("%s: %r is not one or two code points" % (key, code_points))
        if value["characters"] != "".join(chr(cp) for cp in code_points):
            raise InputError("%s: its characters are not its code points" % key)
        references[match.group(1)] = tuple(code_points)

    for name, code_points in references.items():
        if not name.endswith(";") and references.get(name + ";") != code_points:
            raise InputError("&%s, read without its ';', is not also there with it, standing for the same" % name)

    return sorted(references.items(), key=lambda item: item[0].encode("ascii"))


HEADER = """// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// GENERATED FILE - do not edit. Written by build/generate-html-references.py from
// the HTML standard's list of named character references, entities.json, under
// Common/AntiSpam/whatwg. Edit the generator and run it.
//
// The list is the WHATWG's, under the BSD 3-Clause License as a portion of the
// HTML standard incorporated into source code (whatwg/entities.LICENSE):
// Copyright (c) WHATWG (Apple, Google, Mozilla, Microsoft).

"""


def source(references):
    with_semicolon = [name for name, _ in references if name.endswith(";")]
    legacy = [name for name, _ in references if not name.endswith(";")]
    out = [HEADER]
    out.append("#include \"StdAfx.h\"\n")
    out.append("#include \"HtmlReferences.h\"\n\n")
    out.append("namespace HM\n{\n   namespace HtmlReferences\n   {\n")
    out.append("      // entities.json: every named character reference, by its name without its\n"
               "      // '&' - with its ';', and for the %d HTML also reads without one, without it\n"
               "      // too - and the code point it stands for, and a second for the %d that stand\n"
               "      // for two (0 for the others). %s names, sorted byte by byte, for a binary\n"
               "      // search.\n" % (len(legacy), sum(1 for _, cps in references if len(cps) == 2),
                                     "{:,}".format(len(references))))
    out.append("      const NamedReference Named[] =\n      {\n")
    for name, code_points in references:
        second = "0x%04X" % code_points[1] if len(code_points) == 2 else "0"
        out.append("         { \"%s\", 0x%04X, %s },\n" % (name, code_points[0], second))
    out.append("      };\n\n")
    out.append("      const size_t NamedCount = sizeof(Named) / sizeof(Named[0]);\n\n")
    out.append("      // The longest name, with its ';', and the longest HTML reads without one.\n")
    out.append("      const size_t LongestName = %d;\n" % max(len(name) for name in with_semicolon))
    out.append("      const size_t LongestLegacyName = %d;\n" % max(len(name) for name in legacy))
    out.append("   }\n}\n")
    return "".join(out)


def read_text(path):
    if not os.path.exists(path):
        return None
    return io.open(path, "r", encoding="utf-8").read().replace("\r\n", "\n")


def main(argv):
    try:
        text = source(read_references())
    except InputError as problem:
        print("generate-html-references: %s" % problem)
        print("Nothing written.")
        return 1

    if "--check" in argv:
        if read_text(OUTPUT) != text:
            print("%s is not what build/generate-html-references.py writes from %s: run it and commit what it "
                  "writes." % (os.path.relpath(OUTPUT, ROOT), os.path.relpath(INPUT, ROOT)))
            return 1
        print("HtmlReferencesData.cpp is what %s makes." % os.path.relpath(INPUT, ROOT))
        return 0

    io.open(OUTPUT, "w", encoding="utf-8", newline="\n").write(text)
    print("Wrote %s." % os.path.relpath(OUTPUT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
