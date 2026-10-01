#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The two verbs that run elevated from the Control Panel's Install update write
nothing where the service can write, and load nothing from where it can put a DLL.

An administrator's install of an update (hmailserver/docs/ServiceAccount.md, "The
live update") runs hMailServer.exe /InstallUpdate elevated, after Windows' own
prompt, and that stage runs hMailServer.exe --verify-bundle with the rights taken
away. The log folder, the error log and the data folder are the service's to
write under NT SERVICE\\hMailServer, so a line an administrator's process wrote
there could go wherever a link the service planted leads (R3 of the design, F5 of
its review). And a DLL loaded from the current directory or PATH before the verb
is even reached would run with the administrator's token (F4). This holds the
code to both:

  1. The files the verbs are made of - AdminUpdate, AdminUpdateChecks,
     AuthenticodePublisher and LoweredChild - name no LOG_ macro, no
     ErrorManager and no CrashOracle, comments and strings aside. They may call
     Logger::ForbidWritesForThisProcess and Logger::SuppressedWrites, which is
     the guard for what they reach (Logger stops every write for the rest of the
     process's life once the first is called).
  2. hMailServer.cpp dispatches both verbs in _tWinMain before
     CrashOracle::Install, and each verb's first two statements are
     Logger::ForbidWritesForThisProcess() and SetDefaultDllDirectories(...).
  3. hMailServer.vcxproj links the server with /DEPENDENTLOADFLAG:0xA00
     (LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) in
     every configuration: its static imports resolve from Bin - which the
     permission rule holds to administrators - and System32, never the current
     directory or PATH. hMailServer.Updater.vcxproj keeps its
     /DEPENDENTLOADFLAG:0x800.

Run from the repository root: python build/check-elevated-verbs.py
Pass --self-test to break each rule in a copy and prove the check reports it.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
UTIL = os.path.join(SERVER, "Common", "Util")

VERB_FILES = [
    os.path.join(UTIL, name)
    for name in ("AdminUpdate.cpp", "AdminUpdate.h", "AdminUpdateChecks.cpp", "AdminUpdateChecks.h",
                 "AuthenticodePublisher.cpp", "AuthenticodePublisher.h", "LoweredChild.cpp", "LoweredChild.h")
]
MAIN = os.path.join(SERVER, "hMailServer", "hMailServer.cpp")
PROJECT = os.path.join(SERVER, "hMailServer", "hMailServer.vcxproj")
UPDATER_PROJECT = os.path.join(SERVER, "hMailServer.Updater", "hMailServer.Updater.vcxproj")

FORBIDDEN = re.compile(r"\b(LOG_[A-Z_]+|ErrorManager|CrashOracle)\b")
ALLOWED_LOGGER = re.compile(r"\bLogger::(ForbidWritesForThisProcess|SuppressedWrites)\s*\(")
ANY_LOGGER = re.compile(r"\bLogger::\w+")


def code_only(text):
    """The text with comments and string and character literals blanked, lines kept."""
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
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * max(0, j - i - 2) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_files(texts):
    problems = []
    for path, text in texts.items():
        code = code_only(text)
        for number, line in enumerate(code.split("\n"), 1):
            for match in FORBIDDEN.finditer(line):
                problems.append("%s:%d: %s in a file of the elevated verbs" % (os.path.relpath(path, ROOT), number, match.group(1)))
            for match in ANY_LOGGER.finditer(line):
                if not ALLOWED_LOGGER.match(line, match.start()):
                    problems.append("%s:%d: %s in a file of the elevated verbs (only ForbidWritesForThisProcess and SuppressedWrites)"
                                    % (os.path.relpath(path, ROOT), number, match.group(0)))
    return problems


def function_body(code, signature):
    start = code.find(signature)
    if start < 0:
        return None
    brace = code.find("{", start)
    depth = 0
    for i in range(brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[brace + 1:i]
    return None


def check_dispatch(main_text, verb_text):
    problems = []
    code = code_only(main_text)
    body = function_body(code, "_tWinMain(")
    if body is None:
        return ["hMailServer.cpp: _tWinMain not found"]
    oracle = body.find("CrashOracle::Install(")
    for call in ("AdminUpdate::IsVerifyBundleCommandLine()", "AdminUpdate::IsInstallUpdateCommandLine()"):
        at = body.find(call)
        if at < 0:
            problems.append("hMailServer.cpp: _tWinMain does not dispatch %s" % call)
        elif oracle >= 0 and at > oracle:
            problems.append("hMailServer.cpp: %s is dispatched after CrashOracle::Install" % call)

    verb_code = code_only(verb_text)
    for verb in ("AdminUpdate::RunVerifyBundleChild()", "AdminUpdate::RunInstallUpdateCommand()"):
        body = function_body(verb_code, verb)
        if body is None:
            problems.append("AdminUpdate.cpp: %s not found" % verb)
            continue
        statements = [s.strip() for s in body.split(";") if s.strip()][:2]
        if len(statements) < 2 or statements[0] != "Logger::ForbidWritesForThisProcess()" or not statements[1].startswith("SetDefaultDllDirectories("):
            problems.append("AdminUpdate.cpp: %s does not begin with Logger::ForbidWritesForThisProcess() and SetDefaultDllDirectories(...)" % verb)
    return problems


def check_projects(project_text, updater_text):
    problems = []
    groups = re.findall(r"<ItemDefinitionGroup Condition=\"'\$\(Configuration\)\|\$\(Platform\)'=='([^']+)'\">(.*?)</ItemDefinitionGroup>", project_text, re.S)
    configurations = 0
    for name, group in groups:
        link = re.search(r"<Link>(.*?)</Link>", group, re.S)
        if not link:
            continue
        configurations += 1
        options = re.search(r"<AdditionalOptions>(.*?)</AdditionalOptions>", link.group(1), re.S)
        if not options or "/DEPENDENTLOADFLAG:0xA00" not in options.group(1):
            problems.append("hMailServer.vcxproj: %s does not link with /DEPENDENTLOADFLAG:0xA00" % name)
    if configurations == 0:
        problems.append("hMailServer.vcxproj: no configuration's link options found")
    if updater_text.count("/DEPENDENTLOADFLAG:0x800") < 3:
        problems.append("hMailServer.Updater.vcxproj: not every configuration links with /DEPENDENTLOADFLAG:0x800")
    return problems


def read(path):
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        return f.read()


def run(texts, main_text, project_text, updater_text):
    problems = check_files(texts)
    problems += check_dispatch(main_text, texts[VERB_FILES[0]])
    problems += check_projects(project_text, updater_text)
    return problems


def self_test(texts, main_text, project_text, updater_text):
    failures = []

    def expect(label, found, needle):
        if not any(needle in p for p in found):
            failures.append("the check did not report %s" % label)

    stage = VERB_FILES[0]
    mutated = dict(texts)
    mutated[stage] = texts[stage] + "\nvoid F() { LOG_APPLICATION(_T(\"x\")); }\n"
    expect("a LOG_ line in the stage", run(mutated, main_text, project_text, updater_text), "LOG_APPLICATION")
    mutated = dict(texts)
    mutated[VERB_FILES[2]] = texts[VERB_FILES[2]] + "\nvoid F() { ErrorManager::Instance(); }\n"
    expect("an ErrorManager call in the checks", run(mutated, main_text, project_text, updater_text), "ErrorManager")
    mutated = dict(texts)
    mutated[VERB_FILES[4]] = texts[VERB_FILES[4]] + "\nvoid F() { Logger::Instance()->LogError(x); }\n"
    expect("another Logger call", run(mutated, main_text, project_text, updater_text), "Logger::Instance")
    # A comment naming LOG_ is not a call.
    mutated = dict(texts)
    mutated[stage] = texts[stage] + "\n// LOG_APPLICATION is never used here\n"
    if run(mutated, main_text, project_text, updater_text) != run(texts, main_text, project_text, updater_text):
        failures.append("the check reported a comment as a call")

    oracle = "HM::CrashOracle::Install();"
    dispatch = "   if (HM::AdminUpdate::IsVerifyBundleCommandLine())"
    if oracle in main_text and dispatch in main_text:
        moved = main_text.replace(dispatch, "   HM::CrashOracle::Install();\n" + dispatch, 1)
        expect("a dispatch after the crash oracle", run(texts, moved, project_text, updater_text), "after CrashOracle::Install")
    else:
        failures.append("hMailServer.cpp's dispatch is not where the mutation looks for it")

    mutated = dict(texts)
    mutated[stage] = texts[stage].replace("      Logger::ForbidWritesForThisProcess();\n      SetDefaultDllDirectories", "      SetDefaultDllDirectories", 1)
    expect("a verb that does not forbid writes first", run(mutated, main_text, project_text, updater_text), "does not begin with")

    expect("a configuration without the load flag",
           run(texts, main_text, project_text.replace(" /DEPENDENTLOADFLAG:0xA00", "", 1), updater_text), "does not link with")
    expect("the Updater without its load flag",
           run(texts, main_text, project_text, updater_text.replace("/DEPENDENTLOADFLAG:0x800", "", 1)), "Updater")
    return failures


def main():
    texts = {}
    for path in VERB_FILES:
        if not os.path.exists(path):
            print("  FAIL  %s: not found" % os.path.relpath(path, ROOT))
            return 1
        texts[path] = read(path)
    main_text = read(MAIN)
    project_text = read(PROJECT)
    updater_text = read(UPDATER_PROJECT)

    if "--self-test" in sys.argv[1:]:
        failures = self_test(texts, main_text, project_text, updater_text)
        for failure in failures:
            print("  FAIL  self-test: %s" % failure)
        if failures:
            return 1
        print("  OK    self-test: every mutation of the elevated verbs' rules was reported")
        return 0

    problems = run(texts, main_text, project_text, updater_text)
    for problem in problems:
        print("  FAIL  " + problem)
    if problems:
        return 1
    print("  OK    the elevated verbs log nothing, are dispatched before the crash oracle, forbid the logger's writes and "
          "limit DLL loading first, and the server links with /DEPENDENTLOADFLAG:0xA00")
    return 0


if __name__ == "__main__":
    sys.exit(main())
