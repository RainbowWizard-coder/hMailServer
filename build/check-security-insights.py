#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""SECURITY-INSIGHTS.yml is valid, current and true to the tree.

SECURITY-INSIGHTS.yml, at the root, states the project's security facts in the
OpenSSF Security Insights format, version 2.2.0, for the tools that read them -
OpenSSF's OSPS Baseline scanner among them. Nothing else in the tree reads it, so
nothing else would notice when it goes wrong. This holds it:

  * to the format: the file is read as YAML - a plain subset of it, block mappings
    and sequences, plain, quoted and block scalars, and the empty {} and [], with
    anything else refused rather than guessed at - and held to the schema of
    spec/schema.cue at the specification's tag v2.2.0, written out below
    (SCHEMA): every required key, no key the schema does not have (its
    definitions are closed), every type, the enumerations, and the patterns of a
    URL, an e-mail address, a date and a schema version;
  * to time: last-updated and last-reviewed are real dates, neither after the
    latest calendar date anywhere on Earth (the UTC clock plus fourteen hours, so
    that a file dated in the owner's time zone, or any other, just after its
    midnight passes on a runner that keeps UTC), and the file is not reviewed
    before it was last updated;
  * to the project: schema-version is 2.2.0, header.url is the file's own raw
    address on GitLab's master, the name says Progressive Robot's fork of
    hMailServer, the repository is the GitLab project and the project's
    repositories name it and the GitHub copy, the licence is AGPL-3.0-or-later
    with LICENSE beside it, exactly one administrator and one core-team member is
    primary, and the vulnerability contact's address is one SECURITY.md gives;
  * to the tree: every link to a file of the repository - on GitLab's master
    (blob, raw or tree) or the GitHub copy's - names a path the tree holds, and a
    link to a heading of a Markdown file names a heading that file has, by
    GitLab's (or GitHub's) anchor rule; a release file named by a link under
    /-/releases/v{version}/downloads/ is a name build/ci/release-chain.py gives
    one, and {version} is the only placeholder;
  * to the tree's paths: every path of the tree the file names anywhere outside a
    link - in a value (a tool's rulesets, a comment) or in a YAML comment - is
    one the tree holds: a path whose first part is one of the tree's top-level
    folders (build/, .gitlab/, .github/, fuzz/, hmailserver/, libraries/, or any
    other the tree has), and a file at the root named in capitals, such as
    SECURITY.md, RELEASE.md or CONTRIBUTING.md; a path that ends in / must be a
    folder. So a renamed script or ruleset fails the check, where only its
    links did before.

It proves its own negative controls on every run, on made-up trees: a file that
breaks each rule above must be refused and the good one must pass, so a check
that has stopped seeing what it reads fails as well. --self-test runs them alone
and says each.

  python build/check-security-insights.py [--self-test]
"""
import datetime
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
INSIGHTS = 'SECURITY-INSIGHTS.yml'
SCHEMA_VERSION = '2.2.0'
GITLAB = 'https://gitlab.com/hmailserver/hmailserver'
GITHUB = 'https://github.com/RainbowWizard-coder/hMailServer'
OWN_URL = GITLAB + '/-/raw/master/' + INSIGHTS
FORK = "Progressive Robot's fork of hMailServer"
LICENCE = 'AGPL-3.0-or-later'
RELEASE_CHAIN = os.path.join('build', 'ci', 'release-chain.py')


class YamlError(Exception):
    pass


# ----------------------------------------------------------------- the YAML subset

def _strip_comment(text):
    """The line without a comment: a # at its start or after a space, outside quotes."""
    quote = None
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = None
            continue
        if ch in '"\'' and (i == 0 or text[i - 1] in ' :-[{,'):
            quote = ch
        elif ch == '#' and (i == 0 or text[i - 1] in ' \t'):
            return text[:i].rstrip()
    return text.rstrip()


def _scalar(text, where):
    """A one-line scalar: quoted, the empty flow {} or [], or plain."""
    if text.startswith("'"):
        if len(text) < 2 or not text.endswith("'"):
            raise YamlError('%s: an unclosed single-quoted scalar' % where)
        body = text[1:-1]
        if "'" in body.replace("''", ''):
            raise YamlError('%s: a quote inside a single-quoted scalar' % where)
        return body.replace("''", "'")
    if text.startswith('"'):
        if len(text) < 2 or not text.endswith('"'):
            raise YamlError('%s: an unclosed double-quoted scalar' % where)
        body = text[1:-1]
        out = []
        i = 0
        while i < len(body):
            ch = body[i]
            if ch == '"':
                raise YamlError('%s: a quote inside a double-quoted scalar' % where)
            if ch == '\\':
                nxt = body[i + 1:i + 2]
                simple = {'\\': '\\', '"': '"', 'n': '\n', 't': '\t', '/': '/'}
                if nxt in simple:
                    out.append(simple[nxt])
                    i += 2
                    continue
                if nxt == 'u' and re.match(r'[0-9A-Fa-f]{4}$', body[i + 2:i + 6]):
                    out.append(chr(int(body[i + 2:i + 6], 16)))
                    i += 6
                    continue
                raise YamlError('%s: an escape this reader does not take' % where)
            out.append(ch)
            i += 1
        return ''.join(out)
    if text == '{}':
        return {}
    if text == '[]':
        return []
    if text[0] in '[{&*!|>%@`,?':
        raise YamlError('%s: %r is YAML this reader does not take (flow collections, anchors, aliases, '
                        'tags and directives are refused)' % (where, text[:20]))
    if ': ' in text or text.endswith(':'):
        raise YamlError('%s: a plain scalar holding ": " - quote it' % where)
    if text in ('true', 'false'):
        return text == 'true'
    if text in ('null', '~', 'Null', 'NULL'):
        return None
    if text in ('True', 'False', 'TRUE', 'FALSE', 'yes', 'no', 'Yes', 'No', 'on', 'off'):
        raise YamlError('%s: %r reads differently in YAML 1.1 and 1.2 - write true or false, or quote it'
                        % (where, text))
    if re.match(r'[-+]?[0-9]+$', text):
        return int(text)
    if re.match(r'[-+]?([0-9]+\.[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?$', text):
        raise YamlError('%s: %r is a number - quote it if it is a string' % (where, text))
    if re.match(r'[0-9]{4}-[0-9]{2}-[0-9]{2}$', text):
        raise YamlError('%s: an unquoted date, which some YAML readers take for a timestamp - quote it'
                        % where)
    return text


def parse_yaml(text):
    """The document, from the subset of YAML described in the module's docstring."""
    if text.startswith('﻿'):
        text = text[1:]
    raw = text.replace('\r\n', '\n').split('\n')
    lines = []  # (number, indent, content) for each line that is not blank or a comment
    for number, line in enumerate(raw, 1):
        if '\t' in line[:len(line) - len(line.lstrip())]:
            raise YamlError('line %d: a tab in the indentation' % number)
        if line.strip() in ('---', '...') or line.startswith('%'):
            raise YamlError('line %d: a document marker or directive' % number)
        lines.append((number, len(line) - len(line.lstrip(' ')), line))
    state = {'i': 0}

    def significant(i):
        while i < len(lines):
            number, indent, line = lines[i]
            content = _strip_comment(line[indent:])
            if content:
                return i
            i += 1
        return None

    def block_scalar(style, indent, where):
        """The lines of a | or > scalar, more indented than its key."""
        out = []
        body_indent = None
        i = state['i']
        while i < len(lines):
            number, ind, line = lines[i]
            if line.strip() == '':
                out.append('')
                i += 1
                continue
            if ind <= indent:
                break
            if body_indent is None:
                body_indent = ind
            if ind < body_indent:
                raise YamlError('line %d: a block scalar line less indented than its first' % number)
            out.append(line[body_indent:])
            i += 1
        state['i'] = i
        while out and out[-1] == '':
            out.pop()
        if not out:
            raise YamlError('%s: an empty block scalar' % where)
        if style[0] == '|':
            value = '\n'.join(out)
        else:
            value = ''
            for part in out:
                if part == '':
                    value += '\n'
                elif value and not value.endswith('\n'):
                    value += ' ' + part
                else:
                    value += part
        if style not in ('|-', '>-'):
            value += '\n'
        return value

    def value_after(rest, indent, where):
        """The value of a key or a sequence entry whose text after the indicator is rest."""
        if rest in ('|', '|-', '>', '>-'):
            return block_scalar(rest, indent, where)
        if rest:
            if rest[0] in '|>':
                raise YamlError('%s: a block scalar indicator this reader does not take' % where)
            return _scalar(rest, where)
        nxt = significant(state['i'])
        if nxt is None:
            raise YamlError('%s: no value' % where)
        number, ind, line = lines[nxt]
        content = _strip_comment(line[ind:])
        if ind > indent or (ind == indent and (content == '-' or content.startswith('- '))):
            return node(ind)
        raise YamlError('%s: no value' % where)

    def node(indent):
        first = significant(state['i'])
        number, ind, line = lines[first]
        content = _strip_comment(line[ind:])
        if content == '-' or content.startswith('- '):
            return sequence(ind)
        return mapping(ind)

    def mapping(indent, first_key_line=None):
        """A block mapping at indent; first_key_line is (number, indent, text) of a first
        key that shares its line with a sequence entry's dash, already read past."""
        result = {}
        pending = first_key_line
        while True:
            if pending is not None:
                number, ind, content = pending
                pending = None
                after = state['i']
            else:
                i = significant(state['i'])
                if i is None:
                    return result
                number, ind, line = lines[i]
                if ind < indent:
                    return result
                if ind > indent:
                    raise YamlError('line %d: indented more than the mapping it is in' % number)
                content = _strip_comment(line[ind:])
                if content == '-' or content.startswith('- '):
                    return result
                after = i + 1
            match = re.match(r'([A-Za-z0-9_][A-Za-z0-9_.-]*)[ ]*:(?: (.*))?$', content)
            if not match:
                raise YamlError('line %d: not a "key: value" line this reader takes: %r' % (number, content[:40]))
            key = match.group(1)
            if key in result:
                raise YamlError('line %d: the key %r twice' % (number, key))
            state['i'] = after
            result[key] = value_after((match.group(2) or '').strip(), ind, 'line %d (%s)' % (number, key))

    def sequence(indent):
        result = []
        while True:
            i = significant(state['i'])
            if i is None:
                return result
            number, ind, line = lines[i]
            content = _strip_comment(line[ind:])
            if ind < indent or not (content == '-' or content.startswith('- ')):
                if ind > indent:
                    raise YamlError('line %d: indented more than the sequence it is in' % number)
                return result
            if ind > indent:
                raise YamlError('line %d: indented more than the sequence it is in' % number)
            rest = content[1:].lstrip(' ')
            state['i'] = i + 1
            where = 'line %d' % number
            if rest and re.match(r'[A-Za-z0-9_][A-Za-z0-9_.-]*[ ]*:(?: |$)', rest):
                inner = ind + (len(content) - len(rest))
                result.append(mapping(inner, (number, inner, rest)))
            else:
                result.append(value_after(rest, ind, where))

    start = significant(0)
    if start is None:
        raise YamlError('the file is empty')
    if lines[start][1] != 0:
        raise YamlError('line %d: the document does not start at the left margin' % lines[start][0])
    document = node(0)
    rest = significant(state['i'])
    if rest is not None:
        raise YamlError('line %d: text after the document ends' % lines[rest][0])
    return document


# ----------------------------------------------------------------- the schema (v2.2.0)

URL_RE = re.compile(r'^https?://[^\s]+$')
EMAIL_RE = re.compile(r'^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$')
SCHEMA_VERSION_RE = re.compile(r'^[1-9]+\.[0-9]+\.[0-9]+$')
TOOL_TYPES = ('fuzzing', 'container', 'secret', 'SCA', 'SAST', 'other')
STATUSES = ('active', 'abandoned', 'concept', 'inactive', 'moved', 'suspended', 'unsupported', 'WIP')

# Each definition: {key: (required, type)}; a type is 'str', 'bool', 'url', 'email',
# 'date', 'schema-version', ('enum', values), ('list', type, minimum) or a definition's name.
SCHEMA = {
    'SecurityInsights': {'header': (True, 'Header'), 'project': (False, 'Project'),
                         'repository': (False, 'Repository')},
    'Header': {'last-reviewed': (True, 'date'), 'last-updated': (True, 'date'),
               'schema-version': (True, 'schema-version'), 'url': (True, 'url'),
               'comment': (False, 'str'), 'project-si-source': (False, 'url')},
    'Assessment': {'comment': (True, 'str'), 'name': (False, 'str'), 'evidence': (False, 'url'),
                   'date': (False, 'date')},
    'Attestation': {'name': (True, 'str'), 'location': (True, 'url'), 'predicate-uri': (True, 'str'),
                    'comment': (False, 'str')},
    'Contact': {'name': (True, 'str'), 'primary': (True, 'bool'), 'affiliation': (False, 'str'),
                'email': (False, 'email'), 'social': (False, 'str')},
    'License': {'url': (True, 'url'), 'expression': (True, 'str')},
    'Link': {'uri': (True, 'str'), 'comment': (True, 'str')},
    'ProjectRepository': {'name': (True, 'str'), 'comment': (True, 'str'), 'url': (True, 'url')},
    'ProjectDocumentation': {key: (False, 'url') for key in (
        'design', 'detailed-guide', 'code-of-conduct', 'quickstart-guide', 'release-process',
        'support-policy', 'signature-verification')},
    'VulnerabilityReporting': {'reports-accepted': (True, 'bool'), 'bug-bounty-available': (True, 'bool'),
                               'bug-bounty-program': (False, 'url'), 'contact': (False, 'Contact'),
                               'comment': (False, 'str'), 'policy': (False, 'url'), 'pgp-key': (False, 'str'),
                               'in-scope': (False, ('list', 'str', 0)),
                               'out-of-scope': (False, ('list', 'str', 0))},
    'Project': {'name': (True, 'str'), 'homepage': (False, 'url'), 'roadmap': (False, 'url'),
                'funding': (False, 'url'), 'steward': (False, 'Link'),
                'administrators': (True, ('list', 'Contact', 1)),
                'repositories': (True, ('list', 'ProjectRepository', 1)),
                'vulnerability-reporting': (True, 'VulnerabilityReporting'),
                'documentation': (False, 'ProjectDocumentation')},
    'SecurityToolIntegration': {'adhoc': (True, 'bool'), 'ci': (True, 'bool'), 'release': (True, 'bool')},
    'SecurityToolResults': {'adhoc': (False, 'Attestation'), 'ci': (False, 'Attestation'),
                            'release': (False, 'Attestation')},
    'SecurityTool': {'name': (True, 'str'), 'type': (True, ('enum', TOOL_TYPES)), 'version': (False, 'str'),
                     'comment': (False, 'str'), 'rulesets': (True, ('list', 'str', 0)),
                     'integration': (True, 'SecurityToolIntegration'), 'results': (True, 'SecurityToolResults')},
    'Assessments': {'self': (True, 'Assessment'), 'third-party': (False, ('list', 'Assessment', 0))},
    'SecurityPosture': {'assessments': (True, 'Assessments'), 'champions': (False, ('list', 'Contact', 0)),
                        'tools': (False, ('list', 'SecurityTool', 0))},
    'RepositoryDocumentation': {key: (False, 'url') for key in (
        'contributing-guide', 'dependency-management-policy', 'governance', 'review-policy',
        'security-policy')},
    'ReleaseDetails': {'automated-pipeline': (True, 'bool'),
                       'distribution-points': (True, ('list', 'Link', 1)), 'changelog': (False, 'url'),
                       'license': (False, 'License'), 'attestations': (False, ('list', 'Attestation', 0))},
    'Repository': {'status': (True, ('enum', STATUSES)), 'url': (True, 'url'),
                   'accepts-change-request': (True, 'bool'), 'accepts-automated-change-request': (True, 'bool'),
                   'bug-fixes-only': (False, 'bool'), 'no-third-party-packages': (False, 'bool'),
                   'core-team': (True, ('list', 'Contact', 1)), 'license': (True, 'License'),
                   'security': (True, 'SecurityPosture'), 'documentation': (False, 'RepositoryDocumentation'),
                   'release': (False, 'ReleaseDetails')},
}


def valid_date(value):
    if not isinstance(value, str) or not re.match(r'^[0-9]{4}-[0-9]{2}-[0-9]{2}$', value):
        return None
    try:
        return datetime.date(int(value[:4]), int(value[5:7]), int(value[8:10]))
    except ValueError:
        return None


def check_type(value, kind, where, problems):
    if isinstance(kind, tuple) and kind[0] == 'list':
        if not isinstance(value, list):
            problems.append('%s is not a list' % where)
            return
        if len(value) < kind[2]:
            problems.append('%s needs at least %d entr%s' % (where, kind[2], 'y' if kind[2] == 1 else 'ies'))
        for index, item in enumerate(value):
            check_type(item, kind[1], '%s[%d]' % (where, index), problems)
        return
    if isinstance(kind, tuple) and kind[0] == 'enum':
        if value not in kind[1]:
            problems.append('%s is %r, not one of %s' % (where, value, ', '.join(kind[1])))
        return
    if kind in SCHEMA:
        if not isinstance(value, dict):
            problems.append('%s is not a mapping (%s)' % (where or 'the file', kind))
            return
        definition = SCHEMA[kind]
        for key in value:
            if key not in definition:
                problems.append('%s has %r, which %s does not have' % (where or 'the file', key, kind))
        for key, (required, sub) in definition.items():
            if key in value:
                check_type(value[key], sub, '%s.%s' % (where, key) if where else key, problems)
            elif required:
                problems.append('%s has no %s, which %s requires' % (where or 'the file', key, kind))
        return
    if kind == 'bool':
        if not isinstance(value, bool):
            problems.append('%s is not true or false' % where)
        return
    if not isinstance(value, str) or not value.strip():
        problems.append('%s is not a string' % where)
        return
    if kind == 'url' and not URL_RE.match(value):
        problems.append('%s is not a URL: %r' % (where, value))
    elif kind == 'email' and not EMAIL_RE.match(value):
        problems.append('%s is not an e-mail address: %r' % (where, value))
    elif kind == 'date' and valid_date(value) is None:
        problems.append('%s is not a date YYYY-MM-DD: %r' % (where, value))
    elif kind == 'schema-version' and not SCHEMA_VERSION_RE.match(value):
        problems.append('%s is not a schema version X.Y.Z: %r' % (where, value))


# ----------------------------------------------------------------- the tree's files and headings

def anchors(text):
    """The heading anchors of a Markdown text, as GitLab (and GitHub) make them."""
    seen = {}
    found = set()
    lines = text.replace('\r\n', '\n').split('\n')
    fence = None
    previous = ''
    for line in lines:
        stripped = line.strip()
        marker = re.match(r'(`{3,}|~{3,})', stripped)
        if fence:
            if marker and stripped.startswith(fence):
                fence = None
            previous = ''
            continue
        if marker:
            fence = marker.group(1)[0] * 3
            previous = ''
            continue
        heading = None
        atx = re.match(r'^ {0,3}#{1,6}[ \t]+(.*?)[ \t]*#*[ \t]*$', line)
        if atx:
            heading = atx.group(1)
        elif re.match(r'^ {0,3}(=+|-+)[ \t]*$', line) and previous.strip() and \
                not re.match(r'^ {0,3}([-*+]|[0-9]+[.)])[ \t]|^ {0,3}#|^ {0,3}>|^ {0,3}\|', previous):
            heading = previous.strip()
        if heading is not None:
            slug = heading.lower()
            slug = re.sub(r'<[^>]*>', '', slug)
            slug = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', slug)
            slug = re.sub(r'[^\w\- ]', '', slug)
            slug = slug.replace(' ', '-')
            slug_gitlab = re.sub(r'-{2,}', '-', slug)
            for candidate in {slug, slug_gitlab}:
                count = seen.get(candidate, 0)
                found.add(candidate if count == 0 else '%s-%d' % (candidate, count))
                seen[candidate] = count + 1
            previous = ''
            continue
        previous = line
    return found


TREE_LINK = re.compile(r'^(?:' + re.escape(GITLAB) + r'/-/(?:blob|raw|tree)/master/|' +
                       re.escape(GITHUB) + r'/(?:blob|raw|tree)/master/)([^?#]+)(?:#(.*))?$')
RELEASE_FILE = re.compile(r'^' + re.escape(GITLAB) + r'/-/releases/v\{version\}/downloads/([^/?#]+)$')


def strings(value, where=''):
    if isinstance(value, dict):
        for key, item in value.items():
            for found in strings(item, '%s.%s' % (where, key) if where else key):
                yield found
    elif isinstance(value, list):
        for index, item in enumerate(value):
            for found in strings(item, '%s[%d]' % (where, index)):
                yield found
    elif isinstance(value, str):
        yield where, value


def today(utc_now=None):
    """The latest calendar date it is anywhere: the UTC clock plus fourteen hours, the
    furthest ahead of UTC any time zone is (UTC+14). A file dated in any time zone's
    today passes, whatever zone the machine running the check keeps."""
    utc_now = utc_now or datetime.datetime.now(datetime.timezone.utc)
    return (utc_now + datetime.timedelta(hours=14)).date()


# A path of the tree named outside a link: segments of letters, digits, _ and -, the
# first of which may start with a dot, joined by /, the last optionally a file name. It
# must not follow a character that would make it part of a longer word, a host name or
# a URL (a letter, a digit, ., /, : or -).
TREE_PATH = re.compile(r'(?<![\w./:-])((?:\.?[A-Za-z0-9_-]+/)+[A-Za-z0-9_.-]*)')
# A file at the root named in capitals, such as SECURITY.md, outside a link.
ROOT_FILE = re.compile(r'(?<![\w./:-])([A-Z][A-Z0-9_-]*\.(?:md|yml|yaml|txt))(?![\w/-])')
TOP_LEVEL = ('build', '.gitlab', '.github', 'fuzz', 'hmailserver', 'libraries')


def named_paths(text, root):
    """Each (line number, path) of the tree that text names outside a URL."""
    try:
        top = set(TOP_LEVEL) | {entry for entry in os.listdir(root) if os.path.isdir(os.path.join(root, entry))}
    except OSError:
        top = set(TOP_LEVEL)
    top.discard('.git')
    for number, line in enumerate(text.splitlines(), 1):
        for token in TREE_PATH.findall(line):
            token = token.rstrip('.')
            if token.split('/', 1)[0] in top:
                yield number, token
        for token in ROOT_FILE.findall(line):
            yield number, token


def check(root, now=None):
    problems = []
    path = os.path.join(root, INSIGHTS)
    try:
        with open(path, encoding='utf-8', newline='') as f:
            text = f.read()
    except OSError:
        return ['%s is not at the root of the tree' % INSIGHTS]
    try:
        document = parse_yaml(text)
    except YamlError as error:
        return ['%s does not parse: %s' % (INSIGHTS, error)]
    if not isinstance(document, dict):
        return ['%s is not a mapping' % INSIGHTS]
    check_type(document, 'SecurityInsights', '', problems)
    if problems:
        return ['%s: %s' % (INSIGHTS, problem) for problem in problems]

    header = document['header']
    project = document.get('project')
    repository = document.get('repository')
    if header['schema-version'] != SCHEMA_VERSION:
        problems.append('header.schema-version is %s; this check is written for %s - read the new '
                        'version\'s schema.cue and bring SCHEMA here to it first'
                        % (header['schema-version'], SCHEMA_VERSION))
    now = now or today()
    updated, reviewed = valid_date(header['last-updated']), valid_date(header['last-reviewed'])
    for key, value in (('last-updated', updated), ('last-reviewed', reviewed)):
        if value > now:
            problems.append('header.%s is %s, which is in the future (today is %s)' % (key, value, now))
    if reviewed < updated:
        problems.append('header.last-reviewed (%s) is before last-updated (%s): review the file with the change'
                        % (reviewed, updated))
    if header['url'] != OWN_URL:
        problems.append('header.url is %r, not the file\'s own address, %s' % (header['url'], OWN_URL))
    if not project or not repository:
        problems.append('the file has no %s: this repository is the project\'s own, and carries both'
                        % ('project' if not project else 'repository'))
        return ['%s: %s' % (INSIGHTS, problem) for problem in problems]

    if FORK not in project['name']:
        problems.append('project.name does not say %r' % FORK)
    if repository['url'] != GITLAB:
        problems.append('repository.url is %r, not the project on GitLab, %s' % (repository['url'], GITLAB))
    urls = [entry['url'] for entry in project['repositories']]
    for wanted, what in ((GITLAB, 'the project on GitLab'), (GITHUB, 'its copy on GitHub')):
        if wanted not in urls:
            problems.append('project.repositories does not name %s, %s' % (what, wanted))
    for where, contacts in (('project.administrators', project['administrators']),
                            ('repository.core-team', repository['core-team'])):
        primaries = sum(1 for contact in contacts if contact['primary'])
        if primaries != 1:
            problems.append('%s has %d primary contacts, where the schema says one' % (where, primaries))
    if repository['license']['expression'] != LICENCE:
        problems.append('repository.license.expression is %r, not %s' % (repository['license']['expression'], LICENCE))
    try:
        with open(os.path.join(root, 'LICENSE'), encoding='utf-8') as f:
            if 'GNU AFFERO GENERAL PUBLIC LICENSE' not in f.read(400):
                problems.append('LICENSE is not the GNU Affero General Public License')
    except OSError:
        problems.append('the tree has no LICENSE')
    contact = project['vulnerability-reporting'].get('contact') or {}
    if contact.get('email'):
        try:
            with open(os.path.join(root, 'SECURITY.md'), encoding='utf-8') as f:
                if contact['email'] not in f.read():
                    problems.append('the vulnerability contact %s is an address SECURITY.md does not give'
                                    % contact['email'])
        except OSError:
            problems.append('the tree has no SECURITY.md')

    try:
        with open(os.path.join(root, RELEASE_CHAIN), encoding='utf-8') as f:
            chain = f.read()
    except OSError:
        chain = ''
    headings = {}
    for where, value in strings(document):
        for placeholder in re.findall(r'\{[^}]*\}', value):
            if placeholder != '{version}':
                problems.append('%s holds the placeholder %s; {version} is the only one' % (where, placeholder))
        if not URL_RE.match(value):
            continue
        release = RELEASE_FILE.match(value)
        if release:
            if '"%s"' % release.group(1) not in chain:
                problems.append('%s names the release file %s, which %s gives no file'
                                % (where, release.group(1), RELEASE_CHAIN.replace(os.sep, '/')))
            continue
        link = TREE_LINK.match(value)
        if not link:
            continue
        relative = link.group(1).rstrip('/')
        target = os.path.join(root, *relative.split('/'))
        if not os.path.exists(target):
            problems.append('%s links to %s, which the tree does not hold' % (where, relative))
            continue
        anchor = link.group(2)
        if anchor:
            if not relative.lower().endswith('.md') or not os.path.isfile(target):
                problems.append('%s links to a heading of %s, which is not a Markdown file' % (where, relative))
                continue
            if relative not in headings:
                with open(target, encoding='utf-8') as f:
                    headings[relative] = anchors(f.read())
            if anchor not in headings[relative]:
                problems.append('%s links to #%s, a heading %s does not have' % (where, anchor, relative))
    seen = set()
    for number, relative in named_paths(text, root):
        if relative in seen:
            continue
        seen.add(relative)
        target = os.path.join(root, *relative.rstrip('/').split('/'))
        if relative.endswith('/') and not os.path.isdir(target):
            problems.append('line %d names the folder %s, which the tree does not hold' % (number, relative))
        elif not relative.endswith('/') and not os.path.exists(target):
            problems.append('line %d names %s, which the tree does not hold' % (number, relative))
    return ['%s: %s' % (INSIGHTS, problem) for problem in problems]


# ----------------------------------------------------------------- the negative controls

GOOD = '''# A comment.
header:
  schema-version: 2.2.0
  last-updated: '2026-01-02'
  last-reviewed: "2026-01-03"
  url: https://gitlab.com/hmailserver/hmailserver/-/raw/master/SECURITY-INSIGHTS.yml
  comment: >-
    Folded, over
    two lines.
project:
  name: hMailServer (Progressive Robot's fork of hMailServer)
  homepage: https://gitlab.com/hmailserver/hmailserver
  administrators:
    - name: A Maintainer
      primary: true
  repositories:
    - name: GitLab
      url: https://gitlab.com/hmailserver/hmailserver
      comment: The home.
    - name: GitHub
      url: https://github.com/RainbowWizard-coder/hMailServer
      comment: |
        The copy.
  vulnerability-reporting:
    reports-accepted: true
    bug-bounty-available: false
    policy: https://gitlab.com/hmailserver/hmailserver/-/blob/master/SECURITY.md#reporting-a-vulnerability
    contact:
      name: Reports
      email: reports@example.org
      primary: true
  documentation:
    signature-verification: https://github.com/RainbowWizard-coder/hMailServer/blob/master/SECURITY.md#verifying-a-release
    detailed-guide: https://gitlab.com/hmailserver/hmailserver/-/blob/master/docs/README.md
repository:
  url: https://gitlab.com/hmailserver/hmailserver
  status: active
  accepts-change-request: true
  accepts-automated-change-request: false
  core-team:
  - name: A Maintainer
    primary: true
  - name: Another
    primary: false
  license:
    url: https://gitlab.com/hmailserver/hmailserver/-/blob/master/LICENSE
    expression: AGPL-3.0-or-later
  documentation:
    governance: https://gitlab.com/hmailserver/hmailserver/-/blob/master/GOVERNANCE.md#how-decisions-are-made
  release:
    automated-pipeline: true
    distribution-points:
      - uri: https://gitlab.com/hmailserver/hmailserver/-/releases
        comment: Releases.
    attestations:
      - name: Provenance
        location: https://gitlab.com/hmailserver/hmailserver/-/releases/v{version}/downloads/hmailserver.intoto.sigstore.json
        predicate-uri: https://slsa.dev/provenance/v1
  security:
    assessments:
      self:
        comment: Done.
        evidence: https://gitlab.com/hmailserver/hmailserver/-/blob/master/ASSURANCE-CASE.md
        date: '2026-01-01'
    tools:
      - name: A scanner
        type: SAST
        rulesets:
          - build/ci/rules.yaml
          - p/csharp
        integration:
          adhoc: false
          ci: true
          release: true
        results: {}
        comment: >-
          The job (build/ci/scan.sh) over fuzz/, as SECURITY.md says; registry.gitlab.com/hmailserver/x
          and https://example.org/build/ci/gone.sh name no path of the tree.
'''
FILES = {
    'LICENSE': '                    GNU AFFERO GENERAL PUBLIC LICENSE\n',
    'SECURITY.md': '# Security Policy\n\n## Reporting a vulnerability\n\nMail reports@example.org.\n\n'
                   '```\n## Not a heading\n```\n\nVerifying a release\n-------------------\n\nText.\n',
    'GOVERNANCE.md': 'Governance\n==========\n\nHow decisions are made\n----------------------\n',
    'ASSURANCE-CASE.md': 'Assurance case\n==============\n',
    'docs/README.md': 'Docs\n',
    'build/ci/rules.yaml': 'rules: []\n',
    'build/ci/scan.sh': '#!/bin/sh\n',
    'fuzz/README.md': 'Fuzzing\n',
    RELEASE_CHAIN: 'pairs = [("hmailserver-1.0.0.intoto.sigstore.json", "hmailserver.intoto.sigstore.json")]\n',
}
NOW = datetime.date(2026, 1, 3)


def self_test(quiet=False):
    failures = []

    def expect(what, condition):
        if not quiet:
            print('%s  %s' % ('ok  ' if condition else 'FAIL', what))
        if not condition:
            failures.append(what)

    def world(insights=GOOD, drop=(), now=NOW):
        work = tempfile.mkdtemp(prefix='hm-insights-')
        try:
            for relative, text in FILES.items():
                if relative in drop:
                    continue
                full = os.path.join(work, *relative.replace(os.sep, '/').split('/'))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, 'w', encoding='utf-8', newline='\r\n') as f:
                    f.write(text)
            if insights is not None:
                with open(os.path.join(work, INSIGHTS), 'w', encoding='utf-8', newline='\r\n') as f:
                    f.write(insights)
            return check(work, now)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    good = world()
    expect('the good file, CRLF, its links and headings in the tree: passes%s'
           % ('' if not good else ' (%s)' % good[0][:120]), good == [])
    parsed = parse_yaml(GOOD)
    expect('the reader: a folded scalar, a literal one, a list at its key\'s indent, {} and quotes',
           parsed['header']['comment'] == 'Folded, over two lines.'
           and parsed['project']['repositories'][1]['comment'] == 'The copy.\n'
           and len(parsed['repository']['core-team']) == 2
           and parsed['repository']['security']['tools'][0]['results'] == {}
           and parsed['header']['last-reviewed'] == '2026-01-03'
           and parsed['repository']['accepts-automated-change-request'] is False)

    def swap(old, new):
        assert old in GOOD, old
        return GOOD.replace(old, new, 1)

    cases = [
        ('a date in the future', swap("last-reviewed: \"2026-01-03\"", "last-reviewed: '2026-01-04'"), (),
         'in the future'),
        ('reviewed before it was updated', swap("last-updated: '2026-01-02'", "last-updated: '2026-01-03'").replace(
            'last-reviewed: "2026-01-03"', "last-reviewed: '2026-01-02'"), (), 'before last-updated'),
        ('a date that is no date', swap("'2026-01-02'", "'2026-02-30'"), (), 'not a date'),
        ('an unquoted date', swap("'2026-01-02'", '2026-01-02'), (), 'unquoted date'),
        ('another schema version', swap('schema-version: 2.2.0', 'schema-version: 2.1.0'), (), 'written for 2.2.0'),
        ('a key the schema does not have', swap('  status: active\n', '  status: active\n  colour: blue\n'), (),
         "has 'colour'"),
        ('a required key left out', swap('    bug-bounty-available: false\n', ''), (), 'no bug-bounty-available'),
        ('a status the schema does not list', swap('status: active', 'status: maintained'), (), 'not one of'),
        ('a tool type the schema does not list', swap('type: SAST', 'type: DAST'), (), 'not one of'),
        ('a string where a boolean goes', swap('reports-accepted: true', 'reports-accepted: "true"'), (),
         'not true or false'),
        ('a URL that is not one', swap('homepage: https://gitlab.com/hmailserver/hmailserver',
                                       'homepage: gitlab.com/hmailserver'), (), 'not a URL'),
        ('an address that is not one', swap('email: reports@example.org', 'email: reports'), (),
         'not an e-mail address'),
        ('no administrator', swap('  administrators:\n    - name: A Maintainer\n      primary: true\n',
                                  '  administrators: []\n'), (), 'at least 1'),
        ('two primary contacts', swap('    primary: false', '    primary: true'), (), '2 primary'),
        ('another header.url', swap('/-/raw/master/SECURITY-INSIGHTS.yml', '/-/raw/main/SECURITY-INSIGHTS.yml'), (),
         "not the file's own address"),
        ('a name that does not say fork', swap("(Progressive Robot's fork of hMailServer)", '(the original)'), (),
         'project.name'),
        ('no GitHub copy among the repositories', swap('https://github.com/RainbowWizard-coder/hMailServer\n',
                                                        'https://example.org/copy\n'), (), 'its copy on GitHub'),
        ('another licence', swap('expression: AGPL-3.0-or-later', 'expression: MIT'), (), 'not AGPL-3.0-or-later'),
        ('a contact SECURITY.md does not give', swap('reports@example.org', 'other@example.org'), (),
         'does not give'),
        ('a linked file the tree does not hold', GOOD, ('docs/README.md',), 'docs/README.md, which the tree'),
        ('a heading the file does not have', swap('#verifying-a-release', '#verifying-releases'), (),
         'a heading SECURITY.md does not have'),
        ('a heading that is only inside a code fence', swap('#verifying-a-release', '#not-a-heading'), (),
         'does not have'),
        ('a heading of a file that is not Markdown', swap('/LICENSE\n', '/LICENSE#top\n'), (), 'not a Markdown'),
        ('a release file the release chain gives no file',
         swap('downloads/hmailserver.intoto.sigstore.json', 'downloads/hmailserver.provenance.json'), (),
         'gives no file'),
        ('a placeholder other than {version}', swap('v{version}', 'v{tag}'), (), 'the placeholder {tag}'),
        ('an anchor', swap('  status: active', '  status: &s active'), (), 'does not take'),
        ('a flow sequence', swap('        rulesets:\n          - build/ci/rules.yaml\n          - p/csharp',
                                 '        rulesets: [default]'), (), 'does not take'),
        ('a ruleset the tree does not hold', GOOD, ('build/ci/rules.yaml',),
         'names build/ci/rules.yaml, which the tree'),
        ('a script named in a comment that the tree does not hold', swap('(build/ci/scan.sh)', '(build/ci/no-scan.sh)'),
         (), 'names build/ci/no-scan.sh, which the tree'),
        ('a folder the tree does not hold', GOOD, ('fuzz/README.md',), 'the folder fuzz/'),
        ('a root file the tree does not hold', swap('as SECURITY.md says', 'as RELEASE.md says'), (),
         'names RELEASE.md, which the tree'),
        ('a path in a YAML comment that the tree does not hold', '# See build/no-such-check.py.\n' + GOOD, (),
         'names build/no-such-check.py, which the tree'),
        ('a tab in the indentation', swap('  status: active', '\tstatus: active'), (), 'a tab'),
        ('a key twice', swap('  status: active\n', '  status: active\n  status: active\n'), (), 'twice'),
        ('a plain scalar holding ": "', swap('comment: Releases.', 'comment: Releases: all'), (), 'quote it'),
        ('yes for true', swap('reports-accepted: true', 'reports-accepted: yes'), (), 'YAML 1.1'),
        ('a document marker', '---\n' + GOOD, (), 'document marker'),
    ]
    for what, insights, drop, needle in cases:
        problems = world(insights, drop)
        said = problems[0].replace(INSIGHTS + ': ', '', 1)[:100] if problems else 'nothing said'
        expect('%s: refused (%s)' % (what, said),
               any(needle in problem for problem in problems))
    expect('no SECURITY-INSIGHTS.yml: refused', any('not at the root' in p for p in world(insights=None)))
    expect('a clock a day behind the file: refused', any('in the future' in p for p in world(now=datetime.date(2026, 1, 2))))
    late = today(datetime.datetime(2026, 1, 2, 23, 0, tzinfo=datetime.timezone.utc))
    expect('a UTC clock an hour before midnight, the file dated the next day: passes (today is %s)' % late,
           late == datetime.date(2026, 1, 3) and world(now=late) == [])
    early = today(datetime.datetime(2026, 1, 1, 9, 0, tzinfo=datetime.timezone.utc))
    expect('a UTC clock at 09:00 two days before the review date: refused (today is %s)' % early,
           early == datetime.date(2026, 1, 1) and any('in the future' in p for p in world(now=early)))
    if not quiet:
        print('')
        print('%d self-test case(s) failed' % len(failures) if failures else 'Every self-test case passed.')
    return failures


def main(argv):
    if argv[1:] == ['--self-test']:
        return 1 if self_test() else 0
    if argv[1:]:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    failures = self_test(quiet=True)
    if failures:
        for failure in failures:
            print('FAIL  negative control: %s' % failure)
        print('The check no longer refuses what it should; run it with --self-test.')
        return 1
    problems = check(ROOT)
    if problems:
        for problem in problems:
            print('FAIL  %s' % problem)
        return 1
    with open(os.path.join(ROOT, INSIGHTS), encoding='utf-8') as f:
        document = parse_yaml(f.read())
    links = sum(1 for _, value in strings(document) if TREE_LINK.match(value))
    print('OK    %s: Security Insights %s, updated %s and reviewed %s, %d links into the tree, %d tools'
          % (INSIGHTS, document['header']['schema-version'], document['header']['last-updated'],
             document['header']['last-reviewed'], links, len(document['repository']['security'].get('tools', []))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
