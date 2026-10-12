<!--
  https://www.progressiverobot.com
  Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
  SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Unicode's data for UTS 46

The server reads the host of a link as a browser reads it - the WHATWG URL
standard's "domain to ASCII", which is Unicode's UTS 46 ToASCII (`../Idna.h`).
The tables it does that from are generated out of the Unicode 18.0.0 files in
this directory by `build/generate-idna-tables.py`, and compiled into the server
on Windows and Linux alike, so that both read a host the same way whatever
Unicode version the operating system has.

| File | From | What the generator takes from it |
|---|---|---|
| `IdnaMappingTable.txt` | <https://www.unicode.org/Public/18.0.0/idna/IdnaMappingTable.txt> | each code point's status (valid, ignored, mapped, deviation, disallowed) and what it maps to |
| `IdnaTestV2.txt` | <https://www.unicode.org/Public/18.0.0/idna/IdnaTestV2.txt> | Unicode's conformance tests for UTS 46, every line of which `IdnaTester` runs |
| `UnicodeData.txt` | <https://www.unicode.org/Public/18.0.0/ucd/UnicodeData.txt> | the canonical decompositions and combining classes NFC needs, and General_Category Mark |
| `CompositionExclusions.txt` | <https://www.unicode.org/Public/18.0.0/ucd/CompositionExclusions.txt> | the composites NFC never makes |
| `DerivedJoiningType.txt` | <https://www.unicode.org/Public/18.0.0/ucd/extracted/DerivedJoiningType.txt> | Joining_Type, for RFC 5892's rule for ZERO WIDTH NON-JOINER |
| `DerivedBidiClass.txt` | <https://www.unicode.org/Public/18.0.0/ucd/extracted/DerivedBidiClass.txt> | Bidi_Class of every code point, an unassigned one's default among them, for RFC 5893's rule |
| `unicode.LICENSE` | <https://www.unicode.org/license.txt> | the Unicode License v3 the files are under |

The files are kept byte for byte as unicode.org serves them, LF line endings
and all (`.gitattributes` marks this directory `-text`), and the generator holds
each to its SHA-256 and to the version its header names before it writes
anything. Nothing fetches them again: the build reads only these copies.

They are not this project's files and carry no header of its own. The licence
asks that its copyright and permission notice go with every copy of the data or
appear in its documentation, which `unicode.LICENSE` here, the generated files'
headers, `hmailserver/docs/Licenses/License - Unicode data.txt`, the packages'
copyright file and the Windows installer's licence do.

To move to another Unicode version: replace the six data files with that
version's, put their SHA-256s and the version in the generator, run
`python3 build/generate-idna-tables.py`, run `IdnaTester` on Windows (the class
tests) and Linux (`idna-tests`), and say in `CHANGELOG.md` what changed.
