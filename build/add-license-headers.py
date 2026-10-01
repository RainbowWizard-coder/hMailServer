#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""
Puts the project's header at the top of every source file, in one form, and
reports what it did - or, with --check, what it WOULD do and exits non-zero if
that is anything.

The header is three lines, in the file's own comment syntax:

    https://www.progressiverobot.com
    Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
    SPDX-License-Identifier: AGPL-3.0-or-later

It goes after a UTF-8 byte-order mark, after a shebang or a Python encoding
line, and after an XML declaration, since each of those has to come first, and
a blank line separates it from whatever the file goes on to say. Whatever
stood in its place before is replaced: the original author's per-file notice,
an earlier wording of this one, the same lines in another order, or an SPDX
line that an earlier version of this script put at the foot of a long opening
comment. Nothing below the header is touched - a comment that opens a file
stays, a third-party notice in a block comment stays (Mime.cpp, the Mime
library's), and a file keeps its own line endings and its byte-order mark.
A file that opens with a third party's notice whose terms forbid changing it
and passing the file off as another's (OWN_NOTICE_FIRST, ScriptSite.h) gets no
header: it keeps that notice first and says below it that it is altered, with
the project's address and an SPDX line of its own, which --check holds.

Why a script and not an afternoon with an editor: 2,000 files, three comment
syntaxes, and four traps that each silently corrupt a file if handled by hand -

  * 85 files start with a UTF-8 byte-order mark. The header goes AFTER it, never
    before; a BOM in the middle of a file is a compile error in C++ and an
    invisible garbage character in C#.
  * Nearly every file is CRLF, a few are LF. Each keeps what it has.
  * Python and shell scripts may start with a shebang, which must stay on line 1.
  * PowerShell comment-based help (<# ... #>) is only recognised as the
    script's help when nothing but comments and blank lines precede it, which
    plain # comment lines satisfy - so the header is plain # lines.

Which files: the source proper (.cpp .h .hpp .idl .cs .js .xaml .ps1 .psm1 .py
.sh, CMakeLists.txt, the packaging's PKGBUILD, Dockerfile and maintainer
scripts) gets the header whether or not it had one. A .resx, a
Markdown document, a packaged .ini/.conf/.service/.logrotate has its header
made canonical when it carries one and is left alone when it does not: the
WinForms designer rewrites a .resx whole on every save, the documents are not
source, and the packaged configuration is read by other programs' parsers.

Generated files are skipped on purpose: MIDL rewrites dlldata.c and
hMailServer_i.c on every build, so a header there lasts until the next compile.
The generators that write source into the tree (build/generate-*.ps1, .py)
emit the same three lines and the blank line, so a regenerated file passes
--check. The third-party sources unpacked under libraries/ and the zlib under
the server are not ours to stamp and are inventoried in
hmailserver/docs/third-party-binaries.json instead, and nor is the QuickJS
beside it (hmailserver/docs/ThirdPartyBinaries.md says where it came from); the
build scripts at the top of libraries/ are the project's own and carry the
header.

Usage:
  python build/add-license-headers.py            # apply
  python build/add-license-headers.py --check    # CI mode: report, change nothing
"""

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = [
    'https://www.progressiverobot.com',
    'Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd',
    'SPDX-License-Identifier: AGPL-3.0-or-later',
]

# What the header replaces, wherever the lines sit in a file's opening comment:
# the notices this tree has carried, in every wording, and any SPDX line.
REPLACED = [re.compile(p) for p in (
    r'^Copyright \(c\)\s*(?:\d{4}(?:\s*-\s*\d{4})?)?\s*Martin Knafve / hMailServer\.com\.?$',
    r'^https?://(?:www\.)?progressiverobot\.com/?$',
    r'^Copyright \(c\) \d{4} Christopher Holloway / Progressive Robot Ltd'
    r'(?: and the hMailServer contributors)?\.?$',
    r'^Copyright \(c\) \d{4} hMailServer$',
    r'^SPDX-License-Identifier: \S+$',
)]

# Comment syntax per extension: line comments, or - for XML and Markdown - one
# comment element at the top, after the XML declaration when there is one.
LINE_COMMENT = {
    '.cpp': '//', '.h': '//', '.hpp': '//', '.idl': '//', '.cs': '//', '.js': '//',
    '.ps1': '#', '.psm1': '#', '.py': '#', '.sh': '#', '.cmake': '#',
    '.conf': '#', '.service': '#', '.logrotate': '#',
    # The Nix flake (flake.nix, nix/), and the OpenRC service and the Alpine package's
    # install scripts under the Linux packaging.
    '.nix': '#', '.initd': '#', '.confd': '#', '.pre-install': '#', '.post-upgrade': '#',
    '.ini': ';',
}
BLOCK_COMMENT = {'.xaml', '.resx', '.md'}
BY_NAME = {'CMakeLists.txt': '.cmake',
           # The Linux packaging's files without an extension.
           'PKGBUILD': '.sh', 'APKBUILD': '.sh', 'Dockerfile': '.sh', 'autoban-hook': '.sh'}
# The maintainer scripts of the .deb and the .rpm, named by their hook, and a
# Debian source package's debian/rules. Only these names: the rest of a debian/
# directory without an extension - changelog, control, copyright, watch - is read
# by dpkg's and uscan's own parsers, and a changelog or a copyright file cannot
# begin with a comment (platform/packaging/debian-src/debian).
SCRIPTLET_DIRS = {'debian', 'rpm'}
SCRIPTLET_NAMES = {'preinst', 'postinst', 'prerm', 'postrm', 'pre', 'post', 'preun', 'postun', 'rules'}

# A header is made canonical here when the file has one, and never added.
NORMALISE_ONLY = {'.resx', '.md', '.ini', '.conf', '.service', '.logrotate'}

SKIP_DIRS = {'.git', 'obj', 'bin', 'publish', 'packages', 'Output',
             'x64', 'Debug', 'Release', 'node_modules', 'DotNet', 'coverage',
             '.nuget', '.obsidian',
             # Build output: git ignores /logs/, and since 15 September 2026
             # build/make-package-manifests.ps1 renders a Chocolatey install
             # script into it, which is generated and not source.
             'logs',
             # Vendored third-party source under its own licence (see docs/Licenses).
             'quickjs', 'zlib'}

# Walked for its own files, never descended: the project's build scripts sit
# at the top of libraries/, and the sources they unpack below them.
NO_DESCEND = {os.path.join(ROOT, 'libraries')}

# MIDL output, regenerated on every build. hMailServer.h is the type library's
# header, ignored by git beside the other three and tracked nowhere, so a
# worktree that has been built is judged the same as a fresh checkout.
SKIP_FILES = {'dlldata.c', 'hMailServer_i.c', 'hMailServer_p.c', 'hMailServer.h'}

# Files that open with a third party's notice whose own terms keep it unchanged
# and forbid passing the file off as another's, so the project's header, which
# would claim the file, does not go above it: below the notice the
# file says it is altered, by whom and under what, and ends that with the
# project's address and an SPDX line of its own. Left alone when applying;
# --check holds each to it - there, opening with the notice, unchanged, and not
# the project's header, and carrying that SPDX line in its opening comment.
# Paths from the top of the tree, with forward slashes, each to (how many lines
# the notice is, the SHA-256 of those lines joined by LF with any CR taken off,
# the SPDX line): the notice is held by its digest because its terms forbid any
# change to it, a reworded line or a line taken out.
OWN_NOTICE_FIRST = {
    # VisionTech's ATL Active Script host (2001), which only hMailServer.exe
    # compiles: "a) This notice may not be removed or changed in any source
    # distribution. b) Altered source versions must be include [sic] a note to
    # that effect, and must not be misrepresented as the original. c) The origin
    # of this software may not be misrepresented"
    # (hmailserver/docs/SourceTarball.md). Until 6.3.6 this script put the
    # project's AGPL header above it, which claimed the file as the project's
    # and said nothing of the alteration. The notice's 31 lines are the ones the
    # 2026-06-12 baseline (fe45f43c3) opens the file with.
    'hmailserver/source/Server/Common/Scripting/ScriptSite.h': (
        31, '83fd885486bb69866746536d0fc3016097e0ce660b0f27c1d4e18d0b0c52e13b',
        'SPDX-License-Identifier: LicenseRef-ScriptSite AND AGPL-3.0-or-later'),
}

BOM = b'\xef\xbb\xbf'
UTF16_BOMS = (b'\xff\xfe', b'\xfe\xff')
CODING = re.compile(rb'^#.*coding[:=]')


def candidates():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [] if dirpath in NO_DESCEND else sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if name in SKIP_FILES:
                continue
            if os.path.relpath(os.path.join(dirpath, name), ROOT).replace(os.sep, '/') in OWN_NOTICE_FIRST:
                continue
            ext = BY_NAME.get(name) or os.path.splitext(name)[1].lower()
            if not ext and os.path.basename(dirpath) in SCRIPTLET_DIRS and name in SCRIPTLET_NAMES:
                ext = '.sh'
            if ext in LINE_COMMENT or ext in BLOCK_COMMENT:
                yield os.path.join(dirpath, name), ext


def replaced(text):
    return any(p.match(text) for p in REPLACED)


def comment_text(line, marker):
    """The text of a line comment of `marker`, or None when the line is not one."""
    s = line.strip()
    if not s.startswith(marker):
        return None
    return s[len(marker):].strip().decode('utf-8', 'replace')


def with_line_comments(lines, marker, cr, add):
    m = marker.encode()

    # A shebang stays on line 1, a Python encoding line with it, and a
    # PKGBUILD's Maintainer line, which opens the file by convention - in an
    # APKBUILD after its Contributor lines, which aports puts first.
    start = 0
    if lines and lines[0].startswith(b'#!'):
        start = 1
    if start < len(lines) and CODING.match(lines[start]):
        start += 1
    while start < len(lines) and lines[start].startswith(b'# Contributor:'):
        start += 1
    if start < len(lines) and lines[start].startswith(b'# Maintainer:'):
        start += 1

    # The opening comment: every comment or blank line from the top.
    end = start
    while end < len(lines):
        s = lines[end].strip()
        if s == b'' or s.startswith(m):
            end += 1
        else:
            break

    block = lines[start:end]
    kept = []
    found = False
    for line in block:
        text = comment_text(line, m)
        if text is not None and replaced(text):
            found = True
        else:
            kept.append(line)
    if not found and not add:
        return None

    # The empty comment lines and blank lines that separated the old header
    # from the text below it; the blank line below puts the new one back.
    while kept and kept[0].strip() in (b'', m):
        kept.pop(0)

    header = [m + b' ' + h.encode() + cr for h in HEADER]
    rest = kept + lines[end:]
    if rest and rest[0].strip() != b'':
        header.append(cr)
    return lines[:start] + header + rest


def with_block_comment(lines, ext, cr, add):
    # An XML declaration has to be the first thing in the file.
    start = 1 if lines and lines[0].lstrip().startswith(b'<?xml') else 0

    # An existing header: a comment element at the top made only of the lines
    # the header replaces. Any other comment there is the file's own.
    end = start
    found = False
    if start < len(lines) and lines[start].lstrip().startswith(b'<!--'):
        j = start
        while j < len(lines) and b'-->' not in lines[j]:
            j += 1
        if j < len(lines):
            inner = b'\n'.join(lines[start:j + 1]).strip()[4:]
            inner = inner[:inner.rfind(b'-->')]
            texts = [t.strip().decode('utf-8', 'replace') for t in inner.split(b'\n')]
            texts = [t for t in texts if t]
            if texts and all(replaced(t) for t in texts):
                found = True
                end = j + 1
    if not found and not add:
        return None

    header = [b'<!--' + cr] + [b'  ' + h.encode() + cr for h in HEADER] + [b'-->' + cr]
    rest = lines[end:]
    if ext == '.md':
        while rest and rest[0].strip() == b'':
            rest.pop(0)
        if rest:
            header.append(cr)
    return lines[:start] + header + rest


def normalised(raw, ext):
    """The file's bytes with the canonical header, or None to leave it alone."""
    if raw.startswith(UTF16_BOMS):
        return None
    bom = raw.startswith(BOM)
    body = raw[len(BOM):] if bom else raw
    # Lines are split on LF with any CR kept attached to its line, so a file
    # with MIXED endings (one test file had a single CRLF among 268 LF lines)
    # is still seen line by line. New lines take the ending the file's own
    # first line uses.
    cr = b'\r' if body.split(b'\n', 1)[0].endswith(b'\r') else b''
    lines = body.split(b'\n')
    add = ext not in NORMALISE_ONLY
    if ext in BLOCK_COMMENT:
        new_lines = with_block_comment(lines, ext, cr, add)
    else:
        new_lines = with_line_comments(lines, LINE_COMMENT[ext], cr, add)
    if new_lines is None:
        return None
    out = b'\n'.join(new_lines)
    return BOM + out if bom else out


def own_notice_problems():
    """What is wrong with each OWN_NOTICE_FIRST file, one sentence each."""
    problems = []
    for rel, (notice_lines, notice_sha256, spdx) in sorted(OWN_NOTICE_FIRST.items()):
        path = os.path.join(ROOT, *rel.split('/'))
        ext = os.path.splitext(rel)[1].lower()
        marker = LINE_COMMENT.get(ext)
        if marker is None:
            problems.append('%s: no line-comment syntax is known for its extension' % rel)
            continue
        try:
            with open(path, 'rb') as f:
                raw = f.read()
        except OSError:
            problems.append('%s is listed in OWN_NOTICE_FIRST and is not in the tree' % rel)
            continue
        body = raw[len(BOM):] if raw.startswith(BOM) else raw
        lines = body.split(b'\n')
        m = marker.encode()
        opening = []
        for line in lines:
            s = line.strip()
            if s == b'' or s.startswith(m):
                opening.append(s)
            else:
                break
        first = comment_text(lines[0], m) if lines else None
        if first is not None and replaced(first):
            problems.append('%s opens with the project header, above the third party\'s notice' % rel)
        notice = b'\n'.join(line.rstrip(b'\r') for line in lines[:notice_lines])
        if len(lines) < notice_lines or hashlib.sha256(notice).hexdigest() != notice_sha256:
            problems.append('%s does not open with the third party\'s notice as its terms keep it: its first %d lines '
                            'are not that notice, unchanged' % (rel, notice_lines))
        texts = [comment_text(s, m) for s in opening]
        if spdx not in texts:
            problems.append('%s has no "%s" line in its opening comment' % (rel, spdx))
    return problems


def main():
    check = '--check' in sys.argv
    problems = own_notice_problems()
    touched = []
    for path, ext in candidates():
        with open(path, 'rb') as f:
            raw = f.read()
        out = normalised(raw, ext)
        if out is None or out == raw:
            continue
        touched.append(os.path.relpath(path, ROOT))
        if not check:
            with open(path, 'wb') as f:
                f.write(out)

    for problem in problems:
        print('%s.' % problem)
    if problems:
        print('A file of OWN_NOTICE_FIRST keeps its third party\'s notice first and unchanged, and its own SPDX line')
        print('below it; this script does not write either, so put them back by hand.')
        print()

    if check:
        if touched:
            print('%d source file(s) do not open with the project header in its one form:' % len(touched))
            for rel in touched[:40]:
                print('  %s' % rel)
            if len(touched) > 40:
                print('  ... and %d more' % (len(touched) - 40))
            print()
            print('Run  python build/add-license-headers.py  to put it there.')
            return 1
        if problems:
            return 1
        print('Every source file opens with the project header, and the %d of OWN_NOTICE_FIRST with their own.'
              % len(OWN_NOTICE_FIRST))
        return 0

    print('Put the project header on %d file(s).' % len(touched))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
