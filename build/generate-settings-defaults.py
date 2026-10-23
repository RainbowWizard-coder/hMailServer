#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a fresh installation has in hm_settings, as a C++ table.

The server's other settings store - hm_settings, the one the COM `Settings`
object and the Control Panel's classic pages have always used - has its shipped
defaults in two places, and neither of them is a list anybody keeps by hand:

  the create scripts        `insert into hm_settings (settingname,
                            settingstring, settinginteger) values (...)` -
                            131 rows, which is what a database made by
                            hmailserver/source/DBScripts/CreateTablesMSSQL.sql
                            starts with. The three dialects are compared here
                            and must agree, because a default that depends on
                            which database you chose is a bug.

  Configuration.cpp         `EnsureSettingDefaults_`, which inserts a row for a
                            property added after the database was created. Those
                            never reach the create script, so both are read.

This writes them into Common/Application/SettingsDefaults.h, so that the server
can answer which of its settings are not what this version ships with -
/api/v1/settings/changed, `Settings.GetChangedFromDefault` over COM, `hmctl
config changed`, and the Control Deck's Changed from stock page. The [Settings]
keys of hMailServer.ini are the other half of that answer and come from
build/ini-settings-baseline.txt through build/check-ini-coverage.py.

WHAT "DEFAULT" MEANS HERE: the value a database created by THIS version's create
script has, before anybody has administered anything. It is not "what an
installation upgraded from 5.6 has", which is whatever its upgrade scripts
inserted at the time - usually the same value, and where it is not, the older
installation is genuinely different from a new one and the list should say so.
That is the honest reading of "changed from stock", and it is the reading that
makes two installations comparable.

A default that is not a literal is resolved - Logger::LSSMTP, and
PasswordBreachCheck::DefaultUrl, are named constants - and the resolution FAILS
rather than guesses, because a default nobody can work out would be reported as
a difference on every server in the world.

Each row also carries its LEVELS MARK - the deepest level the property may be
set at, server, domain or account (docs/SettingLevels.md) - read from
build/settings-levels.txt, which lists every property here by name. A property
the list does not mark, or a mark that is not one of the three, is refused and
nothing is written; so is a line of the list that names no property here.

The names this build READS are written out beside the defaults, because an
installation upgraded from an old schema holds rows no create script ever made:
twelve of them (useordb, usespamhaus, tarpitcount, filters, imapport ...) were
inserted by upgrade scripts for features that have since gone. Nothing reads
them, so they cannot explain anything a server is doing, and a list of what has
been changed from stock that carried twelve fossils on every upgraded
installation would be a list nobody finished reading. SettingsDiff uses this to
leave them out.

Usage:
  python3 build/generate-settings-defaults.py            # write the header
  python3 build/generate-settings-defaults.py --print    # to stdout
  python3 build/generate-settings-defaults.py --check    # fail if it is stale
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
SCRIPTS = os.path.join(ROOT, "hmailserver", "source", "DBScripts")
CONFIGURATION = os.path.join(SERVER, "Common", "Application", "Configuration.cpp")
CONSTANTS = os.path.join(SERVER, "Common", "Application", "Constants.h")
OUT = os.path.join(SERVER, "Common", "Application", "SettingsDefaults.h")
LEVELS_FILE = os.path.join(ROOT, "build", "settings-levels.txt")
LEVELS = ("server", "domain", "account")
LEVEL_WORD = {"server": "LevelServer", "domain": "LevelDomain", "account": "LevelAccount"}

CREATE_SCRIPTS = ["CreateTablesMSSQL.sql", "CreateTablesMySQL.sql", "CreateTablesPGSQL.sql"]

INSERT = re.compile(r"insert\s+into\s+hm_settings\s*\(\s*settingname\s*,\s*settingstring\s*,\s*settinginteger\s*\)"
                    r"\s*values\s*\(\s*'([^']*)'\s*,\s*'((?:[^']|'')*)'\s*,\s*(-?\d+)\s*\)", re.I)
ENSURE = re.compile(r"Ensure(Long|String)\s*\(\s*(PROPERTY_[A-Z0-9_]+|_T\(\"[^\"]*\"\))\s*,\s*(.*?)\)\s*;", re.S)
DEFINE = re.compile(r'#define\s+(PROPERTY_[A-Z0-9_]+)\s+_T\("([^"]*)"\)')
STRING_LITERAL = re.compile(r'^(?:_T|L)?\(?\s*"((?:[^"\\]|\\.)*)"\s*\)?$', re.S)
NUMBER = re.compile(r"^(-?\d+|0[xX][0-9a-fA-F]+)$")
QUALIFIED = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*::([A-Za-z_][A-Za-z0-9_]*)$")
# How the server reads a property, which is the only thing that says WHICH of the
# two columns it uses: hm_settings has both and the row does not say.
ACCESS = re.compile(r"\b(?:Get|Set)(Long|Bool|String)\s*\(\s*(PROPERTY_[A-Z0-9_]+|_T\(\"[^\"]*\"\))\s*[,)]")
# And the wrappers that read or write one property as TEXT by the name their
# caller hands them: Configuration's GetSignInString and SetSignInString, its
# GetImportOAuthString and SetImportOAuthString, and OidcSignIn.cpp's Setting(),
# which is GetSignInString. Until 23 September 2026 the fifteen single sign-on,
# passkey and mail-import rows they read were 'either' - "nothing in this build
# reads it" - and hmailserver --apply-environment refused them on that word,
# the client secrets among them (docs/EnvironmentConfiguration.md).
STRING_ACCESS = re.compile(r"\b(?:(?:Get|Set)(?:SignIn|ImportOAuth)String|Setting)\s*\(\s*(PROPERTY_[A-Z0-9_]+)\s*[,)]")


class Unresolved(Exception):
    pass


def read(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def server_sources():
    for base, _, names in os.walk(SERVER):
        if os.sep + "x64" in base:
            continue
        for name in sorted(names):
            if name.endswith((".cpp", ".h")):
                yield os.path.join(base, name)


_constants = None


def named_constant(name):
    """A Class::NAME the generator met: an enumerator, or a string constant.

    Read out of the server's own sources rather than copied here, so that
    changing the constant changes the default the next run writes - which is the
    whole point of generating this file instead of keeping it."""
    global _constants
    if _constants is None:
        _constants = {}
        enumerator = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(-?\d+|0[xX][0-9a-fA-F]+)\s*,?\s*$", re.M)
        # const wchar_t *, const TCHAR * and const char * alike: an AnsiString
        # default (ReputationWatch::DefaultLists) is as much a constant as a wide one.
        text_constant = re.compile(r"(?:const\s+(?:wchar_t|TCHAR|char)\s*\*(?:\s*const)?)\s*"
                                   r"[A-Za-z_][A-Za-z0-9_]*::([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
                                   r"(?:_T|L)?\(?\s*\"((?:[^\"\\]|\\.)*)\"\s*\)?\s*;")
        for path in server_sources():
            source = read(path)
            for match in enumerator.finditer(source):
                _constants.setdefault(match.group(1), int(match.group(2), 0))
            for match in text_constant.finditer(source):
                _constants.setdefault(match.group(1), match.group(2))
    if name not in _constants:
        raise Unresolved(name)
    return _constants[name]


def resolve(text):
    """One Ensure* value, as (string, number). Raises Unresolved for anything it
    cannot work out, which is deliberate: see the module docstring."""
    text = " ".join(text.split())
    literal = STRING_LITERAL.match(text)
    if literal:
        return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "r": "\r"}.get(m.group(1), m.group(1)), literal.group(1)), 0
    if NUMBER.match(text):
        return "", int(text, 0)
    if "|" in text:
        total = 0
        for part in text.split("|"):
            _, number = resolve(part.strip())
            total |= number
        return "", total
    # String(Class::NAME) is the same constant wrapped for the String overload.
    wrapped = re.match(r"^(?:String|AnsiString)\((.*)\)$", text)
    if wrapped:
        return resolve(wrapped.group(1))
    qualified = QUALIFIED.match(text)
    if qualified:
        value = named_constant(qualified.group(1))
        if isinstance(value, int):
            return "", value
        return value, 0
    raise Unresolved(text)


def property_names():
    """The hm_settings names the server has a constant for, lower-cased.

    A seeded row that is not one of these is a row nothing reads: a leftover of
    a feature that went, kept because dropping a column from a create script
    changes nothing and costs a schema step. They stay in the table - a server
    that has one should not be told it holds a setting nobody has heard of -
    but they are not held to agreeing across the dialects, because there is no
    behaviour for them to disagree about."""
    return set(name.lower() for _, name in DEFINE.findall(read(CONSTANTS)))


def from_create_scripts():
    """The seeded rows, and a complaint for every dialect that disagrees."""
    tables = {}
    for name in CREATE_SCRIPTS:
        path = os.path.join(SCRIPTS, name)
        rows = {}
        for match in INSERT.finditer(read(path)):
            rows[match.group(1)] = (match.group(2).replace("''", "'"), int(match.group(3)))
        tables[name] = rows

    read_by_the_server = property_names()
    base = tables[CREATE_SCRIPTS[0]]
    problems = []
    for name in CREATE_SCRIPTS[1:]:
        other = tables[name]
        for key in sorted(set(base) | set(other)):
            if base.get(key) == other.get(key):
                continue
            if key.lower() not in read_by_the_server:
                continue
            problems.append("%s seeds hm_settings.%s as %r and %s as %r; a default cannot depend "
                            "on which database was chosen" % (CREATE_SCRIPTS[0], key, base.get(key), name, other.get(key)))
    return base, problems


def from_configuration():
    """The rows Configuration::EnsureSettingDefaults_ inserts."""
    names = dict(DEFINE.findall(read(CONSTANTS)))
    rows = {}
    problems = []
    source = read(CONFIGURATION)
    start = source.find("Configuration::EnsureSettingDefaults_")
    if start < 0:
        return rows, ["Configuration.cpp no longer has EnsureSettingDefaults_, which is where the "
                      "defaults of a property added after the create script live"]
    end = source.find("\n   }", start)
    for match in ENSURE.finditer(source[start:end if end > start else len(source)]):
        token = match.group(2)
        key = names.get(token)
        if key is None:
            literal = STRING_LITERAL.match(token)
            key = literal.group(1) if literal else None
        if key is None:
            problems.append("EnsureSettingDefaults_ names %s, which Constants.h does not define" % token)
            continue
        try:
            value, number = resolve(match.group(3))
        except Unresolved as bad:
            problems.append("EnsureSettingDefaults_ gives %s the default `%s`, which this generator cannot "
                            "work out. Give it a literal, or teach resolve() the constant." % (key, bad))
            continue
        rows[key] = (value, number)
    return rows, problems


def kinds():
    """How each property is read, from the calls that read it: {name: kind}.

    hm_settings has a settingstring and a settinginteger and the row does not say
    which one a property uses, so without this a property whose shipped value is
    an empty string and one whose shipped value is the number 0 are the same row,
    and an answer for either is a guess. The server's own PropertySet calls say
    it - GetString, GetLong, GetBool - and are read here rather than listed, so a
    property that changes type changes this file at the next run.

    A property no such call names keeps the kind 'either', and a caller compares
    both columns; there are a handful, all of them rows nothing reads any more."""
    found = {}
    names = dict(DEFINE.findall(read(CONSTANTS)))
    for path in server_sources():
        source = read(path)
        for match in ACCESS.finditer(source):
            token = match.group(2)
            key = names.get(token)
            if key is None:
                literal = STRING_LITERAL.match(token)
                key = literal.group(1) if literal else None
            if key is None:
                continue
            kind = "string" if match.group(1) == "String" else "number"
            if found.setdefault(key.lower(), kind) != kind:
                found[key.lower()] = "either"
        for match in STRING_ACCESS.finditer(source):
            key = names.get(match.group(1))
            if key is None:
                continue
            if found.setdefault(key.lower(), "string") != "string":
                found[key.lower()] = "either"
    return found


def cpp_literal(value):
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "\\r").replace("\n", "\\n")


def read_levels():
    """build/settings-levels.txt as {name: mark}, and the lines it cannot read."""
    marks = {}
    problems = []
    if not os.path.exists(LEVELS_FILE):
        return marks, ["build/settings-levels.txt is missing: every property needs its levels mark"]
    for number, line in enumerate(read(LEVELS_FILE).splitlines(), 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        parts = text.split()
        if len(parts) != 2 or parts[1] not in LEVELS:
            problems.append("build/settings-levels.txt line %d is not `name server|domain|account`: %s" % (number, text))
            continue
        if parts[0] in marks:
            problems.append("build/settings-levels.txt marks %s twice" % parts[0])
            continue
        marks[parts[0]] = parts[1]
    return marks, problems


def generate():
    seeded, problems = from_create_scripts()
    ensured, more = from_configuration()
    problems += more
    if problems:
        for problem in problems:
            print("  " + problem)
        raise SystemExit("FAIL: %d problem(s); nothing was written." % len(problems))

    rows = dict(seeded)
    # A property in both is in both because the create script has caught up with
    # a property that was once only ensured. They agree or the create script
    # wins: it is what a fresh database actually gets.
    for key, value in ensured.items():
        rows.setdefault(key, value)

    # The levels marks: one for every row, and none for a row there is not.
    marks, problems = read_levels()
    for key in sorted(rows, key=lambda k: k.lower()):
        if key not in marks:
            problems.append("%s has no levels mark in build/settings-levels.txt: say whether it is set for the whole "
                            "server only (server), per domain (domain) or per account as well (account)" % key)
    for key in sorted(set(marks) - set(rows)):
        problems.append("build/settings-levels.txt marks %s, which no create script seeds and "
                        "Configuration::EnsureSettingDefaults_ does not ensure" % key)
    if problems:
        for problem in problems:
            print("  " + problem)
        raise SystemExit("FAIL: %d problem(s); nothing was written." % len(problems))

    lines = [
        "// https://www.progressiverobot.com",
        # REUSE-IgnoreStart
        "// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd",
        "// SPDX-License-Identifier: AGPL-3.0-or-later",
        # REUSE-IgnoreEnd
        "",
        "// GENERATED by build/generate-settings-defaults.py from the create scripts'",
        "// `insert into hm_settings` rows and Configuration::EnsureSettingDefaults_, and",
        "// held to them by that script's --check on every pull request. Do not edit.",
        "//",
        "// What a database made by THIS version has in hm_settings before anybody has",
        "// administered anything, so that the server can say which of its settings are",
        "// not that. Both columns are here because the table has both - Value is",
        "// settingstring and Number is settinginteger - and Kind says which of them the",
        "// property actually uses, read from the PropertySet calls that read it:",
        "//",
        "//   KindString   GetString: the number column is nobody's and stays 0",
        "//   KindNumber   GetLong or GetBool: the string column is nobody's and stays empty",
        "//   KindEither   no read names one column only, so neither can be ruled out; a",
        "//                caller compares both. A handful of rows a create script still",
        "//                seeds for a feature that has gone, which nothing reads, and",
        "//                backupencryptionpassphrase, read both ways: the text is the",
        "//                passphrase and the number whether one is set.",
        "//",
        "// Levels is the deepest level the property may be set at - the whole server",
        "// only, a domain, or an account as well - from build/settings-levels.txt. A",
        "// property marked LevelDomain or LevelAccount is resolved by",
        "// Application/EffectiveSetting at every read (docs/SettingLevels.md).",
        "//",
        "// PropertyNames is every hm_settings name this build has a constant for. An",
        "// installation upgraded from an old schema holds rows no create script ever",
        "// made - useordb, usespamhaus, tarpitcount and nine more, inserted by upgrade",
        "// scripts for features that have gone - and a row nothing reads cannot explain",
        "// anything a server is doing. SettingsDiff reports only the rows in this list.",
        "//",
        "// The [Settings] keys of hMailServer.ini are the other half of the same answer",
        "// and are in IniSettingsBaseline.h. SettingsDiff puts the two together.",
        "",
        "#pragma once",
        "",
        "namespace HM",
        "{",
        "   namespace SettingsDefaults",
        "   {",
        "      enum SettingKind",
        "      {",
        "         KindEither = 0,",
        "         KindString = 1,",
        "         KindNumber = 2",
        "      };",
        "",
        "      enum LevelMark",
        "      {",
        "         LevelServer = 0,",
        "         LevelDomain = 1,",
        "         LevelAccount = 2",
        "      };",
        "",
        "      struct Setting",
        "      {",
        "         const TCHAR *Name;",
        "         const TCHAR *Value;",
        "         long Number;",
        "         SettingKind Kind;",
        "         LevelMark Levels;",
        "      };",
        "",
        "      static const Setting Settings[] =",
        "      {",
    ]
    read_as = kinds()
    for key in sorted(rows, key=lambda k: k.lower()):
        value, number = rows[key]
        kind = {"string": "KindString", "number": "KindNumber"}.get(read_as.get(key.lower()), "KindEither")
        lines.append('         { _T("%s"), _T("%s"), %d, %s, %s },'
                     % (cpp_literal(key), cpp_literal(value), number, kind, LEVEL_WORD[marks[key]]))
    lines += [
        "      };",
        "",
        "      static const size_t SettingsCount = sizeof(Settings) / sizeof(Settings[0]);",
        "",
        "      // Every hm_settings name this build reads, lower-cased, whether or not it",
        "      // is seeded above. A row that is in neither list is a fossil an upgrade",
        "      // script left behind.",
        "      static const TCHAR *const PropertyNames[] =",
        "      {",
    ]
    for key in sorted(property_names()):
        lines.append('         _T("%s"),' % cpp_literal(key))
    lines += [
        "      };",
        "",
        "      static const size_t PropertyNameCount = sizeof(PropertyNames) / sizeof(PropertyNames[0]);",
        "   }",
        "}",
    ]
    return "\n".join(lines) + "\n", len(rows)


def main():
    text, count = generate()

    if "--print" in sys.argv:
        sys.stdout.write(text)
        return 0

    if "--check" in sys.argv:
        if not os.path.exists(OUT):
            print("  %s is missing; run python3 build/generate-settings-defaults.py"
                  % os.path.relpath(OUT, ROOT))
            return 1
        if read(OUT) != text:
            print("  %s is not what build/generate-settings-defaults.py would write now. A default "
                  "seeded by a create script or ensured in Configuration.cpp has changed and the "
                  "header has not: run python3 build/generate-settings-defaults.py in the same commit."
                  % os.path.relpath(OUT, ROOT))
            return 1
        print("OK    %s carries the %d hm_settings default(s) the create scripts and "
              "Configuration.cpp give" % (os.path.basename(OUT), count))
        return 0

    with io.open(OUT, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(text)
    print("Wrote %s: %d hm_settings default(s)." % (os.path.basename(OUT), count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
