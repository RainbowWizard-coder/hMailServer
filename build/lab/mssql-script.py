#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A SQL Server script of this repository, split as the server splits it, for sqlcmd.

The server runs CreateTablesMSSQL.sql (and every upgrade script) through
SQLScriptRunner, which hands each statement to the database on its own. The
statements are what SQLScriptParser::Parse makes of the file
(hmailserver/source/Server/Common/SQL/SQLScriptParser.cpp), for SQL Server:

  * CRLF is made LF;
  * the text is split at every blank line ("\\n\\n");
  * each piece loses its leading newlines, spaces and tabs (PreprocessLine_),
    and an empty piece is dropped;
  * a piece that starts with @@@ is a macro, expanded by the server
    (MSSQLMacroExpander); this script refuses one, since sqlcmd cannot run it.

The Linux server has no SQL Server backend (HM6390), so the test lab runs the
create script through sqlcmd instead: this prints each statement followed by a GO
line, which makes every statement its own batch - one statement per round trip,
as the server sends them.

Usage:
  python3 build/lab/mssql-script.py <script.sql>           the batches, on stdout
  python3 build/lab/mssql-script.py <script.sql> --count   how many statements
"""

import sys


def statements(text):
    text = text.replace("\r\n", "\n")
    for piece in text.split("\n\n"):
        while piece[:1] in ("\n", " ", "\t") and piece:
            piece = piece[1:]
        if piece:
            yield piece


def main(argv):
    if len(argv) not in (2, 3) or (len(argv) == 3 and argv[2] != "--count"):
        sys.stderr.write(__doc__)
        return 64
    with open(argv[1], encoding="utf-8-sig", errors="strict") as f:
        found = list(statements(f.read()))
    macros = [s for s in found if s.startswith("@@@")]
    if macros:
        sys.stderr.write("%s has %d macro statement(s) (@@@...@@@), which only the server expands; the first: %s\n"
                         % (argv[1], len(macros), macros[0][:80]))
        return 1
    if len(argv) == 3:
        print(len(found))
        return 0
    out = sys.stdout
    for statement in found:
        out.write(statement.rstrip("\n"))
        out.write("\nGO\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
