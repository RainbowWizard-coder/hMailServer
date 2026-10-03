#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The verbs an administrator runs write nothing where the service can write, and
the two that run elevated from the Control Panel's Install update load nothing
from where it can put a DLL.

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
  4. Every OTHER verb hMailServer.exe takes is an administrator's too - Setup,
     DBSetup and the Control Panel run /Register, /RegisterTypeLib,
     /Unregister, --apply-new-install-defaults, --apply-profile and
     --create-stored-secrets-key elevated, and an administrator runs the rest
     from an elevated prompt - so _tWinMain forbids the logger's writes
     (ForbidServiceFolderWrites, whose first statement is
     Logger::ForbidWritesForThisProcess()) for every command line but the
     server's own, and arms the crash oracle, which writes to the log folder,
     only for those: IsServerCommandLine names RunAsService, /Debug and /Test
     and nothing else, the decision comes before the crash oracle and before
     any verb is dispatched, and CrashOracle::Install is in its else branch.
     The verbs it dispatches are listed when the check passes.
  5. The two verbs whose purpose is to write in the data folder, which the
     service may write, write there only through what holds it: on Windows
     NewInstallDefaults::Apply_ makes the certificate and its key through
     HeldFolder (never ProtectedFile::Write, FileUtilities::WriteToFile or
     FileUtilities::CreateDirectory by path), and the store walks that change
     the data folder are dispatched only after DataFolderHoldsNoLink, and every
     object-store copy too - with no condition before the check, since a copy
     to the store that changes nothing still reads with an administrator's
     rights and hands what it read to a store the service's account reads.

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
DEFAULTS = os.path.join(SERVER, "Common", "Application", "NewInstallDefaults.cpp")
SERVER_COMMAND_LINES = ["RunAsService", "/Debug", "/Test"]
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


def statements_of(body):
    return [s.strip() for s in body.split(";") if s.strip()]


def check_administrator_verbs(main_text):
    """Rule 4. Returns (problems, verbs dispatched)."""
    problems = []
    code = code_only(main_text)
    literals = re.compile(r'_T\("([^"]*)"\)')
    raw_literals = {}

    # The literals blanked by code_only, read back from the original text at the
    # same offsets.
    def literal_values(start, end):
        return [m.group(1) for m in literals.finditer(main_text[start:end])]

    allow_start = code.find("static bool IsServerCommandLine()")
    if allow_start < 0:
        return ["hMailServer.cpp: IsServerCommandLine not found"], []
    allow_body = function_body(code, "static bool IsServerCommandLine()")
    allow_end = code.find(allow_body, allow_start) + len(allow_body)
    named = [value for value in literal_values(allow_start, allow_end) if value.strip() not in ("", " ")]
    if sorted(named) != sorted(SERVER_COMMAND_LINES):
        problems.append("hMailServer.cpp: IsServerCommandLine names %s, not exactly %s" % (named, SERVER_COMMAND_LINES))

    forbid = function_body(code, "static void ForbidServiceFolderWrites()")
    if forbid is None:
        problems.append("hMailServer.cpp: ForbidServiceFolderWrites not found")
    elif not statements_of(forbid) or statements_of(forbid)[0] != "HM::Logger::ForbidWritesForThisProcess()":
        problems.append("hMailServer.cpp: ForbidServiceFolderWrites does not begin with HM::Logger::ForbidWritesForThisProcess()")

    win_start = code.find("_tWinMain(")
    body = function_body(code, "_tWinMain(")
    if body is None:
        return problems + ["hMailServer.cpp: _tWinMain not found"], []
    body_start = code.find(body, win_start)

    decision = re.search(r"if\s*\(\s*!\s*IsServerCommandLine\(\)\s*\)\s*ForbidServiceFolderWrites\(\)\s*;\s*else\s*\{", body)
    if decision is None:
        problems.append("hMailServer.cpp: _tWinMain does not decide if (!IsServerCommandLine()) ForbidServiceFolderWrites(); else { ... }")
        return problems, []

    oracle = [m.start() for m in re.finditer(r"CrashOracle::Install\(", body)]
    else_block = function_body(body[decision.end() - 1:], "{")
    else_start = decision.end()
    else_end = else_start + (len(else_block) if else_block is not None else 0)
    if len(oracle) != 1 or not (else_start <= oracle[0] < else_end):
        problems.append("hMailServer.cpp: CrashOracle::Install is not called once, in the else branch of the IsServerCommandLine decision")

    # Every verb dispatched after the update verbs: a comparison of a token with a
    # literal, or FindVerbWithArgument.
    verbs = []
    for match in re.finditer(r"CompareNoCase\(", body):
        if match.start() < decision.start():
            continue
        values = literal_values(body_start + match.start(), body_start + match.start() + 200)
        if values and values[0].startswith(("/", "-")) or (values and values[0] == "RunAsService"):
            verbs.append(values[0])
    for match in re.finditer(r'FindVerbWithArgument\(L"([^"]*)"', main_text[body_start:body_start + len(body)]):
        verbs.append(match.group(1))
    for match in re.finditer(r"FindVerbWithArgument\(", body):
        if match.start() < decision.start():
            problems.append("hMailServer.cpp: a verb is dispatched before the IsServerCommandLine decision")
    first_dispatch = min([m.start() for m in re.finditer(r"sLastParam\.CompareNoCase\(", body)] or [len(body)])
    if first_dispatch < decision.start():
        problems.append("hMailServer.cpp: a verb is dispatched before the IsServerCommandLine decision")

    switches = ("--config", "--config=", "--account", "--domain", "--confirm", "--once", "--if-new", "--decompress", "--remove-source", "--service-account")
    verbs = sorted(set(v for v in verbs if v not in switches), key=str.lower)
    return problems, verbs


def check_data_folder_writers(main_text, defaults_text):
    """Rule 5."""
    problems = []
    code = code_only(defaults_text)
    apply_body = function_body(code, "NewInstallDefaults::Apply_()")
    if apply_body is None:
        return ["NewInstallDefaults.cpp: Apply_ not found"]
    windows = re.search(r"#ifndef HM_PLATFORM_POSIX(.*?)#else", apply_body, re.S)
    if windows is None or "HeldFolder" not in windows.group(1) or "WriteNewFile(" not in windows.group(1):
        problems.append("NewInstallDefaults.cpp: Apply_ does not write the certificate through HeldFolder on Windows")
    elif re.search(r"ProtectedFile::Write\(|WriteToFile\(|FileUtilities::CreateDirectory\(|FileUtilities::DeleteFile\(", windows.group(1)):
        problems.append("NewInstallDefaults.cpp: Apply_ writes in the data folder by path on Windows")

    main_code = code_only(main_text)
    for call in ("HM::MessageStoreMaintenance::RunCommand(", "HM::MessageStoreObjects::RunCopyCommand("):
        at = main_code.find(call)
        guard = main_code.rfind("DataFolderHoldsNoLink(", 0, at) if at >= 0 else -1
        if at < 0:
            problems.append("hMailServer.cpp: %s is not dispatched" % call)
        elif guard < 0 or at - guard > 400:
            problems.append("hMailServer.cpp: %s is not dispatched right after DataFolderHoldsNoLink" % call)
        elif call.startswith("HM::MessageStoreObjects"):
            line = main_code[main_code.rfind("\n", 0, guard) + 1:main_code.find("\n", guard)]
            if not re.search(r"if\s*\(\s*!\s*DataFolderHoldsNoLink\(", line):
                problems.append("hMailServer.cpp: not every object-store copy is dispatched after DataFolderHoldsNoLink: " + line.strip())
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


def run(texts, main_text, project_text, updater_text, defaults_text=None):
    problems = check_files(texts)
    problems += check_dispatch(main_text, texts[VERB_FILES[0]])
    problems += check_projects(project_text, updater_text)
    administrator, _ = check_administrator_verbs(main_text)
    problems += administrator
    if defaults_text is not None:
        problems += check_data_folder_writers(main_text, defaults_text)
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

    # Rule 4: a verb let off, the forbidding gone or moved, the oracle armed for all.
    allow = '          last.CompareNoCase(_T("/Test")) == 0;'
    if allow in main_text:
        expect("a verb added to the server's command lines",
               run(texts, main_text.replace(allow, '          last.CompareNoCase(_T("/Test")) == 0 ||\n          last.CompareNoCase(_T("--apply-profile")) == 0;', 1), project_text, updater_text),
               "IsServerCommandLine names")
    else:
        failures.append("IsServerCommandLine is not where the mutation looks for it")
    first = "   HM::Logger::ForbidWritesForThisProcess();\n"
    if first in main_text:
        expect("ForbidServiceFolderWrites without the logger's forbidding",
               run(texts, main_text.replace(first, "", 1), project_text, updater_text), "does not begin with")
    else:
        failures.append("ForbidServiceFolderWrites is not where the mutation looks for it")
    decision = "   if (!IsServerCommandLine())\n      ForbidServiceFolderWrites();\n   else\n   {\n"
    if decision in main_text:
        expect("the decision taken away",
               run(texts, main_text.replace(decision, "   {\n", 1), project_text, updater_text), "does not decide")
        moved = main_text.replace(decision, "   HM::CrashOracle::Install();\n" + decision, 1)
        expect("the crash oracle armed for every command line",
               run(texts, moved, project_text, updater_text), "else branch")
    else:
        failures.append("the IsServerCommandLine decision is not where the mutation looks for it")

    # Rule 5: the certificate by path, a store walk without the check.
    defaults_text = read(DEFAULTS)
    held = "         written = held.WriteNewFile(KeyFileName, keyPem, true, ServiceLogOnAccount(), problem);\n"
    if held in defaults_text:
        expect("the key written by path",
               run(texts, main_text, project_text, updater_text,
                   defaults_text.replace(held, held + "         written = ProtectedFile::Write(keyFile, keyPem, problem);\n", 1)),
               "by path")
    else:
        failures.append("Apply_'s key write is not where the mutation looks for it")
    guard = "if (storeVerb != HM::MessageStoreMaintenance::VerbMeasure && !DataFolderHoldsNoLink(heldData, linkWhy))"
    if guard in main_text:
        expect("a store walk without the link check",
               run(texts, main_text.replace(guard, "if (false)", 1), project_text, updater_text, defaults_text),
               "MessageStoreMaintenance::RunCommand")
    else:
        failures.append("the store walk's check is not where the mutation looks for it")
    copy_guard = "if (!DataFolderHoldsNoLink(heldData, linkWhy))"
    if copy_guard in main_text:
        expect("a copy to the object store let past the link check",
               run(texts, main_text.replace(copy_guard, "if ((copyFromObjects || removeSource) && !DataFolderHoldsNoLink(heldData, linkWhy))", 1),
                   project_text, updater_text, defaults_text),
               "not every object-store copy")
    else:
        failures.append("the copies' check is not where the mutation looks for it")
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
        if not failures and run(texts, main_text, project_text, updater_text, read(DEFAULTS)):
            failures.append("the check fails on the tree itself, so its mutations prove nothing")
        for failure in failures:
            print("  FAIL  self-test: %s" % failure)
        if failures:
            return 1
        print("  OK    self-test: every mutation of the elevated verbs' rules was reported")
        return 0

    defaults_text = read(DEFAULTS)
    problems = run(texts, main_text, project_text, updater_text, defaults_text)
    for problem in problems:
        print("  FAIL  " + problem)
    if problems:
        return 1
    _, verbs = check_administrator_verbs(main_text)
    print("  OK    the elevated verbs log nothing, are dispatched before the crash oracle, forbid the logger's writes and "
          "limit DLL loading first, and the server links with /DEPENDENTLOADFLAG:0xA00")
    print("  OK    every other command line but %s forbids the logger's writes and has no crash oracle: %s"
          % (", ".join(SERVER_COMMAND_LINES), ", ".join(v for v in verbs if v not in SERVER_COMMAND_LINES)))
    print("  OK    the certificate of a new installation is written through HeldFolder, and the store walks that change "
          "the data folder and every object-store copy are dispatched after DataFolderHoldsNoLink")
    return 0


if __name__ == "__main__":
    sys.exit(main())
