#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every setting the server keeps in its database can be set from the Control Panel.

build/check-ini-coverage.py holds every [Settings] key of hMailServer.INI to an
editor. Since 15 September 2026 a new setting goes in the DATABASE (CONTRIBUTING.md,
"A new setting goes in the database"), and nothing held those to an editor: on
27 September 2026 a census found 146 of the 314 settings SettingsDefaults.h names
with no editor in the Control Panel at all, and 137 of them (all but the nine in
EXEMPT) needing one - the webmail's sign-in, the alerts, the SNMP agent, the spam
traps, the complaint feedback loops, the object store - some
of them with a COM property the Control Panel had simply never used, and most of
them reachable over the REST API alone. An administrator who does not use the
Control Deck could not find them, and concluded the features did not exist. This
is the check that keeps that from happening again.

The settings are read from Common/Application/SettingsDefaults.h: its rows and its
PropertyNames, which between them are every hm_settings name this build knows.
A setting counts as having an editor in one of three ways, each read off the
sources rather than a list kept by hand:

  com    The server writes it through a COM property or method - a put_ whose
         body calls the setter that writes the setting, or names its PROPERTY_
         constant, or a method such as SetSMTPRelayerPassword doing the same - and
         the Control Panel writes that property or calls that method, as
         build/check-deck-parity.py reads the Control Panel's writes: a settings
         editor's Path, a collection editor's field, a dynamic local's assignment.
  rest   A row of the REST API's settings groups (RestApiSettings.cpp) writes it,
         and a REST row of a Control Panel settings page names that group and that
         key (Views/ServerSettingsView.xaml.cs: new RestBool { Key = "...",
         Group = RestSettings.... }, the group ServerGroup when it names none).
  named  The Control Panel names the setting itself, as a string literal of its
         own ("AlertsEnabled") - the Path of such a REST row, or the read-only row
         of a setting the server stores and never reads.

A setter is a function whose body passes the setting's PROPERTY_ constant (or its
name) to a Set...( call - Configuration::SetAlertRecipient, and also the overloaded
AntiVirusConfiguration::ClamWinEnabled(bool) - found wherever it is in the server's
source, so a setting moved to another class is still found. Names too general to
tie to one setting (SetLong, SetEnabled, Save...) are not used to match.

EXEMPT below is the list of settings that deliberately have no editor, each with
the reason. A setting belongs there only when an editor would be wrong - not when
one has not been written yet. An exemption for a setting the server no longer has
fails, and so does one for a setting that has an editor after all: a list of
exemptions that is not exactly the settings without an editor says nothing.

The check proves its own negative control on every run (--self-test runs only
that): a setting written through neither interface and named nowhere is refused,
one written through a COM property the Control Panel writes is not, one written
through a REST row the Control Panel names is not, and a stale exemption and a
needless one are both refused.

Usage: python build/check-db-settings-coverage.py [--self-test] [--list]
  --list  prints every setting with the way it is covered, or the reason it is exempt.
Exits 1 listing what is missing.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
DEFAULTS = os.path.join(SERVER, "Common", "Application", "SettingsDefaults.h")
COM = os.path.join(SERVER, "COM")
UTIL = os.path.join(SERVER, "Common", "Util")
CONTROL_PANEL = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel")
REST_SETTINGS_CS = os.path.join(CONTROL_PANEL, "Services", "RestSettings.cs")

# A database setting with no Control Panel editor, and why. Keyed by the name as
# SettingsDefaults.h spells it.
EXEMPT = {
   "antispamenabled":
      "A row the create scripts still seed for a switch that has gone; nothing in the server "
      "reads or writes it (SettingsDefaults.h marks it KindEither for that reason). An editor "
      "would change nothing.",
   "relaymode":
      "A row the create scripts still seed for a feature that has gone; nothing reads it.",
   "sendstatistics":
      "A row the create scripts still seed for a feature that has gone; nothing reads it.",
   "smtpnooftries":
      "A row the create scripts still seed under a name nothing reads: the delivery attempts "
      "are smtpnoofretries (Settings.SMTPNoOfTries), which has its editor on the Delivery page.",
   "groupcachettl":
      "A name the server defines and never reads or writes; the group cache has no TTL of its own.",
   "avdeleteinfectedemail":
      "A name the server defines and never reads or writes; what happens to an infected "
      "message is avaction (AntiVirus.Action), which has its editor on the Anti-virus page.",
   "SampleDomainId":
      "The id of the sample domain, written only by POST and DELETE /api/v1/setup/sample-domain "
      "(docs/SampleDomain.md). That is what makes removing the sample exact; an editor would "
      "let the marker point at a real domain, which the removal would then delete.",
   "workerthreadpriority":
      "Stored and read by nothing that acts on it: no thread priority is set from it "
      "(Services/SettingClaims.cs). The Performance page shows it read-only, so a value set "
      "years ago is visible with the reason it does nothing; SettingClaimsTests holds that it "
      "is never offered as an editor.",
   "WebPushLoopbackHttpForTests":
      "For the regression suite only: on, a device's endpoint may be plain http to this "
      "machine's loopback and a push to it skips the public-address check. An editor would "
      "offer an administrator a way to let a reverse proxy on the machine reach its own ports.",
}

# Setter names too general to tie a COM member or a REST row to one setting.
GENERIC = {"SetLong", "SetBool", "SetString", "SetValue", "Set", "SetEnabled", "Enabled",
           "SetActive", "Active", "SetName", "Save", "SetInt", "SetNumber"}

CONSTANT = re.compile(r'#define\s+(PROPERTY_\w+)\s+_T\("([^"]+)"\)')
SETTER_CALL = re.compile(r'\b(?:Set|Put)\w*\s*\(\s*(?:(PROPERTY_\w+)|_T\("(\w+)"\))')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(\s*(?=[^)\s])')
KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "else", "do", "try"}


def read(path):
   with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
      return handle.read()


def strip_comments(text):
   text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
   return re.sub(r"//[^\n]*", "", text)


def blank_strings(text):
   return re.sub(r'"(?:[^"\\\n]|\\.)*"', lambda m: '"' + " " * (len(m.group(0)) - 2) + '"', text)


def balanced(text, start):
   """The text between the brace at text[start] and the one that closes it."""
   depth = 0
   for index in range(start, len(text)):
      if text[index] == "{":
         depth += 1
      elif text[index] == "}":
         depth -= 1
         if depth == 0:
            return text[start + 1:index]
   return text[start + 1:]


# ---------------------------------------------------------------------------
# The server
# ---------------------------------------------------------------------------

def database_settings():
   """{lower name: name as spelled} for every row and every PropertyNames entry."""
   text = read(DEFAULTS)
   head, tail = text.split("PropertyNames[] =", 1)
   names = {}
   for name in re.findall(r'\{\s*_T\("([^"]+)"\)\s*,\s*_T\(', head):
      names[name.lower()] = name
   for name in re.findall(r'_T\("([^"]+)"\)', tail.split("};", 1)[0]):
      names.setdefault(name.lower(), name)
   return names


def server_sources():
   for base, _, files in os.walk(SERVER):
      if os.sep + "x64" in base:
         continue
      for name in files:
         if name.endswith((".cpp", ".h")) and name not in ("SettingsDefaults.h", "Constants.h"):
            yield os.path.join(base, name)


def constants():
   """{PROPERTY_X: lower setting name}."""
   found = {}
   for base, _, files in os.walk(SERVER):
      if os.sep + "x64" in base:
         continue
      for name in files:
         if name.endswith(".h"):
            for constant, setting in CONSTANT.findall(read(os.path.join(base, name))):
               found[constant] = setting.lower()
   return found


def function_spans(code):
   """[(name, start, end)] for every function body in the code, by a scan of its
   braces: a brace whose header ends name(...) - with const, override or
   noexcept after it - opens a function called name. A lambda, a class, an
   initialiser list is not one, and the function around it is the one reported."""
   spans = []
   stack = []
   boundary = 0
   for index, char in enumerate(code):
      if char == "{":
         header = code[boundary:index]
         match = re.search(r'([A-Za-z_~][\w:~]*)\s*\((?:[^()]|\([^()]*\))*\)\s*(?:const\s*)?(?:override\s*)?(?:noexcept\s*)?$', header)
         name = None
         if match:
            candidate = match.group(1).split("::")[-1]
            before = header[:match.start()].rstrip()
            if candidate not in KEYWORDS and not before.endswith(("=", "]", ",", "(", "return")):
               name = match.group(1)
         stack.append((name, index))
         boundary = index + 1
      elif char == "}":
         if stack:
            name, start = stack.pop()
            if name:
               spans.append((name, start, index))
         boundary = index + 1
      elif char == ";":
         boundary = index + 1
   return spans


def enclosing(spans, position):
   best = None
   for name, start, end in spans:
      if start <= position <= end and (best is None or start > best[1]):
         best = (name, start)
   return best[0] if best else None


def setters(constant_map, names):
   """{lower setting: set of setter function names}: every function whose body
   passes the setting's constant, or its name, to a Set...( call."""
   found = {}
   for path in server_sources():
      raw = read(path)
      if "PROPERTY_" not in raw and '_T("' not in raw:
         continue
      code = strip_comments(raw)
      hits = []
      for match in SETTER_CALL.finditer(code):
         if match.group(1):
            setting = constant_map.get(match.group(1))
         else:
            setting = match.group(2).lower() if match.group(2).lower() in names else None
         if setting:
            hits.append((setting, match.start()))
      if not hits:
         continue
      spans = function_spans(blank_strings(code))
      for setting, position in hits:
         function = enclosing(spans, position)
         if not function:
            continue
         short = function.split("::")[-1]
         if short.startswith("Ensure") or short.startswith(("put_", "get_")):
            continue
         found.setdefault(setting, set()).add(short)
   return found


def com_members():
   """[(interface, member, body)] for every COM member defined in Server/COM, the
   property name without its put_ and a method by its own name."""
   members = []
   head = re.compile(r'STDMETHODIMP\s+Interface(\w+)::(\w+)\s*\(')
   for name in sorted(os.listdir(COM)):
      if not (name.startswith("Interface") and name.endswith(".cpp")):
         continue
      code = strip_comments(read(os.path.join(COM, name)))
      for match in head.finditer(code):
         member = match.group(2)
         if member.startswith("get_"):
            continue
         brace = code.find("{", match.end())
         if brace < 0:
            continue
         body = balanced(code, brace)
         kind = "put" if member.startswith("put_") else "method"
         members.append((match.group(1), member[4:] if kind == "put" else member, kind, body))
   return members


def com_writers(settings, constant_map, setter_map, members):
   """{lower setting: set of (interface, member, kind)} the server writes it through."""
   by_setting_constants = {}
   for constant, setting in constant_map.items():
      by_setting_constants.setdefault(setting, set()).add(constant)

   found = {}
   for iface, member, kind, body in members:
      calls = set(CALL.findall(body))
      referenced = set(re.findall(r'\bPROPERTY_\w+\b', body))
      for setting in settings:
         mine = setter_map.get(setting, set()) - GENERIC
         if (referenced & by_setting_constants.get(setting, set())) or (calls & mine):
            found.setdefault(setting, set()).add((iface, member, kind))
   return found


def rest_writers(settings, constant_map, setter_map):
   """{lower setting: set of (group path, key)} the REST settings rows write it through."""
   by_setting_constants = {}
   for constant, setting in constant_map.items():
      by_setting_constants.setdefault(setting, set()).add(constant)

   found = {}
   for name in sorted(os.listdir(UTIL)):
      if not (name.startswith("RestApi") and name.endswith(".cpp")):
         continue
      code = strip_comments(read(os.path.join(UTIL, name)))
      groups = {table: path for path, table in re.findall(r'const Group \w+ = \{ "[\w-]+", "([^"]+)", (\w+),', code)}
      for table in re.finditer(r'const Row (\w+)\[\]\s*=\s*\{', code):
         path = groups.get(table.group(1))
         if not path:
            continue
         rows = balanced(code, table.end() - 1)
         starts = [(m.start(), m.group(1)) for m in re.finditer(r'(?m)^\s*\{\s*"([a-z0-9_]+)",\s*Kind', rows)]
         for index, (start, key) in enumerate(starts):
            end = starts[index + 1][0] if index + 1 < len(starts) else len(rows)
            row = rows[start:end]
            access = re.search(r'Kind\w+\s*,\s*(\w+)', row)
            if access and access.group(1) == "ReadOnly":
               continue
            # Only what the row's setter does: its getter names the constant too.
            setter = row.split("ROW_SET", 1)[1] if "ROW_SET" in row else row.split("[] (const Value &v)", 1)[-1]
            calls = set(CALL.findall(setter))
            referenced = set(re.findall(r'\bPROPERTY_\w+\b', setter))
            for setting in settings:
               mine = setter_map.get(setting, set()) - GENERIC
               if (referenced & by_setting_constants.get(setting, set())) or (calls & mine):
                  found.setdefault(setting, set()).add((path, key))
   return found


# ---------------------------------------------------------------------------
# The Control Panel
# ---------------------------------------------------------------------------

def control_panel_files():
   for base, _, files in os.walk(CONTROL_PANEL):
      if os.sep + "obj" in base or os.sep + "bin" in base:
         continue
      for name in files:
         if name.endswith((".cs", ".xaml")) and not name.endswith(".g.cs"):
            yield os.path.join(base, name)


def rest_group_constants():
   """{RestSettings.X: route}, from the Control Panel's own RestSettings.cs."""
   if not os.path.exists(REST_SETTINGS_CS):
      return {}
   return {name: path for name, path in re.findall(r'public const string (\w+Group) = "([^"]+)"', read(REST_SETTINGS_CS))}


def control_panel_rest_rows(texts):
   """{(group path, key)} of every REST row on a Control Panel settings page."""
   groups = rest_group_constants()
   default = groups.get("ServerGroup", "/api/v1/settings")
   rows = set()
   for text in texts:
      for match in re.finditer(r'new\s+Rest\w+\s*\{', text):
         body = balanced(text, match.end() - 1)
         key = re.search(r'\bKey\s*=\s*"([^"]+)"', body)
         if not key:
            continue
         group = re.search(r'\bGroup\s*=\s*RestSettings\.(\w+)', body)
         rows.add((groups.get(group.group(1), group.group(1)) if group else default, key.group(1)))
   return rows


def control_panel_literals(texts):
   """Every string literal in the Control Panel's source that could be a setting's
   name. Compared with the name exactly as SettingsDefaults.h spells it: a COM
   property is spelled differently from the row it writes more often than not
   (SMTPNoOfTries writes smtpnoofretries), and a name compared without its case
   would let a COM path stand for a row it does not write."""
   literals = set()
   for text in texts:
      literals.update(re.findall(r'"([A-Za-z][A-Za-z0-9]*)"', text))
   return literals


def control_panel_com(interfaces_needed):
   """(writes, methods): the (interface, member) pairs the Control Panel writes and
   calls, as build/check-deck-parity.py reads them."""
   spec = importlib.util.spec_from_file_location("deck_parity", os.path.join(HERE, "check-deck-parity.py"))
   deck = importlib.util.module_from_spec(spec)
   spec.loader.exec_module(deck)
   reader = deck.DesktopReader(deck.read_idl())
   writes, methods = set(), set()
   for access in reader.read_all():
      if access.kind == "write":
         writes.add((access.iface, access.member))
      elif access.kind == "method":
         methods.add((access.iface, access.member))
   return writes, methods


# ---------------------------------------------------------------------------
# The decision
# ---------------------------------------------------------------------------

def coverage(settings, com_map, rest_map, cp_writes, cp_methods, cp_rest_rows, cp_literals, exempt):
   """For every setting, how it is covered - ('com', what), ('rest', what),
   ('named', name), ('exempt', reason) - or None; and the problems with the
   exemptions themselves. Pure, so the self-test can drive it."""
   covered = {}
   for lower, name in settings.items():
      how = None
      for iface, member, kind in sorted(com_map.get(lower, ())):
         if (kind == "put" and (iface, member) in cp_writes) or (kind == "method" and (iface, member) in cp_methods):
            how = ("com", "%s.%s" % (iface, member))
            break
      if how is None:
         for path, key in sorted(rest_map.get(lower, ())):
            if (path, key) in cp_rest_rows:
               how = ("rest", "%s %s" % (path, key))
               break
      if how is None and name in cp_literals:
         how = ("named", name)
      covered[lower] = how

   problems = []
   exempt_lower = {key.lower(): key for key in exempt}
   for lower, key in sorted(exempt_lower.items()):
      if lower not in settings:
         problems.append("  EXEMPT names %s, which SettingsDefaults.h does not have: delete the exemption." % key)
      elif covered.get(lower):
         problems.append("  EXEMPT names %s, which has an editor (%s %s): delete the exemption." % (key, covered[lower][0], covered[lower][1]))
      elif not exempt[key].strip():
         problems.append("  EXEMPT names %s without a reason." % key)

   for lower in settings:
      if covered[lower] is None and lower in exempt_lower:
         covered[lower] = ("exempt", exempt[exempt_lower[lower]])
   return covered, problems


def self_test():
   """The negative control: a setting nothing edits is refused, one the Control
   Panel writes over COM is not, one it writes over REST is not, one it names is
   not, and a stale exemption and a needless one are both refused."""
   failures = []
   settings = {"alpha": "Alpha", "beta": "Beta", "gamma": "Gamma", "delta": "Delta", "omega": "Omega"}
   com_map = {"alpha": {("Settings", "Alpha", "put")}, "delta": {("Settings", "Delta", "put")}}
   rest_map = {"beta": {("/api/v1/settings", "beta")}, "delta": {("/api/v1/settings", "delta")}}
   covered, problems = coverage(
      settings, com_map, rest_map,
      cp_writes={("Settings", "Alpha")}, cp_methods=set(),
      cp_rest_rows={("/api/v1/settings", "beta")},
      cp_literals={"Gamma", "delta"},
      exempt={"Omega": "the negative control's own reason", "Stale": "a setting that is not there",
              "Alpha": "an exemption for a setting that has an editor"})
   if not covered["alpha"] or covered["alpha"][0] != "com":
      failures.append("  self-test: a setting the Control Panel writes over COM was not counted as edited")
   if not covered["beta"] or covered["beta"][0] != "rest":
      failures.append("  self-test: a setting the Control Panel writes over REST was not counted as edited")
   if not covered["gamma"] or covered["gamma"][0] != "named":
      failures.append("  self-test: a setting the Control Panel names was not counted as edited")
   if covered["delta"] is not None:
      failures.append("  self-test: a setting with a COM property and a REST row that the Control Panel uses neither of, "
                      "and whose name it spells only in another case, was counted as edited")
   if not covered["omega"] or covered["omega"][0] != "exempt":
      failures.append("  self-test: an exempt setting was not counted as exempt")
   if not any("Stale" in problem and "does not have" in problem for problem in problems):
      failures.append("  self-test: an exemption for a setting that is not there was not refused")
   if not any("Alpha" in problem and "has an editor" in problem for problem in problems):
      failures.append("  self-test: an exemption for a setting that has an editor was not refused")
   return failures


def main():
   failures = self_test()
   if "--self-test" in sys.argv:
      for line in failures:
         print(line)
      if failures:
         print("FAIL: the negative control did not hold")
         return 1
      print("self-test: a setting with no editor is refused, the three ways of having one are "
            "counted, and a stale and a needless exemption are refused")
      return 0

   if not os.path.exists(DEFAULTS):
      print("SettingsDefaults.h not found at %s" % DEFAULTS)
      return 1

   settings = database_settings()
   constant_map = constants()
   setter_map = setters(constant_map, settings)
   members = com_members()
   com_map = com_writers(settings, constant_map, setter_map, members)
   rest_map = rest_writers(settings, constant_map, setter_map)

   texts = [read(path) for path in control_panel_files()]
   cp_writes, cp_methods = control_panel_com(None)
   cp_rest_rows = control_panel_rest_rows(texts)
   cp_literals = control_panel_literals(texts)

   covered, problems = coverage(settings, com_map, rest_map, cp_writes, cp_methods, cp_rest_rows, cp_literals, EXEMPT)

   if "--list" in sys.argv:
      for lower in sorted(settings, key=str.lower):
         how = covered[lower]
         print("  %-34s %s" % (settings[lower], "%s: %s" % how if how else "MISSING"))

   missing = [lower for lower in sorted(settings, key=str.lower) if covered[lower] is None]
   for lower in missing:
      paths = []
      for iface, member, kind in sorted(com_map.get(lower, ())):
         paths.append("COM %s.%s" % (iface, member))
      for path, key in sorted(rest_map.get(lower, ())):
         paths.append("REST %s %s" % (path, key))
      print("  no Control Panel editor: %s%s" % (
         settings[lower],
         " (the server writes it through %s)" % ", ".join(paths) if paths else
         " (no COM property and no REST row writes it: an editor needs one first, or an exemption with the reason)"))
   for line in problems + failures:
      print(line)

   if missing or problems or failures:
      print("FAIL: %d database setting(s) without a Control Panel editor, %d problem(s) with the exemptions"
            % (len(missing), len(problems) + len(failures)))
      return 1

   counted = {}
   for how in covered.values():
      counted[how[0]] = counted.get(how[0], 0) + 1
   print("OK    all %d database settings in SettingsDefaults.h have a Control Panel editor (%d over COM, "
         "%d over the REST API, %d named) or a recorded reason for none (%d exempt), and every "
         "exemption is for a setting that is there and has no editor"
         % (len(settings), counted.get("com", 0), counted.get("rest", 0), counted.get("named", 0), counted.get("exempt", 0)))
   return 0


if __name__ == "__main__":
   sys.exit(main())
