#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""One spelling per name in hMailServer.idl, whatever its case.

A type library keeps a single spelling of each name, and it finds names without
regard to case: MIDL enters every interface, member and parameter name in one table
for the whole library, and a name spelled another way somewhere else takes the
spelling already there - or gives it its own. So a new member spelled like an old
name in another case renames the old one for every early-bound client: the .NET
interop TlbImp makes, VB6, C++ #import. Late-bound scripts do not notice, since
GetIDsOfNames compares without case, which is why nothing but a compile shows it.

That happened in batch41 (27 September 2026): the quarantine review added
QuarantinedMessage.Helo beside IInterfaceClient.HELO, the type library came out with
one spelling for both, and the regression suite on bench150 no longer compiled -
"'Client' does not contain a definition for 'HELO'" - as no customer's .NET program
using Client.HELO would have. The member is HeloName now, and this keeps the next
one from arriving.

The IDL's identifiers are read with its help strings, other strings and comments
taken out, and grouped without case. A group spelled more than one way fails,
unless it is one of the names below, each spelled two ways before this check
existed and left as it is for the reason given: renaming either spelling now would
change what programs already compile against. An entry the IDL no longer needs is
reported too, so that the list says what is so.

Run from the repository root: python build/check-idl-case.py
Pass --self-test to run it against mutated copies of the IDL and prove it can fail.
"""
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDL = os.path.join(ROOT, "hmailserver", "source", "Server", "hMailServer", "hMailServer.idl")

# Names hMailServer.idl spelled more than one way on 27 September 2026 (6.3.4, master
# 6bc09509d), with the spellings, and why each is left.
ALLOWED = {
    "clamwinexecutable": ({"ClamWinExecutable", "clamWinExecutable"},
                          "the property AntiVirusSettings.ClamWinExecutable and a parameter of TestClamWinScanner; "
                          "shipped for years, so either spelling is what some program compiles against"),
    "emailaddress": ({"EMailAddress", "EmailAddress"},
                     "parameters of Utilities.GetMailServer and IsValidEmailAddress, and a property; shipped as they are"),
    "hostname": ({"HostName", "Hostname"},
                 "the property HostName and a parameter of TestSpamAssassinConnection; shipped as they are"),
    "id": ({"id", "ID", "Id"},
           "id is the IDL's dispatch-id attribute, not a name in the library; ID is the objects' property and Id a "
           "parameter of SaveJournalRule and DeleteJournalRule, shipped as they are"),
    "long": ({"long", "LONG"},
             "the base type and its Windows typedef, neither of them a name the library holds"),
    "messageid": ({"MessageID", "messageID"},
                  "parameters of UtilGetFileNameByMessageID, RestoreMessage and RetrieveMessageID; shipped as they are"),
    "version": ({"Version", "version"},
                "version is the library's version attribute, not a name in it; Version is a property"),
}


def load():
    with open(IDL, encoding="utf-8", errors="replace") as f:
        return f.read()


def spellings(idl):
    """Every identifier of the IDL by its lower-case form: {name: {spelling: first line}}."""
    text = re.sub(r'helpstring\("(?:[^"\\]|\\.)*"\)', lambda m: "\n" * m.group(0).count("\n"), idl)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    found = collections.defaultdict(dict)
    for number, line in enumerate(text.split("\n"), 1):
        for match in re.finditer(r"\b[A-Za-z_][A-Za-z0-9_]*\b", line):
            word = match.group(0)
            found[word.lower()].setdefault(word, number)
    return found


def check(idl):
    problems = []
    found = spellings(idl)
    if len(found) < 1000:
        problems.append("the IDL has only %d distinct names: this check has stopped seeing the source" % len(found))
    for name, spelled in sorted(found.items()):
        if len(spelled) < 2:
            continue
        allowed = ALLOWED.get(name, (None, None))[0]
        if allowed is not None and set(spelled) == allowed:
            continue
        where = ", ".join("%s (line %d)" % (s, line) for s, line in sorted(spelled.items(), key=lambda item: item[1]))
        problems.append("%s is spelled %d ways: %s. The type library keeps one spelling, so an early-bound client "
                        "loses the other; name the new one differently" % (name, len(spelled), where))
    for name, (allowed, _) in sorted(ALLOWED.items()):
        now = set(found.get(name, {}))
        if len(now) < 2:
            problems.append("%s is on the list of names left spelled two ways, and the IDL no longer spells it so "
                            "(%s): take it off the list" % (name, ", ".join(sorted(now)) or "not at all"))
    return problems, len(found)


def self_test(idl):
    failures = []

    def expect(label, mutated, needle):
        found, _ = check(mutated)
        if not any(needle in problem for problem in found):
            failures.append("%s was not reported (got %r)" % (label, found[:3]))

    problems, _ = check(idl)
    if problems:
        failures.append("the tree's IDL passes: %s" % problems)

    member = "HRESULT HeloName([out, retval] BSTR* pVal);"
    if member not in idl:
        failures.append("QuarantinedMessage.HeloName, the member the mutations rename, is not in the IDL")
    else:
        # batch41's QuarantinedMessage.Helo beside Client.HELO.
        expect("a member spelled like another name in another case", idl.replace(member, member.replace("HeloName", "Helo"), 1),
               "helo is spelled 2 ways")
        # A third spelling of a name the list allows two of.
        expect("a new spelling of an allowed name", idl.replace(member, member.replace("HeloName", "EMailaddress"), 1),
               "emailaddress is spelled 3 ways")
        # The same text in a help string, a string or a comment is no name.
        harmless = idl.replace(member, member + ' // Helo, as in "helo"', 1).replace('helpstring("', 'helpstring("Helo ', 1)
        found, _ = check(harmless)
        if found:
            failures.append("a help string or a comment counted as a name: %s" % found[:2])
        # A parameter counts as much as a member: batch41's clash would have been one too.
        expect("a parameter spelled like a member", idl.replace("[out, retval] BSTR* pVal);", "[out, retval] BSTR* Helo);", 1),
               "helo is spelled 2 ways")

    # An allowed pair that is no longer two spellings is taken off the list.
    expect("an entry the IDL no longer needs", re.sub(r"\bhostname\b|\bHostname\b", "HostName", idl, flags=0), "hostname is on the list")
    expect("a check that has stopped seeing the source", "library x { }", "stopped seeing the source")

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    idl = load()
    if "--self-test" in sys.argv:
        return self_test(idl)

    problems, names = check(idl)
    if problems:
        print("hMailServer.idl spells a name more than one way (a type library keeps one spelling):")
        for problem in problems:
            print("  " + problem)
        return 1

    print("check-idl-case: %d names in hMailServer.idl, each spelled one way but the %d left as they shipped"
          % (names, len(ALLOWED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
