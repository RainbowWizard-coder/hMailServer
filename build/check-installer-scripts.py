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

  plugins      The MySQL client's plugins are installed by name, one entry each,
               and the names are the manifest's: every
               hmailserver/installation/Extras/plugin file that
               hmailserver/docs/third-party-binaries.json lists is installed, and
               nothing else is. get-installer-binaries.ps1 places files and removes
               none, so a checkout that once fetched the 3.4.9 client keeps two
               files 6.3.7 dropped in that folder, and the wildcard this
               replaced (every .dll in the folder) would have shipped them again.

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

  credential   The administrator password - or the live update's token - reaches
               DBSetupQuick, and DBSetup when it runs instead, in
               HM_ADMIN_PASSWORD, set to it last before each starts, emptied again
               in a finally once it has run, and never on its command line;
               DBSetupQuick hands it on to DBUpdater in the same variable and
               passes its arguments one by one in ArgumentList. Until 26 September
               2026 they appended " password:" and the password unquoted, so a
               password with a space, a double quote or backslashes before one
               reached the database tools changed and the install failed with exit
               code 3.
  original     An installation of the original hMailServer's 5.7, whose database
               this fork cannot upgrade, is refused first in InitializeSetup, before
               anything is installed, stopped or changed: OriginalHMailServer.iss,
               included in the [Code], decides, and InitializeSetup ends setup on
               its refusal. Its two bounds, 5708 (the last schema the two projects
               share) and 6001 (this fork's first after it), are the upgrade
               chain's: a step from the one to the other is in DBScripts, and no
               step from or to a version between them. The database's version is
               read only after hMailServer.exe is found to be before 6, not this
               fork's, and only from a running service. The decision's self-test,
               build/ci/packages/original-guard-test.iss, includes the same file,
               and windows-packages.ps1 runs it before it compiles the installer,
               stopping with exit 8 when it fails. That self-test runs the cases;
               this holds that they are run.

The check proves its own negative controls on every run, on altered copies of the
texts it reads: an entry without a RunOnceId, two entries sharing one, an
uninstaller that no longer waits, a stop elsewhere that does not wait,
hMailServer.exe without uninsrestartdelete, a client plugin without it, the
plugins taken by a wildcard again, a plugin the manifest lists and the installer
does not name, a plugin named that the manifest does not list, the AppID
registered before /Unregister is read, the AppID removal made conditional again,
the installer's argument spelt differently from the server's, the password on
DBSetupQuick's command line again, HM_ADMIN_PASSWORD left set after DBSetupQuick,
the password on DBSetup's command line, DBSetup started without the credential,
HM_ADMIN_PASSWORD left set after DBSetup, DBUpdater's command line built as one
string and the password on it, the original's shared bound moved past 5708, a step
of this fork's own to a version between the bounds, the decision not included,
its refusal not acted on, the refusal asked after a window check, the database
read from a stopped service, and the self-test not run before the installer is
built, must all be refused.

Usage: python build/check-installer-scripts.py
Exits 1 listing what is wrong.
"""
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INSTALLATION = os.path.join(ROOT, 'hmailserver', 'installation')
SERVER = os.path.join(ROOT, 'hmailserver', 'source', 'Server')
TOOLS = os.path.join(ROOT, 'hmailserver', 'source', 'Tools')

EXTENSION = 'hMailServerInnoExtension.iss'
UNINSTALL_RUN = 'section_uninstallrun.iss'
# The ARM64 installer's files (hMailServerARM64.iss) are held to the same rule.
FILES = ('section_files_64.iss', 'section_files_common.iss', 'section_files_arm64.iss')
MAIN = 'hMailServer/hMailServer.cpp'
EVENT_LOG = 'Common/Application/WindowsEventLog.cpp'
# Under Tools: the database step the installer runs, and the password it hands on.
DBSETUPQUICK = 'DBSetupQuick/Program.cs'
AUTHENTICATOR = 'Shared/Miscellaneous/Authenticator.cs'
# The binary manifest, whose Extras/plugin entries are the plugins the installer names.
MANIFEST = 'hmailserver/docs/third-party-binaries.json'
MANIFEST_PLUGINS = 'hmailserver/installation/Extras/plugin/'
# The installer script that installs the MySQL client and its plugins.
CLIENT_FILES = 'section_files_common.iss'

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
    ('the MySQL client plugins', re.compile(r'\\plugin\\[^\\]+\.dll$', re.I)),
]
# A [Files] Source in the client's plugin folder, and the file it names.
PLUGIN_SOURCE = re.compile(r'(?:^|\\)Extras\\plugin\\([^\\]+)$', re.I)
# The decision on an installation of the original hMailServer's 5.7, its self-test,
# the job that runs it, and the database scripts whose chain it is held to.
ORIGINAL = 'OriginalHMailServer.iss'
GUARD_RUNNER = 'build/ci/packages/original-guard-test.ps1'
GUARD_HARNESS = 'build/ci/packages/original-guard-test.iss'
PACKAGES = 'build/ci/packages/windows-packages.ps1'
DBSCRIPTS = 'hmailserver/source/DBScripts'
UPGRADE_SCRIPT = re.compile(r'^Upgrade(\d+)to(\d+)', re.I)


def read(path):
    return io.open(path, encoding='utf-8-sig', errors='replace').read().replace('\r\n', '\n')


def load():
    texts = {}
    for name in os.listdir(INSTALLATION):
        if name.lower().endswith('.iss'):
            texts[name] = read(os.path.join(INSTALLATION, name))
    texts[MAIN] = read(os.path.join(SERVER, MAIN))
    texts[EVENT_LOG] = read(os.path.join(SERVER, EVENT_LOG))
    texts[DBSETUPQUICK] = read(os.path.join(TOOLS, DBSETUPQUICK))
    texts[AUTHENTICATOR] = read(os.path.join(TOOLS, AUTHENTICATOR))
    texts[MANIFEST] = read(os.path.join(ROOT, *MANIFEST.split('/')))
    for name in (GUARD_RUNNER, GUARD_HARNESS, PACKAGES):
        path = os.path.join(ROOT, *name.split('/'))
        texts[name] = read(path) if os.path.exists(path) else ''
    # The names of the database scripts, one a line: the upgrade chain's steps.
    texts[DBSCRIPTS] = '\n'.join(sorted(os.listdir(os.path.join(ROOT, *DBSCRIPTS.split('/')))))
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


def check_plugins(texts, problems):
    """The client's plugins are installed by name, and the names are the manifest's."""
    try:
        manifest = json.loads(texts.get(MANIFEST, ''))
    except ValueError as error:
        problems.append('%s: not JSON (%s), so the plugins the installer names cannot be held to it' % (MANIFEST, error))
        return
    listed = set()
    for artifact in manifest.get('artifacts', []):
        path = str(artifact.get('path', ''))
        if path.startswith(MANIFEST_PLUGINS) and not artifact.get('platform'):
            listed.add(path[len(MANIFEST_PLUGINS):].lower())
    if not listed:
        problems.append('%s: no entry under %s; the client\'s plugins are no longer where this check looks for them'
                        % (MANIFEST, MANIFEST_PLUGINS))

    named = {}
    for name in FILES:
        for number, entry in section_entries(texts.get(name, ''), 'Files'):
            match = PLUGIN_SOURCE.search(parameter(entry, 'Source') or '')
            if not match:
                continue
            plugin = match.group(1)
            if '*' in plugin or '?' in plugin:
                problems.append('%s:%d: the client\'s plugins are taken by a wildcard, which ships whatever '
                                'Extras\\plugin holds - get-installer-binaries.ps1 removes nothing, so a checkout that '
                                'once fetched an earlier client ships that client\'s other files too; name each '
                                'plugin the manifest lists: %s' % (name, number, entry))
                continue
            if name != CLIENT_FILES:
                problems.append('%s:%d: a client plugin installed outside %s, which installs the client: %s'
                                % (name, number, CLIENT_FILES, entry))
            named[plugin.lower()] = '%s:%d' % (name, number)

    for plugin in sorted(listed - set(named)):
        problems.append('%s: %s lists %s%s, which get-installer-binaries.ps1 places and no [Files] entry installs'
                        % (CLIENT_FILES, MANIFEST, MANIFEST_PLUGINS, plugin))
    for plugin in sorted(set(named) - listed):
        problems.append('%s: installs Extras\\plugin\\%s, which %s does not list, so get-installer-binaries.ps1 does '
                        'not place it: the installer would ship a file left from an earlier client, or fail to build'
                        % (named[plugin], plugin, MANIFEST))


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


def check_credential(texts, problems):
    routine = pascal_routine(texts.get(EXTENSION, ''), 'RunPostInstallTasks') or ''
    for line in re.findall(r'^\s*szParameters\s*:=.*$', routine, re.M):
        if re.search(r'g_szAdminPassword|g_szUpgradeToken|password:', line, re.I):
            problems.append('%s: RunPostInstallTasks puts a credential on the database tools\' command line (%s); it '
                            'goes in HM_ADMIN_PASSWORD' % (EXTENSION, line.strip()))
    if not re.search(r"^\s*szCredential\s*:=\s*g_szAdminPassword\s*$", routine, re.M) or \
            not re.search(r"^\s*szCredential\s*:=\s*'token:'\s*\+\s*g_szUpgradeToken\s*;", routine, re.M):
        problems.append('%s: RunPostInstallTasks no longer makes szCredential of the administrator password or '
                        '"token:" and the update token' % EXTENSION)
    # DBSetupQuick for the built-in database or an existing configuration, DBSetup
    # for an external database chosen for the first time: both sign in with it.
    for tool in ('DBSetupQuick', 'DBSetup'):
        exec_call = re.search(r"Exec\(\s*ExpandConstant\('\{app\}\\Bin\\" + tool + r"\.exe'\)\s*,\s*([^,]*),", routine)
        if not exec_call:
            problems.append('%s: RunPostInstallTasks no longer runs {app}\\Bin\\%s.exe where this check looks for it'
                            % (EXTENSION, tool))
            continue
        if re.search(r'g_szAdminPassword|g_szUpgradeToken|szCredential|password:', exec_call.group(1), re.I):
            problems.append('%s: the administrator password or the update token is on %s\'s command line'
                            % (EXTENSION, tool))
        before = routine[:exec_call.start()]
        after = routine[exec_call.end():]
        # The last value HM_ADMIN_PASSWORD is given before the tool starts is the
        # credential: an earlier tool's set, emptied again after it, does not count.
        handed = re.findall(r"SetProcessEnvironmentVariable\(\s*'HM_ADMIN_PASSWORD'\s*,\s*([^)]*?)\s*\)", before)
        if not handed or handed[-1] != 'szCredential':
            problems.append('%s: RunPostInstallTasks no longer hands %s the credential in HM_ADMIN_PASSWORD'
                            % (EXTENSION, tool))
        if not re.search(r'\btry\b(?:(?!\bend;).)*$', before, re.S | re.I) or \
                not re.match(r"(?:(?!\bend;).)*?\bfinally\s+SetProcessEnvironmentVariable\(\s*'HM_ADMIN_PASSWORD'\s*,"
                             r"\s*''\s*\)", after, re.S | re.I):
            problems.append('%s: HM_ADMIN_PASSWORD is not emptied in a finally around %s, so every process the '
                            'installer starts afterwards inherits the password' % (EXTENSION, tool))

    program = texts.get(DBSETUPQUICK, '')
    if re.search(r'\bupgradeProcess\.Arguments\s*=', program) or 'upgradeProcess.ArgumentList.Add(' not in program:
        problems.append('%s: DBSetupQuick builds DBUpdater\'s command line as one string; each argument goes in '
                        'ArgumentList' % DBSETUPQUICK)
    if not re.search(r'upgradeProcess\.Environment\[\s*Authenticator\.PasswordVariable\s*\]\s*=', program) or \
            re.search(r'"password:"\s*\+', program):
        problems.append('%s: DBSetupQuick no longer hands the password to DBUpdater in HM_ADMIN_PASSWORD, or puts it '
                        'on DBUpdater\'s command line' % DBSETUPQUICK)
    if not re.search(r'\bPasswordVariable\s*=\s*"HM_ADMIN_PASSWORD"', texts.get(AUTHENTICATOR, '')):
        problems.append('%s: Authenticator.PasswordVariable is not HM_ADMIN_PASSWORD, the variable the installer sets'
                        % AUTHENTICATOR)


def check_original(texts, problems):
    """The original's 5.7 is refused first, before anything is installed, by a
    decision whose two numbers are the upgrade chain's, and whose self-test the
    Windows package jobs run before they build the installer."""
    decision = texts.get(ORIGINAL, '')
    extension = texts.get(EXTENSION, '')
    numbers = {}
    for name in ('ORIGINAL_LAST_SHARED_SCHEMA', 'FORK_FIRST_SCHEMA'):
        match = re.search(r'^\s*' + name + r'\s*=\s*(\d+)\s*;', decision, re.M)
        if not match:
            problems.append('%s: %s is not defined; the decision on the original\'s 5.7 has no bound' % (ORIGINAL, name))
            return
        numbers[name] = int(match.group(1))
    last, first = numbers['ORIGINAL_LAST_SHARED_SCHEMA'], numbers['FORK_FIRST_SCHEMA']
    steps = []
    for line in texts.get(DBSCRIPTS, '').split('\n'):
        match = UPGRADE_SCRIPT.match(line)
        if match:
            steps.append((int(match.group(1)), int(match.group(2)), line))
    if not any(start == last and end == first for start, end, _ in steps):
        problems.append('%s: the upgrade chain has no step from %d, the last version the decision takes the two projects '
                        'to share, to %d, the first it takes for this fork\'s own (%s has no Upgrade%dto%d script): the '
                        'decision would refuse a database this build upgrades, or let through one it does not'
                        % (ORIGINAL, last, first, DBSCRIPTS, last, first))
    between = sorted(set(name for start, end, name in steps if last < start < first or last < end < first))
    if between:
        problems.append('%s: the upgrade chain has a step from or to a version between %d and %d (%s), which the decision '
                        'refuses as the original\'s 5.7 alone' % (ORIGINAL, last, first, ', '.join(between[:4])))

    if not re.search(r'^#include\s+"%s"' % re.escape(ORIGINAL), extension, re.M):
        problems.append('%s: does not #include %s, the decision on the original\'s 5.7' % (EXTENSION, ORIGINAL))
    refusal = pascal_routine(extension, 'OriginalHMailServerRefusal') or ''
    if not refusal:
        problems.append('%s: there is no OriginalHMailServerRefusal, so nothing finds an installation of the original\'s '
                        '5.7 before setup replaces its programs' % EXTENSION)
    else:
        asked = refusal.find('IsServerBeforeThisFork(')
        read_at = refusal.find('ReadDatabaseVersion(')
        if asked < 0 or read_at < 0 or asked > read_at:
            problems.append('%s: OriginalHMailServerRefusal does not ask whether hMailServer.exe is older than this fork\'s 6 before it reads '
                            'the database\'s version, so every upgrade of this fork\'s would ask its service too' % EXTENSION)
        if not re.search(r"if\s+IsServiceRunning\('hMailServer'\)\s+then\s+iDatabase\s*:=\s*ReadDatabaseVersion\(", refusal):
            problems.append('%s: OriginalHMailServerRefusal reads the database\'s version without the service running; '
                            'COM activation would start a stopped service for it' % EXTENSION)
    setup = pascal_routine(extension, 'InitializeSetup') or ''
    call = re.search(r"szRefusal\s*:=\s*OriginalHMailServerRefusal\(\);\s*if\s+szRefusal\s*<>\s*''\s*then\s*begin(.*?)\bend;",
                     setup, re.S)
    if not call:
        problems.append('%s: InitializeSetup does not refuse what OriginalHMailServerRefusal answers' % EXTENSION)
    else:
        if not re.search(r'Result\s*:=\s*false\s*;', call.group(1), re.I) or not re.search(r'\bExit\s*;', call.group(1)):
            problems.append('%s: InitializeSetup does not end setup (Result := false; Exit;) on the refusal of the '
                            'original\'s 5.7' % EXTENSION)
        before = setup[:call.start()]
        if re.search(r'GetEnv\(|ExpandConstant\(|SuppressibleMsgBox\(|IsServiceRunning\(|CheckForMutexes\(|FindWindowByWindowName\(',
                     before):
            problems.append('%s: InitializeSetup does something before it asks OriginalHMailServerRefusal; the refusal '
                            'of the original\'s 5.7 comes first' % EXTENSION)

    if '#include "..\\..\\..\\hmailserver\\installation\\%s"' % ORIGINAL not in texts.get(GUARD_HARNESS, ''):
        problems.append('%s: the self-test does not include %s as the installer does' % (GUARD_HARNESS, ORIGINAL))
    packages = texts.get(PACKAGES, '')
    runs = packages.find('original-guard-test.ps1 -Iscc')
    builds = packages.find('& $iscc /Q $iss')
    if runs < 0 or builds < 0 or runs > builds:
        problems.append('%s: the Windows package jobs do not run %s before they compile the installer' % (PACKAGES, GUARD_RUNNER))
    elif 'Stop-Packages 8' not in packages[runs:builds]:
        problems.append('%s: a failed self-test of the decision on the original\'s 5.7 does not stop the job (exit 8)' % PACKAGES)


def check(texts):
    problems = []
    entries = check_run_once(texts, problems)
    check_wait(texts, problems)
    check_reboot(texts, problems)
    check_plugins(texts, problems)
    check_unregister(texts, problems)
    check_event_source(texts, problems)
    check_upgrade(texts, problems)
    check_credential(texts, problems)
    check_original(texts, problems)
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
        ('a client plugin installed without uninsrestartdelete',
         altered(texts, CLIENT_FILES, 'dialog.dll"; DestDir: "{app}\\Bin\\plugin"; Flags: ignoreversion uninsrestartdelete;',
                 'dialog.dll"; DestDir: "{app}\\Bin\\plugin"; Flags: ignoreversion;')),
        ('the client\'s plugins taken by a wildcard again',
         altered(texts, CLIENT_FILES, 'Source: ".\\Extras\\plugin\\auth_gssapi_client.dll";',
                 'Source: ".\\Extras\\plugin\\*.dll";')),
        ('a plugin the manifest lists and the installer does not name',
         altered(texts, CLIENT_FILES, 'Source: ".\\Extras\\plugin\\parsec.dll"; DestDir: "{app}\\Bin\\plugin"; '
                 'Flags: ignoreversion uninsrestartdelete; Components: server;\n', '')),
        ('a plugin named that the manifest does not list (6.3.6\'s auth_named_pipe.dll)',
         altered(texts, CLIENT_FILES, 'Source: ".\\Extras\\plugin\\sha256_password.dll";',
                 'Source: ".\\Extras\\plugin\\auth_named_pipe.dll"; DestDir: "{app}\\Bin\\plugin"; '
                 'Flags: ignoreversion uninsrestartdelete; Components: server;\n'
                 'Source: ".\\Extras\\plugin\\sha256_password.dll";')),
        ('the AppID registered before /Unregister is read',
         altered(texts, MAIN, '   _AtlModule.InitializeCom();\n',
                 '   _AtlModule.InitializeCom();\n   _AtlModule.RegisterAppID();\n')),
        ('the AppID removed only when UnregisterServer succeeded',
         altered(texts, MAIN, '      _AtlModule.UnregisterServer(TRUE);\n      _AtlModule.UnregisterAppId();\n',
                 '      if (SUCCEEDED(_AtlModule.UnregisterServer(TRUE)))\n         _AtlModule.UnregisterAppId();\n')),
        ('the installer\'s argument spelt differently from the server\'s',
         altered(texts, EXTENSION, "'start hMailServer --for-database-upgrade'", "'start hMailServer --database-upgrade'")),
        ('the password on DBSetupQuick\'s command line again',
         altered(texts, EXTENSION, '            szCredential := g_szAdminPassword\n',
                 '            szParameters := szParameters + \' password:\' + g_szAdminPassword\n')),
        ('HM_ADMIN_PASSWORD left set after DBSetupQuick',
         altered(texts, EXTENSION, "               finally\n                  SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '');\n",
                 "               finally\n                  Log('DBSetupQuick has finished');\n")),
        ('the password on DBSetup\'s command line',
         altered(texts, EXTENSION, "Exec(ExpandConstant('{app}\\Bin\\DBSetup.exe'), szParameters,",
                 "Exec(ExpandConstant('{app}\\Bin\\DBSetup.exe'), szParameters + ' password:' + szCredential,")),
        ('DBSetup started without the credential',
         altered(texts, EXTENSION, "if not SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', szCredential) then\n"
                 "               SuppressibleMsgBox('The hMailServer database setup could not be started",
                 "if not SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '') then\n"
                 "               SuppressibleMsgBox('The hMailServer database setup could not be started")),
        ('HM_ADMIN_PASSWORD left set after DBSetup',
         altered(texts, EXTENSION, "ExpandConstant('{app}\\Bin\\DBSetup.exe'), szParameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, "
                 "ResultCode);\n                  if (bStarted = False) then\n                     SuppressibleMsgBox(SysErrorMessage("
                 "ResultCode), mbError, MB_OK, IDOK);\n               finally\n                  SetProcessEnvironmentVariable("
                 "'HM_ADMIN_PASSWORD', '');\n",
                 "ExpandConstant('{app}\\Bin\\DBSetup.exe'), szParameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, "
                 "ResultCode);\n                  if (bStarted = False) then\n                     SuppressibleMsgBox(SysErrorMessage("
                 "ResultCode), mbError, MB_OK, IDOK);\n               finally\n                  Log('DBSetup has finished');\n")),
        ('DBUpdater\'s command line built as one string',
         altered(texts, DBSETUPQUICK, 'upgradeProcess.ArgumentList.Add("/SilentIfOk");',
                 'upgradeProcess.Arguments = "/SilentIfOk";')),
        ('the decision\'s shared bound moved past 5708',
         altered(texts, ORIGINAL, 'ORIGINAL_LAST_SHARED_SCHEMA = 5708;', 'ORIGINAL_LAST_SHARED_SCHEMA = 5709;')),
        ('a step of this fork\'s own to a version the decision refuses',
         altered(texts, DBSCRIPTS, 'Upgrade5708to6001MSSQL.sql', 'Upgrade5708to6001MSSQL.sql\nUpgrade6001to5900MSSQL.sql')),
        ('the decision not included',
         altered(texts, EXTENSION, '#include "OriginalHMailServer.iss"', '// the decision is elsewhere')),
        ('the refusal not acted on',
         altered(texts, EXTENSION, "\t\tSuppressibleMsgBox(szRefusal, mbCriticalError, MB_OK, IDOK);\n\t\tResult := false;\n\t\tExit;\n",
                 "\t\tSuppressibleMsgBox(szRefusal, mbCriticalError, MB_OK, IDOK);\n")),
        ('the refusal asked after a window check',
         altered(texts, EXTENSION, "\tResult := true;\n\n\t// An installation of the original hMailServer's 5.7",
                 "\tResult := true;\n\tif FindWindowByWindowName('x') > 0 then Exit;\n\n\t// An installation of the original hMailServer's 5.7")),
        ('the database read of a stopped service',
         altered(texts, EXTENSION, "   if IsServiceRunning('hMailServer') then\n      iDatabase := ReadDatabaseVersion(",
                 "   if True then\n      iDatabase := ReadDatabaseVersion(")),
        ('the self-test not run before the installer is built',
         altered(texts, PACKAGES, 'original-guard-test.ps1 -Iscc', 'original-guard-test.ps1 -Skip')),
        ('the password on DBUpdater\'s command line',
         altered(texts, DBSETUPQUICK, 'upgradeProcess.Environment[Authenticator.PasswordVariable] = password;',
                 'upgradeProcess.ArgumentList.Add("password:" + password);')),
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
          'the files it loads are queued for a restart if still in use; the client\'s plugins are installed by the '
          'manifest\'s names; /Unregister creates nothing and removes the '
          'event source; the upgrade\'s start is marked; the database tools\' password is on no command line; '
          'the original\'s 5.7 is refused first, by bounds the upgrade chain holds, and its self-test runs before the '
          'installer is built; %d negative controls refused' % (entries, controls))
    return 0


if __name__ == '__main__':
    sys.exit(main())
