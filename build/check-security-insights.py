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
    midnight passes on a runner that keeps UTC), and last-reviewed is not more
    than a year before that date. As the file's header says, last-updated moves
    with any change and last-reviewed when the whole file has been read against
    the tree again, so a review older than the last update is allowed: a change of
    one line needs no review of the whole file, but the whole file is read again
    at least once a year;
  * to the project: schema-version is 2.2.0, header.url is the file's own raw
    address on GitLab's master, the name says Progressive Robot's fork of
    hMailServer, the repository is the GitLab project and the project's
    repositories name it and the GitHub copy, the licence is AGPL-3.0-or-later
    with LICENSE beside it, exactly one administrator and one core-team member is
    primary, and the vulnerability contact's address is one SECURITY.md gives;
  * to the project's own addresses: every URL anywhere in the file - a value, a
    comment or a YAML comment - that is under gitlab.com/hmailserver/ is the
    project's own, gitlab.com/hmailserver/hmailserver, and every one under
    github.com/RainbowWizard-coder/ is the GitHub copy, RainbowWizard-coder/hMailServer
    there - or is the page of the group, or of its owner on GitHub, itself - so a
    misspelt project path is refused;
  * to the links' form: each URL of the project's own is read as GitLab and
    GitHub read it - its path percent-decoded, so that %20 is a space, its query
    left out, as GitLab's own ?ref_type=heads is, and its fragment the anchor
    (link_kind) - and a link on one of GitLab's routes the file reads (blob, raw,
    tree, blame and releases) written without "/-/", which GitLab keeps as a
    legacy form, is refused;
  * to the tree: every link to a file or folder of the repository - GitLab's
    blob, raw, tree or blame, or the GitHub copy's - is at master, the branch the
    file describes, and names a path the tree holds, each part in the case given;
    and a link to a heading of a Markdown file names a heading that file has, by
    GitLab's (or GitHub's) anchor rule;
  * to the releases: a link under /-/releases/<tag>/downloads/ is to a file
    build/ci/release-chain.py gives a release - its release_files, under the
    name permalink_name gives each for the version, or, under the tag of a
    release before 6.3.6, the first whose every link has its file's permanent
    name, the file's own name as well. The tag is v{version} ({version} being the
    only placeholder a value may hold), permalink/latest, or a version's tag,
    v6.3.4 say, whose names are those of that version. Which of the two names a
    release before 6.3.6 served a file under is not in the tree, so either passes;
    only a read of that release tells (6.3.4's SBOMs kept the version, its
    packages did not);
  * to the tree's paths: every path the file names outside a link - in a value
    (a comment) or in a YAML comment - that has a path's look is one the tree
    holds, each part in the case given, and a folder where it ends in /: a path
    whose first part is an entry at the tree's root, in any case, one that ends
    in / and one whose last part has a file's extension (tree_like); and so is
    every path a tool's rulesets name, whatever its look. Either may be one of
    NOT_TREE instead, the names that are not the tree's: Semgrep's registry
    rulesets, such as p/csharp, and the specification's spec/schema.cue. Any
    other word with a / in it - S/MIME, and/or, amd64/arm64, HTTP/1.1 - is prose,
    and is not held. A name of the tree that holds a space, hmailserver/test/SSL
    examples/ say, is read with the words after it that make one the tree holds.
    A path may be written from "./", which is taken off, and one written from
    "../" is never the tree's. A file named with no folder, whatever its case -
    SECURITY.md, renovate.json, .gitlab-ci.yml - and with an extension of
    ROOT_EXTENSIONS is taken for a file at the root of the tree. So a renamed
    script, ruleset, folder or document fails the check, and so does one named
    under the wrong folder or under none.

Nothing is fetched: a URL of another site, and a page of the project's that is
not a file of the tree or of a release (its releases page, its wiki), is held to
none of this but the project's own address.

It proves its own negative controls on every run, on made-up trees: a file that
breaks each rule above must be refused and the good one must pass, so a check
that has stopped seeing what it reads fails as well. --self-test runs them alone
and says each.

  python build/check-security-insights.py [--self-test]
"""
import datetime
import importlib.util
import os
import re
import shutil
import sys
import tempfile
import urllib.parse

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


# Every URL in the file's text, YAML comments included. The punctuation that ends a
# sentence after one is not part of it (urls).
URL_IN_TEXT = re.compile(r'https?://[^\s<>"\'`]+')
# (the URLs that are the project's to name, the group's or the owner's own page, the
# one project there that is the project's): a URL under the first must be under the
# last, in its case, so that a misspelt project path is refused.
OWN = ((re.compile(r'^https?://gitlab\.com/hmailserver/', re.I), 'https://gitlab.com/hmailserver', GITLAB),
       (re.compile(r'^https?://github\.com/RainbowWizard-coder/', re.I), 'https://github.com/RainbowWizard-coder',
        GITHUB))
# The routes to a file or folder of the repository at a ref: GitLab's, after its "/-/",
# and the GitHub copy's, which GitHub writes without one.
FILE_ROUTES = ('blob', 'raw', 'tree', 'blame')
# GitLab's routes written without "/-/", which GitLab keeps as legacy forms of its
# scoped ones and still answers: refused, so that every link is in the form GitLab
# gives now and none slips past the rules below by its form.
LEGACY_ROUTES = FILE_ROUTES + ('releases',)
# (host, the project's path there, whether its routes are scoped by "/-/").
PROJECT_PATHS = (('gitlab.com', '/hmailserver/hmailserver', True),
                 ('github.com', '/RainbowWizard-coder/hMailServer', False))
TRUSTED_REF = 'master'
# The tag of a link to a release's file: v{version}, permalink/latest or a version's.
VERSION_TAG = re.compile(r'^v([0-9]+(?:\.[0-9]+)*(?:-[0-9A-Za-z.]+)?)$')
# The version whose names stand for every release's, for the links that name none: a
# file's permanent name has no version in it (release-chain.py's permalink_name).
SAMPLE_VERSION = '9.9.9'
# The first release in which every file's link has its permanent name, and no other
# (release-chain.py's permalink_name: 6.3.5's bundles kept the version in their links'
# paths, and 6.3.4's SBOMs theirs). Under the tag of an earlier release a link may name
# a file by its own name as well.
PERMANENT_NAMES_FROM = (6, 3, 6)


def link_kind(url):
    """What a URL of the project's own names, read as GitLab and GitHub read it: its
    path percent-decoded (so %20 is a space), its query left out (GitLab's own
    ?ref_type=heads among them) and its fragment the anchor. One of
      ('tree', ref, path, anchor)   a file or folder of the repository at a ref,
                                    by any of FILE_ROUTES;
      ('release', tag, name)        a file of a release on GitLab;
      ('legacy', route)             a route of LEGACY_ROUTES on GitLab without "/-/";
    or None for any other page, of the project's or of another site."""
    parts = urllib.parse.urlsplit(url)
    host = parts.netloc.lower()
    path = urllib.parse.unquote(parts.path)
    for project_host, project, scoped in PROJECT_PATHS:
        if host != project_host or not path.startswith(project + '/'):
            continue
        segments = path[len(project) + 1:].split('/')
        if scoped:
            if segments[0] in LEGACY_ROUTES:
                return ('legacy', segments[0])
            if segments[0] != '-':
                return None
            segments = segments[1:]
        route, rest = segments[0], segments[1:]
        if route in FILE_ROUTES:
            return ('tree', rest[0] if rest else '', '/'.join(rest[1:]), urllib.parse.unquote(parts.fragment))
        if scoped and route == 'releases':
            match = re.match(r'(.+?)/downloads/(.+)$', '/'.join(rest))
            if match:
                return ('release', match.group(1), match.group(2))
        return None
    return None


def before_permanent_names(version):
    """version, such as 6.3.4 or 6.2.22-pre3, is of a release made before
    PERMANENT_NAMES_FROM."""
    numbers = [int(part) for part in re.match(r'[0-9]+(?:\.[0-9]+)*', version).group(0).split('.')]
    return tuple((numbers + [0, 0, 0])[:3]) < PERMANENT_NAMES_FROM


def urls(text):
    """Each (line number, URL) of text, without the punctuation after it."""
    for number, line in enumerate(text.splitlines(), 1):
        for url in URL_IN_TEXT.findall(line):
            url = url.rstrip('.,;:')
            while url.endswith(')') and url.count('(') < url.count(')'):
                url = url[:-1].rstrip('.,;:')
            yield number, url


def is_under(url, base):
    """url is base itself or a path, query or fragment below it."""
    return url == base or (url.startswith(base) and url[len(base)] in '/?#')


def release_names(root, version, cache):
    """(the permanent names, the files' own names) build/ci/release-chain.py, as the tree
    at root has it, gives the files of the release of version - release_files and
    permalink_name - or None, with cache['error'] saying why, when it cannot be read."""
    if 'module' not in cache:
        path = os.path.join(root, RELEASE_CHAIN)
        try:
            spec = importlib.util.spec_from_file_location('release_chain_for_insights', path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            if not callable(getattr(module, 'release_files', None)) or \
                    not callable(getattr(module, 'permalink_name', None)):
                raise AttributeError('it has no release_files and permalink_name')
            cache['module'] = module
        except Exception as error:  # a tree whose release chain is missing or does not load
            cache['module'] = None
            cache['error'] = '%s: %s' % (type(error).__name__, error)
    module = cache['module']
    if module is None:
        return None
    if version not in cache:
        try:
            files = list(module.release_files(version))
            cache[version] = ({module.permalink_name(name, version) for name in files}, set(files))
        except Exception as error:  # a release chain that cannot name this version's files
            cache['error'] = '%s: %s' % (type(error).__name__, error)
            return None
    return cache[version]


def held(root, relative, listings):
    """The tree at root holds relative, a path whose parts are joined by /, with every
    part in the case given, as Linux's file systems compare names - on Windows and
    macOS too, whose file systems ignore case - and as a folder if it ends in /."""
    folder = relative.endswith('/')
    here = root
    for part in relative.rstrip('/').split('/'):
        if part in ('', '.', '..'):
            return False
        if here not in listings:
            try:
                listings[here] = set(os.listdir(here))
            except OSError:
                listings[here] = set()
        if part not in listings[here]:
            return False
        here = os.path.join(here, part)
    return os.path.isdir(here) if folder else True


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


def a_year_before(day):
    """The same date a year earlier; 28 February for a 29 February."""
    try:
        return day.replace(year=day.year - 1)
    except ValueError:
        return day.replace(year=day.year - 1, day=28)


# A path of the tree named outside a link: segments of letters, digits, _ and -, the
# first of which may start with a dot, joined by /, the last optionally a file name,
# and the whole written from the top of the tree or from "./" or "../" (which no path
# of the tree has: "./" is taken off, and a ".." is never held). It must not follow a
# character that would make it part of a longer word, a host name or a URL (a letter,
# a digit, ., /, : or -).
TREE_PATH = re.compile(r'(?<![\w./:-])((?:\.{1,2}/)*(?:\.?[A-Za-z0-9_-]+/)+[A-Za-z0-9_.-]*)')
# A file named with no folder, in any case, whose extension is one of these: a file at
# the root, such as SECURITY.md, renovate.json or .gitlab-ci.yml. It must not follow a
# character that would make it part of a path, a URL, a placeholder or a longer name,
# nor run on into one.
ROOT_EXTENSIONS = ('md', 'yml', 'yaml', 'txt', 'json', 'toml', 'cue', 'py', 'sh', 'ps1', 'cfg', 'ini')
ROOT_FILE = re.compile(r'(?<![\w./:<>{}@~-])(\.?[A-Za-z0-9_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_-]+)*\.(?:' +
                       '|'.join(ROOT_EXTENSIONS) + r'))(?!\.?[\w/-])')
# The names the file gives that are held to the tree (tree_like, or a tool's ruleset)
# and are not the tree's: Semgrep's registry rulesets, which the semgrep job reads from
# Semgrep's registry, and the Security Insights specification's own schema file. Each
# is named here, so that every other such path is held to the tree.
NOT_TREE = ('p/csharp', 'p/javascript', 'p/python', 'r/bash', 'spec/schema.cue')
# The last part of a path that names a file: a name with an extension, which starts
# with a letter (HTTP/1.1 and TLS/1.3 name none).
FILE_EXTENSION = re.compile(r'\.[A-Za-z][A-Za-z0-9]*$')
# A word that may go on after a path whose next part's name holds a space, such as
# hmailserver/test/SSL examples/: what a path's parts are made of; and how many spaces
# such a name may hold.
PATH_WORD = re.compile(r' ([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]*)*)')
MOST_SPACES = 3


def tidy(token):
    """A path as read from the text: the full stop of a sentence after it, and any "./"
    before it, taken off."""
    token = token.rstrip('.')
    while token.startswith('./'):
        token = token[2:]
    return token


def tree_like(token, roots):
    """token, a path TREE_PATH reads, is to be held to the tree: its first part is an
    entry at the tree's root (roots, in lower case, so that one in another case is held
    and refused), it ends in / (it names a folder), or its last part has a file's
    extension. Any other word with a / in it - S/MIME, and/or, amd64/arm64 - is prose."""
    parts = token.split('/')
    return parts[0].lower() in roots or token.endswith('/') or FILE_EXTENSION.search(parts[-1]) is not None


def named_paths(text, roots, holds, every=False):
    """Each (line number, path) that text names outside a URL and that is held to the
    tree: each path TREE_PATH reads that tree_like takes - or, with every, each one it
    reads, as a tool's rulesets are held - and each file named with no folder that
    ROOT_FILE reads, but those of NOT_TREE. Every URL is blanked out first, so that no
    part of one is taken for a path. A path the tree does not hold, whose next words
    make one it does - a folder whose name holds a space - is taken with them."""
    for number, line in enumerate(text.splitlines(), 1):
        line = URL_IN_TEXT.sub(lambda url: ' ' * len(url.group(0)), line)
        taken_to = 0
        for match in TREE_PATH.finditer(line):
            if match.start() < taken_to:
                continue
            token = tidy(match.group(1))
            if token in NOT_TREE or not (every or tree_like(token, roots)):
                continue
            if not holds(token):
                end, longer = match.end(1), token
                for _ in range(MOST_SPACES):
                    word = PATH_WORD.match(line, end)
                    if not word:
                        break
                    end, longer = word.end(1), longer + ' ' + word.group(1)
                    if holds(tidy(longer)):
                        token, taken_to = tidy(longer), end
                        break
            yield number, token
        for token in ROOT_FILE.findall(line):
            token = tidy(token)
            if token not in NOT_TREE:
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
    if reviewed < a_year_before(now):
        problems.append('header.last-reviewed is %s, more than a year before today (%s): read the whole file '
                        'against the tree again, then move last-reviewed' % (reviewed, now))
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
    named = [entry['url'] for entry in project['repositories']]
    for wanted, what in ((GITLAB, 'the project on GitLab'), (GITHUB, 'its copy on GitHub')):
        if wanted not in named:
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

    for where, value in strings(document):
        for placeholder in re.findall(r'\{[^}]*\}', value):
            if placeholder != '{version}':
                problems.append('%s holds the placeholder %s; {version} is the only one' % (where, placeholder))

    chain_names = {}
    listings = {}
    headings = {}
    seen = set()
    for number, url in urls(text):
        if url in seen:
            continue
        seen.add(url)
        mistaken = [own for prefix, page, own in OWN
                    if prefix.match(url) and url.rstrip('/') != page and not is_under(url, own)]
        if mistaken:
            problems.append('line %d: %s is not the project\'s own address, %s' % (number, url, mistaken[0]))
            continue
        kind = link_kind(url)
        if kind is None:
            continue
        if kind[0] == 'legacy':
            problems.append('line %d: %s is on GitLab\'s legacy route %s/, without "/-/": write %s/-/%s/...'
                            % (number, url, kind[1], GITLAB, kind[1]))
            continue
        if kind[0] == 'release':
            tag, name = kind[1:]
            version = VERSION_TAG.match(tag)
            if tag not in ('v{version}', 'permalink/latest') and not version:
                problems.append('line %d: %s is a release link of a tag this check does not know, %s: it takes '
                                'v{version}, permalink/latest and a version\'s tag' % (number, url, tag))
                continue
            names = release_names(root, version.group(1) if version else SAMPLE_VERSION, chain_names)
            if names is None:
                problems.append('line %d: %s names a release file, and %s cannot be read for the names it gives '
                                'them (%s)' % (number, url, RELEASE_CHAIN.replace(os.sep, '/'), chain_names['error']))
                continue
            permanent, own_names = names
            own_taken = version is not None and before_permanent_names(version.group(1))
            if name not in permanent and not (own_taken and name in own_names):
                problems.append('line %d names the release file %s, which %s gives no file of %s%s'
                                % (number, name, RELEASE_CHAIN.replace(os.sep, '/'),
                                   'release %s' % version.group(1) if version else 'a release',
                                   ' (from %s every link has the file\'s permanent name)'
                                   % '.'.join(map(str, PERMANENT_NAMES_FROM)) if name in own_names else ''))
            continue
        ref, relative, anchor = kind[1:]
        if ref != TRUSTED_REF:
            problems.append('line %d: %s links to the tree at %s, not %s, the branch this file describes'
                            % (number, url, ref, TRUSTED_REF))
            continue
        relative = relative.rstrip('/')
        if relative and not held(root, relative, listings):
            problems.append('line %d links to %s, which the tree does not hold' % (number, relative))
            continue
        if anchor:
            target = os.path.join(root, *relative.split('/'))
            if not relative.lower().endswith('.md') or not os.path.isfile(target):
                problems.append('line %d links to a heading of %s, which is not a Markdown file' % (number, relative))
                continue
            if relative not in headings:
                with open(target, encoding='utf-8') as f:
                    headings[relative] = anchors(f.read())
            if anchor not in headings[relative]:
                problems.append('line %d links to #%s, a heading %s does not have' % (number, anchor, relative))

    roots = root_entries(root)

    def holds(relative):
        return held(root, relative, listings)

    seen = set()
    for number, relative in named_paths(text, roots, holds):
        if relative in seen:
            continue
        seen.add(relative)
        if not holds(relative):
            problems.append('line %d names %s%s, which the tree does not hold, in that case (a name that is not the '
                            'tree\'s goes in NOT_TREE)' % (number, 'the folder ' if relative.endswith('/') else '',
                                                           relative))
    # A tool's rulesets are held whatever their look: a ruleset is a file of the tree
    # or one of NOT_TREE's, Semgrep's registry rulesets among them.
    for index, tool in enumerate(repository['security'].get('tools') or []):
        for number, ruleset in enumerate(tool['rulesets']):
            for _, relative in named_paths(ruleset, roots, holds, every=True):
                if relative in seen:
                    continue
                seen.add(relative)
                if not holds(relative):
                    problems.append('repository.security.tools[%d].rulesets[%d] names %s, which the tree does not '
                                    'hold, in that case (a ruleset that is not the tree\'s goes in NOT_TREE)'
                                    % (index, number, relative))
    return ['%s: %s' % (INSIGHTS, problem) for problem in problems]


def root_entries(root):
    """The names of the entries at the tree's root, in lower case."""
    try:
        return {name.lower() for name in os.listdir(root)}
    except OSError:
        return set()


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
    design: https://gitlab.com/hmailserver/hmailserver/-/blob/master/GOVERNANCE.md?ref_type=heads
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
          and https://example.org/build/ci/gone.sh name no path of the tree, nor does spec/schema.cue.
          renovate.json updates it. 1.2.3's SBOM is
          https://gitlab.com/hmailserver/hmailserver/-/releases/v1.2.3/downloads/hMailServer-1.2.3-x64.exe.cdx.json,
          its installer https://gitlab.com/hmailserver/hmailserver/-/releases/v1.2.3/downloads/hMailServer-x64.exe
          (https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/hMailServer-x64.exe
          for the latest), on the group's page https://gitlab.com/hmailserver/. 6.3.7's SBOM is
          https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.7/downloads/hMailServer-x64.exe.cdx.json,
          the job's blame https://gitlab.com/hmailserver/hmailserver/-/blame/master/build/ci/scan.sh, its
          rules (./build/ci/rules.yaml) and the keys
          https://gitlab.com/hmailserver/hmailserver/-/tree/master/test/SSL%20examples/.
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
    'renovate.json': '{}\n',
    'test/SSL examples/example.pfx': 'not a key\n',
    # A release chain of three files, whose permanent names lose the version.
    RELEASE_CHAIN: 'def release_files(version):\n'
                   '    return ["hmailserver-%s.intoto.sigstore.json" % version, "hMailServer-%s-x64.exe" % version,\n'
                   '            "hMailServer-%s-x64.exe.cdx.json" % version]\n'
                   '\n\n'
                   'def permalink_name(name, version):\n'
                   '    return name.replace("-%s" % version, "", 1)\n',
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
        ('reviewed more than a year before today', swap('last-reviewed: "2026-01-03"', "last-reviewed: '2025-01-02'"),
         (), 'more than a year before today'),
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
        ('a path under a folder the tree does not have', swap('(build/ci/scan.sh)', '(tools/scan.sh)'), (),
         'names tools/scan.sh, which the tree'),
        ('a folder named in another case', swap('(build/ci/scan.sh)', '(Build/ci/scan.sh)'), (),
         'names Build/ci/scan.sh, which the tree'),
        ('a file at the root, in lower case, that the tree does not hold', swap('(build/ci/scan.sh)', '(scan.sh)'), (),
         'names scan.sh, which the tree'),
        ('a root file named in another case', swap('as SECURITY.md says', 'as security.md says'), (),
         'names security.md, which the tree'),
        ('a registry ruleset that NOT_TREE does not name', swap('- p/csharp', '- p/csharpx'), (),
         'names p/csharpx, which the tree'),
        ('a linked file named in another case', swap('/-/blob/master/LICENSE', '/-/blob/master/License'), (),
         'links to License, which the tree'),
        ('a link to the tree at another branch', swap('/-/blob/master/LICENSE', '/-/blob/main/LICENSE'), (),
         'at main, not master'),
        ('a link to the GitHub copy at another ref',
         swap('hMailServer/blob/master/SECURITY.md', 'hMailServer/blob/v1/SECURITY.md'), (), 'at v1, not master'),
        ('a misspelt project path on GitLab',
         swap('https://gitlab.com/hmailserver/hmailserver/-/blob/master/ASSURANCE-CASE.md',
              'https://gitlab.com/hmailserver/hmailserve/-/blob/master/ASSURANCE-CASE.md'), (),
         "hmailserve/-/blob/master/ASSURANCE-CASE.md is not the project's own address"),
        ('the project path on GitLab in another case', swap('homepage: https://gitlab.com/hmailserver/hmailserver',
                                                            'homepage: https://gitlab.com/hmailserver/HMailServer'), (),
         "HMailServer is not the project's own address"),
        ('another repository on GitHub',
         swap('https://github.com/RainbowWizard-coder/hMailServer/blob/master/SECURITY.md',
              'https://github.com/RainbowWizard-coder/hMailServer-old/blob/master/SECURITY.md'), (),
         "hMailServer-old/blob/master/SECURITY.md#verifying-a-release is not the project's own address"),
        ('a release file of a version the release chain gives no file',
         swap('hMailServer-1.2.3-x64.exe.cdx.json', 'hMailServer-1.2.3-x65.exe.cdx.json'), (),
         'names the release file hMailServer-1.2.3-x65.exe.cdx.json, which build/ci/release-chain.py gives no file '
         'of release 1.2.3'),
        ('a permanent name under permalink/latest the release chain does not give',
         swap('permalink/latest/downloads/hMailServer-x64.exe', 'permalink/latest/downloads/hMailServer-x65.exe'), (),
         'names the release file hMailServer-x65.exe'),
        ('a release link of a tag that is not a version',
         swap('/-/releases/v1.2.3/downloads/hMailServer-x64.exe', '/-/releases/latest/downloads/hMailServer-x64.exe'),
         (), 'a tag this check does not know, latest'),
        ('release links and no release chain to read their names from', GOOD, (RELEASE_CHAIN,),
         'cannot be read for the names'),
        ('a link with a query, to a file the tree does not hold',
         swap('GOVERNANCE.md?ref_type=heads', 'GOVERNANCES.md?ref_type=heads'), (),
         'links to GOVERNANCES.md, which the tree'),
        ('a link to the tree on the legacy route without /-/',
         swap('/hmailserver/-/blob/master/LICENSE', '/hmailserver/blob/master/LICENSE'), (), 'legacy route blob/'),
        ('a link to a release on the legacy route without /-/',
         swap('/hmailserver/-/releases/v1.2.3/downloads/hMailServer-x64.exe',
              '/hmailserver/releases/v1.2.3/downloads/hMailServer-x64.exe'), (), 'legacy route releases/'),
        ('a blame link at another branch', swap('/-/blame/master/build/ci/scan.sh', '/-/blame/main/build/ci/scan.sh'),
         (), 'at main, not master'),
        ('a blame link to a file the tree does not hold',
         swap('/-/blame/master/build/ci/scan.sh', '/-/blame/master/build/ci/scan.ps1'), (),
         'links to build/ci/scan.ps1, which the tree'),
        ('a release link with a query, of a name the release chain does not give',
         swap('hMailServer-1.2.3-x64.exe.cdx.json,', 'hMailServer-1.2.3-x65.exe.cdx.json?x=1,'), (),
         'names the release file hMailServer-1.2.3-x65.exe.cdx.json'),
        ("a file's own name under the tag of a release from 6.3.6, which named every file by its permanent name",
         swap('v6.3.7/downloads/hMailServer-x64.exe.cdx.json', 'v6.3.7/downloads/hMailServer-6.3.7-x64.exe.cdx.json'),
         (), 'names the release file hMailServer-6.3.7-x64.exe.cdx.json'),
        ('a path written from ./ that the tree does not hold', swap('(./build/ci/rules.yaml)', '(./build/ci/rule.yaml)'),
         (), 'names build/ci/rule.yaml, which the tree'),
        ('a path written from ../', swap('(./build/ci/rules.yaml)', '(../build/ci/rules.yaml)'), (),
         'names ../build/ci/rules.yaml, which the tree'),
        ('a link with an encoded space to a folder the tree does not hold', swap('SSL%20examples/', 'SSL%20exemples/'),
         (), 'links to test/SSL exemples, which the tree'),
        ('a folder with a space in its name that the tree does not hold',
         swap('renovate.json updates it.', 'renovate.json updates it; test/SSL exemples/ holds its keys.'), (),
         'names test/SSL, which the tree'),
        ('a path under a folder at the root, with no / at its end and no extension, that the tree does not hold',
         swap('(build/ci/scan.sh)', '(build/scripts)'), (), 'names build/scripts, which the tree'),
        ('a word with a / that ends in one, a folder the tree does not hold',
         swap('renovate.json updates it.', 'renovate.json updates it under fuzzers/.'), (),
         'names the folder fuzzers/, which the tree'),
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
    later = swap("last-updated: '2026-01-02'", "last-updated: '2026-01-03'").replace(
        'last-reviewed: "2026-01-03"', "last-reviewed: '2026-01-02'")
    said = world(later)
    expect('reviewed before it was updated, as the header allows for a change: passes%s'
           % ('' if not said else ' (%s)' % said[0][:120]), said == [])
    said = world(swap('last-reviewed: "2026-01-03"', "last-reviewed: '2025-01-03'"))
    expect('reviewed a year to the day before today: passes%s' % ('' if not said else ' (%s)' % said[0][:120]),
           said == [])
    for prose in ('for S/MIME', 'and/or by hand', 'on amd64/arm64', 'over HTTP/1.1'):
        said = world(swap('renovate.json updates it.', 'renovate.json updates it %s.' % prose))
        expect('prose with a / in a word, "%s", which is no path of the tree: passes%s'
               % (prose, '' if not said else ' (%s)' % said[0][:120]), said == [])
    for named in ('test/SSL examples/', 'test/SSL examples/example.pfx'):
        said = world(swap('renovate.json updates it.', 'renovate.json updates it; %s holds its keys.' % named))
        expect('a folder of the tree with a space in its name, %s: passes%s'
               % (named, '' if not said else ' (%s)' % said[0][:120]), said == [])
    said = world(swap('/-/tree/master/test/SSL%20examples/', '/-/blob/master/test/SSL%20examples/example.pfx'))
    expect('a link with an encoded space to a file the tree holds: passes%s'
           % ('' if not said else ' (%s)' % said[0][:120]), said == [])
    expect('a link read as GitLab reads it: %20 a space, the query left out, the fragment the anchor',
           link_kind(GITLAB + '/-/blob/master/test/SSL%20examples/example.pfx?ref_type=heads#top')
           == ('tree', 'master', 'test/SSL examples/example.pfx', 'top'))
    expect("a file's own name taken under the tag of a release before 6.3.6 alone",
           before_permanent_names('6.3.4') and before_permanent_names('6.2.22-pre3')
           and not before_permanent_names('6.3.6') and not before_permanent_names('6.3.7')
           and not before_permanent_names('10'))
    expect('a year before 29 February is 28 February',
           a_year_before(datetime.date(2028, 2, 29)) == datetime.date(2027, 2, 28)
           and a_year_before(datetime.date(2026, 1, 3)) == datetime.date(2025, 1, 3))
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
        text = f.read()
    document = parse_yaml(text)
    kinds = [link_kind(url) for url in {url for _, url in urls(text)}]
    links = sum(1 for kind in kinds if kind and kind[0] == 'tree')
    releases = sum(1 for kind in kinds if kind and kind[0] == 'release')
    listings = {}
    paths = len({path for _, path in named_paths(text, root_entries(ROOT),
                                                 lambda relative: held(ROOT, relative, listings))})
    print('OK    %s: Security Insights %s, updated %s and reviewed %s, %d links into the tree, %d to release files, '
          '%d paths of the tree named, %d tools'
          % (INSIGHTS, document['header']['schema-version'], document['header']['last-updated'],
             document['header']['last-reviewed'], links, releases, paths,
             len(document['repository']['security'].get('tools', []))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
