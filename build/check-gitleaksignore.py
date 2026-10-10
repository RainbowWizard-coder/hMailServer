#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Each directory-form line of .gitleaksignore still names the finding it was written for.

gitleaks' directory scan - secret_detection in a pipeline that brings no range of
commits, one started through the API among them - fingerprints a finding as
<path>:<rule>:<line>, the path under the job's checkout, /builds/hmailserver/hmailserver/,
and .gitleaksignore names each such finding that was read and found not to be a secret
in that form, beside its commit form (<commit>:<path>:<rule>:<line>, which names a line
of one commit and moves with no edit). A line number is all that ties the directory form
to its finding: an edit above the line moves the finding, the entry then names a line
with nothing on it, and the next directory scan fails on the finding, unnamed - in a
pipeline after the change that moved it, not in the change's own. Four had moved by 10
October 2026: DkimRotationPlan.cpp 729 to 728, ObjectStoreSignature.cpp 529 to 527,
SamlSignIn.cs 723 to 751 and SURBL.cpp 1886 to 2625.

For every path and rule the directory-form entries name, this finds the lines of the
tree's file at which that rule's pattern matches, as gitleaks matches it - over the
whole file, a match's line the one it starts on - and fails when an entry names a line
that is not one of them (moved, or gone), or one of them has no entry (a finding the
directory scan would report: where an entry moved to), saying which. It fails, too, on
an entry under another directory than the checkout's, on a path the tree does not have,
and on a rule it has no pattern for. The patterns are those of GitLab's rules the
entries name (RULES), as secret_detection's analyzer has them; a new rule in an entry
needs its pattern here, from the analyzer's /gitleaks.toml. A line marked gitleaks:allow
is no finding. The commit-form entries are not read.

The patterns are a copy, read from one analyzer's rules (RULES_FROM, its version): it
fails, too, when .gitlab/ci/gitlab-security.yml has secret_detection run another version,
whose rules may write a pattern otherwise - the entries would then be judged by a pattern
the scan no longer has - until the patterns are read again from that image's
/gitleaks.toml and RULES_FROM set to its version.

  python build/check-gitleaksignore.py              the tree's .gitleaksignore
  python build/check-gitleaksignore.py --self-test  against fixtures

Exit codes: 0 every directory-form entry names its finding; 1 one does not, a finding of
a named file and rule has no entry, or secret_detection runs another analyzer than the
one RULES were read from; 2 the file could not be read.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import argparse
import re
import shutil
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IGNORE = ".gitleaksignore"
SECURITY_YML = ".gitlab/ci/gitlab-security.yml"
# Where the secret_detection job's checkout is: CI_PROJECT_DIR at the group's path.
CHECKOUT = "/builds/hmailserver/hmailserver/"
# The patterns of the rules the directory-form entries name, as GitLab's rules
# (gitlab-org/security-products/secret-detection/secret-detection-rules 0.25.1, MIT
# Expat licence, Copyright (c) 2011-present GitLab B.V.) write them in the analyzer's
# /gitleaks.toml. None of them has an entropy bound, a keyword the match does not hold,
# or an allowlist of its own there, so each match is a finding.
RULES = {
    "AWS": r"\b(?:AKIA|ASIA|A3T[A-Z0-9])[2-7A-Z]{16}\b",
    "Password in URL": (r"[a-zA-Z][a-zA-Z0-9+.-]{1,40}:\/\/[^:@\/ \t]{0,20}:[^$%&{[(\"'\x60+:@\/\\ \t<][^ {(]"
                        r"[A-Za-z0-9_\-:.!#$%*~&=?()^{}|,;<>+]{2,256}[^:@\/ \t]@(?:[0-9]{1,3}(?:\.[0-9]{1,3}){3}|"
                        r"\[[0-9a-fA-F:]+\]|(?:[a-zA-Z0-9-]{1,63}\.){0,5}[a-zA-Z0-9-]{1,63})"),
    # Each armour header in two literals, so that this file is no finding of the rule
    # whose pattern it holds: both secret scans read build/.
    "PGP private key": r"-----BEGIN PGP " r"PRIVATE KEY BLOCK-----",
    "PKCS8 private key": r"-----BEGIN " r"PRIVATE KEY-----",
    "RSA private key": r"-----BEGIN RSA " r"PRIVATE KEY-----",
    "SSH private key": r"-----BEGIN OPENSSH " r"PRIVATE KEY-----",
    "SSH (EC) private key": r"-----BEGIN EC " r"PRIVATE KEY-----",
}
# The analyzer whose /gitleaks.toml RULES were read from: GitLab's secrets analyzer 7.40.0
# (rules 0.25.1), as the project's registry holds it for secret_detection. A job that
# runs another version fails this check until RULES are read again from that version's
# file (ContinuousIntegration.md, "Re-pinning secret_detection's analyzer").
RULES_FROM = "7.40.0"
# The secret_detection job's image line, the analyzer's version in its tag: the project's
# copy (mirror/secrets:<version>-<the digest's first twelve hex digits>), or GitLab's own
# (security-products/secrets:<version>), by digest.
ANALYZER = re.compile(r"^  image: \S*/secrets:([0-9]+(?:\.[0-9]+)*)(?:-[0-9a-f]{12})?@sha256:[0-9a-f]{64}[ \t]*\r?$",
                      re.MULTILINE)
COMMIT_FORM = re.compile(r"^[0-9a-f]{40}:")
ENTRY = re.compile(r"^(.+):([^:]+):([0-9]+)$")


def entries(text):
    """[(line of the ignore file, path, rule, line)] of its directory-form entries; the
    malformed ones as (line, None, the text, None)."""
    found = []
    for number, raw in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#") or COMMIT_FORM.match(line):
            continue
        match = ENTRY.match(line)
        if not match:
            found.append((number, None, line, None))
            continue
        found.append((number, match.group(1), match.group(2), int(match.group(3))))
    return found


def finding_lines(text, rule):
    """The lines (1-based) at which rule's pattern matches text, each match at the line it
    starts on, as gitleaks reports it; a line marked gitleaks:allow is left out."""
    lines = text.split("\n")
    starts = set()
    for match in re.finditer(RULES[rule], text):
        number = text.count("\n", 0, match.start()) + 1
        if "gitleaks:allow" not in lines[number - 1]:
            starts.add(number)
    return starts


def problems_of(root, text):
    problems = []
    named = {}
    for where, path, rule, line in entries(text):
        if path is None:
            problems.append("%s:%d: %r is neither a commit-form nor a directory-form entry (<path>:<rule>:<line>)"
                            % (IGNORE, where, rule))
        elif not path.startswith(CHECKOUT):
            problems.append("%s:%d: %s is not under the job's checkout, %s, so no scan's finding has it: a directory-form "
                            "entry names %s<path>" % (IGNORE, where, path, CHECKOUT, CHECKOUT))
        elif rule not in RULES:
            problems.append("%s:%d: the rule %r has no pattern here (build/check-gitleaksignore.py, RULES): add it from the "
                            "analyzer's /gitleaks.toml" % (IGNORE, where, rule))
        else:
            named.setdefault((path[len(CHECKOUT):], rule), []).append((line, where))
    texts = {}
    for (path, rule), lines in sorted(named.items()):
        full = os.path.join(root, *path.split("/"))
        if path not in texts:
            try:
                with open(full, "rb") as f:
                    texts[path] = f.read().decode("utf-8", "replace").replace("\r\n", "\n")
            except OSError:
                texts[path] = None
        if texts[path] is None:
            problems.append("%s:%d: %s is not in the tree: the entry names nothing" % (IGNORE, lines[0][1], path))
            continue
        found = finding_lines(texts[path], rule)
        listed = set(line for line, _ in lines)
        stale = sorted((line, where) for line, where in lines if line not in found)
        unnamed = sorted(found - listed)
        moved_to = (" - the file's unnamed finding(s) of that rule are at line(s) %s" % ", ".join(map(str, unnamed))
                    if unnamed else "")
        for line, where in stale:
            problems.append("%s:%d: %s line %d holds no finding of the rule %s: the entry has moved or is gone%s"
                            % (IGNORE, where, path, line, rule, moved_to))
        for line in unnamed:
            problems.append("%s: %s line %d is a finding of the rule %s that no directory-form entry names, which the "
                            "directory scan would report: add %s%s:%s:%d, or move the entry that named it before an edit "
                            "above it" % (IGNORE, path, line, rule, CHECKOUT, path, rule, line))
    return problems, sum(len(v) for v in named.values()), len(named)


def analyzer_problems(root):
    """What keeps RULES from being the patterns secret_detection's analyzer has: a job that
    runs another version than RULES_FROM, or that names no version, or two."""
    try:
        with open(os.path.join(root, *SECURITY_YML.split("/")), "rb") as f:
            text = f.read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        return ["%s cannot be read, so the analyzer secret_detection runs, whose rules the patterns here must be, is not "
                "known: %s" % (SECURITY_YML, e)]
    versions = sorted(set(ANALYZER.findall(text)))
    if len(versions) != 1:
        return ["%s names %d secret_detection analyzer version(s) on an image line pinned by digest (%s), not one: the "
                "patterns here (RULES) are those of the analyzer %s" % (SECURITY_YML, len(versions), ", ".join(versions),
                                                                        RULES_FROM)]
    version = versions[0]
    if version != RULES_FROM:
        return ["secret_detection runs the analyzer %s (%s), and the patterns here (RULES) were read from %s's "
                "/gitleaks.toml: read the patterns of the rules the entries name again from %s's, and set RULES_FROM to "
                "%s" % (version, SECURITY_YML, RULES_FROM, version, version)]
    return []


def check(root=ROOT, say=print):
    try:
        with open(os.path.join(root, IGNORE), "rb") as f:
            text = f.read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        say("check-gitleaksignore: cannot read %s: %s" % (IGNORE, e))
        return 2
    problems, count, groups = problems_of(root, text)
    problems = analyzer_problems(root) + problems
    for problem in problems:
        say("FAIL: " + problem)
    if problems:
        say("check-gitleaksignore: %d problem(s) in %s's directory-form entries" % (len(problems), IGNORE))
        return 1
    say("check-gitleaksignore: %d directory-form entr%s of %s, over %d file and rule pair(s), each name a finding, and "
        "every finding of those files and rules is named, by the patterns of the analyzer %s, secret_detection's"
        % (count, "y" if count == 1 else "ies", IGNORE, groups, RULES_FROM))
    return 0


def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    work = tempfile.mkdtemp(prefix="check-gitleaksignore-")
    try:
        def job(version):
            return ("semgrep-sast:\n  image:\n    name: registry.example/semgrep@sha256:%s\n\nsecret_detection:\n"
                    "  # the comment before the line\n  image: registry.gitlab.com/hmailserver/hmailserver/mirror/secrets:"
                    "%s-%s@sha256:%s\n  needs: []\n" % ("1" * 64, version, "2" * 12, "2" * 64))

        def tree(files, ignore, security=None):
            files = dict(files)
            files[SECURITY_YML] = job(RULES_FROM) if security is None else security
            for name, text in files.items():
                full = os.path.join(work, *name.split("/"))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
            with open(os.path.join(work, IGNORE), "w", encoding="utf-8", newline="") as f:
                f.write(ignore)
            said = []
            return check(work, said.append), "\n".join(said)

        pkcs8 = "-----BEGIN " + "PRIVATE KEY-----"
        pgp = "-----BEGIN PGP " + "PRIVATE KEY BLOCK-----"
        source = "a\nb\nconst char *k = \"%s\\n\";\nc\nd\nconst char *p = \"%s\";\n" % (pkcs8, pgp)
        good = ("# a comment\n%s:src/k.cpp:PKCS8 private key:3\n" % ("1" * 40) +
                "%ssrc/k.cpp:PKCS8 private key:3\n%ssrc/k.cpp:PGP private key:6\n" % (CHECKOUT, CHECKOUT))
        code, text = tree({"src/k.cpp": source}, good)
        expect(code == 0 and "2 directory-form entries" in text, "entries that name their findings pass: %s %s" % (code, text))
        code, text = tree({"src/k.cpp": source.replace("\n", "\r\n")}, good.replace("\n", "\r\n"))
        expect(code == 0, "a file and an ignore file checked out CRLF count lines alike: %s %s" % (code, text))
        # An edit above the findings: both entries now name the wrong lines, and the
        # findings are named nowhere.
        code, text = tree({"src/k.cpp": "new line\n" + source}, good)
        expect(code == 1 and "src/k.cpp line 3 holds no finding of the rule PKCS8 private key" in text
               and "unnamed finding(s) of that rule are at line(s) 4" in text
               and "src/k.cpp line 7 is a finding of the rule PGP private key that no directory-form entry names" in text,
               "an entry an edit moved fails, saying where the finding is: %s" % text)
        # A finding of a named file and rule that no entry names.
        code, text = tree({"src/k.cpp": source + "x = \"%s\"\n" % pkcs8}, good)
        expect(code == 1 and "src/k.cpp line 7 is a finding of the rule PKCS8 private key that no directory-form entry names" in text,
               "a second finding of a named file and rule, unnamed, fails: %s" % text)
        code, text = tree({"src/k.cpp": source + "x = \"%s\" // gitleaks:allow\n" % pkcs8}, good)
        expect(code == 0, "a line marked gitleaks:allow is no finding: %s" % text)
        # The entry's finding gone with the line.
        code, text = tree({"src/k.cpp": source.replace(pkcs8, "removed")}, good)
        expect(code == 1 and "line 3 holds no finding of the rule PKCS8 private key: the entry has moved or is gone" in text,
               "an entry whose finding is gone fails: %s" % text)
        code, text = tree({"src/k.cpp": source}, good + "%ssrc/gone.cpp:RSA private key:1\n" % CHECKOUT)
        expect(code == 1 and "src/gone.cpp is not in the tree" in text, "an entry for a path the tree lacks fails: %s" % text)
        code, text = tree({"src/k.cpp": source}, good + "/builds/progressiverobot/hmailserver/src/k.cpp:PKCS8 private key:3\n")
        expect(code == 1 and "is not under the job's checkout" in text, "an entry under another checkout fails: %s" % text)
        code, text = tree({"src/k.cpp": source}, good + "%ssrc/k.cpp:Some new rule:3\n" % CHECKOUT)
        expect(code == 1 and "has no pattern here" in text, "a rule with no pattern fails: %s" % text)
        code, text = tree({"src/k.cpp": source}, good + "not an entry at all\n")
        expect(code == 1 and "neither a commit-form nor a directory-form entry" in text, "a malformed line fails: %s" % text)
        # The commit form is not read: one naming a line of nothing passes.
        code, text = tree({"src/k.cpp": source}, good + "%s:src/k.cpp:PKCS8 private key:99\n" % ("2" * 40))
        expect(code == 0, "a commit-form entry is not read: %s" % text)
        # A match is at the line it starts on, over the whole file, as gitleaks has it.
        url = "s = \"https://user:" + "Secr3tPassw0rd@example.org/x\"\n"
        code, text = tree({"src/u.cs": "a\n" + url}, "%ssrc/u.cs:Password in URL:2\n" % CHECKOUT)
        expect(code == 0, "a password in a URL, at its line: %s" % text)
        aws = "id = \"AKIA" + "IOSFODNN7EXAMPLQ\"\n"
        code, text = tree({"src/a.cpp": aws}, "%ssrc/a.cpp:AWS:1\n" % CHECKOUT)
        expect(code == 0, "an AWS key id, at its line: %s" % text)
        code, text = tree({"src/a.cpp": "\n" + aws}, "%ssrc/a.cpp:AWS:1\n" % CHECKOUT)
        expect(code == 1, "an AWS key id moved a line down: %s" % text)
        # secret_detection's analyzer: the one RULES were read from passes, in the project's
        # copy or GitLab's own, CRLF or LF; another version, none, or two fail, saying what
        # to read again.
        code, text = tree({"src/k.cpp": source}, good)
        expect(code == 0 and "the analyzer %s, secret_detection's" % RULES_FROM in text,
               "the analyzer the patterns were read from passes, and is named: %s" % text)
        code, text = tree({"src/k.cpp": source}, good, job(RULES_FROM).replace("\n", "\r\n"))
        expect(code == 0, "a CI file checked out CRLF is read the same: %s" % text)
        gitlabs = "secret_detection:\n  image: registry.gitlab.com/security-products/secrets:%s@sha256:%s\n" % (RULES_FROM, "3" * 64)
        code, text = tree({"src/k.cpp": source}, good, gitlabs)
        expect(code == 0, "GitLab's own image of that version passes: %s" % text)
        moved = ".".join(RULES_FROM.split(".")[:-1] + [str(int(RULES_FROM.split(".")[-1]) + 1)])
        code, text = tree({"src/k.cpp": source}, good, job(moved))
        expect(code == 1 and "secret_detection runs the analyzer %s" % moved in text and "set RULES_FROM to %s" % moved in text,
               "another analyzer version fails, saying to read the patterns again: %s" % text)
        code, text = tree({"src/k.cpp": source}, good, job(RULES_FROM).replace("@sha256:" + "2" * 64, ""))
        expect(code == 1 and "names 0 secret_detection analyzer version(s)" in text,
               "an image line not pinned by digest names no analyzer: %s" % text)
        code, text = tree({"src/k.cpp": source}, good, job(RULES_FROM) + job(moved).split("\n\n", 1)[1])
        expect(code == 1 and "names 2 secret_detection analyzer version(s)" in text, "two analyzers fail: %s" % text)
        os.remove(os.path.join(work, *SECURITY_YML.split("/")))
        said = []
        code = check(work, said.append)
        expect(code == 1 and "cannot be read" in "\n".join(said), "no CI file fails: %s %s" % (code, said))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # This file holds the patterns, and is no finding of any of them.
    with open(os.path.abspath(__file__), "rb") as f:
        own = f.read().decode("utf-8").replace("\r\n", "\n")
    for rule in sorted(RULES):
        expect(not finding_lines(own, rule), "this file is no finding of the rule %s: lines %s"
               % (rule, sorted(finding_lines(own, rule))))
    # The tree's own file passes, and names some directory-form entries.
    said = []
    code = check(ROOT, said.append)
    expect(code == 0 and "directory-form entries" in "\n".join(said), "the tree's %s passes: %s" % (IGNORE, said))

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", default=ROOT, help="the tree (default: this script's)")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return check(args.root)


if __name__ == "__main__":
    sys.exit(main())
