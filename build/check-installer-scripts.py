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
               its refusal, which it writes to a file in %TEMP% as well as to the
               log. Its two bounds, 5708 (the last schema the two projects
               share) and 6001 (this fork's first after it), are the upgrade
               chain's: a step from the one to the other is in DBScripts, and no
               step from or to a version between them. The database's version is
               read only after hMailServer.exe is found to be before 6, not this
               fork's, and only from a running service; with it unread, the
               [Database] PasswordEncryption of the settings file the installed
               server reads - for the original's 5.7.0 build 3013 and later the one
               in ProgramData when it exists, before the earlier installation's own,
               and for its earlier builds (IsBeforeProgramDataSettings) the one in
               Bin alone - is read and passed to the decision, so that a database
               whose password only this fork can read is not refused. The decision's
               self-test,
               build/ci/packages/original-guard-test.iss, includes the same file,
               and windows-packages.ps1 runs it before it compiles the installer,
               stopping with exit 8 when it fails. That self-test runs the cases;
               this holds that they are run.
  program folder
               Work item 78's R1 to R4: the program folder, Bin and ControlPanel
               never let anybody but SYSTEM and the Administrators change them at
               any point of an installation. PrepareToInstall's last step,
               PrepareProgramFolders, holds each from the root of its volume down
               (HoldFolderChain, HM_HOLD_FOLDER_ACCESS with READ_CONTROL, not shared
               for deleting) until DeinitializeSetup; refuses a link on the way that
               anybody but SYSTEM, the Administrators and TrustedInstaller owns or may
               change or remove - every right ProtectedFile.cpp's
               OnlyAdministratorsControl counts on the link, read through the held
               handle (GetSecurityInfo) - and goes on from where an administrators'
               link leads; refuses a share, a network drive and a volume that keeps
               no permissions; makes a program folder that is not there with
               CreateDirectoryW and a protected descriptor, the folders above it
               too, and section_dirs.iss gives the uninstaller each, when this
               setup made it, while DeinitializeSetup removes those still empty
               when the files were not installed (R1); hardens one that is there,
               Bin and ControlPanel, before the files, the program folder of a
               server installation only when its data folders inherit nothing, and
               one another account owns rebuilt with the entries that give no right
               to write kept, the service's among them (R2); and says, in the log,
               what another account owns directly inside Bin and ControlPanel and
               the entries of a folder's own that let every user write (R4).
               HardenNewProgramFolder, which hardened a folder Inno Setup had made,
               is not to come back.
  masks        Every mask the [Code] tests an entry's rights against (head.Mask and
               HM_..., and the rights AclLetsOthers is asked about) is a named
               HM_ constant that carries exactly the generic rights whose mapping
               for a file or folder (FILE_GENERIC_READ, FILE_GENERIC_WRITE,
               FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS) meets one of its own, since
               an entry handed down is often written with generic rights and the
               server maps them before it compares (ServiceAccount.cpp's
               FileRights_); and HM_FOLDER_READ_RIGHTS asks for FILE_LIST_DIRECTORY,
               as EveryUserReads_ asks the mapped rights for FILE_READ_DATA. A read
               mask without GENERIC_READ ($10000001) found no user who could read a
               folder made in Program Files, which hands Users (GR,GE) down, so
               setup made C:\\Program Files\\hMailServer with no entry for Users
               (the installer smoke VM, 23 October 2026).

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
its refusal not acted on, the refusal not written to its file, the refusal
asked after a window check, the database
read from a stopped service, the settings file's password scheme not passed to the
decision, the settings file read in Bin alone, the one in ProgramData read whatever
the installed server's build, the original's builds 2788 and 2843 taken to read it,
the self-test not run before the installer is built, a program folder made without
its descriptor, the program folder recorded for the uninstaller whatever made it,
HardenNewProgramFolder back after Inno Setup's folder, the folders setup made left
after a cancel, Bin not hardened before its files, a folder another account owns
given three entries alone, the service's inherited Read dropped from it, the hold
without READ_CONTROL, a link gone through unasked, a link's permissions read by its
path, Bin's link not checked, the rights on a link narrowed below the server's, the
folders prepared without the refusal reaching PrepareToInstall's answer, the owners
in Bin not said, the entries kept not said, a volume without permissions not
refused, the read mask without GENERIC_READ, and an entry's rights tested against
an unnamed mask, must all be refused.

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
# The server's rule for a link it goes through, which the installer's is held to.
PROTECTED_FILE = 'Common/Util/ProtectedFile.cpp'
# The server's question before it gives Users Read & execute (EveryUserReads_),
# which the installer's read mask is held to.
SERVICE_ACCOUNT = 'Common/Util/ServiceAccount.cpp'
DIRS = 'section_dirs.iss'
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
    texts[PROTECTED_FILE] = read(os.path.join(SERVER, PROTECTED_FILE))
    texts[SERVICE_ACCOUNT] = read(os.path.join(SERVER, SERVICE_ACCOUNT))
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
        if not re.search(r"bForkSecret\s*:=\s*IsForkOnlyPasswordEncryption\(GetIniInt\('Database',\s*'PasswordEncryption',", refusal) \
                or not re.search(r'OriginalHMailServerKind\([^)]*,\s*bForkSecret\)', refusal):
            problems.append('%s: OriginalHMailServerRefusal does not pass the decision whether the earlier settings file keeps '
                            'the database\'s password as only this fork does ([Database] PasswordEncryption), so an '
                            'installation of this fork that the original\'s 5.7 installer was run over is refused every '
                            'time' % EXTENSION)
        if not re.search(r"szSettings\s*:=\s*'';\s*"
                         r"if\s+not\s+IsBeforeProgramDataSettings\(iMajor,\s*iMinor,\s*wRevision,\s*wBuild\)\s+then\s+"
                         r"szSettings\s*:=\s*ExpandConstant\('\{commonappdata\}\\hMailServer\\hMailServer\.ini'\);\s*"
                         r"if\s+\(szSettings\s*=\s*''\)\s+or\s+not\s+FileExists\(szSettings\)\s+then\s+"
                         r"szSettings\s*:=\s*PreviousSettingsFile\(szFolder\);",
                         refusal):
            problems.append('%s: OriginalHMailServerRefusal does not read the settings file the installed server reads: '
                            'for the original\'s 5.7.0 build 3013 and later the one in ProgramData '
                            '({commonappdata}\\hMailServer\\hMailServer.ini) before the earlier installation\'s own '
                            '(PreviousSettingsFile), and for its earlier builds (IsBeforeProgramDataSettings) the one in '
                            'Bin alone: a stale settings file of this fork\'s in Bin would let a working original 5.7 '
                            'through, or a ProgramData file left by a later 5.7 would refuse this fork\'s own database '
                            'under the original\'s builds 2788 and 2843' % EXTENSION)
        before = pascal_routine(decision, 'IsBeforeProgramDataSettings') or ''
        if not re.search(r'\(ServerMinor\s*=\s*7\)\s+and\s+\(ServerRevision\s*=\s*0\)\s+and\s+\(ServerBuild\s*<\s*3013\)', before):
            problems.append('%s: IsBeforeProgramDataSettings does not take the original\'s 5.7.0 builds before 3013, '
                            'which read their settings in Bin alone' % ORIGINAL)
    setup =pascal_routine(extension, 'InitializeSetup') or ''
    call = re.search(r"szRefusal\s*:=\s*OriginalHMailServerRefusal\(\);\s*if\s+szRefusal\s*<>\s*''\s*then\s*begin(.*?)\bend;",
                     setup, re.S)
    if not call:
        problems.append('%s: InitializeSetup does not refuse what OriginalHMailServerRefusal answers' % EXTENSION)
    else:
        if not re.search(r'Result\s*:=\s*false\s*;', call.group(1), re.I) or not re.search(r'\bExit\s*;', call.group(1)):
            problems.append('%s: InitializeSetup does not end setup (Result := false; Exit;) on the refusal of the '
                            'original\'s 5.7' % EXTENSION)
        if not re.search(r'\bWriteRefusalFile\(szRefusal\)\s*;', call.group(1)):
            problems.append('%s: InitializeSetup does not write the refusal of the original\'s 5.7 to its file '
                            '(WriteRefusalFile), so a silent setup run without /LOG leaves no record of why' % EXTENSION)
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


# What each right OnlyAdministratorsControl counts on a link is, and the mask the
# installer counts on one (HM_FOLDER_WRITE_RIGHTS).
FILE_RIGHTS = {'FILE_WRITE_DATA': 0x2, 'FILE_APPEND_DATA': 0x4, 'FILE_WRITE_EA': 0x10, 'FILE_WRITE_ATTRIBUTES': 0x100,
               'FILE_DELETE_CHILD': 0x40, 'DELETE': 0x10000, 'WRITE_DAC': 0x40000, 'WRITE_OWNER': 0x80000}
# The [Code] of work item 78's R1 to R4, every one of which an attended and a silent
# installation run.
PROGRAM_FOLDER_ROUTINES = ('PrepareProgramFolders', 'HoldFolderChain', 'HoldForSetup', 'LinkIsAdministratorsAlone', 'LinkTarget',
                           'MakeProgramFolder', 'RemoveFoldersMadeHere', 'HardenHeldFolder', 'RebuildFolderOfUntrustedOwner',
                           'SayBroadEntriesKept', 'SayForeignOwners', 'DataFolderThatInherits', 'InheritsNothing',
                           'VolumeKeepsPermissions', 'SplitAtRoot', 'DeinitializeSetup', 'FolderMadeBySetup', 'SetupMadeFolder',
                           'SetupMadeProgramFolder', 'AclLetsOthers', 'IsTrustedSid')


def check_program_folders(texts, problems):
    """Work item 78's R1 to R4 (the docstring's "program folder")."""
    ext = texts.get(EXTENSION, '')
    routines = {}
    for name in PROGRAM_FOLDER_ROUTINES:
        routines[name] = pascal_routine(ext, name)
        if routines[name] is None:
            problems.append('%s: %s is not there (work item 78)' % (EXTENSION, name))
    if any(body is None for body in routines.values()):
        return
    for name, body in routines.items():
        if re.search(r'(?<!Suppressible)MsgBox\(', body):
            problems.append('%s: %s uses a message box /SUPPRESSMSGBOXES does not suppress' % (EXTENSION, name))

    prepare = routines['PrepareProgramFolders']
    # Last in PrepareToInstall, its refusal PrepareToInstall's answer.
    to_install = pascal_routine(ext, 'PrepareToInstall') or ''
    if not re.search(r"Result\s*:=\s*PrepareProgramFolders\(\);\s*if\s+Result\s*<>\s*''\s*then\s*Log\('Refused: '\s*\+\s*Result\);\s*end;\s*$", to_install):
        problems.append('%s: PrepareToInstall does not end by preparing the program folders, its refusal its answer' % EXTENSION)
    step = pascal_routine(ext, 'CurStepChanged') or ''
    if re.search(r'Harden\w*\(', step):
        problems.append('%s: a program folder is hardened at ssInstall, after Inno Setup has begun, rather than in PrepareToInstall' % EXTENSION)

    # R3: the hold, the chain and the rule.
    if not re.search(r'HM_HOLD_FOLDER_ACCESS\s*=\s*\$200A1;', ext):
        problems.append('%s: the hold does not ask for READ_CONTROL ($200A1), so a link is not read through the handle that holds it' % EXTENSION)
    if not re.search(r'HM_SHARE_ALL_BUT_DELETE\s*=\s*\$3;', ext) or not re.search(r'HM_HOLD_FOLDER_FLAGS\s*=\s*\$02200000;', ext):
        problems.append('%s: a folder is held shared for deleting, or through its link' % EXTENSION)
    chain = routines['HoldFolderChain']
    for word in ('HoldForSetup(szNext)', 'LinkIsAdministratorsAlone(handle, parent, szWhy)', 'LinkTarget(handle, szNext, szTarget, szWhy)',
                 'HoldFolderChain(szTarget, Depth + 1, Real, Why)', 'Depth >= 8', 'LinkRemedy(szNext)', 'HM_DRIVE_REMOTE'):
        if word not in chain:
            problems.append('%s: HoldFolderChain does not use %s' % (EXTENSION, word))
    rule = routines['LinkIsAdministratorsAlone']
    if len(re.findall(r'ReadHeldSecurity\(h(?:Link|Parent),\s*HM_FILE_OBJECT,\s*HM_OWNER_AND_DACL_INFORMATION', rule)) != 2:
        problems.append('%s: a link\'s and its folder\'s owner and permissions are not read through the handles that hold them' % EXTENSION)
    if re.search(r'ReadFolder(?:Permissions|Owner)\(', rule):
        problems.append('%s: a link\'s permissions are read by its path' % EXTENSION)
    for word in ('AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho)', 'AclLetsOthers(pDacl, HM_REMOVE_FROM_FOLDER_RIGHTS, true, szWho)'):
        if word not in rule:
            problems.append('%s: LinkIsAdministratorsAlone does not use %s' % (EXTENSION, word))
    mask = re.search(r'HM_FOLDER_WRITE_RIGHTS\s*=\s*\$([0-9A-Fa-f]+);', ext)
    on_it = re.search(r'const ACCESS_MASK onIt\s*=\s*([A-Z_ |]+);', texts.get(PROTECTED_FILE, ''))
    if not mask or not on_it:
        problems.append('%s or %s: the rights counted on a link are not there' % (EXTENSION, PROTECTED_FILE))
    else:
        value = int(mask.group(1), 16)
        for right in [part.strip() for part in on_it.group(1).split('|')]:
            if right not in FILE_RIGHTS:
                problems.append('%s: OnlyAdministratorsControl counts %s, which this check does not know' % (PROTECTED_FILE, right))
            elif not value & FILE_RIGHTS[right]:
                problems.append('%s: the installer does not count %s on a link, which the server does' % (EXTENSION, right))
    if not re.search(r'HM_REMOVE_FROM_FOLDER_RIGHTS\s*=\s*\$100C0040;', ext):
        problems.append('%s: what removes a link from its folder is not FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL' % EXTENSION)
    chains = [prepare.find('if not HoldFolderChain(' + path + ') then') for path in
              ('szApp, 0, szReal, szWhy', "AddBackslash(szApp) + 'Bin', 0, szBin, szWhy", "AddBackslash(szApp) + 'ControlPanel', 0, szControlPanel, szWhy")]
    if min(chains) < 0:
        problems.append('%s: PrepareProgramFolders does not hold and check the program folder, Bin and ControlPanel each' % EXTENSION)
    else:
        changes = [prepare.find(word) for word in ('MakeProgramFolder(', 'HardenHeldFolder(', 'SayForeignOwners(')]
        if min(changes) < 0 or min(changes) < max(chains):
            problems.append('%s: PrepareProgramFolders changes or reads a folder before every link on the way is checked' % EXTENSION)
    if 'ReleaseFolder(' in prepare or 'ReleaseHeldFolders();' not in routines['DeinitializeSetup']:
        problems.append('%s: the folders are not held until DeinitializeSetup' % EXTENSION)

    # The volumes no permissions protect.
    if 'if not VolumeKeepsPermissions(handle) then' not in prepare or 'HM_PERSISTENT_ACLS' not in routines['VolumeKeepsPermissions']:
        problems.append('%s: a program folder on a volume that keeps no permissions is not refused' % EXTENSION)
    if not re.search(r"else\s+if\s+Copy\(Path,\s*1,\s*2\)\s*=\s*'\\\\'\s+then\s+Why\s*:=", routines['SplitAtRoot']):
        problems.append('%s: a program folder on a share is not refused' % EXTENSION)

    # R1: made with its descriptor, recorded for the uninstaller, removed when not used.
    make = routines['MakeProgramFolder']
    if not re.search(r"function\s+MakeFolderWithSecurity\(lpPathName:\s*String;\s*var\s+lpSecurityAttributes:\s*HM_SECURITY_ATTRIBUTES\):\s*Boolean;\s*external\s+'CreateDirectoryW@kernel32\.dll stdcall';", ext):
        problems.append('%s: CreateDirectoryW is not declared with a SECURITY_ATTRIBUTES' % EXTENSION)
    for word in ("'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)'", "'(A;OICI;0x1200a9;;;BU)'", 'MakeFolderWithSecurity(missing[index], attributes)',
                 'HM_ERROR_ALREADY_EXISTS', 'g_aFoldersMade', 'HoldForSetup(missing[index])'):
        if word not in make:
            problems.append('%s: MakeProgramFolder does not use %s' % (EXTENSION, word))
    if pascal_routine(ext, 'HardenNewProgramFolder') is not None:
        problems.append('%s: HardenNewProgramFolder is back' % EXTENSION)
    dirs = [line for _, line in section_entries(texts.get(DIRS, ''), 'Dirs')]
    expected = ['Name: "{code:FolderMadeBySetup|%d}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(%d)' % (index, index) for index in range(8)] + \
               ['Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder']
    if dirs[:9] != expected:
        problems.append('%s: the first nine [Dirs] entries are not the folders setup made, above the program folder first, then the program folder, '
                        'each recorded for the uninstaller only when setup made it' % DIRS)
    if any('AfterInstall:' in line for line in dirs):
        problems.append('%s: a [Dirs] entry gives a folder its permissions after Inno Setup has made it' % DIRS)
    if not re.search(r'ReleaseHeldFolders\(\);\s*if\s+not\s+g_bFilesInstalled\s+then\s+RemoveFoldersMadeHere\(\);', routines['DeinitializeSetup']):
        problems.append('%s: the empty folders setup made are not removed when the files were not installed' % EXTENSION)
    if not re.search(r'CurStep\s*=\s*ssPostInstall\s+then\s+begin\s+(?://[^\n]*\n\s*)*g_bFilesInstalled\s*:=\s*true;', step):
        problems.append('%s: the files are not marked installed at ssPostInstall' % EXTENSION)

    # R2: hardened before the files, every installation, the service's entries kept.
    for word in ('HardenHeldFolder(szBin)', 'HardenHeldFolder(szControlPanel)', 'HardenHeldFolder(szReal)', 'DataFolderThatInherits(szReal)'):
        if word not in prepare:
            problems.append('%s: PrepareProgramFolders does not use %s' % (EXTENSION, word))
    if 'IsComponentSelected(' in prepare:
        problems.append('%s: the program folders are prepared for some components only' % EXTENSION)
    held = routines['HardenHeldFolder']
    if 'Result := RebuildFolderOfUntrustedOwner(Folder, bUsers);' not in held or 'O:BAD:PAI' in held:
        problems.append('%s: a folder another account owns is not rebuilt with the entries that give no right to write kept' % EXTENSION)
    rebuild = routines['RebuildFolderOfUntrustedOwner']
    if not re.search(r'SidIs\(pAce\s*\+\s*8,\s*HM_SERVICE_SID\)\s+and\s+\(not\s+writes\)\s+then\s+ok\s*:=\s*AddAsOwnEntry', rebuild):
        problems.append('%s: the service\'s inherited Read is not kept on a rebuilt folder' % EXTENSION)
    if not re.search(r'aceType\s*=\s*HM_ACCESS_DENIED\s+then\s+ok\s*:=\s*AddAsOwnEntry', rebuild):
        problems.append('%s: the deny entries are not kept on a rebuilt folder' % EXTENSION)

    # R4: said.
    position = held.find('SayBroadEntriesKept(Folder);')
    if position < 0 or position < held.find('icacls hardened'):
        problems.append('%s: the entries kept on a folder the administrators own are not said after icacls' % EXTENSION)
    first_harden = prepare.find('HardenHeldFolder(')
    for word in ('SayForeignOwners(szBin);', 'SayForeignOwners(szControlPanel);'):
        position = prepare.find(word)
        if position < 0 or position > first_harden:
            problems.append('%s: %s is not said before setup changes the folders' % (EXTENSION, word))
    if 'ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_INFORMATION' not in routines['SayForeignOwners']:
        problems.append('%s: the owners in Bin and ControlPanel are not read through a handle on each' % EXTENSION)


# Each generic right, its bit, and what it means for a file or a folder (the
# GENERIC_MAPPING ServiceAccount.cpp's FileRights_ and ProtectedFile.cpp map with:
# FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS).
GENERIC_RIGHTS = (('GENERIC_READ', 0x80000000, 0x120089), ('GENERIC_WRITE', 0x40000000, 0x120116),
                  ('GENERIC_EXECUTE', 0x20000000, 0x1200A0), ('GENERIC_ALL', 0x10000000, 0x1F01FF))
GENERIC_BITS = 0xF0000000
# The masks the [Code] is known to test an entry's rights against, so that a rule
# that found none of them would not pass for want of anything to look at.
TESTED_MASKS = ('HM_FOLDER_WRITE_RIGHTS', 'HM_FOLDER_READ_RIGHTS', 'HM_REMOVE_FROM_FOLDER_RIGHTS')


def check_access_masks(texts, problems):
    """Every mask an entry's rights are tested against counts generic rights as what they mean (the docstring's "masks")."""
    ext = texts.get(EXTENSION, '')
    constants = dict((match.group(1), int(match.group(2), 16))
                     for match in re.finditer(r'^\s*(HM_\w+)\s*=\s*\$([0-9A-Fa-f]+)\s*;', ext, re.M))
    tested = set(match.group(1) for match in re.finditer(r'\.Mask\s+and\s+(\$?\w+)', ext))
    tested |= set(match.group(1) for match in re.finditer(r'\bAclLetsOthers\(\s*\w+\s*,\s*(\$?\w+)\s*,', ext))
    # AclLetsOthers' own test is of its parameter, which its calls name.
    tested.discard('Rights')
    for name in TESTED_MASKS:
        if name not in tested:
            problems.append('%s: no entry\'s rights are tested against %s any more; update this check' % (EXTENSION, name))
    for name in sorted(tested):
        if name not in constants:
            problems.append('%s: an entry\'s rights are tested against %s, which is not a named HM_ mask, so what its generic '
                            'rights mean is not held' % (EXTENSION, name))
            continue
        value = constants[name]
        own = value & ~GENERIC_BITS & 0xFFFFFFFF
        for generic, bit, meaning in GENERIC_RIGHTS:
            if (meaning & own) and not (value & bit):
                problems.append('%s: %s ($%08X) leaves out %s, which an entry may carry unmapped and which means one of '
                                'its rights for a folder' % (EXTENSION, name, value, generic))
            elif (value & bit) and not (meaning & own):
                problems.append('%s: %s ($%08X) counts %s, which means none of its rights for a folder' % (EXTENSION, name, value, generic))
    if constants.get('HM_FOLDER_READ_RIGHTS', 0) & ~GENERIC_BITS != 0x1:
        problems.append('%s: HM_FOLDER_READ_RIGHTS does not ask for FILE_LIST_DIRECTORY alone, as the server asks for '
                        'FILE_READ_DATA' % EXTENSION)
    for routine in ('EveryUserMayRead', 'EveryUserMayReadMadeIn'):
        if 'head.Mask and HM_FOLDER_READ_RIGHTS' not in (pascal_routine(ext, routine) or ''):
            problems.append('%s: %s does not test an entry\'s rights against HM_FOLDER_READ_RIGHTS' % (EXTENSION, routine))
    server = texts.get(SERVICE_ACCOUNT, '')
    start = server.find('bool EveryUserReads_(PACL dacl)')
    reads = server[start:server.find('\n      }\n', start)] if start >= 0 else ''
    if 'FileRights_(allow->Mask) & FILE_READ_DATA' not in reads:
        problems.append('%s: EveryUserReads_ no longer asks the mapped rights for FILE_READ_DATA; hold the installer\'s '
                        'HM_FOLDER_READ_RIGHTS to what it asks now' % SERVICE_ACCOUNT)


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
    check_program_folders(texts, problems)
    check_access_masks(texts, problems)
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
        ('the refusal not written to its file',
         altered(texts, EXTENSION, "\t\tWriteRefusalFile(szRefusal);\n", "")),
        ('the refusal asked after a window check',
         altered(texts, EXTENSION, "\tResult := true;\n\n\t// An installation of the original hMailServer's 5.7",
                 "\tResult := true;\n\tif FindWindowByWindowName('x') > 0 then Exit;\n\n\t// An installation of the original hMailServer's 5.7")),
        ('the database read of a stopped service',
         altered(texts, EXTENSION, "   if IsServiceRunning('hMailServer') then\n      iDatabase := ReadDatabaseVersion(",
                 "   if True then\n      iDatabase := ReadDatabaseVersion(")),
        ('the settings file\'s password scheme not passed to the decision',
         altered(texts, EXTENSION, 'OriginalHMailServerKind(iMajor, iMinor, iDatabase, bScripts, bForkSecret)',
                 'OriginalHMailServerKind(iMajor, iMinor, iDatabase, bScripts, False)')),
        ('the settings file read in Bin alone',
         altered(texts, EXTENSION, "      szSettings := '';\n"
                 "      if not IsBeforeProgramDataSettings(iMajor, iMinor, wRevision, wBuild) then\n"
                 "         szSettings := ExpandConstant('{commonappdata}\\hMailServer\\hMailServer.ini');\n"
                 "      if (szSettings = '') or not FileExists(szSettings) then\n"
                 "         szSettings := PreviousSettingsFile(szFolder);\n",
                 "      szSettings := PreviousSettingsFile(szFolder);\n")),
        ('the settings file in ProgramData read whatever the installed server\'s build',
         altered(texts, EXTENSION, "      szSettings := '';\n"
                 "      if not IsBeforeProgramDataSettings(iMajor, iMinor, wRevision, wBuild) then\n"
                 "         szSettings := ExpandConstant('{commonappdata}\\hMailServer\\hMailServer.ini');\n",
                 "      szSettings := '';\n"
                 "         szSettings := ExpandConstant('{commonappdata}\\hMailServer\\hMailServer.ini');\n")),
        ('the original\'s builds 2788 and 2843 taken to read the settings file in ProgramData',
         altered(texts, ORIGINAL, '(ServerBuild < 3013)', '(ServerBuild < 2788)')),
        ('the self-test not run before the installer is built',
         altered(texts, PACKAGES, 'original-guard-test.ps1 -Iscc', 'original-guard-test.ps1 -Skip')),
        ('the password on DBUpdater\'s command line',
         altered(texts, DBSETUPQUICK, 'upgradeProcess.Environment[Authenticator.PasswordVariable] = password;',
                 'upgradeProcess.ArgumentList.Add("password:" + password);')),
        ('a program folder made without its descriptor',
         altered(texts, EXTENSION, 'if MakeFolderWithSecurity(missing[index], attributes) then', 'if CreateDir(missing[index]) then')),
        ('the program folder recorded for the uninstaller whatever made it',
         altered(texts, DIRS, 'Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder', 'Name: "{app}"; Flags: uninsalwaysuninstall')),
        ('HardenNewProgramFolder back after Inno Setup\'s folder',
         altered(texts, DIRS, 'Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder', 'Name: "{app}"; AfterInstall: HardenNewProgramFolder')),
        ('the folders setup made left after a cancel',
         altered(texts, EXTENSION, '\tif not g_bFilesInstalled then\n\t\tRemoveFoldersMadeHere();\n', '')),
        ('Bin not hardened before its files',
         altered(texts, EXTENSION, '   if DirExists(szBin) then\n      HardenHeldFolder(szBin);\n', '')),
        ('a folder another account owns given three entries alone',
         altered(texts, EXTENSION, '   Result := RebuildFolderOfUntrustedOwner(Folder, bUsers);',
                 "   szDescriptor := 'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)';")),
        ('the service\'s inherited Read dropped from a rebuilt folder',
         altered(texts, EXTENSION, 'SidIs(pAce + 8, HM_SERVICE_SID) and (not writes)', 'false')),
        ('the hold without READ_CONTROL',
         altered(texts, EXTENSION, 'HM_HOLD_FOLDER_ACCESS       = $200A1;', 'HM_HOLD_FOLDER_ACCESS       = $A1;')),
        ('a link gone through unasked',
         altered(texts, EXTENSION, 'if not LinkIsAdministratorsAlone(handle, parent, szWhy) then', 'if false then')),
        ('a link\'s permissions read by its path',
         altered(texts, EXTENSION, 'readResult := ReadHeldSecurity(hLink, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);',
                 'readResult := ReadFolderPermissions(Link, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);')),
        ('Bin\'s link not checked',
         altered(texts, EXTENSION, "if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0, szBin, szWhy) then", 'if false then')),
        ('the rights on a link narrowed below the server\'s (no FILE_WRITE_ATTRIBUTES)',
         altered(texts, EXTENSION, 'HM_FOLDER_WRITE_RIGHTS      = $500D0156;', 'HM_FOLDER_WRITE_RIGHTS      = $500D0056;')),
        ('the folders prepared without the refusal reaching PrepareToInstall\'s answer',
         altered(texts, EXTENSION, '   Result := PrepareProgramFolders();\n', '   PrepareProgramFolders();\n')),
        ('the owners in Bin not said',
         altered(texts, EXTENSION, '   if DirExists(szBin) then\n      SayForeignOwners(szBin);\n', '')),
        ('the entries kept on a folder the administrators own not said',
         altered(texts, EXTENSION, '      SayBroadEntriesKept(Folder);\n', '')),
        ('a volume without permissions not refused',
         altered(texts, EXTENSION, '      if not VolumeKeepsPermissions(handle) then', '      if false then')),
        ('the read mask without GENERIC_READ (the program folder made in Program Files with no entry for Users)',
         altered(texts, EXTENSION, 'HM_FOLDER_READ_RIGHTS       = $90000001;', 'HM_FOLDER_READ_RIGHTS       = $10000001;')),
        ('an entry\'s rights tested against an unnamed mask',
         altered(texts, EXTENSION, '((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and IsBroadSid(pAce + 8)',
                 '((head.Mask and $000D0156) <> 0) and IsBroadSid(pAce + 8)')),
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
          'installer is built; the program folder, Bin and ControlPanel are held from the root down, a link only the '
          'administrators control is the only one gone through, and they are made or hardened before the files; '
          '%d negative controls refused' % (entries, controls))
    return 0


if __name__ == '__main__':
    sys.exit(main())
