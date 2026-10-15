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
    begin a list of its own, as entries that list their parts do;
  - each number of .gitlab/security-exceptions.yml's entries that the Unreleased section
    gives to the day it was counted. The section is written when a batch lands, and the
    file changes between landings, each entry in a change of its own: the section's
    entry on SECURITY.md's policy, written on 6 October 2026, said that the file's
    header "counts its 500 entries", while four were added on 13 October and thirteen
    taken out on 14 and 15 October, leaving 491 on 15 October 2026, and no check
    noticed. A clause - up to a ";" or the end of a sentence - that names the file, or
    "the exceptions file", and gives in figures a number of entries, exceptions or
    accepted findings names a day as well ("500 on 6 October 2026"), with no more than
    eight words between the two. In an entry that names the file - a paragraph, or a
    list item with what is indented under it - "the file", "its header" and "the
    header" name it too, and a figure that the header counts or the file holds is a
    count: the entry that brought this rule in said "leaving the 491 the header counts
    now", in a clause that named the file as "the file's header" and held the day the
    four were added, 13 October 2026, eleven words before the 491, and the rule as
    first written passed it. A release's section is history and is not read.

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
# A count of .gitlab/security-exceptions.yml's entries: a clause that names the file - not
# where its name ends another, as in the artifact master-security-exceptions.yml - or, in
# an entry that names it, one that calls it "the file", "its header" or "the header"; a
# number of entries in figures, or a figure that the header counts or the file holds ("the
# 491 the header counts"); and a day no more than NEAR words from the count, in its clause.
EXCEPTIONS_NAMED = re.compile(r'(?<![\w./-])(?:\.gitlab/)?security-exceptions\.yml(?![\w-])|\bexceptions file\b')
EXCEPTIONS_MENTIONED = re.compile(r'\b(?:the file|its header|the header)\b', re.IGNORECASE)
ENTRY_COUNT = re.compile(r'(?<![\w.,])\d[\d,]*(?: (?:entries|exceptions|accepted findings)\b'
                         r'|(?= (?:that |which )?(?:the header counts|the file holds)\b))')
DAY = re.compile(r'\b\d{1,2} (?:January|February|March|April|May|June|July|August|September|October|November|'
                 r'December) \d{4}\b')
NEAR = 8
# Where a clause ends: a ";" or the end of a sentence, a closing bracket, quote or mark-up
# after it, then a space.
CLAUSE_BREAK = re.compile(r'(?<=[.;!?])[)\]"\'*_`]*\s+')


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


def items_of(lines, first, last):
    """The paragraphs and list items of lines[first:last] (indexes from 0), each a list of
    (line number, the line's text): one ends at a blank line, a fence, a heading, a row of
    a table or a rule, and where a list marker begins a line, which begins the next."""
    items, current, fenced = [], [], False
    for k in range(first, last):
        line = lines[k]
        stripped = line.strip()
        fence = stripped.startswith('```')
        if fence:
            fenced = not fenced
        if fence or fenced or not stripped or stripped.startswith(('#', '|')) or stripped == '---':
            if current:
                items.append(current)
            current = []
            continue
        if NESTED_ITEM.match(line) or line.startswith(('- ', '* ', '+ ')):
            if current:
                items.append(current)
            current = []
        current.append((k + 1, stripped))
    if current:
        items.append(current)
    return items


def words_in(text):
    """How many words text holds, its marks and mark-up left out."""
    return sum(1 for word in text.split() if re.search(r'\w', word))


def undated_exception_counts(lines, first, last):
    """(line number, the count, the words around it) for each number of
    .gitlab/security-exceptions.yml's entries that lines[first:last] give in figures with
    no day near it: a count the file's next entry, or the next taken out, makes untrue."""
    entries = []                   # each a list of (an item's lines joined, [(offset, line number)])
    for item in items_of(lines, first, last):
        text, starts = '', []
        for number, part in item:
            starts.append((len(text), number))
            text += part + ' '
        # An entry is a paragraph, or a list item at the start of a line with the items and
        # paragraphs indented under it: "the file" in any part of it is the file it names.
        if not entries or not lines[item[0][0] - 1].startswith(' '):
            entries.append([])
        entries[-1].append((text, starts))
    out = []
    for entry in entries:
        named = any(EXCEPTIONS_NAMED.search(text) for text, _ in entry)
        for text, starts in entry:
            ends = [0] + [m.end() for m in CLAUSE_BREAK.finditer(text)] + [len(text)]
            for begin, end in zip(ends, ends[1:]):
                clause = text[begin:end]
                if not (EXCEPTIONS_NAMED.search(clause) or (named and EXCEPTIONS_MENTIONED.search(clause))):
                    continue
                days = list(DAY.finditer(clause))
                for m in ENTRY_COUNT.finditer(clause):
                    # The words between the count and each day of its clause, before it or after it.
                    if any(words_in(clause[d.end():m.start()] if d.end() <= m.start() else clause[m.end():d.start()])
                           <= NEAR for d in days):
                        continue
                    at = begin + m.start()
                    # The clause's text from NEAR words before the count to NEAR words after it.
                    start = ([w.start() for w in re.finditer(r'\S+', clause[:m.start()])][-NEAR:] or [m.start()])[0]
                    stop = m.end() + ([w.end() for w in re.finditer(r'\S+', clause[m.end():])][:NEAR] or [0])[-1]
                    out.append((max(n for s, n in starts if s <= at), m.group(0), clause[start:stop]))
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
    # A count of the exceptions file's entries in the Unreleased section, with its day.
    for i, (n, h) in enumerate(sections):
        if not h.startswith('## Unreleased'):
            continue
        for number, count, around in undated_exception_counts(lines, n, bounds[i + 1] - 1):
            out.append('line %d: the Unreleased section gives "%s" of .gitlab/security-exceptions.yml with no day within '
                       '%d words of it: the file changes between landings, each entry in a change of its own, and the '
                       'section only when a batch lands, so give the day it was counted beside it ("500 on 6 October '
                       '2026"): %s' % (number, count, NEAR, around[:160]))
    return out


def self_test():
    good = '\n'.join([
        '# Changelog', '', 'Intro.', '', '---', '',
        '## Unreleased - in the tree after 6.3.5, not yet released', '', '### Build and tests', '',
        '- **An entry of parts.** It has three:', '  - the first, which goes on', '    over two lines;',
        '  - the second', '  - and a third - with a spaced', '    hyphen in it.', '',
        '- **The exceptions.** `.gitlab/security-exceptions.yml`\'s header counts its entries by scan and by group (500',
        '  on 6 October 2026). On 13 October 2026 the exceptions file held 504 entries; the artifact',
        '  `master-security-exceptions.yml` holds 3 entries.', '',
        # A day eight words from its count, before it and after it, a mark between them no word.
        '- **The exceptions, counted.** On 14 October 2026 the exceptions file - read by its parser - held 501 entries. The',
        '  exceptions file held 491 entries, as its parser read and counted them on 15 October 2026.', '',
        # The file named in the first sentence of an entry alone, which then calls it "the file" and gives its
        # count with the day, and quotes another beside its own; and after it, an entry of another file.
        '- **A count of `.gitlab/security-exceptions.yml`\'s entries has its day.** The file changes between landings: an',
        '  entry, written on 6 October 2026, said that the file\'s header "counts its 500 entries", while four were added',
        '  on 13 October 2026 and thirteen taken out on 14 and 15 October, leaving 491 on 15 October 2026; the entry gives',
        '  the 500 as 6 October\'s now.',
        '- **Another file.** The file of this entry holds 7 entries, the 7 the header counts.', '',
        # An entry whose indented parts name the file through its first line, which holds a figure
        # inside a name: no count.
        '- **The exceptions\' parts.** `.gitlab/security-exceptions.yml` has two, the batch48 entries among them:',
        '  - its groups, each with a comment;', '  - its header, which counts the entries by scan.', '',
        # A paragraph at the start of a line is an entry of its own, though the entry before it names the file.
        'A paragraph of its own: the file it is about holds 7 entries.', '',
        '- **An entry.** Text that goes on', '  over two lines.', '', '---', '',
        '## [6.3.5](https://gitlab.com/p/h/-/releases/v6.3.5) - 2026-09-28', '', 'Its introduction.', '',
        '### Operations', '', '- **One.** Text.', '',
        'Full notes: [the 6.3.5 release](https://gitlab.com/p/h/-/releases/v6.3.5).', '', '---', '',
        '## [6.3.0](https://gitlab.com/p/h/-/releases/v6.3.0) - 2026-09-10', '',
        '- **Two.** Text; `.gitlab/security-exceptions.yml` holds 469 entries.', '', '---', '',
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
    # Refused by the count's own rule, which another rule refusing them would not prove.
    counts = {
        'a count of the exceptions file\'s entries with no day': good.replace(
            'counts its entries by scan and by group (500\n  on 6 October 2026)', 'counts its 500 entries by scan and\n  by group'),
        'the same, the number and its noun on two lines': good.replace(
            'counts its entries by scan and by group (500\n  on 6 October 2026)', 'counts its 500\n  entries by scan and by group'),
        'the same, its accepted findings counted, the noun on two lines': good.replace(
            'counts its entries by scan and by group (500\n  on 6 October 2026)', 'counts its 469 accepted\n  findings by scan and by group'),
        'the same, the file named without its folder, its exceptions counted': good.replace(
            '`.gitlab/security-exceptions.yml`\'s header counts its entries by scan and by group (500\n  on 6 October 2026)',
            '`security-exceptions.yml`\'s header counts its 491 exceptions'),
        'the same of "the exceptions file", the day in the next clause': good.replace(
            'On 13 October 2026 the exceptions file held 504 entries;', 'The exceptions file holds 504 entries; on 13 October 2026'),
        # The words this rule's entry first had, which the rule as first written passed.
        'a figure the header counts, the file called "the file", a day of another fact eleven words before it':
            good.replace('leaving 491 on 15 October 2026;', 'leaving the 491 the header counts now;'),
        'the same, a figure that the file holds': good.replace(
            'leaving 491 on 15 October 2026;', 'leaving the 491 that the file holds now;'),
        'the same, a figure which the header counts': good.replace(
            'leaving 491 on 15 October 2026;', 'leaving the 491 which the header counts now;'),
        '"the file" in an entry that names the file': good.replace(
            '`master-security-exceptions.yml` holds 3 entries.',
            '`master-security-exceptions.yml` holds 3 entries. The file holds 491 entries.'),
        '"its header" in an entry that names the file': good.replace(
            '`master-security-exceptions.yml` holds 3 entries.',
            '`master-security-exceptions.yml` holds 3 entries. Its header counts 491 entries.'),
        '"the header" in an entry that names the file': good.replace(
            '`master-security-exceptions.yml` holds 3 entries.',
            '`master-security-exceptions.yml` holds 3 entries; the header counts 491 entries.'),
        'in a part of an entry whose first line names the file': good.replace(
            '  - its header, which counts the entries by scan.', '  - its header, which counts 491 entries by scan.'),
        'an entry whose indented part alone names the file, its first giving the count': good.replace(
            '- **An entry of parts.** It has three:', '- **An entry of parts.** The file holds 491 entries, in three:').replace(
            '  - the second\n', '  - the second, of `.gitlab/security-exceptions.yml`\n'),
        'a paragraph indented under an entry whose first line names the file': good.replace(
            '  - its header, which counts the entries by scan.\n\n',
            '  - its header, which counts the entries by scan.\n\n  Its second paragraph: the file holds 491 entries.\n\n'),
        'a day nine words before its count': good.replace('read by its parser - held 501', 'read by its own parser - held 501'),
        'a day nine words after its count': good.replace('as its parser read and counted', 'as its own parser read and counted'),
        'a count in the bold first sentence of an entry, its day in the next': good.replace(
            '- **The exceptions, counted.** On 14 October', '- **The exceptions file holds 501 entries.** On 14 October'),
    }
    cases.update(counts)
    for name, text in cases.items():
        found = problems_of(text)
        if not found:
            print('FAIL: not refused: %s' % name)
            failures += 1
        elif name in counts:
            mine = [p for p in found if 'of .gitlab/security-exceptions.yml with no day' in p]
            if not mine:
                print('FAIL: refused, but not for its count: %s: %s' % (name, found))
                failures += 1
            # Each at the line its count's figure stands on.
            for p in mine:
                at = re.match(r'line (\d+): the Unreleased section gives "([\d,]+)', p)
                line = text.split('\n')[int(at.group(1)) - 1] if at else ''
                if not at or not re.search(r'(?<![\w.,])%s(?![\d,])' % re.escape(at.group(2)), line):
                    print('FAIL: refused at a line that does not hold its count: %s: %s' % (name, p[:90]))
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
