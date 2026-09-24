#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""One value, three levels: every read of a setting that may be set below the
server goes through the resolver, and the marks agree with the table.

A setting a domain or an account may hold its own value of is resolved in one
place, Common/Application/EffectiveSetting - the account's own value, else its
domain's, else the server's (docs/SettingLevels.md). The failure this holds off
is the one the row was written against: a read site that asks the domain's
column, or the server's getter, straight - which is exactly how every such read
was written before the resolver existed - answers the domain's value for an
account that has one of its own, and nothing says so. So it fails here instead,
with the file and the function. The choke-point checkers' shape:

  table     The rows of EffectiveSetting.cpp's table are read: each name at most
            thirty characters (hm_settingoverrides.soname), lower-case, and
            there once; each exactly one kind - a limit (with the way it
            tightens), a gate or a preference (neither with one) - and a domain
            field marked as capped by the server only where it is a limit's.
            The kind decides the combine and who may set it, so the two cannot
            disagree with it.

  marks     A row whose server level is a property is marked with its levels in
            build/settings-levels.txt, and one whose server level is a key of the
            settings store is marked in build/ini-settings-baseline.txt's sixth
            field; and every property or key marked domain or account there is a
            row here. A setting three-level on paper and server in the code, or
            the other way round, fails.

  readers   Every call of a getter that reads one level of a three-level setting
            directly - the domain's column (GetMaxMessageSize, GetQuarantineDigestHour,
            ...), the account's (GetMessageRetentionDays), the server's
            (Configuration::GetRecallDays, IniFileSettings::GetQuotaWarningPercent,
            RateLimiter::GetServerLimits, ...) - is in a function this file lets
            read it: where the getter is defined, where a level is stored or
            written, the interfaces that report or set a level's own value, the
            resolver itself, and the few reads that are the server's by design,
            each with the reason. Anything else is refused by file and function.
            Every branch of an #if is read for these calls - a reader that only
            the POSIX build compiles is a reader - while the functions they are
            in are found from the first branch, whose braces are the ones that
            balance.

  resolved  Every pinned read site calls the resolver for its name, and every
            name in the table has a pinned read site: a site whose resolver call
            was taken out, or a name that nothing reads through the resolver, is
            refused.

  panel     The Control Panel's list of the names (Services/SettingLevels.cs) is
            the table's list, each with the table's type, fields and kind.

The check proves its own negative controls on every run, in temporary copies of
the files it mutates: a new function reading a domain's digest hour straight, the
plus-addressing read site with its resolver call swapped for the domain's
column, the same kind of reader in the #else branch of an #if (the POSIX build's),
and a gate given a way to tighten, must all be refused.

Usage: python build/check-setting-levels.py
Exits 1 listing what is wrong.
"""
import fnmatch
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
TABLE_FILE = "Common/Application/EffectiveSetting.cpp"
BASELINE = os.path.join(HERE, "ini-settings-baseline.txt")
LEVELS_FILE = os.path.join(HERE, "settings-levels.txt")
PANEL_LIST = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Services", "SettingLevels.cs")

ROW = re.compile(
    r'\{\s*"([^"]+)",\s*E::Type(\w+),\s*E::Levels(\w+),\s*E::Server(\w+),\s*"([^"]*)",\s*E::Class(\w+),\s*E::Tight(\w+),\s*'
    r'&(\w+),\s*(&\w+|nullptr),\s*(&\w+|nullptr),\s*(true|false),\s*(&\w+|nullptr),\s*(true|false),\s*(true|false),\s*&(\w+)\s*\}')

# The getters that read ONE level of a three-level setting, by name. A getter's
# name is the pin: Domain::GetSessionIdleSeconds and Configuration::GetSessionIdleSeconds
# are two levels of one setting and both are held here.
GUARDED = [
    "GetMaxMessageSize", "GetQuarantineDigest", "GetQuarantineDigestHour", "GetExternalTagSubject",
    "GetExternalTagHeader", "GetImpersonationAction", "GetSessionIdleSeconds", "GetSessionLifetimeSeconds",
    "GetSessionOnePerDevice", "GetSessionBindToAddress", "GetQuotaWarningPercent", "GetQuotaGracePercent",
    "GetQuotaSendStopPercent", "GetUsePlusAddressing", "GetCountryPolicy", "GetMessageRetentionDays",
    "GetRecallDays", "GetRecoverDeletedDays", "GetOpenPGPPortalEnabled", "GetZeroAccessAllowed",
    "GetReaderFetchAccountsMax", "GetReaderSenderAddressesMax", "GetReaderMaskedAddressesMax",
    "GetReaderOfflineDaysMax", "GetDirectoryUserEditable", "GetServerLimits", "GetAddressLimits",
]
GUARDED_CALL = re.compile(r"\b(" + "|".join(GUARDED) + r")\s*\(")

# Where a direct read is right, file by file: a function pattern (fnmatch) or
# "*" for the whole file, with the reason. A file that is not here may not read
# a guarded getter at all.
ALLOWED = {
    # Where the getters are defined, and where a level is stored, loaded, backed
    # up and validated as the column it is.
    "Common/BO/Domain.h": ("*", "defines the domain's columns"),
    "Common/BO/Domain.cpp": ("*", "the domain's own XML store and load"),
    "Common/BO/Account.h": ("*", "defines the account's columns"),
    "Common/BO/Account.cpp": ("*", "the account's own XML store and load"),
    "Common/Persistence/PersistentDomain.cpp": ("*", "writes the domain's columns"),
    "Common/Persistence/PersistentAccount.cpp": ("*", "writes the account's columns"),
    "Common/Persistence/PreSaveLimitationsCheck.cpp": ("*", "judges a domain's own columns before they are saved"),
    "Common/Application/Configuration.h": ("*", "defines the server's properties"),
    "Common/Application/Configuration.cpp": ("*", "defines the server's properties"),
    "Common/Application/IniFileSettings.h": ("*", "defines the settings store's keys"),
    "Common/Application/IniFileSettings.cpp": ("*", "defines the settings store's keys"),
    "SMTP/SMTPConfiguration.h": ("*", "defines the server's maximum message size"),
    "SMTP/SMTPConfiguration.cpp": ("*", "defines the server's maximum message size"),
    "Common/Util/RateLimiter.h": ("*", "defines the file's sending limits"),
    "Common/Util/RateLimiter.cpp": ("*", "defines the file's sending limits"),
    # The resolver.
    "Common/Application/EffectiveSetting.cpp": ("*", "the resolver"),
    # The interfaces that report and set ONE level's own value - a property of the
    # domain, the account or the server - which is what they have always done,
    # beside the effective value the resolver answers.
    "COM/InterfaceDomain.cpp": (["InterfaceDomain::get_*", "InterfaceDomain::put_*"], "the domain's own properties over COM"),
    "COM/InterfaceAccount.cpp": (["InterfaceAccount::get_*", "InterfaceAccount::put_*"], "the account's own properties over COM"),
    "COM/InterfaceSettings.cpp": (["InterfaceSettings::get_*", "InterfaceSettings::put_*"], "the server's own properties over COM"),
    "Common/Util/RestApiAdministration.cpp": (["RestApiServer::DomainEntryJson_", "ApplyDomainBody"], "the domain's own fields over REST"),
    "Common/Util/RestApiRoutes.cpp": (["AccountEntryJson", "RestApiServer::HandleUpdateAccount_"], "the account's own fields over REST"),
    "Common/Util/RestApiSettings.cpp": ("*", "the server's own settings rows over REST"),
    # What each level holds, reported beside the answer: the session policy's JSON.
    "Common/Util/SessionPolicies.cpp": (["SessionPolicies::ReadServer_", "SessionPolicies::ApplyDomain_"], "the policy document names each level's own value"),
    # The server's by design, before any account is known or as it always was -
    # docs/SettingLevels.md says so for each.
    "SMTP/SMTPConnection.cpp": (["SMTPConnection::SendEHLOKeywords_"], "the SIZE the EHLO advertises is the server's: no sender is known yet"),
    "Common/Util/RestApiServer.cpp": (["RestApiServer::CheckComposedMessage_"], "the webmail's send has always checked the server's limit alone, and the send API shares its check (docs/SendApi.md); unchanged, and said"),
}

# Every read site that resolves, and the names it must resolve.
PINNED = [
    ("SMTP/SMTPConnection.cpp", "SMTPConnection::GetMaxMessageSize_", ["max_message_size_kb"]),
    ("IMAP/IMAPCommandAppend.cpp", "IMAPCommandAppend::GetMaxMessageSize_", ["max_message_size_kb"]),
    ("IMAP/IMAPCommandAppend.cpp", "IMAPCommandAppend::GetEffectiveAppendLimitBytes", ["max_message_size_kb"]),
    ("Common/Application/QuarantineDigestTask.cpp", "QuarantineDigestTask::RunPass_", ["quarantine_digest", "quarantine_digest_hour"]),
    ("Common/Util/RestApiSenderLists.cpp", "RestApiServer::HandleMeQuarantineDigest_", ["quarantine_digest", "quarantine_digest_hour"]),
    ("Common/Util/ExternalSenderTagger.cpp", "ExternalSenderTagger::Apply", ["external_tag_subject", "external_tag_header"]),
    ("Common/Util/ImpersonationCheck.cpp", "ImpersonationCheck::Apply", ["impersonation_action"]),
    ("Common/Util/SessionPolicies.cpp", "SessionPolicies::Resolve_", ["session_idle_seconds", "session_lifetime_seconds",
                                                                      "session_one_per_device", "session_bind_to_address"]),
    ("Common/Util/QuotaPolicy.cpp", "QuotaPolicy::WarningPercent", ["quota_warning_percent"]),
    ("Common/Util/QuotaPolicy.cpp", "QuotaPolicy::GracePercent", ["quota_grace_percent"]),
    ("Common/Util/QuotaPolicy.cpp", "QuotaPolicy::SendStopPercent", ["quota_send_stop_percent"]),
    ("SMTP/PlusAddressing.cpp", "PlusAddressing::ExtractAccountAddress", ["plus_addressing_enabled"]),
    ("Common/Util/CountryBlocking.cpp", "CountryBlocking::IsLogonAllowed", ["country_policy"]),
    ("Common/Application/MailboxRetentionTask.cpp", "MailboxRetentionTask::Sweep", ["message_retention_days"]),
    ("Common/Util/MessageRecall.cpp", "MessageRecall::Days", ["recall_days"]),
    ("Common/Util/MessageRecall.cpp", "MessageRecall::Sweep", ["recall_days"]),
    ("Common/Util/RecoverableStore.cpp", "RecoverableStore::Days", ["recover_deleted_days"]),
    ("Common/Util/RecoverableStore.cpp", "RecoverableStore::Sweep", ["recover_deleted_days"]),
    ("Common/Util/RateLimiter.cpp", "RateLimiter::GetAccountLimits", ["sending_max_messages", "sending_max_recipients"]),
    ("Common/Util/RestApiOpenPgpKeys.cpp", "RestApiServer::HandleMeOpenPgpKeys_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiOpenPgpKeys.cpp", "RestApiServer::HandleMeOpenPgpKeyPut_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiOpenPgpKeys.cpp", "RestApiServer::HandleMeOpenPgpKeyDelete_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiOpenPgpKeys.cpp", "RestApiServer::HandleMeOpenPgpLookup_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiOpenPgpKeys.cpp", "RestApiServer::HandleMeOpenPgpKeyTrust_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiOpenPgpKeys.cpp", "RestApiServer::HandlePortalOpenPgpScript_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiProtectedMessages.cpp", "NotOffered", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiProtectedMessages.cpp", "RestApiServer::HandleMeProtected_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiProtectedMessages.cpp", "RestApiServer::HandleProtectedFetch_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiProtectedMessages.cpp", "RestApiServer::HandleProtectedReply_", ["openpgp_portal_enabled"]),
    ("Common/Util/RestApiZeroAccess.cpp", "RestApiServer::HandleMeZeroAccessPut_", ["openpgp_portal_enabled"]),
    ("Common/Util/ZeroAccess.cpp", "ZeroAccess::Allowed", ["zero_access_allowed"]),
    ("Common/Util/RestApiFetchAccounts.cpp", "RestApiServer::HandleMeFetchAccounts_", ["reader_fetch_accounts_max"]),
    ("Common/Util/RestApiFetchAccounts.cpp", "RestApiServer::HandleMeFetchImport_", ["reader_fetch_accounts_max"]),
    ("Common/Util/RestApiFetchAccounts.cpp", "RestApiServer::HandlePortalOAuthCallback_", ["reader_fetch_accounts_max"]),
    ("Common/Util/RestApiSenderAddresses.cpp", "RestApiServer::HandleMeSenderAddresses_", ["reader_sender_addresses_max"]),
    ("Common/Util/RestApiMaskedAddresses.cpp", "RestApiServer::HandleMeMaskedAddresses_", ["reader_masked_addresses_max"]),
    ("Common/Util/RestApiOffline.cpp", "EffectiveDays", ["reader_offline_days_max"]),
    ("Common/Util/DirectoryStore.cpp", "DirectoryStore::UserEditable", ["directory_user_editable"]),
]

RESOLVE_CALL = re.compile(r'EffectiveSetting::(?:Resolve\w*|LargestAnywhere|SmallestAnywhere)\s*\(\s*_T\(\s*"([a-z_0-9]+)"\s*\)')

KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "else", "do", "try", "defined"}

LEVEL_WORDS = {"Server": "server", "Domain": "domain", "Account": "account"}


def read(path):
    with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
        return handle.read()


def strip(text, every_branch=False):
    """Comments, string contents and preprocessor lines blanked, lengths kept.
    Only the first branch of each #if is kept unless every_branch is asked for."""
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
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append('"' + " " * (min(j, n) - i - 1) + '"')
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append("'" + " " * (min(j, n) - i - 1) + "'")
            i = j + 1
        else:
            out.append(c)
            i += 1
    code = "".join(out)
    if every_branch:
        return re.sub(r"(?m)^[ \t]*#.*$", lambda m: " " * len(m.group(0)), code)
    return first_branches(code)


def first_branches(code):
    """Preprocessor lines blanked, and every #else and #elif branch with them:
    the braces of the two branches of one #if would otherwise both be counted,
    and one function would swallow the rest of its file. The first branch is
    kept - on this server the Windows build's, which is the one read here."""
    lines = code.split("\n")
    skipping = []   # per open #if: whether its current branch is dropped
    for index, line in enumerate(lines):
        directive = re.match(r"\s*#\s*(\w+)", line)
        if directive:
            word = directive.group(1)
            if word in ("if", "ifdef", "ifndef"):
                skipping.append(False)
            elif word in ("else", "elif") and skipping:
                skipping[-1] = True
            elif word == "endif" and skipping:
                skipping.pop()
            lines[index] = " " * len(line)
            continue
        if any(skipping):
            lines[index] = " " * len(line)
    return "\n".join(lines)


def header_name(code, brace):
    """The function a brace opens, or None: the text back to the previous ; { or }."""
    j = brace - 1
    while j >= 0 and code[j] not in ";{}":
        j -= 1
    header = " ".join(code[j + 1:brace].split())
    if not header or "(" not in header:
        return None
    if re.search(r"\b(namespace|class|struct|enum|union)\b", header):
        return None
    if "=" in header.split("(")[0]:
        return None
    m = re.search(r"([A-Za-z_~][\w:~]*)\s*\(", header)
    if not m or m.group(1) in KEYWORDS:
        return None
    return m.group(1)


def function_spans(code):
    """(start, end, name) of every outermost function body."""
    spans = []
    depth = 0
    open_at = None
    start = 0
    name = None
    for i, c in enumerate(code):
        if c == "{":
            if open_at is None:
                found = header_name(code, i)
                if found:
                    open_at, start, name = depth, i, found
            depth += 1
        elif c == "}":
            depth -= 1
            if open_at is not None and depth == open_at:
                spans.append((start, i, name))
                open_at = None
    return spans


def enclosing(spans, position):
    for start, end, name in spans:
        if start <= position <= end:
            return name
    return "<file>"


def load_sources(root):
    sources = {}
    for folder, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ("x64", "Debug", "Release", ".vs", "obj", "bin")]
        for f in files:
            if f.endswith((".cpp", ".h")):
                path = os.path.join(folder, f)
                sources[os.path.relpath(path, root).replace(os.sep, "/")] = path
    return sources


def allowed(rel, function):
    rule = ALLOWED.get(rel)
    if not rule:
        return False
    patterns = rule[0]
    if patterns == "*":
        return True
    return any(fnmatch.fnmatchcase(function, p) for p in patterns)


def table_rows(text):
    rows = []
    for m in ROW.finditer(text):
        rows.append({
            "name": m.group(1), "type": m.group(2), "levels": LEVEL_WORDS.get(m.group(3), m.group(3)),
            "server_kind": m.group(4), "server_key": m.group(5), "class": m.group(6), "tight": m.group(7),
            "domain_column": m.group(10) != "nullptr", "domain_column_capped": m.group(11) == "true",
            # A level the Control Panel edits on another tab: a dedicated column,
            # which the overlay does not hold at that level.
            "domain_field": m.group(10) != "nullptr" and m.group(13) == "false",
            "account_field": m.group(12) != "nullptr" and m.group(14) == "false",
            "domain_overlay": m.group(13) == "true", "account_overlay": m.group(14) == "true",
        })
    return rows


def check(sources, contents):
    """sources: {rel: path}; contents: {rel: text} overriding what is on disk."""
    failures = []

    def text_of(rel):
        if rel in contents:
            return contents[rel]
        return read(sources[rel])

    # table
    if TABLE_FILE not in sources:
        return ["%s is missing: the resolver's table is what this checks against" % TABLE_FILE], 0, 0
    rows = table_rows(text_of(TABLE_FILE))
    if len(rows) < 10:
        failures.append("%s: only %d row(s) of the table were read; has the row's shape changed?" % (TABLE_FILE, len(rows)))
    names = [r["name"] for r in rows]
    for name in names:
        if len(name) > 30:
            failures.append("%s: %s is %d characters, and hm_settingoverrides.soname holds 30" % (TABLE_FILE, name, len(name)))
        if not re.match(r"^[a-z][a-z0-9_]*$", name):
            failures.append("%s: %s is not a lower-case name as REST spells a field" % (TABLE_FILE, name))
        if names.count(name) > 1:
            failures.append("%s: %s is in the table more than once" % (TABLE_FILE, name))
    for row in rows:
        if row["class"] not in ("Limit", "Gate", "Preference"):
            failures.append("%s: %s is of no kind the header names (%s): a limit, a gate or a preference" % (TABLE_FILE, row["name"], row["class"]))
        elif row["class"] == "Limit" and row["tight"] == "None":
            failures.append("%s: %s is a limit and says no way it tightens: a limit is held to the level above by it" % (TABLE_FILE, row["name"]))
        elif row["class"] != "Limit" and row["tight"] != "None":
            failures.append("%s: %s is a %s and says it tightens %s: only a limit tightens" % (TABLE_FILE, row["name"], row["class"].lower(), row["tight"]))
        if row["domain_column_capped"] and not (row["class"] == "Limit" and row["domain_column"]):
            failures.append("%s: %s marks its domain field capped by the server, and only a limit's domain field is" % (TABLE_FILE, row["name"]))
        if row["domain_column"] == row["domain_overlay"]:
            failures.append("%s: %s holds its domain level in %s: exactly one of a field and the overlay" % (TABLE_FILE, row["name"], "both" if row["domain_overlay"] else "neither"))
        if row["tight"] == "On" and row["type"] != "Boolean":
            failures.append("%s: %s tightens on, and is not a switch" % (TABLE_FILE, row["name"]))

    # marks
    property_marks = {}
    for line in read(LEVELS_FILE).splitlines():
        parts = line.split()
        if len(parts) == 2 and not line.startswith("#"):
            property_marks[parts[0]] = parts[1]
    ini_marks = {}
    for line in read(BASELINE).splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[0] == "Settings" and not line.startswith("#"):
            ini_marks[parts[1]] = parts[-1]

    for row in rows:
        if row["server_kind"] == "Property":
            mark = property_marks.get(row["server_key"])
            if mark != row["levels"]:
                failures.append("%s is resolved at the %s level, and build/settings-levels.txt marks its property %s %s"
                                % (row["name"], row["levels"], row["server_key"], mark or "not at all"))
        elif row["server_kind"] == "IniKey":
            mark = ini_marks.get(row["server_key"])
            if mark != row["levels"]:
                failures.append("%s is resolved at the %s level, and build/ini-settings-baseline.txt marks [Settings] %s %s"
                                % (row["name"], row["levels"], row["server_key"], mark or "not at all"))
    table_keys = dict((r["server_key"], r) for r in rows if r["server_key"])
    for key, mark in sorted(property_marks.items()):
        if mark in ("domain", "account") and (key not in table_keys or table_keys[key]["server_kind"] != "Property"):
            failures.append("build/settings-levels.txt marks %s %s, and no row of %s resolves it: a setting three-level "
                            "on paper is read for the whole server by every read site" % (key, mark, TABLE_FILE))
    for key, mark in sorted(ini_marks.items()):
        if mark in ("domain", "account") and (key not in table_keys or table_keys[key]["server_kind"] != "IniKey"):
            failures.append("build/ini-settings-baseline.txt marks [Settings] %s %s, and no row of %s resolves it"
                            % (key, mark, TABLE_FILE))

    # readers
    readers = 0
    for rel in sorted(sources):
        text = text_of(rel)
        if not GUARDED_CALL.search(text):
            continue
        code = strip(text)
        every = strip(text, every_branch=True)
        spans = None
        for m in GUARDED_CALL.finditer(every):
            if spans is None:
                spans = function_spans(code)
            function = enclosing(spans, m.start())
            readers += 1
            if not allowed(rel, function):
                line = code.count("\n", 0, m.start()) + 1
                failures.append("%s:%d: %s reads %s straight, and that getter is one level of a setting a domain or an "
                                "account may hold its own value of: resolve it with EffectiveSetting (docs/SettingLevels.md), "
                                "or - where a single level is what is meant - say why in build/check-setting-levels.py's ALLOWED"
                                % (rel, line, function, m.group(1)))

    # resolved
    pinned_names = set()
    for rel, function, wanted in PINNED:
        if rel not in sources:
            failures.append("%s: missing, and %s is where %s is read" % (rel, function, ", ".join(wanted)))
            continue
        text = text_of(rel)
        code = strip(text)
        spans = [s for s in function_spans(code) if s[2] == function]
        if not spans:
            failures.append("%s: %s is not there, and it is where %s is read through the resolver" % (rel, function, ", ".join(wanted)))
            continue
        for name in wanted:
            pinned_names.add(name)
            if name not in names:
                failures.append("%s: %s is pinned to %s, which is not a row of the table" % (rel, function, name))
            # One of the function's overloads is enough: MailboxRetentionTask::Sweep()
            # hands on to Sweep(int &). The strings are blanked in code, so the
            # resolver's argument is read from the text at the same offsets.
            if not any(any(m.group(1) == name for m in RESOLVE_CALL.finditer(text[start:end + 1])) for start, end, _ in spans):
                line = code.count("\n", 0, spans[0][0]) + 1
                failures.append("%s:%d: %s no longer resolves %s through EffectiveSetting" % (rel, line, function, name))
    for name in names:
        if name not in pinned_names:
            failures.append("%s is a row of the table and no pinned read site resolves it: a setting that may be set per "
                            "domain or account and that nothing reads that way" % name)

    # panel
    if os.path.exists(PANEL_LIST):
        shown = os.path.relpath(PANEL_LIST, ROOT)
        entries = re.findall(r'new SettingLevel\(\s*"([a-z_0-9]+)",\s*SettingLevelType\.(\w+),\s*(true|false),\s*(true|false),\s*SettingLevelClass\.(\w+)\s*\)',
                             read(PANEL_LIST))
        panel = [e[0] for e in entries]
        for name in sorted(set(names) - set(panel)):
            failures.append("%s: the Control Panel's list lacks %s" % (shown, name))
        for name in sorted(set(panel) - set(names)):
            failures.append("%s: the Control Panel's list has %s, which is not a row of the table" % (shown, name))
        by_name = dict((r["name"], r) for r in rows)
        for name, kind, domain_field, account_field, klass in entries:
            row = by_name.get(name)
            if not row:
                continue
            expected = (row["type"], "true" if row["domain_field"] else "false", "true" if row["account_field"] else "false", row["class"])
            if (kind, domain_field, account_field, klass) != expected:
                failures.append("%s: %s is %s there and %s in the table (type, domain field, account field, kind)"
                                % (shown, name, (kind, domain_field, account_field, klass), expected))
    else:
        failures.append("%s is missing" % os.path.relpath(PANEL_LIST, ROOT))

    return failures, len(rows), readers


def self_test(sources):
    """Both negative controls, each in a temporary copy of the file it mutates."""
    problems = []
    temp = tempfile.mkdtemp(prefix="check-setting-levels-")
    try:
        # A new reader of a domain column, added to a copy of the tagger's file.
        rel = "Common/Util/ExternalSenderTagger.cpp"
        copy = os.path.join(temp, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(copy), exist_ok=True)
        shutil.copyfile(sources[rel], copy)
        with open(copy, "a", encoding="utf-8") as handle:
            handle.write("\nnamespace HM\n{\n   int\n   ExternalSenderTagger::NegativeControlHour_(std::shared_ptr<const Domain> domain)\n"
                         "   {\n      return domain->GetQuarantineDigestHour();\n   }\n}\n")
        mutated = dict(sources)
        mutated[rel] = copy
        failures, _, _ = check(mutated, {})
        if not any("NegativeControlHour_ reads GetQuarantineDigestHour straight" in f for f in failures):
            problems.append("self-test: a new function reading a domain's digest hour straight was not refused (%r)" % failures[:3])

        # The plus-addressing site with its resolver call swapped for the column.
        rel = "SMTP/PlusAddressing.cpp"
        copy = os.path.join(temp, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(copy), exist_ok=True)
        text = read(sources[rel])
        swapped = re.sub(r'EffectiveSetting::ResolveBool\(_T\("plus_addressing_enabled"\), pDomain, account\)',
                         "pDomain->GetUsePlusAddressing()", text)
        if swapped == text:
            problems.append("self-test: the plus-addressing site's resolver call was not found to swap; the control proves nothing")
        with open(copy, "w", encoding="utf-8") as handle:
            handle.write(swapped)
        mutated = dict(sources)
        mutated[rel] = copy
        failures, _, _ = check(mutated, {})
        if not any("ExtractAccountAddress reads GetUsePlusAddressing straight" in f for f in failures):
            problems.append("self-test: the plus-addressing site reading the column straight was not refused")
        if not any("ExtractAccountAddress no longer resolves plus_addressing_enabled" in f for f in failures):
            problems.append("self-test: the plus-addressing site without its resolver call was not refused")

        # A reader in the second branch of an #if, which only the POSIX build compiles.
        rel = "Common/Util/ExternalSenderTagger.cpp"
        copy = os.path.join(temp, "posix", rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(copy), exist_ok=True)
        shutil.copyfile(sources[rel], copy)
        with open(copy, "a", encoding="utf-8") as handle:
            handle.write("\nnamespace HM\n{\n   int\n   ExternalSenderTagger::NegativeControlPosix_(std::shared_ptr<const Domain> domain)\n"
                         "   {\n#ifdef _WIN32\n      return 0;\n#else\n      return domain->GetQuarantineDigestHour();\n#endif\n   }\n}\n")
        mutated = dict(sources)
        mutated[rel] = copy
        failures, _, _ = check(mutated, {})
        if not any("NegativeControlPosix_ reads GetQuarantineDigestHour straight" in f for f in failures):
            problems.append("self-test: a reader in an #else branch was not refused (%r)" % failures[:3])

        # A gate that says it tightens: the kind and the way it tightens disagree.
        text = read(sources[TABLE_FILE])
        swapped = text.replace('"zero_access_allowed", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "ZeroAccessAllowed", E::ClassGate, E::TightNone',
                               '"zero_access_allowed", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "ZeroAccessAllowed", E::ClassGate, E::TightOn')
        if swapped == text:
            problems.append("self-test: zero_access_allowed's row was not found to mutate; the control proves nothing")
        failures, _, _ = check(sources, {TABLE_FILE: swapped})
        if not any("zero_access_allowed is a gate and says it tightens" in f for f in failures):
            problems.append("self-test: a gate that tightens was not refused")
    finally:
        shutil.rmtree(temp, ignore_errors=True)
    return problems


def main():
    sources = load_sources(SERVER)
    problems = self_test(sources)
    failures, rows, readers = check(sources, {})
    failures = problems + failures
    for failure in failures:
        print("  " + failure)
    if failures:
        print("FAIL: %d problem(s) with the settings that may be set below the server" % len(failures))
        return 1

    kinds = {}
    for row in table_rows(read(sources[TABLE_FILE])):
        kinds[row["class"]] = kinds.get(row["class"], 0) + 1
    counted = {"server": 0, "domain": 0, "account": 0}
    for line in read(LEVELS_FILE).splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] in counted and not line.startswith("#"):
            counted[parts[1]] += 1
    ini = {"server": 0, "domain": 0, "account": 0}
    for line in read(BASELINE).splitlines():
        parts = line.split()
        if len(parts) >= 6 and not line.startswith("#") and parts[-1] in ini:
            ini[parts[-1]] += 1
    print("OK    %d setting(s) with levels, each resolved by EffectiveSetting at %d pinned read site(s); %d direct read(s) of a "
          "level, each where one level is meant; the marks agree - properties %d server, %d domain, %d account; ini keys %d "
          "server, %d domain, %d account; kinds %d limit, %d gate, %d preference; the Control Panel's list is the table's; and "
          "the negative controls (a new direct reader, a site with its resolver call taken out, a reader in an #else branch, a "
          "gate that tightens) are refused"
          % (rows, len(PINNED), readers, counted["server"], counted["domain"], counted["account"],
             ini["server"], ini["domain"], ini["account"], kinds.get("Limit", 0), kinds.get("Gate", 0), kinds.get("Preference", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
