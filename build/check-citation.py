#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""CITATION.cff names the release the tree is at, and says what it is.

CITATION.cff, at the root, is how the project is cited, in the Citation File Format
1.2.0: GitLab and GitHub show it as "Cite this repository", and an archive such as
Zenodo makes its record of a release from it - a record that can never be deleted.
Its version and date-released are written by hand at each release's stamp
(RELEASE.md, step 7), so this holds them to the tree: the version to
Common/Application/Version.h's HMAILSERVER_VERSION, and the date to the date of
CHANGELOG.md's section for that version, which is written with the same stamp. It
also holds what the file says the project is - Progressive Robot's fork of
hMailServer, with the original, Martin Knafve's, cited in its references - and that
it claims no DOI, since none has been made.

The schema itself is the format's (citation-file-format.github.io/1.2.0/schema.json);
`cffconvert --validate -i CITATION.cff` checks the file against it, and needs the
network to install. This reads the file with nothing but Python, as the checkers job
runs every build/check-*.py, and that job runs --self-test too, with the other
checks' negative controls.

  python build/check-citation.py [--self-test]
"""
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
CITATION = 'CITATION.cff'
VERSION_H = os.path.join('hmailserver', 'source', 'Server', 'Common', 'Application', 'Version.h')
CHANGELOG = 'CHANGELOG.md'
FORK = "Progressive Robot's fork of hMailServer"


def read(root, relative):
    with open(os.path.join(root, relative), encoding='utf-8') as f:
        return f.read().replace('\r\n', '\n')


def top_level(text, key):
    """A top-level key's value on its own line, unquoted; None where it has none."""
    match = re.search(r'^%s:[ \t]*(.*?)[ \t]*$' % re.escape(key), text, re.M)
    if not match:
        return None
    value = match.group(1)
    if len(value) >= 2 and value[0] == value[-1] and value[0] in '"\'':
        value = value[1:-1]
    return value


def block(text, key):
    """A top-level key's indented block - a folded scalar or a list - as one line."""
    match = re.search(r'^%s:[^\n]*\n((?:[ \t]+[^\n]*\n|\n)*)' % re.escape(key), text, re.M)
    return ' '.join(match.group(1).split()) if match else ''


def check(root):
    problems = []
    try:
        citation = read(root, CITATION)
    except OSError:
        return ['%s is not at the root of the tree' % CITATION]
    version_h = read(root, VERSION_H)
    changelog = read(root, CHANGELOG)

    found = re.search(r'^#define\s+HMAILSERVER_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"', version_h, re.M)
    version = found.group(1) if found else None
    if not version:
        problems.append('%s gives no HMAILSERVER_VERSION' % VERSION_H)

    if top_level(citation, 'cff-version') != '1.2.0':
        problems.append('%s: cff-version is %r, not 1.2.0' % (CITATION, top_level(citation, 'cff-version')))
    if top_level(citation, 'type') != 'software':
        problems.append('%s: type is %r, not software' % (CITATION, top_level(citation, 'type')))
    if top_level(citation, 'title') != 'hMailServer':
        problems.append('%s: title is %r, not hMailServer' % (CITATION, top_level(citation, 'title')))
    if top_level(citation, 'license') != 'AGPL-3.0-or-later':
        problems.append('%s: license is %r, not AGPL-3.0-or-later' % (CITATION, top_level(citation, 'license')))
    if top_level(citation, 'repository-code') != 'https://gitlab.com/hmailserver/hmailserver':
        problems.append('%s: repository-code is %r, not the project on GitLab' % (CITATION, top_level(citation, 'repository-code')))

    cited = top_level(citation, 'version')
    if version and cited != version:
        problems.append('%s: version is %r, but Version.h says %s - stamp it with the release (RELEASE.md, step 7)'
                        % (CITATION, cited, version))
    released = top_level(citation, 'date-released')
    if version:
        heading = re.search(r'^## \[%s\]\([^)]*\) - ([0-9]{4}-[0-9]{2}-[0-9]{2})' % re.escape(version), changelog, re.M)
        if not heading:
            problems.append('%s has no section for %s with a date' % (CHANGELOG, version))
        elif released != heading.group(1):
            problems.append('%s: date-released is %r, but %s dates %s %s' % (
                CITATION, released, CHANGELOG, version, heading.group(1)))

    if FORK not in block(citation, 'abstract'):
        problems.append('%s: the abstract does not say %r' % (CITATION, FORK))
    references = block(citation, 'references')
    if 'type: software' not in references or 'title: hMailServer' not in references \
            or 'given-names: Martin' not in references or 'family-names: Knafve' not in references:
        problems.append('%s: the references do not cite the original hMailServer, software by Martin Knafve' % CITATION)
    if re.search(r'^\s*(?:-\s*)?(?:doi|identifiers)\s*:', citation, re.M) or re.search(r'\b10\.[0-9]{4,}/', citation):
        problems.append('%s claims a DOI, and the project has none' % CITATION)
    return problems


GOOD = '''cff-version: 1.2.0
message: Cite it.
type: software
title: hMailServer
abstract: >-
  Progressive Robot's fork of
  hMailServer, not the original.
authors:
  - name: Progressive Robot Ltd
version: 9.9.9
date-released: 2026-01-02
license: AGPL-3.0-or-later
repository-code: https://gitlab.com/hmailserver/hmailserver
references:
  - type: software
    title: hMailServer
    authors:
      - given-names: Martin
        family-names: Knafve
'''
CHANGES = '# Changelog\n\n## Unreleased - in the tree\n\n---\n\n## [9.9.9](https://example.org) - 2026-01-02\n\nText.\n'
HEADER = '#define HMAILSERVER_VERSION "9.9.9"\n'


def self_test():
    failures = []

    def expect(what, condition):
        print('%s  %s' % ('ok  ' if condition else 'FAIL', what))
        if not condition:
            failures.append(what)

    def world(citation=GOOD, changes=CHANGES, header=HEADER):
        work = tempfile.mkdtemp(prefix='hm-citation-')
        os.makedirs(os.path.join(work, os.path.dirname(VERSION_H)))
        for relative, text in ((VERSION_H, header), (CHANGELOG, changes), (CITATION, citation)):
            if text is not None:
                with open(os.path.join(work, relative), 'w', encoding='utf-8', newline='\r\n') as f:
                    f.write(text)
        try:
            return check(work)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    expect('a file at the release, dated by its section, citing the original: passes', world() == [])
    cases = [
        ('a version that is not Version.h\'s', GOOD.replace('version: 9.9.9', 'version: 9.9.8'), CHANGES, 'Version.h says 9.9.9'),
        ('a date that is not the section\'s', GOOD.replace('2026-01-02', '2026-01-03'), CHANGES, 'dates 9.9.9 2026-01-02'),
        ('a release with no section', GOOD, CHANGES.replace('[9.9.9]', '[9.9.7]'), 'no section for 9.9.9'),
        ('an abstract that does not say fork', GOOD.replace("Progressive Robot's fork of\n  hMailServer", 'A mail\n  server'), CHANGES, 'the abstract'),
        ('no reference to the original', GOOD.split('references:')[0], CHANGES, 'the references'),
        ('another schema version', GOOD.replace('cff-version: 1.2.0', 'cff-version: 1.1.0'), CHANGES, 'cff-version'),
        ('another licence', GOOD.replace('AGPL-3.0-or-later', 'GPL-3.0-only'), CHANGES, 'license is'),
        ('a DOI', GOOD + 'doi: 10.5281/zenodo.1\n', CHANGES, 'claims a DOI'),
        ('identifiers', GOOD + 'identifiers:\n  - type: doi\n    value: x\n', CHANGES, 'claims a DOI'),
    ]
    for what, citation, changes, needle in cases:
        problems = world(citation, changes)
        expect('%s: refused (%s)' % (what, problems[0][:90] if problems else 'nothing said'),
               any(needle in p for p in problems))
    expect('no CITATION.cff: refused', any('not at the root' in p for p in world(citation=None)))
    print('')
    if failures:
        print('%d self-test case(s) failed' % len(failures))
        return 1
    print('Every self-test case passed.')
    return 0


def main(argv):
    if argv[1:] == ['--self-test']:
        return self_test()
    if argv[1:]:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    problems = check(ROOT)
    if problems:
        for problem in problems:
            print('FAIL  %s' % problem)
        return 1
    citation = read(ROOT, CITATION)
    print('OK    %s: hMailServer %s of %s, Progressive Robot\'s fork, citing the original'
          % (CITATION, top_level(citation, 'version'), top_level(citation, 'date-released')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
