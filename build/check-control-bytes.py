# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""No tracked text file holds a control character that nobody meant to write.

A tool that reads an escape as the character it stands for writes that character
where the escape was meant to go, and nothing on the screen shows it. On 12
October 2026 the tree held six files with such a byte. RegressionEnvironment.md
said `installation\\Extras<BEL>za.exe`, its "\\7" taken for an octal escape, and
the help page the server serves from it carried the BEL. build/check-cli.py's
pattern for a colour code began with an ESC where "\\x1b" was written. Four C#
tests held SOH, STX, ETX, EOT and DEL raw where escapes were meant: those held
the values meant, and are written as four-digit escapes now, since C#'s "\\x"
takes up to four hex digits and "\\x01auth" is U+001A followed by "uth". A
seventh file, MessageIndexing.cs, had a line ending CR CR LF.

A file is text unless .gitattributes says it is not (-text, or binary) or it
holds a NUL in its first 8,000 bytes, which is git's own test. In a text file
this refuses:

  * a C0 control character other than tab, line feed and carriage return -
    0x00 to 0x08, 0x0B, 0x0C and 0x0E to 0x1F - and DEL, 0x7F;
  * a carriage return not followed by a line feed. git also stores a file that
    holds one as it is, its line ends left unnormalised.

The few files that hold such a byte on purpose are listed in EXPECTED, with the
bytes each may hold and why. A listed file that no longer holds its byte, or is
no longer tracked, is reported too, so that the list does not outlive its
reasons.

The check proves its own negative controls on every run, on made-up files: a
BEL, an ESC, a SOH, a DEL, CR CR LF, a carriage return that ends a file, a NUL
past the first 8,000 bytes, and a listed file holding a byte it is not listed
for must each be refused; tab, CR LF and LF line ends, UTF-8 text, a NUL in the
first 8,000 bytes, a file .gitattributes calls not text, and a listed file's own
byte must pass. It also fails when git lists no file of the tree, this script
among them.

Run from the repository root: python build/check-control-bytes.py
To fix a report, write the escape the character was meant as. Where a file
needs the byte itself, add the file to EXPECTED with the reason.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELF = 'build/check-control-bytes.py'

# Every byte below is named by its number, so that this file holds none of the
# bytes it refuses and no tool on the way to it can decode an escape into one.
NUL, TAB, LF, CR, DEL = 0, 9, 10, 13, 127
NUL_BYTE, TAB_BYTE, LF_BYTE, CR_BYTE = bytes([NUL]), bytes([TAB]), bytes([LF]), bytes([CR])
NAMES = ['NUL', 'SOH', 'STX', 'ETX', 'EOT', 'ENQ', 'ACK', 'BEL', 'BS', 'HT', 'LF', 'VT', 'FF', 'CR',
         'SO', 'SI', 'DLE', 'DC1', 'DC2', 'DC3', 'DC4', 'NAK', 'SYN', 'ETB', 'CAN', 'EM', 'SUB', 'ESC',
         'FS', 'GS', 'RS', 'US']
STRAY = frozenset([b for b in range(32) if b not in (TAB, LF, CR)] + [DEL])
PATTERN = re.compile(b'[' + b''.join(re.escape(bytes([b])) for b in sorted(STRAY)) + b']|'
                     + re.escape(CR_BYTE) + b'(?!' + re.escape(LF_BYTE) + b')')
# git's own test for a binary file: a NUL among its first 8,000 bytes.
BINARY_PROBE = 8000

WORDPAD = 'the NUL WordPad ends an RTF file with, its last byte; the licence is kept as it was received'
# The files that hold such a byte on purpose: the bytes each may hold, and why.
EXPECTED = {
    'hmailserver/docs/Licenses/License - MariaDB Connector C.txt':
        (frozenset([12]), "the page breaks (form feeds) of the GNU LGPL 2.1 text, as the Free Software "
                          "Foundation publishes it and Connector/C's COPYING.LIB carries it"),
    'hmailserver/docs/Licenses/License - Fat Cow Icons.rtf': (frozenset([NUL]), WORDPAD),
    'libraries/License - Fat Cow Icons.rtf': (frozenset([NUL]), WORDPAD),
    'libraries/License - Microsoft SQL Server Compact Edition.rtf': (frozenset([NUL]), WORDPAD),
    'hmailserver/source/DBScripts/Upgrade6021to6022MSSQLCE.sql':
        (frozenset([CR]), 'CR CR LF inside the QUOTA_WARNING text: SQLScriptParser folds CR LF to LF and then '
                          'splits a Compact script at each blank line, so the message keeps CR LF line breaks '
                          'and is not split'),
}


def name(byte):
    return 'DEL' if byte == DEL else NAMES[byte]


def is_text(head, not_text):
    """Whether a file is text: .gitattributes does not say otherwise, and its first
    8,000 bytes (head) hold no NUL."""
    return not not_text and NUL_BYTE not in head[:BINARY_PROBE]


def show(data, offset):
    """The line that holds offset, each control character in it named."""
    start = data.rfind(LF_BYTE, 0, offset) + 1
    end = data.find(LF_BYTE, offset)
    if end < 0:
        end = len(data)
    elif end > start and data[end - 1] == CR:
        end -= 1
    text = data[start:end].decode('utf-8', 'replace')
    if len(text) > 160:
        text = text[:160] + '...'
    return ''.join('<%s>' % name(ord(c)) if ord(c) in STRAY or ord(c) == CR else c for c in text)


def text_problems(path, data):
    """What a text file holds that it may not, and which of the bytes EXPECTED lists
    for it the file held."""
    allowed = EXPECTED.get(path, (frozenset(), ''))[0]
    problems = []
    held = set()
    for match in PATTERN.finditer(data):
        byte = data[match.start()]
        if byte in allowed:
            held.add(byte)
            continue
        line = data.count(LF_BYTE, 0, match.start()) + 1
        if byte == CR:
            what = 'a carriage return not followed by a line feed'
        else:
            what = '%s (0x%02X)' % (name(byte), byte)
        problems.append('%s:%d: %s in "%s"' % (path, line, what, show(data, match.start())))
    return problems, held


def tracked():
    """Each tracked path, with / between its parts, and whether .gitattributes says
    it is not text."""
    out = subprocess.run(['git', '-C', ROOT, 'ls-files', '-z', '--eol'],
                         check=True, stdout=subprocess.PIPE).stdout
    files = []
    for record in out.split(NUL_BYTE):
        if not record:
            continue
        info, _, path = record.partition(TAB_BYTE)
        attributes = info.split(b'attr/', 1)[1].split() if b'attr/' in info else []
        files.append((path.decode('utf-8', 'surrogateescape'), b'-text' in attributes))
    return files


def made(*parts):
    """A made-up file: text, and bytes given by number. made('a', 7) is a and BEL."""
    out = bytearray()
    for part in parts:
        out += part.encode('utf-8') if isinstance(part, str) else bytes([part])
    return bytes(out)


def negative_controls():
    """Each made-up file must be refused or must pass. Answers what did not."""
    listed = 'hmailserver/docs/Licenses/License - MariaDB Connector C.txt'
    padding = 'x' * BINARY_PROBE
    refused = [
        ('a BEL where "\\7" was written', 'docs/Bench.md',
         made('  `installation', 92, 'Extras', 7, 'za.exe` and', 13, 10)),
        ('an ESC where "\\x1b" was written', 'build/check-x.py',
         made('re.sub(r"', 27, 92, '[[0-9;]*m", "", text)', 13, 10)),
        ('a SOH in a C# literal', 'Tests/XOAuth2.cs',
         made('"user=" + email + "', 1, 'auth=Bearer " + token + "', 1, 1, '"', 13, 10)),
        ('a DEL', 'Tests/Labels.cs', made('[InlineData("CI', DEL, 'deploy")]', 13, 10)),
        ('CR CR LF', 'Tests/Using.cs', made('using System;', 13, 13, 10, 'using NUnit;', 13, 10)),
        ('a carriage return that ends the file', 'notes.txt', made('last line', 13)),
        ('a NUL past the first 8,000 bytes', 'notes.txt', made(padding, 0, 13, 10)),
        ('a BEL in the file listed for form feeds', listed, made('Version 2.1', 12, 13, 10, 'and', 7, 13, 10)),
    ]
    passed = [
        ('tab, CR LF and LF line ends', 'notes.txt', made('a', 9, 'b', 13, 10, 'c', 10), False),
        ('UTF-8 text', 'docs/Page.md', made('caf', 0xC3, 0xA9, ' ', 0xE2, 0x80, 0x94, ' na', 0xC3, 0xAF, 'f', 10), False),
        ('a NUL in the first 8,000 bytes (binary)', 'data.bin', made('PK', 3, 4, 0, 0, 7, 13), False),
        ('a file .gitattributes calls not text', 'fuzz/case', made('a', 13, 'b', 27, 1), True),
        ('a form feed in the file listed for it', listed, made('Version 2.1', 13, 10, 12, 13, 10), False),
    ]
    missed = []
    for description, path, data in refused:
        if not is_text(data, False) or not text_problems(path, data)[0]:
            missed.append('%s: was not refused' % description)
    for description, path, data, not_text in passed:
        if is_text(data, not_text) and text_problems(path, data)[0]:
            missed.append('%s: was refused' % description)
    return missed, len(refused), len(passed)


def main():
    try:
        sys.stdout.reconfigure(errors='backslashreplace')
    except AttributeError:
        pass
    missed, refused, passed = negative_controls()
    try:
        files = tracked()
    except (OSError, subprocess.CalledProcessError) as error:
        print('FAIL: git ls-files did not list the tree (%s); run this in a git checkout' % error)
        return 1
    problems = []
    held = {}
    seen = set()
    texts = 0
    others = 0
    for path, not_text in files:
        seen.add(path)
        full = os.path.join(ROOT, *path.split('/'))
        if os.path.islink(full) or not os.path.isfile(full):
            continue
        with open(full, 'rb') as stream:
            head = stream.read(BINARY_PROBE)
            if not is_text(head, not_text):
                others += 1
                continue
            data = head + stream.read()
        texts += 1
        found, got = text_problems(path, data)
        problems.extend(found)
        if got:
            held[path] = got
    for path, (allowed, _) in sorted(EXPECTED.items()):
        if path not in seen:
            problems.append('%s: listed in EXPECTED but not tracked; take it off the list' % path)
        elif held.get(path, set()) != set(allowed):
            problems.append('%s: listed in EXPECTED for %s, but holds %s; correct the list' % (
                path, ', '.join(sorted(name(b) for b in allowed)),
                ', '.join(sorted(name(b) for b in held.get(path, set()))) or 'none of them'))
    if SELF not in seen:
        problems.append('%s is not among the files git lists: the check is not reading the tree' % SELF)

    if problems:
        print('\n'.join(problems))
    if missed:
        print('\n'.join('negative control: ' + line for line in missed))
    if problems or missed:
        print('FAIL: %d problem(s), %d negative control(s) not as expected. Write the escape the character '
              'was meant as; where a file needs the byte itself, add it to EXPECTED in %s with the reason.'
              % (len(problems), len(missed), SELF))
        return 1

    print('OK    %d text files hold no stray control character (%d more are binary or not text by '
          '.gitattributes; %d listed files hold the bytes they are listed for); %d negative controls refused, '
          '%d passed as they should' % (texts, others, len(EXPECTED), refused, passed))
    return 0


if __name__ == '__main__':
    sys.exit(main())
