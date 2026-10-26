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
               let nobody but SYSTEM and the Administrators change them at any
               point of an installation, but for an entry of a folder's own for a
               named account, which setup says, or setup does not install.
               CheckProgramFolders, on the Ready page before the service is
               stopped, changes nothing: it holds each from the root of its volume
               down (HoldFolderChain, HM_HOLD_FOLDER_ACCESS with READ_CONTROL, not
               shared for deleting) until DeinitializeSetup, a missing name never
               looked at again by its path; refuses a link on the way that anybody
               but SYSTEM, the Administrators and TrustedInstaller owns or may
               change or remove - every right ProtectedFile.cpp's
               OnlyAdministratorsControl counts on the link, read through the held
               handle (GetSecurityInfo) - and goes on from where an administrators'
               link leads; refuses a folder on the way that is not a link, the root
               of its volume included, that another account owns or may rename,
               empty or change who may (AdminUpdateChecks.cpp's ReplaceRights_,
               which HM_REPLACE_RIGHTS is held to); refuses a share, a network
               drive, walks that disagree, and a volume of any of the three that
               keeps no permissions or cannot be told; refuses one of the three
               another account owns unless it is the earlier installation's; and
               refuses a link, or a program, a library or a folder another account
               controls, in Bin and ControlPanel at any depth, and on the program
               folder's own level and in Addons, Events, DBScripts, WebAdmin and
               Languages, scripts counted as programs there (R4).
               PrepareProgramFolders, the last step of PrepareToInstall, then makes
               a program folder that is not there with CreateDirectoryW and a
               protected descriptor - exactly the names below the deepest folder
               held, that folder read again through its handle and each folder made
               read where it is (HeldIsLink, MadeWhereNamed) - and section_dirs.iss
               gives the uninstaller each, when this setup made it, while
               DeinitializeSetup removes those still empty when the files were not
               installed (R1); hardens one that is there whatever it holds, but for
               the earlier installation's whose data folders inherit, through the
               held handle, an entry of its own that lets every user write taken
               away, one another account owns given to the Administrators with
               takeown /A and rebuilt, each read again, any failure a refusal; then
               makes Bin and ControlPanel again when they hold nothing and let
               another account write them, or hardens or makes them, removes the
               empty folders in them, and looks at what is in them again (R2, R4);
               and after the files, before setup runs any program of the
               installation, reads the three again through the handles that hold
               them (FoldersStillAsPrepared), any change failing the installation
               and keeping the service from starting. Each decision is held to its
               statement (PROGRAM_FOLDER_STATEMENTS), the helpers it relies on
               among them (SidIs, IsBroadSid, BroadGroupSid, AclLetsOthers and
               HoldForSetup whole), so that a one-line change of one is refused;
               HM_SERVICE_SID is the SID ServiceAccountTester holds the service's
               to, and a folder is made with its descriptor set once.
               HardenNewProgramFolder, which hardened a folder Inno Setup had made,
               and MakeProgramFolder, which looked at a path again, are not to come
               back.
  masks        Every mask the [Code] tests an entry's rights against (head.Mask and
               ..., and the rights AclLetsOthers and OwnEntriesLetOthers are asked
               about) is a named HM_ constant alone that carries exactly the
               generic rights whose mapping for a file or folder
               (FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
               FILE_ALL_ACCESS) meets one of its own, since an entry handed down is
               often written with generic rights and the server maps them before it
               compares (ServiceAccount.cpp's FileRights_); and
               HM_FOLDER_READ_RIGHTS asks for FILE_LIST_DIRECTORY, as
               EveryUserReads_ asks the mapped rights for FILE_READ_DATA. A read
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
the self-test not run before the installer is built, and work item 78's: each of
R1 to R4's decisions changed by one line - the nineteen of the review of 26 October
2026 among them - and each shape of its first repair undone (a folder made in the
meantime taken, a missing folder decided by its path, walks that disagree taken,
Bin and ControlPanel left to Inno Setup, a folder that could not be hardened
installed into, the program folder left for any inheriting Data, a folder another
account owns taken whoever's it is, a held folder read by its path, an entry that
lets every user write kept, the hardened folder not read again, takeown not run,
what is in Bin and ControlPanel only said or not looked at, a volume that cannot be
told taken, the folders checked after the service is stopped, a refusal after the
stop not saying so, a mask in parentheses or narrowed) and of its second repair (a
folder held made a link before what is made in it, a folder above the program folder
another account may rename or empty, an empty folder left for an earlier handle, the
look after the files left out, scripts and the program folder's other folders not
looked at, a volume's root taken for its device, the service's stop forgotten, and
the sixteen one-line changes of the review of 26 October 2026 to the helpers) - must
all be refused.

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
# The live update's rule for the folders above the program folder, which the
# installer's is held to.
ADMIN_UPDATE_CHECKS = 'Common/Util/AdminUpdateChecks.cpp'
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
    texts[ADMIN_UPDATE_CHECKS] = read(os.path.join(SERVER, ADMIN_UPDATE_CHECKS))
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
PROGRAM_FOLDER_ROUTINES = ('CheckProgramFolders', 'PrepareProgramFolders', 'HoldFolderChain', 'HoldForSetup', 'LinkIsAdministratorsAlone',
                           'LinkTarget', 'MakeFolders', 'RemoveFoldersMadeHere', 'HardenHeldFolder', 'RebuildFolderOfUntrustedOwner',
                           'HeldFolderIsHardened', 'LetsEveryUserWrite', 'SayWriteEntriesKept', 'CheckWhatIsInside', 'OwnEntriesLetOthers',
                           'IsProgramFile', 'OwnerIsAllowed', 'HeldOwnerIsTrusted', 'WalksAgree', 'DataFolderThatInherits', 'InheritsNothing',
                           'VolumeKeepsPermissions', 'SplitAtRoot', 'DeinitializeSetup', 'FolderMadeBySetup', 'SetupMadeFolder',
                           'SetupMadeProgramFolder', 'SetupMadeBin', 'SetupMadeControlPanel', 'AclLetsOthers', 'IsTrustedSid',
                           'PrepareToInstall', 'PrepareToInstallRefusal', 'NextButtonClick', 'EveryUserMayRead', 'EveryUserMayReadMadeIn',
                           'HeldIsLink', 'MadeWhereNamed', 'FinalPathOfHeld', 'FolderAboveIsAdministratorsAlone', 'IsAtOrAboveChecked', 'IsVolumeRoot',
                           'IsScriptFile', 'IsDataFolderName', 'CheckWhatElseIsInside',
                           'RemakeEmptyFolder', 'LetGoOfHeld', 'FolderHoldsNothing', 'ItemIsIn', 'NothingTurnedIn', 'PreparedFolderIsAsLeft',
                           'FoldersStillAsPrepared', 'CurStepChanged', 'KeepServiceFromStarting', 'LinkRemedy', 'HeldLetsOthersWrite',
                           'SidIs', 'IsBroadSid', 'BroadGroupSid')

# Each decision of R1 to R4 held by its exact statement, so that a one-line change of
# one is refused (the review of 26 October 2026 changed nineteen such lines, one at a
# time, and the rule that looked for words refused none of them). Each is (routine,
# what a change of it would do, its statement): Pascal as the script has it, where a
# space stands for any run of white space, ~ for anything up to the next semicolon,
# @@ for comment lines, and a ^ that starts it or a $ that ends it for the start or
# the end of the routine.
PROGRAM_FOLDER_STATEMENTS = (
    # R3: the chain and the rule of a link.
    ('HoldFolderChain', 'a name that cannot be read is taken for one that is not there',
     "if (lastError = 2) or (lastError = 3) then begin"),
    ('HoldFolderChain', 'a folder is not read again once it is held, so a link put in its place before the hold is taken for a folder',
     "handle := HoldForSetup(szNext); if handle = -1 then begin Why := ~; bDone := true; end else begin @@"
     "attributes := FolderAttributes(szNext); if (attributes = HM_INVALID_ATTRIBUTES) or ((attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) then"),
    ('HoldFolderChain', 'a link is gone through without the rule',
     "else if (attributes and HM_REPARSE_POINT) <> 0 then begin if not LinkIsAdministratorsAlone(handle, parent, szWhy) then Why := "),
    ('HoldFolderChain', 'what a missing name is made below is not the deepest folder held',
     "if CompareText(szCurrent, szRoot) = 0 then Held := szRoot else Held := RemoveBackslashUnlessRoot(szCurrent); HeldHandle := parent; Whole := false; Result := true;"),
    ('HoldFolderChain', 'the root of a volume reached by its name is taken for its device, its last backslash taken off (the review of 26 October 2026)',
     "if CompareText(szCurrent, szRoot) = 0 then Real := szRoot else Real := RemoveBackslashUnlessRoot(szCurrent); Held := Real;"),
    ('IsVolumeRoot', 'more than the root of a volume is taken for one, or the root of a volume reached by its name is not',
     "^function IsVolumeRoot(Path: String): Boolean; begin Result := Length(Path) <= 3; if not Result then if CompareText(Copy(Path, 1, 11), '\\\\?\\Volume{') = 0 then Result := Copy(Path, Length(Path) - 1, 2) = '}\\'; end;$"),
    ('HoldFolderChain', 'the last folder found is not marked as held',
     "Real := szNext; Held := szNext; HeldHandle := handle; Whole := true; Result := true;"),
    ('LinkIsAdministratorsAlone', 'a link\'s own owner is not held to the three',
     "if not IsTrustedSid(pOwner) then Why := 'it is owned by '"),
    ('LinkIsAdministratorsAlone', 'what may change a link is not asked',
     "else if AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho) then Why := szWho + ' may change it'"),
    ('LinkIsAdministratorsAlone', 'what may remove a link from its folder is not asked',
     "else if AclLetsOthers(pDacl, HM_REMOVE_FROM_FOLDER_RIGHTS, true, szWho) then Why := "),
    ('AclLetsOthers', 'the rights on a link itself are not weighed',
     "if (aceFlags and HM_INHERIT_ONLY) = 0 then begin if aceType = HM_ACCESS_ALLOWED then"),
    ('IsTrustedSid', 'an account other than SYSTEM, the Administrators and TrustedInstaller is trusted',
     "^function IsTrustedSid(pSid: cardinal): Boolean; begin "
     "Result := SidIs(pSid, 'S-1-5-18') or SidIs(pSid, 'S-1-5-32-544') or SidIs(pSid, HM_TRUSTED_INSTALLER_SID); end;$"),
    ('HoldFolderChain', 'the root of the volume is not held to the rule of a folder above the program folder',
     "if szRest <> '' then if not IsAtOrAboveChecked(szRoot) then if not FolderAboveIsAdministratorsAlone(parent, szRoot, szWhy) then begin Why := szWhy; Exit; end;"),
    ('HoldFolderChain', 'a folder above the program folder that another account owns, or may rename or empty, is gone through, or the walk goes on from a folder it did not take',
     "if szRest <> '' then if not IsAtOrAboveChecked(szNext) then if not FolderAboveIsAdministratorsAlone(handle, szNext, szWhy) then begin Why := szWhy; bDone := true; end; if not bDone then begin parent := handle; szCurrent := szNext;"),
    ('FolderAboveIsAdministratorsAlone', 'a folder above the program folder is not read through the handle that holds it, or one that cannot be read is taken',
     "readResult := ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin Why := ~; Exit; end;"),
    ('FolderAboveIsAdministratorsAlone', 'a folder above the program folder that another account owns is gone through',
     "if not IsTrustedSid(pOwner) then Why := "),
    ('FolderAboveIsAdministratorsAlone', 'what may rename or empty a folder above the program folder, or change who may, is not asked',
     "else if AclLetsOthers(pDacl, HM_REPLACE_RIGHTS, true, szWho) then begin"),
    ('IsAtOrAboveChecked', 'Bin\'s and ControlPanel\'s walks pass folders the program folder\'s walk did not hold to the rule',
     "^function IsAtOrAboveChecked(Folder: String): Boolean; var szFolder: String; begin Result := false; if g_szFolderAboveChecked = '' then Exit; szFolder := AddBackslash(Folder); Result := CompareText(Copy(AddBackslash(g_szFolderAboveChecked), 1, Length(szFolder)), szFolder) = 0; end;$"),
    ('CheckProgramFolders', 'the program folder\'s walk passes a folder above it unasked',
     "g_szFolderAboveChecked := ''; g_hProgramFolderPrepared := -1; g_hBinPrepared := -1; g_hControlPanelPrepared := -1; g_bProgramFolderHardened := false; "
     "CheckProgramFolderPermissions(szApp, false); if not HoldFolderChain(szApp, 0,"),
    ('CheckProgramFolders', 'Bin\'s and ControlPanel\'s walks are not held to the rule past what the program folder\'s walk held',
     "g_szFolderAboveChecked := g_szProgramFolderReal; if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0,"),
    ('SidIs', 'a SID is taken for another, so that every owner is trusted or every group of every user is not one (the review of 26 October 2026)',
     "^function SidIs(pSid: cardinal; szSid: String): Boolean; var pOther: cardinal; begin Result := false; if pSid = 0 then Exit; pOther := 0; if SidOfText(szSid, pOther) then begin Result := SidsAreEqual(pSid, pOther); FreeWindowsMemory(pOther); end; end;$"),
    ('IsBroadSid', 'a group of every user is not taken for one, so that an entry that lets every user write is kept',
     "^function IsBroadSid(pSid: cardinal): Boolean; var szSid: String; szName: String; group: Integer; begin Result := false; group := 0; szSid := BroadGroupSid(group, szName); while (not Result) and (szSid <> '') do begin if SidIs(pSid, szSid) then Result := true; group := group + 1; szSid := BroadGroupSid(group, szName); end; end;$"),
    ('BroadGroupSid', 'a group of every user is left out, or its SID mistyped',
     "^function BroadGroupSid(Index: Integer; var Name: String): String; begin Result := ''; Name := ''; case Index of 0: begin Result := 'S-1-1-0'; Name := 'Everyone'; end; 1: begin Result := 'S-1-5-11'; Name := 'Authenticated Users'; end; 2: begin Result := 'S-1-5-32-545'; Name := 'Users'; end; 3: begin Result := 'S-1-5-4'; Name := 'INTERACTIVE'; end; 4: begin Result := 'S-1-2-0'; Name := 'LOCAL'; end; 5: begin Result := 'S-1-5-32-546'; Name := 'Guests'; end; 6: begin Result := 'S-1-5-32-547'; Name := 'Power Users'; end; 7: begin Result := 'S-1-5-32-555'; Name := 'Remote Desktop Users'; end; 8: begin Result := 'S-1-5-14'; Name := 'REMOTE INTERACTIVE LOGON'; end; 9: begin Result := 'S-1-5-13'; Name := 'TERMINAL SERVER USER'; end; end; end;$"),
    ('AclLetsOthers', 'an entry of a form setup does not read, or one it cannot read, is taken for one that lets nobody, or another right counted',
     "^function AclLetsOthers(pDacl: cardinal; Rights: cardinal; OwnerTrusted: Boolean; var Who: String): Boolean; var info: ACL_SIZE_INFO; head: ACE_START; pAce: cardinal; index: cardinal; aceType: cardinal; aceFlags: cardinal; begin Result := false; Who := ''; if pDacl = 0 then begin Result := true; Who := 'everyone (it has no list of permissions at all)'; Exit; end; if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then begin Result := true; Who := 'an account in a list of permissions that could not be read'; Exit; end; index := 0; while (not Result) and (index < info.AceCount) do begin pAce := 0; if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then begin CopyAceStart(head, pAce, 8); aceType := head.TypeFlagsSize and $FF; aceFlags := (head.TypeFlagsSize shr 8) and $FF; if (aceFlags and HM_INHERIT_ONLY) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) then begin if not (OwnerTrusted and SidIs(pAce + 8, 'S-1-3-4')) then begin Result := true; Who := SidNameAt(pAce + 8); end; end; end else if (aceType = 4) or (aceType = 5) or (aceType = 9) or (aceType = 11) then begin Result := true; Who := 'an account in an entry of a form setup does not read'; end; end; end else begin Result := true; Who := 'an account in an entry that could not be read'; end; index := index + 1; end; end;$"),
    ('HoldForSetup', 'a folder is held shared for deleting, or through its link, or not kept until the end',
     "^function HoldForSetup(Folder: String): Integer; begin Result := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if Result <> -1 then begin SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1); g_aHeldFolders[GetArrayLength(g_aHeldFolders) - 1] := Result; end; end;$"),
    ('HoldFolderChain', 'a network drive is taken',
     "if KindOfDrive(szRoot) = HM_DRIVE_REMOTE then begin Why := "),
    ('LinkTarget', 'a reparse point other than a mount point or a symbolic link is gone through',
     "if (tagHigh = $A000) and (tagLow = $0003) then start := 9 else if (tagHigh = $A000) and (tagLow = $000C) then begin start := 11; relative := (Ord(buffer[9]) and 1) <> 0; end else begin Why := 'it is a reparse point of a kind setup does not go through (tag ' + IntToStr(tagHigh) + ':' + IntToStr(tagLow) + ')'; Exit; end;"),
    ('CheckWhatIsInside', 'an item of Bin or ControlPanel that cannot be opened is taken',
     "handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then szWho := 'its owner could not be read (Windows error '"),
    ('CheckWhatElseIsInside', 'an item of the program folder that cannot be opened is taken',
     "handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then szWho := 'its owner could not be read (Windows error '"),
    ('HeldFolderIsHardened', 'a rebuilt folder is taken although another account may still write it',
     "else if AnyAccount and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) then begin Result := false;"),
    ('MakeFolders', 'a folder is made without its descriptor',
     "attributes.nLength := 12; attributes.lpSecurityDescriptor := pDescriptor; attributes.bInheritHandle := 0; Result := true; if HeldIsLink(HeldHandle, bKnown) then"),
    ('LinkTarget', 'a link to a share is gone through',
     "else if CompareText(Copy(szName, 1, 8), '\\??\\UNC\\') = 0 then begin Why := "),
    # The volumes.
    ('VolumeKeepsPermissions', 'a volume that keeps no permissions is taken',
     "else if (flags and HM_PERSISTENT_ACLS) = 0 then Why := "),
    ('VolumeKeepsPermissions', 'a volume that cannot be told is taken',
     "begin Result := false; Why := ''; serial := 0; longest := 0; flags := 0; if not VolumeOfHeldFolder(HeldHandle, "),
    # The check, before anything changes and before the service is stopped.
    ('CheckProgramFolders', 'the program folder is not held and checked',
     "if not HoldFolderChain(szApp, 0, g_szProgramFolderReal, g_szProgramFolderHeld, g_hProgramFolder, g_bProgramFolderThere, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'Bin is not held and checked',
     "if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0, g_szBinReal, g_szBinHeld, g_hBin, g_bBinThere, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'ControlPanel is not held and checked',
     "if not HoldFolderChain(AddBackslash(szApp) + 'ControlPanel', 0, g_szControlPanelReal, g_szControlPanelHeld, g_hControlPanel, g_bControlPanelThere, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'the program folder\'s volume is not asked',
     "if not VolumeKeepsPermissions(g_hProgramFolder, g_szProgramFolderHeld, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'Bin\'s volume is not asked',
     "if not VolumeKeepsPermissions(g_hBin, g_szBinHeld, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'ControlPanel\'s volume is not asked',
     "if not VolumeKeepsPermissions(g_hControlPanel, g_szControlPanelHeld, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'walks that disagree - a folder or a link made while setup looked - are not refused',
     "if (not WalksAgree(g_szBinHeld, g_bBinThere)) or (not WalksAgree(g_szControlPanelHeld, g_bControlPanelThere)) then begin Result := "),
    ('CheckProgramFolders', 'the program folder is taken for the earlier installation\'s whatever that installation\'s folder',
     "g_bEarlierInstallation := false; if Trim(Earlier) <> '' then g_bEarlierInstallation := CompareText(RemoveBackslashUnlessRoot(Trim(Earlier)), szApp) = 0;"),
    ('CheckProgramFolders', 'the program folder\'s owner is not held to the rule',
     "if g_bProgramFolderThere and (not IsVolumeRoot(g_szProgramFolderReal)) then if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'Bin\'s owner is not held to the rule',
     "if g_bBinThere then if not OwnerIsAllowed(g_szBinReal, g_hBin, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'ControlPanel\'s owner is not held to the rule',
     "if g_bControlPanelThere then if not OwnerIsAllowed(g_szControlPanelReal, g_hControlPanel, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'what is in Bin and ControlPanel is not refused before the service is stopped',
     "if g_bBinThere then CheckWhatIsInside(g_szBinReal, 0, false, false, found, szWhy); if g_bControlPanelThere then "
     "CheckWhatIsInside(g_szControlPanelReal, 0, false, false, found, szWhy); if g_bProgramFolderThere then "
     "CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy); if found > 0 then Result := "),
    ('OwnerIsAllowed', 'a folder another account owns is taken although it is not the earlier installation\'s',
     "else if g_bEarlierInstallation then Result := true else Why := "),
    ('WalksAgree', 'what one walk found is not held to the others',
     "if g_bProgramFolderThere then Result := Whole or (CompareText(Held, g_szProgramFolderReal) = 0) else "
     "Result := (not Whole) and (CompareText(Held, g_szProgramFolderHeld) = 0);"),
    # R1: made.
    ('MakeFolders', 'a folder somebody else made in the meantime is taken',
     "if MakeFolderWithSecurity(missing[index], attributes) then begin if Kind = 0 then"),
    ('MakeFolders', 'a name somebody else took is not refused',
     "lastError := DLLGetLastError(); if lastError = HM_ERROR_ALREADY_EXISTS then Why := ~ else Why := ~; Result := false;"),
    ('MakeFolders', 'what is made is not the names below the deepest folder held',
     "szBase := AddBackslash(Held); if (Length(Folder) <= Length(szBase)) or (CompareText(Copy(Folder, 1, Length(szBase)), szBase) <> 0) then"),
    ('MakeFolders', 'the folder the walk held is not read again before a folder is made in it, so a link put in its place since is gone through (the review of 26 October 2026)',
     "Result := true; if HeldIsLink(HeldHandle, bKnown) then begin if bKnown then Why := ~ else Why := ~; Result := false; end; index := 0; parent := HeldHandle; while Result and (index < count) do"),
    ('MakeFolders', 'a folder made is not held and read where it is through its handle, so one made through a link put on the way is taken',
     "handle := HoldForSetup(missing[index]); if handle = -1 then begin Why := ~; Result := false; end "
     "else if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then begin Why := ~; Result := false; end "
     "else begin Log(~); parent := handle; MadeHandle := handle; end;"),
    ('MadeWhereNamed', 'a folder made is taken wherever it is, or where it is when that cannot be read',
     "if (szParent = '') or (szMade = '') then Where := ~ else if CompareText(szMade, AddBackslash(szParent) + Name) <> 0 then Where := ~ else Result := true;"),
    ('MadeWhereNamed', 'where a folder made is is not read through the handles',
     "Result := false; szParent := FinalPathOfHeld(Parent); szMade := FinalPathOfHeld(Made);"),
    ('HeldIsLink', 'a held folder that is a link now, or whose attributes cannot be read, is taken for a folder',
     "^function HeldIsLink(HeldHandle: Integer; var Known: Boolean): Boolean; var information: HM_FILE_INFORMATION; begin Result := true; "
     "Known := ReadHeldInformation(HeldHandle, information); if Known then Result := (information.dwFileAttributes and HM_REPARSE_POINT) <> 0; end;$"),
    ('FinalPathOfHeld', 'where a held folder is is not read through its handle, by its volume\'s name',
     "size := FinalPathOfHandle(HeldHandle, buffer, capacity, HM_VOLUME_NAME_GUID); if size >= capacity then begin"),
    ('RemoveFoldersMadeHere', 'a folder setup made is removed with what is in it',
     "if RemoveDir(g_aFoldersMade[index]) then Log("),
    ('RemoveFoldersMadeHere', 'a Bin or ControlPanel setup made is removed with what is in it',
     "if RemoveDir(szMade) then Log("),
    # R2: hardened, and refused when it cannot be.
    ('PrepareProgramFolders', 'a program folder that is not there is decided by its path',
     "if not g_bProgramFolderThere then begin if not MakeFolders(g_szProgramFolderHeld, g_hProgramFolder, g_szProgramFolderReal, 0, hMade, szWhy) then Result := "),
    ('PrepareProgramFolders', 'more than a drive\'s root is taken for one, or Bin and ControlPanel are passed over at a root',
     "if IsVolumeRoot(g_szProgramFolderReal) then Log(~) else begin szInherits := '';"),
    ('PrepareProgramFolders', 'the program folder is left as it is for an inheriting data folder that is not the earlier installation\'s',
     "szInherits := ''; if g_bEarlierInstallation then if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data') or "
     "FileExists(AddBackslash(g_szBinReal) + 'hMailServer.INI') then szInherits := DataFolderThatInherits(g_szProgramFolderReal);"),
    ('PrepareProgramFolders', 'a program folder that could not be hardened is installed into',
     "else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, g_bEarlierInstallation, szWhy) then begin Result := ~; Exit; end "
     "else g_bProgramFolderHardened := true; end; g_hProgramFolderPrepared := g_hProgramFolder;"),
    ('PrepareProgramFolders', 'a Bin that is there is not hardened, or one that could not be is installed into',
     "if g_bBinThere then begin if FolderHoldsNothing(g_szBinReal) and HeldLetsOthersWrite(g_hBin) then begin if not RemakeEmptyFolder(g_szBinReal, g_hBin, 1, hMade, szWhy) then begin Result := ~; Exit; end; "
     "g_hBinPrepared := hMade; end else if not HardenHeldFolder(g_szBinReal, g_hBin, g_bEarlierInstallation, szWhy) then begin Result := ~; Exit; end else g_hBinPrepared := g_hBin; end"),
    ('PrepareProgramFolders', 'a Bin that is not there is not made by setup',
     "end else if not MakeFolders(g_szBinHeld, g_hBin, g_szBinReal, 1, hMade, szWhy) then begin Result := ~; Exit; end else g_hBinPrepared := hMade;"),
    ('PrepareProgramFolders', 'a ControlPanel that is there is not hardened, or one that could not be is installed into',
     "if g_bControlPanelThere then begin if FolderHoldsNothing(g_szControlPanelReal) and HeldLetsOthersWrite(g_hControlPanel) then begin if not RemakeEmptyFolder(g_szControlPanelReal, g_hControlPanel, 2, hMade, szWhy) then "
     "begin Result := ~; Exit; end; g_hControlPanelPrepared := hMade; end else if not HardenHeldFolder(g_szControlPanelReal, g_hControlPanel, g_bEarlierInstallation, szWhy) then "
     "begin Result := ~; Exit; end else g_hControlPanelPrepared := g_hControlPanel; end"),
    ('PrepareProgramFolders', 'a ControlPanel that is not there is not made by setup',
     "end else if ControlPanelIsInstalled() then begin if not MakeFolders(g_szControlPanelHeld, g_hControlPanel, g_szControlPanelReal, 2, hMade, szWhy) then begin Result := ~; Exit; end; "
     "g_hControlPanelPrepared := hMade; end;"),
    ('PrepareProgramFolders', 'what is in Bin and ControlPanel is not looked at again once they are hardened',
     "if g_bBinThere then CheckWhatIsInside(g_szBinReal, 0, true, false, found, szWhy); if g_bControlPanelThere then "
     "CheckWhatIsInside(g_szControlPanelReal, 0, true, false, found, szWhy); if g_bProgramFolderThere then "
     "CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy); if found > 0 then begin Result := ~; Exit; end;"),
    ('HardenHeldFolder', 'a held folder\'s owner and permissions are not read through its handle',
     "readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);"),
    ('HardenHeldFolder', 'an entry of the folder\'s own that lets every user write is kept',
     "if ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0) and LetsEveryUserWrite(pAce) then begin szSid := TextOfSidAt(pAce + 8);"),
    ('HardenHeldFolder', 'an entry that lets every user write is not taken away, or a failure to take it is not a refusal',
     "'\" /remove:g ' + szSid, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin Why := ~; Exit; end; if ResultCode <> 0 then begin Why := ~; Exit; end;"),
    ('HardenHeldFolder', 'a trusted owner\'s folder does not stop inheriting, or icacls\'s failure is not a refusal',
     "'\"' + Folder + '\" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then "
     "begin Why := ~; Exit; end; Log(~); if ResultCode <> 0 then begin Why := ~; Exit; end;"),
    ('HardenHeldFolder', 'a folder another account owns is taken although it is not the earlier installation\'s',
     "else if not Adopt then begin Why := ~; FreeWindowsMemory(pDescriptor); Exit; end else begin "
     "Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, bUsers, Why);"),
    ('HardenHeldFolder', 'a hardened folder is not read again',
     "if not HeldFolderIsHardened(HeldHandle, not bTrusted, Why) then begin Why := ~; Exit; end; if bTrusted then SayWriteEntriesKept(Folder, HeldHandle) else begin"),
    ('HeldFolderIsHardened', 'a list that still inherits is taken for hardened',
     "else if (control and HM_DACL_PROTECTED) = 0 then Why := "),
    ('HeldFolderIsHardened', 'a folder another account still owns is taken for hardened',
     "else if not IsTrustedSid(pOwner) then Why := "),
    ('HeldFolderIsHardened', 'an entry that lets every user write is taken for hardened',
     "if LetsEveryUserWrite(pAce) then begin Result := false;"),
    ('LetsEveryUserWrite', 'what lets every user write is not an allow entry with a right to write for a group of every user',
     "if (head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED then if (head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0 then Result := IsBroadSid(pAce + 8);"),
    ('RebuildFolderOfUntrustedOwner', 'a rebuilt folder keeps an entry of its own that lets another account write',
     "if aceType <> HM_ACCESS_ALLOWED then szDropped := szDropped + ', an entry of its own of another kind' "
     "else if writes and (not IsTrustedSid(pAce + 8)) then szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own' "
     "else ok := AddAsOwnEntry(pNew, pAce);"),
    ('RebuildFolderOfUntrustedOwner', 'a rebuilt folder keeps an inherited entry other than the service\'s Read',
     "if aceType <> HM_ACCESS_ALLOWED then szDropped := szDropped + ', an inherited entry of another kind' "
     "else if SidIs(pAce + 8, HM_SERVICE_SID) and (not writes) then ok := AddAsOwnEntry(pNew, pAce) "
     "else if writes and (not SidIs(pAce + 8, 'S-1-5-18')) and (not SidIs(pAce + 8, 'S-1-5-32-544')) then szDropped := ~; end;"),
    ('RebuildFolderOfUntrustedOwner', 'the deny entries are not kept on a rebuilt folder',
     "if pass = 0 then begin if aceType = HM_ACCESS_DENIED then ok := AddAsOwnEntry(pNew, pAce); end"),
    ('RebuildFolderOfUntrustedOwner', 'a folder another account owns is not given to the Administrators with their privilege first',
     "if not Exec(ExpandConstant('{sys}\\takeown.exe'), '/F \"' + Folder + '\" /A', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then Why := ~ "
     "else if ResultCode <> 0 then Why := ~ else begin written := WriteFolderSecurity(Folder, HM_FILE_OBJECT, HM_PROTECTED_DACL_INFORMATION, 0, 0, pNew, 0);"),
    # R4: what is in Bin and ControlPanel, and what is said.
    ('CheckWhatIsInside', 'a program, a library, a folder or a link another account controls is said and not refused',
     "if bLink or bCode or bFolder then begin Found := Found + 1;"),
    ('CheckWhatIsInside', 'a link, a program or a library inside Bin or ControlPanel is not known as one',
     "bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0; bCode := (not bFolder) and (IsProgramFile(rec.Name) or (Scripts and IsScriptFile(rec.Name)));"),
    ('CheckWhatIsInside', 'an owner other than the three is not refused',
     "else if not IsTrustedSid(pOwner) then begin if (not bCode) and (not bFolder) and SidIs(pOwner, HM_SERVICE_SID) then szWho := ''"),
    ('CheckWhatIsInside', 'a program or a folder an entry of whose own lets another account change it is not refused',
     "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szText) then"),
    ('CheckWhatIsInside', 'what is below the first level is not looked at',
     "for index := 0 to count - 1 do if not CheckWhatIsInside(folders[index], Depth + 1, Final, Scripts, Found, Why) then Result := false;"),
    ('CheckWhatIsInside', 'the owner of what is in Bin and ControlPanel is not read through a handle on each',
     "readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);"),
    ('IsProgramFile', 'a program or a library is not what the live update holds to its rule',
     "Result := (szExtension = '.exe') or (szExtension = '.dll');"),
    ('OwnEntriesLetOthers', 'an inherited entry is weighed, or an entry of the item\'s own is not',
     "if (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin "
     "if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) then"),
    ('IsScriptFile', 'a script the service, DBSetup or the web server runs is not counted as a program',
     "Result := (szExtension = '.vbs') or (szExtension = '.js') or (szExtension = '.sql') or (szExtension = '.php') or (szExtension = '.ps1') or (szExtension = '.cmd') or (szExtension = '.bat');"),
    ('IsDataFolderName', 'a folder other than the data folders the service writes in is taken for one',
     "Result := (CompareText(Name, 'Data') = 0) or (CompareText(Name, 'Logs') = 0) or (CompareText(Name, 'Temp') = 0) or (CompareText(Name, 'Database') = 0);"),
    ('CheckWhatElseIsInside', 'which items of the program folder are links, programs, scripts or data folders is not read',
     "bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0; bData := bFolder and IsDataFolderName(rec.Name); bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsScriptFile(rec.Name));"),
    ('CheckWhatElseIsInside', 'a link in the program folder that another account controls is taken (the review of 26 October 2026)',
     "if bLink then begin if not LinkIsAdministratorsAlone(handle, -1, szText) then begin szWho := ~; szRemedy := ~; end; end"),
    ('CheckWhatElseIsInside', 'the owner of what is in the program folder is not read through a handle on each',
     "readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);"),
    ('CheckWhatElseIsInside', 'an owner other than the three is taken in the program folder, the service\'s of a data folder aside',
     "else if not IsTrustedSid(pOwner) then begin if bData and SidIs(pOwner, HM_SERVICE_SID) then szWho := '' else szWho := "),
    ('CheckWhatElseIsInside', 'an entry of its own that lets another account change a program, a script or a folder of the program folder is taken',
     "else if bCode or (bFolder and (not bData)) then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szText) then"),
    ('CheckWhatElseIsInside', 'a link, a program, a script or a folder another account controls in the program folder is said, not refused',
     "if bLink or bCode or (bFolder and (not bData)) then begin Found := Found + 1;"),
    ('CheckWhatElseIsInside', 'the folders setup installs into that hold what is run are not these five',
     "0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages';"),
    ('CheckWhatElseIsInside', 'what is in those folders is not looked at at any depth, scripts counted as programs',
     "if not CheckWhatIsInside(szFolder, 0, Final, true, Found, Why) then Result := false;"),
    ('LinkIsAdministratorsAlone', 'the folder a link is in is not asked about unless the link is in the program folder itself',
     "FreeWindowsMemory(pDescriptor); if not Result then Exit; if hParent = -1 then Exit; Result := false;"),
    ('RemakeEmptyFolder', 'a Bin or ControlPanel that holds nothing is not let go, removed and made again, or a failure of either is taken (the review of 26 October 2026)',
     "parent := HoldForSetup(szParent); if parent = -1 then begin Why := ~; Exit; end; LetGoOfHeld(HeldHandle); if not RemoveDir(Folder) then begin Why := ~; Exit; end; Log(~); Result := MakeFolders(szParent, parent, Folder, Kind, MadeHandle, Why);"),
    ('HeldLetsOthersWrite', 'a folder another account could write before it was hardened is taken for one it could not, or one that cannot be read is',
     "Result := true; pOwner := 0; pDacl := 0; pDescriptor := 0; if ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then Exit; "
     "if IsTrustedSid(pOwner) then Result := AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho);"),
    ('LetGoOfHeld', 'a folder let go is let go again at the end, or another is',
     "if g_aHeldFolders[index] = Handle then begin ReleaseFolder(Handle); last := GetArrayLength(g_aHeldFolders) - 1; g_aHeldFolders[index] := g_aHeldFolders[last]; SetArrayLength(g_aHeldFolders, last); end;"),
    ('FolderHoldsNothing', 'a folder that holds something is taken for empty, or an empty one for one that holds something',
     "Result := false; if FindFirst(AddBackslash(Folder) + '*', rec) then begin try Result := true; repeat if (rec.Name <> '.') and (rec.Name <> '..') then Result := false; until (not Result) or (not FindNext(rec));"),
    ('CheckWhatIsInside', 'an empty folder in Bin, ControlPanel or the folders that hold what is run is left, once they are hardened, for a handle opened before to make a junction',
     "if Final and FolderHoldsNothing(szItem) then begin if RemoveDir(szItem) then Log('Setup removed ' + szItem + ', an empty folder there before this installation"),
    ('CheckWhatIsInside', 'an empty folder that could not be removed is taken',
     "Log('The program folder: refused, ' + szText); Result := false; end; end else begin SetArrayLength(folders, count + 1);"),
    ('CheckWhatElseIsInside', 'an empty folder of the program folder\'s own level is left, once it is hardened, for a handle opened before to make a junction',
     "else if Final and bFolder and (not bLink) and (not bData) and FolderHoldsNothing(szItem) then begin @@if RemoveDir(szItem) then Log('Setup removed ' + szItem + ', an empty folder there before this installation"),
    ('ItemIsIn', 'what the path reaches is taken for what is in the folder setup holds without being read through the two handles',
     "handle := HoldFolder(Item, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then Exit; szItem := FinalPathOfHeld(handle); ReleaseFolder(handle); szFolder := FinalPathOfHeld(FolderHandle); if (szItem <> '') and (szFolder <> '') then Result := CompareText(szItem, AddBackslash(szFolder) + ExtractFileName(Item)) = 0;"),
    ('NothingTurnedIn', 'a link or an empty folder setup did not leave is taken after the files, or left where it is',
     "if bLink or (bFolder and FolderHoldsNothing(szItem)) then begin if bLink then szWhat := ~ else szWhat := ~; if bLink and (not bFolder) then DeleteFile(szItem) else RemoveDir(szItem); Found := Found + 1;"),
    ('NothingTurnedIn', 'what is below the first level is not looked at after the files',
     "for index := 0 to count - 1 do if not NothingTurnedIn(folders[index], Depth + 1, Found, Why) then Result := false;"),
    ('PreparedFolderIsAsLeft', 'a folder setup prepared is taken after the files as a link, reached elsewhere by its path, or with its permissions changed',
     "Result := false; if HeldIsLink(Handle, bKnown) then Why := ~ else if (Item <> '') and (not ItemIsIn(Item, Handle)) then Why := ~ else if Hardened and (not HeldFolderIsHardened(Handle, false, szWhy)) then Why := ~ else Result := true;"),
    ('FoldersStillAsPrepared', 'the program folder and Bin are not read again after the files',
     "if g_hProgramFolderPrepared <> -1 then PreparedFolderIsAsLeft(g_szProgramFolderReal, g_hProgramFolderPrepared, szApp + 'Bin', g_bProgramFolderHardened, Why); if g_hBinPrepared <> -1 then PreparedFolderIsAsLeft(g_szBinReal, g_hBinPrepared, szApp + 'Bin\\hMailServer.INI', true, Why);"),
    ('FoldersStillAsPrepared', 'ControlPanel is not read again after the files',
     "if ControlPanelIsInstalled() then PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, szApp + 'ControlPanel\\hMailCP.exe', true, Why) else PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, '', true, Why);"),
    ('FoldersStillAsPrepared', 'what is in Bin, ControlPanel and the folders that hold what is run is not looked at after the files',
     "NothingTurnedIn(szApp + 'Bin', 0, found, Why); NothingTurnedIn(szApp + 'ControlPanel', 0, found, Why); for index := 0 to 4 do begin case index of 0: NothingTurnedIn(szApp + 'Addons', 0, found, Why); 1: NothingTurnedIn(szApp + 'Events', 0, found, Why); 2: NothingTurnedIn(szApp + 'DBScripts', 0, found, Why); 3: NothingTurnedIn(szApp + 'WebAdmin', 0, found, Why); 4: NothingTurnedIn(szApp + 'Languages', 0, found, Why); end; end; Result := Why = '';"),
    ('CurStepChanged', 'the folders are not read again before setup runs a program of the installation, or what is not as left does not fail it and keep the service from starting',
     "g_bFilesInstalled := true; @@if not FoldersStillAsPrepared(szWhy) then begin g_bStepsIncomplete := true; KeepServiceFromStarting(); RaiseException("),
    ('KeepServiceFromStarting', 'the service is not set not to start',
     "if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= disabled', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then"),
    ('LinkRemedy', 'the remedy of a link does not say that a restart takes away what a program opened it for',
     "/L /inheritance:r /grant:r *S-1-5-18:F *S-1-5-32-544:F *S-1-5-32-545:RX - restarts Windows, since a program that opened the link while another account could ' + 'change it keeps that right until it closes it, and runs setup again"),
    ('HardenHeldFolder', 'the earlier installation\'s folder another account owned is not said, with the restart that takes its handles away',
     "if bTrusted then SayWriteEntriesKept(Folder, HeldHandle) else begin @@szText := ~; Log('The program folder: ' + szText); g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10; end; Result := true;"),
    ('PrepareProgramFolders', 'a program folder setup made is not held for the look after the files',
     "else begin g_hProgramFolderPrepared := hMade; g_bProgramFolderHardened := true; end;"),
    ('SayWriteEntriesKept', 'an entry kept that lets an account other than the three write is not said',
     "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and "
     "(not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) then"),
    # The service: what is refused before it is stopped, and said after.
    ('NextButtonClick', 'the folders are not checked before the service is stopped, or a refusal there stops it all the same',
     "g_szFolderRefusal := CheckProgramFolders(PreviousInstallationFolder()); if g_szFolderRefusal <> '' then Exit;"),
    ('NextButtonClick', 'whether setup stopped a running service is not known',
     "bWasRunning := IsServiceRunning('hMailServer'); if bWasRunning then g_bServiceStoppedForSetup := true; if (StopHMailServerService() = false) then Result := false;"),
    ('PrepareToInstall', 'a refusal after setup stopped the service does not say so',
     "Result := PrepareToInstallRefusal(); if Result <> '' then begin @@if g_bServiceStoppedForSetup then Result := Result + ' Setup stopped the hMailServer service~; "
     "Log('Refused: ' + Result); end; end;$"),
    ('PrepareToInstallRefusal', 'the program folders are not prepared last, their refusal the answer',
     "Result := PrepareProgramFolders(PreviousInstallationFolder()); end;$"),
)


def statement_pattern(statement):
    """A PROGRAM_FOLDER_STATEMENTS statement as a regular expression."""
    out = []
    if statement.startswith('^'):
        out.append(r'\A')
        statement = statement[1:]
    end = statement.endswith('$')
    if end:
        statement = statement[:-1]
    index = 0
    while index < len(statement):
        if statement.startswith('@@', index):
            out.append(r'(?://[^\n]*\n\s*)*')
            index += 2
        elif statement[index] == '~':
            out.append(r'[^;]*')
            index += 1
        elif statement[index] == ' ':
            out.append(r'\s*')
            index += 1
        else:
            out.append(re.escape(statement[index]))
            index += 1
    if end:
        out.append(r'\s*\Z')
    return ''.join(out)


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

    for routine, decision, statement in PROGRAM_FOLDER_STATEMENTS:
        if not re.search(statement_pattern(statement), routines[routine]):
            problems.append('%s: %s: %s (the statement work item 78 holds it to is not there)' % (EXTENSION, routine, decision))

    prepare = routines['PrepareProgramFolders']
    check = routines['CheckProgramFolders']
    step = pascal_routine(ext, 'CurStepChanged') or ''
    if re.search(r'Harden\w*\(', step):
        problems.append('%s: a program folder is hardened at ssInstall, after Inno Setup has begun, rather than in PrepareToInstall' % EXTENSION)

    # R3: the hold, the chain and the rule.
    if not re.search(r'HM_HOLD_FOLDER_ACCESS\s*=\s*\$200A1;', ext):
        problems.append('%s: the hold does not ask for READ_CONTROL ($200A1), so a link is not read through the handle that holds it' % EXTENSION)
    if not re.search(r'HM_SHARE_ALL_BUT_DELETE\s*=\s*\$3;', ext) or not re.search(r'HM_HOLD_FOLDER_FLAGS\s*=\s*\$02200000;', ext):
        problems.append('%s: a folder is held shared for deleting, or through its link' % EXTENSION)
    chain = routines['HoldFolderChain']
    for word in ('HoldForSetup(szNext)', 'LinkTarget(handle, szNext, szTarget, szWhy)', 'HoldFolderChain(szTarget, Depth + 1, Real, Held, HeldHandle, Whole, Why)',
                 'Depth >= 8', 'LinkRemedy(szNext)', 'HM_DRIVE_REMOTE'):
        if word not in chain:
            problems.append('%s: HoldFolderChain does not use %s' % (EXTENSION, word))
    if len(re.findall(r'attributes\s*:=\s*FolderAttributes\(szNext\);', chain)) != 2:
        problems.append('%s: HoldFolderChain does not read a folder\'s attributes before and after it holds it' % EXTENSION)
    rule = routines['LinkIsAdministratorsAlone']
    if len(re.findall(r'ReadHeldSecurity\(h(?:Link|Parent),\s*HM_FILE_OBJECT,\s*HM_OWNER_AND_DACL_INFORMATION', rule)) != 2:
        problems.append('%s: a link\'s and its folder\'s owner and permissions are not read through the handles that hold them' % EXTENSION)
    if len(re.findall(r'not\s+IsTrustedSid\(pOwner\)', rule)) != 2:
        problems.append('%s: the owner of a link, or of the folder it is in, is not held to SYSTEM, the Administrators and TrustedInstaller' % EXTENSION)
    for name in ('LinkIsAdministratorsAlone', 'HardenHeldFolder', 'HeldFolderIsHardened', 'HeldOwnerIsTrusted', 'CheckWhatIsInside', 'CheckWhatElseIsInside',
                 'SayWriteEntriesKept', 'EveryUserMayReadMadeIn', 'RebuildFolderOfUntrustedOwner', 'MakeFolders', 'CheckProgramFolders', 'PrepareProgramFolders'):
        if re.search(r'ReadFolder(?:Permissions|Owner)\(', routines[name]):
            problems.append('%s: %s reads a folder\'s permissions by its path, which names whatever is there by then, rather than through the handle '
                            'that holds it' % (EXTENSION, name))
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
    # The service's SID, which a rebuilt Bin keeps the Read of and whose files in Bin are
    # not said: the one ServiceAccountTester holds NT SERVICE\hMailServer's to.
    tester = re.search(r'ServiceAccount::ServiceSid\(_T\("hMailServer"\)\)\s*!=\s*_T\("(S-1-5-80-[0-9-]+)"\)', texts.get(SERVICE_ACCOUNT, ''))
    if not tester:
        problems.append('%s: ServiceAccountTester no longer holds NT SERVICE\\hMailServer\'s SID to a value' % SERVICE_ACCOUNT)
    elif "HM_SERVICE_SID              = '%s';" % tester.group(1) not in ext:
        problems.append('%s: HM_SERVICE_SID is not NT SERVICE\\hMailServer\'s SID, %s' % (EXTENSION, tester.group(1)))
    # A folder is made with a descriptor of its own, and nothing takes it away.
    if len(re.findall(r'attributes\.lpSecurityDescriptor\s*:=', ext)) != 1:
        problems.append('%s: the descriptor a folder is made with is set other than once, as setup made it' % EXTENSION)
    if not re.search(r'HM_REMOVE_FROM_FOLDER_RIGHTS\s*=\s*\$100C0040;', ext):
        problems.append('%s: what removes a link from its folder is not FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL' % EXTENSION)
    # The folders above the program folder: every right the live update counts on one.
    replace = re.search(r'HM_REPLACE_RIGHTS\s*=\s*\$([0-9A-Fa-f]+);', ext)
    above = re.search(r'const DWORD ReplaceRights_\s*=\s*([A-Z_ |]+);', texts.get(ADMIN_UPDATE_CHECKS, ''))
    if not replace or not above:
        problems.append('%s or %s: the rights counted on a folder above the program folder are not there' % (EXTENSION, ADMIN_UPDATE_CHECKS))
    else:
        for right in [part.strip() for part in above.group(1).split('|')]:
            if right not in FILE_RIGHTS:
                problems.append('%s: ReplaceRights_ counts %s, which this check does not know' % (ADMIN_UPDATE_CHECKS, right))
            elif not int(replace.group(1), 16) & FILE_RIGHTS[right]:
                problems.append('%s: setup does not count %s on a folder above the program folder, which the live update does' % (EXTENSION, right))
    if not re.search(r"else\s+if\s+Copy\(Path,\s*1,\s*2\)\s*=\s*'\\\\'\s+then\s+Why\s*:=", routines['SplitAtRoot']):
        problems.append('%s: a program folder on a share is not refused' % EXTENSION)

    # Nothing changed before every walk is checked, nothing decided by a path again, and
    # the check before the service is stopped.
    if re.search(r'\b(?:MakeFolders|HardenHeldFolder|RebuildFolderOfUntrustedOwner)\(', check):
        problems.append('%s: CheckProgramFolders, which runs before the service is stopped, changes a folder' % EXTENSION)
    if 'CheckProgramFolders(' not in prepare or prepare.find('CheckProgramFolders(') > prepare.find('MakeFolders('):
        problems.append('%s: PrepareProgramFolders changes a folder before the walks are checked' % EXTENSION)
    for name in ('MakeFolders', 'PrepareProgramFolders', 'CheckProgramFolders', 'HoldFolderChain'):
        if re.search(r'DirExists\((?:szReal|szPath|missing\[|szNext|Folder\)|g_szProgramFolderReal\)|g_szProgramFolderHeld|g_szBin|g_szControlPanel)', routines[name]):
            problems.append('%s: %s decides what is there by looking at a path again, not by what the walk held' % (EXTENSION, name))
    if 'ReleaseFolder(' in prepare or 'ReleaseFolder(' in check or 'ReleaseHeldFolders();' not in routines['DeinitializeSetup']:
        problems.append('%s: the folders are not held until DeinitializeSetup' % EXTENSION)
    if 'IsComponentSelected(' in prepare or 'IsComponentSelected(' in check:
        problems.append('%s: the program folders are prepared for some components only' % EXTENSION)
    ready = routines['NextButtonClick']
    after = routines['CurStepChanged']
    looked = after.find('FoldersStillAsPrepared(')
    if looked < 0 or any(0 <= after.find(word) < looked for word in ('RunPostInstallTasks()', 'RegisterTypeLib()', 'DeleteOldFiles()')):
        problems.append('%s: the folders setup prepared are not read again before setup runs a program of the installation' % EXTENSION)
    if re.findall(r'g_bServiceStoppedForSetup\s*:=\s*(\w+)', ext) != ['true']:
        problems.append('%s: whether setup stopped a running service is set other than once, to true, so a later pass may clear it' % EXTENSION)
    if ready.find('CheckProgramFolders(') < 0 or ready.find('CheckProgramFolders(') > ready.find('StopHMailServerService()'):
        problems.append('%s: the folders are checked after the service is stopped, so a refusal leaves a running server stopped' % EXTENSION)

    # R1: made with its descriptor, recorded for the uninstaller, removed when not used.
    make = routines['MakeFolders']
    if not re.search(r"function\s+MakeFolderWithSecurity\(lpPathName:\s*String;\s*var\s+lpSecurityAttributes:\s*HM_SECURITY_ATTRIBUTES\):\s*Boolean;\s*external\s+'CreateDirectoryW@kernel32\.dll stdcall';", ext):
        problems.append('%s: CreateDirectoryW is not declared with a SECURITY_ATTRIBUTES' % EXTENSION)
    for word in ("'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)'", "'(A;OICI;0x1200a9;;;BU)'", 'EveryUserMayReadMadeIn(HeldHandle)', 'g_aFoldersMade',
                 'g_szBinMade := missing[index]', 'g_szControlPanelMade := missing[index]', 'HoldForSetup(missing[index])', 'HeldIsLink(HeldHandle, bKnown)',
                 'MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere)'):
        if word not in make:
            problems.append('%s: MakeFolders does not use %s' % (EXTENSION, word))
    if not re.search(r"function\s+ReadHeldInformation\(hFile:\s*Integer;\s*var\s+lpFileInformation:\s*HM_FILE_INFORMATION\):\s*Boolean;\s*external\s+'GetFileInformationByHandle@kernel32\.dll stdcall';", ext) or \
       not re.search(r"function\s+FinalPathOfHandle\(hFile:\s*Integer;\s*lpszFilePath:\s*String;\s*cchFilePath:\s*cardinal;\s*dwFlags:\s*cardinal\):\s*cardinal;\s*external\s+'GetFinalPathNameByHandleW@kernel32\.dll stdcall';", ext) or \
       not re.search(r'HM_VOLUME_NAME_GUID\s*=\s*1;', ext):
        problems.append('%s: what a held folder is now, and where, is not read through its handle (GetFileInformationByHandle, GetFinalPathNameByHandleW by the volume\'s name)' % EXTENSION)
    if pascal_routine(ext, 'HardenNewProgramFolder') is not None or pascal_routine(ext, 'MakeProgramFolder') is not None:
        problems.append('%s: HardenNewProgramFolder or MakeProgramFolder is back' % EXTENSION)
    dirs = [line for _, line in section_entries(texts.get(DIRS, ''), 'Dirs')]
    expected = ['Name: "{code:FolderMadeBySetup|%d}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(%d)' % (index, index) for index in range(8)] + \
               ['Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder',
                'Name: "{app}\\Bin"; Flags: uninsalwaysuninstall; Check: SetupMadeBin',
                'Name: "{app}\\ControlPanel"; Flags: uninsalwaysuninstall; Check: SetupMadeControlPanel']
    if dirs[:11] != expected:
        problems.append('%s: the first eleven [Dirs] entries are not the folders setup made, above the program folder first, then the program folder, Bin and '
                        'ControlPanel, each recorded for the uninstaller only when setup made it' % DIRS)
    if any('AfterInstall:' in line for line in dirs):
        problems.append('%s: a [Dirs] entry gives a folder its permissions after Inno Setup has made it' % DIRS)
    if not re.search(r"Result\s*:=\s*g_szBinMade\s*<>\s*'';", routines['SetupMadeBin']) or \
       not re.search(r"Result\s*:=\s*g_szControlPanelMade\s*<>\s*'';", routines['SetupMadeControlPanel']):
        problems.append('%s: Bin or ControlPanel is given to the uninstaller whatever made it' % EXTENSION)
    if not re.search(r'ReleaseHeldFolders\(\);\s*if\s+not\s+g_bFilesInstalled\s+then\s+RemoveFoldersMadeHere\(\);', routines['DeinitializeSetup']):
        problems.append('%s: the empty folders setup made are not removed when the files were not installed' % EXTENSION)
    if not re.search(r'CurStep\s*=\s*ssPostInstall\s+then\s+begin\s+(?://[^\n]*\n\s*)*g_bFilesInstalled\s*:=\s*true;', step):
        problems.append('%s: the files are not marked installed at ssPostInstall' % EXTENSION)
    if 'DelTree(' in routines['RemoveFoldersMadeHere']:
        problems.append('%s: RemoveFoldersMadeHere removes what is in a folder setup made' % EXTENSION)

    # R2: in order - the program folder, then Bin and ControlPanel, then what is in them.
    order = [prepare.find(word) for word in ('HardenHeldFolder(g_szProgramFolderReal', 'HardenHeldFolder(g_szBinReal', 'MakeFolders(g_szBinHeld',
                                             'HardenHeldFolder(g_szControlPanelReal', 'MakeFolders(g_szControlPanelHeld', 'CheckWhatIsInside(g_szBinReal, 0, true')]
    if min(order) < 0 or order != sorted(order):
        problems.append('%s: PrepareProgramFolders does not harden the program folder, then make or harden Bin and ControlPanel, then look at what is in them' % EXTENSION)
    held = routines['HardenHeldFolder']
    if 'O:BAD:PAI' in held:
        problems.append('%s: a folder another account owns is given three entries alone' % EXTENSION)
    if held.find('/remove:g') < 0 or held.find('/remove:g') > held.find('/inheritance:r'):
        problems.append('%s: the entries that let every user write are not taken away before the folder stops inheriting' % EXTENSION)
    if 'IsBroadSid(' in routines['SayWriteEntriesKept']:
        problems.append('%s: SayWriteEntriesKept says the groups of every user alone, not every other account an entry kept lets write' % EXTENSION)
    if not re.search(r'HM_PROTECTED_DACL_INFORMATION\s*=\s*\$80000004;', ext):
        problems.append('%s: a rebuilt list is not written as one that inherits nothing ($80000004)' % EXTENSION)


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
    tested = set()
    for match in re.finditer(r'\.Mask\s+and\s+([^)]*)\)', ext):
        tested.add(match.group(1).strip())
    for match in re.finditer(r'\b(?:AclLetsOthers|OwnEntriesLetOthers)\(([^,()]*),([^,]*),', ext):
        if match.group(1).strip() != 'pDacl: cardinal':
            tested.add(match.group(2).strip())
    # AclLetsOthers' own test is of its parameter, which its calls name.
    tested.discard('Rights')
    for name in TESTED_MASKS:
        if name not in tested:
            problems.append('%s: no entry\'s rights are tested against %s any more; update this check' % (EXTENSION, name))
    tested.discard('Rights: cardinal')
    for name in sorted(tested):
        if name not in constants:
            problems.append('%s: an entry\'s rights are tested against %s, which is not a named HM_ mask alone, so what its generic '
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
        # Work item 78's R1 to R4: each decision, one line changed at a time - the
        # nineteen of the review of 26 October 2026 among them - and each shape the
        # first repair of that review gave the installer.
        # R1: made, below what the walk held, and recorded for the uninstaller.
        ('a program folder made without its descriptor',
         altered(texts, EXTENSION, 'if MakeFolderWithSecurity(missing[index], attributes) then', 'if CreateDir(missing[index]) then')),
        ('a folder somebody else made in the meantime taken (ERROR_ALREADY_EXISTS taken for made)',
         altered(texts, EXTENSION, 'if MakeFolderWithSecurity(missing[index], attributes) then',
                 'if MakeFolderWithSecurity(missing[index], attributes) or (DLLGetLastError() = HM_ERROR_ALREADY_EXISTS) then')),
        ('the folder the walk held not read again before a folder is made in it (a link put in its place gone through; the review of 26 October 2026)',
         altered(texts, EXTENSION, '   if HeldIsLink(HeldHandle, bKnown) then\n', '   if false then\n')),
        ('a folder made not read where it is (one made through a link put on the way taken)',
         altered(texts, EXTENSION, 'else if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then', 'else if false then')),
        ('each folder made compared with the folder the walk held, not with the one made above it',
         altered(texts, EXTENSION, '            parent := handle;\n            MadeHandle := handle;\n', '            MadeHandle := handle;\n')),
        ('a held folder whose attributes cannot be read taken for one that is no link',
         altered(texts, EXTENSION, '   Result := true;\n   Known := ReadHeldInformation(HeldHandle, information);', '   Result := false;\n   Known := ReadHeldInformation(HeldHandle, information);')),
        ('a folder made taken wherever it is',
         altered(texts, EXTENSION, 'else if CompareText(szMade, AddBackslash(szParent) + Name) <> 0 then', 'else if false then')),
        ('a folder above the program folder that another account may rename or empty gone through (the review of 26 October 2026)',
         altered(texts, EXTENSION, 'if not FolderAboveIsAdministratorsAlone(handle, szNext, szWhy) then', 'if false then')),
        ('the root of the volume not held to the rule of a folder above the program folder',
         altered(texts, EXTENSION, 'if not FolderAboveIsAdministratorsAlone(parent, szRoot, szWhy) then', 'if false then')),
        ('renaming a folder above the program folder not counted (DELETE out of the mask)',
         altered(texts, EXTENSION, 'HM_REPLACE_RIGHTS           = $100D0040;', 'HM_REPLACE_RIGHTS           = $100C0040;')),
        ('a folder above the program folder that another account owns gone through',
         altered(texts, EXTENSION, "   if not IsTrustedSid(pOwner) then\n      Why := Folder + ', a folder on the way to it, is owned by '",
                 "   if false then\n      Why := Folder + ', a folder on the way to it, is owned by '")),
        ('a folder above the program folder asked what may remove a link from it alone',
         altered(texts, EXTENSION, 'AclLetsOthers(pDacl, HM_REPLACE_RIGHTS, true, szWho)', 'AclLetsOthers(pDacl, HM_REMOVE_FROM_FOLDER_RIGHTS, true, szWho)')),
        ('every folder taken for one the program folder\'s walk held to the rule',
         altered(texts, EXTENSION, "   Result := false;\n   if g_szFolderAboveChecked = '' then\n      Exit;", "   Result := true;\n   if g_szFolderAboveChecked = '' then\n      Exit;")),
        ('the program folder\'s own walk passing what is above it as checked',
         altered(texts, EXTENSION, "g_szFolderAboveChecked := '';\n   g_hProgramFolderPrepared := -1;", "g_szFolderAboveChecked := szApp;\n   g_hProgramFolderPrepared := -1;")),
        ('an empty Bin that was there hardened, not made again (a handle opened before keeps its rights; the review of 26 October 2026)',
         altered(texts, EXTENSION, 'if FolderHoldsNothing(g_szBinReal) and HeldLetsOthersWrite(g_hBin) then', 'if false then')),
        ('an empty ControlPanel that was there hardened, not made again',
         altered(texts, EXTENSION, 'if FolderHoldsNothing(g_szControlPanelReal) and HeldLetsOthersWrite(g_hControlPanel) then', 'if false then')),
        ('a folder another account owned taken for one it could not write',
         altered(texts, EXTENSION, '   if IsTrustedSid(pOwner) then\n      Result := AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho);',
                 '   Result := AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho);')),
        ('a folder made again not held to where it is made',
         altered(texts, EXTENSION, '   Result := MakeFolders(szParent, parent, Folder, Kind, MadeHandle, Why);\n', '   Result := CreateDir(Folder);\n')),
        ('no folder ever taken for empty',
         altered(texts, EXTENSION, '         Result := true;\n         repeat\n            if (rec.Name <> \'.\')', '         Result := false;\n         repeat\n            if (rec.Name <> \'.\')')),
        ('an empty folder in Bin or ControlPanel left once they are hardened',
         altered(texts, EXTENSION, 'if Final and FolderHoldsNothing(szItem) then', 'if false then')),
        ('an empty folder of the program folder\'s own level left once it is hardened',
         altered(texts, EXTENSION, 'else if Final and bFolder and (not bLink) and (not bData) and FolderHoldsNothing(szItem) then', 'else if false then')),
        ('the folders not read again after the files',
         altered(texts, EXTENSION, 'if not FoldersStillAsPrepared(szWhy) then', 'if false then')),
        ('the service let start when the folders are not as setup left them',
         altered(texts, EXTENSION, '\t    KeepServiceFromStarting();\n', '')),
        ('the folders not as setup left them ending in exit code 0',
         altered(texts, EXTENSION, '\t    g_bStepsIncomplete := true;\n\t    KeepServiceFromStarting();', '\t    KeepServiceFromStarting();')),
        ('a link found after the files left',
         altered(texts, EXTENSION, 'if bLink or (bFolder and FolderHoldsNothing(szItem)) then', 'if bFolder and FolderHoldsNothing(szItem) then')),
        ('Bin not held to holding the file setup installed there',
         altered(texts, EXTENSION, "szApp + 'Bin\\hMailServer.INI', true, Why)", "'', true, Why)")),
        ('a folder setup prepared not read for a link after the files',
         altered(texts, EXTENSION, '   if HeldIsLink(Handle, bKnown) then\n      Why := Why + Folder', '   if false then\n      Why := Why + Folder')),
        ('a folder setup prepared not read for its permissions after the files',
         altered(texts, EXTENSION, 'else if Hardened and (not HeldFolderIsHardened(Handle, false, szWhy)) then', 'else if false then')),
        ('what a path reaches taken for what is in the folder held',
         altered(texts, EXTENSION, 'Result := CompareText(szItem, AddBackslash(szFolder) + ExtractFileName(Item)) = 0;', 'Result := true;')),
        ('the remedy of a link without the restart',
         altered(texts, EXTENSION, ' - restarts Windows, since a program that opened the link while another account could \' +\n             \'change it keeps that right until it closes it, and runs setup again;',
                 ' - and runs setup again;')),
        ('the earlier installation\'s folder another account owned not said',
         altered(texts, EXTENSION, "      g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10;\n   end;\n   Result := true;\nend;", "   end;\n   Result := true;\nend;")),
        ('HoldForSetup holding a folder shared for deleting (the review of 26 October 2026: missed)',
         altered(texts, EXTENSION, 'HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);',
                 'HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);')),
        ('HoldForSetup following a link',
         altered(texts, EXTENSION, 'HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);',
                 'HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, $02000000, 0);')),
        ('a folder made with no descriptor after its attributes are set',
         altered(texts, EXTENSION, '   attributes.bInheritHandle := 0;\n', '   attributes.bInheritHandle := 0;\n   attributes.lpSecurityDescriptor := 0;\n')),
        ('IsBroadSid taking no group for one of every user',
         altered(texts, EXTENSION, '      if SidIs(pSid, szSid) then\n         Result := true;\n      group := group + 1;', '      if SidIs(pSid, szSid) then\n         Result := false;\n      group := group + 1;')),
        ('Authenticated Users\' SID mistyped',
         altered(texts, EXTENSION, "1: begin Result := 'S-1-5-11'; Name := 'Authenticated Users'; end;", "1: begin Result := 'S-1-5-111'; Name := 'Authenticated Users'; end;")),
        ('SidIs answering true for every SID',
         altered(texts, EXTENSION, '      Result := SidsAreEqual(pSid, pOther);\n', '      Result := true;\n')),
        ('AclLetsOthers reading a callback or object entry as letting nobody',
         altered(texts, EXTENSION, "            else if (aceType = 4) or (aceType = 5) or (aceType = 9) or (aceType = 11) then\n            begin\n               Result := true;\n               Who := 'an account in an entry of a form setup does not read';",
                 "            else if false then\n            begin\n               Result := true;\n               Who := 'an account in an entry of a form setup does not read';")),
        ('AclLetsOthers taking an entry it cannot read for one that lets nobody',
         altered(texts, EXTENSION, "      else\n      begin\n         Result := true;\n         Who := 'an account in an entry that could not be read';\n      end;\n      index := index + 1;\n   end;\nend;\n\n// Whether only SYSTEM",
                 "      else\n      begin\n         Result := false;\n         Who := 'an account in an entry that could not be read';\n      end;\n      index := index + 1;\n   end;\nend;\n\n// Whether only SYSTEM")),
        ('a network drive taken, the test keeping its words',
         altered(texts, EXTENSION, 'if KindOfDrive(szRoot) = HM_DRIVE_REMOTE then', 'if KindOfDrive(szRoot) = HM_DRIVE_REMOTE + 100 then')),
        ('HeldFolderIsHardened\'s test of every other account on a rebuilt folder taken out',
         altered(texts, EXTENSION, '            else if AnyAccount and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED)', '            else if false and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED)')),
        ('a reparse point of any kind gone through',
         altered(texts, EXTENSION, "   else\n   begin\n      Why := 'it is a reparse point of a kind setup does not go through (tag '",
                 "   else if false then\n   begin\n      Why := 'it is a reparse point of a kind setup does not go through (tag '")),
        ('an item of Bin or ControlPanel that cannot be opened taken',
         altered(texts, EXTENSION, "               if handle = -1 then\n                  szWho := 'its owner could not be read (Windows error '",
                 "               if false then\n                  szWho := 'its owner could not be read (Windows error '")),
        ('the walk going on without taking the folder it held for the next one\'s (parent := handle gone)',
         altered(texts, EXTENSION, '                     parent := handle;\n                     szCurrent := szNext;', '                     szCurrent := szNext;')),
        ('the service\'s SID mistyped',
         altered(texts, EXTENSION, "HM_SERVICE_SID              = 'S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809';",
                 "HM_SERVICE_SID              = 'S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046800';")),
        ('the program folder recorded for the uninstaller whatever made it',
         altered(texts, DIRS, 'Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder', 'Name: "{app}"; Flags: uninsalwaysuninstall')),
        ('HardenNewProgramFolder back after Inno Setup\'s folder',
         altered(texts, DIRS, 'Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder', 'Name: "{app}"; AfterInstall: HardenNewProgramFolder')),
        ('Bin recorded for the uninstaller whatever made it',
         altered(texts, DIRS, 'Name: "{app}\\Bin"; Flags: uninsalwaysuninstall; Check: SetupMadeBin', 'Name: "{app}\\Bin"; Flags: uninsalwaysuninstall')),
        ('the folders setup made left after a cancel',
         altered(texts, EXTENSION, '\tif not g_bFilesInstalled then\n\t\tRemoveFoldersMadeHere();\n', '')),
        ('the folders setup made removed with what is in them (DelTree)',
         altered(texts, EXTENSION, 'if RemoveDir(g_aFoldersMade[index]) then', 'if DelTree(g_aFoldersMade[index], True, True, True) then')),
        ('a Bin setup made removed with what is in it',
         altered(texts, EXTENSION, 'if RemoveDir(szMade) then', 'if DelTree(szMade, True, True, True) then')),
        ('a missing program folder decided by its path again, not by the walk',
         altered(texts, EXTENSION, 'if not g_bProgramFolderThere then\n   begin\n      if not MakeFolders(',
                 'if not DirExists(g_szProgramFolderReal) then\n   begin\n      if not MakeFolders(')),
        ('MakeFolders looking at a name again by its path',
         altered(texts, EXTENSION, '         szPath := AddBackslash(szPath) + szName;\n',
                 '         szPath := AddBackslash(szPath) + szName;\n         if DirExists(szPath) then\n            szName := \'\';\n')),
        ('a missing name taken for one the walk held',
         altered(texts, EXTENSION, '               HeldHandle := parent;\n               Whole := false;', '               HeldHandle := parent;\n               Whole := true;')),
        ('walks that disagree taken (a folder or a link made while setup looked)',
         altered(texts, EXTENSION, 'if (not WalksAgree(g_szBinHeld, g_bBinThere))', 'if false and (not WalksAgree(g_szBinHeld, g_bBinThere))')),
        ('a Bin that is not there left to Inno Setup to make',
         altered(texts, EXTENSION, 'end\n   else if not MakeFolders(g_szBinHeld, g_hBin, g_szBinReal, 1, hMade, szWhy) then', 'end\n   else if false then')),
        ('a ControlPanel that is not there left to Inno Setup to make',
         altered(texts, EXTENSION, 'else if ControlPanelIsInstalled() then\n   begin\n      if not MakeFolders(', 'else if false then\n   begin\n      if not MakeFolders(')),
        # R2: hardened before the files, or refused.
        ('every path up to 300 characters taken for a drive\'s root (nothing made or hardened)',
         altered(texts, EXTENSION, '   Result := Length(Path) <= 3;\n', '   Result := Length(Path) <= 300;\n')),
        ('Bin and ControlPanel passed over at a drive\'s root',
         altered(texts, EXTENSION, 'if IsVolumeRoot(g_szProgramFolderReal) then\n      Log(', 'if IsVolumeRoot(g_szProgramFolderReal) then\n      Exit;\n   if false then\n      Log(')),
        ('a volume\'s root reached by its name taken for its device again (its last backslash taken off; the review of 26 October 2026)',
         altered(texts, EXTENSION, '            if CompareText(szCurrent, szRoot) = 0 then\n               Real := szRoot\n            else\n               Real := RemoveBackslashUnlessRoot(szCurrent);',
                 '            Real := RemoveBackslashUnlessRoot(szCurrent);')),
        ('any path under a volume\'s name taken for its root',
         altered(texts, EXTENSION, "         Result := Copy(Path, Length(Path) - 1, 2) = '}\\';", '         Result := true;')),
        ('the program folder\'s owner never asked (every program folder taken for a volume\'s root)',
         altered(texts, EXTENSION, 'if g_bProgramFolderThere and (not IsVolumeRoot(g_szProgramFolderReal)) then', 'if g_bProgramFolderThere and false then')),
        ('an existing Bin never hardened (its condition inverted)',
         altered(texts, EXTENSION, 'if g_bBinThere then\n   begin\n      if FolderHoldsNothing(g_szBinReal)', 'if not g_bBinThere then\n   begin\n      if FolderHoldsNothing(g_szBinReal)')),
        ('an existing ControlPanel never hardened (its condition inverted)',
         altered(texts, EXTENSION, 'if g_bControlPanelThere then\n   begin\n      if FolderHoldsNothing(g_szControlPanelReal)',
                 'if not g_bControlPanelThere then\n   begin\n      if FolderHoldsNothing(g_szControlPanelReal)')),
        ('a Bin that could not be hardened installed into (the result unused)',
         altered(texts, EXTENSION, 'if not HardenHeldFolder(g_szBinReal, g_hBin, g_bEarlierInstallation, szWhy) then',
                 'HardenHeldFolder(g_szBinReal, g_hBin, g_bEarlierInstallation, szWhy);\n      if false then')),
        ('a program folder that could not be hardened installed into',
         altered(texts, EXTENSION, 'else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, g_bEarlierInstallation, szWhy) then',
                 'else if HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, g_bEarlierInstallation, szWhy) and false then')),
        ('the program folder left as it is for any inheriting Data, not only the earlier installation\'s',
         altered(texts, EXTENSION, "if g_bEarlierInstallation then\n         if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data')",
                 "if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data')")),
        ('every program folder taken for the earlier installation\'s',
         altered(texts, EXTENSION, 'g_bEarlierInstallation := CompareText(RemoveBackslashUnlessRoot(Trim(Earlier)), szApp) = 0;', 'g_bEarlierInstallation := true;')),
        ('a held folder\'s owner and permissions read by its path',
         altered(texts, EXTENSION, "readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);\n"
                 "   if readResult <> 0 then\n   begin\n      Why := 'its owner and permissions",
                 "readResult := ReadFolderPermissions(Folder, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);\n"
                 "   if readResult <> 0 then\n   begin\n      Why := 'its owner and permissions")),
        ('an entry of the folder\'s own that lets every user write kept',
         altered(texts, EXTENSION, 'and LetsEveryUserWrite(pAce) then\n               begin\n                  szSid := TextOfSidAt(pAce + 8);',
                 'and false then\n               begin\n                  szSid := TextOfSidAt(pAce + 8);')),
        ('a failure to take such an entry away not a refusal',
         altered(texts, EXTENSION, "if ResultCode <> 0 then\n         begin\n            Why := 'icacls could not take '",
                 "if false then\n         begin\n            Why := 'icacls could not take '")),
        ('a trusted owner\'s folder not stopped inheriting (icacls /inheritance:e)',
         altered(texts, EXTENSION, "/inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers,",
                 "/inheritance:e /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers,")),
        ('icacls\'s failure on the folder not a refusal',
         altered(texts, EXTENSION, "if ResultCode <> 0 then\n      begin\n         Why := 'icacls could not change its permissions",
                 "if false then\n      begin\n         Why := 'icacls could not change its permissions")),
        ('a folder another account owns taken whoever\'s it is (HardenHeldFolder)',
         altered(texts, EXTENSION, 'else if not Adopt then', 'else if false then')),
        ('a folder another account owns taken whoever\'s it is (OwnerIsAllowed)',
         altered(texts, EXTENSION, 'else if g_bEarlierInstallation then\n      Result := true', 'else if true then\n      Result := true')),
        ('the program folder\'s owner not asked',
         altered(texts, EXTENSION, 'if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then', 'if false then')),
        ('a hardened folder not read again',
         altered(texts, EXTENSION, 'if not HeldFolderIsHardened(HeldHandle, not bTrusted, Why) then', 'if false then')),
        ('a list that still inherits taken for hardened',
         altered(texts, EXTENSION, 'else if (control and HM_DACL_PROTECTED) = 0 then', 'else if false then')),
        ('a folder another account owns not given to the Administrators with their privilege (no takeown)',
         altered(texts, EXTENSION, "if not Exec(ExpandConstant('{sys}\\takeown.exe'), '/F \"' + Folder + '\" /A', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then",
                 'if false then')),
        ('a folder another account owns given three entries alone',
         altered(texts, EXTENSION, '      Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, bUsers, Why);',
                 "      szSid := 'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)';")),
        ('a rebuilt folder keeping the entries of its own that let another account write',
         altered(texts, EXTENSION, "else if writes and (not IsTrustedSid(pAce + 8)) then\n                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own'",
                 "else if false then\n                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own'")),
        ('a rebuilt folder keeping every inherited entry that lets another account write, as its own',
         altered(texts, EXTENSION, "else if writes and (not SidIs(pAce + 8, 'S-1-5-18')) and (not SidIs(pAce + 8, 'S-1-5-32-544')) then\n"
                 "                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s inherited';",
                 "else\n                     ok := AddAsOwnEntry(pNew, pAce);")),
        ('the service\'s inherited Read dropped from a rebuilt folder',
         altered(texts, EXTENSION, 'SidIs(pAce + 8, HM_SERVICE_SID) and (not writes)', 'false')),
        ('the deny entries dropped from a rebuilt folder',
         altered(texts, EXTENSION, 'if aceType = HM_ACCESS_DENIED then\n                  ok := AddAsOwnEntry(pNew, pAce);', 'if false then\n                  ok := AddAsOwnEntry(pNew, pAce);')),
        # R3: the hold, the chain and the rule of a link.
        ('the hold without READ_CONTROL',
         altered(texts, EXTENSION, 'HM_HOLD_FOLDER_ACCESS       = $200A1;', 'HM_HOLD_FOLDER_ACCESS       = $A1;')),
        ('a link gone through unasked',
         altered(texts, EXTENSION, 'if not LinkIsAdministratorsAlone(handle, parent, szWhy) then', 'if false then')),
        ('a link\'s permissions read by its path',
         altered(texts, EXTENSION, 'readResult := ReadHeldSecurity(hLink, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);',
                 'readResult := ReadFolderPermissions(Link, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);')),
        ('Bin\'s link not checked',
         altered(texts, EXTENSION, "if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0, g_szBinReal, g_szBinHeld, g_hBin, g_bBinThere, szWhy) then", 'if false then')),
        ('the rights on a link narrowed below the server\'s (no FILE_WRITE_ATTRIBUTES)',
         altered(texts, EXTENSION, 'HM_FOLDER_WRITE_RIGHTS      = $500D0156;', 'HM_FOLDER_WRITE_RIGHTS      = $500D0056;')),
        ('Authenticated Users trusted like the administrators',
         altered(texts, EXTENSION, "Result := SidIs(pSid, 'S-1-5-18') or SidIs(pSid, 'S-1-5-32-544') or SidIs(pSid, HM_TRUSTED_INSTALLER_SID);",
                 "Result := SidIs(pSid, 'S-1-5-18') or SidIs(pSid, 'S-1-5-32-544') or SidIs(pSid, HM_TRUSTED_INSTALLER_SID) or SidIs(pSid, 'S-1-5-11');")),
        ('a link\'s own owner not asked',
         altered(texts, EXTENSION, "   if not IsTrustedSid(pOwner) then\n      Why := 'it is owned by '", "   if false then\n      Why := 'it is owned by '")),
        ('only inherit-only entries on a link weighed (every right on the link itself ignored)',
         altered(texts, EXTENSION, 'if (aceFlags and HM_INHERIT_ONLY) = 0 then', 'if (aceFlags and HM_INHERIT_ONLY) <> 0 then')),
        ('a folder\'s attributes not read again once it is held',
         altered(texts, EXTENSION, '               attributes := FolderAttributes(szNext);\n               if (attributes = HM_INVALID_ATTRIBUTES)',
                 '               if (attributes = HM_INVALID_ATTRIBUTES)')),
        ('a folder that cannot be read taken for one that is not there',
         altered(texts, EXTENSION, 'if (lastError = 2) or (lastError = 3) then', 'if true then')),
        ('a link to a share gone through',
         altered(texts, EXTENSION, "else if CompareText(Copy(szName, 1, 8), '\\??\\UNC\\') = 0 then", "else if CompareText(Copy(szName, 1, 8), '\\??\\UNC\\') = 1 then")),
        ('the folders prepared without the refusal reaching PrepareToInstall\'s answer',
         altered(texts, EXTENSION, '   Result := PrepareProgramFolders(PreviousInstallationFolder());\n', '   PrepareProgramFolders(PreviousInstallationFolder());\n')),
        # The volumes.
        ('a volume without permissions taken (the flag test always true)',
         altered(texts, EXTENSION, 'else if (flags and HM_PERSISTENT_ACLS) = 0 then', 'else if (flags and HM_PERSISTENT_ACLS) < 0 then')),
        ('a volume that cannot be told taken',
         altered(texts, EXTENSION, "   Result := false;\n   Why := '';\n   serial := 0;", "   Result := true;\n   Why := '';\n   serial := 0;")),
        ('Bin\'s volume not asked',
         altered(texts, EXTENSION, 'if not VolumeKeepsPermissions(g_hBin, g_szBinHeld, szWhy) then', 'if false then')),
        # R4: what is in Bin and ControlPanel, and what is said.
        ('a program another account controls in Bin said, not refused',
         altered(texts, EXTENSION, 'if bLink or bCode or bFolder then', 'if false then')),
        ('a link in Bin or ControlPanel not known as one',
         altered(texts, EXTENSION, 'bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0;', 'bLink := false;')),
        ('no owner in Bin or ControlPanel held to the three',
         altered(texts, EXTENSION, 'else if not IsTrustedSid(pOwner) then\n                     begin\n                        if (not bCode)',
                 'else if false then\n                     begin\n                        if (not bCode)')),
        ('what is below the first level of Bin and ControlPanel not looked at',
         altered(texts, EXTENSION, 'if not CheckWhatIsInside(folders[index], Depth + 1, Final, Scripts, Found, Why) then', 'if false then')),
        ('what is in Bin not looked at once it is hardened',
         altered(texts, EXTENSION, '   if g_bBinThere then\n      CheckWhatIsInside(g_szBinReal, 0, true, false, found, szWhy);\n', '')),
        ('what is in Bin not looked at before the service is stopped',
         altered(texts, EXTENSION, '   if g_bBinThere then\n      CheckWhatIsInside(g_szBinReal, 0, false, false, found, szWhy);\n', '')),
        ('a script not counted as a program in the folders that hold what is run (the review of 26 October 2026)',
         altered(texts, EXTENSION, '(IsProgramFile(rec.Name) or (Scripts and IsScriptFile(rec.Name)))', 'IsProgramFile(rec.Name)')),
        ('Addons, Events, DBScripts, WebAdmin and Languages not looked at',
         altered(texts, EXTENSION, 'if not CheckWhatIsInside(szFolder, 0, Final, true, Found, Why) then', 'if false then')),
        ('those folders looked at with scripts not counted',
         altered(texts, EXTENSION, 'CheckWhatIsInside(szFolder, 0, Final, true, Found, Why)', 'CheckWhatIsInside(szFolder, 0, Final, false, Found, Why)')),
        ('the program folder\'s own level and other folders not looked at before the service is stopped',
         altered(texts, EXTENSION, '   if g_bProgramFolderThere then\n      CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy);\n', '')),
        ('the program folder\'s own level and other folders not looked at once it is hardened',
         altered(texts, EXTENSION, '   if g_bProgramFolderThere then\n      CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy);\n', '')),
        ('a link in the program folder that another account controls taken',
         altered(texts, EXTENSION, 'if not LinkIsAdministratorsAlone(handle, -1, szText) then', 'if false then')),
        ('a folder of the program folder another account owns taken, as the service\'s data folder is',
         altered(texts, EXTENSION, "if bData and SidIs(pOwner, HM_SERVICE_SID) then\n                              szWho := ''", "if true then\n                              szWho := ''")),
        ('Events not among the folders that hold what is run',
         altered(texts, EXTENSION, "1: szFolder := 'Events';", "1: szFolder := 'Data';")),
        ('an event script not counted as a script (.vbs)',
         altered(texts, EXTENSION, "(szExtension = '.vbs') or (szExtension = '.js')", "(szExtension = '.js')")),
        ('Events taken for a data folder',
         altered(texts, EXTENSION, "(CompareText(Name, 'Database') = 0);", "(CompareText(Name, 'Database') = 0) or (CompareText(Name, 'Events') = 0);")),
        ('a program, a script or a folder another account controls in the program folder said, not refused',
         altered(texts, EXTENSION, 'if bLink or bCode or (bFolder and (not bData)) then', 'if bLink then')),
        ('the folder of every link asked about as the program folder\'s',
         altered(texts, EXTENSION, '   if hParent = -1 then\n      Exit;\n', '   if hParent <> -2 then\n      Exit;\n')),
        ('a program an entry of whose own lets another account change it taken',
         altered(texts, EXTENSION, 'if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szText) then', 'if false then')),
        ('a library not taken for a program',
         altered(texts, EXTENSION, "(szExtension = '.exe') or (szExtension = '.dll')", "(szExtension = '.exe')")),
        ('deny entries said instead of the allow entries kept that let another account write',
         altered(texts, EXTENSION, '((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and\n',
                 '((head.TypeFlagsSize and $FF) = HM_ACCESS_DENIED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and\n')),
        ('the entries kept said for the groups of every user alone',
         altered(texts, EXTENSION, "(not IsTrustedSid(pAce + 8)) and\n               (not SidIs(pAce + 8, 'S-1-3-0'))",
                 "IsBroadSid(pAce + 8) and\n               (not SidIs(pAce + 8, 'S-1-3-0'))")),
        ('the entries kept on a folder the administrators own not said',
         altered(texts, EXTENSION, '      SayWriteEntriesKept(Folder, HeldHandle)\n', '      Log(Folder)\n')),
        # The service.
        ('the folders checked after the service is stopped',
         altered(texts, EXTENSION, "\t\tg_szFolderRefusal := CheckProgramFolders(PreviousInstallationFolder());\n\t\tif g_szFolderRefusal <> '' then\n\t\t\tExit;\n", '')),
        ('whether setup stopped the service cleared by a later click on the Ready page (the review of 26 October 2026)',
         altered(texts, EXTENSION, "\t\tif bWasRunning then\n\t\t\tg_bServiceStoppedForSetup := true;\n\t\tif (StopHMailServerService() = false) then\n\t\t\tResult := false;\n",
                 "\t\tif (StopHMailServerService() = false) then\n\t\t\tResult := false\n\t\telse\n\t\t\tg_bServiceStoppedForSetup := bWasRunning;\n")),
        ('whether setup stopped the service cleared elsewhere',
         altered(texts, EXTENSION, "   Result := PrepareToInstallRefusal();\n", "   Result := PrepareToInstallRefusal();\n   g_bServiceStoppedForSetup := false;\n")),
        ('a refusal after the service was stopped not saying so',
         altered(texts, EXTENSION, "      if g_bServiceStoppedForSetup then\n         Result := Result + ' Setup stopped",
                 "      if false then\n         Result := Result + ' Setup stopped")),
        # The masks.
        ('the read mask without GENERIC_READ (the program folder made in Program Files with no entry for Users)',
         altered(texts, EXTENSION, 'HM_FOLDER_READ_RIGHTS       = $90000001;', 'HM_FOLDER_READ_RIGHTS       = $10000001;')),
        ('an entry\'s rights tested against a literal mask in parentheses',
         altered(texts, EXTENSION, '((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and\n',
                 '((head.Mask and ($000D0156)) <> 0) and (not IsTrustedSid(pAce + 8)) and\n')),
        ('the link rule asked about a narrowed mask',
         altered(texts, EXTENSION, 'AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho)', 'AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS and $FFFFFEFF, true, szWho)')),
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
          'administrators control is the only one gone through, they are made or hardened before the files or refused, '
          'what is in Bin and ControlPanel is held to the live update\'s rule, and each decision is held to its statement; '
          '%d negative controls refused' % (entries, controls))
    return 0


if __name__ == '__main__':
    sys.exit(main())
