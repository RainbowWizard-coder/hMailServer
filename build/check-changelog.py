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
  - within a section, each "### " heading once;
  - a release section's "Full notes: [the X release](link)" line, where it has one, is
    there once, is the section's last paragraph - only blank lines and the rule after
    it - and links its own release. release-publish ends the notes with the section's
    text, so a line left in the middle sends the reader to the release page half-way
    through; a fold of parallel work on 1 October 2026 left 6.3.6's mid-section, and it
    was put back by hand in 26311a532;
  - no line of an entry's text that a spaced hyphen begins in the middle of a sentence:
    "- " at the start of a line opens a list there, so that a rewrap that put the hyphen
    of "under the form - the level a lost link has - often below" first on a line broke
    the entry after "has" and made the rest of it a list of one item nested in it (found
    in t34a's CHANGELOG entry for review findings F088 and F089, 10 October 2026, with
    two older entries of the same shape). A line before that ends a sentence or a clause
    (".", ":", ";", "!" or "?", a closing bracket, quote or mark-up after it) is taken to
    begin a list of its own, as entries that list their parts do.

  python build/check-changelog.py [--self-test]
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
RELEASE = re.compile(r'^## \[(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.]+)?\]\(https://gitlab\.com/[^)]+\) - \d{4}-\d{2}-\d{2}( .*)?$')
INLINE = re.compile(r'\S\s+(---\s+##(\s|$)|##\s+\[\d|###\s+[A-Z])')
FULL_NOTES = re.compile(r'^Full notes: \[the [^\]]+ release\]\(https://gitlab\.com/[^)]*/-/releases/v([^)/]+)\)\.\s*$')
# A line of an entry's text that a list marker begins, and the end of a line before it
# that closes a sentence or a clause.
NESTED_ITEM = re.compile(r'^ +[-*+] ')
CLAUSE_END = re.compile(r'[.:;!?][)\]"\'*_`]*$')


def mid_sentence_items(lines):
    """The line numbers of an entry's lines that a list marker begins mid-sentence."""
    out = []
    fenced = False
    for n, line in enumerate(lines, 1):
        if line.lstrip().startswith('```'):
            fenced = not fenced
            continue
        if fenced or n < 2 or not NESTED_ITEM.match(line):
            continue
        before = lines[n - 2].rstrip()
        if not before or before.lstrip().startswith(('|', '#')) or CLAUSE_END.search(before):
            continue
        indent = len(line) - len(line.lstrip(' '))
        indent_before = len(before) - len(before.lstrip(' '))
        # The line before is a deeper item's text, or an item beside this one: this line
        # is the next item of a list. Otherwise it is of the paragraph the line before
        # stops in the middle of.
        if indent_before > indent or (indent_before == indent and NESTED_ITEM.match(before)):
            continue
        out.append(n)
    return out


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
    for n in mid_sentence_items(lines):
        out.append('line %d: a line of an entry begins with "- " in the middle of a sentence, which opens a nested '
                   'list there (a spaced hyphen a rewrap put first on the line?): %s' % (n, lines[n - 1].strip()[:120]))
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
    # The "Full notes" line: once, last, and its own release's.
    for i, (n, h) in enumerate(sections):
        m = RELEASE.match(h)
        if not m:
            continue
        version = '%s.%s.%s%s' % (m.group(1), m.group(2), m.group(3), m.group(4) or '')
        body = range(n, bounds[i + 1] - 1)          # 0-based indexes of the section's lines
        notes = [k for k in body if lines[k].startswith('Full notes:')]
        if not notes:
            continue
        if len(notes) > 1:
            out.append('%s: %d "Full notes" lines (lines %s), where there is one'
                       % (version, len(notes), ', '.join(str(k + 1) for k in notes)))
        last = notes[-1]
        after = [lines[k].strip() for k in range(last + 1, bounds[i + 1] - 1)]
        rest = [x for x in after if x not in ('', '---')]
        if rest:
            out.append('%s: the "Full notes" line (line %d) is not the section\'s last paragraph: "%s" follows it'
                       % (version, last + 1, rest[0][:60]))
        named = FULL_NOTES.match(lines[last])
        if not named:
            out.append('%s: the "Full notes" line (line %d) is not "Full notes: [the X release](https://gitlab.com/.../-/releases/vX)."'
                       % (version, last + 1))
        elif named.group(1) != version:
            out.append('%s: the "Full notes" line (line %d) links the release of v%s'
                       % (version, last + 1, named.group(1)))
    return out


def self_test():
    good = '\n'.join([
        '# Changelog', '', 'Intro.', '', '---', '',
        '## Unreleased - in the tree after 6.3.5, not yet released', '', '### Build and tests', '',
        '- **An entry of parts.** It has three:', '  - the first, which goes on', '    over two lines;',
        '  - the second', '  - and a third - with a spaced', '    hyphen in it.', '',
        '- **An entry.** Text that goes on', '  over two lines.', '', '---', '',
        '## [6.3.5](https://gitlab.com/p/h/-/releases/v6.3.5) - 2026-09-28', '', 'Its introduction.', '',
        '### Operations', '', '- **One.** Text.', '',
        'Full notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).', '', '---', '',
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
        'the "Full notes" line in the middle of its section': good.replace(
            '### Operations\n\n- **One.** Text.\n\nFull notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).\n',
            'Full notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).\n\n### Operations\n\n- **One.** Text.\n'),
        'a "Full notes" line twice in a section': good.replace(
            'Its introduction.', 'Its introduction.\n\nFull notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).'),
        'a "Full notes" line that links another release': good.replace(
            'Full notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).',
            'Full notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.4).'),
        'a spaced hyphen first on a line of an entry': good.replace(
            '  over two lines.', '  over two lines - the level a lost link\n  - has - and more.'),
        'a spaced hyphen first on the line after an entry\'s first': good.replace(
            'Text that goes on\n  over two lines.', 'Text that goes on\n  - over two lines.'),
        'a spaced hyphen first on a line of a list\'s item': good.replace(
            '  - the first, which goes on\n    over two lines;', '  - the first, which goes on\n    - over two lines;'),
        'a "Full notes" line not in its form': good.replace(
            'Full notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).', 'Full notes: on the release page.'),
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
