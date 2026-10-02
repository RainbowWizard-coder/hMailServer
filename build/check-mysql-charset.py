#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every MySQL table is utf8mb4, and stays so.

MySQL's `utf8` is utf8mb3: three bytes a character, so nothing outside the Basic
Multilingual Plane - an emoji, many CJK Extension B characters - fits in it. The
connection has spoken utf8mb4 for years (MySQLConnection::SetConnectionCharacterSet_),
so such a character reached a utf8mb3 column and was refused with error 1366 under
strict mode, the default since MySQL 5.7.5 and MariaDB 10.2.4, or silently made a
question mark without it. Until schema 6102, 106 of the create script's 123 tables
said `DEFAULT CHARSET=utf8`, fifteen said nothing and took the database's default,
and two were utf8mb4 under whatever utf8mb4 collation the server preferred. The
6101 to 6102 step converts every table, and this keeps the scripts from drifting
back. The rules:

  options    Every CREATE TABLE in CreateTablesMySQL.sql, and in every MySQL upgrade
             script from step 6102 on, ends with exactly these table options:
                 ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci
             A table that names no character set takes the database's default,
             which on a database made before 6102, or made by an administrator,
             is utf8mb3 or latin1. utf8mb4 without the collation takes the server's
             own utf8mb4 collation - utf8mb4_0900_ai_ci on MySQL 8, uca1400 on
             MariaDB 11 - which compares differently from every other table, and
             two columns under different collations cannot be compared at all
             (error 1267). InnoDB and DYNAMIC because the index lengths below
             assume them: MyISAM keys stop at 1000 bytes, COMPACT and REDUNDANT
             key parts at 767, and a varchar(255) at four bytes a character is
             1020.

  columns    Every CHARACTER SET, CHARSET and COLLATE those scripts name is
             utf8mb4, utf8mb4_general_ci or utf8mb4_bin - a column that says
             `character set utf8 collate utf8_bin` is the same defect one level
             down.

  step       Upgrade6101to6102MySQL.sql converts every table the create script
             makes, with ENGINE=InnoDB and ROW_FORMAT=DYNAMIC, to utf8mb4 under
             utf8mb4_general_ci, and alters the database's own default. CONVERT TO
             sets every column of a table to the collation it names, whatever the
             column said before, so a table with a utf8mb4_bin column is converted
             column by column with MODIFY instead, every text column named: a
             binary unique index briefly made case-insensitive refuses its own rows.

  indexes    Every index in the create script fits InnoDB's DYNAMIC limits at four
             bytes a character: no key part over 3072 bytes, no key over 3072 bytes
             in all, and no TEXT column without a prefix length.

  probe      Every MySQL upgrade step from 6.3.3's schema (6040) to 6102 that makes a
             utf8mb4 table opens with the probe - DROP TABLE IF EXISTS hm_upgradeprobe,
             a CREATE TABLE hm_upgradeprobe whose primary key is a utf8mb4
             varchar(255), InnoDB and DYNAMIC, and DROP TABLE hm_upgradeprobe - and
             every utf8mb4 table it makes says ENGINE=InnoDB and ROW_FORMAT=DYNAMIC.
             MySQL commits each CREATE and ALTER TABLE as it runs, so a step whose
             1020-byte key was refused (error 1071, on a server older than MySQL
             5.7.9 or MariaDB 10.2.2, or one whose default engine or row format is
             MyISAM or COMPACT) stopped with its first tables made and the version
             row unchanged, and every later run failed on a table already there.
             The probe is refused before anything is changed.

  server     No MySQL CREATE TABLE compiled into the server (the SQL log device
             creates hm_log on demand), and no CREATE DATABASE (the setup's and
             --create-database's, SQLStatement::GetCreateDatabase), names a
             character set other than utf8mb4 under utf8mb4_general_ci.

Run from anywhere: python3 build/check-mysql-charset.py
Pass --self-test to run every rule against a mutated copy of the tree and prove each
can fail.
"""
import io
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SCRIPTS = os.path.join(ROOT, "hmailserver", "source", "DBScripts")
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")

CREATE = "CreateTablesMySQL.sql"
CONVERSION = "Upgrade6101to6102MySQL.sql"

# Upgrade steps whose target is this or later are held to the options rule. The
# steps before it shipped under utf8 and are converted, whole, by the 6102 step.
FIRST_CHECKED_STEP = 6102

# The steps an upgrade from 6.3.3 runs before the conversion: from this schema to
# FIRST_CHECKED_STEP. Each that makes a utf8mb4 table opens with the probe.
PROBE_FROM = 6040
PROBE_TABLE = "hm_upgradeprobe"

CHARSET = "utf8mb4"
COLLATION = "utf8mb4_general_ci"
BINARY = "utf8mb4_bin"
TABLE_OPTIONS = "ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci"

# InnoDB with ROW_FORMAT=DYNAMIC and a 16 KB page.
KEY_PART_LIMIT = 3072
KEY_LIMIT = 3072
BYTES_PER_CHARACTER = 4

# Tables the server creates at run time rather than a script: the 6102 step may
# convert them (conditionally - they need not exist) although the create script
# does not make them.
RUNTIME_TABLES = {"hm_log"}

# Generated files that embed the documents, which may quote what not to write.
SERVER_EXCLUDED = {"HelpPagesData.cpp"}

FIXED_WIDTH = {
    "tinyint": 1, "smallint": 2, "mediumint": 3, "int": 4, "integer": 4, "bigint": 8,
    "float": 4, "double": 8, "datetime": 5, "date": 3, "timestamp": 4, "time": 3,
}
STRING_TYPES = {"varchar", "char"}
TEXT_TYPES = {"tinytext", "text", "mediumtext", "longtext", "tinyblob", "blob", "mediumblob", "longblob"}

STEP = re.compile(r"^Upgrade(\d+)to(\d+)MySQL\.sql$", re.I)
CREATE_TABLE = re.compile(r"^\s*create\s+table\s+(?:if\s+not\s+exists\s+)?`?(\w+)`?", re.I)
CREATE_INDEX = re.compile(r"^\s*create\s+(unique\s+)?index\s+`?(\w+)`?\s+on\s+`?(\w+)`?\s*\((.*)\)\s*;?\s*$", re.I | re.S)
ALTER_TABLE = re.compile(r"alter\s+table\s+`?(\w+)`?\s+(.*?)(?:;|'|$)", re.I | re.S)
NAMED_CHARSET = re.compile(r"(?:character\s+set|charset)\s*=?\s*'?(\w+)", re.I)
NAMED_COLLATION = re.compile(r"collate\s*=?\s*'?(\w+)", re.I)


def read(path):
    with io.open(path, encoding="utf-8-sig", errors="replace") as handle:
        return handle.read().replace("\r\n", "\n")


def strip_comments(text):
    """The text with -- and /* */ comments blanked out, quotes respected, and every
    newline kept so that line numbers still count."""
    out = []
    i = 0
    quote = None
    while i < len(text):
        c = text[i]
        if quote:
            out.append(c)
            if c == quote:
                quote = None
            i += 1
        elif c in ("'", '"', "`"):
            quote = c
            out.append(c)
            i += 1
        elif text.startswith("--", i):
            end = text.find("\n", i)
            end = len(text) if end < 0 else end
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = len(text) if end < 0 else end + 2
            out.append("".join("\n" for ch in text[i:end] if ch == "\n"))
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def commands(text):
    """(first line number, command) for each command, split as SQLScriptParser splits
    a MySQL script: on a blank line. Comments are blanked first."""
    clean = strip_comments(text)
    line = 1
    for command in clean.split("\n\n"):
        leading = len(command) - len(command.lstrip("\n"))
        if command.strip():
            yield line + leading, command
        line += command.count("\n") + 2


def split_top(text, separator=","):
    """Split on the separator where it is outside parentheses and quotes."""
    parts, depth, quote, current = [], 0, None, []
    for c in text:
        if quote:
            current.append(c)
            if c == quote:
                quote = None
            continue
        if c in ("'", '"', "`"):
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == separator and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(c)
    parts.append("".join(current))
    return [part.strip() for part in parts if part.strip()]


def matching_paren(text, start):
    depth, quote = 0, None
    for i in range(start, len(text)):
        c = text[i]
        if quote:
            if c == quote:
                quote = None
            continue
        if c in ("'", '"', "`"):
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def key_parts(text):
    """[(column, prefix length or None)] from "`a`, b(255)"."""
    parts = []
    for part in split_top(text):
        match = re.match(r"^`?(\w+)`?\s*(?:\(\s*(\d+)\s*\))?", part)
        if match:
            parts.append((match.group(1).lower(), int(match.group(2)) if match.group(2) else None))
    return parts


def parse_create(command):
    """A CREATE TABLE command as {name, columns, keys, options}, or None."""
    match = CREATE_TABLE.match(command)
    if not match:
        return None
    open_at = command.find("(", match.end())
    close_at = matching_paren(command, open_at) if open_at >= 0 else -1
    if close_at < 0:
        return None
    table = {"name": match.group(1).lower(), "columns": {}, "order": [], "keys": [],
             "options": command[close_at + 1:].strip().rstrip(";").strip()}
    for item in split_top(command[open_at + 1:close_at]):
        lowered = item.lower()
        key = re.match(r"^(primary\s+key|unique\s+key|unique\s+index|unique|key|index)\b\s*`?(\w*)`?\s*\((.*)\)\s*$",
                       item, re.I | re.S)
        if key:
            kind = "primary" if lowered.startswith("primary") else ("unique" if lowered.startswith("unique") else "index")
            table["keys"].append({"kind": kind, "name": key.group(2) or "", "parts": key_parts(key.group(3))})
            continue
        if re.match(r"^(constraint|foreign\s+key|check)\b", item, re.I):
            continue
        column = re.match(r"^`?(\w+)`?\s+(\w+)\s*(?:\(\s*(\d+)(?:\s*,\s*\d+)?\s*\))?(.*)$", item, re.I | re.S)
        if not column:
            continue
        rest = column.group(4)
        charset = NAMED_CHARSET.search(rest)
        collate = NAMED_COLLATION.search(rest)
        name = column.group(1).lower()
        table["columns"][name] = {
            "type": column.group(2).lower(),
            "length": int(column.group(3)) if column.group(3) else None,
            "charset": charset.group(1).lower() if charset else None,
            "collate": collate.group(1).lower() if collate else None,
            "definition": item,
        }
        table["order"].append(name)
    return table


def parse_create_index(command):
    match = CREATE_INDEX.match(command.strip())
    if not match:
        return None
    return {"table": match.group(3).lower(), "kind": "unique" if match.group(1) else "index",
            "name": match.group(2), "parts": key_parts(match.group(4))}


def schema(text):
    """The tables of a create script, each with its keys, in order."""
    tables = {}
    order = []
    for line, command in commands(text):
        table = parse_create(command)
        if table:
            table["line"] = line
            tables[table["name"]] = table
            order.append(table["name"])
            continue
        index = parse_create_index(command)
        if index and index["table"] in tables:
            index["line"] = line
            tables[index["table"]]["keys"].append(index)
    return tables, order


def part_bytes(table, column, prefix):
    """(bytes, problem) for one key part at four bytes a character."""
    definition = table["columns"].get(column)
    if definition is None:
        return 0, "names %s, which %s does not have" % (column, table["name"])
    kind = definition["type"]
    if kind in STRING_TYPES:
        characters = prefix if prefix else (definition["length"] or 1)
        return characters * BYTES_PER_CHARACTER, None
    if kind in TEXT_TYPES:
        if not prefix:
            return 0, "indexes %s, a %s, with no prefix length" % (column, kind)
        return prefix * BYTES_PER_CHARACTER, None
    return FIXED_WIDTH.get(kind, 8), None


def index_lengths(tables):
    """[(table, key, [(column, bytes)], total, problems)] for every key over a string."""
    rows = []
    for table in tables.values():
        for key in table["keys"]:
            parts, problems, total = [], [], 0
            textual = False
            for column, prefix in key["parts"]:
                size, problem = part_bytes(table, column, prefix)
                if problem:
                    problems.append(problem)
                definition = table["columns"].get(column)
                if definition and (definition["type"] in STRING_TYPES or definition["type"] in TEXT_TYPES):
                    textual = True
                    if size > KEY_PART_LIMIT:
                        problems.append("its part %s is %d bytes at four a character, over InnoDB's %d"
                                        % (column, size, KEY_PART_LIMIT))
                parts.append((column, size, prefix))
                total += size
            if total > KEY_LIMIT:
                problems.append("it is %d bytes in all at four a character, over InnoDB's %d" % (total, KEY_LIMIT))
            if textual or problems:
                rows.append((table["name"], key, parts, total, problems))
    return rows


def string_columns(table):
    return [name for name in table["order"]
            if table["columns"][name]["type"] in STRING_TYPES or table["columns"][name]["type"] in TEXT_TYPES]


def check_options(name, text, problems):
    """The options and columns rules over one script."""
    for line, command in commands(text):
        table = parse_create(command)
        where = "%s:%d" % (name, line)
        if table:
            options = table["options"]
            charset = NAMED_CHARSET.search(options)
            collate = NAMED_COLLATION.search(options)
            if not charset:
                problems.append("%s: create table %s names no character set, so it takes the database's default - "
                                "utf8mb3 or latin1 on a database made before 6102 or by an administrator; end it with "
                                "\") %s;\"" % (where, table["name"], TABLE_OPTIONS))
            elif charset.group(1).lower() != CHARSET:
                problems.append("%s: create table %s is %s, not utf8mb4, and refuses an emoji; end it with \") %s;\""
                                % (where, table["name"], charset.group(1), TABLE_OPTIONS))
            elif not collate or collate.group(1).lower() != COLLATION:
                problems.append("%s: create table %s is utf8mb4 without COLLATE=%s, so it compares under the "
                                "server's own utf8mb4 collation and unlike every other table; end it with \") %s;\""
                                % (where, table["name"], COLLATION, TABLE_OPTIONS))
            if not re.search(r"\bengine\s*=\s*innodb\b", options, re.I):
                problems.append("%s: create table %s does not say ENGINE=InnoDB; a MyISAM key stops at 1000 bytes, "
                                "and a varchar(255) at four bytes a character is 1020" % (where, table["name"]))
            if not re.search(r"\brow_format\s*=\s*dynamic\b", options, re.I):
                problems.append("%s: create table %s does not say ROW_FORMAT=DYNAMIC; under COMPACT a key part "
                                "stops at 767 bytes, and a varchar(255) at four bytes a character is 1020"
                                % (where, table["name"]))
        for match in NAMED_CHARSET.finditer(command):
            if match.group(1).lower() != CHARSET:
                problems.append("%s: character set %s; this schema is utf8mb4 throughout"
                                % (name + ":" + str(line + command[:match.start()].count("\n")), match.group(1)))
        for match in NAMED_COLLATION.finditer(command):
            if match.group(1).lower() not in (COLLATION, BINARY):
                problems.append("%s: collation %s; this schema compares under %s, or %s where a column must be exact"
                                % (name + ":" + str(line + command[:match.start()].count("\n")), match.group(1),
                                   COLLATION, BINARY))


def check_probe(name, text, problems):
    """The probe rule over one step before the conversion."""
    listed = list(commands(text))
    makes = False
    for line, command in listed:
        table = parse_create(command)
        if not table or table["name"] == PROBE_TABLE or not re.search(r"\butf8mb4\b", command, re.I):
            continue
        makes = True
        if not re.search(r"\bengine\s*=\s*innodb\b", table["options"], re.I) or \
           not re.search(r"\brow_format\s*=\s*dynamic\b", table["options"], re.I):
            problems.append("%s:%d: create table %s is utf8mb4 without ENGINE=InnoDB and ROW_FORMAT=DYNAMIC, so its "
                            "1020-byte keys depend on the server's default engine and row format"
                            % (name, line, table["name"]))
    if not makes:
        return
    texts = [command.strip() for _, command in listed]
    probe = parse_create(texts[1]) if len(texts) > 1 else None
    opened = bool(len(texts) > 2 and
                  re.match(r"^drop\s+table\s+if\s+exists\s+`?" + PROBE_TABLE + r"`?\s*;?$", texts[0], re.I) and
                  probe is not None and probe["name"] == PROBE_TABLE and
                  re.match(r"^drop\s+table\s+`?" + PROBE_TABLE + r"`?\s*;?$", texts[2], re.I))
    if opened:
        options = probe["options"]
        wide = [key for key in probe["keys"] if key["kind"] == "primary" and
                any(probe["columns"].get(column, {}).get("type") == "varchar" and
                    (probe["columns"][column]["length"] or 0) >= 255 for column, _ in key["parts"])]
        opened = bool(wide and re.search(r"\bengine\s*=\s*innodb\b", options, re.I) and
                      re.search(r"\brow_format\s*=\s*dynamic\b", options, re.I) and
                      re.search(r"\bcharset\s*=\s*utf8mb4\b", options, re.I))
    if not opened:
        problems.append("%s: makes a utf8mb4 table without opening with the probe (DROP TABLE IF EXISTS %s; a "
                        "CREATE TABLE %s whose primary key is a utf8mb4 varchar(255), InnoDB and DYNAMIC; DROP "
                        "TABLE %s), so on a server that cannot hold a 1020-byte key it stops half applied"
                        % (name, PROBE_TABLE, PROBE_TABLE, PROBE_TABLE))


def check_step(text, tables, problems, born_utf8mb4=frozenset()):
    """The step rule: the 6102 script converts every table of the create script
    but those a later step first makes, which are born utf8mb4 and held to the
    options rule by their own step (born_utf8mb4)."""
    converted = {}
    database = False
    for line, command in commands(text):
        if re.match(r"^\s*alter\s+(database|schema)\b", command, re.I):
            charset = NAMED_CHARSET.search(command)
            collate = NAMED_COLLATION.search(command)
            if charset and charset.group(1).lower() == CHARSET and collate and collate.group(1).lower() == COLLATION:
                database = True
            continue
        for match in ALTER_TABLE.finditer(command):
            body = match.group(2)
            if re.search(r"\butf8mb4\b", body, re.I):
                converted.setdefault(match.group(1).lower(), []).append((line, body))
    if not database:
        problems.append("%s: does not alter the database's default to %s %s, so a table a later script creates "
                        "without naming one would not be utf8mb4" % (CONVERSION, CHARSET, COLLATION))
    for name, table in tables.items():
        if name not in converted and name in born_utf8mb4:
            continue
        if name not in converted:
            problems.append("%s: %s is not converted; every table the create script makes is"
                            % (CONVERSION, name))
            continue
        line, body = converted[name][0]
        where = "%s:%d" % (CONVERSION, line)
        if not re.search(r"\bengine\s*=\s*innodb\b", body, re.I) or not re.search(r"\brow_format\s*=\s*dynamic\b", body, re.I):
            problems.append("%s: %s is converted without ENGINE=InnoDB and ROW_FORMAT=DYNAMIC; a table an old "
                            "installer made MyISAM or COMPACT refuses a 1020-byte key" % (where, name))
        binary = [column for column in string_columns(table) if table["columns"][column]["collate"] == BINARY]
        if binary:
            if re.search(r"\bconvert\s+to\b", body, re.I):
                problems.append("%s: %s has %s column(s) (%s) and is converted with CONVERT TO, which sets every "
                                "column to one collation; convert it column by column with MODIFY"
                                % (where, name, BINARY, ", ".join(binary)))
                continue
            if not re.search(r"\bdefault\s+(character\s+set|charset)\s*=?\s*utf8mb4\s+collate\s*=?\s*" + COLLATION,
                             body, re.I):
                problems.append("%s: %s's default is not set to %s %s" % (where, name, CHARSET, COLLATION))
            for column in string_columns(table):
                want = table["columns"][column]["collate"] or COLLATION
                modify = re.search(r"\bmodify\s+(?:column\s+)?`?" + column + r"`?\s+(.*?)(?:,\s*(?:modify|change|add|drop)\b|$)",
                                   body, re.I | re.S)
                if not modify:
                    problems.append("%s: %s.%s is not converted; a table converted column by column names "
                                    "every text column" % (where, name, column))
                    continue
                collate = NAMED_COLLATION.search(modify.group(1))
                if not collate or collate.group(1).lower() != want:
                    problems.append("%s: %s.%s is converted to %s; the create script has %s"
                                    % (where, name, column, collate.group(1) if collate else "no collation", want))
        else:
            if not re.search(r"\bconvert\s+to\s+(character\s+set|charset)\s+utf8mb4\s+collate\s+" + COLLATION + r"\b",
                             body, re.I):
                problems.append("%s: %s is not converted with CONVERT TO CHARACTER SET %s COLLATE %s"
                                % (where, name, CHARSET, COLLATION))
    for name in converted:
        if name not in tables and name not in RUNTIME_TABLES:
            problems.append("%s: converts %s, which the create script does not make" % (CONVERSION, name))


def check_indexes(tables, problems):
    for table_name, key, parts, total, key_problems in index_lengths(tables):
        for problem in key_problems:
            label = key["name"] or ("primary key" if key["kind"] == "primary" else key["kind"] + " (" +
                                    ", ".join(column for column, _, _ in parts) + ")")
            problems.append("%s:%d: %s on %s: %s" % (CREATE, key.get("line", tables[table_name]["line"]),
                                                      label, table_name, problem))


def check_server(sources, problems):
    for path, text in sources.items():
        for number, line in enumerate(text.split("\n"), 1):
            for match in re.finditer(r"\bdefault\s+charset\s*=\s*(\w+)", line, re.I):
                if match.group(1).lower() != CHARSET or not re.search(r"\bcollate\s*=\s*" + COLLATION, line, re.I):
                    problems.append("%s:%d: a MySQL table the server creates is %s; say DEFAULT CHARSET=%s COLLATE=%s"
                                    % (path, number, match.group(1), CHARSET, COLLATION))
            for match in re.finditer(r"create\s+database\b[^\"]*?\bcharacter\s+set\s+'?(\w+)", line, re.I):
                if match.group(1).lower() != CHARSET or not re.search(r"\bcollate\s+" + COLLATION, line, re.I):
                    problems.append("%s:%d: the MySQL database the server creates is %s; say character set %s collate %s"
                                    % (path, number, match.group(1), CHARSET, COLLATION))


def check(scripts, sources):
    """All the rules. scripts maps a DBScripts file name to its text; sources maps a
    server source path to its text. Returns (problems, create-script tables)."""
    problems = []
    tables, _ = schema(scripts.get(CREATE, ""))
    if len(tables) < 100:
        problems.append("%s: only %d tables were read; the check has stopped seeing the create script"
                        % (CREATE, len(tables)))
    check_options(CREATE, scripts.get(CREATE, ""), problems)
    for name in sorted(scripts):
        step = STEP.match(name)
        if step and int(step.group(2)) >= FIRST_CHECKED_STEP:
            check_options(name, scripts[name], problems)
        elif step and int(step.group(1)) >= PROBE_FROM:
            check_probe(name, scripts[name], problems)
    # The tables a step after the conversion first makes: a database that ran
    # the conversion did not have them yet, and the step that makes them is held
    # to the options rule above, so they are born utf8mb4 (hm_neverbannames, 6108).
    born = set()
    for name in scripts:
        step = STEP.match(name)
        if step and int(step.group(1)) >= FIRST_CHECKED_STEP:
            later, _ = schema(scripts[name])
            born.update(later)
    if CONVERSION in scripts:
        check_step(scripts[CONVERSION], tables, problems, frozenset(born))
    else:
        problems.append("%s is missing: nothing converts the tables of a database made before schema 6102"
                        % CONVERSION)
    check_indexes(tables, problems)
    check_server(sources, problems)
    return problems, tables


def load():
    scripts = {}
    for name in os.listdir(SCRIPTS):
        if name == CREATE or STEP.match(name):
            scripts[name] = read(os.path.join(SCRIPTS, name))
    sources = {}
    for folder, _, names in os.walk(SERVER):
        for name in names:
            if name.endswith((".cpp", ".h")) and name not in SERVER_EXCLUDED:
                path = os.path.join(folder, name)
                text = read(path)
                if re.search(r"charset|character\s+set", text, re.I):
                    sources[os.path.relpath(path, ROOT).replace(os.sep, "/")] = text
    return scripts, sources


def self_test(scripts, sources):
    """Each rule, broken on purpose in a copy of the tree, must be reported."""
    ok = True
    problems, _ = check(scripts, sources)
    if problems:
        print("SELF-TEST FAILED: the tree itself does not pass:")
        for problem in problems[:10]:
            print("  " + problem)
        return False

    def expect(label, mutated_scripts, mutated_sources, needle):
        nonlocal ok
        found, _ = check(mutated_scripts, mutated_sources)
        if any(needle in problem for problem in found):
            print("self-test: %s is reported" % label)
        else:
            print("SELF-TEST FAILED: %s was not reported (got %r)" % (label, found[:3]))
            ok = False

    create = scripts[CREATE]
    first = ") " + TABLE_OPTIONS + ";"

    copy = dict(scripts)
    copy[CREATE] = create.replace(first, ");", 1)
    expect("a create-script table with no character set", copy, sources, "names no character set")

    copy = dict(scripts)
    copy[CREATE] = create.replace(first, ") ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8;", 1)
    expect("a create-script table in utf8", copy, sources, "is utf8, not utf8mb4")

    copy = dict(scripts)
    copy[CREATE] = create.replace(first, ") ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4;", 1)
    expect("utf8mb4 without the collation", copy, sources, "utf8mb4 without COLLATE")

    copy = dict(scripts)
    copy[CREATE] = create.replace(first, ") DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;", 1)
    expect("a table without ENGINE and ROW_FORMAT", copy, sources, "does not say ROW_FORMAT=DYNAMIC")

    copy = dict(scripts)
    copy[CREATE] = create.replace("character set utf8mb4 collate utf8mb4_bin", "character set utf8 collate utf8_bin", 1)
    expect("a column in utf8 under utf8_bin", copy, sources, "character set utf8;")

    later = "Upgrade6102to6103MySQL.sql"
    copy = dict(scripts)
    copy[later] = "create table hm_later\n(\n\tlaterid int not null\n);\n\nupdate hm_dbversion set value = 6103;\n"
    expect("a later upgrade that creates a table with no character set", copy, sources, later + ":1: create table hm_later names no character set")

    copy = dict(scripts)
    copy[later] = "alter table hm_accounts add column accountlater varchar(10) character set latin1 not null default '';\n"
    expect("a later upgrade that adds a latin1 column", copy, sources, "character set latin1")

    step = scripts[CONVERSION]
    copy = dict(scripts)
    copy[CONVERSION] = re.sub(r"(?m)^ALTER TABLE hm_aliases .*\n", "", step, count=1)
    expect("a table the step leaves out", copy, sources, "hm_aliases is not converted")

    copy = dict(scripts)
    copy[CONVERSION] = re.sub(r"(?m)^ALTER DATABASE .*\n", "", step, count=1)
    expect("a step that leaves the database's default", copy, sources, "does not alter the database's default")

    copy = dict(scripts)
    copy[CONVERSION] = re.sub(r"(?m)^ALTER TABLE hm_knownsenders .*$",
                              "ALTER TABLE hm_knownsenders ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET "
                              "utf8mb4 COLLATE utf8mb4_general_ci;", step, count=1)
    expect("a binary column converted with CONVERT TO", copy, sources, "converted with CONVERT TO, which sets every column")

    copy = dict(scripts)
    copy[CONVERSION] = step.replace(", MODIFY kslastseen varchar(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL", "", 1)
    expect("a text column a column-by-column conversion leaves out", copy, sources, "hm_knownsenders.kslastseen is not converted")

    copy = dict(scripts)
    copy[CONVERSION] = step.replace("MODIFY arsender varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin",
                                    "MODIFY arsender varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci", 1)
    expect("a binary column converted to the case-insensitive collation", copy, sources,
           "hm_autoreplies.arsender is converted to utf8mb4_general_ci; the create script has utf8mb4_bin")

    copy = dict(scripts)
    copy[CONVERSION] = step.replace("ALTER TABLE hm_domains ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;",
                                    "ALTER TABLE hm_domains ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;", 1)
    expect("a conversion under another collation", copy, sources, "hm_domains is not converted with CONVERT TO")

    copy = dict(scripts)
    copy[CONVERSION] = step + "\nALTER TABLE hm_nosuchtable ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;\n"
    expect("a conversion of a table the create script does not make", copy, sources, "converts hm_nosuchtable, which the create script does not make")

    copy = dict(scripts)
    del copy[CONVERSION]
    expect("a tree without the conversion step", copy, sources, CONVERSION + " is missing")

    copy = dict(scripts)
    copy[CONVERSION] = step.replace("ALTER TABLE hm_messages ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, ", "ALTER TABLE hm_messages ", 1)
    expect("a conversion without ENGINE and ROW_FORMAT", copy, sources, "hm_messages is converted without ENGINE=InnoDB")

    copy = dict(scripts)
    copy[CREATE] = create + ("\ncreate table hm_toolong\n(\n\ttlid int not null,\n\ttlpath varchar(1024) not null\n) "
                             + TABLE_OPTIONS + ";\n\nCREATE UNIQUE INDEX idx_hm_toolong_path ON hm_toolong (tlpath);\n")
    copy[CONVERSION] = step + "\nALTER TABLE hm_toolong ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;\n"
    expect("an index over a varchar(1024)", copy, sources, "idx_hm_toolong_path on hm_toolong")

    copy = dict(scripts)
    copy[CREATE] = create + ("\ncreate table hm_textkey\n(\n\ttkid int not null,\n\ttkbody text not null\n) "
                             + TABLE_OPTIONS + ";\n\nCREATE INDEX idx_hm_textkey_body ON hm_textkey (tkbody);\n")
    copy[CONVERSION] = step + "\nALTER TABLE hm_textkey ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;\n"
    expect("an index over a text column with no prefix", copy, sources, "a text, with no prefix length")

    copy = dict(sources)
    copy["Server/Common/Application/Example.cpp"] = '_T(") DEFAULT CHARSET=utf8 /* [IGNORE-ERRORS] */"));\n'
    expect("a table the server creates in utf8", scripts, copy, "a MySQL table the server creates is utf8")

    copy = dict(sources)
    copy["Server/Common/SQL/Example.cpp"] = '         sSQL.Format(_T("create database %s character set \'utf8\'"), sDatabaseName.c_str());\n'
    expect("a database the server creates in utf8", scripts, copy, "the MySQL database the server creates is utf8")

    calendars = "Upgrade6040to6041MySQL.sql"
    copy = dict(scripts)
    copy[calendars] = scripts[calendars].split("DROP TABLE hm_upgradeprobe;\n\n", 1)[1]
    expect("a step before the conversion that makes a utf8mb4 table without the probe", copy, sources,
           calendars + ": makes a utf8mb4 table without opening with the probe")

    copy = dict(scripts)
    copy[calendars] = scripts[calendars].replace("probevalue varchar(255) not null", "probevalue int not null", 1)
    expect("a probe whose key is not a wide string", copy, sources,
           calendars + ": makes a utf8mb4 table without opening with the probe")

    copy = dict(scripts)
    copy[calendars] = scripts[calendars].replace("primary key(probevalue)", "unique(probevalue)", 1)
    expect("a probe with no primary key, which sql_require_primary_key refuses", copy, sources,
           calendars + ": makes a utf8mb4 table without opening with the probe")

    copy = dict(scripts)
    copy[calendars] = scripts[calendars].replace(") ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4;",
                                                 ") DEFAULT CHARSET=utf8mb4;", 1)
    expect("a utf8mb4 table before the conversion without ENGINE and ROW_FORMAT", copy, sources,
           "create table hm_calendars is utf8mb4 without ENGINE=InnoDB and ROW_FORMAT=DYNAMIC")

    copy = dict(scripts)
    copy[CREATE] = "\n".join(create.split("\n")[:40])
    expect("a check that has stopped seeing the create script", copy, sources, "has stopped seeing the create script")

    return ok


def main():
    scripts, sources = load()
    if "--self-test" in sys.argv:
        return 0 if self_test(scripts, sources) else 1

    problems, tables = check(scripts, sources)
    if problems:
        print("Every MySQL table is utf8mb4 under utf8mb4_general_ci, InnoDB and DYNAMIC:")
        for problem in problems:
            print("  " + problem)
        print("FAIL: %d problem(s)" % len(problems))
        return 1

    keys = sum(1 for _ in index_lengths(tables))
    longest = max((total for _, _, _, total, _ in index_lengths(tables)), default=0)
    later = sum(1 for name in scripts if STEP.match(name) and int(STEP.match(name).group(2)) >= FIRST_CHECKED_STEP)
    print("OK    %d tables in %s are utf8mb4 under %s, InnoDB and DYNAMIC, and %s converts every one; "
          "%d upgrade script(s) from %d on hold to it; %d keys over text, the longest %d bytes at four a character"
          % (len(tables), CREATE, COLLATION, CONVERSION, later, FIRST_CHECKED_STEP, keys, longest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
