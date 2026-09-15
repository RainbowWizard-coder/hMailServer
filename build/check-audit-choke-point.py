#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every administrative change reaches the audit trail, and no write route skips it.

An audit trail with a hole in it is worse than none: it reads as a complete
record of who changed what, and the one change somebody went out of their way to
make is the one it does not have. The hole is never deliberate - it is a new COM
setter, or a new REST route, written by somebody who had no reason to know this
class existed. So it fails here, with the file and the line, rather than being
discovered by a regulator.

Seven rules, in the shape of build/check-authz-choke-point.py:

  recorded    AuditTrail::RecordStatement is called from ONE file - the
              statement chokepoint in the DAL - and ::RecordSettingChange from
              the two settings chokepoints, PropertySet for hm_settings and
              IniSettingStore for hm_inisettings. Another caller means one
              change recorded twice, or recorded in a place that does not know
              what the change was.

  installed   The actor - who is making the change - is installed at one place
              per interface: RestApiServer::ProcessRequest_ for REST and the
              Deck, COMAuthentication's two authorisation questions for COM and
              so for the Control Panel. AuditTrail::SetActor is named nowhere
              else, because an actor set anywhere else is a change attributed to
              whoever happened to be there before.

  com         Every COM method that writes either asks one of those
              authorisation questions - which installs the actor on the thread
              that is about to do the writing - or carries an AuditScope of its
              own. EXEMPT below is the list of write-shaped methods that change
              nothing persistent, each with the reason, and every name in it
              must be a COM method that exists: an exemption for a method
              nobody wrote reads as a decision somebody made about it.

  rest        Every route kind that mutates is answered through the dispatch in
              ProcessRequest_, which is where the scope is installed. A handler
              reached any other way is a change made with no actor on the
              thread, and so a change nobody made.

  store       IniSettingStore::WriteSetting and ::RemoveSetting, the two writes
              of hm_inisettings an interface reaches, each call RecordChange_.
              XMLLoad (a restore) must NOT: it runs on the maintenance work
              queue, where no actor is installed, so a call there records
              nothing while reading as though it did.

  table       hm_audit is written by AuditTrail and by nothing else. The row a
              caller writes for itself is the row that says whatever it likes.

  tables      The statement chokepoint records a table only when its name is in
              AuditTrail.cpp's AuditedTables, matched exactly - so a name there
              that is not the table the server writes records nothing and says
              nothing, which is how five of them (hm_domainaliases for
              hm_domain_aliases, and four more) left domain aliases, incoming
              relays, both white lists and blocked senders out of the record
              until 15 September 2026. Every name listed must be a table all
              three create scripts define, and every table they define must be
              either listed or named in NOT_AUDITED below with the reason. The
              second half is what makes a NEW configuration table a decision
              rather than an omission: hm_acl and hm_remotedomainpolicies were
              both simply never added. hm_settings and hm_inisettings must NOT
              be listed, because their chokepoints already record every change
              with the value it had, and a listing would record it twice.

Run from the repository root: python3 build/check-audit-choke-point.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
COM = os.path.join(SERVER, "COM")
DBSCRIPTS = os.path.join(ROOT, "hmailserver", "source", "DBScripts")

# symbol -> the files (relative to hmailserver/source/Server, forward slashes)
# that may name it.
ALLOWED = {
   "RecordStatement(": {
      "Common/Util/AuditTrail.cpp",
      "Common/Util/AuditTrail.h",
      "Common/SQL/DatabaseConnectionManager.cpp",   # the one statement chokepoint
   },
   "RecordSettingChange(": {
      "Common/Util/AuditTrail.cpp",
      "Common/Util/AuditTrail.h",
      "Common/Application/PropertySet.cpp",         # the hm_settings chokepoint
      "Common/Application/IniSettingStore.cpp",     # the hm_inisettings chokepoint
   },
   "AuditTrail::SetActor(": {
      "Common/Util/AuditTrail.cpp",                 # AuditScope itself
      "COM/COMAuthentication.cpp",                  # the COM identity chokepoint
   },
   "AuditTrail::ClearActor(": {
      "Common/Util/AuditTrail.cpp",
   },
}

# The call sites the rules above protect must exist, or they guard nothing.
REQUIRED = [
   ("Common/SQL/DatabaseConnectionManager.cpp",
    "AuditTrail::Instance()->RecordStatement(command, bResult);",
    "the statement chokepoint no longer records anything"),
   ("Common/Application/PropertySet.cpp",
    "AuditTrail::Instance()->RecordSettingChange(sPropertyName,",
    "the settings chokepoint no longer records anything"),
   ("Common/Application/IniSettingStore.cpp",
    "AuditTrail::Instance()->RecordSettingChange(name,",
    "the [Settings] store no longer records anything, so none of the keys that left hMailServer.INI is audited"),
   ("Common/Util/RestApiServer.cpp",
    "AuditScope auditScope(AuditActorFor_(caller));",
    "REST no longer says who is making a change"),
   ("COM/COMAuthentication.cpp",
    "AuditTrail::SetActor(GetAuditActor());",
    "COM no longer says who is making a change"),
   ("Common/Util/AuditTrail.h",
    "bool Verify(__int64 &firstBroken, __int64 &rowsChecked, String &reason);",
    "the chain can no longer be walked, which is the whole point of chaining it"),
]

# The two administrative writes of hm_inisettings each have to reach the
# recorder, or a route through the one that does not is a way round it.
INI_STORE_RECORDS = ("IniSettingStore::WriteSetting", "IniSettingStore::RemoveSetting")

# Writes of hm_inisettings that must NOT call the recorder, because no actor is
# ever on the thread they run on and a call there records nothing while reading
# as though it did. XMLLoad is a restore, run as a BackupTask on the maintenance
# work queue; docs/AlertsAndAuditTrail.md says a restore is not recorded, and a
# RecordChange_ put back here would make that page look wrong without making the
# trail any more complete. Recording a restore is a decision - one row for the
# restore, or one per value - that starts by carrying the actor to that thread.
INI_STORE_CANNOT_RECORD = ("IniSettingStore::XMLLoad",)

# A COM method, by its signature.
COM_METHOD = re.compile(r"^STDMETHODIMP\s+(\w+)::(\w+)\s*\(", re.M)

# What a method body looks like when it PERSISTS something - a row through the
# object or collection layer, or a value through Configuration and so through
# hm_settings. A property setter on a business object is not here on purpose: it
# sets a field in memory and changes nothing until Save, which is.
HARD_MARKERS = ("SaveObject(", "DeleteObject(", "DeleteItem", "DeleteAll", "DeleteByDBID",
                "ini_file_settings_->Set", "antiVirusConfiguration_.", "AntiSpamConfiguration().Set")

# What installs the actor on the calling thread, one way or the other.
INSTALLS = ("GetIsServerAdmin(", "GetIsDomainAdmin(", "AuditScope ")


# The settings interfaces reach the configuration through a cached pointer, and
# a getter names that pointer too - so what says "this writes" is a Set on it,
# directly or through one of the sub-configurations. Matched on the receiver so
# that GetBackupOption(BOSettings), which merely has the letters, is not a write.
CONFIG_SET = re.compile(r"(?:config_|cache_config_|ini_file_settings_)->(?:\w+\(\)->)?Set[A-Z]")

# The same through the IniFileSettings singleton, which is how the [Settings]
# store is reached from COM: the tarpit setters, SetIniSetting and
# DeleteIniSetting. DeleteIniSetting wrote through here with no actor of its own
# until this pattern existed, which the rule above could not see.
INI_WRITE = re.compile(r"IniFileSettings::Instance\(\)->(?:Set[A-Z]|WriteSettingsValue\(|RemoveSettingsValue\()")


def persists(body):
   if any(marker in body for marker in HARD_MARKERS):
      return True

   return CONFIG_SET.search(body) is not None or INI_WRITE.search(body) is not None


# A method that reaches one of those markers and still changes nothing in the
# configuration, with the reason. A method belongs here only when the thing it
# writes is not configuration; "nobody will notice" is not a reason.
EXEMPT = {
   "InterfaceAttachments::Add": "attaches a file to a message being composed; a message is traffic, not configuration.",
   "InterfaceAttachments::Clear": "as Add.",
   "InterfaceAttachment::Delete": "removes an attachment from a message being composed.",
   "InterfaceAttachment::SaveAs": "writes an attachment to a file the caller names; nothing in the server changes.",
   "InterfaceMessage::Save": "a message, which is traffic and not configuration.",
   "InterfaceMessages::Clear": "the messages of a folder.",
   "InterfaceMessages::DeleteByDBID": "one message of a folder.",
   "InterfaceAccount::DeleteMessages": "empties a mailbox; the account itself is unchanged.",
}

# The create scripts. A table has to be defined by all three to be listed: the
# statement chokepoint runs on every backend, and a name one of them spells
# differently is a hole on that backend only. SQL Server Compact runs the SQL
# Server script.
CREATE_SCRIPTS = ("CreateTablesMSSQL.sql", "CreateTablesMySQL.sql", "CreateTablesPGSQL.sql")

CREATE_TABLE = re.compile(r"\bcreate\s+table\s+(?:if\s+not\s+exists\s+)?[`\"\[]?(\w+)", re.I)

# Every table the create scripts define that is deliberately NOT one of the
# statement chokepoint's, with the reason. "Nobody changes it" is not a reason;
# "what changes it is traffic, or the account's own data, or another recorder"
# is.
NOT_AUDITED = {
   "hm_settings": "recorded by PropertySet with the value it had; listing it would record every change twice.",
   "hm_inisettings": "recorded by IniSettingStore with the value it had; listing it would record every change twice.",
   "hm_audit": "the trail itself, written by AuditTrail alone (rule 'table').",
   "hm_dbversion": "the schema version, written by the upgrade scripts and nobody else.",
   "hm_messages": "messages are traffic.",
   "hm_messagerecipients": "the recipients of a message in the queue; traffic.",
   "hm_message_metadata": "per-message metadata the IMAP server keeps; traffic.",
   "hm_messageindexterms": "the full-text index; traffic.",
   "hm_messageindexstate": "the full-text index's progress; traffic.",
   "hm_messagetrace": "where each message went; traffic.",
   "hm_quarantine": "held messages, written by delivery and by each account's own portal. An administrator releasing or deleting one is NOT "
                    "recorded, and docs/AlertsAndAuditTrail.md says so: both end in the same 'delete from hm_quarantine where "
                    "quarantineid = @ID', so a listed table would record a release and a delete as the same row, naming an id that "
                    "identifies nothing once the row is gone. Recording it needs a record of its own in QuarantineStore.",
   "hm_imapfolders": "every account's folders, which its mail client and its webmail create and delete as much as any administrator does - the webmail "
                     "under the account's own actor, so listing the table would record each folder a person makes in their own mailbox. Public "
                     "folders share the table, so an administrator creating, renaming or deleting one is NOT recorded, and "
                     "docs/AlertsAndAuditTrail.md says so. The permissions on folders (hm_acl) are audited.",
   "hm_imapexpunged": "IMAP's record of expunged messages for QRESYNC; traffic.",
   "hm_imap_metadata": "IMAP METADATA an account or its client sets on its own mailbox.",
   "hm_fetchaccounts_uids": "which messages an external fetch has already downloaded; traffic.",
   "hm_greylisting_triplets": "greylisting's record of the senders it has seen; traffic.",
   "hm_logon_failures": "failed logons, which are traffic and the auto-ban's business.",
   "hm_metricsamples": "metric history; traffic.",
   "hm_alertevents": "what an alert condition raised; the alert rules that decide it (hm_alertrules) are audited.",
   "hm_knownsenders": "who an account has corresponded with, learned from its mail.",
   "hm_passwordhistory": "the hashes of an account's earlier passwords, written as a consequence of the password change the hm_accounts row already records.",
   "hm_accountprefs": "an account's own webmail preferences, set by the account in the portal.",
   "hm_calendars": "an account's own calendars, written by it over CalDAV and the portal.",
   "hm_calendarobjects": "the events in an account's own calendars.",
   "hm_contacts": "an account's own address book, written by it over CardDAV and the portal.",
   "hm_files": "files an account attached as links from webmail; the account's own data.",
   "hm_scheduled": "an account's own scheduled sends and snoozed messages.",
   "hm_smimekeys": "an account's own S/MIME certificates, kept for it by the portal.",
}

# Tables whose chokepoint is not the statement one, and which must therefore
# never be listed.
RECORDED_ELSEWHERE = ("hm_settings", "hm_inisettings", "hm_audit")


def read(path):
   with open(path, "r", encoding="utf-8", errors="replace") as handle:
      return handle.read()


def relative(path):
   return os.path.relpath(path, SERVER).replace(os.sep, "/")


def sources():
   for base, _, names in os.walk(SERVER):
      for name in names:
         if name.endswith((".cpp", ".h")):
            yield os.path.join(base, name)


def check_allowed(problems):
   for path in sources():
      rel = relative(path)
      lines = read(path).split("\n")

      for number, line in enumerate(lines, 1):
         for symbol, allowed in ALLOWED.items():
            if symbol in line and rel not in allowed:
               problems.append("%s:%d: %s outside the audit chokepoints - see AuditTrail.h"
                               % (rel, number, symbol[:-1]))


def check_required(problems):
   for rel, needle, why in REQUIRED:
      path = os.path.join(SERVER, rel.replace("/", os.sep))
      text = read(path) if os.path.exists(path) else ""

      if needle not in text:
         problems.append("%s: '%s' is gone - %s" % (rel, needle, why))


def method_body(text, start):
   """The text of the method whose signature starts at start, by brace depth."""
   open_brace = text.find("{", start)
   if open_brace < 0:
      return ""

   depth = 0
   index = open_brace
   while index < len(text):
      if text[index] == "{":
         depth += 1
      elif text[index] == "}":
         depth -= 1
         if depth == 0:
            return text[open_brace:index + 1]
      index += 1

   return text[open_brace:]


def check_ini_store(problems):
   rel = "Common/Application/IniSettingStore.cpp"
   path = os.path.join(SERVER, rel.replace("/", os.sep))
   if not os.path.exists(path):
      problems.append("%s: is gone - the [Settings] store no longer records anything" % rel)
      return

   text = read(path)

   for function in INI_STORE_RECORDS:
      match = re.search(r"^\s*" + re.escape(function) + r"\s*\(", text, re.M)
      if match is None:
         problems.append("%s: %s is gone - an administrative write of hm_inisettings that is not recorded" % (rel, function))
         continue

      if "RecordChange_(" not in method_body(text, match.end()):
         line = text.count("\n", 0, match.start()) + 1
         problems.append("%s:%d: %s writes hm_inisettings without RecordChange_, so the change it makes leaves no audit row"
                         % (rel, line, function))

   for function in INI_STORE_CANNOT_RECORD:
      match = re.search(r"^\s*" + re.escape(function) + r"\s*\(", text, re.M)
      if match is None:
         continue

      if "RecordChange_(" in method_body(text, match.end()):
         line = text.count("\n", 0, match.start()) + 1
         problems.append("%s:%d: %s calls RecordChange_, but it runs where no audit actor is installed, so the call records nothing - "
                         "carry the actor to that thread first, and change docs/AlertsAndAuditTrail.md and INI_STORE_CANNOT_RECORD with it"
                         % (rel, line, function))


def check_com(problems):
   seen = set()

   for name in sorted(os.listdir(COM)):
      if not name.endswith(".cpp"):
         continue

      path = os.path.join(COM, name)
      text = read(path)

      for match in COM_METHOD.finditer(text):
         method = "%s::%s" % (match.group(1), match.group(2))
         seen.add(method)
         if method in EXEMPT:
            continue

         body = method_body(text, match.end())
         if not persists(body):
            continue

         if any(marker in body for marker in INSTALLS):
            continue

         line = text.count("\n", 0, match.start()) + 1
         problems.append("COM/%s:%d: %s persists something without installing the audit actor - ask "
                         "GetIsServerAdmin/GetIsDomainAdmin, or open an AuditScope(GetAuditActor()), "
                         "or name it in EXEMPT with the reason" % (name, line, method))

   # Until 15 September 2026 seven of these named InterfaceDatabase setters and
   # an InterfaceMessages::Delete that no COM file defines, so the list recorded
   # decisions about methods that did not exist - and the methods that really
   # write [Database], CreateInternalDatabase, CreateExternalDatabase and
   # SetDefaultDatabase, ask GetIsServerAdmin and need no exemption.
   for method in sorted(EXEMPT):
      if method not in seen:
         problems.append("build/check-audit-choke-point.py: EXEMPT names %s, which no file under COM defines - "
                         "remove it, or correct the name" % method)


def check_rest(problems):
   text = read(os.path.join(SERVER, "Common", "Util", "RestApiServer.cpp"))

   # The scope has to be installed BEFORE the dispatch, or the handlers run
   # without it.
   install = text.find("AuditScope auditScope(AuditActorFor_(caller));")
   dispatch = text.find("switch (route.kind)")

   if install < 0 or dispatch < 0 or install > dispatch:
      problems.append("Common/Util/RestApiServer.cpp: the audit scope is not installed before the route dispatch")

   # And exactly once: a second one would mean a second dispatch somewhere that
   # the first does not cover.
   if text.count("AuditScope auditScope(") != 1:
      problems.append("Common/Util/RestApiServer.cpp: the audit scope is installed %d times; there is one dispatch, so there is one install"
                      % text.count("AuditScope auditScope("))


def check_table(problems):
   for path in sources():
      rel = relative(path)
      if rel == "Common/Util/AuditTrail.cpp":
         continue

      lower = read(path).lower()
      for marker in ("insert into hm_audit", "update hm_audit", "delete from hm_audit", '"hm_audit"'):
         if marker in lower:
            problems.append("%s: writes hm_audit directly; the chain is AuditTrail's to keep" % rel)
            break


def audited_tables(problems):
   """AuditedTables in AuditTrail.cpp, as (table, line) pairs."""
   rel = "Common/Util/AuditTrail.cpp"
   text = read(os.path.join(SERVER, "Common", "Util", "AuditTrail.cpp"))

   start = text.find("AuditedTables[] =")
   if start < 0:
      problems.append("%s: AuditedTables is gone - the statement chokepoint records nothing" % rel)
      return []

   end = text.find("};", start)
   block = text[start:end]
   first_line = text.count("\n", 0, start) + 1

   entries = []
   for match in re.finditer(r'\{\s*L"([^"]+)"\s*,\s*L"([^"]+)"\s*\}', block):
      entries.append((match.group(1), first_line + block.count("\n", 0, match.start())))

   if not entries:
      problems.append("%s: AuditedTables names no table - the statement chokepoint records nothing" % rel)

   return entries


def created_tables():
   """script name -> the set of tables it creates, lower-cased."""
   created = {}

   for script in CREATE_SCRIPTS:
      path = os.path.join(DBSCRIPTS, script)
      text = read(path) if os.path.exists(path) else ""
      created[script] = set(name.lower() for name in CREATE_TABLE.findall(text))

   return created


def check_tables(problems):
   rel = "Common/Util/AuditTrail.cpp"
   entries = audited_tables(problems)
   created = created_tables()

   for script, names in created.items():
      if not names:
         problems.append("hmailserver/source/DBScripts/%s: defines no table, so the audited table names cannot be checked" % script)
         return

   everywhere = set.intersection(*created.values())
   anywhere = set.union(*created.values())

   listed = set()
   for table, line in entries:
      if table != table.lower():
         problems.append("%s:%d: %s - AuditedTable lower-cases the statement's table name before matching, so a name with a capital in it matches nothing"
                         % (rel, line, table))

      name = table.lower()

      if name in listed:
         problems.append("%s:%d: %s is listed twice" % (rel, line, table))
      listed.add(name)

      if name in RECORDED_ELSEWHERE:
         problems.append("%s:%d: %s is recorded by its own chokepoint with the value it had; listing it records every change to it twice"
                         % (rel, line, table))
         continue

      if name not in anywhere:
         problems.append("%s:%d: %s is not a table any create script defines, so the statement chokepoint matches nothing and the changes it was meant to record are never recorded"
                         % (rel, line, table))
      elif name not in everywhere:
         missing = ", ".join(script for script, names in sorted(created.items()) if name not in names)
         problems.append("%s:%d: %s is not created by %s, so on that backend it is recorded under no name at all"
                         % (rel, line, table, missing))

      if name in NOT_AUDITED:
         problems.append("%s:%d: %s is listed AND named in NOT_AUDITED in build/check-audit-choke-point.py; it is one or the other"
                         % (rel, line, table))

   for table in sorted(anywhere):
      if table in listed or table in NOT_AUDITED:
         continue

      problems.append("hmailserver/source/DBScripts: %s is created but is neither in AuditTrail.cpp's AuditedTables nor in NOT_AUDITED in build/check-audit-choke-point.py - "
                      "a configuration table goes in the first, anything else in the second with the reason" % table)

   for table in sorted(NOT_AUDITED):
      if table not in anywhere:
         problems.append("build/check-audit-choke-point.py: NOT_AUDITED names %s, which no create script defines any more - remove it" % table)


def main():
   problems = []

   check_allowed(problems)
   check_required(problems)
   check_ini_store(problems)
   check_com(problems)
   check_rest(problems)
   check_table(problems)
   check_tables(problems)

   if problems:
      print("Audit chokepoint: %d problem(s)" % len(problems))
      for problem in problems:
         print("  " + problem)
      return 1

   print("Audit chokepoint: every administrative change is recorded once, by whoever made it")
   return 0


if __name__ == "__main__":
   sys.exit(main())
