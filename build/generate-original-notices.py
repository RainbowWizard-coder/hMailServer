#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The docstring quotes his notice, which reuse would take for this file's own.
# REUSE-IgnoreStart
"""Martin Knafve's copyright notices, file by file, from the original's own trees.

The project is a fork of Martin Knafve's hMailServer, whose files each carried his
notice in their opening comment - most of them "Copyright (c) 2010 Martin Knafve /
hMailServer.com." in the file's comment syntax. On 13 September 2026 (adb5e3743)
the fork's header took the place of that notice in about 1,100 files; on 24 October
2026 it was decided to put each file's own notice back beside the header, as it
was, since the AGPL (its sections 4 and 5) asks that the notices a work carries be
kept intact. build/original-notices.txt is the list of them, and
build/add-license-headers.py writes each one below the project's header and refuses
a file of the list without it, and his notice on a file the list does not name.

The list is made from the original's own trees, the ones build/generate-reuse-original.py
reads, so that it says what he wrote and not what the fork made of it:

  * the files: every file of the tree that is the original's - at one of the
    original's paths, letter case aside, or moved, split or copied out of one of
    them (that generator's DERIVED), but not one the fork made anew at such a path
    (its FORK_MADE) or a third party's (its THIRD_PARTY) - and that
    build/add-license-headers.py writes the project's header into;
  * the notice of each: the lines of the opening comment of the original's own
    file (a DERIVED file's source) that are his notice, whole - a comment line whose
    text is "Copyright (c)", a year or two or none, and "Martin Knafve /
    hMailServer.com.". The original's file is the one at the newest of the tags
    hMailServer-5.7.0-b3013, -b2843 and -b2788 that holds the path, or, for a path
    none of them holds, the one the fork's first commit, fe45f43c3, took over from
    the original's history before them (the two agree for every path both hold).
    A file whose original carried no notice gets none: not every one did (Version.h,
    hMailServer.idl, BO/Route.h and BLCheck.h among them, and many of the Windows
    suite's tests), so the list names most of the original's files that carry the
    header, not all. Nor does a DERIVED file whose source is at one of the
    original's later commits (LATER), the SPF port's: that commit is not in the
    project's repository, so the reuse job could not hold a line taken from it to
    it, and each file of the port quotes his notice in its own attribution, which
    stays as the port wrote it; REUSE.toml's tables name him for each of them.
    Nor does a DERIVED file that keeps his words alone - a sentence of an error or
    a warning of his, verbatim, and no code, comment or data of his (WORDS_ONLY):
    the notice the list puts back is the one his files carried, which goes with
    code moved out of one of them, and a file the fork wrote that repeats a message
    of his carried none; REUSE.toml's tables name him for each of them too.
    A line is written as the original wrote it, trailing blanks aside, where the
    file has the original's comment syntax, and in the file's own syntax where it
    has another (a .resx, whose header is an XML comment).

Where the list and the fork's own history part (--against-history, which reads every
line naming him that one of the fork's commits took out of a file): the fork gave
some files of its own a notice in his name, and wrote a year of its own into some it
ported. NOT_HIS names each such line, and why it is not put back; a file of
WORDS_ONLY gets no line by that rule, whatever a commit took out of it (the notice the
list once gave each of them, before WORDS_ONLY); and the list's own lines, which a
commit takes out of the list when a file leaves it, are not a notice on the list. Every
other notice the fork's commits took out of a file of the tree is in the list.

Where the original's trees come from: the clone, or the git directory that
HM_REUSE_ORIGINAL_GIT_DIR names - build/ci/reuse-lint.sh fetches the three tags and
the fork's first commit there, their commits and trees alone, when the clone has not
got them, as CI's depth-1 one has not. This reads only the files it needs from them;
in such a directory git fetches those, in one request, before they are read.

Usage:
  python3 build/generate-original-notices.py             write the list (the
                                                         original's trees must be
                                                         at hand)
  python3 build/generate-original-notices.py --check     exit 1 when the list is
                                                         not what this writes, or
                                                         names what it may not;
                                                         without the trees, the
                                                         list as committed is held
                                                         to the tree and to the
                                                         list of the original's
                                                         files
  python3 build/generate-original-notices.py --check --require-tags
                                                         the same, and exit 2 when
                                                         the trees are not at
                                                         hand: the reuse job's
  python3 build/generate-original-notices.py --against-history
                                                         exit 1 when a notice one
                                                         of the fork's commits
                                                         took out of a file of
                                                         the tree is neither in
                                                         the list nor in NOT_HIS
                                                         (a full clone; about a
                                                         minute)
  python3 build/generate-original-notices.py --self-test the rules above, against
                                                         files and lists made to
                                                         break them

The list is written with LF line endings and compared with CRLF folded to LF.
"""
# REUSE-IgnoreEnd

import importlib.util
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST_FILE = "build/original-notices.txt"
# The empty tree, which git knows without being told.
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
# The fork's commit that took the notices out, and the first of its commits.
TAKEN_OUT = "adb5e3743"

# REUSE-IgnoreStart
# His notice, whole, as the text of a comment line: the wordings the original's files
# carry ("Copyright (c) 2010 Martin Knafve / hMailServer.com.", a year of 2005 to
# 2014, one with two spaces, one with none). The word is put together so that reuse
# does not take this file's pattern for a notice of its own.
# REUSE-IgnoreEnd
NOTICE = re.compile(r"^" + "Copy" + r"right \(c\)\s*(?:\d{4}(?:\s*-\s*\d{4})?)?\s*Martin Knafve / hMailServer\.com\.?$")

# Lines naming him that one of the fork's commits took out of a file of the tree and
# that the list does not put back: (the tree's path, the line, trailing blanks aside,
# and why). The fork's first commit, fe45f43c3, and its later work gave files of its
# own the original's template notice (build/generate-reuse-original.py does not count
# them among the original's: they keep nothing of his, or only the form his files of
# their kind share, which is not one of his more than another), and its ports of the
# original's files of 2026 wrote "2026" into his notice, where the original's file
# carries 2010 or none.
_TEMPLATE = "the fork's own file, which keeps nothing of his, given the original's template notice by %s"
_FORM = ("the fork's own declaration, in the form his headers of its kind share and not one of them more than "
         "another (build/generate-reuse-original.py's docstring), given the original's template notice by %s")
_NONE = ("the fork's port (%s) of the original's file, which carries no notice at hMailServer-5.7.0-b3013, "
         "given one in his name with the year 2026")
_YEAR = ("the fork's port (%s) of the original's file wrote the year 2026 into his notice; the original's file at "
         "hMailServer-5.7.0-b3013 carries 2010, which the list gives")
_N2026 = "Copy" + "right (c) 2026 Martin Knafve / hMailServer.com."
_N2010 = "Copy" + "right (c) 2010 Martin Knafve / hMailServer.com."
NOT_HIS = (
    ("hmailserver/source/Server/Common/AntiSpam/DMARC/DMARC.cpp", "// " + _N2010, _TEMPLATE % "fe45f43c3"),
    ("hmailserver/source/Server/Common/AntiSpam/DMARC/DMARC.h", "// " + _N2010, _TEMPLATE % "fe45f43c3"),
    ("hmailserver/source/Server/Common/AntiSpam/DMARC/SpamTestDMARC.cpp", "// " + _N2010,
     _TEMPLATE % "fe45f43c3" + " (FORK_MADE)"),
    ("hmailserver/source/Server/Common/AntiSpam/DMARC/SpamTestDMARC.h", "// " + _N2010,
     _FORM % "fe45f43c3" + " (FORK_MADE)"),
    ("hmailserver/source/Server/IMAP/IMAPCommandID.cpp", "// " + _N2010, _TEMPLATE % "fe45f43c3"),
    ("hmailserver/source/Server/IMAP/IMAPCommandID.h", "// " + _N2010, _FORM % "fe45f43c3"),
    ("hmailserver/source/Server/IMAP/IMAPCommandMove.h", "// " + _N2010, _FORM % "fe45f43c3"),
    ("hmailserver/source/Server/IMAP/IMAPMove.h", "// " + _N2010, _FORM % "fe45f43c3"),
    ("hmailserver/test/RegressionTests/IMAP/SequenceSets.cs", "// " + _N2010, _TEMPLATE % "fd9729f3d"),
    ("build/Get-LibraryVersions.ps1", "# " + _N2026, _NONE % "5268247804"),
    ("libraries/build-boost.ps1", "# " + _N2026, _NONE % "5268247804"),
    ("libraries/build-common.ps1", "# " + _N2026, _NONE % "5268247804"),
    ("libraries/build-openssl.ps1", "# " + _N2026, _NONE % "5268247804"),
    ("libraries/build-pgsql.ps1", "# " + _N2026, _NONE % "5268247804"),
    ("hmailserver/test/RegressionTests/IMAP/SessionMessageNumbering.cs", "// " + _N2026, _NONE % "c8607f392d"),
    ("hmailserver/test/RegressionTests/Stress/IMAP/ConcurrentConnectionsStressTests.cs", "// " + _N2026,
     _NONE % "e3b8d99e92"),
    ("hmailserver/test/RegressionTests/Stress/IMAP/NotificationThreadingStressTests.cs", "// " + _N2026,
     _NONE % "e3b8d99e92"),
    ("hmailserver/source/Server/ExternalFetcher/FetchAccountLock.cpp", "// " + _N2026, _YEAR % "77915f8faa"),
    ("hmailserver/source/Server/ExternalFetcher/FetchAccountLock.h", "// " + _N2026, _YEAR % "77915f8faa"),
    ("hmailserver/source/Server/IMAP/IMAPFolderView.cpp", "// " + _N2026, _YEAR % "c8607f392d"),
    ("hmailserver/source/Server/IMAP/IMAPFolderView.h", "// " + _N2026, _YEAR % "c8607f392d"),
)

# The DERIVED files of build/generate-reuse-original.py that keep his words alone: a
# sentence of an error or a warning of his, verbatim, and no code, comment or data of
# his. REUSE.toml's tables name him for each; the list gives none of them a line (the
# module docstring says why). Each must be a DERIVED file whose source is one of the
# original's files.
WORDS_ONLY = (
    "hmailserver/source/Server/Common/Persistence/MailboxMove.cpp",
    "hmailserver/source/Server/Common/Persistence/MessageStore.cpp",
    "hmailserver/source/Server/Common/SQL/ScriptTransaction.cpp",
    "hmailserver/source/Server/Common/Scripting/ScriptBinding.cpp",
    "hmailserver/source/Server/Common/Util/ApiSends.cpp",
    "hmailserver/source/Server/Common/Util/DateTime.cpp",
    "hmailserver/source/Server/Common/Util/RestApiMailbox.cpp",
    "hmailserver/source/Server/Common/Util/RestApiRoutes.cpp",
    "hmailserver/source/Server/Common/Util/RestApiSettings.cpp",
    "hmailserver/source/Tools/ControlPanel/Resources/Strings.resx",
    "hmailserver/source/Tools/ControlPanel/Services/ConfigurationChecks.cs",
)

LIST_HEAD = [
    "# https://www.progressiverobot.com",
    # REUSE-IgnoreStart
    "# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd",
    "# SPDX-License-Identifier: AGPL-3.0-or-later",
    # REUSE-IgnoreEnd
    "#",
    "# Generated by build/generate-original-notices.py - do not edit; run it again.",
    "# Martin Knafve's copyright notice of each of the original hMailServer's files that",
    "# carry the project's header and whose own file in the original carried it, as that",
    "# file wrote it (its docstring says which): the file's path, then each line of the",
    "# notice, a tab before it.",
    "# build/add-license-headers.py writes the lines below the header and holds each",
    "# file to them.",
]
# Written between the two, so that reuse does not take the notices for this file's.
IGNORE_START = "# REUSE-" + "IgnoreStart"
IGNORE_END = "# REUSE-" + "IgnoreEnd"


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, relative))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


original = load("generate_reuse_original", "build/generate-reuse-original.py")
headers = load("add_license_headers", "build/add-license-headers.py")


# ---------------------------------------------------------------- reading a notice

def comment_marker(path):
    """The line-comment marker of a file, as build/add-license-headers.py writes the
    header in it, None for a file it writes a comment element in, and False for one it
    does not write a header in at all."""
    ext = headers.kind(path)
    if ext is None:
        return False
    return headers.LINE_COMMENT.get(ext)


def notice_lines(raw, marker):
    """The lines of a file's opening comment - every line from the top that is blank
    or a comment of marker - that are his notice, whole, trailing blanks aside."""
    if raw.startswith(headers.BOM):
        raw = raw[len(headers.BOM):]
    m = marker.encode()
    found = []
    for line in raw.split(b"\n"):
        s = line.strip()
        if s and not s.startswith(m):
            break
        if s and NOTICE.match(s[len(m):].strip().decode("utf-8", "replace")):
            found.append(line.rstrip().decode("utf-8"))
    return found


def written_for(line, source_marker, target_marker):
    """A notice line of the original's, as the tree's file writes it: unchanged where
    the file has the original's comment syntax, its text in the file's own where it has
    another, and indented by two in a comment element (target_marker None)."""
    text = line.strip()[len(source_marker):].strip()
    if target_marker is None:
        return "  " + text
    if target_marker == source_marker:
        return line
    return target_marker + " " + text


def is_notice_line(line, marker):
    """Whether a line of the list is his notice in a file whose header is written with
    marker (None: in a comment element)."""
    if line != line.rstrip():
        return False
    if marker is None:
        return line.startswith("  ") and bool(NOTICE.match(line.strip()))
    return line.startswith(marker + " ") and bool(NOTICE.match(line[len(marker):].strip()))


# ---------------------------------------------------------------- the list

def render_list(entries):
    lines = list(LIST_HEAD) + [IGNORE_START]
    for path in sorted(entries):
        if path.startswith(("#", "\t")) or "\n" in path or path != path.strip():
            sys.exit("%s: a path this list cannot hold" % path)
        lines.append(path)
        lines += ["\t" + line for line in entries[path]]
    lines.append(IGNORE_END)
    return "\n".join(lines) + "\n"


def parse_list(text):
    """(entries, problems): {path: [notice lines]} from the list's text, in its order."""
    entries, problems, order = {}, [], []
    path = None
    for number, line in enumerate(text.split("\n"), 1):
        if not line or line.startswith("#"):
            continue
        if line.startswith("\t"):
            if path is None:
                problems.append("%s, line %d: a notice before any path" % (LIST_FILE, number))
            else:
                entries[path].append(line[1:])
            continue
        path = line
        if path in entries:
            problems.append("%s names %s twice" % (LIST_FILE, path))
        entries.setdefault(path, [])
        order.append(path)
    for p in order:
        if not entries[p]:
            problems.append("%s names %s with no notice" % (LIST_FILE, p))
    if order != sorted(set(order)):
        problems.append("%s is not in the order this writes: run build/generate-original-notices.py" % LIST_FILE)
    return entries, problems


def read_text(relative):
    path = os.path.join(ROOT, relative)
    if not os.path.exists(path):
        return None
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def later_sourced(derived=original.DERIVED):
    """The DERIVED files whose source is at one of the original's later commits."""
    return set(entry[0] for entry in derived if original.LATER_SOURCE.match(entry[1]))


def words_only_problems(words=WORDS_ONLY, derived=original.DERIVED):
    """What WORDS_ONLY names that it may not: a path twice, or one that is not a DERIVED
    file whose source is one of the original's files."""
    problems = []
    sources = dict((entry[0], entry[1]) for entry in derived)
    seen = set()
    for path in words:
        if path in seen:
            problems.append("WORDS_ONLY names %s twice" % path)
        seen.add(path)
        if path not in sources:
            problems.append("WORDS_ONLY names %s, which is not in build/generate-reuse-original.py's DERIVED" % path)
        elif original.LATER_SOURCE.match(sources[path]):
            problems.append("WORDS_ONLY names %s, whose source is at a later commit of the original's: it gets no line "
                            "as such" % path)
    return problems


def entry_problems(entries, theirs_files, tree, fork_made=original.FORK_MADE, marker_of=comment_marker,
                   derived=original.DERIVED, words=WORDS_ONLY):
    """What the list names that it may not: a file the tree has not, one that is not
    the original's (theirs_files: the original's files and the DERIVED ones), one the
    fork made at the original's paths, one ported from a later commit of the
    original's, one that keeps his words alone (WORDS_ONLY), one the header is not
    written in, a line that is not his notice in the file's comment syntax."""
    problems = []
    in_tree = set(tree)
    his = set(theirs_files)
    made = set(entry[0] for entry in fork_made)
    later = later_sourced(derived)
    words = set(words)
    for path, lines in sorted(entries.items()):
        if path not in in_tree:
            problems.append("%s names %s, which the tree has not" % (LIST_FILE, path))
            continue
        if path in made:
            problems.append("%s names %s, which the fork made (FORK_MADE): it carries no notice of his" % (LIST_FILE, path))
        elif path in later:
            problems.append("%s names %s, which DERIVED gives a source at a later commit of the original's that the "
                            "project's repository has not: it gets no line, and quotes his notice in its own "
                            "attribution" % (LIST_FILE, path))
        elif path in words:
            problems.append("%s names %s, which keeps his words alone (WORDS_ONLY): REUSE.toml's tables name him, "
                            "and it gets no line" % (LIST_FILE, path))
        elif path not in his:
            problems.append("%s names %s, which is not one of the original's files (build/generate-reuse-original.py)"
                            % (LIST_FILE, path))
        marker = marker_of(path)
        if marker is False:
            problems.append("%s names %s, which build/add-license-headers.py writes no header in" % (LIST_FILE, path))
            continue
        for line in lines:
            if not is_notice_line(line, marker):
                problems.append("%s gives %s a line that is not his notice in its comment syntax: %r"
                                % (LIST_FILE, path, line))
    return problems


def list_difference(committed, wanted):
    """The entries one list has and the other has not, or has otherwise: ("+" the
    committed list's, "-" the original's trees')."""
    out = []
    for path in sorted(set(committed) | set(wanted)):
        have, want = committed.get(path), wanted.get(path)
        if have == want:
            continue
        if have is not None:
            out += ["+ %s: %s" % (path, line) for line in have]
        if want is not None:
            out += ["- %s: %s" % (path, line) for line in want]
    return out


# ---------------------------------------------------------------- the original's trees

def git_original(*args, **kwargs):
    return subprocess.run(["git", "-c", "core.quotepath=off"] + original.original_git_args() + list(args), cwd=ROOT,
                          capture_output=True, **kwargs)


def tree_of(ref):
    """{path: blob} of a commit's tree (its trees alone are read)."""
    out = {}
    for record in git_original("ls-tree", "-r", "-z", ref, check=True).stdout.split(b"\0"):
        if record:
            meta, path = record.split(b"\t", 1)
            out[path.decode("utf-8")] = meta.split()[2].decode()
    return out


def read_blobs(wanted):
    """{blob: bytes} for (ref, path, blob) triples. In a directory a fetch filled with
    the trees alone the blobs are not there, and git would fetch each one alone as it
    is read; a diff fetches all that it needs in one request, and so the missing blobs
    are put in a tree of their own (mktree --missing writes it without them) and that
    tree is compared with the empty one first. cat-file then reads them."""
    env = dict(os.environ, GIT_NO_LAZY_FETCH="1")
    blobs = sorted(set(b for _, _, b in wanted))
    if not blobs:
        return {}
    check = git_original("cat-file", "--batch-check", input=("\n".join(blobs) + "\n").encode(), env=env)
    missing = sorted(set(line.split()[0].decode() for line in check.stdout.split(b"\n") if line.endswith(b" missing")))
    if missing:
        entries = "".join("100644 blob %s\t%d\n" % (blob, i) for i, blob in enumerate(missing))
        tree = git_original("mktree", "--missing", input=entries.encode(), check=True).stdout.decode().strip()
        git_original("diff", "--stat", "--no-renames", EMPTY_TREE, tree, check=True)
    out = git_original("cat-file", "--batch", input=("\n".join(blobs) + "\n").encode(), check=True).stdout
    result, i = {}, 0
    while i < len(out):
        end = out.index(b"\n", i)
        head = out[i:end].split()
        if len(head) < 3 or head[1] != b"blob":
            sys.exit("git cat-file could not read %s" % out[i:end].decode("utf-8", "replace"))
        size = int(head[2])
        result[head[0].decode()] = out[end + 1:end + 1 + size]
        i = end + 1 + size + 1
    return result


def trees_at_hand():
    return original.tags_present()


def wanted_entries(mine, derived=original.DERIVED, tree_reader=tree_of, blob_reader=read_blobs,
                   marker_of=comment_marker, words=WORDS_ONLY):
    """(entries, notes): the list as the original's trees give it, for the original's
    files of the tree (mine). A DERIVED file whose source is at a later commit of the
    original's gets no entry (the module docstring says why), whether or not that
    commit is at hand, so that the list is the same wherever it is made; nor does one
    that keeps his words alone (WORDS_ONLY)."""
    sources = dict((entry[0], entry[1]) for entry in derived)
    words = set(words)
    refs = tuple(reversed(original.TAGS)) + (original.TAKEN_OVER,)
    trees = dict((ref, tree_reader(ref)) for ref in refs)
    folded = dict((ref, dict((p.lower(), p) for p in trees[ref])) for ref in refs)
    notes = []
    wanted = []
    for path in mine:
        target = marker_of(path)
        if target is False:
            continue
        source = sources.get(path, path)
        if original.LATER_SOURCE.match(source) or path in words:
            continue
        for ref in refs:
            at = source if source in trees[ref] else folded[ref].get(source.lower())
            if at is not None:
                wanted.append((path, ref, at, trees[ref][at]))
                break
        else:
            notes.append("%s: its source %s is in none of the original's trees" % (path, source))
    blobs = blob_reader([(ref, at, blob) for _, ref, at, blob in wanted])
    entries = {}
    for path, ref, at, blob in wanted:
        name = os.path.basename(at)
        source_marker = headers.LINE_COMMENT.get(headers.BY_NAME.get(name) or os.path.splitext(name)[1].lower())
        if not source_marker:
            continue
        lines = notice_lines(blobs[blob], source_marker)
        if lines:
            entries[path] = [written_for(line, source_marker, marker_of(path)) for line in lines]
    return entries, notes


# ---------------------------------------------------------------- the fork's history

def removed_by_history():
    """{tree path: [lines]}: every line naming him, trailing blanks aside, that one of the
    fork's commits since its first took out of a file, and did not put back in the same
    file, mapped to the file's path in the tree through the renames since."""
    def run(*args):
        return subprocess.run(["git", "-c", "core.quotepath=off"] + list(args), cwd=ROOT, capture_output=True,
                              check=True).stdout
    commits = run("log", "--format=%H", "--no-merges", "-G", "Knafve", original.TAKEN_OVER + "..HEAD").decode().split()
    removed = {}
    for commit in commits:
        taken, put = {}, {}
        new = None
        for line in run("show", "-M", "--format=", "-U0", "--no-color", commit).split(b"\n"):
            if line.startswith(b"diff --git "):
                new = None
            elif line.startswith(b"+++ "):
                p = line[4:].decode("utf-8", "replace")
                new = None if p == "/dev/null" else p[2:]
            elif line.startswith((b"-", b"+")) and not line.startswith((b"---", b"+++")) and b"Knafve" in line:
                text = line[1:].decode("utf-8", "replace").lstrip("﻿").rstrip()
                (taken if line.startswith(b"-") else put).setdefault(new, []).append(text)
        if not any(p for p in taken):
            continue
        moved = {}
        status = run("diff", "-M", "--name-status", "-z", commit, "HEAD").split(b"\0")
        i = 0
        while i < len(status) - 1:
            kind = status[i].decode()
            if kind[:1] in ("R", "C"):
                moved[status[i + 1].decode("utf-8")] = status[i + 2].decode("utf-8")
                i += 3
            else:
                if kind == "D":
                    moved[status[i + 1].decode("utf-8")] = None
                i += 2
        for path, lines in taken.items():
            if path is None:
                continue
            back = list(put.get(path, []))
            for text in lines:
                if text in back:
                    back.remove(text)
                    continue
                at = moved.get(path, path)
                if at is not None:
                    removed.setdefault(at, [])
                    if text not in removed[at]:
                        removed[at].append(text)
    return removed


def is_removed_notice(line):
    """Whether a line a commit took out is his notice, whole, in a line comment."""
    text = line.strip()
    return any(text.startswith(m) and bool(NOTICE.match(text[len(m):].strip())) for m in ("//", "#", ";"))


def history_problems(removed, entries, tree, not_his=NOT_HIS, words=WORDS_ONLY):
    """What the fork's commits took out that is neither in the list nor in NOT_HIS - but
    for a file of WORDS_ONLY and the list itself (the module docstring says why) - and
    what NOT_HIS names that they did not take out."""
    problems = []
    in_tree = set(tree)
    excused = {}
    for path, line, why in not_his:
        excused.setdefault(path, []).append(line)
    words = set(words)
    for path in sorted(removed):
        if path not in in_tree or path in words or path == LIST_FILE:
            continue
        for line in removed[path]:
            if not is_removed_notice(line):
                continue
            if line in excused.get(path, []):
                continue
            if line not in entries.get(path, []):
                problems.append("%s: the fork's commits took out %r, which the list does not put back and NOT_HIS "
                                "does not name" % (path, line))
    for path, line, why in not_his:
        if line not in removed.get(path, []):
            problems.append("NOT_HIS names %r of %s, which no commit of the fork's took out of it" % (line, path))
        if line in entries.get(path, []):
            problems.append("NOT_HIS names %r of %s, which the list puts back" % (line, path))
    return problems


# ---------------------------------------------------------------- the modes

def his_files():
    committed = original.read_text(original.PATHS_FILE)
    if committed is None:
        return None, None
    tree = original.tree_paths()
    mine, theirs = original.original_files(original.read_paths(committed), tree)
    return mine, tree


def generate(check, require):
    mine, tree = his_files()
    if mine is None:
        print("%s is missing" % original.PATHS_FILE)
        return 2
    text = read_text(LIST_FILE)
    committed, problems = parse_list(text) if text is not None else ({}, [])
    if text is None:
        problems.append("%s is missing: run build/generate-original-notices.py" % LIST_FILE)
    if not check:
        # What the committed list's form is does not matter to writing it anew.
        problems = []
    problems += words_only_problems()
    if not trees_at_hand():
        if require or not check:
            print("The original's tags (%s) and the fork's first commit are not at hand, here or in %s: %s cannot "
                  "be made from them" % (", ".join(original.TAGS), original.ORIGINAL_GIT_DIR, LIST_FILE))
            return 2
        print("This clone has not the original's tags: %s is taken as committed and held to the tree." % LIST_FILE)
        problems += entry_problems(committed, mine, tree)
        for problem in problems:
            print(problem)
        if problems:
            return 1
        print("%s names %d of the original's files, each with his notice in its comment syntax"
              % (LIST_FILE, len(committed)))
        return 0
    wanted, notes = wanted_entries(mine)
    for note in notes:
        print(note)
    # What the committed list names that it may not, by name, beside its difference
    # from the trees' below; what this would write, when it writes.
    problems += entry_problems(committed if check else wanted, mine, tree)
    rendered = render_list(wanted)
    if text != rendered:
        if check:
            difference = list_difference(committed, wanted)
            problems.append("%s is not what the original's trees give; run build/generate-original-notices.py" % LIST_FILE)
            problems += ["  " + line for line in difference[:20]]
            if len(difference) > 20:
                problems.append("  ... and %d more" % (len(difference) - 20))
        elif not problems:
            with io.open(os.path.join(ROOT, LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
                f.write(rendered)
            print("wrote " + LIST_FILE)
    for problem in problems:
        print(problem)
    if problems:
        return 1
    years = {}
    for lines in wanted.values():
        for line in lines:
            year = re.search(r"\(c\)\s*([0-9 -]*)", line).group(1).strip() or "no year"
            years[year] = years.get(year, 0) + 1
    print("%s gives Martin Knafve's notice, as the original's files carry it, to %d of them (%s)"
          % (LIST_FILE, len(wanted), ", ".join("%d of %s" % (n, y) for y, n in sorted(years.items()))))
    return 0


def against_history():
    mine, tree = his_files()
    text = read_text(LIST_FILE)
    if text is None or mine is None:
        print("%s or %s is missing" % (LIST_FILE, original.PATHS_FILE))
        return 2
    ok = subprocess.run(["git", "rev-parse", "--verify", "--quiet", TAKEN_OUT + "^{commit}"], cwd=ROOT,
                        capture_output=True).returncode == 0
    if not ok:
        print("The fork's history (%s) is not at hand: run this in a full clone" % TAKEN_OUT)
        return 2
    entries, problems = parse_list(text)
    removed = removed_by_history()
    problems += history_problems(removed, entries, tree)
    for problem in problems:
        print(problem)
    if problems:
        return 1
    in_tree = set(tree)
    taken = set(p for p, lines in removed.items() if p in in_tree and any(is_removed_notice(line) for line in lines))
    words = taken & set(WORDS_ONLY)
    count = len(taken - words - set([LIST_FILE]))
    print("Every notice of his that the fork's commits took out of a file of the tree, %d files, is in %s, but the %d "
          "lines NOT_HIS names (and the %d files of WORDS_ONLY that a commit took one out of)"
          % (count, LIST_FILE, len(NOT_HIS), len(words)))
    return 0


def self_test():
    failures = []

    def expect(what, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (what, got, want))

    n2010 = "// " + _N2010
    n2005 = "// Copy" + "right (c) 2005 Martin Knafve / hMailServer.com."
    expect("a notice, a BOM and trailing blanks", notice_lines(headers.BOM + (n2010 + "  \r\n// http://x\r\n\r\n#include").encode(), "//"),
           [n2010])
    expect("two notices", notice_lines((n2005 + "\n" + n2010 + "\n\nint x;\n").encode(), "//"), [n2005, n2010])
    expect("no year, two spaces",
           notice_lines(("// Copy" + "right (c) Martin Knafve / hMailServer.com.\n// Copy" + "right (c) 2013  Martin "
                         "Knafve / hMailServer.com.\n").encode(), "//"),
           ["// Copy" + "right (c) Martin Knafve / hMailServer.com.", "// Copy" + "right (c) 2013  Martin Knafve / hMailServer.com."])
    expect("a notice below the opening comment", notice_lines(("#include\n" + n2010 + "\n").encode(), "//"), [])
    expect("a notice quoted in a sentence",
           notice_lines(("// where this file carries the notice: " + _N2010 + "\n").encode(), "//"), [])
    expect("a notice in another comment syntax", notice_lines(("# " + _N2010 + "\n").encode(), "//"), [])
    expect("as the original wrote it", written_for(n2010, "//", "//"), n2010)
    expect("in a comment element", written_for(n2010, "//", None), "  " + _N2010)
    expect("in another syntax", written_for(n2010, "//", "#"), "# " + _N2010)
    expect("a notice line", is_notice_line(n2010, "//"), True)
    expect("a notice line in the wrong syntax", is_notice_line("# " + _N2010, "//"), False)
    expect("a notice line with trailing blanks", is_notice_line(n2010 + " ", "//"), False)
    expect("a fork notice", is_notice_line("// Copy" + "right (c) 2026 Christopher Holloway / Progressive Robot Ltd", "//"),
           False)
    expect("a notice in a comment element", is_notice_line("  " + _N2010, None), True)

    entries = {"a/One.cpp": [n2010], "a/Two.cs": [n2005, n2010], "b/Res.resx": ["  " + _N2010]}
    expect("the list read back", parse_list(render_list(entries)), (entries, []))
    expect("a notice before a path", len(parse_list("\t" + n2010 + "\na/One.cpp\n\t" + n2010 + "\n")[1]), 1)
    expect("a path twice", len(parse_list("a/One.cpp\n\t" + n2010 + "\na/One.cpp\n\t" + n2010 + "\n")[1]) >= 1, True)
    expect("a path without a notice", len(parse_list("a/One.cpp\n")[1]), 1)
    expect("out of order", len(parse_list("b/x.cpp\n\t" + n2010 + "\na/y.cpp\n\t" + n2010 + "\n")[1]), 1)

    markers = {"a/One.cpp": "//", "a/Two.cs": "//", "b/Res.resx": None, "a/Made.cpp": "//", "a/New.cpp": "//",
               "a/data.bin": False, "a/Ps.ps1": "#"}
    tree = list(markers)
    his = ["a/One.cpp", "a/Two.cs", "b/Res.resx", "a/data.bin", "a/Ps.ps1"]
    made = (("a/Made.cpp", "abc", "made"),)

    def problems_of(e):
        return entry_problems(e, his, tree, made, markers.get)

    expect("good entries", problems_of(entries), [])
    expect("a file gone", len(problems_of({"a/Gone.cpp": [n2010]})), 1)
    expect("a file the fork made", len(problems_of({"a/Made.cpp": [n2010]})), 1)
    expect("a file of the fork's", len(problems_of({"a/New.cpp": [n2010]})), 1)
    expect("a file without a header", len(problems_of({"a/data.bin": [n2010]})), 1)
    expect("a notice in the wrong syntax", len(problems_of({"a/Ps.ps1": [n2010]})), 1)
    expect("a line that is not his notice", len(problems_of({"a/One.cpp": ["// Copy" + "right (c) 2010 Someone"]})), 1)

    expect("the lists' difference", list_difference({"a": ["x"], "b": ["y"]}, {"a": ["x"], "b": ["z"], "c": ["w"]}),
           ["+ b: y", "- b: z", "- c: w"])

    # The original's trees, in memory: a tag and the fork's first commit.
    tag, first = original.TAGS[-1], original.TAKEN_OVER
    trees = {tag: {"a/One.cpp": "1", "a/TWO.CPP": "2", "c/Src.cs": "3", "d/None.cpp": "4"},
             first: {"a/One.cpp": "5", "e/Old.cpp": "6"}}
    data = {"1": (n2010 + "\n").encode(), "2": (n2005 + "\n").encode(), "3": (n2010 + "\n").encode(),
            "4": b"// nothing\n", "5": ("// Copy" + "right (c) 2009 Martin Knafve / hMailServer.com.\n").encode(),
            "6": (n2005 + "\n").encode()}
    tmarkers = {"a/One.cpp": "//", "a/Two.cpp": "//", "b/Res.resx": None, "b/Moved.ps1": "#", "d/None.cpp": "//",
                "e/Old.cpp": "//", "f/x.bin": False}

    def reader(ref):
        return trees.get(ref, {})

    def blobs(wanted):
        return dict((blob, data[blob]) for _, _, blob in wanted)

    later = original.LATER[0][0]
    derived = (("b/Res.resx", "c/Src.cs", "x", "y"), ("b/Moved.ps1", "a/One.cpp", "x", "y"),
               ("g/Port.cpp", later + ":g/Port.cpp", "x", "y"))
    tmarkers["g/Port.cpp"] = "//"
    got, notes = wanted_entries(["a/One.cpp", "a/Two.cpp", "b/Res.resx", "b/Moved.ps1", "d/None.cpp",
                                 "e/Old.cpp", "f/x.bin", "g/Port.cpp"], derived, reader, blobs, tmarkers.get)
    expect("the original's trees' notices, and none for a port of a later commit",
           got, {"a/One.cpp": [n2010], "a/Two.cpp": [n2005], "b/Res.resx": ["  " + _N2010], "b/Moved.ps1": ["# " + _N2010],
                 "e/Old.cpp": [n2005]})
    expect("a port of a later commit in the list",
           len(entry_problems({"g/Port.cpp": [n2010]}, ["g/Port.cpp"], ["g/Port.cpp"], (), tmarkers.get, derived)), 1)
    expect("a port of a later commit not in the list",
           entry_problems({"a/One.cpp": [n2010]}, ["a/One.cpp", "g/Port.cpp"], ["a/One.cpp", "g/Port.cpp"], (),
                          tmarkers.get, derived), [])
    got, notes = wanted_entries(["a/One.cpp", "b/Res.resx", "b/Moved.ps1"], derived, reader, blobs, tmarkers.get,
                                ("b/Moved.ps1",))
    expect("no line for a file that keeps his words alone", got, {"a/One.cpp": [n2010], "b/Res.resx": ["  " + _N2010]})
    expect("a file that keeps his words alone in the list",
           len(entry_problems({"b/Moved.ps1": ["# " + _N2010]}, ["b/Moved.ps1"], ["b/Moved.ps1"], (), tmarkers.get,
                              derived, ("b/Moved.ps1",))), 1)
    expect("WORDS_ONLY as it may be", words_only_problems(("b/Moved.ps1", "b/Res.resx"), derived), [])
    expect("WORDS_ONLY naming a file not in DERIVED", len(words_only_problems(("a/One.cpp",), derived)), 1)
    expect("WORDS_ONLY naming a port of a later commit", len(words_only_problems(("g/Port.cpp",), derived)), 1)
    expect("WORDS_ONLY naming a file twice", len(words_only_problems(("b/Res.resx", "b/Res.resx"), derived)), 1)
    expect("the tree's WORDS_ONLY", words_only_problems(), [])

    removed = {"a/One.cpp": [n2010], "a/Fork.cpp": [n2010], "gone.cpp": [n2010], "a/Two.cs": ["// a test of Knafve"]}
    not_his = (("a/Fork.cpp", n2010, "the fork's"),)
    tree2 = ["a/One.cpp", "a/Fork.cpp", "a/Two.cs"]
    expect("history and the list agree", history_problems(removed, {"a/One.cpp": [n2010]}, tree2, not_his), [])
    expect("a notice the list does not put back",
           len(history_problems(removed, {}, tree2, not_his)), 1)
    expect("a NOT_HIS line no commit took out",
           len(history_problems({"a/One.cpp": [n2010]}, {"a/One.cpp": [n2010]}, tree2, not_his)), 1)
    expect("a NOT_HIS line the list puts back",
           len(history_problems(removed, {"a/One.cpp": [n2010], "a/Fork.cpp": [n2010]}, tree2, not_his)), 1)
    removed3 = dict(removed, **{"a/Words.cpp": [n2010], LIST_FILE: ["\t" + n2010]})
    tree3 = tree2 + ["a/Words.cpp", LIST_FILE]
    expect("a notice taken out of a file that keeps his words alone, and a line of the list",
           history_problems(removed3, {"a/One.cpp": [n2010]}, tree3, not_his, ("a/Words.cpp",)), [])
    expect("a notice taken out of a file that does not keep his words alone",
           len(history_problems(removed3, {"a/One.cpp": [n2010]}, tree3, not_his, ())), 1)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--against-history" in argv:
        return against_history()
    return generate("--check" in argv, "--require-tags" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
