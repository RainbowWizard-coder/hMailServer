#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A COM change to an item of a shared collection is written onto a copy.

Ten of the collections COM hands out are the running server's own - the SMTP
configuration's routes, incoming relays and remote domain policies, the
anti-spam configuration's DNS black lists and SURBL servers, the server
messages, the blocked attachments, the SSL certificates, the IMAP groups and a
running route's addresses - and each item handed out of one is the object every
session reads. Until 24 September 2026 their put_* methods wrote those objects'
fields in place, under the sessions reading them: a string assigned while a
session copies it frees the buffer being copied (the use-after-free of the
heap-corruption abort, one level below the vector), and a record written a
field at a time hands a reader half of each. COMCollectionItem
(COM/COMCollection.h) now copies the item at the first put_* (Writable_), and
Save() saves the copy and reads the collection again (PublishSaved_). The fix
was made by hand in ten classes and at every place that hands one of them out,
and one line written the old way would undo it without failing anything else,
so this holds it:

  In the ten item classes' .cpp files -
    written    no object_->Set...(value), nor a route setter called with a
               value (DomainName, TargetSMTPHost, TargetSMTPPort,
               NumberOfTries, MinutesBetweenTry, ToAllAddresses): a put_*
               writes Writable_() (WritableRoute_() for the route);
    assigned   no assignment to object_, which is the template's to change;
    read       no ReadObject(object_ ...) - a read of the database into the
               object - unless HoldsCopy_() guards it on its line or the line
               before, so that only the object's own copy is read into;
    appended   no AddToParentCollection(): Save publishes through
               PublishSaved_, which appends a new item and reads the list
               again for a changed one;
    saved      the class's Save() calls Writable_() (WritableRoute_()) before
               its SaveObject - a Save with nothing put before it saves a copy
               too, never the running item and never one a refresh has
               replaced - and PublishSaved_ after it.
  In every COM source -
    handed     every CComObject<> of one of the ten classes made in a function
               is attached with AttachSharedParent in that function, and never
               with AttachParent.

Comments and string and character literals are blanked before anything is
read, so the old code quoted in a comment is not taken for code.

Run from the repository root: python3 build/check-com-shared-copies.py
Pass --self-test to run it against a temporary tree with one of each case.
ARCHITECTURE.md has the rule ("A COM change to an item of a shared collection
is written onto a copy").
"""
import os
import re
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Server", "COM")

ITEMS = (
    "InterfaceRoute",
    "InterfaceRouteAddress",
    "InterfaceIncomingRelay",
    "InterfaceDNSBlackList",
    "InterfaceSURBLServer",
    "InterfaceRemoteDomainPolicy",
    "InterfaceServerMessage",
    "InterfaceBlockedAttachment",
    "InterfaceSSLCertificate",
    "InterfaceGroup",
)

WRITTEN = "a put writes the item itself"
ASSIGNED = "object_ assigned outside COMCollection.h"
READ = "the database read into the item itself"
APPENDED = "AddToParentCollection called; Save publishes through PublishSaved_"
NO_SAVE = "no Save() found"
NOT_COPIED = "Save() does not copy (Writable_) before its SaveObject"
NOT_PUBLISHED = "Save() does not call PublishSaved_ after its SaveObject"
NOT_SHARED = "handed out without AttachSharedParent"
PARENT = "handed out with AttachParent"
MISSING = "the class's source is not there"

MUTATOR = re.compile(r"\bobject_\s*->\s*(Set\w*|DomainName|TargetSMTPHost|TargetSMTPPort|NumberOfTries|MinutesBetweenTry|ToAllAddresses)\s*\(\s*[^)\s]")
ASSIGNMENT = re.compile(r"\bobject_\s*=(?!=)")
READ_INTO = re.compile(r"\bReadObject\s*\(\s*object_\b")
APPEND = re.compile(r"\bAddToParentCollection\s*\(")
WRITABLE = re.compile(r"\bWritable(?:Route)?_\s*\(")
SAVE_OBJECT = re.compile(r"\bSaveObject\s*\(")
PUBLISH = re.compile(r"\bPublishSaved_\s*\(")
MADE = re.compile(r"\bCComObject\s*<\s*(\w+)\s*>\s*\*\s*(\w+)\s*=\s*new\b")


def blank(text):
    """The code with comments and string and character literals blanked to
    spaces, every newline and every length kept, so that an offset is the same
    offset in the source."""
    out = []
    i = 0
    n = len(text)

    def spaces(chunk):
        return re.sub(r"[^\n]", " ", chunk)

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
            out.append(spaces(text[i:j]))
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            out.append(c + spaces(text[i + 1:j]) + text[j])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(code, offset):
    return code.count("\n", 0, offset) + 1


def block_end(code, start):
    """The offset of the brace that closes the block start is in."""
    depth = 0
    for i in range(start, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth < 0:
                return i
    return len(code)


def body_of(code, match):
    """The body of the function whose definition begins at match, as (start,
    end) offsets, or None when no body follows."""
    brace = code.find("{", match.end())
    if brace < 0:
        return None
    between = code[match.end():brace]
    if ";" in between:
        return None
    end = block_end(code, brace + 1)
    return brace, end


def check_item(rel, name, text):
    code = blank(text)
    lines = code.split("\n")
    raw = text.split("\n")
    failures = []

    for match in MUTATOR.finditer(code):
        number = line_of(code, match.start())
        failures.append((rel, number, WRITTEN, raw[number - 1].strip()))

    for match in ASSIGNMENT.finditer(code):
        number = line_of(code, match.start())
        failures.append((rel, number, ASSIGNED, raw[number - 1].strip()))

    for match in READ_INTO.finditer(code):
        number = line_of(code, match.start())
        here = lines[number - 1]
        before = lines[number - 2] if number >= 2 else ""
        if "HoldsCopy_()" not in here and "HoldsCopy_()" not in before:
            failures.append((rel, number, READ, raw[number - 1].strip()))

    for match in APPEND.finditer(code):
        number = line_of(code, match.start())
        failures.append((rel, number, APPENDED, raw[number - 1].strip()))

    saves = [m for m in re.finditer(r"\b" + re.escape(name) + r"\s*::\s*Save\s*\(\s*\)", code)]
    bodies = [(m, body_of(code, m)) for m in saves]
    bodies = [(m, b) for m, b in bodies if b]
    if not bodies:
        failures.append((rel, 1, NO_SAVE, name + "::Save()"))
    for match, (start, end) in bodies:
        number = line_of(code, match.start())
        body = code[start:end]
        save = SAVE_OBJECT.search(body)
        copy = WRITABLE.search(body)
        publish = PUBLISH.search(body)
        if not save or not copy or copy.start() > save.start():
            failures.append((rel, number, NOT_COPIED, raw[number - 1].strip()))
        if not save or not publish or publish.start() < save.start():
            failures.append((rel, number, NOT_PUBLISHED, raw[number - 1].strip()))

    writes = len(re.findall(r"\bWritable(?:Route)?_\s*\(\s*\)\s*->", code))
    return failures, writes


def check_handouts(rel, text, items):
    code = blank(text)
    raw = text.split("\n")
    failures = []
    handed = 0

    for match in MADE.finditer(code):
        cls, variable = match.group(1), match.group(2)
        if cls not in items:
            continue
        handed += 1
        end = block_end(code, match.end())
        scope = code[match.end():end]
        number = line_of(code, match.start())

        if not re.search(r"\b" + re.escape(variable) + r"\s*->\s*AttachSharedParent\s*\(", scope):
            failures.append((rel, number, NOT_SHARED, raw[number - 1].strip()))

        for parent in re.finditer(r"\b" + re.escape(variable) + r"\s*->\s*AttachParent\s*\(", scope):
            at = line_of(code, match.end() + parent.start())
            failures.append((rel, at, PARENT, raw[at - 1].strip()))

    return failures, handed


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read().replace("\r\n", "\n")


def check(root, require_all=True):
    failures = []
    writes = 0
    handed = 0
    saves = 0

    for name in ITEMS:
        path = os.path.join(root, name + ".cpp")
        if not os.path.exists(path):
            if require_all:
                failures.append((name + ".cpp", 0, MISSING, ""))
            continue
        found, counted = check_item(name + ".cpp", name, read(path))
        failures.extend(found)
        writes += counted
        saves += 1

    for f in sorted(os.listdir(root)):
        if not f.endswith(".cpp"):
            continue
        found, counted = check_handouts(f, read(os.path.join(root, f)), ITEMS)
        failures.extend(found)
        handed += counted

    return failures, writes, saves, handed


SELF_TEST_FILES = {
    # The old shape: a put writing the item, a getter that is not a write, and
    # a Save that neither copies nor publishes.
    "InterfaceRoute.cpp":
        "STDMETHODIMP InterfaceRoute::put_DomainName(BSTR newVal)\n"
        "{\n"
        "   object_->DomainName(newVal);\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceRoute::get_DomainName(BSTR *pVal)\n"
        "{\n"
        "   *pVal = object_->DomainName().AllocSysString();\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceRoute::Save()\n"
        "{\n"
        "   if (HM::PersistentRoute::SaveObject(object_, e, m))\n"
        "   {\n"
        "      AddToParentCollection();\n"
        "      return S_OK;\n"
        "   }\n"
        "   return S_FALSE;\n"
        "}\n",
    # The new shape, with the old one quoted in a comment and in a string.
    "InterfaceGroup.cpp":
        "// object_->SetName(sVal) is what this used to do.\n"
        "STDMETHODIMP InterfaceGroup::put_Name(BSTR sVal)\n"
        "{\n"
        "   Writable_()->SetName(sVal);\n"
        "   const char *s = \"object_->SetName(x); AddToParentCollection();\";\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceGroup::Save()\n"
        "{\n"
        "   Writable_();\n"
        "   if (HM::PersistentGroup::SaveObject(object_, e, m))\n"
        "   {\n"
        "      PublishSaved_(\"InterfaceGroup::Save\");\n"
        "      return S_OK;\n"
        "   }\n"
        "   return S_FALSE;\n"
        "}\n",
    # A Set... on the item, an assignment to object_, and a copy made after
    # the save instead of before it.
    "InterfaceDNSBlackList.cpp":
        "STDMETHODIMP InterfaceDNSBlackList::put_Score(long newVal)\n"
        "{\n"
        "   object_->SetScore(newVal);\n"
        "   object_ = other;\n"
        "   bool same = object_ == other;\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceDNSBlackList::Save()\n"
        "{\n"
        "   if (HM::PersistentDNSBlackList::SaveObject(object_))\n"
        "   {\n"
        "      Writable_();\n"
        "      PublishSaved_(\"InterfaceDNSBlackList::Save\");\n"
        "   }\n"
        "   return S_OK;\n"
        "}\n",
    # A read into the object: allowed when HoldsCopy_() guards it, refused
    # when nothing does.
    "InterfaceRemoteDomainPolicy.cpp":
        "STDMETHODIMP InterfaceRemoteDomainPolicy::Save()\n"
        "{\n"
        "   Writable_();\n"
        "   if (HM::PersistentRemoteDomainPolicy::SaveObject(object_, e, m))\n"
        "   {\n"
        "      PublishSaved_(\"InterfaceRemoteDomainPolicy::Save\");\n"
        "      return S_OK;\n"
        "   }\n"
        "   if (HoldsCopy_() && object_->GetID() != 0)\n"
        "      HM::PersistentRemoteDomainPolicy::ReadObject(object_, (long) object_->GetID());\n"
        "\n"
        "   HM::PersistentRemoteDomainPolicy::ReadObject(object_, 1);\n"
        "   return S_FALSE;\n"
        "}\n",
    # Hand-outs: one with AttachParent, one with no parent at all, one
    # attached inside a block below the one it is made in, and a class that is
    # not one of the ten.
    "InterfaceRoutes.cpp":
        "STDMETHODIMP InterfaceRoutes::get_Item(long Index, IInterfaceRoute **pVal)\n"
        "{\n"
        "   CComObject<InterfaceRoute>* pInterfaceRoute = new CComObject<InterfaceRoute>();\n"
        "   pInterfaceRoute->AttachItem(pRoute);\n"
        "   pInterfaceRoute->AttachParent(routes_, true);\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceRoutes::Add(IInterfaceRoute **pVal)\n"
        "{\n"
        "   CComObject<InterfaceRoute>* pRouteInterface = new CComObject<InterfaceRoute>();\n"
        "   pRouteInterface->AttachItem(pRoute);\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceRoutes::get_ItemByName(BSTR n, IInterfaceRoute **pVal)\n"
        "{\n"
        "   CComObject<InterfaceRoute>* p = new CComObject<InterfaceRoute>();\n"
        "   if (pRoute)\n"
        "   {\n"
        "      p->AttachItem(pRoute);\n"
        "      p->AttachSharedParent(routes_, true);\n"
        "   }\n"
        "   return S_OK;\n"
        "}\n"
        "STDMETHODIMP InterfaceRoutes::get_Account(IInterfaceAccount **pVal)\n"
        "{\n"
        "   CComObject<InterfaceAccount>* a = new CComObject<InterfaceAccount>();\n"
        "   a->AttachParent(accounts_, true);\n"
        "   return S_OK;\n"
        "}\n",
}

SELF_TEST_EXPECTED = {
    ("InterfaceRoute.cpp", 3): WRITTEN,
    ("InterfaceRoute.cpp", 11): NOT_COPIED,
    ("InterfaceRoute.cpp", 15): APPENDED,
    ("InterfaceDNSBlackList.cpp", 3): WRITTEN,
    ("InterfaceDNSBlackList.cpp", 4): ASSIGNED,
    ("InterfaceDNSBlackList.cpp", 8): NOT_COPIED,
    ("InterfaceRemoteDomainPolicy.cpp", 12): READ,
    ("InterfaceRoutes.cpp", 3): NOT_SHARED,
    ("InterfaceRoutes.cpp", 5): PARENT,
    ("InterfaceRoutes.cpp", 10): NOT_SHARED,
}

# Reported as well, beside an expectation above on the same line.
SELF_TEST_ALSO = {
    ("InterfaceRoute.cpp", 11): {NOT_PUBLISHED},
}


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as root:
        for rel, text in SELF_TEST_FILES.items():
            with open(os.path.join(root, rel), "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)

        failures, _writes, _saves, _handed = check(root, require_all=False)
        reported = {}
        for rel, number, why, _line in failures:
            reported.setdefault((rel, number), set()).add(why)

        for key, why in sorted(SELF_TEST_EXPECTED.items()):
            if why in reported.get(key, set()):
                print("self-test: %s:%d is reported, %s" % (key[0], key[1], why))
            else:
                print("SELF-TEST FAILED: %s:%d was not reported as %s (got %s)" %
                      (key[0], key[1], why, sorted(reported.get(key, set())) or "nothing"))
                ok = False

        for key in sorted(reported):
            allowed = set()
            if key in SELF_TEST_EXPECTED:
                allowed.add(SELF_TEST_EXPECTED[key])
            allowed |= SELF_TEST_ALSO.get(key, set())
            extra = reported[key] - allowed
            if extra:
                print("SELF-TEST FAILED: %s:%d was reported (%s) and should not be" % (key[0], key[1], ", ".join(sorted(extra))))
                ok = False

        for rel, label in (("InterfaceGroup.cpp", "the new shape, with the old one in a comment and a string"),):
            if any(key[0] == rel for key in reported):
                ok = False
            else:
                print("self-test: %s is not reported" % label)

        for key, label in ((("InterfaceRoute.cpp", 8), "a getter's call of a route setter's name with no value"),
                           (("InterfaceDNSBlackList.cpp", 5), "a comparison with object_"),
                           (("InterfaceRemoteDomainPolicy.cpp", 10), "a read into the object that HoldsCopy_() guards"),
                           (("InterfaceRoutes.cpp", 16), "a hand-out attached inside a block below it"),
                           (("InterfaceRoutes.cpp", 26), "a class that is not one of the ten")):
            if key in reported:
                ok = False
                print("SELF-TEST FAILED: %s is reported" % label)
            else:
                print("self-test: %s is not reported" % label)

    return ok


def main():
    if "--self-test" in sys.argv:
        return 0 if self_test() else 1

    failures, writes, saves, handed = check(ROOT)
    if failures:
        print("A COM change to an item of a shared collection is written onto a copy (COM/COMCollection.h):")
        print("a put_* writes Writable_(), Save() copies before it saves and publishes after, and every")
        print("hand-out of one of the ten classes attaches the shared collection with AttachSharedParent:")
        for rel, number, why, line in failures:
            print("  %s:%d: %s: %s" % (rel, number, why, line))
        return 1

    print("check-com-shared-copies: %d item classes, %d puts written onto a copy, %d Saves that copy and publish, "
          "%d hand-outs attaching the shared collection" % (len(ITEMS), writes, saves, handed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
