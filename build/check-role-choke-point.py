#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every administrative decision goes through RoleManager, and this keeps it that way.

"May this credential do this, here" is answered by RoleManager::Allows and by
nothing else. Three files may ask it: RoleManager itself, the REST API's
authorisation choke point (RestApiServer.cpp), and the COM one
(COM/COMAuthentication.cpp). A call anywhere else is a second decision-maker,
which is how an administration interface ends up letting a help desk change a
setting in one place and refusing it in another - so it fails here, with the
file and the line, before it can be merged.

Four things are checked, and the last two are the ones that earn this script:

  1. RoleManager::Allows is named only in the three files above, and
     RoleManager::Encompasses - may this credential hand out, or take over,
     another grant's authority - only where a role is given or an account is
     changed; the raw role accessors (Account::GetAdminLevel, RoleGrant::Holds)
     are named only where a grant is BUILT or REPORTED, never where one is judged.

  2. The decision functions exist. Without them the rule above guards nothing.

  3. EVERY RouteKind of the REST API is classified. A route must appear in
     RestApiServer::PermissionFor_, or in IsSelfServiceRoute_ (the account's own
     endpoints, which the roles do not reach), or in IsSessionRoute_ (the three
     that are about the credential rather than the server). A route in none of
     them is a route nobody decided about - which is exactly what "a route that
     decides for itself" looks like in this codebase, since Authorize_ would
     then fall through its switch. Adding one and not classifying it fails here.

  4. THE AUDITOR INVARIANT. Every route IsMutatingRoute_ calls mutating needs a
     permission the auditor role does not hold, and every other administrative
     route needs one it does. That is what makes "an auditor reads everything
     and changes nothing" a property of the code rather than a claim about it,
     and it is checked by reading the four built-in roles out of RoleManager.cpp
     and the classification out of RestApiServer.cpp - the two files that would
     otherwise be free to drift apart.

     The API-key routes are the documented exception: they are refused to every
     credential but the administrator password in Authorize_ before any
     permission is consulted, so their classification (manage_roles) is about
     the administrator password and not about what a read-only key may read.

Run from the repository root: python3 build/check-role-choke-point.py
Self-test (proves each rule fires): python3 build/check-role-choke-point.py --self-test
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Server")

REST_H = "Common/Util/RestApiServer.h"
REST_CPP = "Common/Util/RestApiServer.cpp"
ROLE_CPP = "Common/Application/RoleManager.cpp"
ROLE_H = "Common/Application/RoleManager.h"
COM_CPP = "COM/COMAuthentication.cpp"

# symbol -> the files (relative to hmailserver/source/Server, forward slashes)
# that may name it
ALLOWED = {
    "RoleManager::Allows(": {
        ROLE_CPP,
        ROLE_H,
        REST_CPP,     # the REST choke point, Authorize_
        COM_CPP,      # the COM one, COMAuthentication
    },
    # The other half of the decision: may this credential hand out, or take
    # over, the authority another grant holds. Named where a role is given or
    # taken away and where an account is changed or its password set.
    "RoleManager::Encompasses(": {
        ROLE_CPP,
        ROLE_H,
        "Common/Util/RestApiRoles.cpp",      # the assignment and the password reset
        "Common/Util/RestApiRoutes.cpp",     # the account update
        "COM/InterfaceApplication.cpp",      # AssignRole and ResetPassword
        "COM/InterfaceAccount.cpp",          # RoleName and Save
    },
    # The historical level. A grant is DERIVED from it in RoleManager; every
    # other reader either stores it, reports it or applies the one rule that
    # predates roles (a server administrator's account is not a domain
    # administrator's to edit). A NEW decision made from it would be a second
    # authority model beside the roles.
    "GetAdminLevel(": {
        ROLE_CPP,
        "Common/BO/Account.h",
        "Common/BO/Account.cpp",
        "Common/Persistence/PersistentAccount.cpp",
        COM_CPP,                             # the administrator stand-in, when a grant is BUILT
        "COM/InterfaceAccount.cpp",          # the property itself, and the server-admin rule
        "COM/InterfaceAccounts.cpp",
        "COM/InterfaceDistributionList.cpp",
        "Common/Util/RestApiRoutes.cpp",     # the same property, over REST
    },
    # The grant's own bit test. Asking it directly is fine where a credential
    # is deciding what it may HAND OUT (which is not the same question as what
    # it may do) and where the grant is reported; anywhere else, ask Allows.
    "grant.Holds(": {
        ROLE_CPP,
        ROLE_H,
        "Common/Util/RestApiRoles.cpp",      # GET /api/v1/session reports what the grant holds
    },
}

# The decision functions themselves must exist, or the rule above guards nothing.
REQUIRED = [
    (ROLE_H, "static bool Allows(const RoleGrant &grant, RolePermission permission, const String &domainName, String &reason);"),
    (ROLE_H, "static RoleGrant GrantForAccount("),
    (ROLE_H, "static RoleGrant GrantForApiKey("),
    (REST_H, "static RolePermission PermissionFor_(RouteKind kind);"),
    (REST_H, "static bool IsSessionRoute_(RouteKind kind);"),
    (COM_CPP, "RoleManager::Allows(grant_, RolePermission::AdministerServer)"),
    (COM_CPP, "RoleManager::Allows(grant_, RolePermission::AdministerDomain)"),
]

# Refused to every credential but the administrator password before any
# permission is consulted; see Authorize_.
API_KEY_ROUTES = {
    "RouteApiKeyUnsupported", "RouteApiKeyList", "RouteApiKeyCreate", "RouteApiKeyRevoke",
    "RouteIniSettingList", "RouteIniSettingGet", "RouteIniSettingPut", "RouteIniSettingDelete",
}


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as handle:
        return handle.read()


def function_body(text, name):
    """The text of one member function, from its definition to its closing brace."""
    marker = "RestApiServer::" + name + "(" if not name.startswith("RoleManager") else name + "("
    index = text.find(marker)
    if index < 0:
        return None
    end = text.find("\n   }\n", index)
    if end < 0:
        return None
    return text[index:end]


def route_kinds(header):
    match = re.search(r"enum RouteKind\s*\{(.*?)\n      \};", header, re.S)
    if not match:
        return []
    return re.findall(r"^\s+(Route\w+)\s*(?:=\s*\d+)?\s*,?\s*$", match.group(1), re.M)


def cases(body):
    return set(re.findall(r"case (Route\w+):", body or ""))


def permission_map(body):
    """RouteKind -> permission, read out of PermissionFor_'s switch."""
    mapping = {}
    pending = []
    for line in (body or "").split("\n"):
        label = re.match(r"\s+case (Route\w+):\s*$", line)
        if label:
            pending.append(label.group(1))
            continue
        answer = re.match(r"\s+return RolePermission::(\w+);\s*$", line)
        if answer and pending:
            for kind in pending:
                mapping[kind] = answer.group(1)
            pending = []
    return mapping


def builtin_permissions(role_cpp, function):
    """The permissions one built-in role holds, read out of its function."""
    index = role_cpp.find("unsigned __int64 " + function + "()")
    if index < 0:
        return None
    end = role_cpp.find("\n      }\n", index)
    if end < 0:
        return None
    return set(re.findall(r"Bit\(RolePermission::(\w+)\)", role_cpp[index:end]))


def check_call_sites(problems):
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
                problems.append("%s: cannot read (%s)" % (rel, error))
                continue
            for number, line in enumerate(lines, 1):
                for symbol, allowed in ALLOWED.items():
                    if symbol in line and rel not in allowed:
                        problems.append(
                            "%s:%d: %s outside RoleManager - ask RoleManager::Allows instead"
                            % (rel, number, symbol[:-1]))


def check_required(problems):
    for rel, needle in REQUIRED:
        try:
            text = read(rel)
        except OSError:
            text = ""
        if needle not in text:
            problems.append("%s: %s is missing - the choke point this check protects"
                            % (rel, needle.rstrip("(")))


def check_routes(problems, header, cpp, role_cpp):
    kinds = route_kinds(header)
    if not kinds:
        problems.append("%s: the RouteKind enumeration could not be read" % REST_H)
        return

    classified = permission_map(function_body(cpp, "PermissionFor_"))
    self_service = cases(function_body(cpp, "IsSelfServiceRoute_"))
    session = cases(function_body(cpp, "IsSessionRoute_"))
    mutating = cases(function_body(cpp, "IsMutatingRoute_"))

    if not classified:
        problems.append("%s: PermissionFor_ could not be read" % REST_CPP)
        return
    if not session:
        problems.append("%s: IsSessionRoute_ could not be read" % REST_CPP)
        return

    for kind in kinds:
        if kind in classified or kind in self_service or kind in session:
            continue
        problems.append(
            "%s: %s is in no classification - name it in PermissionFor_ (it is administered), "
            "in IsSelfServiceRoute_ (it is the account's own) or in IsSessionRoute_ "
            "(it is about the credential). A route nobody classified is a route that decides for itself."
            % (REST_H, kind))

    auditor = builtin_permissions(role_cpp, "AuditorPermissions")
    if auditor is None:
        problems.append("%s: the auditor role could not be read" % ROLE_CPP)
        return

    for kind, permission in sorted(classified.items()):
        if kind in API_KEY_ROUTES:
            continue
        held = permission in auditor
        if kind in mutating and held:
            problems.append(
                "%s: %s changes something and needs '%s', which the auditor holds - "
                "an auditor that can change something is not an auditor"
                % (REST_CPP, kind, permission))
        if kind not in mutating and not held:
            problems.append(
                "%s: %s only reads and needs '%s', which the auditor does not hold - "
                "an auditor that cannot read something is not reading everything"
                % (REST_CPP, kind, permission))

    # The two historical levels must be exactly what they were: serveradmin
    # holds everything, domainadmin holds AdministerDomain, and neither of the
    # narrow roles holds either - which is what keeps every existing COM gate
    # answering what it answered before.
    domainadmin = builtin_permissions(role_cpp, "DomainAdminPermissions")
    helpdesk = builtin_permissions(role_cpp, "HelpdeskPermissions")

    if domainadmin is None or helpdesk is None:
        problems.append("%s: the built-in roles could not be read" % ROLE_CPP)
        return

    if "AdministerDomain" not in domainadmin:
        problems.append("%s: domainadmin does not hold AdministerDomain - every COM gate that asks "
                        "GetIsDomainAdmin would start refusing a domain administrator" % ROLE_CPP)

    for role, held in (("helpdesk", helpdesk), ("auditor", auditor)):
        for level in ("AdministerServer", "AdministerDomain"):
            if level in held:
                problems.append("%s: %s holds %s, which is the whole of the authority it is "
                                "supposed to be narrower than" % (ROLE_CPP, role, level))


def self_test():
    """Each rule, fired against text written to break it."""
    failures = []

    fake_header = """
      enum RouteKind
      {
         RouteUnknown = 0,
         RouteStatus,
         RouteThingNobodyClassified
      };
"""
    fake_cpp = """
   RestApiServer::PermissionFor_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteUnknown:
      case RouteStatus:
         return RolePermission::ViewConfiguration;

      default:
         break;
      }
   }

   RestApiServer::IsSelfServiceRoute_(RouteKind kind)
   {
      switch (kind)
      {
      default:
         break;
      }
   }

   RestApiServer::IsSessionRoute_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteSessionGet:
         return true;
      }
   }

   RestApiServer::IsMutatingRoute_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteStatus:
         return true;
      }
   }
"""
    fake_roles = """
      unsigned __int64 AuditorPermissions()
      {
         unsigned __int64 set = 0;
         set |= Bit(RolePermission::ViewConfiguration);
         return set;
      }

      unsigned __int64 DomainAdminPermissions()
      {
         unsigned __int64 set = 0;
         set |= Bit(RolePermission::ViewAccounts);
         return set;
      }

      unsigned __int64 HelpdeskPermissions()
      {
         unsigned __int64 set = 0;
         set |= Bit(RolePermission::AdministerServer);
         return set;
      }
"""
    problems = []
    check_routes(problems, fake_header, fake_cpp, fake_roles)

    wanted = [
        ("RouteThingNobodyClassified", "a route in no classification is not reported"),
        ("changes something and needs", "a mutating route the auditor may reach is not reported"),
        ("domainadmin does not hold AdministerDomain", "a domainadmin without its level is not reported"),
        ("helpdesk holds AdministerServer", "a narrow role holding the full authority is not reported"),
    ]

    for needle, complaint in wanted:
        if not any(needle in problem for problem in problems):
            failures.append(complaint)

    # And the happy case: the tree as it stands must produce none of these.
    real = []
    check_routes(real, read(REST_H), read(REST_CPP), read(ROLE_CPP))
    if real:
        failures.append("the tree itself fails the route rules: " + "; ".join(real[:3]))

    if failures:
        print("Role choke point self-test: %d problem(s)" % len(failures))
        for failure in failures:
            print("  " + failure)
        return 1

    print("Role choke point self-test: every rule fires on text written to break it (%d fired)"
          % len(wanted))
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    problems = []
    check_call_sites(problems)
    check_required(problems)
    check_routes(problems, read(REST_H), read(REST_CPP), read(ROLE_CPP))

    if problems:
        print("Role choke point: %d problem(s)" % len(problems))
        for problem in problems:
            print("  " + problem)
        return 1

    print("Role choke point: every administrative decision is RoleManager's, "
          "every route is classified, and the auditor reads everything and changes nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
