#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every commit in a range carries a Developer Certificate of Origin sign-off by its author.

A commit is signed off when its message has a `Signed-off-by:` trailer whose
address is the commit's author email, compared without regard to case: the
trailer `git commit -s` writes. A sign-off naming somebody else certifies nothing
about the person who wrote the change, so it does not count
(https://developercertificate.org/; CONTRIBUTING.md, "Sign-off").

Two places ask this question and both use this file, so they cannot drift apart:

  * the sign-off job of .gitlab-ci.yml, for every merge request - a fork's and a
    maintainer's - and for the pipeline of every batch* branch, which is how a
    maintainer's change reaches master (a merge request from a batch* branch runs
    no pipeline of its own; the branch's stands for it);
  * build/land-via-mr.py, which refuses to push a batch that would fail it.

The range is BASE..HEAD: what the merge request or the batch adds to master.
--exempt REV (any number) leaves out every commit reachable from REV. It is for
the release tags, and only where they are this project's own protected v* tags:
a commit a release has already published cannot take a trailer without being
rewritten, and until 26 September 2026 the maintainers' own commits were not
signed off. It is never given for a fork's merge request, whose tags are the
fork's.

Usage:
  python3 build/sign-off.py BASE HEAD [--exempt REV ...]
  python3 build/sign-off.py --self-test

Exit 0 when every commit is signed off, 1 when one is not, 2 when the range
cannot be read.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path: a file put there named as one of Python's modules (json.py, re.py) would
# run as this script's code in a change that leaves the script alone
# (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import shutil
import subprocess
import tempfile

FIX = ("Add one with 'git commit --amend -s' for the last commit, or 'git rebase --signoff <base>' for several, "
       "and push again. It is your statement that you wrote the change or may submit it under the project's "
       "licence: https://developercertificate.org/")


class RangeError(Exception):
    pass


def _git(repo, *args):
    done = subprocess.run(["git", "-C", repo] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if done.returncode != 0:
        raise RangeError("git %s: %s" % (" ".join(args), done.stderr.decode("utf-8", "replace").strip()))
    return done.stdout.decode("utf-8", "replace")


def commits(repo, base, head, exempt=()):
    """The commits of BASE..HEAD, oldest first, less those reachable from an exempt rev."""
    for rev in [base, head] + list(exempt):
        _git(repo, "rev-parse", "--verify", "--quiet", rev + "^{commit}")
    args = ["rev-list", "--reverse", head, "^" + base] + ["^" + rev for rev in exempt]
    return [line for line in _git(repo, *args).split() if line]


def signed_off(repo, sha):
    """(author email, subject, True when a Signed-off-by trailer names that email)."""
    sep = "\x1f"
    info = _git(repo, "log", "-1", "--format=%ae" + sep + "%s" + sep + "%(trailers:key=Signed-off-by,valueonly,separator=%x1e)", sha)
    author, subject, trailers = (info.rstrip("\n").split(sep) + ["", ""])[:3]
    author = author.strip().lower()
    names = [t.strip().lower() for t in trailers.split("\x1e") if t.strip()]
    return author, subject, any(("<%s>" % author) in t for t in names)


def unsigned(repo, base, head, exempt=()):
    """[(sha, subject, author)] for every commit of the range without its author's sign-off,
    and the number of commits looked at."""
    found = []
    shas = commits(repo, base, head, exempt)
    for sha in shas:
        author, subject, ok = signed_off(repo, sha)
        if not ok:
            found.append((sha, subject, author))
    return found, len(shas)


def report(repo, base, head, exempt=(), out=sys.stdout):
    """Print one line a commit and a verdict; return the exit code."""
    try:
        shas = commits(repo, base, head, exempt)
    except RangeError as e:
        print("The range %s..%s cannot be read: %s" % (base, head, e), file=out)
        return 2
    missing = 0
    for sha in shas:
        author, subject, ok = signed_off(repo, sha)
        if ok:
            print("ok       %s %s" % (sha[:12], subject[:100]), file=out)
        else:
            print("MISSING  %s %s - no Signed-off-by for %s" % (sha[:12], subject[:100], author), file=out)
            missing += 1
    if exempt:
        print("(Commits already in %d release tag(s) were not looked at.)" % len(exempt), file=out)
    if missing:
        print("", file=out)
        print("%d of %d commit(s) have no Developer Certificate of Origin sign-off by their author." % (missing, len(shas)), file=out)
        print(FIX, file=out)
        return 1
    print("Every commit (%d) carries its author's sign-off." % len(shas), file=out)
    return 0


def self_test():
    """Build a small repository and hold the check to what it must say of each commit."""
    failures = []
    tmp = tempfile.mkdtemp(prefix="sign-off-")
    try:
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", HOME=tmp, GIT_CONFIG_GLOBAL=os.path.join(tmp, "none"))

        def git(*args, **kw):
            e = dict(env)
            e.update(kw.get("env", {}))
            done = subprocess.run(["git", "-C", tmp] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=e)
            if done.returncode != 0:
                raise RuntimeError("git %s: %s" % (args, done.stderr.decode("utf-8", "replace")))
            return done.stdout.decode("utf-8", "replace").strip()

        def commit(name, email, message):
            with open(os.path.join(tmp, "f.txt"), "a") as f:
                f.write(message + "\n")
            git("add", "f.txt")
            who = {"GIT_AUTHOR_NAME": name, "GIT_AUTHOR_EMAIL": email,
                   "GIT_COMMITTER_NAME": name, "GIT_COMMITTER_EMAIL": email}
            git("commit", "-q", "-m", message, env=who)
            return git("rev-parse", "HEAD")

        git("init", "-q", "-b", "main")
        base = commit("Old", "old@example.org", "released, never signed off")
        git("tag", "v1.0.0")
        after_tag = commit("Old", "old@example.org", "after the tag, not signed off")
        ok1 = commit("Ann", "ann@example.org", "signed\n\nSigned-off-by: Ann <ann@example.org>")
        ok2 = commit("Bob", "Bob@Example.org", "case differs\n\nSigned-off-by: Bob <bob@EXAMPLE.org>")
        other = commit("Cat", "cat@example.org", "signed by another\n\nSigned-off-by: Dan <dan@example.org>")
        body = commit("Eve", "eve@example.org", "Signed-off-by: Eve <eve@example.org> in the first line is no trailer\n\nmore text")
        two = commit("Fay", "fay@example.org", "two trailers\n\nSigned-off-by: Gil <gil@example.org>\nSigned-off-by: Fay <fay@example.org>")

        def expect(what, got, want):
            if got != want:
                failures.append("%s: got %r, want %r" % (what, got, want))

        missing, n = unsigned(tmp, base, "HEAD")
        expect("commits looked at", n, 6)
        expect("the unsigned ones", sorted(sha for sha, _, _ in missing), sorted([after_tag, other, body]))
        expect("a trailer naming the author, any case, counts", ok1 not in [m[0] for m in missing] and ok2 not in [m[0] for m in missing], True)
        expect("a second trailer naming the author counts", two not in [m[0] for m in missing], True)
        # The release tag leaves out what it holds, and nothing more.
        missing, n = unsigned(tmp, base, "HEAD", exempt=["v1.0.0"])
        expect("with the tag exempt, looked at", n, 6)
        git("tag", "-f", "v1.0.0", after_tag)
        missing, n = unsigned(tmp, base, "HEAD", exempt=["v1.0.0"])
        expect("with the tag moved on, looked at", n, 5)
        expect("with the tag moved on, unsigned", sorted(sha for sha, _, _ in missing), sorted([other, body]))
        # The verdicts.
        sink = open(os.devnull, "w")
        try:
            expect("exit for a range with unsigned commits", report(tmp, base, "HEAD", out=sink), 1)
            expect("exit for a signed range", report(tmp, after_tag, ok2, out=sink), 0)
            expect("exit for an empty range", report(tmp, "HEAD", "HEAD", out=sink), 0)
            expect("exit for a range that is not there", report(tmp, "0" * 40, "HEAD", out=sink), 2)
        finally:
            sink.close()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for f in failures:
        print("FAIL " + f)
    print("sign-off.py self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    positional, exempt = [], []
    it = iter(argv)
    for arg in it:
        if arg == "--exempt":
            value = next(it, None)
            if not value:
                print(__doc__)
                return 2
            exempt.append(value)
        elif arg.startswith("-"):
            print(__doc__)
            return 2
        else:
            positional.append(arg)
    if len(positional) != 2:
        print(__doc__)
        return 2
    return report(os.getcwd(), positional[0], positional[1], exempt)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
