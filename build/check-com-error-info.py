#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A COM class that refuses must be able to say why, and this keeps it that way.

COMError::GenerateError builds an IErrorInfo, calls SetErrorInfo and returns
0x800403E9. That text reaches a .NET caller only if the object answers S_OK when it
is asked for ISupportErrorInfo: the CLR queries for it first, and when the query
fails it throws the description away and builds the exception from the HRESULT
alone - "Exception from HRESULT: 0x800403E9", which is what an administrator sees
in place of the sentence somebody wrote for them.

That was found on InterfaceRuleCriteria on 16 August 2026 and fixed there alone.
On 16 September 2026 a gate found it again on the message trace, the accounts
collection and an app password, and a census found seventy-two classes raising
error text no caller could read - including two coclasses added the same week. So
the category is swept, and this is what stops it growing back. For every
COM/Interface*.cpp that calls COMError::GenerateError, GenerateGenericMessage or
GetAccessDenied, its header must:

  inherits   name ISupportErrorInfo among the class's bases.

  mapped     carry COM_INTERFACE_ENTRY(ISupportErrorInfo), or the QueryInterface
             the CLR makes still fails and the base is decoration.

  answers    define InterfaceSupportsErrorInfo and return S_OK for the IID the
             class's own IDispatchImpl names - S_FALSE for that IID is the same
             silence with more code. The definition may be in the header, as the
             classes swept in September 2026 have it, or in the .cpp with the
             array of IIDs the ATL wizard used to write; both are read here.

None of this touches hMailServer.idl, which does not mention ISupportErrorInfo:
the IID is Windows' own, with the system proxy and stub, so a class gains this
with three lines and no registration.

Run from the repository root: python3 build/check-com-error-info.py
Pass --self-test to run it against mutated copies and prove each rule can fail.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Server", "COM")

RAISES = ("COMError::GenerateError(", "COMError::GenerateGenericMessage(", "GetAccessDenied(")

DISPATCH = re.compile(r"IDispatchImpl<\s*\w+\s*,\s*&(IID_\w+)")
INHERITS = re.compile(r"^\s*public\s+ISupportErrorInfo\s*,?\s*$", re.M)
MAPPED = "COM_INTERFACE_ENTRY(ISupportErrorInfo)"
METHOD = "InterfaceSupportsErrorInfo"
# What follows the name in the shorter of the two shapes is one return statement;
# in the older one, an array of IIDs and a loop. Both fit in this much.
BODY = 700


def check(sources):
    """sources: {file name: text}, the .h and .cpp of hmailserver/source/Server/COM."""
    failures = []
    checked = 0

    for name in sorted(sources):
        if not name.startswith("Interface") or not name.endswith(".cpp"):
            continue

        code = sources[name]
        if not any(symbol in code for symbol in RAISES):
            continue

        header = name[:-4] + ".h"
        if header not in sources:
            failures.append("%s: refuses with error text and has no header to declare ISupportErrorInfo in" % name)
            continue

        text = sources[header]
        checked += 1

        iid = DISPATCH.search(text)
        if not iid:
            failures.append("%s: no IDispatchImpl base, so there is no IID to answer for" % header)
            continue
        iid = iid.group(1)

        if not INHERITS.search(text):
            failures.append("%s: does not inherit ISupportErrorInfo, so every refusal in %s reaches "
                            ".NET as a bare HRESULT" % (header, name))
        if MAPPED not in text:
            failures.append("%s: no %s, so the QueryInterface the CLR makes still fails" % (header, MAPPED))

        # The method is declared in the header and defined either there or in the
        # .cpp, so both are read: a body that names this class's IID and returns
        # S_OK, wherever it is written, is the answer that makes the text readable.
        where = text + "\n" + code
        bodies = [where[at:at + BODY] for at in range(len(where)) if where.startswith(METHOD, at)]
        if not bodies:
            failures.append("%s: no InterfaceSupportsErrorInfo(REFIID)" % header)
        elif not any(iid in body and "S_OK" in body for body in bodies):
            failures.append("%s: InterfaceSupportsErrorInfo does not answer S_OK for %s" % (header, iid))

    if checked == 0:
        failures.append("no COM interface was checked at all - this check has stopped seeing the source")

    return failures, checked


def load():
    sources = {}
    for name in os.listdir(ROOT):
        if name.endswith((".h", ".cpp")):
            with open(os.path.join(ROOT, name), encoding="utf-8", errors="replace") as handle:
                sources[name] = handle.read()
    return sources


def self_test(sources):
    """Each rule, broken on purpose in a copy, must be reported."""
    ok = True

    def expect(label, mutated, needle):
        nonlocal ok
        failures, _ = check(mutated)
        if not any(needle in failure for failure in failures):
            print("SELF-TEST FAILED: %s was not reported (got %r)" % (label, failures[:4]))
            ok = False
        else:
            print("self-test: %s is reported" % label)

    header = "InterfaceAccounts.h"
    text = sources[header]

    copy = dict(sources)
    copy[header] = text.replace("   public ISupportErrorInfo,\n", "", 1)
    expect("a class that does not inherit it", copy, "does not inherit ISupportErrorInfo")

    copy = dict(sources)
    copy[header] = text.replace("   COM_INTERFACE_ENTRY(ISupportErrorInfo)\n", "", 1)
    expect("a base with no map entry", copy, "the QueryInterface the CLR makes still fails")

    copy = dict(sources)
    copy[header] = text.replace("return riid == IID_IInterfaceAccounts ? S_OK : S_FALSE;",
                                "return S_FALSE;", 1)
    expect("an answer that is never S_OK", copy, "does not answer S_OK for IID_IInterfaceAccounts")

    copy = dict(sources)
    copy[header] = re.sub(r"\n\s*STDMETHOD\(InterfaceSupportsErrorInfo\).*?\n\s*\}\n", "\n", text, flags=re.S)
    expect("a class with no InterfaceSupportsErrorInfo", copy, "no InterfaceSupportsErrorInfo(REFIID)")

    copy = {name: text for name, text in sources.items() if not name.startswith("Interface")}
    expect("a check that has stopped seeing the source", copy, "has stopped seeing the source")

    return ok


def main():
    sources = load()
    if "--self-test" in sys.argv:
        return 0 if self_test(sources) else 1

    failures, checked = check(sources)
    if failures:
        print("A COM class that refuses must answer S_OK for its own IID from InterfaceSupportsErrorInfo,")
        print("or the sentence it refuses with never reaches the caller:")
        for failure in failures:
            print("  " + failure)
        return 1

    print("check-com-error-info: %d COM classes raise error text, and every one can be read" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
