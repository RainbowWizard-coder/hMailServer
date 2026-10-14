<!--
  https://www.progressiverobot.com
  Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
  SPDX-License-Identifier: AGPL-3.0-or-later
-->

# HTML's named character references

The SURBL test and the sender lists' URL entries read the links of an HTML
part as a browser reads them, its character references first
(`../SURBL.cpp`, `DecodeCharacterReferences_`): `<a href="http://&eopf;vil.example/">`
goes to evil.example, since `&eopf;` is U+1D556 MATHEMATICAL DOUBLE-STRUCK SMALL E
and UTS 46 maps it to an `e`. The table of every named reference HTML defines,
`../HtmlReferencesData.cpp`, is generated out of the file in this directory by
`build/generate-html-references.py`, and compiled into the server on Windows
and Linux alike.

| File | From | What the generator takes from it |
|---|---|---|
| `entities.json` | <https://html.spec.whatwg.org/entities.json> | each named character reference - 2,231 names, the 106 HTML also reads without their `;` among them - and the one or two code points it stands for |
| `entities.LICENSE` | the HTML standard's terms (its "Intellectual property rights" section) and <https://opensource.org/license/bsd-3-clause> | the BSD 3-Clause License the list is under as a portion of the standard incorporated into source code |

`entities.json` is kept byte for byte as the WHATWG serves it, LF line endings
and all (`.gitattributes` marks this directory `-text`), and the generator holds
it to its SHA-256 before it writes anything. Nothing fetches it again: the
build reads only this copy. The standard says the list "is static and will not
be expanded or changed in the future".

It is not this project's file and carries no header of its own. The licence
asks that its copyright notice, conditions and disclaimer go with the source
and, in a binary, with its documentation. With the source, `entities.LICENSE`
here, the generated file's header and
`hmailserver/docs/Licenses/License - HTML named character references.txt` do.
With a binary: on Linux, every install - the packages and the AppImages - puts
`entities.LICENSE` beside the server as
`/usr/share/hmailserver/licenses/HTML-named-character-references.txt` (the
CMake install rules, and `build-appimage.sh` for the AppImages), and the `.deb`
and the `.rpm` carry the Debian copyright file as well; on Windows, the
installer's licence carries it.
