#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The original hMailServer's files in REUSE.toml, made from the original's tags.

The project is a fork of Martin Knafve's hMailServer. In September 2026 the
fork's header replaced his notice in the files that came from the original (the
owner's decision, about the files' text), so a file's own header no longer says
that he is a holder of it, and reuse would record the fork alone. This keeps
the record true: it names Martin Knafve as a holder of every file of the
original, as debian/copyright's "Files: *" does, beside what the file says
itself, from the original's own trees rather than from a pattern of
directories, which would credit him with what the fork adds there.

The original's files are the paths that its tags hold -

  hMailServer-5.7.0-b2788, hMailServer-5.7.0-b2843 and hMailServer-5.7.0-b3013

- and those that the fork's first commit, fe45f43c3, took over from the
original's history before them (it was made from an earlier tree of the
original, whose translation tools and licence notices the tags hold at other
paths): build/reuse-original-paths.txt lists them, one a line. A file of the
tree is the original's when its path is one of them, letter case aside (the
fork renamed CreateTablesMYSQL.sql and IMAPCommandLSUB.CPP to their case).

A path is not the whole story, in either direction, and two lists below, kept
by hand, say where it is not:

  DERIVED    a file the fork moved, split or copied out of one of the original's
             to a path the original's tags do not hold - its code, or a passage
             of it, kept at the new path: the tree's path, the original's file it
             came from, the fork's commit and what it keeps. The source is a path
             of the list, or <commit>:<path> for one of the original's commits
             after its tags that the fork ported (LATER). Any of his text that a
             file keeps verbatim as its own makes it one: code of his, a comment,
             a file of his test data or lines of one (a fuzzer's input that
             splices them in among others'), or a message he wrote for a case of
             his code's - one sentence of an error message or a warning included,
             as in RestApiSettings.cpp, RestApiRoutes.cpp and DateTime.cpp, even
             where the fork's sentence goes on after his, as ApiSends.cpp's after
             his "Tried %d time(s)" does. A file of the fork's that
             only names or quotes what the original does is not one: a reply or
             a message of the server's that a test expects or a document quotes,
             a setting's or a control's caption, a string table's key, an SQL
             statement of the schema, an include path; nor is one that only
             follows the original's patterns - the COM wrappers' idioms, the test
             fixtures' (the message his tests send to trip the SURBL test among
             them), the stock replies and errors his code gives wherever it
             meets the same case (an IMAP command's "Authenticate first" or
             "Folder could not be found.", a COM object's "Failed to save
             object."), what a project template or a designer writes.
  FORK_MADE  a file the fork made itself at a path the original's tags also hold,
             which keeps nothing of the original's file there: its path, the
             fork's commit and why. It is left out of the tables, and a file of
             them without the project's header is named in the fork's table of
             REUSE.toml instead.

A file moved, split or copied out of one of the original's is added to DERIVED
(git log --follow and git diff -M -C name the moves; a passage copied is found by
comparing the file with the original's, and a message by looking its words up in
the original's tags), and a file at one of the original's
paths that the fork made anew, to FORK_MADE; then this is run again.

From the list, those two and the tree this writes the tables between the two
marker lines of REUSE.toml: the original's files that carry the project's
header, which names the maintainers, and those that carry none, each with Martin
Knafve as a holder (precedence "aggregate", so that what a file says itself
stands beside it). The third parties' files the original carried are left to
their own tables (THIRD_PARTY below says which, and why), and a table further
down REUSE.toml that names one of the original's files takes the place of these
for it, so it has to name him too - which --holders holds it to.

Where the original's tags come from: the clone, or the git directory that
HM_REUSE_ORIGINAL_GIT_DIR names - build/ci/reuse-lint.sh fetches the three tags
and the fork's first commit there, without their files' contents, when the clone
has not got them, as CI's depth-1 one has not.

Usage:
  python3 build/generate-reuse-original.py             write the list (from the
                                                       tags, when they are at
                                                       hand) and the tables
  python3 build/generate-reuse-original.py --check     exit 1 when the list or
                                                       the tables are not what
                                                       this writes, or DERIVED or
                                                       FORK_MADE names what it
                                                       may not; without the tags
                                                       the tables are checked
                                                       against the list as
                                                       committed
  python3 build/generate-reuse-original.py --check --require-tags
                                                       the same, and exit 2 when
                                                       the tags are not at hand:
                                                       the reuse job's, so that
                                                       CI holds the list to them
  python3 build/generate-reuse-original.py --holders LINT.json
                                                       exit 1 when reuse lint
                                                       --json's record of one of
                                                       the original's files, but
                                                       a third party's, names no
                                                       Martin Knafve, or that of
                                                       a FORK_MADE file names him
  python3 build/generate-reuse-original.py --self-test the checks above, against
                                                       records made to break them

build/ci/reuse-lint.sh, the reuse job's script, runs --check --require-tags and
--holders, and a negative control of each. Every output is written with LF line
endings and compared with CRLF folded to LF, since the tree checks text out with
CRLF.
"""

import io
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATHS_FILE = "build/reuse-original-paths.txt"
REUSE_TOML = "REUSE.toml"
TAGS = ("hMailServer-5.7.0-b2788", "hMailServer-5.7.0-b2843", "hMailServer-5.7.0-b3013")
# The fork's first commit, made from an earlier tree of the original.
TAKEN_OVER = "fe45f43c3db6c40dfdb42c57f3db2cc312d53af1"
# A git directory that holds the tags and TAKEN_OVER, when the clone does not.
ORIGINAL_GIT_DIR = "HM_REUSE_ORIGINAL_GIT_DIR"
KNAFVE = "2005-2026 Martin Knafve / hMailServer.com"
FORK = "2026 Christopher Holloway / Progressive Robot Ltd"
LICENCE = "AGPL-3.0-or-later"
BEGIN = "# >>> build/generate-reuse-original.py writes what follows, to the line that ends it."
END = "# <<< the end of what build/generate-reuse-original.py writes."

# The third parties' files that the original carried, which are not Martin
# Knafve's and which their own tables of REUSE.toml give their holders: REUSE.toml
# path patterns ("*" any name, "**" any path), each with the reason. Each must
# still match one of the original's files, so that the list cannot outgrow them.
THIRD_PARTY = (
    ("libraries/fat_cow_icons/**", "the Fat Cow icons, FatCow's"),
    ("hmailserver/source/Tools/DBSetup/database_add.ico", "a Fat Cow icon, byte for byte"),
    ("hmailserver/source/Tools/DBUpdater/database_refresh.ico", "a Fat Cow icon, byte for byte"),
    ("hmailserver/source/Tools/DataDirectorySynchronizer/georectify.ico", "a Fat Cow icon, byte for byte"),
    ("libraries/mssqlce3.5/*.h", "SQL Server Compact's headers, Microsoft's"),
    ("libraries/License - *.rtf", "third parties' licence texts"),
    ("hmailserver/docs/Licenses/License - *", "third parties' licence texts"),
    ("hmailserver/installation/Extras/7za.exe.license.txt", "7-Zip's licence text"),
    ("hmailserver/test/TestData/DKIM/Neutral - Missing bodyhash/*", "Mail::DKIM's test corpus"),
    ("hmailserver/test/TestData/DKIM/PermFail/*", "Mail::DKIM's test corpus, and Port25's reply"),
    ("hmailserver/test/TestData/DKIM/Unsupported/*", "Mail::DKIM's test corpus"),
    ("hmailserver/test/TestData/DKIM/Good/altn.response.txt", "Alt-N's reply"),
    ("hmailserver/test/TestData/DKIM/Good/elandit.response.txt", "Eland Systems' reply"),
    ("hmailserver/test/RegressionTests/Resources/SecuniaBody1.txt", "Secunia's advisory"),
    ("hmailserver/test/SSL examples/google.cer", "the Google Internet Authority's certificate"),
)
# The original's files that reuse does not read: a file named LICENSE is a licence
# text to the REUSE Specification, not a file it needs information for; and reuse
# passes over an empty file (empty() below) as well.
NOT_READ = ("LICENSE",)

# The original's commits after its tags that the fork ported, which a DERIVED
# source may name as <commit>:<path>: the commit, and what it is.
LATER = (
    ("1beaeab9c212cfc484c180c90cab37cb46cdb6be",
     "the original's \"Replace SPF evaluator\" (its pull request 645), 13 September 2026"),
)

# Files the fork moved, split or copied out of one of the original's, at a path
# the original's tags do not hold: (the tree's path, the original's file it came
# from, the fork's commit, what the file keeps of it). The module docstring says
# what is one and what is not.
DERIVED = (
    ("hmailserver/source/Server/ExternalFetcher/ExternalFetchClientBase.cpp",
     "hmailserver/source/Server/ExternalFetcher/POP3ClientConnection.cpp", "baa827c28",
     "the half of the POP3 fetcher that makes a downloaded message a delivered one, moved out of it"),
    ("hmailserver/source/Server/ExternalFetcher/ExternalFetchClientBase.h",
     "hmailserver/source/Server/ExternalFetcher/POP3ClientConnection.h", "baa827c28",
     "the declarations of that half, moved out of it"),
    ("hmailserver/source/Server/IMAP/IMAPMove.cpp",
     "hmailserver/source/Server/IMAP/IMAPCopy.cpp", "fe45f43c3",
     "COPY's argument parsing and change notification, for MOVE"),
    ("hmailserver/source/Server/IMAP/IMAPCommandMove.cpp",
     "hmailserver/source/Server/IMAP/IMAPCommandCopy.cpp", "fe45f43c3",
     "the COPY command's checks and parameters, for MOVE"),
    ("hmailserver/source/Tools/ImportTool/Program.cs",
     "hmailserver/source/Migration/Mercury/Program.cs", "b19ccba0b",
     "the Mercury migration tool's entry point, which the Import Tool's replaced (git's rename, 50%)"),
    ("hmailserver/test/LinuxRegressionTests/Shims/SingletonProvider.cs",
     "hmailserver/test/RegressionTests/Shared/SingletonProvider.cs", "f18c38a04",
     "the Windows suite's SingletonProvider, its body unchanged"),
    ("hmailserver/test/LinuxRegressionTests/Shims/Infrastructure.cs",
     "hmailserver/test/RegressionTests/Infrastructure/CustomAsserts.cs", "f18c38a04",
     "the Windows suite's CustomAsserts, its methods in part"),
    ("hmailserver/test/LinuxRegressionTests/Shims/TestSetup.cs",
     "hmailserver/test/RegressionTests/Shared/TestSetup.cs", "f18c38a04",
     "the Windows suite's TestSetup, its helpers in part"),
    ("hmailserver/test/LinuxRegressionTests/Shims/TestFixtureBase.cs",
     "hmailserver/test/RegressionTests/Shared/TestFixtureBase.cs", "f18c38a04",
     "the Windows suite's TestFixtureBase, its shape and its set-up"),
    ("hmailserver/test/RegressionTests/API/EventsJavaScript.cs",
     "hmailserver/test/RegressionTests/API/Events.cs", "d8308e04b",
     "the event tests of Events.cs, in its order, with their scripts in JavaScript"),
    ("hmailserver/source/Server/Common/Util/InputCostTester.cpp",
     "hmailserver/source/Server/Common/AntiSpam/DKIM/Canonicalization.cpp", "53e95b832",
     "the DKIM canonicalisation's header functions, kept as 6.3.5 had them"),
    ("hmailserver/source/Server/Common/Persistence/MailboxMove.cpp",
     "hmailserver/source/Server/Common/Persistence/PersistentAccount.cpp", "ee1365651",
     "the original's refusal to rename a mailbox while message paths are full ones, in its words"),
    ("hmailserver/source/Server/Common/Util/RestApiSettings.cpp",
     "hmailserver/source/Server/COM/InterfaceSettings.cpp", "2baeafbd8",
     "the original's refusal of a hierarchy delimiter that a folder's name holds, in its words"),
    ("hmailserver/source/Server/Common/Util/RestApiMailbox.cpp",
     "hmailserver/source/Server/IMAP/IMAPCommandRename.cpp", "d5a14a7b9",
     "the IMAP commands' refusals to create, rename or delete a folder, in their words: RENAME's six, "
     "CREATE's three and DELETE's two, and the first sentence of the COM IMAPFolder's refusal to delete one"),
    ("hmailserver/source/Server/Common/Util/RestApiRoutes.cpp",
     "hmailserver/source/Server/COM/InterfaceAccount.cpp", "c88f41983",
     "the original's refusal of an auto-reply expiry date that is not a date, in its words"),
    ("hmailserver/source/Server/Common/Util/ApiSends.cpp",
     "hmailserver/source/Server/SMTP/ExternalDelivery.cpp", "e3221b3cc",
     "the original's line on how many times delivery was tried, in its words, at the head of the reason given "
     "for a deferred recipient"),
    ("hmailserver/source/Server/Common/Persistence/MessageStore.cpp",
     "hmailserver/source/Server/Common/Util/File.cpp", "236f25a94",
     "File's errors for a file not opened or not read, and FileUtilities' for one it cannot open, in their words"),
    ("hmailserver/source/Server/Common/Util/DateTime.cpp",
     "hmailserver/source/Server/Common/Util/VariantDateTime.cpp", "9623846e5",
     "VariantDateTime's error 5160 for a value that is not a date, in its words (the original's own, which its "
     "Errors.txt lists), in the class written anew in its place"),
    ("hmailserver/source/Server/Common/SQL/ScriptTransaction.cpp",
     "hmailserver/source/Server/COM/InterfaceDatabase.cpp", "1cbe558e0",
     "the COM Database object's errors for a statement that failed and for no transaction, in their words, "
     "with the transaction it held"),
    ("hmailserver/source/Server/Common/Scripting/ScriptBinding.cpp",
     "hmailserver/source/Server/COM/InterfaceMessage.cpp", "d8308e04b",
     "three of the COM Message object's errors, in their words, for the event scripts' Message in JavaScript"),
    ("hmailserver/source/Tools/ControlPanel/Services/ConfigurationChecks.cs",
     "hmailserver/source/Tools/Administrator/Main panes/ucStatus.cs", "6e1860333",
     "the Administrator's configuration warnings, the one for a banned localhost in its words"),
    ("hmailserver/source/Tools/ControlPanel/Resources/Strings.resx",
     "hmailserver/source/Tools/Administrator/Main panes/ucStatus.cs", "4ef456f16",
     "the Administrator's warning for a banned localhost, in its words, as an English text the Control Panel shows"),
    ("hmailserver/source/Server/ExternalFetcher/IMAPClientConnection.cpp",
     "hmailserver/source/Server/ExternalFetcher/POP3ClientConnection.cpp", "31fe9e6db",
     "the POP3 fetcher's connection handling - its time-out, OnConnected and OnHandshakeCompleted - and its "
     "handling of a download's buffer, of its delivery and of the days a message is kept, for IMAP"),
    ("hmailserver/test/LinuxRegressionTests/Shims/SslSetup.cs",
     "hmailserver/test/RegressionTests/SSL/SslSetup.cs", "f18c38a04",
     "the Windows suite's SslSetup, its methods' names and three of its messages"),
    ("hmailserver/test/RegressionTests/AntiSpam/DKIM/ArcSealing.cs",
     "hmailserver/test/RegressionTests/AntiSpam/DKIM/Signing.cs", "619ca858e",
     "the DKIM signing tests' GetPrivateKeyFile, with its message"),
    ("hmailserver/test/RegressionTests/AntiSpam/DKIM/DualSelectorRotation.cs",
     "hmailserver/test/RegressionTests/AntiSpam/DKIM/Signing.cs", "404863f81",
     "the DKIM signing tests' GetPrivateKeyFile, with its message"),
    ("fuzz/regression/mime_message_fuzzer/new-delete-type-mismatch-mimecodebase-nonvirtual-dtor",
     "hmailserver/test/RegressionTests/Resources/EmailWith-TextPlainBody+TextHtmlBody.txt", "205388ab3",
     "the original's test message, the same text with CRLF line endings, as the MIME fuzzer's input that found a fault"),
    ("fuzz/regression/mime_decode_fuzzer/out-of-memory-mimeunicodeencoder-encodevalue",
     "hmailserver/test/RegressionTests/Messages/MessageContainingGreekSubject.txt", "8087b6dcd",
     "header lines of the original's test message, as the MIME decode fuzzer spliced them into a third party's "
     "reply that is most of the input"),
    ("hmailserver/source/Server/SMTP/SPF/README.md",
     "1beaeab9c212cfc484c180c90cab37cb46cdb6be:hmailserver/source/Server/SMTP/SPF/README.md", "f0ee7803a",
     "the port's README, its introduction and its table of files"),
    ("hmailserver/source/Server/SMTP/SPF/Conformance/README.md",
     "1beaeab9c212cfc484c180c90cab37cb46cdb6be:hmailserver/source/Server/SMTP/SPF/Conformance/README.md",
     "f0ee7803a", "the port's README of the conformance suite, most of its text"),
)

# Files the fork made itself at a path the original's tags also hold, which keep
# nothing of the original's file there: (the path, the fork's commit, why).
FORK_MADE = (
    ("hmailserver/installation/Utilities/Uninstall current version.bat", "ed8dcf97a",
     "the fork's developer script, written anew in place of the original's two commands"),
    ("hmailserver/installation/setup.bmp", "2c16cdb28",
     "the installer's wizard picture, the fork's brand in place of the original's"),
    ("hmailserver/source/Server/Common/AntiSpam/DMARC/SpamTestDMARC.cpp", "fe45f43c3",
     "the fork's DMARC test, made before the original's own at this path (e5352cb9a, 30 August 2026)"),
    ("hmailserver/source/Server/Common/AntiSpam/DMARC/SpamTestDMARC.h", "fe45f43c3",
     "its declaration, made before the original's own at this path (e5352cb9a, 30 August 2026)"),
    ("hmailserver/test/RegressionTests/IMAP/SpecialUse.cs", "5c6a9827d",
     "the fork's tests, made four days before the original's own at this path (239f7364b, 17 August 2026)"),
    ("hmailserver/test/SSL examples/WithPassword/server.crt", "fe45f43c3",
     "a new self-signed certificate in place of the original's of 2008"),
    ("hmailserver/test/SSL examples/WithPassword/server.key", "fe45f43c3",
     "a new 2048-bit key in place of the original's"),
    ("hmailserver/test/SSL examples/example.crt", "fe45f43c3",
     "a new self-signed certificate in place of the original's of 2007"),
    ("hmailserver/test/SSL examples/example.key", "fe45f43c3",
     "a new 2048-bit key in place of the original's"),
    ("hmailserver/test/SSL examples/localhost.pfx", "fe45f43c3",
     "a new self-signed certificate for localhost and its key in place of the original's"),
)

# REUSE-IgnoreStart
HEADER_COPYRIGHT = re.compile(r"Copyright \(c\) 2026 Christopher Holloway / Progressive Robot Ltd")
HEADER_LICENCE = re.compile(r"SPDX-License-Identifier:\s*\S")
# REUSE-IgnoreEnd
HEADER_LINES = 25
LATER_SOURCE = re.compile(r"^([0-9a-f]{40}):(.+)$")


def glob_regex(pattern):
    """A REUSE.toml path pattern as a regular expression: "**" any path, "*" any
    name within one directory, everything else literal."""
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("^" + "".join(out) + "$")


THIRD_PARTY_RE = [(glob_regex(p), p, why) for p, why in THIRD_PARTY]


def third_party(path):
    for regex, pattern, why in THIRD_PARTY_RE:
        if regex.match(path):
            return pattern
    return None


def git(*args):
    return subprocess.run(["git", "-c", "core.quotepath=off"] + list(args), cwd=ROOT,
                          capture_output=True, check=True).stdout


def original_git_args():
    git_dir = os.environ.get(ORIGINAL_GIT_DIR)
    return ["--git-dir", git_dir] if git_dir else []


def git_original(*args):
    """git over the original's tags: the clone, or HM_REUSE_ORIGINAL_GIT_DIR."""
    return subprocess.run(["git", "-c", "core.quotepath=off"] + original_git_args() + list(args), cwd=ROOT,
                          capture_output=True, check=True).stdout


def git_original_ok(*args):
    return subprocess.run(["git"] + original_git_args() + list(args), cwd=ROOT, capture_output=True).returncode == 0


def tree_paths():
    return sorted(p.decode("utf-8") for p in git("ls-files", "-z").split(b"\0") if p)


def tags_present():
    return all(git_original_ok("rev-parse", "--verify", "--quiet", ref + "^{commit}") for ref in TAGS + (TAKEN_OVER,))


def paths_at(ref):
    return set(p.decode("utf-8") for p in git_original("ls-tree", "-r", "-z", "--name-only", ref).split(b"\0") if p)


def originals_from_tags():
    """The paths the three tags hold, and those the fork's first commit took over
    from the original's history before them."""
    originals = set()
    for tag in TAGS:
        originals |= paths_at(tag)
    for path in sorted(paths_at(TAKEN_OVER) - originals):
        if git_original("log", "-1", "--format=%H", TAGS[-1], "--", path).strip():
            originals.add(path)
    return sorted(originals)


def later_at_hand(commit, path):
    """None when the original's commit is not at hand; else whether it holds path
    (its trees alone are read, which a fetch without the files' contents has)."""
    if not git_original_ok("rev-parse", "--verify", "--quiet", commit + "^{commit}"):
        return None
    return bool(git_original("ls-tree", "--name-only", commit, "--", path).strip())


def render_paths(originals):
    lines = [
        "# https://www.progressiverobot.com",
        # REUSE-IgnoreStart
        "# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd",
        "# SPDX-License-Identifier: AGPL-3.0-or-later",
        # REUSE-IgnoreEnd
        "#",
        "# Generated by build/generate-reuse-original.py - do not edit; run it again.",
        "# The original hMailServer's files: every path that its tags",
        "# " + ", ".join(TAGS[:-1]) + " and " + TAGS[-1],
        "# hold, and those that the fork's first commit, " + TAKEN_OVER[:9] + ", took over from",
        "# the original's history before them. One path a line.",
    ]
    for path in originals:
        if path.startswith("#") or "\n" in path:
            sys.exit("%s: a path this list cannot hold" % path)
        lines.append(path)
    return "\n".join(lines) + "\n"


def read_text(relative):
    path = os.path.join(ROOT, relative)
    if not os.path.exists(path):
        return None
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def read_paths(text):
    return [line for line in text.split("\n") if line and not line.startswith("#")]


def empty(relative):
    """reuse reads no file of no bytes (its covered_files)."""
    path = os.path.join(ROOT, relative)
    return os.path.isfile(path) and os.path.getsize(path) == 0


def original_files(originals, tree, derived=DERIVED, fork_made=FORK_MADE):
    """The tree's files that are the original's, at their path or at it letter
    case aside, or moved or copied out of one of them (DERIVED), but for those
    the fork made at the original's paths (FORK_MADE), the third parties' files
    and those reuse does not read: (files, third parties' files)."""
    exact = set(originals)
    folded = set(p.lower() for p in originals)
    copied = set(entry[0] for entry in derived)
    made = set(entry[0] for entry in fork_made)
    mine, theirs = [], []
    for path in tree:
        if path in NOT_READ or path in made or empty(path):
            continue
        if path in exact or path.lower() in folded or path in copied:
            (theirs if third_party(path) else mine).append(path)
    return mine, theirs


def list_problems(originals, tree, derived=DERIVED, fork_made=FORK_MADE, later=LATER, at_hand=later_at_hand):
    """What is wrong with DERIVED and FORK_MADE: an entry named twice or in both, a
    file the tree has not, a DERIVED file that is one of the original's already or
    whose source is not one of the original's files, a FORK_MADE path that is not
    one of the original's. And notes: the LATER commits not at hand."""
    problems, notes = [], []
    exact = set(originals)
    folded = set(p.lower() for p in originals)
    in_tree = set(tree)
    later_ids = set(commit for commit, what in later)
    made = [entry[0] for entry in fork_made]
    seen = set()
    for path, source, commit, why in derived:
        if path in seen:
            problems.append("DERIVED names %s twice" % path)
        seen.add(path)
        if path not in in_tree:
            problems.append("DERIVED names %s, which the tree has not: take it out" % path)
        if path in exact or path.lower() in folded:
            problems.append("DERIVED names %s, which is one of the original's files already: take it out" % path)
        if path in made:
            problems.append("%s is in both DERIVED and FORK_MADE" % path)
        m = LATER_SOURCE.match(source)
        if m:
            if m.group(1) not in later_ids:
                problems.append("DERIVED's %s comes from %s, a commit LATER does not name" % (path, source))
            else:
                held = at_hand(m.group(1), m.group(2))
                if held is None:
                    notes.append("%s: the original's commit %s is not at hand; %s is taken as given"
                                 % (path, m.group(1)[:9], m.group(2)))
                elif not held:
                    problems.append("DERIVED's %s comes from %s, which that commit has not" % (path, source))
        elif source not in exact:
            problems.append("DERIVED's %s comes from %s, which is not one of the original's files (%s)"
                            % (path, source, PATHS_FILE))
    seen = set()
    for path, commit, why in fork_made:
        if path in seen:
            problems.append("FORK_MADE names %s twice" % path)
        seen.add(path)
        if path not in in_tree:
            problems.append("FORK_MADE names %s, which the tree has not: take it out" % path)
        if path not in exact and path.lower() not in folded:
            problems.append("FORK_MADE names %s, which is not one of the original's paths: it needs no entry" % path)
    return problems, notes


def carries_header(relative):
    try:
        with open(os.path.join(ROOT, relative), "rb") as f:
            head = f.read(8192).decode("latin-1")
    except OSError:
        return False
    head = "\n".join(head.splitlines()[:HEADER_LINES])
    return bool(HEADER_COPYRIGHT.search(head) and HEADER_LICENCE.search(head))


def toml_string(value):
    if "*" in value or "\\" in value or "\n" in value:
        sys.exit("%s: a path REUSE.toml would read as a pattern" % value)
    return '"' + value.replace('"', '\\"') + '"'


def render_tables(mine):
    header = set(p for p in mine if carries_header(p))
    with_header = [p for p in mine if p in header]
    without = [p for p in mine if p not in header]
    out = [
        BEGIN,
        "# The original hMailServer's files, from build/reuse-original-paths.txt: every file",
        "# of the tree at a path the original's tags hMailServer-5.7.0-b2788, -b2843 and",
        "# -b3013 hold, or that the fork's first commit, fe45f43c3, took over from the",
        "# original's history before them, letter case aside, and every file the fork moved,",
        "# split or copied out of one of them (the generator's DERIVED) - but for the files",
        "# the fork made itself at one of those paths (its FORK_MADE) and the third parties'",
        "# files among them, which their own tables below give their holders. Martin Knafve",
        "# is a holder of each (from 2005, the year of the original's earliest notices, to",
        "# 2026, that of 5.7.0), beside what the file says itself (precedence aggregate):",
        "# the first table is the files that carry the project's header, which names the",
        "# maintainers, and the second those that carry none, which names the maintainers",
        "# beside him, as debian/copyright's \"Files: *\" does. A table further down that",
        "# names one of these files takes the place of these for it, and names him too;",
        "# build/ci/reuse-lint.sh refuses a record of one of them without him.",
        "[[annotations]]",
        "path = [",
    ]
    out += ["    %s," % toml_string(p) for p in with_header]
    out += [
        "]",
        'precedence = "aggregate"',
        'SPDX-FileCopyrightText = "%s"' % KNAFVE,
        'SPDX-License-Identifier = "%s"' % LICENCE,
        "",
        "[[annotations]]",
        "path = [",
    ]
    out += ["    %s," % toml_string(p) for p in without]
    out += [
        "]",
        'precedence = "aggregate"',
        "SPDX-FileCopyrightText = [",
        '    "%s",' % KNAFVE,
        '    "%s",' % FORK,
        "]",
        'SPDX-License-Identifier = "%s"' % LICENCE,
        END,
    ]
    return "\n".join(out), len(with_header), len(without)


def splice(toml, block):
    start = toml.find(BEGIN + "\n")
    end = toml.find(END + "\n")
    if start < 0 or end < start:
        return None
    return toml[:start] + block + "\n" + toml[end + len(END) + 1:]


def stale_third_parties(theirs):
    used = set(third_party(p) for p in theirs)
    return [p for p, why in THIRD_PARTY if p not in used]


def list_difference(committed, wanted):
    """The paths one list has and the other has not: ("+" committed only, "-" the
    tags' only)."""
    have, want = set(read_paths(committed or "")), set(read_paths(wanted))
    return ["+ " + p for p in sorted(have - want)] + ["- " + p for p in sorted(want - have)]


def holders(originals, tree, lint, derived=DERIVED, fork_made=FORK_MADE):
    """What is wrong with reuse lint --json's record of the original's files, and
    of those the fork made at the original's paths."""
    mine, theirs = original_files(originals, tree, derived, fork_made)
    record = dict((f["path"], f) for f in lint.get("files", []))
    problems = []

    def names(entry):
        return "; ".join(c.get("value", "") for c in entry.get("copyrights", [])) or "nobody"

    for path in mine:
        entry = record.get(path)
        if entry is None:
            problems.append("%s: reuse lint did not read it" % path)
        elif not any("Martin Knafve" in c.get("value", "") for c in entry.get("copyrights", [])):
            problems.append("%s: the original's, and reuse records only %s" % (path, names(entry)))
    for path, commit, why in fork_made:
        entry = record.get(path)
        if entry is not None and any("Martin Knafve" in c.get("value", "") for c in entry.get("copyrights", [])):
            problems.append("%s: the fork made it (FORK_MADE), and reuse records %s" % (path, names(entry)))
    return mine, theirs, problems


def generate(check, require):
    status = 0
    have_tags = tags_present()
    committed = read_text(PATHS_FILE)
    if have_tags:
        text = render_paths(originals_from_tags())
        if committed != text:
            if check:
                print("%s is not what the original's tags give; run build/generate-reuse-original.py" % PATHS_FILE)
                difference = list_difference(committed, text)
                for line in difference[:20]:
                    print("  " + line)
                if len(difference) > 20:
                    print("  ... and %d more" % (len(difference) - 20))
                status = 1
            else:
                with io.open(os.path.join(ROOT, PATHS_FILE), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                print("wrote " + PATHS_FILE)
        committed = text
    elif require:
        print("The original's tags (%s) and the fork's first commit are not at hand, here or in %s, and "
              "--require-tags asks for them: %s cannot be held to them"
              % (", ".join(TAGS), ORIGINAL_GIT_DIR, PATHS_FILE))
        return 2
    elif committed is None:
        print("%s is missing, and this clone has not the original's tags to make it" % PATHS_FILE)
        return 2
    else:
        print("This clone has not the original's tags (%s): %s is taken as committed."
              % (", ".join(TAGS), PATHS_FILE))
    originals = read_paths(committed)
    tree = tree_paths()
    problems, notes = list_problems(originals, tree)
    for note in notes:
        print(note)
    for problem in problems:
        print(problem)
    if problems:
        print("DERIVED or FORK_MADE in build/generate-reuse-original.py names what it may not (its docstring "
              "says what each holds)")
        return 1
    mine, theirs = original_files(originals, tree)
    stale = stale_third_parties(theirs)
    for pattern in stale:
        print("THIRD_PARTY's %s matches none of the original's files: take it out" % pattern)
        status = 1
    block, with_header, without = render_tables(mine)
    toml = read_text(REUSE_TOML)
    if toml is None:
        print("There is no REUSE.toml")
        return 2
    spliced = splice(toml, block)
    if spliced is None:
        print("REUSE.toml has not the two lines between which this writes:\n  %s\n  %s" % (BEGIN, END))
        return 2
    if spliced != toml:
        if check:
            print("REUSE.toml's tables of the original's files are not what this writes; "
                  "run build/generate-reuse-original.py")
            status = 1
        else:
            with io.open(os.path.join(ROOT, REUSE_TOML), "w", encoding="utf-8", newline="\n") as f:
                f.write(spliced)
            print("wrote " + REUSE_TOML)
    if status == 0:
        print("REUSE.toml names Martin Knafve for the original's %d files (%d with the project's header, "
              "%d without; %d of them moved or copied out of the original's, and %d files the fork made at "
              "its paths left out), and leaves its %d files of third parties to their tables"
              % (len(mine), with_header, without, len(DERIVED), len(FORK_MADE), len(theirs)))
    return status


def check_holders(lint_path):
    committed = read_text(PATHS_FILE)
    if committed is None:
        print("%s is missing" % PATHS_FILE)
        return 2
    try:
        with io.open(lint_path, "r", encoding="utf-8") as f:
            lint = json.load(f)
    except (OSError, ValueError) as e:
        print("%s is not reuse lint --json's output: %s" % (lint_path, e))
        return 2
    mine, theirs, problems = holders(read_paths(committed), tree_paths(), lint)
    for problem in problems[:40]:
        print(problem)
    if len(problems) > 40:
        print("... and %d more" % (len(problems) - 40))
    if problems:
        print("%d records are wrong: a table of REUSE.toml that names one of the original's files has to name "
              "Martin Knafve too, and one the fork made at the original's paths must not "
              "(build/generate-reuse-original.py)" % len(problems))
        return 1
    print("reuse names Martin Knafve as a holder of every one of the original's %d files but the %d "
          "third parties', and of none of the %d the fork made at its paths" % (len(mine), len(theirs), len(FORK_MADE)))
    return 0


def self_test():
    failures = []

    def expect(what, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (what, got, want))

    def record(copyrights):
        return {"files": [{"path": p, "copyrights": [{"value": v} for v in vs]} for p, vs in copyrights.items()]}

    originals = ["a/One.cpp", "a/TWO.CPP", "a/Made.cpp", "libraries/fat_cow_icons/ico/x.ico", "LICENSE", "gone.txt"]
    tree = ["a/One.cpp", "a/Two.cpp", "a/New.cpp", "a/Made.cpp", "b/Moved.cpp", "libraries/fat_cow_icons/ico/x.ico",
            "LICENSE"]
    later = (("1" * 40, "a later commit of the original's"),)
    derived = (("b/Moved.cpp", "a/One.cpp", "abc", "moved out of it"),)
    fork_made = (("a/Made.cpp", "def", "made by the fork"),)
    mine, theirs = original_files(originals, tree, (), ())
    expect("the original's files, letter case aside", mine, ["a/One.cpp", "a/Two.cpp", "a/Made.cpp"])
    expect("the third parties' files", theirs, ["libraries/fat_cow_icons/ico/x.ico"])
    mine, theirs = original_files(originals, tree, derived, fork_made)
    expect("a moved file in, a file the fork made out", mine, ["a/One.cpp", "a/Two.cpp", "b/Moved.cpp"])

    def never(commit, path):
        return None

    def problems_of(derived_, fork_made_, at_hand=never):
        return list_problems(originals, tree, derived_, fork_made_, later, at_hand)[0]

    expect("good entries", problems_of(derived, fork_made), [])
    expect("a DERIVED file gone", len(problems_of((("b/Gone.cpp", "a/One.cpp", "abc", "x"),), ())), 1)
    expect("a DERIVED source not in the list",
           [p for p in problems_of((("b/Moved.cpp", "a/New.cpp", "abc", "x"),), ()) if "not one of" in p] != [], True)
    expect("a DERIVED file that is the original's already", len(problems_of((("a/One.cpp", "a/TWO.CPP", "a", "x"),), ())), 1)
    expect("a DERIVED source at a commit LATER does not name",
           len(problems_of((("b/Moved.cpp", "2" * 40 + ":a/x.cpp", "abc", "x"),), ())), 1)
    expect("a LATER source not at hand is a note, not a problem",
           list_problems(originals, tree, (("b/Moved.cpp", "1" * 40 + ":a/x.cpp", "abc", "x"),), (), later, never),
           ([], ["b/Moved.cpp: the original's commit 111111111 is not at hand; a/x.cpp is taken as given"]))
    expect("a LATER source the commit has not",
           len(problems_of((("b/Moved.cpp", "1" * 40 + ":a/x.cpp", "abc", "x"),), (), lambda c, p: False)), 1)
    expect("a LATER source the commit has", problems_of((("b/Moved.cpp", "1" * 40 + ":a/x.cpp", "abc", "x"),), (),
                                                        lambda c, p: True), [])
    expect("a FORK_MADE path not the original's", len(problems_of((), (("a/New.cpp", "d", "x"),))), 1)
    expect("a FORK_MADE path gone", len(problems_of((), (("gone.txt", "d", "x"),))), 1)
    expect("a file in both", len(problems_of((("b/Moved.cpp", "a/One.cpp", "a", "x"),), (("b/Moved.cpp", "d", "x"),))) >= 1,
           True)

    # Split, so that reuse does not take them for this file's own.
    fork = "Copy" + "right (C) " + FORK
    knafve = "SPDX-File" + "CopyrightText: " + KNAFVE
    good = {"a/One.cpp": [fork, knafve], "a/Two.cpp": [knafve], "a/New.cpp": [fork], "a/Made.cpp": [fork],
            "b/Moved.cpp": [fork, knafve], "libraries/fat_cow_icons/ico/x.ico": ["FatCow Web Hosting"]}
    expect("a record that names him", holders(originals, tree, record(good), derived, fork_made)[2], [])
    dropped = dict(good)
    dropped["a/Two.cpp"] = [fork]
    expect("a record of the original's file without him",
           [p.split(":")[0] for p in holders(originals, tree, record(dropped), derived, fork_made)[2]], ["a/Two.cpp"])
    moved = dict(good)
    moved["b/Moved.cpp"] = [fork]
    expect("a record of a moved file without him",
           [p.split(":")[0] for p in holders(originals, tree, record(moved), derived, fork_made)[2]], ["b/Moved.cpp"])
    claimed = dict(good)
    claimed["a/Made.cpp"] = [fork, knafve]
    expect("a record of a file the fork made that names him",
           [p.split(":")[0] for p in holders(originals, tree, record(claimed), derived, fork_made)[2]], ["a/Made.cpp"])
    unread = dict(good)
    del unread["a/One.cpp"]
    expect("an original's file reuse did not read",
           holders(originals, tree, record(unread), derived, fork_made)[2], ["a/One.cpp: reuse lint did not read it"])
    expect("a stale third party", "hmailserver/test/SSL examples/google.cer" in stale_third_parties(theirs), True)
    expect("a pattern's one level", bool(glob_regex("a/*.h").match("a/b/c.h")), False)
    expect("a pattern's any level", bool(glob_regex("a/**").match("a/b/c.h")), True)
    expect("the list's difference", list_difference("# c\na\nb\nfork.py\n", "# c\na\nb\nc\n"), ["+ fork.py", "- c"])

    block = "\n".join([BEGIN, "old", END])
    toml = "version = 1\n\n" + block + "\n\n# after\n"
    expect("the tables spliced in", splice(toml, "\n".join([BEGIN, "new", END])),
           "version = 1\n\n" + "\n".join([BEGIN, "new", END]) + "\n\n# after\n")
    expect("REUSE.toml without the marker lines", splice("version = 1\n", block), None)
    try:
        toml_string("a/*.cpp")
        failures.append("a path with a * was written as a pattern")
    except SystemExit:
        pass

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--holders" in argv:
        i = argv.index("--holders")
        if i + 1 >= len(argv):
            print("--holders needs reuse lint --json's output")
            return 2
        return check_holders(argv[i + 1])
    return generate("--check" in argv, "--require-tags" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
