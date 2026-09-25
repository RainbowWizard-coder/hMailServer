# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Windows installer removes what it installed, once, from a server that has
gone.

The installer smoke test (docs/ContinuousIntegration.md) installed 6.3.2, upgraded
it to 6.3.3 and uninstalled it on 25 September 2026, and found four defects in the
product that nothing short of a clean machine had shown. The smoke test needs a
throwaway VM and a built installer; this holds the shape of each fix in the text
of the Inno scripts and the server, so that undoing one fails here in a second:

  run-once     Every [UninstallRun] entry carries a RunOnceId, and no two share
               one. Inno records the entries at every installation, so without an
               id an uninstall after an upgrade ran each of them twice.

  wait         The uninstaller stops the service and waits for its PROCESS before
               anything is deleted: CurUninstallStepChanged, at usUninstall, calls
               StopHMailServerAndWaitForExit, which opens the process before it
               asks for the stop and waits on it after (WaitForSingleObject). The
               upgrade's stop at the Ready page, StopHMailServerService, goes
               through the same function, and so does every other stop of the
               service in the [Code]: a StopService call anywhere else is refused.
               net stop returns when the service says STOPPED; the process lives
               about 1.1 seconds longer, and the uninstall left hMailServer.exe
               and six DLLs in Bin.

  reboot       Every [Files] entry that installs a file the server's process
               loads - hMailServer.exe, the C++ runtime, OpenSSL, libpq, the MySQL
               client and its plugins - is marked uninsrestartdelete, so that a
               file still in use when the bounded wait ends is queued for deletion
               at the next restart instead of being left without a word. Each of
               those entries must be found, so a renamed one is not passed over.

  unregister   hMailServer.exe decides /Unregister before it registers anything:
               the branch comes before the first _AtlModule.RegisterAppID() call,
               removes the AppID whatever UnregisterServer answered, and removes
               the event log source (WindowsEventLog::UnregisterSource) last. A
               second /Unregister used to write the AppID and leave it.

  event source The key WindowsEventLog creates on its first event is the key
               UnregisterSource removes: both name EVENT_SOURCE_KEY.

  upgrade      The installer starts the service for the database step of an
               upgrade with the argument ServiceMain looks for, and ServiceMain
               marks the start with it (SetStartedForDatabaseUpgrade), so that the
               too-old refusal DBUpdater expects is not reported as HM5011.

The check proves its own negative controls on every run, on altered copies of the
texts it reads: an entry without a RunOnceId, two entries sharing one, an
uninstaller that no longer waits, a stop elsewhere that does not wait,
hMailServer.exe without uninsrestartdelete, the AppID registered before
/Unregister is read, the AppID removal made conditional again, and the installer's
argument spelt differently from the server's, must all be refused.

Usage: python build/check-installer-scripts.py
Exits 1 listing what is wrong.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INSTALLATION = os.path.join(ROOT, 'hmailserver', 'installation')
SERVER = os.path.join(ROOT, 'hmailserver', 'source', 'Server')

EXTENSION = 'hMailServerInnoExtension.iss'
UNINSTALL_RUN = 'section_uninstallrun.iss'
# The ARM64 installer's files (hMailServerARM64.iss) are held to the same rule.
FILES = ('section_files_64.iss', 'section_files_common.iss', 'section_files_arm64.iss')
MAIN = 'hMailServer/hMailServer.cpp'
EVENT_LOG = 'Common/Application/WindowsEventLog.cpp'

# The files the server's process loads, as their [Files] Source spells them. Each
# must be found at least once, and every entry it matches must carry
# uninsrestartdelete.
LOADED_BY_THE_SERVER = [
    ('hMailServer.exe', re.compile(r'\\hMailServer\.exe$', re.I)),
    ('the C++ runtime', re.compile(r'^(?:ARM64\\)?Microsoft\.VC\d+\.CRT\\\*$', re.I)),
    ('libcrypto', re.compile(r'\\libcrypto-[\w-]+\.dll$', re.I)),
    ('libssl', re.compile(r'\\libssl-[\w-]+\.dll$', re.I)),
    ('libpq', re.compile(r'POSTGRESQL_LIBPQ_PATH\}\\\*\.dll$', re.I)),
    ('the MySQL client', re.compile(r'\\libmysql\.dll$', re.I)),
    ('the MySQL client plugins', re.compile(r'\\plugin\\\*\.dll$', re.I)),
]


def read(path):
    return io.open(path, encoding='utf-8-sig', errors='replace').read().replace('\r\n', '\n')


def load():
    texts = {}
    for name in os.listdir(INSTALLATION):
        if name.lower().endswith('.iss'):
            texts[name] = read(os.path.join(INSTALLATION, name))
    texts[MAIN] = read(os.path.join(SERVER, MAIN))
    texts[EVENT_LOG] = read(os.path.join(SERVER, EVENT_LOG))
    return texts


def section_entries(text, section):
    """The entry lines of every [section] in an Inno script, comment lines dropped."""
    entries = []
    inside = False
    for number, raw in enumerate(text.split('\n'), 1):
        line = raw.strip()
        header = re.match(r'^\[([^\]]+)\]', line)
        if header:
            inside = header.group(1).strip().lower() == section.lower()
            continue
        if inside and line and not line.startswith(';') and not line.startswith('#'):
            entries.append((number, line))
    return entries


def parameter(entry, name):
    """One parameter of an Inno entry line: 'Name: "value"' or 'Name: value'."""
    match = re.search(r'(?:^|;)\s*' + re.escape(name) + r'\s*:\s*("([^"]*)"|[^;]*)', entry, re.I)
    if not match:
        return None
    return match.group(2) if match.group(2) is not None else match.group(1).strip()


def pascal_routine(text, name):
    """The text of a [Code] function or procedure, from its header to the 'end;'
    at the start of a line that closes it. None when it is not there."""
    header = re.search(r'^(?:function|procedure)\s+' + re.escape(name) + r'\s*[(:;]', text, re.I | re.M)
    if not header:
        return None
    end = re.search(r'^end;', text[header.end():], re.M)
    if not end:
        return None
    return text[header.start():header.end() + end.end()]


def braced_block(text, start):
    """The text from the first '{' at or after start to the brace that closes it."""
    opening = text.find('{', start)
    if opening < 0:
        return None
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == '{':
            depth += 1
        elif text[index] == '}':
            depth -= 1
            if depth == 0:
                return text[opening:index + 1]
    return None


def check_run_once(texts, problems):
    seen = {}
    count = 0
    for name in sorted(texts):
        if not name.lower().endswith('.iss'):
            continue
        for number, entry in section_entries(texts[name], 'UninstallRun'):
            count += 1
            identifier = parameter(entry, 'RunOnceId')
            if not identifier:
                problems.append('%s:%d: an [UninstallRun] entry without a RunOnceId runs once for every installation '
                                'that recorded it - twice after an upgrade: %s' % (name, number, entry))
                continue
            if identifier in seen:
                problems.append('%s:%d: RunOnceId "%s" is also %s; an entry sharing an id with another is skipped '
                                'once the other has run' % (name, number, identifier, seen[identifier]))
            seen[identifier] = '%s:%d' % (name, number)
    if count == 0:
        problems.append('%s: no [UninstallRun] entries were found; the uninstaller no longer stops or unregisters '
                        'hMailServer, or this check no longer reads them' % UNINSTALL_RUN)
    return count


def check_wait(texts, problems):
    extension = texts.get(EXTENSION, '')

    step = pascal_routine(extension, 'CurUninstallStepChanged')
    if step is None:
        problems.append('%s: there is no CurUninstallStepChanged, so nothing waits for the server\'s process before '
                        'the uninstaller deletes its files; net stop alone left hMailServer.exe and six DLLs in Bin' % EXTENSION)
    elif not re.search(r'\busUninstall\b', step) or 'StopHMailServerAndWaitForExit(' not in step:
        problems.append('%s: CurUninstallStepChanged does not call StopHMailServerAndWaitForExit at usUninstall, '
                        'the step before [UninstallRun] and before any file is deleted' % EXTENSION)

    wait = pascal_routine(extension, 'StopHMailServerAndWaitForExit')
    if wait is None:
        problems.append('%s: StopHMailServerAndWaitForExit is gone' % EXTENSION)
    else:
        opened = wait.find('OpenProcessForWait(')
        stopped = wait.find('StopService(')
        if opened < 0 or 'WaitForProcessExit(' not in wait:
            problems.append('%s: StopHMailServerAndWaitForExit no longer opens the service\'s process and waits on it; '
                            'the service reports STOPPED about 1.1 seconds before its process has gone' % EXTENSION)
        elif stopped < 0 or opened > stopped:
            problems.append('%s: StopHMailServerAndWaitForExit asks for the stop before it opens the process; once the '
                            'service has stopped its process id is no longer named, and an id looked up then could '
                            'be another process\'s' % EXTENSION)

    upgrade = pascal_routine(extension, 'StopHMailServerService')
    if upgrade is None or 'StopHMailServerAndWaitForExit(' not in upgrade:
        problems.append('%s: StopHMailServerService, the stop before an upgrade replaces the program files, does not '
                        'wait for the process through StopHMailServerAndWaitForExit' % EXTENSION)

    # Every stop of the service goes through the one function that waits for the
    # process: a stop anywhere else is the same race again.
    inside = (extension.find(wait), extension.find(wait) + len(wait)) if wait else (-1, -1)
    for match in re.finditer(r"\bStopService\s*\(\s*'hMailServer'", extension):
        if not inside[0] <= match.start() < inside[1]:
            number = extension.count('\n', 0, match.start()) + 1
            problems.append('%s:%d: the service is stopped here without StopHMailServerAndWaitForExit, so whatever '
                            'follows meets a process that is still exiting' % (EXTENSION, number))

    for script_name, windows_name in (('OpenProcessForWait', 'OpenProcess@kernel32.dll'),
                                      ('WaitForProcessExit', 'WaitForSingleObject@kernel32.dll')):
        declared = re.search(r'^function\s+' + script_name + r"\b[^\n]*\nexternal\s+'([^']*)'", extension, re.M)
        if not declared or not declared.group(1).startswith(windows_name + ' '):
            problems.append('%s: %s is no longer declared as the external %s' % (EXTENSION, script_name, windows_name))


def check_reboot(texts, problems):
    found = dict((label, 0) for label, _ in LOADED_BY_THE_SERVER)
    for name in FILES:
        for number, entry in section_entries(texts.get(name, ''), 'Files'):
            source = parameter(entry, 'Source') or ''
            flags = (parameter(entry, 'Flags') or '').lower().split()
            for label, pattern in LOADED_BY_THE_SERVER:
                if pattern.search(source):
                    found[label] += 1
                    if 'uninsrestartdelete' not in flags:
                        problems.append('%s:%d: the server\'s process loads %s, and this entry has no '
                                        'uninsrestartdelete, so a file still in use at uninstall is left in Bin '
                                        'without a word: %s' % (name, number, label, entry))
    for label, count in sorted(found.items()):
        if count == 0:
            problems.append('%s: no [Files] entry installs %s; if it was renamed, update LOADED_BY_THE_SERVER in '
                            'this check' % (' and '.join(FILES), label))


def check_unregister(texts, problems):
    main = texts.get(MAIN, '')
    branch = re.search(r'if\s*\(\s*sLastParam\.CompareNoCase\(\s*_T\("/Unregister"\)\s*\)\s*==\s*0\s*\)', main)
    register = main.find('_AtlModule.RegisterAppID()')
    if not branch:
        problems.append('%s: the /Unregister branch of _tWinMain is not where this check looks for it' % MAIN)
        return
    if register < 0:
        problems.append('%s: _tWinMain no longer calls _AtlModule.RegisterAppID(); update this check' % MAIN)
    elif register < branch.start():
        problems.append('%s: _AtlModule.RegisterAppID() is called before the /Unregister branch, so /Unregister writes '
                        'HKCR\\AppID\\{5EDEC473-...} before it removes it, and leaves it when nothing else was '
                        'registered' % MAIN)

    block = braced_block(main, branch.end()) or ''
    if 'UnregisterAppId()' not in block:
        problems.append('%s: /Unregister no longer removes the AppID (UnregisterAppId)' % MAIN)
    elif re.search(r'if\s*\([^)]*SUCCEEDED', block):
        problems.append('%s: /Unregister removes the AppID only when UnregisterServer succeeded; on a second '
                        '/Unregister, with nothing registered, that leaves it' % MAIN)
    source = block.find('WindowsEventLog::UnregisterSource()')
    if source < 0:
        problems.append('%s: /Unregister no longer removes the Application log\'s event source '
                        '(WindowsEventLog::UnregisterSource)' % MAIN)
    elif source < max(block.find('UnregisterAppId()'), block.find('UnregisterServer('), block.find('UnregisterService(')):
        problems.append('%s: /Unregister removes the event source before the rest; it goes last, so that nothing on '
                        'that path can write an event after it and register the source again' % MAIN)


def check_event_source(texts, problems):
    log = texts.get(EVENT_LOG, '')
    create = re.search(r'RegCreateKeyEx\(\s*HKEY_LOCAL_MACHINE\s*,\s*(\w+)', log)
    delete = re.search(r'RegDeleteKey\(\s*HKEY_LOCAL_MACHINE\s*,\s*(\w+)', log)
    if not create or not delete:
        problems.append('%s: the event source is no longer created with RegCreateKeyEx(HKEY_LOCAL_MACHINE, ...) and '
                        'removed with RegDeleteKey(HKEY_LOCAL_MACHINE, ...) by name' % EVENT_LOG)
    elif create.group(1) != delete.group(1):
        problems.append('%s: the key the first event creates (%s) is not the one UnregisterSource removes (%s)'
                        % (EVENT_LOG, create.group(1), delete.group(1)))


def check_upgrade(texts, problems):
    extension = texts.get(EXTENSION, '')
    main = texts.get(MAIN, '')
    started = re.search(r"'start hMailServer (--[\w-]+)'", extension)
    if not started:
        problems.append('%s: the installer no longer starts the service for the database step of an upgrade with an '
                        'argument (StartServiceForDatabaseUpgrade); DBSetupQuick\'s COM activation then starts it, and '
                        'every silent upgrade across a schema step reports HM5011 as a Critical event' % EXTENSION)
        return
    routine = pascal_routine(extension, 'RunPostInstallTasks') or ''
    if 'StartServiceForDatabaseUpgrade(' not in routine:
        problems.append('%s: RunPostInstallTasks no longer calls StartServiceForDatabaseUpgrade before the database '
                        'step' % EXTENSION)
    elif routine.find('StartServiceForDatabaseUpgrade(') > routine.find('DBSetupQuick.exe'):
        problems.append('%s: RunPostInstallTasks starts the service for the upgrade after DBSetupQuick has already '
                        'started it' % EXTENSION)

    service_main = re.search(r'^ServiceMain\s*\(', main, re.M)
    body = braced_block(main, service_main.end()) if service_main else None
    if not body:
        problems.append('%s: ServiceMain is not where this check looks for it' % MAIN)
        return
    if '_T("%s")' % started.group(1) not in body or 'SetStartedForDatabaseUpgrade()' not in body:
        problems.append('%s: ServiceMain does not mark the start with SetStartedForDatabaseUpgrade when it is given '
                        '%s, the argument the installer starts it with' % (MAIN, started.group(1)))


def check(texts):
    problems = []
    entries = check_run_once(texts, problems)
    check_wait(texts, problems)
    check_reboot(texts, problems)
    check_unregister(texts, problems)
    check_event_source(texts, problems)
    check_upgrade(texts, problems)
    return problems, entries


def altered(texts, name, old, new):
    copy = dict(texts)
    if old not in copy.get(name, ''):
        return None
    copy[name] = copy[name].replace(old, new, 1)
    return copy


def negative_controls(texts):
    """Each alteration breaks one rule; each must be refused. Answers what was not."""
    first_entry = section_entries(texts.get(UNINSTALL_RUN, ''), 'UninstallRun')
    first_line = first_entry[0][1] if first_entry else ''
    first_id = parameter(first_line, 'RunOnceId') or ''
    second_line = first_entry[1][1] if len(first_entry) > 1 else ''
    second_id = parameter(second_line, 'RunOnceId') or ''

    controls = [
        ('an [UninstallRun] entry without a RunOnceId',
         altered(texts, UNINSTALL_RUN, '; RunOnceId: "%s"' % first_id, '')),
        ('two [UninstallRun] entries sharing a RunOnceId',
         altered(texts, UNINSTALL_RUN, 'RunOnceId: "%s"' % second_id, 'RunOnceId: "%s"' % first_id)),
        ('an uninstaller that no longer waits for the process',
         altered(texts, EXTENSION, 'if (StopHMailServerAndWaitForExit(budgetMs) = false) then\n      begin\n         Log(',
                 'if (IsServiceStopped(\'hMailServer\') = false) then\n      begin\n         Log(')),
        ('a stop that does not wait for the process',
         altered(texts, EXTENSION, '      if (StopHMailServerAndWaitForExit(90000) = false) then\n',
                 '      StopService(\'hMailServer\');\n      if (IsServiceStopped(\'hMailServer\') = false) then\n')),
        ('hMailServer.exe installed without uninsrestartdelete',
         altered(texts, 'section_files_64.iss', 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete;',
                 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion;')),
        ('the AppID registered before /Unregister is read',
         altered(texts, MAIN, '   _AtlModule.InitializeCom();\n',
                 '   _AtlModule.InitializeCom();\n   _AtlModule.RegisterAppID();\n')),
        ('the AppID removed only when UnregisterServer succeeded',
         altered(texts, MAIN, '      _AtlModule.UnregisterServer(TRUE);\n      _AtlModule.UnregisterAppId();\n',
                 '      if (SUCCEEDED(_AtlModule.UnregisterServer(TRUE)))\n         _AtlModule.UnregisterAppId();\n')),
        ('the installer\'s argument spelt differently from the server\'s',
         altered(texts, EXTENSION, "'start hMailServer --for-database-upgrade'", "'start hMailServer --database-upgrade'")),
    ]

    missed = []
    for description, copy in controls:
        if copy is None:
            missed.append('%s: the text this control alters is not there; update the control' % description)
            continue
        problems, _ = check(copy)
        if not problems:
            missed.append('%s: was not refused' % description)
    return missed, len(controls)


def main():
    texts = load()
    problems, entries = check(texts)
    missed, controls = negative_controls(texts)

    if problems:
        print('\n'.join(problems))
    if missed:
        print('\n'.join('negative control: ' + line for line in missed))
    if problems or missed:
        print('FAIL: %d problem(s) in the installer scripts and the server they install, %d negative control(s) not '
              'refused' % (len(problems), len(missed)))
        return 1

    print('OK    %d [UninstallRun] entries run once each; the uninstall and the upgrade wait for the server\'s process; '
          'the files it loads are queued for a restart if still in use; /Unregister creates nothing and removes the '
          'event source; the upgrade\'s start is marked; %d negative controls refused' % (entries, controls))
    return 0


if __name__ == '__main__':
    sys.exit(main())
