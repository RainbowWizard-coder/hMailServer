#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""UTS 46's tables, and its conformance tests, as C++ the server compiles.

The server reads the host of a link as a browser reads it (SURBL, the sender
lists' URL entries): the WHATWG URL standard's "domain to ASCII", which is
Unicode's UTS 46 ToASCII - the IDNA Mapping Table, Normalization Form C, the
validity criteria with the bidi and joiner rules of IDNA2008, and Punycode.
Common/Idna/Idna.cpp does that; this writes the data it does it from, out of
the Unicode data files vendored, as unicode.org serves them, under
hmailserver/source/Server/Common/Idna/ucd:

  IdnaMappingTable.txt       each code point's status (valid, ignored, mapped,
                             deviation, disallowed) and what it maps to
  UnicodeData.txt            the canonical decompositions and combining
                             classes NFC needs, the marks a label may not
                             begin with (General_Category M*), and the
                             punctuation, symbols, separators and others
                             (P*, S*, Z*, C*), whose group SURBL's reading of
                             a link in running text tells where a mail
                             program ends a link by
  DerivedBidiClass.txt       every code point's Bidi_Class, for RFC 5893's rule -
                             an unassigned one's too, the default its @missing
                             lines give, which UnicodeData.txt does not
  CompositionExclusions.txt  the composites NFC never makes, beside the
                             singletons and non-starter decompositions, which
                             are worked out from UnicodeData.txt
  DerivedJoiningType.txt     Joining_Type, for RFC 5892's rule for ZERO WIDTH
                             NON-JOINER
  IdnaTestV2.txt             Unicode's conformance tests for UTS 46

and writes

  Common/Idna/IdnaTables.cpp    the tables (Common/Idna/IdnaTables.h says what
                                each holds)
  Common/Idna/IdnaTestData.cpp  IdnaTestV2.txt's test lines, each as the file
                                writes it with its comment cut off, for
                                IdnaTester (ClassTester on Windows, idna-tests
                                on Linux), which reads them as the file's own
                                FORMAT section says

Every input is held to the SHA-256 it is pinned by below and to the Unicode
version its header names, and nothing is written when one is not: a table made
from a file nobody can name is a table nobody can check. To move to a new
Unicode version, replace the six files with the new version's from
https://www.unicode.org/Public/<version>/ucd/ (DerivedBidiClass.txt and
DerivedJoiningType.txt under ucd/extracted/) and /Public/<version>/idna/, put
their SHA-256s and the version here, run this, run IdnaTester on both
platforms, and say in CHANGELOG.md what changed.

Every output is written with LF line endings and compared with CRLF folded to
LF, since the tree checks text out with CRLF.

Usage:
  python build/generate-idna-tables.py            write both files
  python build/generate-idna-tables.py --check    exit 1 when either committed
                                                  file is not what this writes
  python build/generate-idna-tables.py --differences OLD_DIR --output FILE
        write FILE, a C++ header listing the code points whose UTS 46 data
        differ between the Unicode data in OLD_DIR (the same file names but
        IdnaTestV2.txt, of another version) and the vendored data - for the
        differential fuzz
        harness against ada (fuzz/idna/), whose tables are of an older
        version: an input holding one of them is not a difference of the two
        implementations' code. OLD_DIR's files are not held to a pin.
"""
import hashlib
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDNA = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Idna")
UCD = os.path.join(IDNA, "ucd")
OUT_TABLES = os.path.join(IDNA, "IdnaTables.cpp")
OUT_TESTS = os.path.join(IDNA, "IdnaTestData.cpp")

VERSION = "18.0.0"

# The SHA-256 of each file as unicode.org serves it (LF line endings; the tree
# keeps them byte for byte, .gitattributes marks the directory -text).
PINS = {
    "IdnaMappingTable.txt": "a03b1eb38032268c696406a83f0972d6a815acd2c8d4151d42ec0fda70ffced1",
    "IdnaTestV2.txt": "0236b75c5b20dfd857b3b5cf75509887959d6ab00d6c7bda9b7dc3df518c1fde",
    "UnicodeData.txt": "0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950",
    "CompositionExclusions.txt": "c759b100e9ae8960ae6b50fc5765950a7581fd9f30d4ef95f68f25a6d97e818e",
    "DerivedJoiningType.txt": "e2408ff2c92b175b0f7bf62c989bbb54c7b077528fe31f8d69b96fa09e7d61ed",
    "DerivedBidiClass.txt": "d9e23222522551348ea1ccfbb4f62efbf98982afb95840f8959c08ed992c5607",
}

# Where each file names its version, when it does (UnicodeData.txt does not:
# its pin is its version).
VERSION_LINES = {
    "IdnaMappingTable.txt": "# Version: %s",
    "IdnaTestV2.txt": "# Version: %s",
    "CompositionExclusions.txt": "# CompositionExclusions-%s.txt",
    "DerivedJoiningType.txt": "# DerivedJoiningType-%s.txt",
    "DerivedBidiClass.txt": "# DerivedBidiClass-%s.txt",
}

STATUSES = ["valid", "ignored", "mapped", "deviation", "disallowed"]
STATUS_NAMES = {"valid": "StatusValid", "ignored": "StatusIgnored", "mapped": "StatusMapped",
                "deviation": "StatusDeviation", "disallowed": "StatusDisallowed"}

BIDI_CLASSES = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
                "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"]
# The long names DerivedBidiClass.txt's @missing lines give the defaults by.
BIDI_LONG_NAMES = {
    "Left_To_Right": "L", "Right_To_Left": "R", "Arabic_Letter": "AL", "European_Number": "EN",
    "European_Separator": "ES", "European_Terminator": "ET", "Arabic_Number": "AN", "Common_Separator": "CS",
    "Nonspacing_Mark": "NSM", "Boundary_Neutral": "BN", "Paragraph_Separator": "B", "Segment_Separator": "S",
    "White_Space": "WS", "Other_Neutral": "ON", "Left_To_Right_Embedding": "LRE", "Left_To_Right_Override": "LRO",
    "Right_To_Left_Embedding": "RLE", "Right_To_Left_Override": "RLO", "Pop_Directional_Format": "PDF",
    "Left_To_Right_Isolate": "LRI", "Right_To_Left_Isolate": "RLI", "First_Strong_Isolate": "FSI",
    "Pop_Directional_Isolate": "PDI",
}
JOINING_TYPES = ["U", "C", "D", "L", "R", "T"]

MAX_CODE_POINT = 0x10FFFF

HANGUL_FIRST, HANGUL_LAST = 0xAC00, 0xD7A3


class InputError(Exception):
    pass


def read_input(directory, name, pinned):
    """The file's text, held to its pin and its version when pinned is true."""
    path = os.path.join(directory, name)
    if not os.path.isfile(path):
        raise InputError("%s is missing" % path)
    data = open(path, "rb").read()
    if pinned:
        digest = hashlib.sha256(data).hexdigest()
        if digest != PINS[name]:
            folded = hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()
            if folded == PINS[name]:
                raise InputError("%s has CRLF line endings: the file is kept as unicode.org serves it "
                                 "(.gitattributes marks ucd/ -text); check it out again" % path)
            raise InputError("%s has SHA-256 %s, and %s is pinned: it is not the Unicode %s file "
                             "this was written for" % (path, digest, PINS[name], VERSION))
        if name in VERSION_LINES:
            line = VERSION_LINES[name] % VERSION
            if line.encode("ascii") not in data:
                raise InputError("%s does not say \"%s\"" % (path, line))
    return data.decode("utf-8").replace("\r\n", "\n")


def code_points(field):
    return [int(x, 16) for x in field.split()]


def parse_range(field):
    field = field.strip()
    if ".." in field:
        first, last = field.split("..")
        return int(first, 16), int(last, 16)
    value = int(field, 16)
    return value, value


def data_lines(text):
    for raw in text.split("\n"):
        line = raw.split("#", 1)[0].strip()
        if line:
            yield line


def read_mapping_table(text):
    """Every code point's (status, mapping), as ranges: [(first, last, status, mapping)]."""
    ranges = []
    for line in data_lines(text):
        fields = [f.strip() for f in line.split(";")]
        first, last = parse_range(fields[0])
        status = fields[1]
        if status not in STATUSES:
            raise InputError("IdnaMappingTable.txt: status %r on %s is not one this knows (UTS 46 removed the "
                             "STD3 statuses in 16.0; a new one needs Idna.cpp to say what it means)" % (status, fields[0]))
        mapping = code_points(fields[2]) if len(fields) > 2 and status in ("mapped", "deviation") else []
        if status == "mapped" and not mapping:
            raise InputError("IdnaMappingTable.txt: %s is mapped to nothing" % fields[0])
        ranges.append((first, last, status, tuple(mapping)))
    ranges.sort()
    expected = 0
    for first, last, _, _ in ranges:
        if first != expected:
            raise InputError("IdnaMappingTable.txt does not cover U+%04X" % expected)
        expected = last + 1
    if expected != MAX_CODE_POINT + 1:
        raise InputError("IdnaMappingTable.txt stops at U+%04X" % (expected - 1))
    return ranges


def merged_mapping_ranges(ranges):
    """Neighbouring ranges of one status and no mapping made one."""
    merged = []
    for first, last, status, mapping in ranges:
        if merged and not mapping and not merged[-1][3] and merged[-1][2] == status and merged[-1][1] + 1 == first:
            merged[-1] = (merged[-1][0], last, status, mapping)
        else:
            merged.append((first, last, status, mapping))
    return merged


def read_unicode_data(text):
    """Per code point: general category, combining class, bidi class and canonical decomposition.
    The bidi class is read only to be held to DerivedBidiClass.txt's, which the tables take."""
    records = {}
    pending_first = None
    for line in text.split("\n"):
        if not line.strip():
            continue
        fields = line.split(";")
        if len(fields) != 15:
            raise InputError("UnicodeData.txt: a line of %d fields: %r" % (len(fields), line))
        cp = int(fields[0], 16)
        name, category, ccc, bidi, decomposition = fields[1], fields[2], int(fields[3]), fields[4], fields[5]
        if bidi not in BIDI_CLASSES:
            raise InputError("UnicodeData.txt: Bidi_Class %r of U+%04X is not one this knows" % (bidi, cp))
        canonical = tuple(code_points(decomposition)) if decomposition and not decomposition.startswith("<") else ()
        record = (category, ccc, bidi, canonical)
        if name.endswith(", First>"):
            pending_first = (cp, record)
            continue
        if name.endswith(", Last>"):
            if pending_first is None:
                raise InputError("UnicodeData.txt: a range's Last with no First at U+%04X" % cp)
            start, first_record = pending_first
            pending_first = None
            for value in range(start, cp + 1):
                records[value] = first_record
            continue
        records[cp] = record
    return records


def read_composition_exclusions(text):
    excluded = set()
    for line in data_lines(text):
        first, last = parse_range(line)
        excluded.update(range(first, last + 1))
    return excluded


def read_bidi_classes(text):
    """Every code point's Bidi_Class, [class] * 0x110000: the defaults of the @missing
    lines first, in the file's order, each over the ones before it (UAX #44, 4.2.10),
    then the data lines."""
    classes = [None] * (MAX_CODE_POINT + 1)
    for raw in text.split("\n"):
        if not raw.startswith("# @missing:"):
            continue
        fields = [f.strip() for f in raw[len("# @missing:"):].split(";")]
        first, last = parse_range(fields[0])
        if fields[1] not in BIDI_LONG_NAMES:
            raise InputError("DerivedBidiClass.txt: @missing value %r is not one this knows" % fields[1])
        classes[first:last + 1] = [BIDI_LONG_NAMES[fields[1]]] * (last - first + 1)
    if None in classes:
        raise InputError("DerivedBidiClass.txt's @missing lines do not cover U+%04X" % classes.index(None))
    for line in data_lines(text):
        fields = [f.strip() for f in line.split(";")]
        first, last = parse_range(fields[0])
        if fields[1] not in BIDI_CLASSES:
            raise InputError("DerivedBidiClass.txt: Bidi_Class %r is not one this knows" % fields[1])
        classes[first:last + 1] = [fields[1]] * (last - first + 1)
    return classes


def read_joining_types(text):
    types = {}
    for line in data_lines(text):
        fields = [f.strip() for f in line.split(";")]
        first, last = parse_range(fields[0])
        if fields[1] not in JOINING_TYPES:
            raise InputError("DerivedJoiningType.txt: Joining_Type %r is not one this knows" % fields[1])
        for cp in range(first, last + 1):
            types[cp] = fields[1]
    return types


def full_decomposition(cp, records, memo):
    if cp in memo:
        return memo[cp]
    record = records.get(cp)
    if not record or not record[3]:
        result = (cp,)
    else:
        result = ()
        for part in record[3]:
            result += full_decomposition(part, records, memo)
    memo[cp] = result
    return result


def normalisation_data(records, exclusions):
    """(decompositions {cp: full canonical decomposition}, combining classes {cp: ccc},
    compositions {(first, second): composite})."""
    memo = {}
    decompositions = {}
    for cp, record in records.items():
        if record[3]:
            if HANGUL_FIRST <= cp <= HANGUL_LAST:
                raise InputError("UnicodeData.txt gives a Hangul syllable a decomposition; NFC makes those itself")
            decompositions[cp] = full_decomposition(cp, records, memo)
    classes = {cp: record[1] for cp, record in records.items() if record[1] != 0}
    compositions = {}
    for cp, record in records.items():
        mapping = record[3]
        if len(mapping) != 2:
            continue  # a singleton, which is never composed (Full_Composition_Exclusion)
        if cp in exclusions:
            continue
        if record[1] != 0 or classes.get(mapping[0], 0) != 0:
            continue  # a non-starter decomposition (Full_Composition_Exclusion)
        compositions[(mapping[0], mapping[1])] = cp
    return decompositions, classes, compositions


def value_ranges(values, default):
    """{cp: value} as [(first, last, value)] for every value other than default."""
    ranges = []
    for cp in sorted(values):
        value = values[cp]
        if value == default:
            continue
        if ranges and ranges[-1][1] + 1 == cp and ranges[-1][2] == value:
            ranges[-1] = (ranges[-1][0], cp, value)
        else:
            ranges.append((cp, cp, value))
    return ranges


DATA_FILES = ["IdnaMappingTable.txt", "UnicodeData.txt", "CompositionExclusions.txt", "DerivedJoiningType.txt",
              "DerivedBidiClass.txt"]


def collect(directory, pinned):
    """The tables' data. Pinned, every input is read and held to its pin; not (another
    version's, for --differences), the five the tables are made from."""
    names = list(PINS) if pinned else DATA_FILES
    texts = {name: read_input(directory, name, pinned) for name in names}
    mapping = read_mapping_table(texts["IdnaMappingTable.txt"])
    records = read_unicode_data(texts["UnicodeData.txt"])
    exclusions = read_composition_exclusions(texts["CompositionExclusions.txt"])
    joining = read_joining_types(texts["DerivedJoiningType.txt"])
    decompositions, classes, compositions = normalisation_data(records, exclusions)
    marks = {cp: True for cp, record in records.items() if record[0].startswith("M")}
    # Punctuation 1, Symbol 2, Separator 3, Other 4 (Idna::CategoryGroup).
    category_groups = {cp: "PSZC".index(record[0][0]) + 1 for cp, record in records.items()
                       if record[0][0] in "PSZC"}
    classes_of_all = read_bidi_classes(texts["DerivedBidiClass.txt"])
    for cp, record in records.items():
        if classes_of_all[cp] != record[2]:
            raise InputError("U+%04X: UnicodeData.txt gives Bidi_Class %s and DerivedBidiClass.txt %s"
                             % (cp, record[2], classes_of_all[cp]))
    bidi = {cp: value for cp, value in enumerate(classes_of_all) if value != "L"}
    longest = processing_invariants(decompositions, classes, compositions, classes_of_all)
    mapping_invariants(mapping)
    return {
        "texts": texts,
        "mapping": mapping,
        "decompositions": decompositions,
        "classes": classes,
        "compositions": compositions,
        "marks": marks,
        "category_groups": category_groups,
        "bidi": bidi,
        "joining": joining,
        "longest_decomposition": longest,
    }


def mapping_invariants(mapping):
    """The data held to what Idna::KindOfMapping and SURBL's weighing of a host before it
    is mapped rest on (Idna.h, SURBL.cpp's CodePointsToMap_): a mapping that holds a
    full stop is a full stop alone - none is mapped to "a.b" or ".." - so that the full
    stops of a mapped name are those of the code points KindOfMapping says MapsToFullStop
    of, and a code point mapped to anything else ends no label and makes none empty."""
    full_stop = 0x2E
    for first, _, status, target in mapping:
        if status == "mapped" and full_stop in target and target != (full_stop,):
            raise InputError("U+%04X is mapped to a full stop beside other code points: a host weighed before it is "
                             "mapped would not count that full stop" % first)


def processing_invariants(decompositions, classes, compositions, bidi_classes):
    """The length of the longest full canonical decomposition, once the data is held to
    what Idna.cpp's ProcessLabels rests on when it maps, normalises and breaks a name a
    label at a time, and stops mapping a label that can never be short enough:

      - NFC neither makes nor moves a full stop, nor composes anything across one: U+002E
        is a starter, in no canonical decomposition and in no composition pair;
      - NFC does not change whether a text holds a code point that is R, AL or AN (which
        makes a name a bidi domain name): a code point is one exactly when its full
        canonical decomposition holds one;
      - NFC makes a text at most LongestDecomposition times shorter, which this returns
        (a Hangul syllable decomposes to three jamo at most)."""
    full_stop = 0x2E
    if classes.get(full_stop, 0) != 0:
        raise InputError("U+002E has a combining class: NFC could move a full stop")
    for cp, target in decompositions.items():
        if full_stop in target:
            raise InputError("U+%04X decomposes to a full stop: NFC could make one inside a label" % cp)
    for (first, second), composite in compositions.items():
        if full_stop in (first, second):
            raise InputError("U+%04X is composed with a full stop: NFC could compose across one" % composite)

    def right_to_left(cp):
        return bidi_classes[cp] in ("R", "AL", "AN")

    for cp, target in decompositions.items():
        if right_to_left(cp) != any(right_to_left(part) for part in target):
            raise InputError("U+%04X is %s and its canonical decomposition is not, or the other way round: NFC could "
                             "make or unmake a bidi domain name" % (cp, bidi_classes[cp]))
    return max([3] + [len(target) for target in decompositions.values()])


def hex_cp(cp):
    return "0x%04X" % cp


def wrap(items, indent="      ", width=110):
    """Comma-separated items, wrapped."""
    lines = []
    current = indent
    for item in items:
        piece = item + ","
        if len(current) + len(piece) + 1 > width and current.strip():
            lines.append(current.rstrip())
            current = indent
        current += piece + " "
    if current.strip():
        lines.append(current.rstrip())
    return "\n".join(lines)


HEADER = """// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// GENERATED FILE - do not edit. Written by build/generate-idna-tables.py from
// the Unicode %s data under Common/Idna/ucd:
// %s.
// Edit the generator, or move the data to another version as it says, and run
// it.
//
// The data is Unicode's, under the Unicode License v3 (ucd/unicode.LICENSE):
// Copyright (c) 1991-2026 Unicode, Inc.

"""


def tables_source(data):
    out = []
    out.append(HEADER % (VERSION, "IdnaMappingTable.txt, UnicodeData.txt, CompositionExclusions.txt,\n"
                                  "// DerivedJoiningType.txt and DerivedBidiClass.txt"))
    out.append("#include \"StdAfx.h\"\n")
    out.append("#include \"IdnaTables.h\"\n\n")
    out.append("namespace HM\n{\n   namespace IdnaTables\n   {\n")
    out.append("      const char *const UnicodeVersion = \"%s\";\n\n" % VERSION)

    mapping = merged_mapping_ranges(data["mapping"])
    mapping_data = []
    rows = []
    for first, last, status, target in mapping:
        offset = len(mapping_data)
        mapping_data.extend(target)
        if len(target) > 255:
            raise InputError("a mapping of more than 255 code points")
        rows.append("{ %s, %s, %s, %d, %d }" % (hex_cp(first), hex_cp(last), STATUS_NAMES[status], len(target), offset if target else 0))
    out.append("      // IdnaMappingTable.txt: every code point's status, and the code points\n"
               "      // MappingData holds at offset for one that is mapped or a deviation.\n"
               "      // Sorted, and covering U+0000..U+10FFFF without a gap.\n")
    out.append("      const MappingRange MappingRanges[] =\n      {\n")
    for row in rows:
        out.append("         %s,\n" % row)
    out.append("      };\n\n")
    out.append("      const size_t MappingRangeCount = sizeof(MappingRanges) / sizeof(MappingRanges[0]);\n\n")
    out.append("      const unsigned int MappingData[] =\n      {\n")
    out.append(wrap([hex_cp(cp) for cp in mapping_data] or ["0"], "         ") + "\n")
    out.append("      };\n\n")

    classes = value_ranges(data["classes"], 0)
    out.append("      // UnicodeData.txt: Canonical_Combining_Class, for every code point whose\n"
               "      // class is not 0. Sorted.\n")
    out.append("      const ValueRange CombiningClasses[] =\n      {\n")
    out.append(wrap(["{ %s, %s, %d }" % (hex_cp(a), hex_cp(b), v) for a, b, v in classes], "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t CombiningClassCount = sizeof(CombiningClasses) / sizeof(CombiningClasses[0]);\n\n")

    decomposition_data = []
    decomposition_rows = []
    for cp in sorted(data["decompositions"]):
        target = data["decompositions"][cp]
        decomposition_rows.append("{ %s, %d, %d }" % (hex_cp(cp), len(target), len(decomposition_data)))
        decomposition_data.extend(target)
    out.append("      // UnicodeData.txt: each code point's full canonical decomposition, its\n"
               "      // mapping decomposed again until nothing in it decomposes, at offset in\n"
               "      // DecompositionData. Hangul syllables are not here: NFC decomposes and\n"
               "      // composes them by arithmetic. Sorted.\n")
    out.append("      const Decomposition Decompositions[] =\n      {\n")
    out.append(wrap(decomposition_rows, "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t DecompositionCount = sizeof(Decompositions) / sizeof(Decompositions[0]);\n\n")
    out.append("      const unsigned int DecompositionData[] =\n      {\n")
    out.append(wrap([hex_cp(cp) for cp in decomposition_data], "         ") + "\n")
    out.append("      };\n\n")
    out.append("      // The longest of them, and of a Hangul syllable's (three jamo): NFC makes a\n"
               "      // text this many times shorter at most.\n")
    out.append("      const unsigned int LongestDecomposition = %d;\n\n" % data["longest_decomposition"])

    out.append("      // The primary composites: each canonical decomposition of two code points\n"
               "      // that is not excluded from composition (CompositionExclusions.txt, and\n"
               "      // the non-starter decompositions), as (first, second, composite). Sorted\n"
               "      // by first, then second.\n")
    out.append("      const Composition Compositions[] =\n      {\n")
    out.append(wrap(["{ %s, %s, %s }" % (hex_cp(a), hex_cp(b), hex_cp(c)) for (a, b), c in sorted(data["compositions"].items())],
                    "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t CompositionCount = sizeof(Compositions) / sizeof(Compositions[0]);\n\n")

    marks = value_ranges(data["marks"], False)
    out.append("      // UnicodeData.txt: the code points of General_Category Mark (Mn, Mc,\n"
               "      // Me), which a label may not begin with. Sorted.\n")
    out.append("      const ValueRange Marks[] =\n      {\n")
    out.append(wrap(["{ %s, %s, 1 }" % (hex_cp(a), hex_cp(b)) for a, b, _ in marks], "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t MarkCount = sizeof(Marks) / sizeof(Marks[0]);\n\n")

    category_groups = value_ranges(data["category_groups"], 0)
    out.append("      // UnicodeData.txt: the group of the General_Category of each code point\n"
               "      // the file gives one of Punctuation, Symbol, Separator or Other (P*, S*,\n"
               "      // Z*, C*) - 1, 2, 3 and 4 (Idna::CategoryGroup); an unassigned code\n"
               "      // point (Cn), which the file does not list, is none of them here. Sorted.\n")
    out.append("      const ValueRange CategoryGroups[] =\n      {\n")
    out.append(wrap(["{ %s, %s, %d }" % (hex_cp(a), hex_cp(b), value) for a, b, value in category_groups],
                    "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t CategoryGroupCount = sizeof(CategoryGroups) / sizeof(CategoryGroups[0]);\n\n")

    bidi = value_ranges(data["bidi"], "L")
    out.append("      // DerivedBidiClass.txt: Bidi_Class, for every code point whose class is\n"
               "      // not L (BidiL is the value for every other) - an unassigned one's the\n"
               "      // default the file's @missing lines give it. Sorted.\n")
    out.append("      const ValueRange BidiClasses[] =\n      {\n")
    out.append(wrap(["{ %s, %s, Bidi%s }" % (hex_cp(a), hex_cp(b), v) for a, b, v in bidi], "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t BidiClassCount = sizeof(BidiClasses) / sizeof(BidiClasses[0]);\n\n")

    joining = value_ranges(data["joining"], "U")
    out.append("      // DerivedJoiningType.txt: Joining_Type, for every code point whose type is\n"
               "      // not U (JoiningU is the value for every other). Sorted.\n")
    out.append("      const ValueRange JoiningTypes[] =\n      {\n")
    out.append(wrap(["{ %s, %s, Joining%s }" % (hex_cp(a), hex_cp(b), v) for a, b, v in joining], "         ") + "\n")
    out.append("      };\n\n")
    out.append("      const size_t JoiningTypeCount = sizeof(JoiningTypes) / sizeof(JoiningTypes[0]);\n")

    out.append("   }\n}\n")
    return "".join(out)


def cpp_string(data):
    """A narrow C++ literal holding these bytes: ASCII printable as itself, anything
    else as a three-digit octal escape (which no following digit can extend), and
    '?' escaped so that no two of them make a trigraph."""
    out = ['"']
    for byte in data:
        if byte == 0x22:
            out.append('\\"')
        elif byte == 0x5C:
            out.append("\\\\")
        elif byte == 0x3F:
            out.append("\\?")
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append("\\%03o" % byte)
    out.append('"')
    return "".join(out)


def test_lines(text):
    """IdnaTestV2.txt's test lines: each line's text before its comment, without the
    spaces and tabs at its end."""
    lines = []
    for raw in text.split("\n"):
        line = raw.split("#", 1)[0].rstrip(" \t")
        if line.strip():
            lines.append(line)
    return lines


def tests_source(data):
    lines = test_lines(data["texts"]["IdnaTestV2.txt"])
    out = []
    out.append(HEADER % (VERSION, "IdnaTestV2.txt"))
    out.append("#include \"StdAfx.h\"\n")
    out.append("#include \"IdnaTables.h\"\n\n")
    out.append("namespace HM\n{\n   namespace IdnaTables\n   {\n")
    out.append("      // IdnaTestV2.txt, every test line in order, as the file writes it with its\n"
               "      // comment cut off; the file's FORMAT section says how one is read, and\n"
               "      // IdnaTester reads it so. In UTF-8, with the escapes the file writes some\n"
               "      // characters in left as it writes them.\n")
    out.append("      const char *const TestLines[] =\n      {\n")
    for line in lines:
        out.append("         %s,\n" % cpp_string(line.encode("utf-8")))
    out.append("      };\n\n")
    out.append("      const size_t TestLineCount = sizeof(TestLines) / sizeof(TestLines[0]);\n")
    out.append("   }\n}\n")
    return "".join(out)


def differences_header(old, new):
    """The code points whose UTS 46 data differ between two versions' data, as a
    sorted C++ table of ranges."""
    def per_code_point(ranges):
        table = {}
        for first, last, status, mapping in ranges:
            for cp in range(first, last + 1):
                table[cp] = (status, mapping)
        return table

    old_mapping = per_code_point(old["mapping"])
    new_mapping = per_code_point(new["mapping"])
    differ = set()
    for cp in range(MAX_CODE_POINT + 1):
        if old_mapping[cp] != new_mapping[cp]:
            differ.add(cp)
    for key in ("decompositions", "classes", "marks", "bidi", "joining"):
        a, b = old[key], new[key]
        for cp in set(a) | set(b):
            if a.get(cp) != b.get(cp):
                differ.add(cp)
    for pair in set(old["compositions"]) | set(new["compositions"]):
        if old["compositions"].get(pair) != new["compositions"].get(pair):
            differ.update(pair)
            for side in (old, new):
                if pair in side["compositions"]:
                    differ.add(side["compositions"][pair])
    ranges = value_ranges({cp: True for cp in differ}, False)
    out = ["// GENERATED by build/generate-idna-tables.py --differences: the code points whose\n"
           "// UTS 46 data (mapping table, NFC, General_Category Mark, Bidi_Class,\n"
           "// Joining_Type) differ between the older Unicode data ada's tables come from and\n"
           "// the server's own (%s). %d code points in %d ranges.\n" % (VERSION, len(differ), len(ranges)),
           "#pragma once\n",
           "static const unsigned int IdnaVersionDifferences[][2] =\n{\n"]
    out.append(wrap(["{ %s, %s }" % (hex_cp(a), hex_cp(b)) for a, b, _ in ranges] or ["{ 0xFFFFFFFF, 0xFFFFFFFF }"], "   ") + "\n")
    out.append("};\n")
    return "".join(out)


def read_text(path):
    if not os.path.exists(path):
        return None
    return io.open(path, "r", encoding="utf-8").read().replace("\r\n", "\n")


def main(argv):
    try:
        if "--differences" in argv:
            old_dir = argv[argv.index("--differences") + 1]
            output = argv[argv.index("--output") + 1]
            new = collect(UCD, True)
            old = collect(old_dir, False)
            io.open(output, "w", encoding="utf-8", newline="\n").write(differences_header(old, new))
            print("Wrote %s." % output)
            return 0

        data = collect(UCD, True)
        outputs = [(OUT_TABLES, tables_source(data)), (OUT_TESTS, tests_source(data))]
    except InputError as problem:
        print("generate-idna-tables: %s" % problem)
        print("Nothing written.")
        return 1

    if "--check" in argv:
        stale = [path for path, text in outputs if read_text(path) != text]
        for path in stale:
            print("%s is not what build/generate-idna-tables.py writes from the Unicode data under %s: run it "
                  "and commit what it writes." % (os.path.relpath(path, ROOT), os.path.relpath(UCD, ROOT)))
        if stale:
            return 1
        print("IdnaTables.cpp and IdnaTestData.cpp are what the Unicode %s data under %s make." % (VERSION, os.path.relpath(UCD, ROOT)))
        return 0

    for path, text in outputs:
        io.open(path, "w", encoding="utf-8", newline="\n").write(text)
        print("Wrote %s." % os.path.relpath(path, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
