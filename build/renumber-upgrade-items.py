#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Renumber one CHANGELOG.md section's "What changes on upgrade" list 1..n, and every
reference to its items inside that section.

A release section's "### What changes on upgrade without a switch" is a numbered list,
and the section's own text points into it - "(What changes on upgrade, item 21)",
"(item 22)", "items 31 and 32", "items 21 to 23" - wrapped wherever its line ended.
Parallel branches each add items at the end of the list, so a fold of two of them
leaves two items with one number: on 2 October 2026 6.3.6's list had two items 14, and
a renumbering by hand has to find every reference in a section of three thousand lines.
This does it:

  - the section's list is renumbered 1..n in document order. An item whose number gains
    or loses a digit takes its indented lines with it, so a list nested in it stays
    nested.
  - every reference to an item inside the section, and only inside it, is rewritten to
    that item's new number. A reference is "item N", "items N and M", "items N, M and
    K", "items N to M" or "items N-M", across a line break, outside a code span. "work
    item N" (GitLab's) is not one; nor is one that names another release just before it
    ("6.3.5's upgrade item 3", "(6.3.4, What changes on upgrade, item 7)"), which is
    left as it is and said. A line outside the section that names this release and an
    item number is said too, and left.
  - a reference to a number no item has is refused, naming its line.
  - a reference to a number two items have is taken to the one whose heading (the
    item's bold first words) the reference's own text names: the clause around the
    reference, then its sentence, holds a word of that item's heading that no other
    such item's heading has, and no other's heading has such a word there. The tool
    says which it took and the words that decided. Otherwise it is refused, and the
    tool asks: run it again with --resolve LINE:OLD=NEW, NEW the number the item meant
    is given, which it lists.

Nothing is written when anything is refused. Line numbers are the file's as read. The
file's line endings are kept.

  python build/renumber-upgrade-items.py [--section VERSION|unreleased] [--file PATH]
                                         [--check] [--verbose] [--resolve LINE:OLD=NEW ...]
  python build/renumber-upgrade-items.py --self-test

The section is, by default, the first from the top that has the list: the Unreleased
section once it has one, and until then the newest release's. --check writes nothing
and exits 1 when anything would change; --verbose lists every item and every reference,
changed or not. Exit 2: refused.
"""
import argparse
import bisect
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

SECTION_HEAD = re.compile(r'^## (?:(Unreleased)\b|\[([^\]\s]+)\]\()', re.M)
ANY_H2 = re.compile(r'^## ', re.M)
LIST_HEAD = re.compile(r'^### What changes on upgrade\b[^\n]*', re.M)
LIST_END = re.compile(r'^(?:#{1,6} |---[ \t]*$)', re.M)
ITEM = re.compile(r'^(\d+)\. ', re.M)
# "item N" / "items N": not inside a word, a path or an identifier (`work_items/49`,
# `Recipients.Item`), and N not the start of a version or a longer token.
REF = re.compile(r'(?<![\w.#/\\-])([Ii]tem(s?))\s+(\d+)(?![\d.,]\d|\w)')
# After a plural: ", M", ", and M", " and M", " or M", " to M", "-M", "–M".
MORE = re.compile(r'(\s*,\s*(?:and\s+|or\s+)?|\s+(?:and|or)\s+|\s+(to)\s+|\s*([-–])\s*)(\d+)(?![\d.,]\d|\w)')
# The words before "item N" that make it something else's item.
NOT_OURS = {'work', 'roadmap', 'menu'}
# A release named just before the reference: the reference is to that release's list.
OTHER_RELEASE = re.compile(r"(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)(?:'s)?[\s,(]+"
                           r"(?:What\s+changes\s+on\s+upgrade(?:\s+without\s+a\s+switch)?[\s,]*|upgrade\s+|list[\s,]+)?$")
QUALIFIER = re.compile(r'What\s+changes\s+on\s+upgrade(?:\s+without\s+a\s+switch)?')
PARAGRAPH_BREAK = re.compile(r'\n[ \t]*\n|\n[ \t]*(?:[-*+]|\d+\.)[ \t]')
SENTENCE_END = re.compile(r'[.!?][)\]"\'*_`]*\s+(?=[A-Z(\[`*"])')
CLAUSE_END = re.compile(r'[.!?][)\]"\'*_`]*\s+(?=[A-Z(\[`*"])|[;:]\s')
WORD = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+/-]*[A-Za-z0-9]|[A-Za-z0-9]")
STOP = set('''a an the of in on at to for by with from and or nor but is are was were be been its it as
that this these those which what who whom whose not no now more most less than so may can could each every
any all one two three only also where when does do has have had their there they them his her he she
says said say upgrade upgrades changes change item items switch without new old into out up down over under
after before until while about once still just same other others own such very much many some few after
how why yes too'''.split())


def stem(word):
    word = word.lower()
    if word.endswith("'s"):
        word = word[:-2]
    if len(word) > 3 and word.endswith('s') and not word.endswith('ss') and not any(c.isdigit() for c in word):
        word = word[:-1]
    return word


def words_of(text):
    """The text's words that can name a subject, as {stem: the word as written}."""
    out = {}
    for w in WORD.findall(text):
        s = stem(w)
        if len(s) < 3 or s in STOP:
            continue
        out.setdefault(s, w)
    return out


def mask_code_spans(text):
    """The text with every code span's characters but its line breaks made NUL, so a
    reference inside one is not found; a paragraph whose backticks do not pair is left
    as it is, so a stray backtick hides nothing."""
    out = list(text)
    for block in re.finditer(r'(?:[^\n]|\n(?![ \t]*\n))+', text):
        runs = [(m.start(), m.end()) for m in re.finditer(r'`+', block.group(0))]
        i = 0
        while i < len(runs):
            s, e = runs[i]
            width = e - s
            j = i + 1
            while j < len(runs) and runs[j][1] - runs[j][0] != width:
                j += 1
            if j == len(runs):
                break
            for k in range(block.start() + s, block.start() + runs[j][1]):
                if out[k] != '\n':
                    out[k] = '\0'
            i = j + 1
    return ''.join(out)


class Item(object):
    def __init__(self, old, start, end, line, title):
        self.old, self.start, self.end, self.line, self.title = old, start, end, line, title
        self.new = None


class Result(object):
    def __init__(self):
        self.text = None
        self.section = None
        self.list_line = None
        self.list_head = None
        self.items = []        # Item
        self.refs = []         # (line, old text, new text, [target items]) for each reference found
        self.decisions = []    # str: which of two items a reference was taken to, and why
        self.notes = []        # str: left alone, and why
        self.errors = []       # str: refused

    @property
    def changed(self):
        return self.text is not None and self.original != self.text


def find_section(text, section):
    heads = []
    for m in SECTION_HEAD.finditer(text):
        name = 'unreleased' if m.group(1) else m.group(2)
        nxt = ANY_H2.search(text, m.end())
        heads.append((name, m.start(), nxt.start() if nxt else len(text)))
    if section is None:
        for name, s, e in heads:
            if LIST_HEAD.search(text, s, e):
                return name, s, e
        return None
    want = section.lower()
    for name, s, e in heads:
        if name.lower() == want:
            return name, s, e
    return None


def title_of(text, item_start, item_end):
    first = text[item_start:item_end]
    first = re.sub(r'^\d+\.\s*', '', first)
    para = re.split(r'\n[ \t]*\n', first, maxsplit=1)[0]
    bold = re.match(r'\s*\*\*(.+?)\*\*', para, re.S)
    title = bold.group(1) if bold else para[:100]
    return re.sub(r'\s+', ' ', title).strip()


def context(masked, lo, hi, s, e, boundary):
    """The text around a reference at [s, e), within its paragraph, back to and on to
    the nearest boundary; the reference itself and the list's name taken out."""
    pstart = lo
    for m in PARAGRAPH_BREAK.finditer(masked, lo, s):
        pstart = m.end()
    m = PARAGRAPH_BREAK.search(masked, e, hi)
    pend = m.start() if m else hi
    cstart = pstart
    for m in boundary.finditer(masked, pstart, s):
        cstart = m.end()
    m = boundary.search(masked, e, pend)
    cend = m.start() + 1 if m else pend
    around = masked[cstart:s] + ' ' + masked[e:cend]
    return QUALIFIER.sub(' ', around.replace('\0', ' '))


def renumber(text, section=None, resolutions=None):
    """Renumber text (with \\n line ends) as the module says. resolutions maps
    (line, old) to the new number of the item meant."""
    resolutions = dict(resolutions or {})
    used = set()
    r = Result()
    r.original = text
    starts = [0] + [i + 1 for i, c in enumerate(text) if c == '\n']

    def line_of(off):
        return bisect.bisect_right(starts, off)

    found = find_section(text, section)
    if not found:
        r.errors.append('no section %s' % ('"%s"' % section if section else 'has a "### What changes on upgrade" list'))
        return r
    name, lo, hi = found
    r.section = name
    heads = list(LIST_HEAD.finditer(text, lo, hi))
    if len(heads) != 1:
        r.errors.append('the %s section has %d "### What changes on upgrade" lists, where this renumbers one%s'
                        % (name, len(heads), ' (lines %s)' % ', '.join(str(line_of(h.start())) for h in heads) if heads else ''))
        return r
    head = heads[0]
    r.list_line, r.list_head = line_of(head.start()), head.group(0)
    end = LIST_END.search(text, head.end() + 1, hi)
    list_end = end.start() if end else hi
    marks = list(ITEM.finditer(text, head.end(), list_end))
    if not marks:
        r.errors.append('line %d: the list "%s" has no numbered item' % (r.list_line, head.group(0)))
        return r
    for i, m in enumerate(marks):
        e = marks[i + 1].start() if i + 1 < len(marks) else list_end
        it = Item(int(m.group(1)), m.start(), e, line_of(m.start()), title_of(text, m.start(), e))
        it.new = i + 1
        r.items.append(it)
    by_old = {}
    for it in r.items:
        by_old.setdefault(it.old, []).append(it)

    edits = []          # (start, end, replacement)
    masked = mask_code_spans(text)

    def resolve(num, line, s, e):
        cands = by_old.get(num, [])
        if not cands:
            r.errors.append('line %d: "item %d" - no item of the list is numbered %d (they run %s)'
                            % (line, num, num, describe_numbers()))
            return None
        if len(cands) == 1:
            return cands[0]
        where = ' and '.join('line %d "%s"' % (c.line, short(c.title)) for c in cands)
        if (line, num) in resolutions:
            used.add((line, num))
            want = resolutions[(line, num)]
            for c in cands:
                if c.new == want:
                    r.decisions.append('line %d: %d items are numbered %d (%s); took the one at line %d, now %d, '
                                       'as --resolve %d:%d=%d says' % (line, len(cands), num, where, c.line, c.new, line, num, want))
                    return c
            r.errors.append('line %d: --resolve %d:%d=%d names none of the items numbered %d; they become %s'
                            % (line, line, num, want, num, choices(cands)))
            return None
        for level, boundary in (('clause', CLAUSE_END), ('sentence', SENTENCE_END)):
            said = words_of(context(masked, lo, hi, s, e, boundary))
            hits = []
            for c in cands:
                others = set()
                for o in cands:
                    if o is not c:
                        others |= set(words_of(o.title))
                own = (set(words_of(c.title)) - others) & set(said)
                if own:
                    hits.append((c, own))
            if len(hits) == 1:
                c, own = hits[0]
                r.decisions.append('line %d: %d items are numbered %d (%s); took the one at line %d, now %d, because '
                                   'the reference\'s %s names %s, which that item\'s heading holds and the other%s not'
                                   % (line, len(cands), num, where, c.line, c.new, level,
                                      ', '.join('"%s"' % said[w] for w in sorted(own)),
                                      's\' do' if len(cands) > 2 else '\'s does'))
                return c
            if len(hits) > 1:
                break
        r.errors.append('line %d: "item %d" is ambiguous - %d items are numbered %d (%s) and the reference\'s text '
                        'names neither\'s subject over the other\'s. Which is meant? Run again with --resolve %d:%d=NEW, '
                        'NEW being %s' % (line, num, len(cands), num, where, line, num, choices(cands)))
        return None

    def describe_numbers():
        nums = sorted(by_old)
        return '%d to %d' % (nums[0], nums[-1]) if nums == list(range(nums[0], nums[-1] + 1)) else ', '.join(map(str, nums))

    def choices(cands):
        return ' or '.join('%d for the one at line %d' % (c.new, c.line) for c in cands)

    for m in REF.finditer(masked, lo, hi):
        before = masked[max(lo, m.start() - 40):m.start()]
        w = re.search(r'([A-Za-z]+)[\s\[(]*$', before)
        if w and w.group(1).lower() in NOT_OURS:
            continue
        nums = [(m.start(3), m.end(3), None)]
        pos = m.end()
        if m.group(2):
            while True:
                more = MORE.match(masked, pos)
                if not more or more.end() > hi:
                    break
                nums.append((more.start(4), more.end(4), 'range' if (more.group(2) or more.group(3)) else None))
                pos = more.end()
        s, e = m.start(), pos
        line = line_of(m.start(3))
        other = OTHER_RELEASE.search(masked[max(lo, s - 120):s])
        if other and other.group(1) != name:
            r.notes.append('line %d: "%s" names %s\'s list, not this one; left as it is'
                           % (line, re.sub(r'\s+', ' ', text[s:e]), other.group(1)))
            continue
        targets = []
        for a, b, kind in nums:
            num = int(text[a:b])
            it = resolve(num, line_of(a), s, e)
            targets.append(it)
            if it is not None and it.new != num:
                edits.append((a, b, str(it.new)))
        for k, (a, b, kind) in enumerate(nums):
            if kind == 'range' and targets[k] is not None and targets[k - 1] is not None and targets[k].new < targets[k - 1].new:
                r.errors.append('line %d: "%s" runs from item %d (line %d) back to item %d (line %d)'
                                % (line, re.sub(r'\s+', ' ', text[s:e]), targets[k - 1].new, targets[k - 1].line,
                                   targets[k].new, targets[k].line))
        old_text = re.sub(r'\s+', ' ', text[s:e])
        new_text = old_text
        if None not in targets:
            pieces, at = [], s
            for (a, b, kind), it in zip(nums, targets):
                pieces.append(text[at:a])
                pieces.append(str(it.new))
                at = b
            pieces.append(text[at:e])
            new_text = re.sub(r'\s+', ' ', ''.join(pieces))
        r.refs.append((line, old_text, new_text, targets))

    for key in sorted(set(resolutions) - used):
        r.errors.append('line %d: --resolve %d:%d=%d names no reference to two items numbered %d on that line'
                        % (key[0], key[0], key[1], resolutions[key], key[1]))

    # Outside the section: a line that names this release and an item number.
    if name != 'unreleased':
        near = re.compile(re.escape(name) + r"(?:'s)?\b(?:[^\n]|\n(?![ \t]*\n)){0,100}?(?<![\w.#/\\-])items?\s+\d+")
        for span in ((0, lo), (hi, len(text))):
            for m in near.finditer(masked, *span):
                if 'work item' in m.group(0).lower():
                    continue
                r.notes.append('line %d, outside the section: "%s" may name an item of %s\'s list; not rewritten - '
                               'read it' % (line_of(m.start()), re.sub(r'\s+', ' ', text[m.start():m.end()]), name))

    # The markers, and the lines of an item whose marker changes width.
    for it in r.items:
        if it.new == it.old:
            continue
        a = it.start
        edits.append((a, a + len(str(it.old)), str(it.new)))
        delta = len(str(it.new)) - len(str(it.old))
        if not delta:
            continue
        width = len(str(it.old)) + 2
        k = text.find('\n', it.start, it.end)
        while k != -1 and k + 1 < it.end:
            ls = k + 1
            nxt = text.find('\n', ls, it.end)
            line_text = text[ls:nxt if nxt != -1 else it.end]
            indent = len(line_text) - len(line_text.lstrip(' '))
            if line_text.strip() and indent >= width:
                if delta > 0:
                    edits.append((ls, ls, ' ' * delta))
                else:
                    edits.append((ls, ls - delta, ''))
            k = nxt

    if r.errors:
        return r
    out = text
    for a, b, rep in sorted(edits, key=lambda x: (x[0], x[1]), reverse=True):
        out = out[:a] + rep + out[b:]
    r.text = out
    return r


def short(title, n=60):
    return title if len(title) <= n else title[:n - 3].rstrip() + '...'


def report(r, path, verbose, out):
    if r.section is None or r.list_line is None:
        return
    nums = [it.old for it in r.items]
    print('%s, the %s section\'s "%s" (line %d): %d items, numbered as read %s'
          % (path, r.section, r.list_head[4:], r.list_line, len(r.items),
             ', '.join(map(str, nums)) if nums != list(range(1, len(nums) + 1)) else '1 to %d' % len(nums)), file=out)
    for it in r.items:
        if verbose or it.new != it.old:
            print('  item at line %d: %d -> %d%s  %s' % (it.line, it.old, it.new, '' if it.new != it.old else ' (as it was)',
                                                       short(it.title, 90)), file=out)
    for line, old, new, targets in r.refs:
        if verbose or old != new:
            target = ', '.join('line %d' % t.line for t in targets if t is not None)
            print('  reference at line %d: "%s" -> "%s"%s (the item at %s)'
                  % (line, old, new, '' if old != new else ' (as it was)', target or '?'), file=out)
    for d in r.decisions:
        print('  ' + d, file=out)
    for n in r.notes:
        print('  note: ' + n, file=out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--file', default=os.path.join(ROOT, 'CHANGELOG.md'))
    ap.add_argument('--section')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--resolve', action='append', default=[], metavar='LINE:OLD=NEW')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    resolutions = {}
    for spec in a.resolve:
        m = re.match(r'^(\d+):(\d+)=(\d+)$', spec)
        if not m:
            print('REFUSED --resolve %s: not LINE:OLD=NEW' % spec)
            return 2
        resolutions[(int(m.group(1)), int(m.group(2)))] = int(m.group(3))
    with open(a.file, 'rb') as f:
        raw = f.read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    crlf = '\r\n' in text
    text = text.replace('\r\n', '\n')
    r = renumber(text, a.section, resolutions)
    shown = os.path.relpath(a.file, ROOT) if os.path.abspath(a.file).startswith(ROOT) else a.file
    report(r, shown, a.verbose, sys.stdout)
    if r.errors:
        for e in r.errors:
            print('REFUSED %s %s' % (shown, e))
        print('Nothing written.')
        return 2
    if not r.changed:
        print('OK    %s: the %s section\'s list is numbered 1 to %d and its references name its items'
              % (shown, r.section, len(r.items)))
        return 0
    if a.check:
        print('FAIL  %s: the %s section\'s list or its references would change; run '
              'python build/renumber-upgrade-items.py%s and read what it says'
              % (shown, r.section, ' --section ' + r.section if a.section else ''))
        return 1
    out = r.text.replace('\n', '\r\n') if crlf else r.text
    with open(a.file, 'wb') as f:
        f.write((b'\xef\xbb\xbf' if bom else b'') + out.encode('utf-8'))
    print('WROTE %s' % shown)
    return 0


def self_test():
    failures = []

    def expect(name, cond, detail=''):
        if not cond:
            failures.append('%s%s' % (name, (': ' + detail) if detail else ''))

    def item(n, title, body='Text of the item.'):
        pad = ' ' * (len(str(n)) + 2)
        return '%d. **%s** %s\n%s%s' % (n, title, body, pad, 'A second line.')

    titles = {n: 'Change number %s to the mail flow' % w for n, w in
              zip(range(1, 25), ('one two three four five six seven eight nine ten eleven twelve thirteen '
                                 'fourteen fifteen sixteen seventeen eighteen nineteen twenty twentyone '
                                 'twentytwo twentythree twentyfour').split())}
    titles[14] = 'Smaller things a script, a monitor or a log reader may notice.'
    entries = [item(n, titles[n]) for n in range(1, 15)]
    entries.append(item(14, 'Event scripts on Linux run in QuickJS 2026-06-04'))
    entries += [item(n, titles[n]) for n in range(15, 25)]
    good_list = '\n'.join(entries)
    head = '# Changelog\n\nIntro.\n\n---\n\n## Unreleased - in the tree after 6.3.6, not yet released\n\n---\n\n'
    # An older release's section with a duplicate of its own: left alone unless asked for.
    old_section = ('## [6.3.5](https://gitlab.com/p/h/-/releases/v6.3.5) - 2026-09-29\n\nIts introduction.\n\n'
                   '### What changes on upgrade without a switch\n\n1. **One.** Text.\n1. **Again one.** Text.\n' +
                   ''.join('%d. **Older %d.** Text.\n' % (n, n) for n in range(2, 25)) +
                   '\n### Operations\n\n- **Logged.** And logged (upgrade item 24). See the '
                   'What changes on\nupgrade, item 2.\n\n')
    section = ('## [6.3.6](https://gitlab.com/p/h/-/releases/v6.3.6) - 2026-10-01\n\n'
               'The schema moves (What changes on upgrade without a switch, item 21); the key (item 22).\n\n'
               '### Security\n\n'
               '- **A flaw.** Fixed for the domains of a sender (What changes on\nupgrade, item 23). Also 6.3.5\'s '
               'upgrade item 2 and (6.3.5, What changes on upgrade, item 1) stay.\n'
               '- **Sessions.** A table holds more (What changes on upgrade, items 21 to 23), and items 22\n  and 24 '
               'and items 18-19 too.\n'
               '- **Work.** Text copied (work item 48, [work item #49](https://x/-/work_items/49)) and `item 7` in code.\n\n'
               '### What changes on upgrade without a switch\n\n' + good_list + '\n\n'
               '### Build and tests\n\n'
               '- **Renovate.** Taken: the images; and QuickJS 2026-06-04\n  (What changes on upgrade, item 14). '
               'Left: the floor.\n'
               '- **Probes.** What a log reader may notice (What changes on upgrade, item 14).\n\n'
               'Full notes: [the 6.3.6 release](https://gitlab.com/p/h/-/releases/v6.3.6).\n\n---\n\n')
    text = head + section + old_section
    r = renumber(text)
    expect('the default section is the newest with a list', r.section == '6.3.6', str(r.section))
    expect('no refusal of a good file', not r.errors, '; '.join(r.errors))
    out = r.text or ''
    new_sec = out[out.find('## [6.3.6]'):out.find('## [6.3.5]')]
    old_sec_out = out[out.find('## [6.3.5]'):]
    expect('the list is 1..25 in order', [int(x) for x in re.findall(r'(?m)^(\d+)\. ', new_sec)] == list(range(1, 26)),
           str(re.findall(r'(?m)^(\d+)\. ', new_sec)))
    expect('the duplicate 14: the second is 15', '15. **Event scripts on Linux run in QuickJS' in new_sec)
    expect('the duplicate 14: the first stays 14', '14. **Smaller things' in new_sec)
    expect('the last item is 25', '25. **Change number twentyfour' in new_sec)
    expect('a reference after the duplicate moves', 'without a switch, item 22); the key (item 23).' in new_sec)
    expect('a reference across a line break moves and keeps its break', 'What changes on\nupgrade, item 24).' in new_sec)
    expect('"items 21 to 23" becomes "items 22 to 24"', 'items 22 to 24)' in new_sec)
    expect('"items 22\\n  and 24" and "items 18-19" move', 'items 23\n  and 25' in new_sec and 'items 19-20 too' in new_sec)
    expect('QuickJS\'s "item 14" is taken to the QuickJS item', 'QuickJS 2026-06-04\n  (What changes on upgrade, item 15).' in new_sec)
    expect('a log reader\'s "item 14" is taken to the smaller things', 'log reader may notice (What changes on upgrade, item 14).' in new_sec)
    expect('it says which it took and why', any('took the one at' in d and '"QuickJS"' in d for d in r.decisions)
           and any('took the one at' in d and '"reader"' in d for d in r.decisions), '; '.join(r.decisions))
    expect('work items are not references', '(work item 48, [work item #49](https://x/-/work_items/49))' in new_sec)
    expect('a code span is not a reference', '`item 7` in code' in new_sec)
    expect('another release\'s item named inside the section is left, and said',
           "6.3.5's upgrade item 2 and (6.3.5, What changes on upgrade, item 1) stay" in new_sec
           and sum('6.3.5\'s list' in n for n in r.notes) == 2, '; '.join(r.notes))
    expect('an older release\'s section is left alone, its "upgrade item 24" and its own duplicate',
           old_sec_out == old_section, old_sec_out[:200])
    r2 = renumber(out)
    expect('a second run changes nothing', not r2.errors and not r2.changed, '; '.join(r2.errors))

    # Asked: a reference to the duplicate whose text names neither.
    ask = text.replace('- **Probes.** What a log reader may notice', '- **Probes.** As said')
    ra = renumber(ask)
    ask_line = ask[:ask.index('- **Probes.** As said')].count('\n') + 1
    expect('an ambiguous reference naming neither is refused, naming its line and asking',
           ra.text is None and any(e.startswith('line %d: "item 14" is ambiguous' % ask_line) and '--resolve %d:14=NEW' % ask_line in e
                                   and '14 for the one at line' in e and '15 for the one at line' in e for e in ra.errors),
           '; '.join(ra.errors))
    rr = renumber(ask, resolutions={(ask_line, 14): 15})
    expect('--resolve answers it', not rr.errors and 'As said (What changes on upgrade, item 15).' in (rr.text or ''),
           '; '.join(rr.errors))
    rw = renumber(ask, resolutions={(ask_line, 14): 16})
    expect('a --resolve that names neither item is refused', rw.errors and rw.text is None)
    rn = renumber(text, resolutions={(3, 14): 15})
    expect('a --resolve with no such reference is refused', any('names no reference' in e for e in rn.errors))

    # Refused: a number that is not an item.
    bad = text.replace('the key (item 22)', 'the key (item 99)')
    rb = renumber(bad)
    bad_line = bad[:bad.index('(item 99)')].count('\n') + 1
    expect('a reference to no item is refused, naming its line',
           rb.text is None and any(e.startswith('line %d: "item 99" - no item' % bad_line) for e in rb.errors),
           '; '.join(rb.errors))

    # --section: the older release's own list, renumbered; the newer one's left.
    ro = renumber(text, section='6.3.5')
    o = ro.text or ''
    expect('--section 6.3.5 renumbers its list and its own references only',
           not ro.errors and '2. **Again one.**' in o and '3. **Older 2.**' in o and '25. **Older 24.**' in o
           and 'What changes on\nupgrade, item 3.' in o and '(upgrade item 25)' in o
           and o[:o.find('## [6.3.5]')] == text[:text.find('## [6.3.5]')],
           '; '.join(ro.errors))
    rs = renumber(text, section='6.3.6')
    expect('--section 6.3.6 is the default\'s result', rs.text == r.text)
    rnone = renumber(text, section='6.2.0')
    expect('an absent section is refused', rnone.errors and rnone.text is None)
    rnolist = renumber(text, section='unreleased')
    expect('a section without the list is refused', rnolist.errors and rnolist.text is None)

    # 9 -> 10: an item's indented lines move with its wider number.
    nine = ('## Unreleased - x\n\n### What changes on upgrade without a switch\n\n' +
            '\n'.join('%d. **Item %s.** Text.' % (n, n) for n in range(1, 9)) + '\n'
            '9. **The archive.** Text\n   wrapped.\n   - a nested point\n     wrapped too.\n'
            '9. **The journal.** Text\n   wrapped.\n   - its own point\n'
            '10. **Tenth.** Text\n    wrapped, as the journal says (item 9).\n\n### Operations\n\nSee item 10.\n')
    rn9 = renumber(nine)
    t9 = rn9.text or ''
    expect('9 -> 10 takes its indented lines with it',
           not rn9.errors and '10. **The journal.** Text\n    wrapped.\n    - its own point\n' in t9
           and '9. **The archive.** Text\n   wrapped.\n   - a nested point\n     wrapped too.\n' in t9
           and '11. **Tenth.** Text\n    wrapped, as the journal says (item 10).' in t9 and 'See item 11.' in t9,
           '; '.join(rn9.errors) + ' ' + t9[-260:])

    # A line outside the section that names the release and an item is said.
    outside = text.replace('## Unreleased - in the tree after 6.3.6, not yet released\n\n',
                           '## Unreleased - in the tree after 6.3.6, not yet released\n\n- As 6.3.6\'s upgrade item 21 '
                           'said.\n\n')
    rout = renumber(outside)
    expect('a reference from outside the section is said, and left',
           any('outside the section' in n for n in rout.notes) and "6.3.6's upgrade item 21 said" in (rout.text or ''),
           '; '.join(rout.notes))

    # --check and the line endings, through main() on a file.
    import tempfile
    d = tempfile.mkdtemp(prefix='renumber-upgrade-items-')
    try:
        p = os.path.join(d, 'CHANGELOG.md')
        with open(p, 'wb') as f:
            f.write(text.replace('\n', '\r\n').encode('utf-8'))
        import io
        import contextlib
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            c1 = main(['--file', p, '--check'])
            w = main(['--file', p])
            c2 = main(['--file', p, '--check'])
        with open(p, 'rb') as f:
            data = f.read()
        expect('--check exits 1 before, 0 after; the write exits 0', (c1, w, c2) == (1, 0, 0), str((c1, w, c2)))
        expect('the file keeps CRLF', data.count(b'\r\n') == data.count(b'\n') and data.decode('utf-8').replace('\r\n', '\n') == r.text)
        with open(p, 'wb') as f:
            f.write(bad.encode('utf-8'))
        with contextlib.redirect_stdout(sink):
            c3 = main(['--file', p])
        with open(p, 'rb') as f:
            expect('a refusal exits 2 and writes nothing', c3 == 2 and f.read() == bad.encode('utf-8'), str(c3))
    finally:
        for name in os.listdir(d):
            os.remove(os.path.join(d, name))
        os.rmdir(d)

    for f in failures:
        print('FAIL: ' + f)
    print('self-test: %d failure(s)' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
