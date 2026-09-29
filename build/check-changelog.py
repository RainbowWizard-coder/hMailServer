#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""CHANGELOG.md keeps its shape: one Unreleased section, then every release's section,
newest first, each behind its own rule and heading.

release-publish makes a release's notes from its version's section as the tag's tree
has it, and refuses a version without one (RELEASE.md, "The release on GitLab"). On
29 September 2026 a rewrap of the Unreleased section's last entry ran on past the
section's end and made a rule, 6.3.5's heading, its introduction and two of its
subheadings one paragraph of that entry; the tree had no "## [6.3.5]" heading for
eleven commits and no check noticed. This one holds:

  - the headings "## Unreleased - ..." once, then "## [X.Y.Z...](link) - YYYY-MM-DD ...",
    each at the start of a line, the Unreleased heading and the release after it each
    after a line "---" and a blank line;
  - the releases' versions in descending order, each once;
  - no rule, "## " or "### " heading written inside a line of text - what a rewrap
    that ran too far leaves;
  - within a section, each "### " heading once.

  python build/check-changelog.py [--self-test]
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
RELEASE = re.compile(r'^## \[(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.]+)?\]\(https://gitlab\.com/[^)]+\) - \d{4}-\d{2}-\d{2}( .*)?$')
INLINE = re.compile(r'\S\s+(---\s+##(\s|$)|##\s+\[\d|###\s+[A-Z])')


def problems_of(text):
    out = []
    lines = text.replace('\r\n', '\n').split('\n')
    sections = []                  # (line number, heading)
    for n, line in enumerate(lines, 1):
        if line.startswith('## Unreleased') or RELEASE.match(line) or line.startswith('## ['):
            sections.append((n, line))
        if INLINE.search(line) and not line.lstrip().startswith(('|', '`')):
            out.append('line %d: a rule or a heading inside a line of text (a rewrap that ran on too far?): %s'
                       % (n, line.strip()[:120]))
    unreleased = [n for n, h in sections if h.startswith('## Unreleased')]
    if len(unreleased) != 1:
        out.append('%d "## Unreleased" headings, where there must be one' % len(unreleased))
    releases = [(n, h) for n, h in sections if not h.startswith('## Unreleased')]
    if unreleased and releases and releases[0][0] < unreleased[0]:
        out.append('line %d: a release section before the Unreleased section' % releases[0][0])
    previous = None
    seen = set()
    for n, h in releases:
        m = RELEASE.match(h)
        if not m:
            out.append('line %d: a release heading not of the form "## [X.Y.Z](link) - YYYY-MM-DD": %s' % (n, h[:100]))
            continue
        # Newest first by the date each heading gives: the file's order is the order
        # the releases were published in, which for the 6.0.0 betas is not the order
        # of their numbers.
        version = '%s.%s.%s%s' % (m.group(1), m.group(2), m.group(3), m.group(4) or '')
        date = re.search(r'\) - (\d{4}-\d{2}-\d{2})', h).group(1)
        if version in seen:
            out.append('line %d: %s has a second section' % (n, version))
        seen.add(version)
        if previous is not None and date > previous[0]:
            out.append('line %d: %s (%s) after %s (%s) - the sections go newest first'
                       % (n, version, date, previous[1], previous[0]))
        previous = (date, version)
    # The Unreleased section and the release after it each stand behind a rule: the
    # boundary a rewrap that ran on too far loses. (The oldest sections, from before
    # the rule was kept, have none.)
    for n, h in sections[:2]:
        if n < 3 or lines[n - 2].strip() != '' or lines[n - 3].strip() != '---':
            out.append('line %d: "%s" is not after a line "---" and a blank line' % (n, h[:60]))
    # Each "### " heading once within its section.
    bounds = [n for n, _ in sections] + [len(lines) + 1]
    for i, (n, h) in enumerate(sections):
        heads = {}
        for k in range(n, bounds[i + 1] - 1):
            if lines[k].startswith('### '):
                heads.setdefault(lines[k], []).append(k + 1)
        for head, at in heads.items():
            if len(at) > 1:
                out.append('%s: "%s" %d times (lines %s)' % (h[:40], head, len(at), ', '.join(map(str, at))))
    return out


def self_test():
    good = '\n'.join([
        '# Changelog', '', 'Intro.', '', '---', '',
        '## Unreleased - in the tree after 6.3.5, not yet released', '', '### Build and tests', '',
        '- **An entry.** Text that goes on', '  over two lines.', '', '---', '',
        '## [6.3.5](https://gitlab.com/p/h/-/releases/v6.3.5) - 2026-09-28', '', 'Its introduction.', '',
        '### Operations', '', '- **One.** Text.', '', '---', '',
        '## [6.3.0](https://gitlab.com/p/h/-/releases/v6.3.0) - 2026-09-10', '', '- **Two.** Text.', '', '---', '',
        '## [6.3.0-alpha1](https://gitlab.com/p/h/-/releases/v6.3.0-alpha1) - 2026-09-01', '', 'Text.', ''])
    failures = 0
    if problems_of(good):
        print('FAIL: a good file is refused:', problems_of(good))
        failures += 1
    cases = {
        'a rewrap that swallowed the rule and the heading': good.replace(
            '  over two lines.\n\n---\n\n## [6.3.5]', '  over two lines.  ---  ## [6.3.5]'),
        'the same, wrapped after the rule and its hashes': good.replace(
            '  over two lines.\n\n---\n\n## [6.3.5]', '  over two lines.  ---  ##\n  [6.3.5]'),
        'a subheading inside a line': good.replace('Its introduction.\n\n### Operations', 'Its introduction. ### Operations'),
        'two Unreleased sections': good.replace('## [6.3.0](', '## Unreleased - again\n\n---\n\n## [6.3.0]('),
        'an older release first': good.replace('v6.3.5) - 2026-09-28', 'v6.3.5) - 2026-09-08'),
        'a version twice': good.replace('## [6.3.0](https://gitlab.com/p/h/-/releases/v6.3.0)',
                                        '## [6.3.5](https://gitlab.com/p/h/-/releases/v6.3.5)'),
        'the Unreleased section with no rule before it': good.replace(
            'Intro.\n\n---\n\n## Unreleased', 'Intro.\n\n## Unreleased'),
        'the newest release with no rule before it': good.replace('---\n\n## [6.3.5]', '\n## [6.3.5]'),
        'a heading of the wrong form': good.replace('- 2026-09-10', '(10 September)'),
        'a subheading twice in a section': good.replace('- **An entry.**', '### Build and tests\n\n- **An entry.**'),
    }
    for name, text in cases.items():
        if not problems_of(text):
            print('FAIL: not refused: %s' % name)
            failures += 1
    print('self-test: %d failure(s)' % failures)
    return 1 if failures else 0


def main():
    if '--self-test' in sys.argv[1:]:
        return self_test()
    path = os.path.join(ROOT, 'CHANGELOG.md')
    with open(path, encoding='utf-8') as f:
        found = problems_of(f.read())
    for p in found:
        print('FAIL  CHANGELOG.md ' + p)
    if found:
        return 1
    print('OK    CHANGELOG.md: one Unreleased section, then each release newest first, each behind its rule')
    return 0


if __name__ == '__main__':
    sys.exit(main())
