#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every place a password is chosen asks the breach check, and asks it in the right place.

The password policy's own comment said, for a year, that there was one place a
password is chosen. By September 2026 there were five - two COM properties, three
REST routes - each a copy of the same sequence, and one of them (account creation
over REST) had never applied the policy at all. The breach check was added to every
copy by reading each one, which is exactly the work the next new route will not know
it has to do. So it fails here instead, with the file and the function:

  paired     A function that asks PasswordPolicy::IsAcceptable also asks
             PasswordBreachCheck::IsAcceptable, after it. A new place a password
             is chosen starts by copying the policy call; this is what makes it
             copy the other one too.

  recorded   A function that calls PasswordHistory::Record asks the breach check
             BEFORE it. Record writes the old hash into the history and trims the
             table, and a refused change must not leave that behind.

  named      The places the policy does not reach - the administrator password,
             and account creation over REST - still ask the breach check.

  guarded    None of the call sites asks the breach check before the reuse
             history in the same function, so the rules this server decides on its
             own cost no request to somebody else's.

Run from the repository root: python3 build/check-password-choice-sites.py
Pass --self-test to run it against mutated copies and prove each rule can fail.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Server")

POLICY = "PasswordPolicy::IsAcceptable("
BREACH = "PasswordBreachCheck::IsAcceptable("
REUSE = "PasswordHistory::IsReuse("
RECORD = "PasswordHistory::Record("

# (file, function) pairs that must ask the breach check whether or not they ask the policy.
NAMED = [
    ("COM/InterfaceSettings.cpp", "InterfaceSettings::SetAdministratorPassword"),
    ("Common/Util/RestApiServer.cpp", "RestApiServer::HandleCreateAccount_"),
    ("COM/InterfaceAccount.cpp", "InterfaceAccount::put_Password"),
    ("COM/InterfaceAppPassword.cpp", "InterfaceAppPassword::SetPassword"),
    ("Common/Util/RestApiRoutes.cpp", "RestApiServer::HandleUpdateAccount_"),
    ("Common/Util/RestApiServer.cpp", "RestApiServer::HandleMePassword_"),
    ("Common/Util/RestApiAccountResources.cpp", "CreateAppPassword"),
]

# The files that define the symbols rather than call them.
DEFINITIONS = {
    "Common/Util/PasswordPolicy.cpp",
    "Common/Util/PasswordBreachCheck.cpp",
    "Common/Util/PasswordHistory.cpp",
}

HEADER_NAME = re.compile(r"([A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)?)\s*\(")


def strip_comments(text):
    """Comments and string literals blanked to spaces, lengths and lines kept."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append('"' + " " * (min(j, n) - i - 1) + '"')
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(text):
    """(name, body) for every function whose body opens with a brace alone on its
    line at an indentation of at most three spaces - the layout of every
    definition in this tree - with the name read from the header lines above it."""
    lines = text.split("\n")
    result = []
    i = 0
    while i < len(lines):
        line = lines[i].rstrip("\r")
        m = re.match(r"^( {0,3})\{\s*$", line)
        if not m:
            i += 1
            continue
        indent = m.group(1)
        header = ""
        k = i - 1
        while k >= 0 and len(header) < 400:
            h = lines[k].rstrip("\r")
            if not h.strip():
                break
            header = h + " " + header
            if HEADER_NAME.search(h) and not h.strip().startswith(":"):
                break
            k -= 1
        names = HEADER_NAME.findall(header)
        name = names[0] if names else ""
        # A namespace, a class or an initialiser is not a function: look inside it.
        if not name or re.search(r"\b(namespace|class|struct|enum)\b", header) or "=" in header.split("(")[0]:
            i += 1
            continue
        j = i + 1
        closing = re.compile(r"^" + indent + r"\}")
        while j < len(lines) and not closing.match(lines[j].rstrip("\r")):
            j += 1
        result.append((name, "\n".join(lines[i:j + 1])))
        i = j + 1
    return result


def check(sources):
    """sources: {relative path: text}. Returns the list of failures."""
    failures = []
    seen = set()

    for rel in sorted(sources):
        if rel in DEFINITIONS:
            continue
        code = strip_comments(sources[rel])
        if POLICY not in code and BREACH not in code and RECORD not in code:
            continue
        for name, body in functions(code):
            seen.add((rel, name)) if BREACH in body else None
            policy = body.find(POLICY)
            breach = body.find(BREACH)
            reuse = body.find(REUSE)
            record = body.find(RECORD)

            if policy >= 0 and (breach < 0 or breach < policy):
                failures.append("%s: %s asks the password policy and not the breach check after it" % (rel, name))
            if record >= 0 and (breach < 0 or breach > record):
                failures.append("%s: %s records the password history without asking the breach check first" % (rel, name))
            if reuse >= 0 and breach >= 0 and breach < reuse:
                failures.append("%s: %s asks the breach check before the reuse history, so a reused password costs a request" % (rel, name))

    for rel, name in NAMED:
        if rel not in sources:
            failures.append("%s: missing, and %s is a place a password is chosen" % (rel, name))
        elif (rel, name) not in seen:
            failures.append("%s: %s no longer asks the breach check" % (rel, name))

    return failures


def load():
    sources = {}
    for folder, _dirs, files in os.walk(ROOT):
        if any(part in ("x64", "Debug", "Release", ".vs", "obj", "bin") for part in folder.split(os.sep)):
            continue
        for f in files:
            if f.endswith(".cpp"):
                path = os.path.join(folder, f)
                rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="replace") as handle:
                    sources[rel] = handle.read()
    return sources


def self_test(sources):
    """Each rule, broken on purpose in a copy, must be reported."""
    ok = True

    def expect(label, mutated, needle):
        nonlocal ok
        failures = check(mutated)
        if not any(needle in f for f in failures):
            print("SELF-TEST FAILED: %s was not reported (got %r)" % (label, failures))
            ok = False
        else:
            print("self-test: %s is reported" % label)

    rel = "COM/InterfaceAccount.cpp"
    text = sources[rel]
    call = re.search(r"\n[^\n]*PasswordBreachCheck::IsAcceptable\([^\n]*\n[^\n]*\n", text)
    copy = dict(sources)
    copy[rel] = text[:call.start()] + "\n" + text[call.end():]
    expect("a dropped breach check beside the policy", copy, "asks the password policy and not the breach check")
    expect("a dropped breach check before the history", copy, "records the password history without asking")

    rel = "COM/InterfaceSettings.cpp"
    text = sources[rel]
    call = re.search(r"\n[^\n]*PasswordBreachCheck::IsAcceptable\([^\n]*\n[^\n]*\n", text)
    copy = dict(sources)
    copy[rel] = text[:call.start()] + "\n" + text[call.end():]
    expect("the administrator password unchecked", copy, "SetAdministratorPassword no longer asks")

    rel = "Common/Util/RestApiRoutes.cpp"
    text = sources[rel]
    record = text.find("PasswordHistory::Record(account);")
    breach = text.find("PasswordBreachCheck::IsAcceptable(", text.find("RestApiServer::HandleUpdateAccount_("))
    copy = dict(sources)
    copy[rel] = text[:breach] + "PasswordHistory::Record(account); " + text[breach:record] + "/* moved */" + text[record + len("PasswordHistory::Record(account);"):]
    expect("the history recorded before the breach check", copy, "records the password history without asking the breach check first")

    return ok


def main():
    sources = load()
    if "--self-test" in sys.argv:
        return 0 if self_test(sources) else 1

    failures = check(sources)
    if failures:
        print("Every place a password is chosen must ask PasswordBreachCheck::IsAcceptable, after the")
        print("policy and the reuse history and before PasswordHistory::Record:")
        for f in failures:
            print("  " + f)
        return 1

    print("check-password-choice-sites: %d places a password is chosen, each asking the breach check in order" % len(NAMED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
