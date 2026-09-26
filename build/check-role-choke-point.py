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
     And the routes that change an account, or what is in it, with no account
     update to pass through ask whether the credential takes the account in
     (TAKES_IN): an app password issued on the server administrator's mailbox
     by its domain's administrator was a logon as the server administrator
     until 26 September 2026, because nothing required the question there.
     The question is asked for every such route in one place,
     RestApiServer::ChangesAccountByAddress_, and TAKES_IN_COMPLETE fails any
     mutating route named by an account's address that is neither there nor
     exempt for a stated reason; COM_TAKES_IN holds every COM write to what is
     in an account - its fetch accounts, rules, folders, grants, messages,
     recovery - to COMAuthentication's MayChange* questions.

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

     A route may need TWO permissions: PermissionFor_ names the first and
     RestApiServer::SecondPermissionFor_ the second, and Authorize_ asks for
     both. The invariant then holds of the pair - a route that only reads needs
     two permissions the auditor holds, and one that changes something needs at
     least one it does not - and a route named in SecondPermissionFor_ must be
     classified in PermissionFor_ as well, since a second permission can only
     narrow a route somebody already decided about.

     The second permissions that were DECIDED are pinned in SECOND_REQUIRED:
     the reports need view_message_trace beside view_configuration, because
     their senders and recipients are who mails whom. Without the pin, deleting
     the case from SecondPermissionFor_ would pass every rule above - the
     invariant only judges what is there - and the decision would be guarded
     by a regression test alone. And Authorize_ must ask for the second
     permission with an AND: the check names the whole expression, so that an
     OR, which would let either permission open the route, fails here too.

     The API-key routes are the documented exception: they are refused to every
     credential but the administrator password in Authorize_ before any
     permission is consulted, so their classification (manage_roles) is about
     the administrator password and not about what a read-only key may read.

     SCIM's reads are the second: SCIM is an identity provider's interface, and
     an identity provider reads there only to provision - it matches its users
     before it writes them - so its reads need provision as its writes do, which
     the auditor does not hold. They are refused to every credential but an API
     key in Authorize_ before any permission is consulted, and what they show
     an auditor reads at /api/v1/domains/{domain}/accounts, /aliases and /lists.
     Their writes are held to the invariant like any other route's.

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
        "Common/Util/RestApiMailboxKeys.cpp", # the mailbox key recovery, which sets a password as the reset does
        "Common/Util/RestApiRoutes.cpp",     # the account update
        "Common/Util/RestApiScim.cpp",       # SCIM's changes to an account: a key may not take over one whose authority it lacks
        "COM/InterfaceApplication.cpp",      # AssignRole and ResetPassword
        "COM/InterfaceAccount.cpp",          # RoleName, Save and MoveTo
        "COM/InterfaceUtilities.cpp",        # MoveMailbox, which is Account.MoveTo reached by address
        REST_CPP,                            # CallerTakesInAccount_, which the account's other routes ask
        COM_CPP,                             # TakesInAccount, which the app passwords ask over COM
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
        "Common/Scripting/ScriptBinding.cpp",  # the same property, read-only, to a JavaScript event script
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
    (REST_H, "static bool SecondPermissionFor_(RouteKind kind, RolePermission &permission);"),
    # Authorize_ must ask for the second permission, or naming one guards
    # nothing - and ask for it with an AND after the first, so that neither
    # permission alone opens the route. The whole expression, line by line.
    (REST_CPP, "SecondPermissionFor_(route.kind, secondPermission)"),
    (REST_CPP, "RoleManager::Allows(caller.grant, PermissionFor_(route.kind), routeDomain, roleRefusal) &&"),
    (REST_CPP, "RoleManager::Allows(caller.grant, secondPermission, routeDomain, roleRefusal));"),
    (REST_H, "static bool IsSessionRoute_(RouteKind kind);"),
    (COM_CPP, "RoleManager::Allows(grant_, RolePermission::AdministerServer)"),
    (COM_CPP, "RoleManager::Allows(grant_, RolePermission::AdministerDomain)"),
    # An app password is a mail client's credential and never a COM logon:
    # accepted there, it carried the account's whole role past its second
    # factor. Asked in both Authenticate overloads' three paths.
    (COM_CPP, "RefuseAppPassword_(viaAppPassword);"),
    # Issuing or taking back an app password over COM asks what an Account.Save
    # would have: whether the credential takes the account in.
    ("COM/InterfaceAppPasswords.cpp", "authentication_->TakesInAccount(account_id_)"),
    ("COM/InterfaceAppPassword.cpp", "authentication_->TakesInAccount(object_->GetAccountID())"),
]

# The REST handlers that change an account, or what is in it, and must ask
# whether the credential takes the account in: Authorize_ scopes them to the
# account's DOMAIN, which is not the same thing - a domain administrator's
# domain can hold the server administrator's mailbox. (file, handler, what the
# handler's body must say)
TAKES_IN = [
    ("Common/Util/RestApiAccountResources.cpp", "HandleAccountResources_",
     "IsMutatingRoute_(route.kind) && !CallerTakesInAccount_(caller, account)"),
    ("Common/Util/RestApiServer.cpp", "HandleDeleteAccount_", "!CallerTakesInAccount_(caller, account)"),
    ("Common/Util/RestApiMailboxMove.cpp", "HandleMailboxMove_", "!CallerTakesInAccount_(caller, account)"),
    # THE rule: every route that changes an account named by the address in
    # its path, asked once, after Authorize_, for every kind
    # ChangesAccountByAddress_ names - which TAKES_IN_COMPLETE below holds to
    # every such route there is.
    ("Common/Util/RestApiServer.cpp", "ProcessRequest_", "named && !CallerTakesInAccount_(caller, named)"),
    # A migration's job writes mail into its target account: a job is added,
    # and a migration or a job run, stopped, retried or removed, only by a
    # credential that takes the target in.
    ("Common/Util/RestApiMigrations.cpp", "HandleMigrations_", "!CallerTakesInAccount_(caller, account)"),
    ("Common/Util/RestApiMigrations.cpp", "HandleMigrations_", "!takesInTarget(one)"),
    ("Common/Util/RestApiMigrations.cpp", "HandleMigrations_", "!takesInTarget(job)"),
    # An inbound webhook route takes an address's mail instead of its mailbox,
    # or copies it: made, changed or removed only by a credential that takes in
    # every account the route reaches - the address's, or a routed domain's
    # administrative ones (InboundWebhookReachesAccountBeyond_, which asks
    # CallerTakesInAccount_ of each).
    ("Common/Util/RestApiInboundWebhooks.cpp", "InboundWebhookReachesAccountBeyond_",
     "!CallerTakesInAccount_(caller, account)"),
]

# The inbound webhook writes that must ask InboundWebhookReachesAccountBeyond_,
# and how many times HandleInboundWebhooks_ must ask it: the create, the update
# and the delete.
INBOUND_WEBHOOK_TAKES_IN = ("Common/Util/RestApiInboundWebhooks.cpp", "HandleInboundWebhooks_",
                            "InboundWebhookReachesAccountBeyond_(caller, hook.address)", 3)

# TAKES_IN_COMPLETE. Every route DomainNamedByRoute_ scopes by the domain of an
# ADDRESS (StringParser::ExtractDomain of the identifier) and IsMutatingRoute_
# calls mutating must be in ChangesAccountByAddress_, be one of the exemptions
# below, or name something that is not an account. A new route that changes an
# account and is in none of them is a route that forgot the question - the
# shape of every take-in finding of 26 September 2026 - and fails here.
#
# Exempt, each for its reason (RestApiServer::ChangesAccountByAddress_ says the same):
TAKES_IN_EXEMPT = {
    "RouteAccountUnlock": "letting the owner try again takes nothing over",
    "RouteAccountReviewUnlock": "letting the owner try again takes nothing over",
    "RouteAccountExport": "reads the account out and changes nothing; needs manage_roles, the server administrator's alone",
}

# Named by an address that is not an account's: a list's, an alias's, a spam
# trap's. Held to their domain by DomainNamedByRoute_, which is the whole of it.
NOT_ACCOUNT_ADDRESS_ROUTES = {
    "RouteListDelete", "RouteListUpdate", "RouteListMembers", "RouteListArchive", "RouteListArchiveGet",
    "RouteListMemberResetBounces", "RouteListMemberApprove", "RouteAliasDelete", "RouteSpamTrapDelete",
}

# The COM half of the rule: every COM write to what is in an account asks
# COMAuthentication's MayChangeAccount, MayChangeRule, MayChangeRuleOwnedBy or
# MayChangeFolder - where the write is made, since a rule's AccountID or an
# action's RuleID can be pointed elsewhere after the object was reached.
# (file, Class::Method) - the method's body must name one of them.
COM_TAKES_IN = [
    ("COM/InterfaceFetchAccount.cpp", "InterfaceFetchAccount::put_AccountID"),
    ("COM/InterfaceFetchAccount.cpp", "InterfaceFetchAccount::Save"),
    ("COM/InterfaceFetchAccount.cpp", "InterfaceFetchAccount::DownloadNow"),
    ("COM/InterfaceFetchAccount.cpp", "InterfaceFetchAccount::Delete"),
    ("COM/InterfaceFetchAccounts.cpp", "InterfaceFetchAccounts::Delete"),
    ("COM/InterfaceFetchAccounts.cpp", "InterfaceFetchAccounts::DeleteByDBID"),
    ("COM/InterfaceRule.cpp", "InterfaceRule::put_AccountID"),
    ("COM/InterfaceRule.cpp", "InterfaceRule::MoveUp"),
    ("COM/InterfaceRule.cpp", "InterfaceRule::MoveDown"),
    ("COM/InterfaceRule.cpp", "InterfaceRule::Save"),
    ("COM/InterfaceRule.cpp", "InterfaceRule::Delete"),
    ("COM/InterfaceRules.cpp", "InterfaceRules::DeleteByDBID"),
    ("COM/InterfaceRuleAction.cpp", "InterfaceRuleAction::put_RuleID"),
    ("COM/InterfaceRuleAction.cpp", "InterfaceRuleAction::Save"),
    ("COM/InterfaceRuleAction.cpp", "InterfaceRuleAction::MoveUp"),
    ("COM/InterfaceRuleAction.cpp", "InterfaceRuleAction::MoveDown"),
    ("COM/InterfaceRuleAction.cpp", "InterfaceRuleAction::Delete"),
    ("COM/InterfaceRuleActions.cpp", "InterfaceRuleActions::DeleteByDBID"),
    ("COM/InterfaceRuleActions.cpp", "InterfaceRuleActions::Delete"),
    ("COM/InterfaceRuleCriteria.cpp", "InterfaceRuleCriteria::put_RuleID"),
    ("COM/InterfaceRuleCriteria.cpp", "InterfaceRuleCriteria::Save"),
    ("COM/InterfaceRuleCriteria.cpp", "InterfaceRuleCriteria::Delete"),
    ("COM/InterfaceRuleCriterias.cpp", "InterfaceRuleCriterias::DeleteByDBID"),
    ("COM/InterfaceRuleCriterias.cpp", "InterfaceRuleCriterias::Delete"),
    ("COM/InterfaceIMAPFolder.cpp", "InterfaceIMAPFolder::Save"),
    ("COM/InterfaceIMAPFolder.cpp", "InterfaceIMAPFolder::Delete"),
    ("COM/InterfaceIMAPFolders.cpp", "InterfaceIMAPFolders::DeleteByDBID"),
    ("COM/InterfaceIMAPFolders.cpp", "InterfaceIMAPFolders::Add"),
    ("COM/InterfaceIMAPFolderPermission.cpp", "InterfaceIMAPFolderPermission::Save"),
    ("COM/InterfaceIMAPFolderPermission.cpp", "InterfaceIMAPFolderPermission::Delete"),
    ("COM/InterfaceIMAPFolderPermissions.cpp", "InterfaceIMAPFolderPermissions::Delete"),
    ("COM/InterfaceIMAPFolderPermissions.cpp", "InterfaceIMAPFolderPermissions::Add"),
    ("COM/InterfaceIMAPFolderPermissions.cpp", "InterfaceIMAPFolderPermissions::DeleteByDBID"),
    ("COM/InterfaceMessage.cpp", "InterfaceMessage::Save"),
    ("COM/InterfaceMessage.cpp", "InterfaceMessage::Copy"),
    ("COM/InterfaceMessages.cpp", "InterfaceMessages::DeleteByDBID"),
    ("COM/InterfaceMessages.cpp", "InterfaceMessages::Clear"),
    ("COM/InterfaceAccount.cpp", "InterfaceAccount::DeleteMessages"),
    ("COM/InterfaceAccount.cpp", "InterfaceAccount::RevokeDevice"),
    ("COM/InterfaceAccount.cpp", "InterfaceAccount::ClearRecoveryAddress"),
    ("COM/InterfaceAccount.cpp", "InterfaceAccount::ClearRecoveryCodes"),
    ("COM/InterfaceAccount.cpp", "InterfaceAccount::RemovePushSubscriptions"),
]

COM_TAKES_IN_QUESTIONS = ("MayChangeAccount(", "MayChangeRule(", "MayChangeRuleOwnedBy(", "MayChangeFolder(")

# The second permissions that were decided, and must stay decided. A route
# here must be named in SecondPermissionFor_ with exactly this permission;
# the invariant above cannot notice a case that was deleted.
#
#   RouteReport   GET /api/v1/reports and its senders and recipients list who
#                 mails whom, which is traffic metadata read out of the
#                 message trace and not configuration (15 September 2026).
#
#   RouteDiscoveryExportGet
#                 GET /api/v1/discovery/cases/{id}/exports/{export} hands over
#                 the custodians' mail itself, byte for byte, so reading the
#                 cases is not enough to take it away: view_mailbox beside
#                 view_discovery (24 September 2026, docs/Discovery.md). The
#                 auditor holds both. Together they reach more than
#                 view_mailbox alone does: an export carries the custodians'
#                 deleted items, what a hold kept from a rule included, which
#                 no other administrative route hands over; and an export of a
#                 search that read the archive needs view_archive as well,
#                 which the handler asks (CallerMayReadArchive_).
#
#   RouteInboundWebhookDeliveryMessage
#                 GET /api/v1/inbound-webhooks/deliveries/{id}/message hands over
#                 a message an inbound webhook kept because its endpoint never
#                 accepted it - the mail itself - so reading the routes is not
#                 enough: view_mailbox beside view_configuration (24 September
#                 2026, docs/InboundWebhooks.md). The auditor holds both.
SECOND_REQUIRED = {
    "RouteReport": "ViewMessageTrace",
    "RouteDiscoveryExportGet": "ViewMailbox",
    "RouteInboundWebhookDeliveryMessage": "ViewMailbox",
}

# Refused to every credential but the administrator password before any
# permission is consulted; see Authorize_.
API_KEY_ROUTES = {
    "RouteApiKeyUnsupported", "RouteApiKeyList", "RouteApiKeyCreate", "RouteApiKeyRevoke",
    "RouteIniSettingList", "RouteIniSettingGet", "RouteIniSettingPut", "RouteIniSettingDelete",
}

# SCIM's reads (RestApiScim.cpp, docs/Scim.md): an identity provider's
# interface, answered to an API key alone - every other credential is refused
# /scim/ in Authorize_ before any permission is consulted - and read only to
# provision, so they need provision, as SCIM's writes do. An auditor reads the
# same accounts, aliases and lists at /api/v1/domains/{domain}/accounts,
# /aliases and /lists. The exception is to the read half of the rule only: a
# SCIM write that needed a permission the auditor holds would still fail it.
SCIM_READ_ROUTES = {
    "RouteScimServiceProviderConfig", "RouteScimSchemas", "RouteScimResourceTypes",
    "RouteScimUserList", "RouteScimUserGet", "RouteScimGroupList", "RouteScimGroupGet",
    "RouteScimUnknown",
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


def second_permission_map(body):
    """RouteKind -> second permission, read out of SecondPermissionFor_'s switch."""
    mapping = {}
    pending = []
    for line in (body or "").split("\n"):
        label = re.match(r"\s+case (Route\w+):\s*$", line)
        if label:
            pending.append(label.group(1))
            continue
        answer = re.match(r"\s+permission = RolePermission::(\w+);\s*$", line)
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


def check_takes_in(problems, reader=read):
    for rel, handler, needle in TAKES_IN:
        try:
            text = reader(rel)
        except OSError:
            text = ""
        body = function_body(text, handler)
        if body is None:
            problems.append("%s: RestApiServer::%s could not be read" % (rel, handler))
        elif needle not in body:
            problems.append(
                "%s: RestApiServer::%s does not ask '%s' - it changes an account, and the account's "
                "domain being the credential's is not the account being the credential's to change"
                % (rel, handler, needle))


def check_inbound_webhook_takes_in(problems, reader=read):
    rel, handler, needle, wanted = INBOUND_WEBHOOK_TAKES_IN
    try:
        text = reader(rel)
    except OSError:
        text = ""
    body = function_body(text, handler)
    if body is None:
        problems.append("%s: RestApiServer::%s could not be read" % (rel, handler))
        return
    found = body.count(needle)
    if found < wanted:
        problems.append(
            "%s: RestApiServer::%s asks '%s' %d time(s), not %d - a route's create, update and delete each change "
            "what reaches the accounts it routes" % (rel, handler, needle, found, wanted))


def address_named_routes(cpp):
    """The route kinds DomainNamedByRoute_ scopes by the domain of an address:
    the cases whose group returns StringParser::ExtractDomain(...identifier...)."""
    body = function_body(cpp, "DomainNamedByRoute_") or ""
    named = set()
    pending = []
    for line in body.split("\n"):
        label = re.match(r"\s+case (Route\w+):", line)
        if label:
            pending.append(label.group(1))
            continue
        answer = re.match(r"\s+return (.*);", line)
        if answer and pending:
            if "ExtractDomain" in answer.group(1) and "identifier" in answer.group(1):
                named.update(pending)
            pending = []
    return named


def check_takes_in_complete(problems, cpp):
    named = address_named_routes(cpp)
    mutating = cases(function_body(cpp, "IsMutatingRoute_"))
    central = cases(function_body(cpp, "ChangesAccountByAddress_"))

    if not named:
        problems.append("%s: DomainNamedByRoute_'s address-named routes could not be read" % REST_CPP)
        return
    if not central:
        problems.append("%s: ChangesAccountByAddress_ could not be read - the take-in rule's one list" % REST_CPP)
        return

    for kind in sorted(named & mutating):
        if kind in central or kind in TAKES_IN_EXEMPT or kind in NOT_ACCOUNT_ADDRESS_ROUTES:
            continue
        problems.append(
            "%s: %s changes something named by an account's address and is not in ChangesAccountByAddress_ - "
            "a domain's administrator would reach the server administrator's mailbox in its domain by it. "
            "Name it there, or in TAKES_IN_EXEMPT here with the reason it takes nothing over"
            % (REST_CPP, kind))

    for kind in sorted(central):
        if kind not in named:
            problems.append(
                "%s: %s is in ChangesAccountByAddress_ but DomainNamedByRoute_ does not read its identifier as an "
                "address - the rule would look up an account under something that is not one" % (REST_CPP, kind))
        if kind in TAKES_IN_EXEMPT:
            problems.append("%s: %s is both asked and exempt - one of the two is wrong" % (REST_CPP, kind))


def com_method_body(text, name):
    """One COM method's body, from 'Class::Method(' to the closing brace in column 0."""
    match = re.search(re.escape(name) + r"\s*\(", text)
    if not match:
        return None
    end = text.find("\n}\n", match.start())
    if end < 0:
        end = text.find("\r\n}\r\n", match.start())
    if end < 0:
        return None
    return text[match.start():end]


def check_com_takes_in(problems, reader=read):
    for rel, method in COM_TAKES_IN:
        try:
            text = reader(rel)
        except OSError:
            text = ""
        body = com_method_body(text, method)
        if body is None:
            problems.append("%s: %s could not be read" % (rel, method))
        elif not any(question in body for question in COM_TAKES_IN_QUESTIONS):
            problems.append(
                "%s: %s writes to what is in an account without asking COMAuthentication's MayChange* - "
                "a domain's administrator would reach the server administrator's mailbox in its domain by it"
                % (rel, method))


def check_routes(problems, header, cpp, role_cpp):
    kinds = route_kinds(header)
    if not kinds:
        problems.append("%s: the RouteKind enumeration could not be read" % REST_H)
        return

    classified = permission_map(function_body(cpp, "PermissionFor_"))
    second = second_permission_map(function_body(cpp, "SecondPermissionFor_"))
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

    for kind in sorted(second):
        if kind not in classified:
            problems.append(
                "%s: %s is named in SecondPermissionFor_ and not in PermissionFor_ - a second "
                "permission narrows a route somebody classified, it does not classify one"
                % (REST_CPP, kind))

    for kind, permission in sorted(SECOND_REQUIRED.items()):
        if second.get(kind) != permission:
            problems.append(
                "%s: %s needs '%s' as its second permission and has %s - that permission was "
                "decided, not derived, and deleting the case from SecondPermissionFor_ does not undecide it"
                % (REST_CPP, kind, permission, "'%s'" % second[kind] if kind in second else "none"))

    for kind, permission in sorted(classified.items()):
        if kind in API_KEY_ROUTES:
            continue
        needed = [permission] + ([second[kind]] if kind in second else [])
        if kind in mutating and all(p in auditor for p in needed):
            problems.append(
                "%s: %s changes something and needs '%s', which the auditor holds - "
                "an auditor that can change something is not an auditor"
                % (REST_CPP, kind, "' and '".join(needed)))
        if kind not in mutating and kind not in SCIM_READ_ROUTES:
            for p in needed:
                if p not in auditor:
                    problems.append(
                        "%s: %s only reads and needs '%s', which the auditor does not hold - "
                        "an auditor that cannot read something is not reading everything"
                        % (REST_CPP, kind, p))

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
         RouteReadNeedingTwo,
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
      case RouteReadNeedingTwo:
         return RolePermission::ViewConfiguration;

      default:
         break;
      }
   }

   RestApiServer::SecondPermissionFor_(RouteKind kind, RolePermission &permission)
   {
      switch (kind)
      {
      case RouteReadNeedingTwo:
         permission = RolePermission::ViewMessageTrace;
         return true;

      case RouteSecondAndNothingElse:
         permission = RolePermission::ViewConfiguration;
         return true;

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
        ("RouteReadNeedingTwo only reads and needs 'ViewMessageTrace'",
         "a read whose second permission the auditor lacks is not reported"),
        ("RouteSecondAndNothingElse is named in SecondPermissionFor_ and not in PermissionFor_",
         "a second permission on a route nobody classified is not reported"),
        ("RouteReport needs 'ViewMessageTrace' as its second permission and has none",
         "a decided second permission missing from SecondPermissionFor_ is not reported"),
        ("domainadmin does not hold AdministerDomain", "a domainadmin without its level is not reported"),
        ("helpdesk holds AdministerServer", "a narrow role holding the full authority is not reported"),
    ]

    # A handler in TAKES_IN that no longer asks: its body without the question.
    fake_handler = """
   HttpResponse
   RestApiServer::HandleAccountResources_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<Account> account = AccountAt(String(route.identifier));
      return CreateAppPassword(bridge, account, requestBody);
   }
"""
    check_takes_in(problems, lambda rel: fake_handler)
    wanted.append(("RestApiServer::HandleAccountResources_ does not ask",
                   "a handler that changes an account without asking whether the credential takes it in is not reported"))

    # A mutating route named by an account's address that nobody put in
    # ChangesAccountByAddress_, and one put there that names no address.
    fake_takes_in = """
   RestApiServer::IsMutatingRoute_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteAccountThingNobodyAsked:
      case RouteAccountRuleCreate:
         return true;
      }
   }

   RestApiServer::ChangesAccountByAddress_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteAccountRuleCreate:
      case RouteDomainThing:
         return true;
      default:
         return false;
      }
   }

   RestApiServer::DomainNamedByRoute_(const Route &route)
   {
      switch (route.kind)
      {
      case RouteDomainThing:
         return String(route.identifier);
      case RouteAccountThingNobodyAsked:
      case RouteAccountRuleCreate:
         return StringParser::ExtractDomain(String(route.identifier));
      }
   }
"""
    check_takes_in_complete(problems, fake_takes_in)
    wanted.append(("RouteAccountThingNobodyAsked changes something named by an account's address",
                   "a mutating account route missing from ChangesAccountByAddress_ is not reported"))
    wanted.append(("RouteDomainThing is in ChangesAccountByAddress_ but DomainNamedByRoute_ does not read",
                   "a route in ChangesAccountByAddress_ that names no address is not reported"))

    # A COM write to an account's content that no longer asks.
    fake_com = """
STDMETHODIMP InterfaceFetchAccount::Save(void)
{
   try
   {
      HM::PersistentFetchAccount oPersAccount;
      return oPersAccount.SaveObject(object_) ? S_OK : S_FALSE;
   }
}
"""
    check_com_takes_in(problems, lambda rel: fake_com)
    wanted.append(("InterfaceFetchAccount::Save writes to what is in an account without asking",
                   "a COM write to an account's content without MayChange* is not reported"))

    # An inbound webhook delete that no longer asks whether the route reaches
    # an account beyond the credential.
    fake_hooks = """
   HttpResponse
   RestApiServer::HandleInboundWebhooks_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      if (InboundWebhookReachesAccountBeyond_(caller, hook.address))
         return refuseBeyond(hook.address);
      if (InboundWebhookReachesAccountBeyond_(caller, hook.address))
         return refuseBeyond(hook.address);
      InboundWebhooks::Instance()->DeleteRoute(hook.address);
   }
"""
    check_inbound_webhook_takes_in(problems, lambda rel: fake_hooks)
    wanted.append(("RestApiServer::HandleInboundWebhooks_ asks 'InboundWebhookReachesAccountBeyond_(caller, hook.address)' 2 time(s), not 3",
                   "an inbound webhook write that no longer asks the take-in question is not reported"))

    for needle, complaint in wanted:
        if not any(needle in problem for problem in problems):
            failures.append(complaint)

    # And the happy case: the tree as it stands must produce none of these.
    real = []
    check_routes(real, read(REST_H), read(REST_CPP), read(ROLE_CPP))
    check_takes_in(real)
    check_takes_in_complete(real, read(REST_CPP))
    check_com_takes_in(real)
    check_inbound_webhook_takes_in(real)
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
    check_takes_in(problems)
    check_takes_in_complete(problems, read(REST_CPP))
    check_com_takes_in(problems)
    check_inbound_webhook_takes_in(problems)
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
