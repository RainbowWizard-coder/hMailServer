# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every database setting has a row on a database made before it.

The server reads its database settings out of hm_settings into a property set
when it starts, and a read of a name that has no row is reported to the error
log as HM5015 every single time - PropertySet::GetProperty_. A setting added in
a new build therefore needs its row on every database that already exists, and
there are two ways this project gives it one:

  * Configuration::EnsureSettingDefaults_ calls EnsureLong or EnsureString for
    it, which creates the row with its default at the next start; or
  * a database script inserts it - CreateTables for a new database, an
    Upgrade step for an old one.

SettingsDefaults.h names every database setting there is, with the default the
"changed from stock" report measures it against. A name there that neither an
Ensure call nor any script mentions is a setting with no row on an existing
database. The quota policy's two server-wide shares were exactly that on 17
September 2026: the fourth batch-12 gate logged HM5015 over nine thousand times
and failed 458 tests in their SetUp, because every delivery reads the grace.

And every name fits the column it is kept in: hm_settings.settingname is
nvarchar(30), and SQL Server Compact refuses - with "Unknown error", HM10046 -
any comparison of that column with a longer parameter, so a longer name has no
row, cannot be given one, and fails every read. Three names of batch 13 were 32
and 35 characters on 17 September 2026 and the server logged errors at start.

And every name carries a LEVELS MARK in build/settings-levels.txt - server,
domain or account, the deepest level the setting may be set at (one value, three
levels, docs/SettingLevels.md) - and SettingsDefaults.h carries it as the row's
Levels. A name without one is refused: a setting added without anybody deciding
whether a domain or an account may hold its own value is the setting the next
"why is this account not doing what the domain says" is about. The check proves
its own negative control on every run: a list missing a name, and a mark that is
not one of the three, are fed to it and must be refused.

Usage: python build/check-settings-rows.py
"""
import io
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
APPLICATION = os.path.join(ROOT, 'hmailserver', 'source', 'Server', 'Common', 'Application')
DEFAULTS = os.path.join(APPLICATION, 'SettingsDefaults.h')
CONFIGURATION = os.path.join(APPLICATION, 'Configuration.cpp')
CONSTANTS = os.path.join(APPLICATION, 'Constants.h')
SCRIPTS = os.path.join(ROOT, 'hmailserver', 'source', 'DBScripts')

SETTING = re.compile(r'\{\s*_T\("([^"]+)"\)\s*,\s*_T\(')
# A row with its levels mark, as the generator writes it.
SETTING_LEVEL = re.compile(r'\{\s*_T\("([^"]+)"\)\s*,\s*_T\("(?:[^"\\]|\\.)*"\)\s*,\s*-?\d+\s*,\s*Kind\w+\s*,\s*Level(Server|Domain|Account)\s*\}')
LEVELS_FILE = os.path.join(ROOT, 'build', 'settings-levels.txt')
LEVELS = ('server', 'domain', 'account')
CONSTANT = re.compile(r'#define\s+(PROPERTY_\w+)\s+_T\("([^"]+)"\)')
ENSURE = re.compile(r'\bEnsure(?:Long|String)\s*\(\s*(?:(PROPERTY_\w+)|_T\("([^"]+)"\))')


def read(path):
    return io.open(path, encoding='utf-8-sig', errors='replace').read()


def levels_problems(names, text, header):
    """The names without a levels mark, the marks that are not one of the three,
    and the rows of the header whose Levels is not the list's. Empty when all is
    well. text is build/settings-levels.txt, header is SettingsDefaults.h."""
    problems = []
    marks = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) != 2 or parts[1] not in LEVELS:
            problems.append('build/settings-levels.txt line %d: `%s` is not a name and one of server, domain or account' % (number, line))
            continue
        marks[parts[0]] = parts[1]
    for name in names:
        if name not in marks:
            problems.append('%s: named in SettingsDefaults.h and given no levels mark in build/settings-levels.txt - say whether '
                            'it is set for the whole server only (server), per domain (domain) or per account as well (account)' % name)
    in_header = dict((name, level.lower()) for name, level in SETTING_LEVEL.findall(header))
    for name in names:
        if name in marks and in_header.get(name) != marks[name]:
            problems.append('%s: SettingsDefaults.h carries Levels %s and build/settings-levels.txt says %s; run '
                            'python3 build/generate-settings-defaults.py' % (name, in_header.get(name, 'none'), marks[name]))
    return problems


def self_test():
    """The negative control: a list that misses a name, and a mark that is not
    one of the three, must each be refused."""
    failures = []
    header = '{ _T("RecallDays"), _T(""), 14, KindNumber, LevelAccount },'
    if levels_problems(['RecallDays'], 'RecallDays account\n', header):
        failures.append('self-test: a well-formed list was refused')
    if not any('no levels mark' in p for p in levels_problems(['RecallDays'], '# nothing\n', header)):
        failures.append('self-test: a name with no levels mark was not refused')
    if not any('is not a name and one of' in p for p in levels_problems(['RecallDays'], 'RecallDays mailbox\n', header)):
        failures.append('self-test: a mark that is not one of the three was not refused')
    return failures


def main():
    names = SETTING.findall(read(DEFAULTS))
    if not names:
        print('FAIL: no setting names were read from SettingsDefaults.h; the pattern no longer matches the table')
        return 1

    constants = dict(CONSTANT.findall(read(CONSTANTS)))

    too_long = sorted(set(n for n in list(names) + list(constants.values()) if len(n) > 30 and n in names))
    if too_long:
        for name in too_long:
            print('%s: %d characters, and hm_settings.settingname holds 30 - SQL Server Compact refuses the row' % (name, len(name)))
        print('FAIL: %d setting name(s) do not fit the settings table' % len(too_long))
        return 1
    ensured = set()
    for constant, literal in ENSURE.findall(read(CONFIGURATION)):
        name = literal or constants.get(constant)
        if name is None:
            print('FAIL: Configuration.cpp ensures %s, which Constants.h does not define' % constant)
            return 1
        ensured.add(name.lower())

    scripted = ''
    for entry in sorted(os.listdir(SCRIPTS)):
        if entry.lower().endswith('.sql'):
            scripted += read(os.path.join(SCRIPTS, entry)).lower()

    missing = [n for n in names if n.lower() not in ensured and ("'" + n.lower() + "'") not in scripted]
    if missing:
        for name in missing:
            print('%s: named in SettingsDefaults.h, but no Ensure call in Configuration::EnsureSettingDefaults_ creates its row '
                  'and no database script inserts it, so every read of it on an existing database reports HM5015' % name)
        print('FAIL: %d of %d database setting(s) have no row on a database made before them' % (len(missing), len(names)))
        return 1

    failures = self_test()
    if failures:
        for failure in failures:
            print(failure)
        print('FAIL: the levels check no longer refuses what it must')
        return 1

    problems = levels_problems(names, read(LEVELS_FILE) if os.path.exists(LEVELS_FILE) else '', read(DEFAULTS))
    if problems:
        for problem in problems:
            print(problem)
        print('FAIL: %d database setting(s) without the levels mark that says how deep they may be set' % len(problems))
        return 1

    marked = read(LEVELS_FILE)
    counted = dict((level, len(re.findall(r'^\S+ %s\s*$' % level, marked, re.M))) for level in LEVELS)
    print('OK    %d database setting(s): each has its row created at start-up (%d) or inserted by a database script, '
          'and each its levels mark (%d server, %d domain, %d account; a name without one is refused)' %
          (len(names), len(ensured), counted['server'], counted['domain'], counted['account']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
