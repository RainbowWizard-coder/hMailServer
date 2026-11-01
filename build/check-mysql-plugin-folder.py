#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The MySQL client's plugin folder is the one MySQLConnection::PluginFolderOption gave.

On Windows the bundled MariaDB Connector/C is handed its authentication-plugin
folder with mysql_options(HM_MYSQL_PLUGIN_DIR), and loads a plugin from the name
it makes of that folder. MySQLConnection::PluginFolderOption decides the folder:
Bin\\plugin as MySQLConnection::PluginDirectoryBytes writes it, where that is an
absolute folder, or else a folder under the system folder that holds no plugin,
and never the empty value. MySQLConnectionTester, which ClassTester runs, holds
what PluginFolderOption answers, but never makes a connection, so it cannot see
whether the connection hands the client that answer: the option set from
PluginDirectoryBytes again, or from anything else, would compile and pass it.
This check holds the use.

It fails, in the server's C and C++ (hmailserver/source/Server), comments and
strings skipped, where:

- HM_MYSQL_PLUGIN_DIR, or another of MySQLInterface.h's option numbers with its
  value (but the Oracle client's, HM_ORACLE_*), is set anywhere but in one
  p_mysql_options call in MySQLConnection::ConnectOnce_, on dbconn_, with
  pluginFolder.c_str() as its value; or HM_MYSQL_PLUGIN_DIR appears anywhere
  else but its #define in MySQLInterface.h;
- a p_mysql_options call's option is not one of MySQLInterface.h's numbers by its
  name, or p_mysql_options is taken, outside MySQLInterface.cpp and .h, other
  than to call it or to test it against 0;
- ConnectOnce_'s pluginFolder is not its eighth parameter, a const AnsiString &,
  or is used there other than in one "if (pluginFolder.IsEmpty())" whose
  statement returns 0, before the option's call in the same block, in the
  option's value, and in "(void) pluginFolder;" on POSIX;
- ConnectOnce_ is called outside MySQLConnection::Connect, or with anything but
  Connect's pluginFolder as its eighth argument; Connect's pluginFolder is
  declared other than as "AnsiString pluginFolder;" or used other than as the
  second argument of its one call of PluginFolderOption and in those calls;
- PluginDirectoryBytes is used outside PluginFolderOption and
  MySQLConnectionTester, or PluginFolderOption outside Connect and the tester.

Run from the repository root: python build/check-mysql-plugin-folder.py
Pass --self-test to run it against the tree with each rule broken in a copy - the
connection handed the plugin folder from PluginDirectoryBytes again, as before
PluginFolderOption, among them - and prove it can fail.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREE = os.path.join(ROOT, "hmailserver", "source", "Server")
EXTENSIONS = (".cpp", ".h", ".hpp", ".c", ".cc", ".inl")

SQL = "hmailserver/source/Server/Common/SQL/"
CONNECTION = SQL + "MySQLConnection.cpp"
INTERFACE_H = SQL + "MySQLInterface.h"
INTERFACE_CPP = SQL + "MySQLInterface.cpp"

OPTION = "HM_MYSQL_PLUGIN_DIR"
NAMES = ("PluginDirectoryBytes", "PluginFolderOption", "ConnectOnce_", OPTION, "p_mysql_options")
IDENTIFIER = r"[A-Za-z_]\w*"
NOT_TYPES = {"return", "else", "case", "do", "throw", "new", "delete", "sizeof", "co_return"}


def without_comments(text):
    """The text with comments and the insides of string and character literals blanked, its length and line breaks kept."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:end]))
            i = end
            continue
        c = text[i]
        if c == '"' or (c == "'" and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_"))):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            # The literal's length is kept, so that offsets in the code are the text's.
            out.append(c + " " * max(0, min(j, n) - i - 1) + (c if j < n and text[j] == c else ""))
            i = j + 1 if j < n and text[j] == c else j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def arguments(code, open_paren):
    """[(argument stripped, its offset)] of the call whose ( is at open_paren, and the offset after its )."""
    depth = 0
    args = []
    start = open_paren + 1
    i = open_paren

    def add(end):
        raw = code[start:end]
        stripped = raw.strip()
        args.append((stripped, start + (len(raw) - len(raw.lstrip()))))

    while i < len(code):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                add(i)
                return args, i + 1
        elif c == "," and depth == 1:
            add(i)
            start = i + 1
        i += 1
    add(len(code))
    return args, len(code)


def matching_brace(code, open_brace):
    depth = 0
    for i in range(open_brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(code) - 1


def brace_depths(code):
    depths = []
    depth = 0
    for c in code:
        if c == "{":
            depth += 1
        depths.append(depth)
        if c == "}":
            depth -= 1
    return depths


def line_of(code, offset):
    return code.count("\n", 0, offset) + 1


def normal(text):
    return re.sub(r"\s*([&*])\s*", r" \1", " ".join(text.split())).strip()


QUALIFIED = re.compile(r"\b(" + IDENTIFIER + r")\s*::\s*(~?" + IDENTIFIER + r")\s*\(")
TRAIL = re.compile(r"(?:\s|\bconst\b|\bnoexcept\b|\boverride\b)*")


def definitions(code):
    """{(class, name): [(offset of the name, parameters, body start, body end)]} of the qualified definitions."""
    found = {}
    for match in QUALIFIED.finditer(code):
        params, after = arguments(code, match.end() - 1)
        at = TRAIL.match(code, after).end()
        if at < len(code) and code[at] == "{":
            found.setdefault((match.group(1), match.group(2)), []).append((match.start(2), params, at, matching_brace(code, at)))
    return found


def is_declaration(code, offset, name):
    """Whether the name at offset is declared there (a type before it and a parameter list after), not used."""
    before = code[max(0, offset - 400):offset].rstrip()
    after = code[offset + len(name):offset + len(name) + 400].lstrip()
    if not after.startswith("("):
        return False
    while before.endswith(("*", "&")):
        before = before[:-1].rstrip()
        if before.endswith(("=", "(", ",", "&", "|", "!", "?", ":")):
            return False
    word = re.search(r"(" + IDENTIFIER + r")$", before)
    return word is not None and word.group(1) not in NOT_TYPES


def macros(code):
    """{name: value} of the #defines of numbers in the header."""
    values = {}
    for match in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(HM_\w+)[ \t]+(\d+)[uUlL]*\b", code, re.M):
        values[match.group(1)] = int(match.group(2))
    return values


def problems_of(sources):
    """The problems of the tree, given {relative path: text}; and a summary of what was seen."""
    problems = []
    codes = {path: without_comments(text) for path, text in sources.items()
             if any(name in text for name in NAMES)}
    seen = {"files": len(sources), "option calls": 0, "ConnectOnce_ calls": 0}

    connection = codes.get(CONNECTION)
    header = codes.get(INTERFACE_H)
    if connection is None or header is None:
        return ["%s or %s no longer names the plugin folder's option: this check has stopped seeing the source"
                % (CONNECTION, INTERFACE_H)], seen

    numbers = macros(header)
    if OPTION not in numbers:
        return ["%s does not #define %s as a number" % (INTERFACE_H, OPTION)], seen
    plugin_number = numbers[OPTION]
    for name, value in sorted(numbers.items()):
        if value == plugin_number and name != OPTION and not name.startswith("HM_ORACLE_"):
            problems.append("%s: %s is #defined as %d, the number of %s, the plugin folder's option"
                            % (INTERFACE_H, name, value, OPTION))

    defined = definitions(connection)
    spans = {}
    for key in (("MySQLConnection", "Connect"), ("MySQLConnection", "ConnectOnce_"), ("MySQLConnection", "PluginFolderOption")):
        found = defined.get(key, [])
        if len(found) != 1:
            problems.append("%s: %d definitions of %s::%s, not one" % (CONNECTION, len(found), key[0], key[1]))
            return problems, seen
        spans[key[1]] = found[0]
    testers = [(start, end) for (cls, _), entries in defined.items() if cls == "MySQLConnectionTester"
               for _, _, start, end in entries]
    if not testers:
        problems.append("%s: MySQLConnectionTester has no definition" % CONNECTION)

    def inside(path, offset, *names):
        if path != CONNECTION:
            return False
        for name in names:
            if name == "tester":
                if any(start <= offset <= end for start, end in testers):
                    return True
            elif spans[name][2] <= offset <= spans[name][3]:
                return True
        return False

    connect_once_at, connect_once_params, once_start, once_end = spans["ConnectOnce_"]
    connect_start, connect_end = spans["Connect"][2], spans["Connect"][3]
    definition_offsets = {(CONNECTION, entry[0]) for entries in defined.values() for entry in entries}

    # ConnectOnce_'s pluginFolder: its eighth parameter, by reference to a constant.
    if len(connect_once_params) < 8 or normal(connect_once_params[7][0]) != normal("const AnsiString &pluginFolder"):
        problems.append("%s:%d: ConnectOnce_'s eighth parameter is %s, not const AnsiString &pluginFolder"
                        % (CONNECTION, line_of(connection, connect_once_at),
                           connect_once_params[7][0] if len(connect_once_params) >= 8 else "missing"))

    # Every p_mysql_options call: an option number by its name; the plugin folder's,
    # once, in ConnectOnce_, from pluginFolder.
    option_calls = []
    plugin_value_offsets = set()
    plugin_option_args = set()
    for path, code in sorted(codes.items()):
        for match in re.finditer(r"\bp_mysql_options\b", code):
            offset = match.start()
            rest = code[match.end():]
            if path in (INTERFACE_H, INTERFACE_CPP):
                if not re.match(r"\s*\(", rest):
                    continue
            elif re.match(r"\s*[!=]=\s*(?:0|nullptr|NULL)\b", rest):
                continue
            elif not re.match(r"\s*\(", rest):
                problems.append("%s:%d: p_mysql_options is taken other than to call it or to test it against 0"
                                % (path, line_of(code, offset)))
                continue
            args, _ = arguments(code, match.end() + (len(rest) - len(rest.lstrip())))
            if len(args) < 3:
                if path not in (INTERFACE_H, INTERFACE_CPP):
                    problems.append("%s:%d: p_mysql_options is called with %d argument(s)" % (path, line_of(code, offset), len(args)))
                continue
            option = args[1][0]
            if option not in numbers:
                problems.append("%s:%d: p_mysql_options is handed the option %s, not one of %s's numbers by its name"
                                % (path, line_of(code, offset), " ".join(option.split()), INTERFACE_H))
                continue
            if option != OPTION and (numbers[option] != plugin_number or option.startswith("HM_ORACLE_")):
                continue
            seen["option calls"] += 1
            plugin_option_args.add((path, args[1][1]))
            if not inside(path, offset, "ConnectOnce_"):
                problems.append("%s:%d: the plugin folder's option (%s) is set outside MySQLConnection::ConnectOnce_"
                                % (path, line_of(code, offset), option))
                continue
            if args[0][0] != "dbconn_" or normal(args[2][0]) != normal("pluginFolder.c_str()") or len(args) != 3:
                problems.append("%s:%d: the plugin folder's option is set to %s, not to pluginFolder.c_str() on dbconn_"
                                % (path, line_of(code, offset), " ".join(args[2][0].split())))
            plugin_value_offsets.add(args[2][1])
            option_calls.append(offset)
    if seen["option calls"] != 1 and not any("option" in p and "outside" in p for p in problems):
        problems.append("the plugin folder's option is set in %d call(s), not one" % seen["option calls"])

    # HM_MYSQL_PLUGIN_DIR itself: its #define, and the option of that one call.
    for path, code in sorted(codes.items()):
        for match in re.finditer(r"\b" + OPTION + r"\b", code):
            if (path, match.start()) in plugin_option_args:
                continue
            if path == INTERFACE_H and re.search(r"#[ \t]*define[ \t]+$", code[code.rfind("\n", 0, match.start()) + 1:match.start()]):
                continue
            problems.append("%s:%d: %s is used other than as the option of the one p_mysql_options call that sets it"
                            % (path, line_of(code, match.start()), OPTION))

    # ConnectOnce_'s body: pluginFolder refused when empty, then handed over.
    depths = brace_depths(connection)
    guards = [m for m in re.finditer(r"\bif\s*\(\s*pluginFolder\s*\.\s*IsEmpty\s*\(\s*\)\s*\)", connection)
              if once_start <= m.start() <= once_end]
    guard_end = None
    if len(guards) != 1:
        problems.append("%s:%d: ConnectOnce_ has %d \"if (pluginFolder.IsEmpty())\", not one before the plugin folder's option"
                        % (CONNECTION, line_of(connection, connect_once_at), len(guards)))
    else:
        guard = guards[0]
        statement = guard.end() + (len(connection[guard.end():]) - len(connection[guard.end():].lstrip()))
        if statement < len(connection) and connection[statement] == "{":
            end = matching_brace(connection, statement)
            body = connection[statement + 1:end]
        else:
            end = connection.find(";", statement)
            body = connection[statement:end + 1]
        if not re.search(r"\breturn\s+(?:0|nullptr|NULL)\s*;", body):
            problems.append("%s:%d: ConnectOnce_'s \"if (pluginFolder.IsEmpty())\" does not return 0"
                            % (CONNECTION, line_of(connection, guard.start())))
        guard_end = end
        for call in option_calls:
            if call < guard_end or depths[call] != depths[guard.start()] or min(depths[guard.start():call + 1]) < depths[guard.start()]:
                problems.append("%s:%d: the plugin folder's option is not set after ConnectOnce_'s \"if (pluginFolder.IsEmpty())\" "
                                "returned, in the same block" % (CONNECTION, line_of(connection, call)))
    guard_offsets = {m.start() for m in re.finditer(r"\bpluginFolder\b", connection)
                     if any(g.start() <= m.start() <= g.end() for g in guards)}
    for match in re.finditer(r"\bpluginFolder\b", connection):
        offset = match.start()
        if not once_start <= offset <= once_end or offset in guard_offsets or offset in plugin_value_offsets:
            continue
        if re.search(r"\(\s*void\s*\)\s*$", connection[max(0, offset - 100):offset]) and re.match(r"\s*;", connection[match.end():match.end() + 100]):
            continue
        problems.append("%s:%d: ConnectOnce_ uses pluginFolder other than to refuse it when empty and hand it to the client"
                        % (CONNECTION, line_of(connection, offset)))

    # ConnectOnce_ called from Connect alone, with Connect's pluginFolder; Connect's
    # pluginFolder written by PluginFolderOption alone; the two helpers called where
    # they may be.
    allowed_folder = set()
    folder_options = 0
    for path, code in sorted(codes.items()):
        for name in ("ConnectOnce_", "PluginFolderOption", "PluginDirectoryBytes"):
            for match in re.finditer(r"\b" + name + r"\b", code):
                offset = match.start()
                if (path, offset) in definition_offsets or is_declaration(code, offset, name):
                    continue
                number = line_of(code, offset)
                rest = code[match.end():]
                call = re.match(r"\s*\(", rest)
                args = arguments(code, match.end() + call.end() - 1)[0] if call else []
                if name == "ConnectOnce_":
                    if not inside(path, offset, "Connect"):
                        problems.append("%s:%d: ConnectOnce_ is used outside MySQLConnection::Connect" % (path, number))
                    elif len(args) < 8 or args[7][0] != "pluginFolder":
                        problems.append("%s:%d: ConnectOnce_ is handed %s as the plugin folder, not Connect's pluginFolder"
                                        % (path, number, " ".join(args[7][0].split()) if len(args) >= 8 else "nothing"))
                    else:
                        seen["ConnectOnce_ calls"] += 1
                        allowed_folder.add(args[7][1])
                elif name == "PluginFolderOption":
                    if inside(path, offset, "tester"):
                        continue
                    if not inside(path, offset, "Connect"):
                        problems.append("%s:%d: PluginFolderOption is used outside MySQLConnection::Connect and MySQLConnectionTester"
                                        % (path, number))
                        continue
                    folder_options += 1
                    if len(args) != 3 or args[1][0] != "pluginFolder":
                        problems.append("%s:%d: Connect's PluginFolderOption writes %s, not pluginFolder"
                                        % (path, number, args[1][0] if len(args) >= 2 else "nothing"))
                    else:
                        allowed_folder.add(args[1][1])
                elif not inside(path, offset, "PluginFolderOption", "tester"):
                    problems.append("%s:%d: PluginDirectoryBytes is used outside MySQLConnection::PluginFolderOption and "
                                    "MySQLConnectionTester: the client is handed the folder PluginFolderOption gives"
                                    % (path, number))
    if folder_options != 1:
        problems.append("%s: Connect calls PluginFolderOption %d time(s), not once" % (CONNECTION, folder_options))
    if seen["ConnectOnce_ calls"] == 0:
        problems.append("%s: Connect never calls ConnectOnce_ with its pluginFolder" % CONNECTION)

    declarations = 0
    for match in re.finditer(r"\bpluginFolder\b", connection):
        offset = match.start()
        if not connect_start <= offset <= connect_end or offset in allowed_folder:
            continue
        if re.search(r"\bAnsiString\s+$", connection[max(0, offset - 100):offset]) and re.match(r"\s*;", connection[match.end():match.end() + 100]):
            declarations += 1
            continue
        problems.append("%s:%d: Connect uses pluginFolder other than to have PluginFolderOption write it and hand it to ConnectOnce_"
                        % (CONNECTION, line_of(connection, offset)))
    if declarations != 1:
        problems.append("%s: Connect declares \"AnsiString pluginFolder;\" %d time(s), not once" % (CONNECTION, declarations))

    return problems, seen


def files():
    for directory, _, names in os.walk(TREE):
        for name in sorted(names):
            if name.lower().endswith(EXTENSIONS):
                path = os.path.join(directory, name)
                yield os.path.relpath(path, ROOT).replace(os.sep, "/"), path


def read(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        return f.read().replace("\r\n", "\n")


def sources():
    return {relative: read(path) for relative, path in files()}


# The connection as it was before PluginFolderOption (e8205013b2): the folder
# written again in ConnectOnce_, and the empty value handed over where it could not
# be written.
BEFORE = ("         AnsiString sPluginDir;\n"
          "         if (!PluginDirectoryBytes(client->GetLibraryDirectory() + _T(\"\\\\plugin\"), sPluginDir))\n"
          "            sPluginDir.Empty();\n"
          "         client->p_mysql_options(dbconn_, HM_MYSQL_PLUGIN_DIR, sPluginDir.c_str());\n")
GUARD = ("         if (pluginFolder.IsEmpty())\n"
         "         {\n")
SET = "         client->p_mysql_options(dbconn_, HM_MYSQL_PLUGIN_DIR, pluginFolder.c_str());\n"
SECOND_CALL = ("            pResult = ConnectOnce_(sServer, sUsername, sPassword, sDatabase, lDBPort, DatabaseTls::ModeDisable, "
               "String(), pluginFolder, sErrorMessage, errorNumber);")
DECLARED = "         AnsiString pluginFolder;\n"
TLS = "      MySQLInterface *client = MySQLInterface::Instance();\n"


def guarded_block(text):
    """The text of ConnectOnce_'s guard and the option's call, from the guard to the call's line."""
    start = text.find(GUARD)
    end = text.find(SET, start)
    return text[start:end + len(SET)] if start >= 0 and end >= 0 else None


def self_test():
    failures = []
    tree = sources()
    problems, seen = problems_of(tree)
    if problems:
        failures.append("the tree passes: %s" % problems[:3])
    if seen["files"] < 1000:
        failures.append("only %d files were read: this check has stopped seeing the source" % seen["files"])
    if seen["option calls"] != 1 or seen["ConnectOnce_ calls"] != 2:
        failures.append("%d option call(s) and %d ConnectOnce_ call(s) were seen, where the tree had 1 and 2 when this check "
                        "was written" % (seen["option calls"], seen["ConnectOnce_ calls"]))

    connection = tree.get(CONNECTION, "")
    block = guarded_block(connection)
    if block is None:
        failures.append("ConnectOnce_'s guard and the option's call were not found as written: the controls below need them")
        block = "\0"
    # ApplyTlsOptions_ is the function after ConnectOnce_: its first line.
    tls_at = connection.find(TLS, connection.find("MySQLConnection::ApplyTlsOptions_("))

    controls = [0]

    def refused(label, path, text, expected):
        changed = dict(tree)
        changed[path] = text
        found, _ = problems_of(changed)
        controls[0] += 1
        if not found:
            failures.append("%s: not refused" % label)
        for words in ((expected,) if isinstance(expected, str) else expected):
            if found and not any(words in problem for problem in found):
                failures.append("%s: refused, but not for \"%s\": %s" % (label, words, found[:3]))

    def control(label, path, old, new, expected):
        text = tree.get(path, "")
        if text.count(old) != 1:
            failures.append("%s: the text the control changes is in %s %d time(s), not once" % (label, path, text.count(old)))
            return
        refused(label, path, text.replace(old, new), expected)

    def insert_in_tls(label, line, expected):
        if tls_at < 0:
            failures.append("%s: ApplyTlsOptions_'s first line was not found" % label)
            return
        at = tls_at + len(TLS)
        refused(label, CONNECTION, connection[:at] + line + connection[at:], expected)

    # The shape before PluginFolderOption, put back where it was: the defect.
    control("the folder written again in ConnectOnce_, empty where it cannot be", CONNECTION, block, BEFORE,
            ("PluginDirectoryBytes is used outside", "set to sPluginDir.c_str()"))
    control("the empty value handed over", CONNECTION, SET, SET.replace("pluginFolder.c_str()", "\"\""), "set to \"\"")
    control("Bin\\plugin's bytes handed over, refused or not", CONNECTION, block,
            "         AnsiString bytes;\n         PluginDirectoryBytes(client->GetLibraryDirectory(), bytes);\n" +
            SET.replace("pluginFolder.c_str()", "bytes.c_str()"), "PluginDirectoryBytes is used outside")
    control("a copy of pluginFolder handed over", CONNECTION, block,
            "         AnsiString copy = pluginFolder;\n" + block.replace("pluginFolder.c_str()", "copy.c_str()"),
            "ConnectOnce_ uses pluginFolder other than")
    control("the empty folder not refused", CONNECTION, block, SET, "has 0 \"if (pluginFolder.IsEmpty())\"")
    control("the empty folder refused without returning", CONNECTION, "            return 0;\n         }\n" + SET,
            "            error.Empty();\n         }\n" + SET, "does not return 0")
    control("the option set before the empty folder is refused", CONNECTION, block, SET + block.replace(SET, ""),
            "is not set after")
    control("the option set inside the guard", CONNECTION, block, block.replace("            return 0;\n", "            return 0;\n" + SET).replace("         }\n" + SET, "         }\n"),
            "is not set after")
    control("the option set twice", CONNECTION, SET, SET + SET, "not one")
    insert_in_tls("the option set in ApplyTlsOptions_", "      client->p_mysql_options(dbconn_, HM_MYSQL_PLUGIN_DIR, \"\");\n",
                  "is set outside MySQLConnection::ConnectOnce_")
    insert_in_tls("the option set by its number", "      client->p_mysql_options(dbconn_, 22, \"\");\n",
                  "not one of")
    insert_in_tls("the option's name kept in a variable", "      const int option = HM_MYSQL_PLUGIN_DIR;\n",
                  "HM_MYSQL_PLUGIN_DIR is used other than")
    insert_in_tls("p_mysql_options kept in a variable", "      hm_mysql_options *set = client->p_mysql_options;\n",
                  "taken other than to call it")
    control("another name for the option's number", INTERFACE_H, "#define HM_MYSQL_DEFAULT_AUTH  23\n",
            "#define HM_MYSQL_DEFAULT_AUTH  23\n#define HM_MYSQL_PLUGIN_FOLDER 22\n", "the number of HM_MYSQL_PLUGIN_DIR")
    control("an alias of the option's name", INTERFACE_H, "#define HM_MYSQL_DEFAULT_AUTH  23\n",
            "#define HM_MYSQL_DEFAULT_AUTH  23\n#define HM_PLUGINS HM_MYSQL_PLUGIN_DIR\n", "HM_MYSQL_PLUGIN_DIR is used other than")
    control("pluginFolder taken by value", CONNECTION, "const String &caFile, const AnsiString &pluginFolder, String &error",
            "const String &caFile, AnsiString pluginFolder, String &error", "eighth parameter")
    control("ConnectOnce_ handed another folder", CONNECTION, SECOND_CALL, SECOND_CALL.replace("pluginFolder", "AnsiString()"),
            "as the plugin folder, not Connect's pluginFolder")
    control("pluginFolder written again in Connect", CONNECTION, "         unsigned int errorNumber = 0;\n",
            "         pluginFolder = \"\";\n         unsigned int errorNumber = 0;\n", "Connect uses pluginFolder other than")
    control("pluginFolder declared with a value", CONNECTION, DECLARED, "         AnsiString pluginFolder = \"x\";\n",
            "Connect uses pluginFolder other than")
    control("PluginFolderOption's answer written elsewhere", CONNECTION, "_T(\"\\\\plugin\"), pluginFolder, pluginFolderStandIn)",
            "_T(\"\\\\plugin\"), other, pluginFolderStandIn)", "writes other, not pluginFolder")
    control("PluginFolderOption not called by Connect", CONNECTION, "             !PluginFolderOption(", "             !(bool) (",
            "calls PluginFolderOption 0 time(s)")
    insert_in_tls("ConnectOnce_ called outside Connect",
                  "      ConnectOnce_(String(), String(), String(), String(), 0, mode, caFile, AnsiString(), error, errno_);\n",
                  "ConnectOnce_ is used outside")
    control("PluginDirectoryBytes called by a helper of its own", CONNECTION, "      bool IsAbsoluteFolderBytes(const AnsiString &bytes)\n      {\n",
            "      bool IsAbsoluteFolderBytes(const AnsiString &bytes)\n      {\n         AnsiString b; MySQLConnection::PluginDirectoryBytes(String(), b);\n",
            "PluginDirectoryBytes is used outside")
    control("PluginDirectoryBytes called in another file", SQL + "DALConnection.h", "#pragma once\n",
            "#pragma once\ninline bool F_(AnsiString &b) { return HM::MySQLConnection::PluginDirectoryBytes(HM::String(), b); }\n",
            "PluginDirectoryBytes is used outside")
    control("PluginFolderOption called in another file", SQL + "DALConnection.h", "#pragma once\n",
            "#pragma once\ninline bool F_(AnsiString &b, bool &s) { return HM::MySQLConnection::PluginFolderOption(HM::String(), b, s); }\n",
            "PluginFolderOption is used outside")

    # Comments and strings that name them are not uses.
    changed = dict(tree)
    changed[CONNECTION] = connection.replace(TLS, TLS + "      // client->p_mysql_options(dbconn_, HM_MYSQL_PLUGIN_DIR, \"\"); PluginDirectoryBytes(x, y);\n"
                                             "      /* ConnectOnce_(a); PluginFolderOption(b); */\n"
                                             "      LOG_DEBUG(\"HM_MYSQL_PLUGIN_DIR PluginDirectoryBytes( p_mysql_options = 0\");\n", 1)
    found, _ = problems_of(changed)
    if found:
        failures.append("the names in a comment and a string are refused: %s" % found[:3])

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    print("self-test: %d failure(s), %d rule(s) broken in a copy of the tree and refused" % (len(failures), controls[0]))
    return 1 if failures else 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    problems, seen = problems_of(sources())
    if problems:
        print("The MySQL client may be handed a plugin folder MySQLConnection::PluginFolderOption did not give:")
        for problem in problems:
            print("  " + problem)
        return 1

    print("check-mysql-plugin-folder: %d C and C++ files; the plugin folder's option is set in %d call, in ConnectOnce_, to the "
          "folder Connect had PluginFolderOption write, handed in %d call(s) of ConnectOnce_; PluginDirectoryBytes is used "
          "by PluginFolderOption and the tester alone" % (seen["files"], seen["option calls"], seen["ConnectOnce_ calls"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
