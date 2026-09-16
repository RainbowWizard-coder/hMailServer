#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every folder-access decision goes through ACLManager, and this keeps it that way.

"May account A do X to folder F" is answered by ACLManager::CheckPermission (folder
access: with enforcement off, everything is allowed - the historical meaning of the
switch) or ACLManager::CheckDelegatedRight (rights one account grants another: with
enforcement off there is no decision-maker, so nothing is granted). The raw resolver,
GetPermissionForFolder, is for REPORTING rights (MYRIGHTS, the SELECT response); the
raw setting, GetUseIMAPACL, is for the settings plumbing and ACLManager itself; the
enforcement switch, GetAclEnforcementEnabled, gates the ACL commands and the
CAPABILITY line. A new call to any of these anywhere else is a second decision-maker,
which is how mail servers serve other people's mail - so it fails here, with the file
and the line, before it can be merged.

One more symbol is here for a neighbouring reason rather than that one: the rows a
folder stores are read through ACLManager::GetPermissionsSetOnFolder, because the
business object's own accessor skips the database read for an account-level folder
and hands back an empty list that a caller then edits and throws away.

Run from the repository root: python3 build/check-authz-choke-point.py
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Server")

# symbol -> the files (relative to hmailserver/source/Server, forward slashes) that may name it
ALLOWED = {
    "GetPermissionForFolder(": {
        "Common/Application/ACLManager.cpp",
        "Common/Application/ACLManager.h",
        "IMAP/IMAPCommandMyRights.cpp",       # reports the caller's rights
        "IMAP/IMAPCommandSelect.cpp",         # reports a delegate's rights in the SELECT response
        "IMAP/IMAPCommandSetAcl.cpp",         # a comment naming the owner rule
    },
    "GetUseIMAPACL(": {
        "Common/Application/ACLManager.cpp",
        "IMAP/IMAPConfiguration.cpp",
        "IMAP/IMAPConfiguration.h",
        "COM/InterfaceSettings.cpp",          # the setting's own property
        "Common/Util/RestApiSettings.cpp",    # the same property, over REST
    },
    # A folder's stored ACL is read through ACLManager::GetPermissionsSetOnFolder,
    # never through the business object: IMAPFolder::GetPermissions() skips the
    # database read for an account-level folder, so a caller that goes there reads
    # an empty list and edits a copy that is thrown away. That is what left the COM
    # Permissions collection blind on every account folder until 16 September 2026.
    # Written with the arrow so that the rule matches a CALL and not the prose,
    # which names it as IMAPFolder::GetPermissions().
    "->GetPermissions()": {
        "Common/Application/ACLManager.cpp",    # the one reader, which handles both kinds of folder
    },
    "GetAclEnforcementEnabled(": {
        "Common/Application/ACLManager.cpp",
        "Common/Application/ACLManager.h",
        "IMAP/IMAPConnection.cpp",            # a comment pointing at ACLManager
        "IMAP/IMAPCommandCapability.cpp",     # advertises ACL
        "IMAP/IMAPCommandGetAcl.cpp",         # the five ACL commands refuse when off
        "IMAP/IMAPCommandSetAcl.cpp",
        "IMAP/IMAPCommandDeleteAcl.cpp",
        "IMAP/IMAPCommandListRights.cpp",
        "IMAP/IMAPCommandMyRights.cpp",
        "IMAP/IMAPCommandSelect.cpp",         # gates the rights report
    },
}

# The decision functions themselves must exist, or the rule above guards nothing.
REQUIRED = [
    ("Common/Application/ACLManager.h", "static bool CheckPermission("),
    ("Common/Application/ACLManager.h", "static bool CheckDelegatedRight("),
    ("Common/Application/ACLManager.h", "static void GetReadWriteAccess("),
]


def main():
    problems = []
    for dirpath, _, files in os.walk(ROOT):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    lines = handle.read().split("\n")
            except OSError as error:
                problems.append(f"{rel}: cannot read ({error})")
                continue
            for number, line in enumerate(lines, 1):
                for symbol, allowed in ALLOWED.items():
                    if symbol in line and rel not in allowed:
                        instead = ("ACLManager::GetPermissionsSetOnFolder, which reads the rows an "
                                   "account folder has too") if symbol.startswith("->") else \
                                  ("ACLManager::CheckPermission or CheckDelegatedRight instead")
                        problems.append(f"{rel}:{number}: {symbol[:-1]} outside ACLManager - ask {instead}")

    for rel, needle in REQUIRED:
        path = os.path.join(ROOT, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            text = ""
        if needle not in text:
            problems.append(f"{rel}: {needle.rstrip('(')} is missing - the choke point this check protects")

    if problems:
        print("Authorisation choke point: %d problem(s)" % len(problems))
        for problem in problems:
            print("  " + problem)
        return 1

    print("Authorisation choke point: every folder-access decision is ACLManager's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
