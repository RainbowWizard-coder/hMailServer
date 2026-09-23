#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every setting name the environment's documents mention is a setting.

Every setting has a name in the environment (docs/EnvironmentConfiguration.md):
HM_SETTING_<key> for a [Settings] key - one of build/ini-settings-baseline.txt -
and HM_PROPERTY_<name> for an hm_settings row - one SettingsDefaults.h lists and
this build reads - either with _FILE after it for a value read from a file. The
server refuses any other name when it is run (hmailserver --apply-environment),
with the nearest one named; but a name written into the container's
entrypoint, the Helm chart, the Compose file or the documents is one somebody
copies, and a misspelled example is a container that does not start. So this
reads them, and fails on any that is not a setting:

  hmailserver/source/Server/platform/packaging/docker/docker-entrypoint.sh
  hmailserver/source/Server/platform/packaging/docker/docker-compose.yml
  hmailserver/source/Server/platform/packaging/docker/Dockerfile
  hmailserver/source/Server/platform/packaging/helm/hmailserver/values.yaml
      - also the keys under settings: and properties:, the examples commented
        out included, and the setting: and property: of each settingsFrom entry
  hmailserver/source/Server/platform/packaging/README.md
  hmailserver/docs/EnvironmentConfiguration.md
  hmailserver/docs/ChangedFromStock.md

And it holds three things the naming rule rests on:

  * no [Settings] key and no property ends in _FILE, so the suffix is never
    part of a name (Common/Application/EnvironmentSettings.cpp relies on it);
  * the four aliases the container has always taken - HM_REST_PORT and the
    other three, in EnvironmentSettings.cpp's table - name [Settings] keys, and
    the entrypoint's comment names each of them;
  * the marker row, EnvironmentManagedSettings, is not a property a variable
    could name, and fits the 30 characters of hm_settings.settingname.

It proves its own negative control on every run: a copy of the document with a
made-up HM_SETTING_ name, and one with a made-up HM_PROPERTY_ name, must both be
refused, and a values file with a made-up key under settings: as well.

Usage: python build/check-environment-names.py
"""
import io
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
BASELINE = os.path.join(ROOT, 'build', 'ini-settings-baseline.txt')
DEFAULTS = os.path.join(ROOT, 'hmailserver', 'source', 'Server', 'Common', 'Application', 'SettingsDefaults.h')
IMPLEMENTATION = os.path.join(ROOT, 'hmailserver', 'source', 'Server', 'Common', 'Application', 'EnvironmentSettings.cpp')
PACKAGING = os.path.join(ROOT, 'hmailserver', 'source', 'Server', 'platform', 'packaging')
ENTRYPOINT = os.path.join(PACKAGING, 'docker', 'docker-entrypoint.sh')
VALUES = os.path.join(PACKAGING, 'helm', 'hmailserver', 'values.yaml')
DOCUMENT = os.path.join(ROOT, 'hmailserver', 'docs', 'EnvironmentConfiguration.md')

FILES = [
    ENTRYPOINT,
    os.path.join(PACKAGING, 'docker', 'docker-compose.yml'),
    os.path.join(PACKAGING, 'docker', 'Dockerfile'),
    VALUES,
    os.path.join(PACKAGING, 'README.md'),
    DOCUMENT,
    os.path.join(ROOT, 'hmailserver', 'docs', 'ChangedFromStock.md'),
]

# HM_SETTING_<name> or HM_PROPERTY_<name>, a name being letters, digits and
# underscores. A placeholder - HM_SETTING_<key> - has no name after the prefix and
# is not one.
VARIABLE = re.compile(r'\bHM_(SETTING|PROPERTY)_([A-Za-z0-9_]+)')
ROW = re.compile(r'\{\s*_T\("([^"]+)"\)\s*,\s*_T\("(?:[^"\\]|\\.)*"\)\s*,\s*-?\d+\s*,\s*(Kind\w+)\s*,\s*Level\w+\s*\}')
ALIAS = re.compile(r'\{\s*_T\("(HM_REST_[A-Z_]+)"\)\s*,\s*_T\("([A-Za-z0-9]+)"\)\s*\}')
MARKER = re.compile(r'EnvironmentSettings::MarkerName\(\)\s*\{\s*return\s+_T\("([^"]+)"\)')
MAP_KEY = re.compile(r'^\s*#?\s{0,4}([A-Za-z][A-Za-z0-9_]*):')
FROM_NAME = re.compile(r'^\s*#?\s*(?:-\s*)?(setting|property):\s*([A-Za-z0-9_]+)\s*$')


def read(path):
    return io.open(path, encoding='utf-8-sig', errors='replace').read()


def baseline_keys():
    keys = set()
    for line in read(BASELINE).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] == 'Settings':
            keys.add(parts[1])
    return keys


def properties():
    """The properties a variable may name: the rows SettingsDefaults.h lists with
    a kind this build reads (KindEither is a row nothing reads)."""
    names = set()
    for name, kind in ROW.findall(read(DEFAULTS)):
        if kind != 'KindEither':
            names.add(name)
    return names


def resolve(kind, name, keys, props):
    """None when the name is a setting of the store its prefix says; the problem
    otherwise."""
    base = name[:-len('_FILE')] if name.endswith('_FILE') else name
    lower_keys = dict((k.lower(), k) for k in keys)
    lower_props = dict((p.lower(), p) for p in props)
    if kind == 'SETTING':
        if base.lower() in lower_keys:
            return None
        if base.lower() in lower_props:
            return 'HM_SETTING_%s: %s is an hm_settings property, which HM_PROPERTY_%s sets' % (name, base, lower_props[base.lower()])
        return 'HM_SETTING_%s: %s is not a [Settings] key in build/ini-settings-baseline.txt' % (name, base)
    if base.lower() in lower_props:
        return None
    if base.lower() in lower_keys:
        return 'HM_PROPERTY_%s: %s is a [Settings] key, which HM_SETTING_%s sets' % (name, base, lower_keys[base.lower()])
    return 'HM_PROPERTY_%s: %s is not a property SettingsDefaults.h lists with a kind this build reads' % (name, base)


def values_names(text):
    """The keys under settings: and properties: in a values file, examples
    commented out included, and each settingsFrom entry's setting or property."""
    found = []
    section = None
    for line in text.splitlines():
        stripped = line.strip()
        if re.match(r'^(settings|properties|settingsFrom):', line):
            section = line.split(':', 1)[0]
            continue
        if line and not line.startswith(' ') and not line.startswith('#'):
            section = None
            continue
        if section is None or not stripped:
            continue
        if section in ('settings', 'properties'):
            # An entry indented two spaces, live or commented out ("#  key: value");
            # a comment of prose ("# Settings whose ...") has one space after its #
            # and no colon after its first word.
            m = re.match(r'^#?\s{2}([A-Za-z][A-Za-z0-9_]*):(\s|$)', line)
            if m:
                found.append(('SETTING' if section == 'settings' else 'PROPERTY', m.group(1)))
        elif section == 'settingsFrom':
            m = FROM_NAME.match(line)
            if m:
                found.append(('SETTING' if m.group(1) == 'setting' else 'PROPERTY', m.group(2)))
    return found


def check_files(paths, keys, props):
    problems = []
    for path in paths:
        if not os.path.exists(path):
            problems.append('%s: missing' % os.path.relpath(path, ROOT))
            continue
        text = read(path)
        relative = os.path.relpath(path, ROOT).replace('\\', '/')
        for number, line in enumerate(text.splitlines(), 1):
            for kind, name in VARIABLE.findall(line):
                problem = resolve(kind, name, keys, props)
                if problem:
                    problems.append('%s:%d: %s' % (relative, number, problem))
        if os.path.basename(path) == 'values.yaml':
            for kind, name in values_names(text):
                problem = resolve(kind, name, keys, props)
                if problem:
                    problems.append('%s: the %s %s: %s' % (relative, 'settings key' if kind == 'SETTING' else 'property', name, problem))
    return problems


def check_rule(keys, props):
    problems = []
    for name in sorted(keys | props):
        if name.upper().endswith('_FILE'):
            problems.append('%s ends in _FILE, which the environment reads as "the value is in this file": rename it or change the rule' % name)

    implementation = read(IMPLEMENTATION)
    aliases = ALIAS.findall(implementation)
    if len(aliases) != 4:
        problems.append('EnvironmentSettings.cpp: expected the four HM_REST_* aliases in its table, found %d' % len(aliases))
    entrypoint = read(ENTRYPOINT)
    for variable, key in aliases:
        if key not in keys:
            problems.append('EnvironmentSettings.cpp: the alias %s names %s, which is not a [Settings] key' % (variable, key))
        if variable not in entrypoint:
            problems.append('docker-entrypoint.sh does not name the alias %s' % variable)

    marker = MARKER.search(implementation)
    if not marker:
        problems.append('EnvironmentSettings.cpp: MarkerName() could not be read')
    else:
        name = marker.group(1)
        if len(name) > 30:
            problems.append('the marker row %s is longer than the 30 characters hm_settings.settingname holds' % name)
        if name.lower() in set(p.lower() for p in props):
            problems.append('the marker row %s is a property SettingsDefaults.h lists, so HM_PROPERTY_%s would name it' % (name, name))
    return problems


def negative_controls(keys, props):
    """Each must be refused; the list of those that were not."""
    failures = []
    scratch = tempfile.mkdtemp(prefix='check-environment-names-')
    try:
        document = read(DOCUMENT)
        for kind, made_up in (('SETTING', 'NoSuchSettingAnywhere'), ('PROPERTY', 'nosuchpropertyanywhere')):
            copy = os.path.join(scratch, 'EnvironmentConfiguration.md')
            with io.open(copy, 'w', encoding='utf-8') as handle:
                handle.write(document + '\n\nHM_%s_%s=1\n' % (kind, made_up))
            if not check_files([copy], keys, props):
                failures.append('a made-up HM_%s_%s in a copy of the document was not refused' % (kind, made_up))

        values = read(VALUES)
        copy = os.path.join(scratch, 'values.yaml')
        with io.open(copy, 'w', encoding='utf-8') as handle:
            handle.write(values.replace('settings: {}', 'settings:\n  NoSuchSettingAnywhere: "1"', 1))
        if not check_files([copy], keys, props):
            failures.append('a made-up key under settings: in a copy of values.yaml was not refused')
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return failures


def main():
    keys = baseline_keys()
    props = properties()
    if not keys or not props:
        print('check-environment-names: could not read the baseline or SettingsDefaults.h')
        return 1

    problems = check_rule(keys, props) + check_files(FILES, keys, props)
    failures = negative_controls(keys, props)

    for problem in problems:
        print('check-environment-names: ' + problem)
    for failure in failures:
        print('check-environment-names: self-test: ' + failure)

    if problems or failures:
        return 1

    print('check-environment-names: every HM_SETTING_ and HM_PROPERTY_ name in the entrypoint, the chart, the Compose file and the documents is a setting '
          '(%d [Settings] keys, %d properties); the negative controls - a made-up name in a copy of the document and in a copy of the values - are refused.'
          % (len(keys), len(props)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
