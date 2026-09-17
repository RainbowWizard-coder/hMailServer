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
CONSTANT = re.compile(r'#define\s+(PROPERTY_\w+)\s+_T\("([^"]+)"\)')
ENSURE = re.compile(r'\bEnsure(?:Long|String)\s*\(\s*(?:(PROPERTY_\w+)|_T\("([^"]+)"\))')


def read(path):
    return io.open(path, encoding='utf-8-sig', errors='replace').read()


def main():
    names = SETTING.findall(read(DEFAULTS))
    if not names:
        print('FAIL: no setting names were read from SettingsDefaults.h; the pattern no longer matches the table')
        return 1

    constants = dict(CONSTANT.findall(read(CONSTANTS)))
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

    print('OK    %d database setting(s): each has its row created at start-up (%d) or inserted by a database script' %
          (len(names), len(ensured)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
