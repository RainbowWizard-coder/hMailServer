#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Embed the setting profiles in the server, and hold each one to the settings.

A profile (docs/Profiles.md) is a document in the tree, not a row in a
database: hmailserver/source/Server/Common/Application/Profiles/<name>.json,
a name, the place it is listed in (order), a title, a paragraph, the steps it
cannot take for an administrator,
and a list of entries - {store, name, value, why}, with a caution where the
entry closes a listener, trusts an address range or turns a filter off. One
profile may include another (the high-volume sender is the relay and more):
the included entries come first, in their order, an entry of the same setting
replacing the included one in its place.

This reads them and writes two things:

  hmailserver/source/Server/Common/Application/ProfilesData.cpp
      the profiles as the server's tables (ProfilesData.h), the includes
      resolved, so that the server applies exactly the entries listed here;
  hmailserver/docs/Profiles.md, between its two generated markers
      each profile as a table - setting, value, why - so that the document an
      administrator reads is the data the server applies, and cannot drift
      from it.

and refuses, with the file and a sentence, before writing anything:

  * a name that is not a setting: a [Settings] key that
    build/ini-settings-baseline.txt does not list in its [Settings] section, or
    an hm_settings property SettingsDefaults.h does not list with a column this
    build reads - and the records the server keeps for itself
    (EnvironmentManagedSettings, EnvironmentSecretSettings, ProfileSnapshot,
    ProfileApplied, SampleDomainId, the VAPID public key);
  * what a profile never sets: a port (a listener's, or a daemon's), a
    directory or a file (a path is the installation's), a database value (the
    [Database] section is not a setting, and the [Settings] keys about the
    database connection are refused as well), and a password, a secret, a token,
    a pepper, a passphrase or a private key;
  * a value its setting would refuse: a whole number for a key the server reads
    as one and for a property kept in the number column, text for the text
    column, no line break and at most 4000 characters; and, for a property a
    REST settings row keeps whose check is written as a range
    (v.number < A || v.number > B), a number outside it. The rest of a row's
    check is held by the server itself, which refuses to apply a profile any
    entry of which its row refuses, and by the regression test that asks the
    server about every entry of every profile shipped (RestApiProfiles.cs);
  * a caution that is missing or wrong: switching one of the three protocol
    listeners off must say closes_listener, switching one of the filters off
    turns_filter_off, and trusting an address range or a peer's word
    trusts_addresses - and an entry that does none of those carries no caution.
    The server asks for an explicit confirmation before it applies such an entry
    to a server where it would change something;
  * a profile whose name is not its file's, is not lower case, or is one of the
    two words its routes use (revert, discard); a title, a paragraph or a reason
    missing or not one line; the same setting twice; an include that names no
    profile or goes round in a circle.

Usage: generate-profiles.py [--check] [--profiles DIR] [--print]
  --check        exits 1 when the committed ProfilesData.cpp or the document's
                 generated section is not what the profiles would make, or when
                 a profile is refused
  --profiles DIR reads the profiles from DIR instead (the negative controls:
                 build/generate-profiles.py --check --profiles <a copy> must
                 refuse a profile that sets a port)
  --print        writes the unit to stdout
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
APPLICATION = os.path.join(SERVER, "Common", "Application")
PROFILES = os.path.join(APPLICATION, "Profiles")
OUT = os.path.join(APPLICATION, "ProfilesData.cpp")
DOC = os.path.join(ROOT, "hmailserver", "docs", "Profiles.md")
BASELINE = os.path.join(ROOT, "build", "ini-settings-baseline.txt")
DEFAULTS = os.path.join(APPLICATION, "SettingsDefaults.h")
CONSTANTS = os.path.join(APPLICATION, "Constants.h")
ROWS = os.path.join(SERVER, "Common", "Util", "RestApiSettings.cpp")

# Where a row's getter may be: the configuration objects the rows reach.
GETTER_SOURCES = [
    os.path.join(APPLICATION, "Configuration.cpp"),
    os.path.join(SERVER, "Common", "AntiSpam", "AntiSpamConfiguration.cpp"),
    os.path.join(SERVER, "Common", "AntiVirus", "AntiVirusConfiguration.cpp"),
    os.path.join(SERVER, "Common", "Cache", "CacheConfiguration.cpp"),
    os.path.join(SERVER, "SMTP", "SMTPConfiguration.cpp"),
    os.path.join(SERVER, "IMAP", "IMAPConfiguration.cpp"),
    os.path.join(SERVER, "POP3", "POP3Configuration.cpp"),
]

BEGIN = "<!-- The four profiles: generated by build/generate-profiles.py from Common/Application/Profiles/*.json. Edit the JSON, not this section. -->"
END = "<!-- End of the generated section. -->"

NAME = re.compile(r"^[a-z][a-z0-9-]{1,30}$")
RESERVED = {"revert", "discard"}
STORES = ("ini", "database")
CAUTIONS = ("closes_listener", "trusts_addresses", "turns_filter_off")

# The records the server keeps for itself, which are nobody's setting.
KEPT = {"environmentmanagedsettings", "environmentsecretsettings", "profilesnapshot", "profileapplied",
        "sampledomainid", "webpushvapidpublickey"}

# A listener switched off closes it.
LISTENERS = {("database", "protocolsmtp"), ("database", "protocolimap"), ("database", "protocolpop3")}

# A filter switched off lets through what it stopped.
FILTERS = {
    ("database", "usespf"), ("database", "usemxchecks"), ("database", "ascheckhostinhelo"), ("database", "ascheckptr"),
    ("database", "asdkimverificationenabled"), ("database", "asdmarcenabled"), ("database", "usegreylisting"),
    ("database", "spamassassinenabled"), ("database", "clamavenabled"), ("database", "avclamwinenable"),
    ("database", "usecustomvirusscanner"), ("database", "enableattachmentblocking"),
    ("database", "autobanonlogonfailureenabled"), ("database", "breachcheckenabled"),
    ("database", "disconnectinvalidclients"), ("database", "verifyremotesslcertificate"),
    ("database", "rejectmultircptnullsender"),
    ("ini", "dnssecvalidationenabled"), ("ini", "daneenforcementenabled"), ("ini", "mtastsenabled"),
    ("ini", "dkimenforcesignatureexpiry"), ("ini", "quarantineenabled"), ("ini", "smtpauthenticatedsendercheck"),
    ("ini", "oauth2requiretls"),
}

# A switch that, on, takes a peer's word for who is connecting or what a
# message is.
TRUST_SWITCHES = {("ini", "smtpxclientenabled"), ("ini", "smtpproxyprotocolenabled"),
                  ("database", "asarcfilteringenabled"), ("database", "readerfetchprivatehosts")}

# A list of addresses, ranges or sealers that, filled, is trusted.
TRUST_LISTS = {("ini", "smtpxclienttrustedips"), ("ini", "smtpproxyprotocoltrustedips"),
               ("ini", "webservicestrustedproxies"), ("ini", "autobanneverban"), ("ini", "authuserreplacementip"),
               ("database", "asarctrustedsealers"), ("database", "countryblockingexemptranges")}

SECRET_FRAGMENTS = ("password", "secret", "token", "pepper", "passphrase", "privatekey")
PATH_SUFFIXES = ("dir", "directory", "folder", "file", "path", "db", "exec", "executable")
# A path whose name ends in none of those: where a backup is written.
PATHS = {"backupdestination"}


def read(path):
    with io.open(path, "r", encoding="utf-8-sig") as handle:
        return handle.read().replace("\r\n", "\n")


def ini_keys():
    """The [Settings] keys: {lower: (spelling, integer)}; and every other section's keys by lower name."""
    keys = {}
    others = {}
    for line in read(BASELINE).splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        section, key, kind = parts[0], parts[1], parts[2]
        if section == "Settings":
            keys[key.lower()] = (key, kind == "integer")
        else:
            others[key.lower()] = section
    return keys, others


def properties():
    """The hm_settings properties: {lower: (spelling, kind)}, kind String, Number or Either."""
    pattern = re.compile(r'\{\s*_T\("([^"]+)"\)\s*,\s*_T\("(?:[^"\\]|\\.)*"\)\s*,\s*-?\d+\s*,\s*Kind(\w+)\s*,\s*Level\w+\s*\}')
    found = {}
    for name, kind in pattern.findall(read(DEFAULTS)):
        found[name.lower()] = (name, kind)
    return found


def row_ranges():
    """{property lower name: (row key, low, high)} for the REST settings rows whose
    getter reads one property through a configuration getter and whose check is
    written as a range. Best effort: a row this cannot read is left to the server."""
    constants = dict(re.findall(r'#define\s+(PROPERTY_[A-Z0-9_]+)\s+_T\("([^"]+)"\)', read(CONSTANTS)))

    getters = {}
    function = re.compile(r"^\s*\w[\w:<>\s\*&]*?\b(\w+)::(Get\w+)\s*\([^)]*\)\s*(?:const)?\s*\n\s*\{(.*?)\n\s*\}", re.S | re.M)
    for source in GETTER_SOURCES:
        if not os.path.exists(source):
            continue
        for _, getter, body in function.findall(read(source)):
            names = set(constants[c] for c in re.findall(r"\b(PROPERTY_[A-Z0-9_]+)\b", body) if c in constants)
            if len(names) == 1:
                getters.setdefault(getter, set()).update(names)

    ranges = {}
    text = read(ROWS)
    starts = [m for m in re.finditer(r'\n\s*\{\s*"([a-z0-9_]+)",\s*Kind(\w+),\s*(ReadWrite|ReadOnly|WriteOnly),', text)]
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
        table_end = text.find("};", match.end())
        if 0 <= table_end < end:
            end = table_end
        body = text[match.end():end]
        getter = re.search(r"ROW_(?:NUMBER|FLAG)\([^;]*?->\s*(Get\w+)\(|ROW_(?:NUMBER|FLAG)\(\w+\(\)\.(Get\w+)\(", body)
        bounds = re.search(r"v\.number\s*<\s*(-?\d+)\s*\|\|\s*v\.number\s*>\s*(-?\d+)", body)
        if not getter or not bounds:
            continue
        name = getter.group(1) or getter.group(2)
        owners = getters.get(name)
        if not owners or len(owners) != 1:
            continue
        prop = next(iter(owners)).lower()
        ranges[prop] = (match.group(1), int(bounds.group(1)), int(bounds.group(2)))
    return ranges


def refusal_of_name(store, name, ini, others, props):
    """Why a profile may not name this setting, or None; and its spelling."""
    lower = name.lower()
    if lower in KEPT:
        return "%s is a record the server keeps for itself, not a setting" % name, None

    if store == "ini":
        if lower in others:
            return "%s is a key of the [%s] section, which is the installation's bootstrap and not a setting" % (name, others[lower]), None
        if lower not in ini:
            return "%s is not one of the [Settings] keys build/ini-settings-baseline.txt lists" % name, None
        spelling = ini[lower][0]
        if spelling != name:
            return "%s is spelled %s in build/ini-settings-baseline.txt; use its spelling" % (name, spelling), None
    else:
        if lower not in props:
            return "%s is not one of the hm_settings properties SettingsDefaults.h lists" % name, None
        spelling, kind = props[lower]
        if spelling != name:
            return "%s is spelled %s in SettingsDefaults.h; use its spelling" % (name, spelling), None
        if kind == "Either":
            return "%s is a row this build does not read by one column, which a profile cannot set" % name, None

    if lower.endswith("port"):
        return "%s is a port, and a profile never sets a port" % name, None
    if lower in PATHS or any(lower.endswith(suffix) for suffix in PATH_SUFFIXES):
        return "%s is a directory, a file or a path, and a profile never sets one: a path is the installation's" % name, None
    if lower.startswith("database") or lower.startswith("db"):
        return "%s is about the database connection, and a profile never sets a database value" % name, None
    if not (lower.startswith("passwordpolicy") or lower.startswith("passwordhash")):
        for fragment in SECRET_FRAGMENTS:
            if fragment in lower:
                return "%s holds a secret, and a profile never sets a password, a secret or a key" % name, None
    return None, spelling


def text_of(value):
    """The entry's value as the text the server applies, or None for a value that is none."""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return value
    return None


def expected_caution(store, lower, text):
    key = (store, lower)
    if key in LISTENERS and text == "0":
        return "closes_listener"
    if key in FILTERS and text == "0":
        return "turns_filter_off"
    if key in TRUST_SWITCHES and text == "1":
        return "trusts_addresses"
    if key in TRUST_LISTS and text.strip() != "":
        return "trusts_addresses"
    return None


def load(directory):
    """Every profile in the directory, parsed: {name: document}, and the problems."""
    documents = {}
    problems = []
    if not os.path.isdir(directory):
        return documents, ["%s is not a directory" % directory]
    for file_name in sorted(os.listdir(directory)):
        if not file_name.endswith(".json"):
            continue
        path = os.path.join(directory, file_name)
        try:
            document = json.loads(read(path))
        except ValueError as bad:
            problems.append("%s: not JSON: %s" % (file_name, bad))
            continue
        if not isinstance(document, dict):
            problems.append("%s: a profile is one JSON object" % file_name)
            continue
        documents[file_name[:-5]] = document
    if not documents:
        problems.append("%s holds no profile" % directory)
    return documents, problems


def one_line(text, limit):
    return isinstance(text, str) and text.strip() != "" and "\n" not in text and "\r" not in text and len(text) <= limit


def ascii_only(text):
    return all(32 <= ord(c) < 127 for c in text)


def validate(documents):
    """The profiles with their includes resolved: [(name, document, entries)], and the problems."""
    ini, others = ini_keys()
    props = properties()
    ranges = row_ranges()
    problems = []

    for file_name, document in sorted(documents.items()):
        where = file_name + ".json"
        name = document.get("name")
        if name != file_name:
            problems.append("%s: its name is %r, and a profile's name is its file's" % (where, name))
        if not isinstance(name, str) or not NAME.match(name):
            problems.append("%s: a profile's name is lower-case letters, digits and hyphens, two to thirty-one of them" % where)
        elif name in RESERVED:
            problems.append("%s: %s is a word the profile routes use (POST /api/v1/settings/profiles/%s), and cannot be a profile's name" % (where, name, name))
        for field, limit in (("title", 80), ("summary", 1200)):
            value = document.get(field)
            if not one_line(value, limit) or not ascii_only(value):
                problems.append("%s: %s is one line of plain text, at most %d characters" % (where, field, limit))
        steps = document.get("steps", [])
        if not isinstance(steps, list) or not all(one_line(s, 600) and ascii_only(s) for s in steps):
            problems.append("%s: steps is a list of sentences, each one line of plain text" % where)
        order = document.get("order")
        if order is not None and (not isinstance(order, int) or isinstance(order, bool) or order < 1):
            problems.append("%s: order is a whole number from 1, the place the profile is listed in" % where)
        includes = document.get("includes")
        if includes is not None and (not isinstance(includes, str) or includes not in documents):
            problems.append("%s: includes %r, which is not a profile" % (where, includes))
        entries = document.get("entries")
        if not isinstance(entries, list) or not entries:
            problems.append("%s: entries is a list of at least one entry" % where)
            continue
        allowed = {"store", "name", "value", "why", "caution"}
        for position, entry in enumerate(entries):
            here = "%s entry %d" % (where, position + 1)
            if not isinstance(entry, dict):
                problems.append("%s: an entry is an object" % here)
                continue
            extra = set(entry) - allowed
            if extra:
                problems.append("%s: %s is not a field of an entry" % (here, ", ".join(sorted(extra))))
            store = entry.get("store")
            setting = entry.get("name")
            if store not in STORES:
                problems.append("%s: store is ini (a [Settings] key) or database (an hm_settings property), not %r" % (here, store))
                continue
            if not isinstance(setting, str) or not setting:
                problems.append("%s: name is the setting's name" % here)
                continue
            here = "%s (%s)" % (here, setting)
            refusal, spelling = refusal_of_name(store, setting, ini, others, props)
            if refusal:
                problems.append("%s: %s" % (here, refusal))
                continue
            text = text_of(entry.get("value"))
            if text is None:
                problems.append("%s: value is a number, true or false, or text" % here)
                continue
            if len(text) > 4000 or "\n" in text or "\r" in text or not ascii_only(text):
                problems.append("%s: value is at most 4000 characters of plain text on one line" % here)
                continue
            lower = setting.lower()
            number = None
            if isinstance(entry.get("value"), (bool, int)):
                number = int(entry.get("value"))
            if store == "ini":
                if ini[lower][1] and number is None:
                    problems.append("%s: %s is read as a whole number, and the value is not one" % (here, setting))
                    continue
            else:
                kind = props[lower][1]
                if kind == "Number" and number is None:
                    problems.append("%s: %s is kept in the number column, and the value is not a number" % (here, setting))
                    continue
                if kind == "String" and number is not None:
                    problems.append("%s: %s is kept in the text column, and the value is a number; write it as text" % (here, setting))
                    continue
                if lower in ranges and number is not None:
                    key, low, high = ranges[lower]
                    if number < low or number > high:
                        problems.append("%s: %d is outside %d to %d, which the REST settings row %s refuses" % (here, number, low, high, key))
                        continue
            if not one_line(entry.get("why"), 240) or not ascii_only(entry.get("why")) or entry.get("why").rstrip().endswith("."):
                problems.append("%s: why is one clause of plain text, at most 240 characters, with no full stop" % here)
            caution = entry.get("caution")
            if caution is not None and caution not in CAUTIONS:
                problems.append("%s: caution is one of %s, not %r" % (here, ", ".join(CAUTIONS), caution))
                continue
            wanted = expected_caution(store, lower, text)
            if caution != wanted:
                if wanted:
                    problems.append("%s: this entry %s, and says so with \"caution\": \"%s\" - the server asks for a confirmation before it applies one" %
                                    (here, CAUTION_WORDS[wanted], wanted))
                else:
                    problems.append("%s: carries the caution %s, which this entry does not earn" % (here, caution))

        seen = set()
        for entry in entries:
            if isinstance(entry, dict):
                key = (entry.get("store"), str(entry.get("name", "")).lower())
                if key in seen:
                    problems.append("%s: %s is named twice" % (where, entry.get("name")))
                seen.add(key)

    resolved = []
    # In the order the profiles give themselves - the order a person meets them in
    # - and by name after that.
    def order_of(item):
        value = item[1].get("order")
        return (value if isinstance(value, int) else 1000, item[0])

    for file_name, document in sorted(documents.items(), key=order_of):
        chain = []
        current = file_name
        circle = False
        while current is not None:
            if current in chain:
                problems.append("%s.json: its includes go round in a circle (%s)" % (file_name, " -> ".join(chain + [current])))
                circle = True
                break
            chain.append(current)
            current = documents.get(current, {}).get("includes") if current in documents else None
        if circle:
            continue
        merged = []
        for member in reversed(chain):
            for entry in documents[member].get("entries") or []:
                if not isinstance(entry, dict):
                    continue
                key = (entry.get("store"), str(entry.get("name", "")).lower())
                replaced = False
                for index, existing in enumerate(merged):
                    if (existing.get("store"), str(existing.get("name", "")).lower()) == key:
                        merged[index] = dict(entry, _from=member)
                        replaced = True
                        break
                if not replaced:
                    merged.append(dict(entry, _from=member))
        resolved.append((file_name, document, merged))

    return resolved, problems


def cpp_string(text):
    out = []
    for c in text:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        else:
            out.append(c)
    return '_T("' + "".join(out) + '")'


def identifier(name):
    return "".join(part.capitalize() for part in re.split(r"[^a-z0-9]", name) if part)


CAUTION_CPP = {None: "CautionNone", "closes_listener": "CautionClosesListener",
               "trusts_addresses": "CautionTrustsAddresses", "turns_filter_off": "CautionTurnsFilterOff"}


def unit(resolved):
    out = []
    out.append("// https://www.progressiverobot.com")
    out.append("// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd")
    out.append("// SPDX-License-Identifier: AGPL-3.0-or-later")
    out.append("")
    out.append("// GENERATED by build/generate-profiles.py from Common/Application/Profiles/*.json,")
    out.append("// and held to them by that script's --check on every pull request. Do not edit:")
    out.append("// edit a profile's JSON and run the generator. What each profile is and why each")
    out.append("// entry is there is docs/Profiles.md, whose tables the same script writes.")
    out.append("")
    out.append('#include "StdAfx.h"')
    out.append('#include "ProfilesData.h"')
    out.append("")
    out.append("namespace HM")
    out.append("{")
    out.append("   namespace ProfilesData")
    out.append("   {")
    out.append("      namespace")
    out.append("      {")
    for name, document, entries in resolved:
        ident = identifier(name)
        out.append("         const Entry %sEntries[] =" % ident)
        out.append("         {")
        for entry in entries:
            out.append("            { %s, %s, %s, %s, %s }," % (
                "StoreIni" if entry["store"] == "ini" else "StoreDatabase",
                cpp_string(entry["name"]), cpp_string(text_of(entry["value"])), cpp_string(entry["why"]),
                CAUTION_CPP[entry.get("caution")]))
        out.append("         };")
        out.append("")
        steps = document.get("steps") or []
        out.append("         const TCHAR *const %sSteps[] =" % ident)
        out.append("         {")
        for step in steps:
            out.append("            %s," % cpp_string(step))
        out.append("            nullptr")
        out.append("         };")
        out.append("")
    out.append("      }")
    out.append("")
    out.append("      extern const Profile Profiles[] =")
    out.append("      {")
    for name, document, entries in resolved:
        ident = identifier(name)
        out.append("         { %s, %s, %s, %s, %sSteps, %d, %sEntries, %d }," % (
            cpp_string(name), cpp_string(document["title"]), cpp_string(document["summary"]),
            cpp_string(document.get("includes") or ""), ident, len(document.get("steps") or []), ident, len(entries)))
    out.append("      };")
    out.append("")
    out.append("      extern const size_t ProfileCount = %d;" % len(resolved))
    out.append("   }")
    out.append("}")
    out.append("")
    return "\n".join(out)


CAUTION_WORDS = {None: "", "closes_listener": "closes a listener", "trusts_addresses": "trusts an address range",
                 "turns_filter_off": "turns a filter off"}


def markdown_cell(text):
    return text.replace("|", "\\|")


def section(resolved):
    out = [BEGIN, ""]
    for name, document, entries in resolved:
        out.append("### %s - %s" % (name, document["title"]))
        out.append("")
        out.append(document["summary"])
        out.append("")
        if document.get("includes"):
            out.append("Everything `%s` sets, with the entries below replacing or adding to it - %d entries in all." % (document["includes"], len(entries)))
        else:
            out.append("%d entries." % len(entries))
        out.append("")
        out.append("| Store | Setting | Value | Why | Asks for a confirmation |")
        out.append("|---|---|---|---|---|")
        for entry in entries:
            from_include = entry.get("_from") != name
            out.append("| %s | `%s` | `%s` | %s%s | %s |" % (
                entry["store"], entry["name"], markdown_cell(text_of(entry["value"])), markdown_cell(entry["why"]),
                " (from `%s`)" % entry["_from"] if from_include else "",
                CAUTION_WORDS[entry.get("caution")]))
        out.append("")
        steps = document.get("steps") or []
        if steps:
            out.append("What it cannot do for you:")
            out.append("")
            for step in steps:
                out.append("* " + step)
            out.append("")
    out.append(END)
    return "\n".join(out)


def document_with(section_text):
    """Profiles.md with its generated section replaced, or None when it has no markers."""
    if not os.path.exists(DOC):
        return None, None
    current = read(DOC)
    start = current.find(BEGIN)
    end = current.find(END)
    if start < 0 or end < start:
        return current, None
    return current, current[:start] + section_text + current[end + len(END):]


def main():
    arguments = sys.argv[1:]
    directory = PROFILES
    if "--profiles" in arguments:
        at = arguments.index("--profiles")
        if at + 1 >= len(arguments):
            print("--profiles needs a directory", file=sys.stderr)
            return 2
        directory = arguments[at + 1]

    documents, problems = load(directory)
    resolved, more = validate(documents)
    problems += more
    if problems:
        for problem in problems:
            print("refused: " + problem, file=sys.stderr)
        print("%d problem(s): nothing was written." % len(problems), file=sys.stderr)
        return 1

    text = unit(resolved)
    doc_now, doc_wanted = document_with(section(resolved))

    if "--print" in arguments:
        sys.stdout.write(text)
        return 0

    counts = ", ".join("%s %d" % (name, len(entries)) for name, _, entries in resolved)

    if "--check" in arguments:
        # A copy of the profiles is checked for what it says, not against the
        # tree's generated files, which it is not the source of.
        if directory != PROFILES:
            print("OK    %d profile(s) in %s are settings a profile may set: %s" % (len(resolved), directory, counts))
            return 0
        failed = False
        current = read(OUT) if os.path.exists(OUT) else ""
        if current != text:
            print("ProfilesData.cpp is not what the profiles make; run python build/generate-profiles.py", file=sys.stderr)
            failed = True
        if doc_wanted is None:
            print("docs/Profiles.md has no generated section between its markers", file=sys.stderr)
            failed = True
        elif doc_now != doc_wanted:
            print("docs/Profiles.md's tables are not what the profiles make; run python build/generate-profiles.py", file=sys.stderr)
            failed = True
        if failed:
            return 1
        print("OK    %d profiles, every entry a setting a profile may set, and ProfilesData.cpp and docs/Profiles.md current: %s" % (len(resolved), counts))
        return 0

    if directory != PROFILES:
        print("--profiles is for --check; the tree's files are made from the tree's profiles.", file=sys.stderr)
        return 2

    with io.open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    if doc_wanted is not None and doc_now != doc_wanted:
        # Encoded, and the file's line endings read, before it is opened for
        # writing: opening it truncates it.
        with io.open(DOC, "rb") as handle:
            crlf = b"\r\n" in handle.read()
        data = doc_wanted.encode("utf-8")
        if crlf:
            data = data.replace(b"\n", b"\r\n")
        with io.open(DOC, "wb") as handle:
            handle.write(data)
    elif doc_wanted is None:
        print("docs/Profiles.md has no generated section between its markers; ProfilesData.cpp written.", file=sys.stderr)
    print("Wrote ProfilesData.cpp and the tables of docs/Profiles.md: %s" % counts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
