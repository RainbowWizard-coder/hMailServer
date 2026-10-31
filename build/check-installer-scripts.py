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
               keeps no permissions or cannot be told, and a program folder that
               is the root of a volume, or whose Bin or ControlPanel leads to one;
               refuses one of the three another account owns unless it is the
               earlier installation's; refuses, before anything else, an {app} setup
               has recorded since Windows started not to install into before a
               restart (RestartNeededFor, a volatile key); decides, before anything
               changes, that a Bin or ControlPanel there that holds nothing is made
               again; and refuses
               a link, or a program, a library, a settings file (IsSettingsFile:
               hMailServer.INI, the key store, which may be the service's, a
               .config, a .json, unins000.dat) or a folder another account
               controls, in Bin and ControlPanel at any depth, and on the program
               folder's own level and in Addons, Events, DBScripts, WebAdmin and
               Languages, scripts counted as programs there, a link at any of
               those five whoever controls it, and a data folder's link whose way
               does not pass the program folder's rules or leads where the grant
               after the files would refuse it, or a Database link to a folder whose
               owner is not the three or the service or that a group of every user
               may write (DataLinkLeadsWhereAllowed) (R4). And it notes, of the
               program folder, Bin and ControlPanel that are there and not made
               again and of the five folders, each another account could change -
               an owner not the three, an entry that lets another account change
               its permissions or owner, or one that lets it write a folder that
               holds anything but empty folders (NoteFoldersOthersCouldChange) - which
               CheckFoldersBeforeTheStop, its one caller, then hardens and refuses
               until Windows has restarted, recorded in the volatile key
               (HardenForRestart, the one place a folder another account owns is
               taken); a refusal whose remedy ends in a restart is recorded too
               (check 4 of R1 to R4, 27 October 2026: a handle opened while another
               account could change a folder keeps that right after the hardening).
               PrepareProgramFolders, the last step of PrepareToInstall, then makes
               a program folder that is not there with NtCreateFile, by each name
               relative to the handle of the folder above it, and a protected
               descriptor - exactly the names below the deepest folder
               held, that folder read again through its handle and each folder made
               read where it is (HeldIsLink, MadeWhereNamed) - and section_dirs.iss
               gives the uninstaller each, when this setup made it - recorded only
               once found where it was named, with where that is - while
               DeinitializeSetup removes those still empty and still there when
               the files were not installed (RemoveMadeFolder), before it lets the
               held folders go (R1); hardens one that is there whatever it holds,
               but for the earlier installation's whose data folders inherit,
               through the held handle, an entry of its own that lets every user
               write taken away, one another account owns given to the
               Administrators with takeown /A and rebuilt, each read again, any
               failure a refusal, and gives each folder it hardened in place a file
               of setup's held until the files are in (MakePlaceholder), so that it
               cannot be made a junction; then makes Bin and ControlPanel again when
               they held nothing, or refuses one made a link since and hardens it,
               or makes them - in a program folder setup made too - reads the three
               again for a link, removes the empty folders in them through handles
               read as those folders (RemoveHeldFolder), and looks at what is in them
               again, walking only folders opened as themselves where their parents
               say (R2, R4); a folder found changed there sets the service not to
               start and the line hMailServer.Updater reads (FoldersChangedLine); and
               after the files, before setup runs any program of the
               installation, reads the three again through the handles that hold
               them (FoldersStillAsPrepared), walking only what was found as setup
               left it and removing nothing, any change failing the installation
               with exit code 12, keeping the service from starting and the last
               page from running the Control Panel (section_run.iss,
               FoldersWereAsPrepared) - and hMailServer.Updater puts no earlier
               version back over such folders, on 12 or on a refusal whose log has
               FoldersChangedLine, in either of its modes (check_updater). Each
               decision is held to its
               statement (PROGRAM_FOLDER_STATEMENTS), the helpers it relies on
               among them (SidIs, IsBroadSid, BroadGroupSid, AclLetsOthers,
               HoldForSetup and the uninstaller's records whole, the walk's start,
               names and way on after a link, where a link leads, Users' Read on a
               folder made), so that a one-line change of one is refused;
               HM_SERVICE_SID is the SID ServiceAccountTester holds the service's
               to, and a folder is made with its descriptor set once. And R1 to R4
               are held whole (check 3 of R1 to R4, 27 October 2026, whose twenty
               one-line changes of constants and helpers the checker had passed
               nineteen times): every HM_ constant to its value, defined once
               (PROGRAM_FOLDER_CONSTANTS), HM_TRUSTED_INSTALLER_SID to the live
               update's TrustedInstallerSid; every Windows function and record they
               use to its declaration (PROGRAM_FOLDER_DECLARATIONS); and every
               routine their entry points reach (PROGRAM_FOLDER_ENTRY_POINTS), but
               the two the rest of setup shares (PROGRAM_FOLDER_SHARED), to a digest
               of its code - comments dropped, white space one space, every string
               as written (PROGRAM_FOLDER_CODE; --digests prints them as the tree
               is) - so that no change of one passes until it has been reviewed and
               its digest written here and in InstallerPackaging.cs. The shared
               IsServiceInstalled, and the service routines it and the Ready page's
               stop ask through (OpenServiceManager, OpenNamedService,
               CloseServiceAndManager, GetServiceState, IsServiceRunning,
               IsServiceStopped), are held by statements whole, and the Service
               Control Manager's functions, record and constants they use among the
               declarations (check 9 of R1 to R4, 31 October 2026). PrepareToInstallRefusal,
               which the rest of setup shares, leaves before the folders are prepared
               only with a refusal.
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
  preprocessor ISCC compiles what the Inno Setup preprocessor makes of the scripts,
               which the rules above read as written: every line ISPP reads - one
               whose first character that is not white space is '#' (#if, #ifdef,
               #else, #endif, #define, #emit, #include, #pragma, #sub, #for...) or
               one with an inline directive, {#...}, which ISPP replaces with the
               text of its expression wherever it stands, a comment or a string
               among them - is held, in every installer script, to where it is now:
               the script, the routine of the [Code] or else the section it is in,
               the line, the lines that are not blank before and after it, and how
               many times (ISPP_PLACES; --preprocessor prints the table as the tree
               is). The digests' reading of a routine takes {#...} for code, not a
               comment. Check 6 of R1 to R4 (28 October 2026) found six one-line
               inline directives that switched R1, R2 or R3 off, one in
               CurStepChanged, and #if 0 around section_dirs.iss's {app} entry,
               each passed and each compiled by ISCC 6.7.

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
the sixteen one-line changes of the review of 26 October 2026 to the helpers) and of
its third (an empty Bin or ControlPanel made again only when read, after the program
folder's hardening, to let another account write it, or decided after it, a Bin or
ControlPanel made a link before its hardening or the last look taken, a settings file,
the key store or the uninstaller's record another account controls only said, a link
at the five folders setup installs into gone through, a data folder's link taken
unwalked, a volume's root installed into, a folder made recorded before it was found
where it was named or removed by a path whatever it leads to, or after its holds are
let go, and the last page run whatever was found) and the twenty changes of the
constants and helpers check 3 of R1 to R4 made, twenty-five of the third repair's code,
a way out of PrepareToInstallRefusal without a refusal, every held constant, declaration
and routine changed, one more constant, one more Windows function and one more helper
not held, and a routine defined twice, and of its fourth (a folder another account
could change - its permissions, or what is in it - hardened and installed into with no
restart, the restart not recorded, or recorded where a restart does not take it away,
or not read, the program folder or the five folders not asked, a folder that holds a
file asked as an empty one, a refusal whose remedy is a restart not recorded, the Ready
page checking without hardening, a Bin another account owns taken where no restart
follows, no file kept in a folder hardened in place, or one shared for deleting, taken
where a link put it, or not let go, a link found after the files removed by its path,
an empty folder removed by its path or through a handle not the folder of its name, a
folder walked as a link or elsewhere, Bin or the five folders walked when not as setup
left them, a folder setup made held to a hardened one's rule, folders found changed
ending in 10, the service left to start or set not to start for good, the Updater not
told, or putting the earlier version back over them in one mode, waiting for the
service, or looking for another line than the installer writes, something new in the last look not taken for a change, a data folder's link held
only to the way there, a Database link's target every user may write taken, and Bin
not made in a new program folder) and of its sixth (a folder whose permissions another
account could change hardened keeping its entries, the earlier installation's program
folder left unhardened before the files asked as one holding only empty folders, a group
of every user left out, and the preprocessor's inline directives and lines of check 6's
mutations in held routines, comments, strings, [Dirs], PrepareToInstallRefusal and the
x64 installer) - must all be refused.

Usage: python build/check-installer-scripts.py [--digests | --preprocessor]
Exits 1 listing what is wrong. --digests prints PROGRAM_FOLDER_CODE as the tree is, and
--preprocessor ISPP_PLACES.
"""
import functools
import hashlib
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
# hMailServer.Updater, which runs the installer in a live update and decides whether the
# earlier version is put back (work item 78: never over folders the installer found
# changed).
UPDATER = 'hMailServer.Updater/hMailServer.Updater.cpp'
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
    texts[UPDATER] = read(os.path.join(SERVER, UPDATER))
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
                           'FoldersStillAsPrepared', 'CurStepChanged', 'KeepServiceFromStarting', 'LinkRemedy',
                           'SidIs', 'IsBroadSid', 'BroadGroupSid', 'RemoveMadeFolder', 'PreparedFolderIsLink', 'IsSettingsFile', 'IsRunFolderName',
                           'DataLinkLeadsWhereAllowed', 'FoldersWereAsPrepared',
                           'ProgramFolderMadeAsNamed', 'MadeParentCount', 'MadeParentOffset', 'ReleaseHeldFolders', 'ControlPanelIsInstalled',
                           'HoldsOnlyEmptyFolders', 'FinalPathOf', 'RemoveHeldFolder', 'MakePlaceholder', 'ReleasePlaceholders', 'IsAtOrBelow', 'ProfilesFolder',
                           'DataLinkTargetIsGrantable', 'AclLetsEveryUserWrite', 'DatabaseTargetIsTheAdministrators', 'RestartNeededFor', 'RecordRestartNeeded',
                           'OthersCouldChange', 'NoteFolderOthersCouldChange', 'NoteFoldersOthersCouldChange', 'HardenForRestart', 'CheckFoldersBeforeTheStop',
                           'FoldersChangedLine', 'RestoreServiceStart', 'GetCustomSetupExitCode', 'IsServiceInstalled', 'OpenNamedService', 'CloseServiceAndManager',
                           'OpenServiceManager', 'GetServiceState', 'IsServiceRunning', 'IsServiceStopped',
                           'DataFolderIsAllowed', 'DatabaseFilesWhy', 'HardenDataFolder', 'AddRestartFolder', 'StaleEntryLetsOthers', 'HandsDownWriteTo',
                           'NoteStaleItem', 'StaleItemIn', 'TakenEntriesLetOthers', 'RestartRecordedForWalked', 'WalkedFinal', 'MakeFolderIn',
                           'HardenDistrustedFolder', 'ProgramFolderWaitsForTheFiles', 'RelativeLinkTarget', 'RemoveAndMakeAgain', 'WriteOwnEntriesOn')

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
     "g_szFolderAboveChecked := ''; g_hProgramFolderPrepared := -1; g_hBinPrepared := -1; g_hControlPanelPrepared := -1; g_bProgramFolderHardened := false; @@szWhy := RemoveBackslashUnlessRoot(ExpandFileName(szApp)); if CompareText(szWhy, szApp) <> 0 then begin Result := ~; Exit; end; CheckProgramFolderPermissions(szApp, false); @@if RestartNeededFor(szApp, szWhy) then begin Result := ~; Exit; end; if not HoldFolderChain(szApp, 0,"),
    ('CheckProgramFolders', 'Bin\'s and ControlPanel\'s walks are not held to the rule past what the program folder\'s walk held',
     "g_szFolderAboveChecked := g_szProgramFolderReal; if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0,"),
    ('SidIs', 'a SID is taken for another, so that every owner is trusted or every group of every user is not one (the review of 26 October 2026)',
     "^function SidIs(pSid: cardinal; szSid: String): Boolean; var pOther: cardinal; begin Result := false; if pSid = 0 then Exit; pOther := 0; if SidOfText(szSid, pOther) then begin Result := SidsAreEqual(pSid, pOther); FreeWindowsMemory(pOther); end; end;$"),
    ('IsBroadSid', 'a group of every user - a domain\'s Domain Users and Domain Guests among them, as ServiceAccount.cpp\'s IsBroad_ counts them (check 6 of R1 to R4, 28 October 2026) - is not taken for one, so that an entry that lets every user write is kept',
     "^function IsBroadSid(pSid: cardinal): Boolean; var szSid: String; szName: String; group: Integer; begin Result := false; group := 0; szSid := BroadGroupSid(group, szName); while (not Result) and (szSid <> '') do begin if SidIs(pSid, szSid) then Result := true; group := group + 1; szSid := BroadGroupSid(group, szName); end; "
     "if not Result then begin szSid := TextOfSidAt(pSid); Result := (Length(szSid) > 13) and (CompareText(Copy(szSid, 1, 9), 'S-1-5-21-') = 0) and "
     "((Copy(szSid, Length(szSid) - 3, 4) = '-513') or (Copy(szSid, Length(szSid) - 3, 4) = '-514')); end; end;$"),
    ('BroadGroupSid', 'a group of every user is left out, or its SID mistyped - each of ServiceAccount.cpp\'s BroadSids_, eight of which were missing before check 5 of R1 to R4 (27 October 2026), and four more before check 6 (28 October 2026)',
     "^function BroadGroupSid(Index: Integer; var Name: String): String; begin Result := ''; Name := ''; case Index of 0: begin Result := 'S-1-1-0'; Name := 'Everyone'; end; 1: begin Result := 'S-1-5-11'; Name := 'Authenticated Users'; end; 2: begin Result := 'S-1-5-32-545'; Name := 'Users'; end; 3: begin Result := 'S-1-5-4'; Name := 'INTERACTIVE'; end; 4: begin Result := 'S-1-2-0'; Name := 'LOCAL'; end; 5: begin Result := 'S-1-5-32-546'; Name := 'Guests'; end; 6: begin Result := 'S-1-5-32-547'; Name := 'Power Users'; end; 7: begin Result := 'S-1-5-32-555'; Name := 'Remote Desktop Users'; end; 8: begin Result := 'S-1-5-14'; Name := 'REMOTE INTERACTIVE LOGON'; end; 9: begin Result := 'S-1-5-13'; Name := 'TERMINAL SERVER USER'; end; "
     "10: begin Result := 'S-1-2-1'; Name := 'CONSOLE LOGON'; end; 11: begin Result := 'S-1-5-2'; Name := 'NETWORK'; end; 12: begin Result := 'S-1-5-3'; Name := 'BATCH'; end; 13: begin Result := 'S-1-5-6'; Name := 'SERVICE'; end; 14: begin Result := 'S-1-5-7'; Name := 'ANONYMOUS LOGON'; end; "
     "15: begin Result := 'S-1-15-2-1'; Name := 'ALL APPLICATION PACKAGES'; end; 16: begin Result := 'S-1-15-2-2'; Name := 'ALL RESTRICTED APPLICATION PACKAGES'; end; "
     "17: begin Result := 'S-1-5-15'; Name := 'This Organization'; end; 18: begin Result := 'S-1-5-113'; Name := 'Local account'; end; "
     "19: begin Result := 'S-1-18-1'; Name := 'Authentication authority asserted identity'; end; 20: begin Result := 'S-1-5-80-0'; Name := 'ALL SERVICES'; end; end; end;$"),
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
     "else if AnyAccount and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and "
     "(not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) then begin Result := false;"),
    ('MakeFolderIn', 'a folder is made without its descriptor, or relative to another handle than the folder above it',
     "attributes.nLength := HM_OBJECT_ATTRIBUTES_SIZE; attributes.RootDirectory := Parent; attributes.ObjectName := pName; attributes.Attributes := HM_OBJ_CASE_INSENSITIVE; "
     "attributes.SecurityDescriptor := pDescriptor; attributes.SecurityQualityOfService := 0;"),
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
     "if g_bProgramFolderThere then if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'a program folder that is the root of a volume is installed into, the uninstaller and its record given what the volume hands down (check 3 of R1 to R4, 27 October 2026)',
     "if IsVolumeRoot(g_szProgramFolderReal) then begin Result := ~; Exit; end;"),
    ('CheckProgramFolders', 'a Bin or a ControlPanel that leads to the root of a volume is installed into',
     "if IsVolumeRoot(g_szBinReal) or IsVolumeRoot(g_szControlPanelReal) then begin Result := ~; Exit; end;"),
    ('CheckProgramFolders', 'whether Bin and ControlPanel are made again is not decided before anything changes, every empty one, but after the program folder\'s hardening has handed its permissions down to them (check 3 of R1 to R4, 27 October 2026)',
     "g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal); g_bRemakeControlPanel := g_bControlPanelThere and FolderHoldsNothing(g_szControlPanelReal);"),
    ('CheckProgramFolders', 'whether the program folder waits for the files is not decided once, before anything changes, with the decisions of Bin and ControlPanel (check 7 of R1 to R4, 28 October 2026)',
     "g_bRemakeControlPanel := g_bControlPanelThere and FolderHoldsNothing(g_szControlPanelReal); @@if g_bProgramFolderThere then g_szProgramFolderWaits := ProgramFolderWaitsForTheFiles(); @@if g_bProgramFolderThere then if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then"),
    ('CheckProgramFolders', 'what an earlier pass made is removed once its holds are let go, by a path nothing holds any more, or what it decided is kept',
     "RemoveFoldersMadeHere(); ReleaseHeldFolders(); g_szFolderFindings := ''; g_bRemakeBin := false; g_bRemakeControlPanel := false; g_szProgramFolderWaits := '';"),
    ('CheckProgramFolders', 'Bin\'s owner is not held to the rule',
     "if g_bBinThere then if not OwnerIsAllowed(g_szBinReal, g_hBin, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'ControlPanel\'s owner is not held to the rule',
     "if g_bControlPanelThere then if not OwnerIsAllowed(g_szControlPanelReal, g_hControlPanel, szWhy) then begin Result := "),
    ('CheckProgramFolders', 'what is in Bin and ControlPanel is not refused before the service is stopped',
     "if g_bBinThere then CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBin), 0, false, false, found, szWhy); if g_bControlPanelThere then CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanel), 0, false, false, found, szWhy); if g_bProgramFolderThere then CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy); if found > 0 then begin Result := "),
    ('OwnerIsAllowed', 'a folder another account owns is taken although it is not the earlier installation\'s',
     "else if g_bEarlierInstallation then Result := true else Why := "),
    ('WalksAgree', 'what one walk found is not held to the others',
     "if g_bProgramFolderThere then Result := Whole or (CompareText(Held, g_szProgramFolderReal) = 0) else "
     "Result := (not Whole) and (CompareText(Held, g_szProgramFolderHeld) = 0);"),
    # R1: made.
    ('MakeFolders', 'a folder is made by its path, through any link put on the way since the walk, or relative to another folder than the one made or held above it (check 5 of R1 to R4, 27 October 2026), or one somebody else made in the meantime is taken',
     "status := 0; handle := MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status); if handle <> -1 then begin if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then"),
    ('MakeFolders', 'a name somebody else took is not refused, a folder above made a link is not taken for a folder changed while setup prepared it, or another failure is taken',
     "if status = HM_STATUS_NAME_COLLISION then begin g_bFoldersChanged := true; Why := ~; end else if HeldIsLink(parent, bKnown) then begin g_bFoldersChanged := true; Why := ~; end else Why := ~; Result := false;"),
    ('MakeFolderIn', 'a folder is made by a path, through a link, shared for deleting, or opened when it is there rather than made (check 5 of R1 to R4, 27 October 2026)',
     "Status := MakeFileRelative(handle, HM_MAKE_FOLDER_ACCESS, attributes, io, 0, 0, HM_SHARE_ALL_BUT_DELETE, HM_FILE_CREATE, HM_MAKE_FOLDER_OPTIONS, 0, 0); FreeWindowsMemory(pName); if Status <> 0 then Exit;"),
    ('MakeFolderIn', 'the name made is not the one name given, relative to the folder above it, with its length',
     "CopyTextTo(pName + 8, Name, bytes); nameRecord.Lengths := bytes or ((bytes + 2) shl 16); nameRecord.Buffer := pName + 8; WriteNameRecord(pName, nameRecord, 8);"),
    ('MakeFolderIn', 'a folder made is not held until setup ends',
     "Result := handle; SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1); g_aHeldFolders[GetArrayLength(g_aHeldFolders) - 1] := Result; end;$"),
    ('MakeFolderIn', 'a failure to make room for the name is taken for a folder made',
     "Result := -1; Status := HM_STATUS_NO_MEMORY; bytes := Length(Name) * 2; pName := AllocateWindowsMemory(HM_LPTR, 8 + bytes + 2); if pName = 0 then Exit;"),
    ('CheckProgramFolders', 'a program folder path Windows reads as another - a name ending in a dot or a space, . or .. - is held as given, so that setup prepares a folder no path reaches and Inno Setup installs into the one Windows reads, made with what the folder above hands down (check 9 of R1 to R4, 31 October 2026)',
     'szWhy := RemoveBackslashUnlessRoot(ExpandFileName(szApp)); if CompareText(szWhy, szApp) <> 0 then begin Result := ~; Exit; end; CheckProgramFolderPermissions(szApp, false);'),
    ('MakeFolders', 'a name that ends in a dot or a space, which Windows drops from a path, is made by its name relative to the folder above, where no path reaches it (check 9 of R1 to R4, 31 October 2026)',
     "if (szName <> '') and ((Copy(szName, Length(szName), 1) = '.') or (Copy(szName, Length(szName), 1) = ' ')) then begin Why := ~; Exit; end; if szName <> '' then begin szPath := AddBackslash(szPath) + szName;"),
    ('MakeFolders', 'what is made is not the names below the deepest folder held',
     "szBase := AddBackslash(Held); if (Length(Folder) <= Length(szBase)) or (CompareText(Copy(Folder, 1, Length(szBase)), szBase) <> 0) then"),
    ('MakeFolders', 'the folder the walk held is not read again before a folder is made in it, so a link put in its place since is gone through (the review of 26 October 2026)',
     "Result := true; if HeldIsLink(HeldHandle, bKnown) then begin g_bFoldersChanged := true; if bKnown then Why := ~ else Why := ~; Result := false; end; index := 0; parent := HeldHandle; while Result and (index < count) do"),
    ('MakeFolders', 'a folder made is not held and read where it is through its handle, so one made through a link put on the way is taken',
     "if handle <> -1 then begin if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then begin g_bFoldersChanged := true; Why := ~; Log(~); Result := false; end else begin @@szWhere := FinalPathOfHeld(handle); if Kind = 0 then"),
    ('MakeFolders', 'a folder made is recorded without where it was made and the handle that holds it, so that it is removed by a path that may lead elsewhere by then (check 3 of R1 to R4, 27 October 2026)',
     "SetArrayLength(g_aFoldersMade, GetArrayLength(g_aFoldersMade) + 1); g_aFoldersMade[GetArrayLength(g_aFoldersMade) - 1] := missing[index]; "
     "SetArrayLength(g_aFoldersMadeAt, GetArrayLength(g_aFoldersMade)); g_aFoldersMadeAt[GetArrayLength(g_aFoldersMade) - 1] := szWhere; "
     "SetArrayLength(g_aFoldersMadeHandle, GetArrayLength(g_aFoldersMade)); g_aFoldersMadeHandle[GetArrayLength(g_aFoldersMade) - 1] := handle; end "
     "else if Kind = 1 then begin g_szBinMade := missing[index]; g_szBinMadeAt := szWhere; g_hBinMade := handle; end "
     "else begin g_szControlPanelMade := missing[index]; g_szControlPanelMadeAt := szWhere; g_hControlPanelMade := handle; end; Log(~); parent := handle; MadeHandle := handle; end;"),
    ('MadeWhereNamed', 'a folder made is taken wherever it is, or where it is when that cannot be read',
     "if (szParent = '') or (szMade = '') then Where := ~ else if CompareText(szMade, AddBackslash(szParent) + Name) <> 0 then Where := ~ else Result := true;"),
    ('MadeWhereNamed', 'where a folder made is is not read through the handles',
     "Result := false; szParent := FinalPathOfHeld(Parent); szMade := FinalPathOfHeld(Made);"),
    ('HeldIsLink', 'a held folder that is a link now, or whose attributes cannot be read, is taken for a folder',
     "^function HeldIsLink(HeldHandle: Integer; var Known: Boolean): Boolean; var information: HM_FILE_INFORMATION; begin Result := true; "
     "Known := ReadHeldInformation(HeldHandle, information); if Known then Result := (information.dwFileAttributes and HM_REPARSE_POINT) <> 0; end;$"),
    ('FinalPathOfHeld', 'where a held folder is is not read through its handle, by its volume\'s name',
     "size := FinalPathOfHandle(HeldHandle, buffer, capacity, HM_VOLUME_NAME_GUID); if size >= capacity then begin"),
    ('RemoveMadeFolder', 'a folder setup made is removed while it is still held, with what is in it, or by its path whatever that leads to now (check 3 of R1 to R4, 27 October 2026)',
     "LetGoOfHeld(Handle); check := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if check = -1 then begin Log(~); Exit; end; "
     "bSame := (not HeldIsLink(check, bKnown)) and (At <> '') and (CompareText(FinalPathOfHeld(check), At) = 0); ReleaseFolder(check); "
     "if not bSame then Log(~) else if RemoveDir(Folder) then Log(~) else Log(~);"),
    ('RemoveFoldersMadeHere', 'a Bin or ControlPanel setup made is not removed where it was made, before the folders above it',
     "if g_szBinMade <> '' then RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade); if g_szControlPanelMade <> '' then RemoveMadeFolder(g_szControlPanelMade, g_szControlPanelMadeAt, g_hControlPanelMade);"),
    ('RemoveFoldersMadeHere', 'the folders setup made above and at the program folder are not removed deepest first, each where it was made',
     "for index := GetArrayLength(g_aFoldersMade) - 1 downto 0 do RemoveMadeFolder(g_aFoldersMade[index], g_aFoldersMadeAt[index], g_aFoldersMadeHandle[index]);"),
    # R2: hardened, and refused when it cannot be.
    ('PrepareProgramFolders', 'a program folder that is not there is decided by its path',
     "if not g_bProgramFolderThere then begin if not MakeFolders(g_szProgramFolderHeld, g_hProgramFolder, g_szProgramFolderReal, 0, hMade, szWhy) then begin Result := ~; Exit; end; g_hProgramFolderPrepared := hMade; g_bProgramFolderHardened := true;"),
    ('PrepareProgramFolders', 'the program folder is left as it is by a rule other than the one the restart rule asks (ProgramFolderWaitsForTheFiles), or what an entry of its own lets a named account do is not said then (check 6 of R1 to R4, 28 October 2026)',
     "szInherits := g_szProgramFolderWaits; if CompareText(ProgramFolderWaitsForTheFiles(), szInherits) <> 0 then begin g_bFoldersChanged := true; Result := ~; Exit; end; "
     "if szInherits <> '' then begin Log(~); SayWriteEntriesKept(g_szProgramFolderReal, g_hProgramFolder); end else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) then"),
    ('PrepareProgramFolders', 'the program folder is left unhardened before the files by an answer asked after the service stop, not the one the restart rule went by, or one that differs from it is taken (check 7 of R1 to R4, 28 October 2026)',
     "szInherits := g_szProgramFolderWaits; if CompareText(ProgramFolderWaitsForTheFiles(), szInherits) <> 0 then begin g_bFoldersChanged := true; Result := 'Setup does not install into ' + szApp + ': a data folder of the earlier installation in ' + g_szProgramFolderReal + ~restarts Windows~; Exit; end;"),
    ('ProgramFolderWaitsForTheFiles', 'the program folder is left as it is for an inheriting data folder that is not the earlier installation\'s',
     "^function ProgramFolderWaitsForTheFiles(): String; begin Result := ''; if g_bEarlierInstallation then if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data') or "
     "FileExists(AddBackslash(g_szBinReal) + 'hMailServer.INI') then Result := DataFolderThatInherits(g_szProgramFolderReal); end;$"),
    ('PrepareProgramFolders', 'a program folder that could not be hardened, or given the file setup keeps in it, is installed into, or one another account owns is taken',
     "else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) then begin Result := ~; Exit; end else if not MakePlaceholder(g_szProgramFolderReal, g_hProgramFolder, szWhy) then begin Result := ~; Exit; end else g_bProgramFolderHardened := true; g_hProgramFolderPrepared := g_hProgramFolder;"),
    ('PrepareProgramFolders', 'a Bin that is there and held nothing is not made again, one made a link since the walk is hardened through its path, one that could not be hardened or given the file setup keeps in it is installed into, or one another account owns is taken (check 3 and check 4 of R1 to R4, 27 October 2026)',
     "if g_bBinThere then begin if g_bRemakeBin then begin if not RemakeEmptyFolder(g_szBinReal, g_hBin, 1, hMade, szWhy) then begin Result := ~; Exit; end; g_hBinPrepared := hMade; end else if HeldIsLink(g_hBin, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szBinReal); Exit; end else if not HardenHeldFolder(g_szBinReal, g_hBin, false, szWhy) then begin Result := ~; Exit; end else if not MakePlaceholder(g_szBinReal, g_hBin, szWhy) then begin Result := ~; Exit; end else g_hBinPrepared := g_hBin; end"),
    ('PrepareProgramFolders', 'a Bin that is not there is not made by setup',
     "end else if not MakeFolders(g_szBinHeld, g_hBin, g_szBinReal, 1, hMade, szWhy) then begin Result := ~; Exit; end else g_hBinPrepared := hMade;"),
    ('PrepareProgramFolders', 'a ControlPanel that is there and held nothing is not made again, one made a link since the walk is hardened through its path, one that could not be hardened or given the file setup keeps in it is installed into, or one another account owns is taken',
     "if g_bControlPanelThere then begin if g_bRemakeControlPanel then begin if not RemakeEmptyFolder(g_szControlPanelReal, g_hControlPanel, 2, hMade, szWhy) then begin Result := ~; Exit; end; g_hControlPanelPrepared := hMade; end else if HeldIsLink(g_hControlPanel, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szControlPanelReal); Exit; end else if not HardenHeldFolder(g_szControlPanelReal, g_hControlPanel, false, szWhy) then begin Result := ~; Exit; end else if not MakePlaceholder(g_szControlPanelReal, g_hControlPanel, szWhy) then begin Result := ~; Exit; end else g_hControlPanelPrepared := g_hControlPanel; end"),
    ('PrepareProgramFolders', 'the program folder, Bin or ControlPanel made a link before the last look, which goes by their paths, is installed into',
     "if g_hProgramFolderPrepared <> -1 then if HeldIsLink(g_hProgramFolderPrepared, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szProgramFolderReal); Exit; end; "
     "if g_hBinPrepared <> -1 then if HeldIsLink(g_hBinPrepared, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szBinReal); Exit; end; "
     "if g_hControlPanelPrepared <> -1 then if HeldIsLink(g_hControlPanelPrepared, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szControlPanelReal); Exit; end;"),
    ('PrepareProgramFolders', 'a ControlPanel that is not there is not made by setup',
     "end else if ControlPanelIsInstalled() then begin if not MakeFolders(g_szControlPanelHeld, g_hControlPanel, g_szControlPanelReal, 2, hMade, szWhy) then begin Result := ~; Exit; end; "
     "g_hControlPanelPrepared := hMade; end;"),
    ('PrepareProgramFolders', 'what is in Bin and ControlPanel is not looked at again once they are hardened, from where their handles say they are, or what is refused then is not taken for a folder changed while setup prepared it',
     "if g_bBinThere then CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true, false, found, szWhy); if g_bControlPanelThere then CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, true, false, found, szWhy); if g_bProgramFolderThere then CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy); if found > 0 then begin g_bFoldersChanged := true; Result := ~; Exit; end;"),
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
     "Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, bUsers, false, Why);"),
    ('HardenHeldFolder', 'a hardened folder is not read again',
     "if not HeldFolderIsHardened(HeldHandle, not bTrusted, false, Why) then begin Why := ~; Exit; end; if bTrusted then SayWriteEntriesKept(Folder, HeldHandle) else begin"),
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
     "else if writes and (not IsTrustedSid(pAce + 8)) and (not (Service and SidIs(pAce + 8, HM_SERVICE_SID))) then szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own' "
     "else ok := AddAsOwnEntry(pNew, pAce);"),
    ('RebuildFolderOfUntrustedOwner', 'a rebuilt folder keeps an inherited entry other than the service\'s Read',
     "if aceType <> HM_ACCESS_ALLOWED then szDropped := szDropped + ', an inherited entry of another kind' "
     "else if SidIs(pAce + 8, HM_SERVICE_SID) and (Service or (not writes)) then ok := AddAsOwnEntry(pNew, pAce) "
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
     "bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0; bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts and IsScriptFile(rec.Name))); "
     "bKeyStore := (not bFolder) and (CompareText(rec.Name, 'hMailServerApiKeys.ini') = 0);"),
    ('IsSettingsFile', 'a file that tells the service, the tools or the uninstaller what to run, or where, is said, not refused, when another account controls it (check 3 of R1 to R4, 27 October 2026)',
     "^function IsSettingsFile(Name: String): Boolean; var szExtension: String; begin szExtension := Lowercase(ExtractFileExt(Name)); "
     "Result := (CompareText(Name, 'hMailServer.INI') = 0) or (CompareText(Name, 'hMailServerApiKeys.ini') = 0) or (szExtension = '.config') or (szExtension = '.json') or "
     "((CompareText(Copy(Name, 1, 5), 'unins') = 0) and (szExtension = '.dat')); end;$"),
    ('CheckWhatIsInside', 'an owner other than the three is not refused',
     "else if (not IsTrustedSid(pOwner)) and (not (bKeyStore and SidIs(pOwner, HM_SERVICE_SID))) then begin if (not bCode) and (not bFolder) and SidIs(pOwner, HM_SERVICE_SID) then szWho := ''"),
    ('CheckWhatIsInside', 'a program or a folder an entry of whose own lets another account change it is not refused',
     "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText) then"),
    ('CheckWhatIsInside', 'what is below the first level is not looked at, or a folder there is walked without being the folder of its name in the one above it (check 4 of R1 to R4, 27 October 2026)',
     "for index := 0 to count - 1 do if not CheckWhatIsInside(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Final, Scripts, Found, Why) then Result := false; ReleaseFolder(folderHandle); end;$"),
    ('CheckWhatIsInside', 'the owner of what is in Bin and ControlPanel is not read through a handle on each',
     "readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);"),
    ('IsProgramFile', 'a program or a library is not what the live update holds to its rule',
     "Result := (szExtension = '.exe') or (szExtension = '.dll');"),
    ('OwnEntriesLetOthers', 'an inherited entry is weighed, or an entry of the item\'s own is not',
     "if (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin "
     "if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) and "
     "(not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) then"),
    ('IsScriptFile', 'a script the service, DBSetup or the web server runs is not counted as a program',
     "Result := (szExtension = '.vbs') or (szExtension = '.js') or (szExtension = '.sql') or (szExtension = '.php') or (szExtension = '.ps1') or (szExtension = '.cmd') or (szExtension = '.bat');"),
    ('IsDataFolderName', 'a folder other than the data folders the service writes in is taken for one',
     "Result := (CompareText(Name, 'Data') = 0) or (CompareText(Name, 'Logs') = 0) or (CompareText(Name, 'Temp') = 0) or (CompareText(Name, 'Database') = 0);"),
    ('CheckWhatElseIsInside', 'which items of the program folder are links, programs, scripts or data folders is not read',
     "bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0; bData := bFolder and IsDataFolderName(rec.Name); bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsScriptFile(rec.Name) or IsSettingsFile(rec.Name));"),
    ('CheckWhatElseIsInside', 'a link in the program folder that another account controls is taken (the review of 26 October 2026), or one on the program folder\'s own level that is not a data folder\'s is taken whoever controls it (check 7 of R1 to R4, 28 October 2026)',
     "if bLink then begin @@if not bData then begin if not LinkIsAdministratorsAlone(handle, -1, szText) then szWho := ~ "
     "else if bFolder and IsRunFolderName(rec.Name) then szWho := ~ else szWho := ~; if bFolder then szRemedy := ~ else szRemedy := ~; end "
     "else if not LinkIsAdministratorsAlone(handle, -1, szText) then begin szWho := ~; szRemedy := LinkRemedy(szItem) + '.'; end "
     "else if not DataLinkLeadsWhereAllowed(szItem, Final, szText) then begin szWho := ~; szRemedy := ~; end; end"),
    ('CheckWhatElseIsInside', 'a link on the program folder\'s own level that only the administrators control is not refused, or not with a remedy that removes it, when it is not a data folder\'s (check 7 of R1 to R4, 28 October 2026)',
     "else szWho := 'is a junction, a symbolic link or a mount point, and on the program folder''s own level setup takes a link only for Data, Logs, Temp or ' + ~; "
     "if bFolder then szRemedy := 'An administrator removes the link - rmdir \"' + szItem + ~ else szRemedy := 'An administrator removes the link - del \"' + szItem + ~; end"),
    ('IsRunFolderName', 'a folder setup installs into that holds what the service or the administrators run is taken behind a link (check 3 of R1 to R4, 27 October 2026)',
     "^function IsRunFolderName(Name: String): Boolean; begin Result := (CompareText(Name, 'Addons') = 0) or (CompareText(Name, 'Events') = 0) or (CompareText(Name, 'DBScripts') = 0) or "
     "(CompareText(Name, 'WebAdmin') = 0) or (CompareText(Name, 'Languages') = 0); end;$"),
    ('DataLinkLeadsWhereAllowed', 'where a data folder\'s link leads is not held to the rules of the program folder\'s own way, or to what the grant after the files refuses (check 4 of R1 to R4, 27 October 2026), or one that is not there is taken',
     "hLink := HoldForSetup(Link); if hLink = -1 then begin Why := ~; Exit; end; if not HeldIsLink(hLink, bKnown) then begin Why := ~; Exit; end; if not LinkTarget(hLink, Link, szTarget, szWhy) then begin Why := szWhy; Exit; end; if not HoldFolderChain(szTarget, 1, szReal, szHeld, hHeld, bWhole, szWhy) then begin Why := ~; Exit; end; if not bWhole then begin Why := ~; Exit; end; if not VolumeKeepsPermissions(hHeld, szHeld, szWhy) then begin Why := ~; Exit; end; @@if not DataLinkTargetIsGrantable(hHeld, szReal, szWhy) then begin Why := szWhy; Exit; end; if CompareText(ExtractFileName(Link), 'Database') = 0 then if not DatabaseTargetIsTheAdministrators(hHeld, szReal, szWhy) then begin Why := szWhy; Exit; end; if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then begin"),
    ('CheckWhatElseIsInside', 'the owner of what is in the program folder is not read through a handle on each',
     "readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);"),
    ('CheckWhatElseIsInside', 'an owner other than the three is taken in the program folder',
     "else if not IsTrustedSid(pOwner) then szWho := 'is owned by ' + SidNameAt(pOwner) + "),
    ('CheckWhatElseIsInside', 'an entry of its own that lets another account change a program, a script or a folder of the program folder is taken',
     "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, false, szText) then"),
    ('CheckWhatElseIsInside', 'a link, a program, a script or a folder - a data folder among them - another account controls in the program folder is said, not refused (check 5 of R1 to R4, 27 October 2026)',
     "if bLink or bCode or bFolder then begin Found := Found + 1;"),
    ('CheckWhatElseIsInside', 'a data folder is not held to its rule, the earlier installation\'s that another account owns is installed into without a restart or one of a new installation taken, or the Database\'s files are not looked at (check 5 of R1 to R4, 27 October 2026)',
     "else if bData then begin @@if not DataFolderIsAllowed(szItem, handle, bOwner, szWho, szRemedy) then begin if bOwner and g_bEarlierInstallation and (not Final) then begin "
     "AddRestartFolder(szItem, -1, true, false, szItem + ' was ' + Copy(szWho, 4, Length(szWho))); szWho := ''; szRemedy := ''; end; end else if CompareText(rec.Name, 'Database') = 0 then "
     "szWho := DatabaseFilesWhy(szItem, szRemedy); end"),
    ('CheckWhatElseIsInside', 'a program, a script or a settings file of the program folder that an entry it kept from where it was made lets another account change is not noted for the restart rule (check 5 of R1 to R4, 27 October 2026)',
     "else if bCode and (not Final) and StaleEntryLetsOthers(pDacl, pAppDescriptor, pAppDacl, false, false, szText) then NoteStaleItem(szItem, szText);"),
    ('CheckWhatElseIsInside', 'what the program folder hands down to the files in it is not read through its handle, or one that cannot be read is taken to hand everything down',
     "if ReadHeldSecurity(AppHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pAppOwner, 0, pAppDacl, 0, pAppDescriptor) <> 0 then pAppDescriptor := 0;"),
    ('CheckWhatElseIsInside', 'the folders setup installs into that hold what is run are not these five',
     "0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages'; 5: szFolder := 'MySQL';"),
    ('CheckWhatElseIsInside', 'what is in those folders is not looked at at any depth, scripts counted as programs',
     "if not CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why) then Result := false;"),
    ('LinkIsAdministratorsAlone', 'the folder a link is in is not asked about unless the link is in the program folder itself',
     "FreeWindowsMemory(pDescriptor); if not Result then Exit; if hParent = -1 then Exit; Result := false;"),
    ('RemakeEmptyFolder', 'a Bin or ControlPanel that holds nothing is not let go, removed and made again, or a failure of either is taken (the review of 26 October 2026)',
     "parent := HoldForSetup(szParent); if parent = -1 then begin Why := ~; Exit; end; LetGoOfHeld(HeldHandle); if not RemoveDir(Folder) then begin g_bFoldersChanged := true; Why := ~; Exit; end; Log(~); Result := MakeFolders(szParent, parent, Folder, Kind, MadeHandle, Why);"),
    ('LetGoOfHeld', 'a folder let go is let go again at the end, or another is',
     "if g_aHeldFolders[index] = Handle then begin ReleaseFolder(Handle); last := GetArrayLength(g_aHeldFolders) - 1; g_aHeldFolders[index] := g_aHeldFolders[last]; SetArrayLength(g_aHeldFolders, last); end;"),
    ('FolderHoldsNothing', 'a folder that holds something is taken for empty, or an empty one for one that holds something',
     "Result := false; if FindFirst(AddBackslash(Folder) + '*', rec) then begin try Result := true; repeat if (rec.Name <> '.') and (rec.Name <> '..') then Result := false; until (not Result) or (not FindNext(rec));"),
    ('CheckWhatIsInside', 'an empty folder in Bin, ControlPanel or the folders that hold what is run is left, once they are hardened, for a handle opened before to make a junction, or removed by its path rather than through a handle read as that folder (check 4 of R1 to R4, 27 October 2026)',
     "if Final and FolderHoldsNothing(szItem) and (not IsAtOrBelow(szItem, AddBackslash(g_szProgramFolderReal) + 'MySQL')) then begin if RemoveHeldFolder(szItem, szFinal, szRemoved) then Log('Setup removed ' + szItem + ', an empty folder there before this installation"),
    ('CheckWhatIsInside', 'an empty folder that could not be removed is taken',
     "Log('The program folder: refused, ' + szText); Result := false; end; end else begin SetArrayLength(folders, count + 1);"),
    ('CheckWhatElseIsInside', 'an empty folder of the program folder\'s own level that setup makes again - one of the five, a data folder when the server is installed - is left, once it is hardened, for a handle opened before to make a junction, or removed by its path rather than through a handle read as that folder (check 4 of R1 to R4, 27 October 2026); or an administrator\'s empty folder setup neither installs into nor makes again is removed (check 5); or one removed is not made again at once with the permissions it had (check 8)',
     "else if Final and bFolder and (not bLink) and (not IsRunFolderName(rec.Name)) and (not (bData and IsComponentSelected('server'))) and FolderHoldsNothing(szItem) then @@"
     "Log('The program folder: ' + szItem + ' is an empty folder there before this installation, which setup neither installs into nor makes again, and is left ' + 'as it is.') "
     "else if Final and bFolder and (not bLink) and FolderHoldsNothing(szItem) then begin @@if RemoveAndMakeAgain(szItem, AppHandle, szAppFinal, szRemoved) then Log('Setup removed ' + szItem + ', an empty folder there before this installation, and made it again with the permissions it had"),
    ('RemoveAndMakeAgain', 'an empty folder of the program folder\'s own level is removed without its permissions read first through a handle on it as itself, '
     'or by a path rather than through a handle read as that folder (check 8 of R1 to R4, 28 October 2026)',
     "handle := HoldFolder(Item, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then begin Why := ~; Exit; end; "
     "pOwner := 0; pDacl := 0; pDescriptor := 0; readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); "
     "ReleaseFolder(handle); if readResult <> 0 then begin Why := ~; Exit; end; @@control := 0; revision := 0; bInherits := true; "
     "if ControlOfDescriptor(pDescriptor, control, revision) then bInherits := (control and HM_DACL_PROTECTED) = 0; "
     "if not RemoveHeldFolder(Item, AppFinal, Why) then begin FreeWindowsMemory(pDescriptor); Exit; end;"),
    ('RemoveAndMakeAgain', 'an empty folder removed in the last look is left for Inno Setup to make again with what the program folder hands down, or made by '
     'a path, or with other permissions than it had - the list it had when it inherited nothing; what the program folder hands down now and '
     'the entries of its own when it inherited, an entry it had marked inherited not carried over (check 9 of R1 to R4, 31 October 2026) - '
     'or one that could not be made again, or given its own entries, is taken (check 8 of R1 to R4)',
     "if bInherits then made := MakeFolderIn(AppHandle, ExtractFileName(Item), 0, status) else made := MakeFolderIn(AppHandle, ExtractFileName(Item), pDescriptor, status); "
     "if made = -1 then begin FreeWindowsMemory(pDescriptor); g_bFoldersChanged := true; Why := ~; Exit; end; "
     "if bInherits then if not WriteOwnEntriesOn(AppHandle, made, Item, ExtractFileName(Item), pDacl, Why) then begin FreeWindowsMemory(pDescriptor); "
     "g_bFoldersChanged := true; Why := ~; Exit; end; FreeWindowsMemory(pDescriptor); if bInherits then Log(~); Result := true; end;$"),
    ('WriteOwnEntriesOn', 'a folder made again whose list inherited is given its own entries though it had none to give, or none though it had some (check 9 of R1 to R4, 31 October 2026)',
     'if (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0 then own := own + 1; index := index + 1; end; end; if own = 0 then begin Result := true; Exit; end;'),
    ('WriteOwnEntriesOn', 'an entry the folder had marked inherited - one it kept from where it was made - is carried over onto the folder made again, or an entry of its own is left out, or what it inherits now from the program folder is not kept (check 9 of R1 to R4, 31 October 2026)',
     'bOwn := (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0; bDeny := (head.TypeFlagsSize and $FF) = HM_ACCESS_DENIED; if ((pass = 0) and bOwn and bDeny) or ((pass = 1) and bOwn and (not bDeny)) or ((pass = 2) and (not bOwn)) then ok := AppendAce(pNew, HM_ACL_REVISION, HM_MAXDWORD, pAce, size);'),
    ('WriteOwnEntriesOn', 'the entries of its own are written through a link, or on another folder than the one setup made and holds, or with the list protected, so that what the program folder hands down is gone (check 9 of R1 to R4, 31 October 2026)',
     "handle := HoldFolder(Item, HM_WRITE_DAC_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then begin FreeWindowsMemory(pNew); Why := ~; Exit; end; if HeldIsLink(handle, bKnown) then Why := ~ else if not MadeWhereNamed(AppHandle, handle, Name, szWhere) then Why := ~ else if not (ReadHeldInformation(handle, openedInformation) and ReadHeldInformation(Made, heldInformation)) then Why := ~ else if (openedInformation.dwVolumeSerialNumber <> heldInformation.dwVolumeSerialNumber) or (openedInformation.nFileIndexHigh <> heldInformation.nFileIndexHigh) or (openedInformation.nFileIndexLow <> heldInformation.nFileIndexLow) then Why := ~ else begin written := WriteHeldSecurity(handle, HM_FILE_OBJECT, HM_UNPROTECTED_DACL_INFORMATION, 0, 0, pNew, 0); if written <> 0 then Why := ~; end; ReleaseFolder(handle); FreeWindowsMemory(pNew); if Why <> '' then Exit;"),
    ('WriteOwnEntriesOn', 'the folder made again is taken without its entries of its own read again through the handle setup holds (check 9 of R1 to R4, 31 October 2026)',
     'readResult := ReadHeldSecurity(Made, HM_FILE_OBJECT, HM_DACL_INFORMATION, pNowOwner, 0, pNowDacl, 0, pNowDescriptor); if readResult <> 0 then begin Why := ~; Exit; end; found := 0;'),
    ('WriteOwnEntriesOn', 'a folder made again whose entries of its own are not all there when read again is taken (check 9 of R1 to R4, 31 October 2026)',
     'FreeWindowsMemory(pNowDescriptor); if found <> own then begin Why := ~; Exit; end; Log(~); Result := true; end;$'),
    ('ItemIsIn', 'what the path reaches is taken for what is in the folder setup holds without being read through the two handles',
     "handle := HoldFolder(Item, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then Exit; szItem := FinalPathOfHeld(handle); ReleaseFolder(handle); szFolder := FinalPathOfHeld(FolderHandle); if (szItem <> '') and (szFolder <> '') then Result := CompareText(szItem, AddBackslash(szFolder) + ExtractFileName(Item)) = 0;"),
    ('NothingTurnedIn', 'a link or an empty folder setup did not leave is taken after the files',
     "if bLink or (bFolder and FolderHoldsNothing(szItem)) then begin if bLink then szWhat := ~ else szWhat := ~; Found := Found + 1; Why := ~; Log(~); Result := false; end"),
    ('NothingTurnedIn', 'what is below the first level is not looked at after the files, or a folder there is walked without being the folder of its name in the one above it (check 4 of R1 to R4, 27 October 2026)',
     "for index := 0 to count - 1 do if not NothingTurnedIn(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Found, Why) then Result := false; ReleaseFolder(folderHandle); end;$"),
    ('PreparedFolderIsAsLeft', 'a folder setup prepared is taken after the files as a link, reached elsewhere by its path, or with its permissions changed - one setup made with an entry for any account but the three',
     "Result := false; if HeldIsLink(Handle, bKnown) then Why := ~ else if (Item <> '') and (not ItemIsIn(Item, Handle)) then Why := ~ else if Hardened and (not HeldFolderIsHardened(Handle, AnyAccount, false, szWhy)) then Why := ~ else Result := true;"),
    ('FoldersStillAsPrepared', 'the program folder and Bin are not read again after the files, a folder setup made is not held to having no other account\'s entry, or Bin is walked when it was not found as setup left it, or from its path rather than its handle',
     "if g_hProgramFolderPrepared <> -1 then if PreparedFolderIsAsLeft(g_szProgramFolderReal, g_hProgramFolderPrepared, szApp + 'Bin', g_bProgramFolderHardened, g_bProgramFolderNew, Why) then szAppFinal := FinalPathOfHeld(g_hProgramFolderPrepared); if g_hBinPrepared <> -1 then if PreparedFolderIsAsLeft(g_szBinReal, g_hBinPrepared, szApp + 'Bin\\hMailServer.INI', true, SetupMadeBin(), Why) then NothingTurnedIn(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, found, Why);"),
    ('FoldersStillAsPrepared', 'ControlPanel is not read again after the files, or is walked when it was not found as setup left it',
     "if ControlPanelIsInstalled() then begin if PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, szApp + 'ControlPanel\\hMailCP.exe', true, SetupMadeControlPanel(), Why) then NothingTurnedIn(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, found, Why); end else if PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, '', true, SetupMadeControlPanel(), Why) then NothingTurnedIn(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, found, Why);"),
    ('FoldersStillAsPrepared', 'what is in the folders that hold what is run is not looked at after the files, or is walked when the program folder was not found as setup left it, or as other than the folders of their names in it',
     "if szAppFinal <> '' then for index := 0 to 4 do begin case index of 0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages'; end; NothingTurnedIn(AddBackslash(g_szProgramFolderReal) + szFolder, AddBackslash(szAppFinal) + szFolder, 0, found, Why); end; Result := Why = '';"),
    ('CurStepChanged', 'the folders are not read again before setup runs a program of the installation, or what is not as left does not fail it, keep the service from starting, keep setup from installing again before a restart and say so for hMailServer.Updater',
     "g_bFilesInstalled := true; @@if not FoldersStillAsPrepared(szWhy) then begin g_bFoldersNotAsPrepared := true; g_bStepsIncomplete := true; KeepServiceFromStarting(); RecordRestartNeeded(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), ~); Log(FoldersChangedLine()); RaiseException("),
    ('FoldersWereAsPrepared', 'the last page runs the Control Panel from the folders found not as setup left them (check 3 of R1 to R4, 27 October 2026)',
     "^function FoldersWereAsPrepared(): Boolean; begin Result := not g_bFoldersNotAsPrepared; end;$"),
    ('KeepServiceFromStarting', 'the service is not set not to start',
     "if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= disabled', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then"),
    ('LinkRemedy', 'the remedy of a link does not say that a restart takes away what a program opened it for',
     "/L /inheritance:r /grant:r *S-1-5-18:F *S-1-5-32-544:F *S-1-5-32-545:RX - restarts Windows, since a program that opened the link while another account could ' + 'change it keeps that right until it closes it, and runs setup again"),
    ('HardenHeldFolder', 'the earlier installation\'s folder another account owned is not said, with the restart that takes its handles away',
     "if bTrusted then SayWriteEntriesKept(Folder, HeldHandle) else begin @@szText := ~; Log('The program folder: ' + szText); g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10; end; Result := true;"),
    ('PrepareProgramFolders', 'Bin, and ControlPanel when it is installed, are not made, and recorded, in a program folder setup made, so that a cancel during the copy leaves them, and every folder setup made above them, behind (check 4 of R1 to R4, 27 October 2026)',
     "if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'Bin', 1, hInside, szWhy) then begin Result := ~; Exit; end; g_hBinPrepared := hInside; if ControlPanelIsInstalled() then begin if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'ControlPanel', 2, hInside, szWhy) then begin Result := ~; Exit; end; g_hControlPanelPrepared := hInside; end; Exit; end;"),
    ('SayWriteEntriesKept', 'an entry kept that lets an account other than the three write is not said',
     "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and "
     "(not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) then"),
    # The service: what is refused before it is stopped, and said after.
    ('NextButtonClick', 'the folders are not checked before the service is stopped, or a refusal there stops it all the same',
     "g_szFolderRefusal := CheckFoldersBeforeTheStop(PreviousInstallationFolder()); if g_szFolderRefusal <> '' then Exit;"),
    ('NextButtonClick', 'whether setup stopped a running service is not known',
     "bWasRunning := IsServiceRunning('hMailServer'); if bWasRunning then g_bServiceStoppedForSetup := true; if (StopHMailServerService() = false) then Result := false;"),
    ('PrepareToInstall', 'a refusal after setup stopped the service does not say so, one that found a folder changed does not keep the service from starting and say so for hMailServer.Updater, or one whose remedy ends in a restart lets setup install again before it',
     "Result := PrepareToInstallRefusal(); if Result <> '' then begin @@if g_bFoldersChanged then begin KeepServiceFromStarting(); Log(FoldersChangedLine()); if g_bServiceStoppedForSetup then Result := Result + ' Setup stopped the hMailServer service~; end else if g_bServiceStoppedForSetup then Result := Result + ' Setup stopped the hMailServer service~; @@if Pos('restarts Windows', Result) > 0 then RecordRestartNeeded(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), ~); Log('Refused: ' + Result); end; end;$"),
    ('PrepareToInstallRefusal', 'the program folders are not prepared last, their refusal the answer',
     "Result := PrepareProgramFolders(PreviousInstallationFolder()); end;$"),
    # The helpers the decisions rely on (check 3 of R1 to R4, 27 October 2026, whose
    # twenty one-line changes of them and of the constants passed the checker nineteen
    # times and the fixture eleven): the walk's start, its names and where it goes on
    # after a link.
    ('HoldFolderChain', 'a walk follows links deeper than eight, or starts from what an earlier walk left (check 3 of R1 to R4, 27 October 2026)',
     "begin Result := false; Real := ''; Held := ''; HeldHandle := -1; Whole := false; Why := ''; if Depth >= 8 then begin Why := ~; Exit; end; "
     "if not SplitAtRoot(Path, szRoot, szRest, Why) then Exit;"),
    ('HoldFolderChain', 'the root of a volume is not held, or one that cannot be opened is walked',
     "parent := HoldForSetup(szRoot); if parent = -1 then begin Why := ~; Exit; end;"),
    ('HoldFolderChain', 'the next name is not cut from the path as it is written, so that a name is passed over or taken with the rest',
     "separator := Pos('\\', szRest); if separator = 0 then begin szName := szRest; szRest := ''; end else begin szName := Copy(szRest, 1, separator - 1); "
     "szRest := Copy(szRest, separator + 1, Length(szRest)); end;"),
    ('HoldFolderChain', 'a name is not looked at where the walk is, or whether it could be read is not asked',
     "szNext := AddBackslash(szCurrent) + szName; attributes := FolderAttributes(szNext); lastError := DLLGetLastError(); if attributes = HM_INVALID_ATTRIBUTES then begin"),
    ('HoldFolderChain', 'the folders below a missing name are not among those setup makes',
     "Real := szNext; if szRest <> '' then Real := AddBackslash(szNext) + szRest;"),
    ('HoldFolderChain', 'the rest of the path after a link is not walked from where the link leads, so that the walk ends there',
     "if szRest <> '' then szTarget := AddBackslash(szTarget) + szRest; Result := HoldFolderChain(szTarget, Depth + 1, Real, Held, HeldHandle, Whole, Why); end; bDone := true;"),
    # Where a link leads.
    ('LinkTarget', 'a link is taken for one setup has read before its data is read',
     "begin Result := false; Target := ''; Why := '';"),
    ('LinkTarget', 'a link whose data cannot be read, or is too short to hold a name, is taken',
     "if not ReadReparseData(hLink, HM_GET_REPARSE_POINT, 0, 0, buffer, 16384, returned, 0) then begin Why := ~; Exit; end; if returned < 20 then begin Why := ~; Exit; end;"),
    ('LinkTarget', 'where a link leads is read from other words of its data - the name shown rather than the substitute name Windows follows',
     "tagLow := Ord(buffer[1]); tagHigh := Ord(buffer[2]); offset := Ord(buffer[5]); size := Ord(buffer[6]); relative := false;"),
    ('LinkTarget', 'a name past the end of the link\'s data, or an empty one, is taken, or the name is read from elsewhere',
     "bytes := returned; if ((offset div 2) + (size div 2) + start - 1 > (bytes div 2)) or (size = 0) then begin Why := ~; Exit; end; "
     "szName := Copy(buffer, start + (offset div 2), size div 2);"),
    ('LinkTarget', 'a relative symbolic link is taken from setup\'s own folder rather than as Windows takes it (RelativeLinkTarget)',
     "if relative then begin if not RelativeLinkTarget(Link, szName, Target, Why) then Exit; end else if"),
    ('RelativeLinkTarget', 'a relative symbolic link is taken from a root other than its own volume\'s, or a link setup cannot name is taken '
     '(check 8 of R1 to R4, 28 October 2026)',
     "begin Result := false; Target := ''; if not SplitAtRoot(Link, szRoot, szRest, Why) then Exit;"),
    ('RelativeLinkTarget', 'a relative symbolic link whose name starts with a separator is taken from the folder the link is in, not from the '
     'root of its volume as Windows takes it, or one without from elsewhere than the folder the link is in (check 8 of R1 to R4)',
     "if Copy(Name, 1, 1) = '\\' then szPath := Name else szPath := szRest + '\\..\\' + Name; szPath := szPath + '\\'; count := 0;"),
    ('RelativeLinkTarget', 'a relative name that goes above the root of its volume is taken, or .. and . are read as folders (check 8 of R1 to R4)',
     "if szPart = '..' then begin if count = 0 then begin Why := ~; Exit; end; count := count - 1; end else if (szPart <> '') and (szPart <> '.') then begin"),
    ('RelativeLinkTarget', 'where a relative link leads is not made a full path from the root of its volume (check 8 of R1 to R4)',
     "Target := szRoot; for index := 0 to count - 1 do begin if index > 0 then Target := Target + '\\'; Target := Target + parts[index]; end; Result := true; end;$"),
    ('LinkTarget', 'what a link leads to is not made a full path as Windows names it, a volume\'s root kept with its backslash',
     "else if Copy(szName, 1, 4) = '\\??\\' then begin Target := Copy(szName, 5, Length(szName)); if CompareText(Copy(Target, 1, 7), 'Volume{') = 0 then "
     "Target := '\\\\?\\' + Target; end else begin Why := ~; Exit; end; Target := RemoveBackslashUnlessRoot(Target); if Target = '' then begin Why := ~; Exit; end; "
     "if Copy(Target, 1, 4) = '\\\\?\\' then if Target[Length(Target)] = '}' then Target := Target + '\\'; Result := true;"),
    # What is in Bin, ControlPanel and the program folder.
    ('CheckWhatIsInside', 'Bin and ControlPanel are looked at past 32 folders down, a folder whose listing cannot be read is taken, or one that is a link now, or not where it was listed, is walked (check 4 of R1 to R4, 27 October 2026)',
     "Result := true; count := 0; if Depth > 32 then begin Found := Found + 1; Why := ~; Result := false; Exit; end; folderHandle := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); szFinal := ''; if folderHandle <> -1 then if not HeldIsLink(folderHandle, bKnown) then szFinal := FinalPathOfHeld(folderHandle); if (szFinal = '') or ((Expected <> '') and (CompareText(szFinal, Expected) <> 0)) then begin if folderHandle <> -1 then ReleaseFolder(folderHandle); Found := Found + 1; szText := ~; Why := Why + szText + ' '; Log(~); Result := false; Exit; end; if not FindFirst(AddBackslash(Folder) + '*', rec) then begin ReleaseFolder(folderHandle); Found := Found + 1; Why := Why + 'what is in ' + Folder + ' could not be read (Windows error '"),
    ('CheckWhatIsInside', 'what is a folder in Bin and ControlPanel is not read, so that nothing below their first level is looked at and a folder another account owns is only said',
     "if (rec.Name <> '.') and (rec.Name <> '..') then begin szItem := AddBackslash(Folder) + rec.Name; bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;"),
    ('CheckWhatElseIsInside', 'the program folder\'s own level is not looked at, or one whose listing cannot be read is taken',
     "Result := true; szAppFinal := FinalPathOfHeld(AppHandle); if not IsVolumeRoot(App) then begin if not FindFirst(AddBackslash(App) + '*', rec) then begin Found := Found + 1;"),
    ('CheckWhatElseIsInside', 'an item of the program folder\'s own level other than Bin and ControlPanel is passed over, or what is a folder there is not read',
     "if (rec.Name <> '.') and (rec.Name <> '..') and (CompareText(rec.Name, 'Bin') <> 0) and (CompareText(rec.Name, 'ControlPanel') <> 0) then begin "
     "szItem := AddBackslash(App) + rec.Name; bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;"),
    ('CheckWhatElseIsInside', 'Addons, Events, DBScripts, WebAdmin or Languages, when it is there and no link, is not looked through, or one that is a link is',
     "for index := 0 to 5 do begin case index of 0: szFolder := 'Addons';"),
    ('NoteFoldersOthersCouldChange', 'Addons, Events, DBScripts, WebAdmin, Languages and MySQL are not the folders on the program folder\'s own level the restart rule asks (check 7 of R1 to R4, 28 October 2026)',
     "for index := 0 to 5 do begin case index of 0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages'; 5: szFolder := 'MySQL'; end;"),
    ('CheckWhatElseIsInside', 'one of the five folders that is there is not looked through, or one that is a link is (check 3 of R1 to R4, 27 October 2026)',
     "szFolder := AddBackslash(App) + szFolder; @@if FolderAttributes(szFolder) <> HM_INVALID_ATTRIBUTES then if ((FolderAttributes(szFolder) and FILE_ATTRIBUTE_DIRECTORY) <> 0) and ((FolderAttributes(szFolder) and HM_REPARSE_POINT) = 0) then if not CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why) then Result := false;"),
    # Users' Read on a folder setup makes.
    ('EveryUserMayReadMadeIn', 'the folder setup makes in is not read through its handle, or one whose permissions cannot be read is taken for one every user may read',
     "Result := false; pDescriptor := 0; pDacl := 0; pOwner := 0; if ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then Exit; "
     "if pDacl = 0 then Result := true else if ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then"),
    ('EveryUserMayReadMadeIn', 'Users is given Read on a folder made where not every user could read one: an entry that is not an allow entry handed down to folders, does not read, or is another group\'s, counted',
     "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and HM_CONTAINER_INHERIT) <> 0) and ((head.Mask and HM_FOLDER_READ_RIGHTS) <> 0) then "
     "begin group := 0; while (not Result) and (group < 3) do begin szSid := BroadGroupSid(group, szName); if SidIs(pAce + 8, szSid) then Result := true; group := group + 1; end; end;"),
    # What the uninstaller is given, and what a cancel removes.
    ('ProgramFolderMadeAsNamed', 'a program folder setup did not make, or made where a link leads, is given to the uninstaller as {app}',
     "^function ProgramFolderMadeAsNamed(): Boolean; begin Result := false; if g_bProgramFolderNew then "
     "Result := CompareText(g_szProgramFolderReal, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0; end;$"),
    ('MadeParentCount', 'the folders setup made above the program folder are counted with it, or without it where a link leads',
     "^function MadeParentCount(): Integer; begin Result := GetArrayLength(g_aFoldersMade); if ProgramFolderMadeAsNamed() then Result := Result - 1; if Result < 0 then Result := 0; end;$"),
    ('MadeParentOffset', 'other than the eight folders setup made nearest the program folder are given to the uninstaller',
     "^function MadeParentOffset(): Integer; begin Result := MadeParentCount() - 8; if Result < 0 then Result := 0; end;$"),
    ('SetupMadeFolder', 'a folder setup made above the program folder is not given to the uninstaller, or one it did not make is',
     "^function SetupMadeFolder(Index: Integer): Boolean; begin Result := g_bProgramFolderNew and (MadeParentOffset() + Index < MadeParentCount()); end;$"),
    ('FolderMadeBySetup', 'the uninstaller is given another folder than the one setup made for an entry',
     "^function FolderMadeBySetup(Param: String): String; var index: Integer; begin index := MadeParentOffset() + StrToIntDef(Param, 0); Result := ''; "
     "if (index >= 0) and (index < GetArrayLength(g_aFoldersMade)) then Result := g_aFoldersMade[index]; end;$"),
    ('SetupMadeProgramFolder', 'the program folder setup made is not given to the uninstaller, or one it did not make is',
     "^function SetupMadeProgramFolder(): Boolean; begin Result := ProgramFolderMadeAsNamed(); end;$"),
    ('SetupMadeBin', 'Bin is given to the uninstaller whatever made it',
     "^function SetupMadeBin(): Boolean; begin Result := g_szBinMade <> ''; end;$"),
    ('SetupMadeControlPanel', 'ControlPanel is given to the uninstaller whatever made it',
     "^function SetupMadeControlPanel(): Boolean; begin Result := g_szControlPanelMade <> ''; end;$"),
    ('RemoveFoldersMadeHere', 'a folder setup made is not removed after a cancel, removed shallowest first, or remembered after it is gone (check 3 of R1 to R4, 27 October 2026)',
     "^procedure RemoveFoldersMadeHere(); var index: Integer; begin @@if g_szBinMade <> '' then RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade); "
     "if g_szControlPanelMade <> '' then RemoveMadeFolder(g_szControlPanelMade, g_szControlPanelMadeAt, g_hControlPanelMade); g_szBinMade := ''; g_szBinMadeAt := ''; "
     "g_szControlPanelMade := ''; g_szControlPanelMadeAt := ''; for index := GetArrayLength(g_aFoldersMade) - 1 downto 0 do "
     "RemoveMadeFolder(g_aFoldersMade[index], g_aFoldersMadeAt[index], g_aFoldersMadeHandle[index]); SetArrayLength(g_aFoldersMade, 0); SetArrayLength(g_aFoldersMadeAt, 0); "
     "SetArrayLength(g_aFoldersMadeHandle, 0); g_bProgramFolderNew := false; end;$"),
    # The third review's helpers (check 3 of R1 to R4, 27 October 2026).
    ('DataLinkLeadsWhereAllowed', 'a data folder\'s link is taken when it cannot be held, read or walked',
     "begin Result := false; Why := ''; hLink := HoldForSetup(Link);"),
    ('DataLinkLeadsWhereAllowed', 'where a data folder\'s link leads, owned by another account or with an entry of its own that lets one write it, is taken, or the earlier installation\'s is installed into without a restart (check 5 of R1 to R4, 27 October 2026)',
     "if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then begin if bOwner and g_bEarlierInstallation and (not Final) then "
     "AddRestartFolder(szReal, hHeld, true, false, Link + ' led to ' + szReal + ', which was ' + Copy(szWho, 4, Length(szWho))) else begin Why := ~; Exit; end; end; Result := true; end;$"),
    ('DataFolderIsAllowed', 'a data folder whose owner and permissions cannot be read, or whose owner is another account than the three or the service, is taken (check 5 of R1 to R4, 27 October 2026)',
     "readResult := ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin Who := ~; Exit; end; "
     "if pOwner = 0 then Who := ~ else if (not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID)) then begin Owner := true;"),
    ('DataFolderIsAllowed', 'a data folder with an entry of its own that lets another account but the service write it is taken (check 5 of R1 to R4, 27 October 2026)',
     "else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then begin Who := "),
    ('DataFolderIsAllowed', 'a data folder is taken but when its owner and its entries were read and found as they may be',
     "^function DataFolderIsAllowed(Folder: String; Handle: Integer; var Owner: Boolean; var Who: String; var Remedy: String): Boolean; var pOwner: cardinal; pDacl: cardinal; "
     "pDescriptor: cardinal; readResult: cardinal; szText: String; szSid: String; begin Result := false;"),
    ('DatabaseFilesWhy', 'a file of the Database another account owns, or whose owner cannot be read, is taken (check 5 of R1 to R4, 27 October 2026)',
     "if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then begin Result := 'holds ' + szItem"),
    ('DatabaseFilesWhy', 'a file of the Database with an entry of its own that lets another account but the service write it is taken',
     "else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then begin Result := 'holds ' + szItem"),
    ('DatabaseFilesWhy', 'a link among the Database\'s files, or a Database whose files cannot be listed, is taken',
     "if (rec.Attributes and HM_REPARSE_POINT) <> 0 then begin Result := 'holds ' + szItem + ', which is a link';"),
    ('DatabaseFilesWhy', 'a Database whose files cannot be listed is taken',
     "Result := ''; Remedy := ''; if not FindFirst(AddBackslash(Folder) + '*', rec) then begin Result := "),
    ('HardenDataFolder', 'a data folder another account owned is rebuilt as a program folder (the service\'s entries dropped, Users given Read), not read again, or its owner named after the list is freed',
     "szOwner := SidNameAt(pOwner); Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, false, true, Why); FreeWindowsMemory(pDescriptor); if not Result then Exit; "
     "Result := HeldFolderIsHardened(HeldHandle, true, true, Why); if not Result then begin Why := ~; Exit; end;"),
    ('AddRestartFolder', 'a folder is noted for the restart rule twice, or without whether it is a data folder, or whether another account could change its permissions or its owner (check 6 of R1 to R4, 28 October 2026)',
     "for index := 0 to GetArrayLength(g_aRestartFolders) - 1 do if CompareText(g_aRestartFolders[index], Folder) = 0 then Exit; count := GetArrayLength(g_aRestartFolders) + 1; "
     "SetArrayLength(g_aRestartFolders, count); SetArrayLength(g_aRestartHandles, count); SetArrayLength(g_aRestartData, count); SetArrayLength(g_aRestartDistrust, count); "
     "g_aRestartFolders[count - 1] := Folder; g_aRestartHandles[count - 1] := Handle; g_aRestartData[count - 1] := Data; g_aRestartDistrust[count - 1] := Distrust;"),
    ('StaleEntryLetsOthers', 'an entry a file or a folder kept from where it was made is not found, or one the folder it is in hands down is taken for it (check 5 of R1 to R4, 27 October 2026; a folder, check 7, 28 October 2026)',
     "if ((aceFlags and HM_INHERITED_ACE) <> 0) and (Folder or ((aceFlags and HM_INHERIT_ONLY) = 0)) and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and "
     "((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-4')) and "
     "(not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) and (not HandsDownWriteTo(pFolderDescriptor, pFolderDacl, pAce + 8, inherit)) then begin Result := true;"),
    ('StaleEntryLetsOthers', 'a folder\'s entry that applies to it is held to what the folder it is in hands down to files, not to folders, or a file\'s to what it hands down to folders (check 7 of R1 to R4, 28 October 2026)',
     "inherit := HM_OBJECT_INHERIT; if Folder then begin if (aceFlags and HM_INHERIT_ONLY) = 0 then inherit := HM_CONTAINER_INHERIT else inherit := HM_OBJECT_INHERIT or HM_CONTAINER_INHERIT; end;"),
    ('HandsDownWriteTo', 'a folder whose list could not be read is taken to hand a right down, or an entry not handed down to files, not giving a right to write, or of another account is taken for one',
     "Result := false; if pFolderDescriptor = 0 then Exit; if pFolderDacl = 0 then begin Result := true; Exit; end; @@"
     "if not ReadAclSize(pFolderDacl, info, 12, HM_ACL_SIZE_INFORMATION) then Exit;"),
    ('HandsDownWriteTo', 'an entry not handed down to files, not giving a right to write, or of another account is taken for one the folder hands down',
     "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and Inherit) <> 0) and "
     "((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and SidsAreEqual(pAce + 8, pSid) then Result := true;"),
    ('HandsDownWriteTo', 'the inheritance asked for is not the one the caller names (check 7 of R1 to R4, 28 October 2026)',
     "^function HandsDownWriteTo(pFolderDescriptor: cardinal; pFolderDacl: cardinal; pSid: cardinal; Inherit: cardinal): Boolean;"),
    ('NoteStaleItem', 'a stale entry is not kept for the restart rule with what it let',
     "SetArrayLength(g_aStaleItems, GetArrayLength(g_aStaleItems) + 1); g_aStaleItems[GetArrayLength(g_aStaleItems) - 1] := Item; "
     "SetArrayLength(g_aStaleWhat, GetArrayLength(g_aStaleItems)); g_aStaleWhat[GetArrayLength(g_aStaleItems) - 1] := Item + ' let ' + Who"),
    ('StaleItemIn', 'what is in Bin, ControlPanel or a folder that holds what is run is taken for the program folder\'s, or the other way about',
     "bDirect := CompareText(Folder, g_szProgramFolderReal) = 0; for index := 0 to GetArrayLength(g_aStaleItems) - 1 do begin if bDirect then "
     "bIn := CompareText(ExtractFileDir(g_aStaleItems[index]), Folder) = 0 else bIn := CompareText(Copy(g_aStaleItems[index], 1, Length(AddBackslash(Folder))), AddBackslash(Folder)) = 0; "
     "if bIn and (Result = '') then Result := g_aStaleWhat[index]; end;"),
    ('TakenEntriesLetOthers', 'an entry setup\'s hardening keeps - a named account\'s own - is counted for a restart, so that every run of setup is refused, or one it takes away - inherited, or a group of every user\'s own - is not (check 5 of R1 to R4, 27 October 2026)',
     "if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-4')) and (((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8)) then begin Result := true;"),
    ('TakenEntriesLetOthers', 'no list, a list that cannot be read, or an entry of a form setup does not read is not counted, or an inherit-only entry is',
     "if pDacl = 0 then begin Result := true; Who := ~; Exit; end; if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then begin Result := true; Who := ~; Exit; end;"),
    ('TakenEntriesLetOthers', 'an inherit-only entry, or one of a form setup does not read, is weighed otherwise',
     "if (aceFlags and HM_INHERIT_ONLY) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin"),
    ('CheckWhatIsInside', 'a program, a library, a script, a settings file or a folder that an entry it kept from where it was made lets another account change is not noted for the restart rule (check 5 of R1 to R4, 27 October 2026; a folder, check 7, 28 October 2026)',
     "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText) then begin szWho := ~; szRemedy := ~; end "
     "else if (not Final) and StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText) then NoteStaleItem(szItem, szText); end;"),
    ('CheckWhatIsInside', 'what a folder hands down to the files in it is not read through its handle, or one that cannot be read is taken to hand everything down',
     "if ReadHeldSecurity(folderHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pFolderOwner, 0, pFolderDacl, 0, pFolderDescriptor) <> 0 then pFolderDescriptor := 0;"),
    ('PreparedFolderIsLink', 'the refusal of a prepared folder made a link does not name it, or does not take it for a folder changed while setup prepared it',
     "^function PreparedFolderIsLink(Folder: String): String; begin g_bFoldersChanged := true; Result := Folder + ' is a junction, a symbolic link or a mount point now"),
    ('ReleaseHeldFolders', 'a folder setup holds is let go before the end, or not at all',
     "^procedure ReleaseHeldFolders(); var index: Integer; begin for index := GetArrayLength(g_aHeldFolders) - 1 downto 0 do ReleaseFolder(g_aHeldFolders[index]); SetArrayLength(g_aHeldFolders, 0); end;$"),
    ('ControlPanelIsInstalled', 'ControlPanel is made, or not, whatever is installed',
     "^function ControlPanelIsInstalled(): Boolean; begin Result := IsComponentSelected('controlpanel'); end;$"),
    # Check 4 of R1 to R4 (27 October 2026): what another account could change before setup, the files setup keeps, removal and walks through
    # handles, the exit code of folders found changed, and where a data folder's link leads.
    ('CheckProgramFolders', 'setup installs again before Windows has restarted, once it has hardened a folder another account could change or refused for what a restart must follow (check 4 of R1 to R4, 27 October 2026)',
     "if RestartNeededFor(szApp, szWhy) then begin Result := ~; Exit; end; if not HoldFolderChain(szApp, 0,"),
    ('CheckProgramFolders', 'what another account could change before this installation is not looked at, or looked at before every refusal of the check',
     "if found > 0 then begin Result := ~; Exit; end; @@NoteFoldersOthersCouldChange(); end;$"),
    ('CheckProgramFolders', 'what an earlier pass found another account could change, or found changed, is kept when the folder page is gone back to',
     "g_bFoldersChanged := false; SetArrayLength(g_aRestartFolders, 0); SetArrayLength(g_aRestartHandles, 0); SetArrayLength(g_aRestartData, 0); SetArrayLength(g_aRestartDistrust, 0); SetArrayLength(g_aStaleItems, 0); "
     "SetArrayLength(g_aStaleWhat, 0); g_szRestartWhy := ''; @@g_hProgramFolder := -1; g_hBin := -1; g_hControlPanel := -1;"),
    ('NoteFoldersOthersCouldChange', 'the program folder, a Bin or a ControlPanel setup hardens in place is not asked whether another account could change it',
     "if not g_bProgramFolderThere then Exit; NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, g_szProgramFolderWaits <> ''); if g_bBinThere and (not g_bRemakeBin) then NoteFolderOthersCouldChange(g_szBinReal, g_hBin, true, false); if g_bControlPanelThere and (not g_bRemakeControlPanel) then NoteFolderOthersCouldChange(g_szControlPanelReal, g_hControlPanel, true, false);"),
    ('NoteFoldersOthersCouldChange', 'Addons, Events, DBScripts, WebAdmin and Languages are not asked whether another account could change them, or are asked through a link',
     "handle := HoldFolder(szFolder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle <> -1 then begin if not HeldIsLink(handle, bKnown) then NoteFolderOthersCouldChange(szFolder, handle, false, false); ReleaseFolder(handle); end;"),
    ('NoteFolderOthersCouldChange', 'a folder that holds something but empty folders, or the program folder setup leaves unhardened before the files, is not asked who could write it, or a folder whose permissions another account could change is not noted so (check 6 of R1 to R4, 28 October 2026)',
     "if not OthersCouldChange(Folder, HeldHandle, Unhardened or (not HoldsOnlyEmptyFolders(Folder)), bDistrust, szWhat) then begin szWhat := StaleItemIn(Folder); if szWhat = '' then Exit; end; "
     "if Keep then AddRestartFolder(Folder, HeldHandle, false, bDistrust, szWhat) else AddRestartFolder(Folder, -1, false, bDistrust, szWhat);"),
    ('OthersCouldChange', 'an owner other than the three, an entry that lets another account change the permissions or the owner, or one that lets it write a folder that holds something, is not found, or a folder whose permissions cannot be read is taken',
     "Result := true; Distrust := false; What := ''; pOwner := 0; pDacl := 0; pDescriptor := 0; readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin What := ~; Exit; end; if pOwner = 0 then What := ~ else if not IsTrustedSid(pOwner) then What := ~ else if TakenEntriesLetOthers(pDacl, HM_CHANGE_PERMISSIONS_RIGHTS, szWho) then begin What := ~; Distrust := true; end else if Contents and TakenEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szWho) then What := ~ else Result := false;"),
    ('HoldsOnlyEmptyFolders', 'a file, a link or a folder that holds something is taken for an empty folder, or a folder whose listing cannot be read for one that holds only those',
     "Result := false; if FindFirst(AddBackslash(Folder) + '*', rec) then begin try Result := true; repeat if (rec.Name <> '.') and (rec.Name <> '..') then begin if ((rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) or ((rec.Attributes and HM_REPARSE_POINT) <> 0) then Result := false else if not FolderHoldsNothing(AddBackslash(Folder) + rec.Name) then Result := false; end; until (not Result) or (not FindNext(rec));"),
    ('HardenForRestart', 'what another account could change is installed into without a restart, or the restart is not recorded before anything is changed',
     "if GetArrayLength(g_aRestartFolders) = 0 then Exit; szApp := RemoveBackslashUnlessRoot(ExpandConstant('{app}')); RecordRestartNeeded(szApp, ~); szDone := '';"),
    ('HardenForRestart', 'a folder another account could change is not hardened, or one another account owned is taken for a folder other than the earlier installation\'s',
     "handle := g_aRestartHandles[index]; if handle = -1 then handle := HoldForSetup(g_aRestartFolders[index]); if handle = -1 then szWhy := ~ else if g_aRestartData[index] then begin "
     "if not HardenDataFolder(g_aRestartFolders[index], handle, szWhy) then begin if szWhy = '' then szWhy := ~; end else szWhy := ''; end "
     "else if g_aRestartDistrust[index] then begin if not HardenDistrustedFolder(g_aRestartFolders[index], handle, szWhy) then begin if szWhy = '' then szWhy := ~; end else szWhy := ''; end "
     "else if not HardenHeldFolder(g_aRestartFolders[index], handle, g_bEarlierInstallation, szWhy) then"),
    ('HardenDistrustedFolder', 'a folder whose permissions another account could change keeps an entry of its own that lets another account write - one that account could have written - or is not read through its handle, or one another account owns is taken although it is not the earlier installation\'s (check 6 of R1 to R4, 28 October 2026)',
     "readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin Why := ~; Exit; end; "
     "if pOwner = 0 then Why := ~ else if (not IsTrustedSid(pOwner)) and (not g_bEarlierInstallation) then Why := ~ else Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, EveryUserMayRead(pDacl), false, Why); "
     "FreeWindowsMemory(pDescriptor); if not Result then Exit; Result := HeldFolderIsHardened(HeldHandle, true, false, Why);"),
    ('HardenForRestart', 'the installation goes on once what another account could change is hardened, rather than being refused until Windows has restarted',
     "szDone := szDone + g_aRestartFolders[index] + ', '; end; Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: "),
    ('CheckFoldersBeforeTheStop', 'what another account could change is not hardened and refused before the service is stopped, or is after a refusal of the check',
     "^function CheckFoldersBeforeTheStop(Earlier: String): String; begin Result := CheckProgramFolders(Earlier); if Result = '' then Result := HardenForRestart(); end;$"),
    ('RestartNeededFor', 'what setup recorded until Windows restarts is read elsewhere, or not by the program folder\'s name',
     "^function RestartNeededFor(App: String; var What: String): Boolean; begin What := ''; Result := RegQueryStringValue(HKLM64, HM_RESTART_KEY, App, What); end;$"),
    ('RecordRestartNeeded', 'what is recorded until Windows restarts is written where a restart does not take it away, or by another name than the program folder\'s, or not also by where the walks of the program folder, Bin and ControlPanel ended (check 5 of R1 to R4, 27 October 2026)',
     "written := MakeVolatileKey(HM_HKEY_LOCAL_MACHINE, HM_RESTART_KEY, 0, 0, HM_REG_OPTION_VOLATILE, HM_KEY_SET_VALUE_64, 0, key, disposition); if written = 0 then begin written := WriteKeyText(key, App, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2); "
     "for which := 0 to 2 do begin szFinal := WalkedFinal(which); if (written = 0) and (szFinal <> '') and (not IsVolumeRoot(szFinal)) then written := WriteKeyText(key, szFinal, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2); end; CloseKey(key); end;"),
    ('RestartRecordedForWalked', 'a record by where a walk ended is not looked up, or one of another folder, or of none, is taken for it (check 5 of R1 to R4, 27 October 2026)',
     "if not RegGetValueNames(HKLM64, HM_RESTART_KEY, names) then Exit; for index := 0 to GetArrayLength(names) - 1 do if (not Result) and (CompareText(Copy(names[index], 1, 4), '\\\\?\\') = 0) then "
     "for which := 0 to 2 do begin szWalked := WalkedFinal(which); if (not Result) and IsAtOrBelow(szWalked, names[index]) then begin Result := true;"),
    ('WalkedFinal', 'where a walk ended is named by a path rather than by its held handle, or a folder not there yet by the one above it alone (check 5 of R1 to R4, 27 October 2026)',
     "if (handle = -1) or (szHeld = '') or (szReal = '') then Exit; szFinal := FinalPathOfHeld(handle); if szFinal = '' then Exit; if CompareText(szHeld, szReal) = 0 then Result := szFinal "
     "else if CompareText(Copy(szReal, 1, Length(AddBackslash(szHeld))), AddBackslash(szHeld)) = 0 then Result := AddBackslash(szFinal) + Copy(szReal, Length(AddBackslash(szHeld)) + 1, Length(szReal));"),
    ('WalkedFinal', 'the walks are named by another walk\'s handle',
     "0: begin handle := g_hProgramFolder; szHeld := g_szProgramFolderHeld; szReal := g_szProgramFolderReal; end; 1: begin handle := g_hBin; szHeld := g_szBinHeld; szReal := g_szBinReal; end; "
     "else begin handle := g_hControlPanel; szHeld := g_szControlPanelHeld; szReal := g_szControlPanelReal; end;"),
    ('CheckProgramFolders', 'the record of a restart is not looked up by where the walks ended, so that another name for the same folder installs before the restart (check 5 of R1 to R4, 27 October 2026)',
     "@@if RestartRecordedForWalked(szWhy) then begin Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: "),
    ('MakePlaceholder', 'the file setup keeps in a folder it hardened in place is shared for deleting, kept after setup, or not made new',
     "handle := HoldFolder(AddBackslash(Folder) + HM_PLACEHOLDER_NAME, HM_PLACEHOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_CREATE_NEW, HM_PLACEHOLDER_FLAGS, 0);"),
    ('MakePlaceholder', 'the file setup keeps is taken where a link put the folder elsewhere',
     "if not MadeWhereNamed(HeldHandle, handle, HM_PLACEHOLDER_NAME, szWhere) then begin g_bFoldersChanged := true; Why := ~; Exit; end;"),
    ('ReleasePlaceholders', 'the files setup keeps are not let go, and so stay',
     "^procedure ReleasePlaceholders(); var index: Integer; begin for index := GetArrayLength(g_aPlaceholders) - 1 downto 0 do ReleaseFolder(g_aPlaceholders[index]); SetArrayLength(g_aPlaceholders, 0); end;$"),
    ('CurStepChanged', 'the files setup keeps go before the folders are read again after the files, or a service an earlier run set not to start stays so',
     "RaiseException(~); end; ReleasePlaceholders(); RestoreServiceStart();"),
    ('DeinitializeSetup', 'the files setup keeps are not let go when setup ends before the files, or the folders setup made are removed after a successful installation too',
     "^procedure DeinitializeSetup(); begin ReleasePlaceholders(); if not g_bFilesInstalled then RemoveFoldersMadeHere(); ReleaseHeldFolders(); end;$"),
    ('IsServiceInstalled', 'whether the service is installed is not asked of the service manager, so that a service found not as prepared is not kept from starting',
     "^function IsServiceInstalled(Name: String) : Boolean; var manager: SC_HANDLE; service: SC_HANDLE; begin service := OpenNamedService(Name, SERVICE_QUERY_CONFIG, manager); Result := (service <> 0); CloseServiceAndManager(service, manager); end;$"),
    # IsServiceInstalled asks through the two routines below, which the service routines of
    # master's 4c57c52f5f share (the move of R1 to R4 onto master, 28 October 2026): the
    # statement above held the manager opened, the service opened by its name with
    # SERVICE_QUERY_CONFIG and both let go in one routine, and these three hold the same.
    ('OpenNamedService', 'the service is not opened by its name through the service manager with the access asked, so that IsServiceInstalled does not answer whether it is installed',
     "^function OpenNamedService(Name: String; Access: cardinal; var Manager: SC_HANDLE) : SC_HANDLE; begin Result := 0; Manager := OpenServiceManager(); if (Manager = 0) then Exit; "
     "Result := OpenServiceW(Manager, Name, Access); if (Result = 0) then begin CloseServiceHandle(Manager); Manager := 0; end; end;$"),
    ('CloseServiceAndManager', 'the service or the connection to the service manager IsServiceInstalled opened is not let go',
     "^procedure CloseServiceAndManager(Service: SC_HANDLE; Manager: SC_HANDLE); begin if (Service <> 0) then CloseServiceHandle(Service); if (Manager <> 0) then CloseServiceHandle(Manager); end;$"),
    # The rest of that chain, and what the Ready page's stop rests on (check 9 of R1 to R4,
    # 31 October 2026: OpenServiceManager answering 0 without asking, IsServiceRunning
    # always false and GetServiceState always SERVICE_STOPPED each passed the checker and
    # the fixture, the first leaving every service found not as prepared free to start).
    # The Service Control Manager's functions, record and constants they use are held among
    # PROGRAM_FOLDER_DECLARATIONS.
    ('OpenServiceManager', 'the service manager is not asked, or a connection is taken where Windows refused one, so that IsServiceInstalled says no service is there and none found not as prepared is kept from starting',
     "^function OpenServiceManager() : SC_HANDLE; var lastError: Integer; who: String; begin Result := OpenSCManagerW(0, 0, SC_MANAGER_CONNECT); if (Result = 0) then begin "
     "lastError := DLLGetLastError(); if IsUninstaller() then who := 'The uninstaller' else who := 'Setup'; Log(~); SuppressibleMsgBox(~); end; end;$"),
    ('GetServiceState', 'the state the service is in is not the one Windows reports, so that the Ready page does not stop a running service, or does not say it stopped it',
     "^function GetServiceState(Name: String) : cardinal; var manager: SC_HANDLE; service: SC_HANDLE; status : SERVICE_STATUS_PROCESS; needed : cardinal; begin Result := 0; "
     "service := OpenNamedService(Name, SERVICE_QUERY_STATUS, manager); if (service = 0) then Exit; "
     "if QueryServiceStatusProcess(service, SERVICE_STATUS_PROCESS_INFO, status, SERVICE_STATUS_PROCESS_SIZE, needed) then Result := status.dwCurrentState; "
     "CloseServiceAndManager(service, manager); end;$"),
    ('IsServiceRunning', 'a running service is not taken for running, so that the Ready page neither stops it nor says, in a refusal, that it stopped it',
     "^function IsServiceRunning(Name: String) : Boolean; begin Result := (GetServiceState(Name) = SERVICE_RUNNING); end;$"),
    ('IsServiceStopped', 'a service that is not stopped is taken for stopped, so that setup does not ask it to stop before it prepares the folders and copies the files',
     "^function IsServiceStopped(Name: String) : Boolean; begin Result := (GetServiceState(Name) = SERVICE_STOPPED); end;$"),
    ('RemoveHeldFolder', 'an empty folder is removed through a link, or by a handle not read as that folder',
     "handle := HoldFolder(Folder, HM_REMOVE_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then begin Why := ~; Exit; end; szFinal := FinalPathOfHeld(handle); if HeldIsLink(handle, bKnown) then Why := ~ else if (szFinal = '') or (ParentFinal = '') or (CompareText(szFinal, AddBackslash(ParentFinal) + ExtractFileName(Folder)) <> 0) then Why := ~ else begin info := HM_DISPOSITION_DELETE_POSIX; Result := SetHeldInformation(handle, HM_FILE_DISPOSITION_INFO_EX, info, 4);"),
    ('NothingTurnedIn', 'a folder is walked after the files without being opened as itself, as no link, and as the folder its path names',
     "folderHandle := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); szFinal := ''; if folderHandle <> -1 then if not HeldIsLink(folderHandle, bKnown) then szFinal := FinalPathOfHeld(folderHandle); if (szFinal = '') or (CompareText(szFinal, Expected) <> 0) then begin"),
    ('GetCustomSetupExitCode', 'folders found not as setup left them after the files end in 10, which hMailServer.Updater rolls back, rather than 12',
     "if g_bFoldersNotAsPrepared then Result := 12 else if g_bStepsIncomplete and g_bDatabaseChanged then Result := 11 else if g_bStepsIncomplete then Result := 10 else Result := 0;"),
    ('FoldersChangedLine', 'the line hMailServer.Updater reads to put nothing back over changed folders is changed',
     "^function FoldersChangedLine(): String; begin Result := 'Setup found the folders it prepares changed (work item 78): the hMailServer service is set not to start, and no earlier version is to be put back over them.'; end;$"),
    ('KeepServiceFromStarting', 'a service that is not installed is configured, or the mark that lets a later run set it to start again is not left',
     "if not IsServiceInstalled('hMailServer') then Exit; if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= disabled', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then begin RegWriteDWordValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting', 1);"),
    ('RestoreServiceStart', 'a service setup did not set not to start is set to start, or one it did is left so for good',
     "if not RegQueryDWordValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting', mark) then Exit; if not IsServiceInstalled('hMailServer') then Exit; if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= auto', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then begin RegDeleteValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting');"),
    ('DataLinkTargetIsGrantable', 'a data folder\'s link to a drive\'s root is taken',
     "if IsVolumeRoot(szFinal) then begin SetLength(szNames, 2048); returned := 0; if not VolumePathNames(szFinal, szNames, 2048, returned) then begin Why := ~; Exit; end;"),
    ('DataLinkTargetIsGrantable', 'a data folder\'s link into, or over, the Windows folder, Program Files, the profiles or ProgramData\\Microsoft is taken',
     "0: szName := ExpandConstant('{win}'); 1: szName := ExpandConstant('{commonpf64}'); 2: szName := ExpandConstant('{commonpf32}'); 3: szName := ProfilesFolder(); 4: szName := AddBackslash(ExpandConstant('{commonappdata}')) + 'Microsoft'; end; szOther := ''; if szName <> '' then szOther := FinalPathOf(szName); if IsAtOrBelow(szFinal, szOther) or IsAtOrBelow(szOther, szFinal) then begin Why := ~; Exit; end;"),
    ('DataLinkTargetIsGrantable', 'a data folder\'s link to what holds ProgramData, or to what is, holds or is in the program folder outside its data folders, is taken',
     "szOther := FinalPathOf(ExpandConstant('{commonappdata}')); if IsAtOrBelow(szOther, szFinal) then begin Why := ~; Exit; end; szApp := FinalPathOfHeld(g_hProgramFolder); if IsAtOrBelow(szApp, szFinal) then begin Why := ~; Exit; end; if IsAtOrBelow(szFinal, szApp) then if not (IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Data') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Logs') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Temp') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Database')) then"),
    ('DatabaseTargetIsTheAdministrators', 'the folder a Database link leads to is taken whoever owns it or may change it',
     "if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then Why := ~ else if AclLetsEveryUserWrite(pDacl) then Why := ~;"),
    ('DatabaseTargetIsTheAdministrators', 'a file in the folder a Database link leads to is taken whoever owns it or may change it, or when it is a link',
     "if (rec.Attributes and HM_REPARSE_POINT) <> 0 then Why := ~ else begin handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);"),
    ('AclLetsEveryUserWrite', 'an entry that lets a group of every user write is missed, or one handed down to what is made in it alone counted',
     "if ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERIT_ONLY) = 0) and LetsEveryUserWrite(pAce) then Result := true; end else Result := true;"),
    ('IsAtOrBelow', 'a folder is taken for one inside another by a part of a name',
     "^function IsAtOrBelow(Path: String; Folder: String): Boolean; begin Result := false; if (Path = '') or (Folder = '') then Exit; Result := (CompareText(AddBackslash(Path), AddBackslash(Folder)) = 0) or (CompareText(Copy(Path, 1, Length(AddBackslash(Folder))), AddBackslash(Folder)) = 0); end;$"),
)

# The constants the R1 to R4 [Code] tests with, each held to its value (check 3 of R1 to
# R4, 27 October 2026: HM_REPARSE_POINT, HM_ACCESS_ALLOWED, HM_INHERIT_ONLY and
# HM_INHERITED_ACE changed by a digit passed the checker and the fixture, and with any of
# them no link, no allow entry, no entry that applies to a folder or no inherited entry
# would have been seen). Every HM_ constant the extension defines is one of these, and
# each is defined once: (name, its value as the script writes it, what Windows calls it).
PROGRAM_FOLDER_CONSTANTS = (
    ('HM_FILE_OBJECT', '1', 'SE_FILE_OBJECT'),
    ('HM_DACL_INFORMATION', '4', 'DACL_SECURITY_INFORMATION'),
    ('HM_ACL_SIZE_INFORMATION', '2', 'AclSizeInformation'),
    ('HM_ACCESS_ALLOWED', '0', 'ACCESS_ALLOWED_ACE_TYPE'),
    ('HM_OBJECT_INHERIT', '$01', 'OBJECT_INHERIT_ACE'),
    ('HM_CONTAINER_INHERIT', '$02', 'CONTAINER_INHERIT_ACE'),
    ('HM_NO_PROPAGATE_INHERIT', '$04', 'NO_PROPAGATE_INHERIT_ACE'),
    ('HM_INHERIT_ONLY', '$08', 'INHERIT_ONLY_ACE'),
    ('HM_FOLDER_WRITE_RIGHTS', '$500D0156', 'FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL'),
    ('HM_FOLDER_READ_RIGHTS', '$90000001', 'FILE_LIST_DIRECTORY | GENERIC_READ | GENERIC_ALL'),
    ('HM_HOLD_FOLDER_ACCESS', '$200A1', 'FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL'),
    ('HM_READ_CONTROL', '$20000', 'READ_CONTROL'),
    ('HM_SHARE_ALL_BUT_DELETE', '$3', 'FILE_SHARE_READ | FILE_SHARE_WRITE'),
    ('HM_SHARE_EVERYTHING', '$7', 'FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE'),
    ('HM_OPEN_EXISTING', '3', 'OPEN_EXISTING'),
    ('HM_HOLD_FOLDER_FLAGS', '$02200000', 'FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT'),
    ('HM_REPARSE_POINT', '$400', 'FILE_ATTRIBUTE_REPARSE_POINT'),
    ('HM_INVALID_ATTRIBUTES', '$FFFFFFFF', 'INVALID_FILE_ATTRIBUTES'),
    ('HM_OWNER_INFORMATION', '1', 'OWNER_SECURITY_INFORMATION'),
    ('HM_OWNER_AND_DACL_INFORMATION', '5', 'OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION'),
    ('HM_PROTECTED_DACL_INFORMATION', '$80000004', 'PROTECTED_DACL_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION'),
    ('HM_REMOVE_FROM_FOLDER_RIGHTS', '$100C0040', 'FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL'),
    ('HM_REPLACE_RIGHTS', '$100D0040', 'FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL'),
    ('HM_GET_REPARSE_POINT', '$000900A8', 'FSCTL_GET_REPARSE_POINT'),
    ('HM_DRIVE_REMOTE', '4', 'DRIVE_REMOTE'),
    ('HM_TRUSTED_INSTALLER_SID', "'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'", 'NT SERVICE\\TrustedInstaller, AdminUpdateChecks.cpp\'s TrustedInstallerSid'),
    ('HM_SERVICE_SID', "'S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809'", 'NT SERVICE\\hMailServer, as ServiceAccountTester holds it'),
    ('HM_STATUS_NAME_COLLISION', '$C0000035', 'STATUS_OBJECT_NAME_COLLISION'),
    ('HM_STATUS_NO_MEMORY', '$C0000017', 'STATUS_NO_MEMORY'),
    ('HM_MAKE_FOLDER_ACCESS', '$1200A1', 'FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE'),
    ('HM_FILE_CREATE', '2', 'FILE_CREATE'),
    ('HM_MAKE_FOLDER_OPTIONS', '$00200021', 'FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT'),
    ('HM_OBJ_CASE_INSENSITIVE', '$40', 'OBJ_CASE_INSENSITIVE'),
    ('HM_OBJECT_ATTRIBUTES_SIZE', '24', 'sizeof(OBJECT_ATTRIBUTES) in a 32-bit process'),
    ('HM_ACL_REVISION', '2', 'ACL_REVISION'),
    ('HM_LPTR', '$40', 'LPTR'),
    ('HM_MAXDWORD', '$FFFFFFFF', 'MAXDWORD'),
    ('HM_ACCESS_DENIED', '1', 'ACCESS_DENIED_ACE_TYPE'),
    ('HM_INHERITED_ACE', '$10', 'INHERITED_ACE'),
    ('HM_INHERITED_FLAG_BIT', '$1000', 'INHERITED_ACE where it sits in ACE_START\'s TypeFlagsSize'),
    ('HM_FILE_ALL_ACCESS', '$1F01FF', 'FILE_ALL_ACCESS'),
    ('HM_RIGHTS_READ_EXECUTE', '$1200A9', 'FILE_GENERIC_READ | FILE_EXECUTE, ServiceAccount::RightsReadExecute'),
    ('HM_DACL_PROTECTED', '$1000', 'SE_DACL_PROTECTED'),
    ('HM_PERSISTENT_ACLS', '$8', 'FILE_PERSISTENT_ACLS'),
    ('HM_VOLUME_NAME_GUID', '1', 'VOLUME_NAME_GUID'),
    ('HM_CHANGE_PERMISSIONS_RIGHTS', '$100C0000', 'WRITE_DAC | WRITE_OWNER | GENERIC_ALL'),
    ('HM_HKEY_LOCAL_MACHINE', '$80000002', 'HKEY_LOCAL_MACHINE'),
    ('HM_REG_OPTION_VOLATILE', '1', 'REG_OPTION_VOLATILE'),
    ('HM_KEY_SET_VALUE_64', '$0102', 'KEY_SET_VALUE | KEY_WOW64_64KEY'),
    ('HM_REG_SZ', '1', 'REG_SZ'),
    ('HM_RESTART_KEY', "'SOFTWARE\\hMailServerSetupRestart'", 'the volatile key that keeps setup from installing again before a restart'),
    ('HM_PLACEHOLDER_ACCESS', '$80010000', 'GENERIC_READ | DELETE'),
    ('HM_CREATE_NEW', '1', 'CREATE_NEW'),
    ('HM_PLACEHOLDER_FLAGS', '$04000102', 'FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN'),
    ('HM_PLACEHOLDER_NAME', "'hMailServer-setup-in-progress.tmp'", 'the file setup keeps in a folder it hardened in place'),
    ('HM_REMOVE_FOLDER_ACCESS', '$00010080', 'DELETE | FILE_READ_ATTRIBUTES'),
    ('HM_FILE_DISPOSITION_INFO', '4', 'FileDispositionInfo'),
    ('HM_FILE_DISPOSITION_INFO_EX', '21', 'FileDispositionInfoEx'),
    ('HM_DISPOSITION_DELETE_POSIX', '3', 'FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS'),
    ('HM_OPEN_FOLLOWING_FLAGS', '$02000000', 'FILE_FLAG_BACKUP_SEMANTICS'),
    ('HM_WRITE_DAC_ACCESS', '$00060080', 'READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES'),
    ('HM_UNPROTECTED_DACL_INFORMATION', '$20000004', 'UNPROTECTED_DACL_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION'),
)

# The Windows functions and records the R1 to R4 [Code] reads and changes folders with,
# each held to its declaration (a statement as PROGRAM_FOLDER_STATEMENTS' are, matched
# once anywhere in the extension): a function declared from another DLL or with another
# parameter, or a record's field moved, reads the wrong thing without a word. Every
# external function a routine of PROGRAM_FOLDER_CODE calls is one of these.
PROGRAM_FOLDER_DECLARATIONS = (
    "ACL_SIZE_INFO = record AceCount : cardinal; AclBytesInUse : cardinal; AclBytesFree : cardinal; end;",
    "ACE_START = record TypeFlagsSize : cardinal; Mask : cardinal; end;",
    "HM_OBJECT_ATTRIBUTES = record nLength : cardinal; RootDirectory : Integer; ObjectName : cardinal; Attributes : cardinal; SecurityDescriptor : cardinal; SecurityQualityOfService : cardinal; end;",
    "HM_UNICODE_STRING = record Lengths : cardinal; Buffer : cardinal; end;",
    "HM_IO_STATUS_BLOCK = record Status : cardinal; Information : cardinal; end;",
    "HM_FILE_INFORMATION = record dwFileAttributes : cardinal; ftCreationTimeLow : cardinal; ftCreationTimeHigh : cardinal; ftLastAccessTimeLow : cardinal; "
    "ftLastAccessTimeHigh : cardinal; ftLastWriteTimeLow : cardinal; ftLastWriteTimeHigh : cardinal; dwVolumeSerialNumber : cardinal; nFileSizeHigh : cardinal; "
    "nFileSizeLow : cardinal; nNumberOfLinks : cardinal; nFileIndexHigh : cardinal; nFileIndexLow : cardinal; end;",
    "function ReadFolderPermissions(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; ppsidOwner: cardinal; ppsidGroup: cardinal; var ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal; external 'GetNamedSecurityInfoW@advapi32.dll stdcall';",
    "function ReadAclSize(pAcl: cardinal; var AclInformation: ACL_SIZE_INFO; nAclInformationLength: cardinal; dwAclInformationClass: cardinal): Boolean; external 'GetAclInformation@advapi32.dll stdcall';",
    "function ReadAclEntry(pAcl: cardinal; dwAceIndex: cardinal; var pAce: cardinal): Boolean; external 'GetAce@advapi32.dll stdcall';",
    "procedure CopyAceStart(var Destination: ACE_START; Source: cardinal; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
    "function SidOfText(StringSid: String; var Sid: cardinal): Boolean; external 'ConvertStringSidToSidW@advapi32.dll stdcall';",
    "function SidsAreEqual(Sid1: cardinal; Sid2: cardinal): Boolean; external 'EqualSid@advapi32.dll stdcall';",
    "function FreeWindowsMemory(hMem: cardinal): cardinal; external 'LocalFree@kernel32.dll stdcall';",
    "function HoldFolder(lpFileName: String; dwDesiredAccess: cardinal; dwShareMode: cardinal; lpSecurityAttributes: cardinal; dwCreationDisposition: cardinal; dwFlagsAndAttributes: cardinal; hTemplateFile: cardinal): Integer; external 'CreateFileW@kernel32.dll stdcall';",
    "function ReleaseFolder(hObject: Integer): Boolean; external 'CloseHandle@kernel32.dll stdcall';",
    "function FolderAttributes(lpFileName: String): cardinal; external 'GetFileAttributesW@kernel32.dll stdcall';",
    "function SecurityDescriptorOfText(StringSecurityDescriptor: String; StringSDRevision: cardinal; var SecurityDescriptor: cardinal; SecurityDescriptorSize: cardinal): Boolean; external 'ConvertStringSecurityDescriptorToSecurityDescriptorW@advapi32.dll stdcall';",
    "function WriteFolderSecurity(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; psidOwner: cardinal; psidGroup: cardinal; pDacl: cardinal; pSacl: cardinal): cardinal; external 'SetNamedSecurityInfoW@advapi32.dll stdcall';",
    "function ReadHeldSecurity(Handle: Integer; ObjectType: cardinal; SecurityInfo: cardinal; var ppsidOwner: cardinal; ppsidGroup: cardinal; var ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal; external 'GetSecurityInfo@advapi32.dll stdcall';",
    "function ControlOfDescriptor(pSecurityDescriptor: cardinal; var pControl: Word; var lpdwRevision: cardinal): Boolean; external 'GetSecurityDescriptorControl@advapi32.dll stdcall';",
    "function SidToText(Sid: cardinal; var StringSid: cardinal): Boolean; external 'ConvertSidToStringSidW@advapi32.dll stdcall';",
    "function WideTextLength(lpString: cardinal): Integer; external 'lstrlenW@kernel32.dll stdcall';",
    "procedure CopyWideText(Destination: String; Source: cardinal; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
    "procedure CopyBytes(Destination: cardinal; Source: cardinal; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
    "procedure WriteAceStart(Destination: cardinal; var Source: ACE_START; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
    "function AllocateWindowsMemory(uFlags: cardinal; uBytes: cardinal): cardinal; external 'LocalAlloc@kernel32.dll stdcall';",
    "function StartAcl(pAcl: cardinal; nAclLength: cardinal; dwAclRevision: cardinal): Boolean; external 'InitializeAcl@advapi32.dll stdcall';",
    "function AppendAce(pAcl: cardinal; dwAceRevision: cardinal; dwStartingAceIndex: cardinal; pAceList: cardinal; nAceListLength: cardinal): Boolean; external 'AddAce@advapi32.dll stdcall';",
    "function AppendAllowAce(pAcl: cardinal; dwAceRevision: cardinal; AceFlags: cardinal; AccessMask: cardinal; pSid: cardinal): Boolean; external 'AddAccessAllowedAceEx@advapi32.dll stdcall';",
    "function ReadReparseData(hDevice: Integer; dwIoControlCode: cardinal; lpInBuffer: cardinal; nInBufferSize: cardinal; lpOutBuffer: String; nOutBufferSize: cardinal; var lpBytesReturned: cardinal; lpOverlapped: cardinal): Boolean; external 'DeviceIoControl@kernel32.dll stdcall';",
    "function MakeFileRelative(var FileHandle: Integer; DesiredAccess: cardinal; var ObjectAttributes: HM_OBJECT_ATTRIBUTES; var IoStatusBlock: HM_IO_STATUS_BLOCK; "
    "AllocationSize: cardinal; FileAttributes: cardinal; ShareAccess: cardinal; CreateDisposition: cardinal; CreateOptions: cardinal; EaBuffer: cardinal; EaLength: cardinal): cardinal; "
    "external 'NtCreateFile@ntdll.dll stdcall';",
    "procedure CopyTextTo(Destination: cardinal; Source: String; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
    "procedure WriteNameRecord(Destination: cardinal; var Source: HM_UNICODE_STRING; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
    "function VolumeOfHeldFolder(hFile: Integer; lpVolumeNameBuffer: cardinal; nVolumeNameSize: cardinal; var lpVolumeSerialNumber: cardinal; var lpMaximumComponentLength: cardinal; var lpFileSystemFlags: cardinal; lpFileSystemNameBuffer: cardinal; nFileSystemNameSize: cardinal): Boolean; external 'GetVolumeInformationByHandleW@kernel32.dll stdcall';",
    "function KindOfDrive(lpRootPathName: String): cardinal; external 'GetDriveTypeW@kernel32.dll stdcall';",
    "function LookupNameOfSid(lpSystemName: cardinal; Sid: cardinal; Name: String; var cchName: cardinal; ReferencedDomainName: String; var cchReferencedDomainName: cardinal; var peUse: cardinal): Boolean; external 'LookupAccountSidW@advapi32.dll stdcall';",
    "function ReadHeldInformation(hFile: Integer; var lpFileInformation: HM_FILE_INFORMATION): Boolean; external 'GetFileInformationByHandle@kernel32.dll stdcall';",
    "function FinalPathOfHandle(hFile: Integer; lpszFilePath: String; cchFilePath: cardinal; dwFlags: cardinal): cardinal; external 'GetFinalPathNameByHandleW@kernel32.dll stdcall';",
    "function MakeVolatileKey(hKey: cardinal; lpSubKey: String; Reserved: cardinal; lpClass: cardinal; dwOptions: cardinal; samDesired: cardinal; lpSecurityAttributes: cardinal; var phkResult: cardinal; var lpdwDisposition: cardinal): Integer; external 'RegCreateKeyExW@advapi32.dll stdcall';",
    "function WriteKeyText(hKey: cardinal; lpValueName: String; Reserved: cardinal; dwType: cardinal; lpData: String; cbData: cardinal): Integer; external 'RegSetValueExW@advapi32.dll stdcall';",
    "function CloseKey(hKey: cardinal): Integer; external 'RegCloseKey@advapi32.dll stdcall';",
    "function SetHeldInformation(hFile: Integer; FileInformationClass: cardinal; var lpFileInformation: cardinal; dwBufferSize: cardinal): Boolean; external 'SetFileInformationByHandle@kernel32.dll stdcall';",
    "function VolumePathNames(lpszVolumeName: String; lpszVolumePathNames: String; cchBufferLength: cardinal; var lpcchReturnLength: cardinal): Boolean; external 'GetVolumePathNamesForVolumeNameW@kernel32.dll stdcall';",
    "function WriteHeldSecurity(Handle: Integer; ObjectType: cardinal; SecurityInfo: cardinal; psidOwner: cardinal; psidGroup: cardinal; pDacl: cardinal; pSacl: cardinal): cardinal; external 'SetSecurityInfo@advapi32.dll stdcall';",
    # The Service Control Manager's, which IsServiceInstalled, which PROGRAM_FOLDER_SHARED
    # names, asks through, and GetServiceState, held by statements (check 9 of R1 to R4, 31
    # October 2026): its functions, the record QueryServiceStatusEx fills and the constants
    # those routines pass and compare with, each to winsvc.h's value. The constants are
    # Windows' names, not HM_, and so held here, each once, rather than among
    # PROGRAM_FOLDER_CONSTANTS.
    "function OpenSCManagerW(lpMachineName: cardinal; lpDatabaseName: cardinal; dwDesiredAccess: cardinal): SC_HANDLE; external 'OpenSCManagerW@advapi32.dll stdcall';",
    "function OpenServiceW(hSCManager: SC_HANDLE; lpServiceName: String; dwDesiredAccess: cardinal): SC_HANDLE; external 'OpenServiceW@advapi32.dll stdcall';",
    "function CloseServiceHandle(hSCObject: SC_HANDLE): Boolean; external 'CloseServiceHandle@advapi32.dll stdcall';",
    "function QueryServiceStatusProcess(hService: HANDLE; InfoLevel: cardinal; var Buffer: SERVICE_STATUS_PROCESS; cbBufSize: cardinal; var pcbBytesNeeded: cardinal): boolean; external 'QueryServiceStatusEx@advapi32.dll stdcall';",
    "SERVICE_STATUS_PROCESS = record dwServiceType : cardinal; dwCurrentState : cardinal; dwControlsAccepted : cardinal; dwWin32ExitCode : cardinal; dwServiceSpecificExitCode : cardinal; "
    "dwCheckPoint : cardinal; dwWaitHint : cardinal; dwProcessId : cardinal; dwServiceFlags : cardinal; end;",
    "SC_MANAGER_CONNECT = $0001;",
    "SERVICE_QUERY_CONFIG = $0001;",
    "SERVICE_QUERY_STATUS = $0004;",
    "SERVICE_STOPPED = $00000001;",
    "SERVICE_RUNNING = $00000004;",
    "SERVICE_STATUS_PROCESS_INFO = 0;",
    "SERVICE_STATUS_PROCESS_SIZE = 36;",
)

# Every routine of R1 to R4, held whole by a digest of its code - comments dropped, each
# run of white space one space, every string as it is written (PROGRAM_FOLDER_CODE's
# first 16 hex digits of SHA-256 over UTF-8; code_digest) - so that no change of one,
# however small, passes until it has been reviewed against work item 78 and its digest
# written here and in InstallerPackaging.cs (python build/check-installer-scripts.py
# --digests prints them as they are). Check 3 of R1 to R4 (27 October 2026) found that a
# one-line change of a helper the statements did not name passed the checker and the
# fixture, and a sweep of every if, while, Result and Exit of these routines on 27
# October 2026 found 433 such changes that the statements alone let through. The
# routines are those the entry points reach (PROGRAM_FOLDER_ENTRY_POINTS), each one this
# table or PROGRAM_FOLDER_SHARED names: a new helper of R1 to R4 is held, or said to be
# another's, before the check passes.
PROGRAM_FOLDER_ENTRY_POINTS = ('CheckProgramFolders', 'PrepareProgramFolders', 'FoldersStillAsPrepared', 'RemoveFoldersMadeHere', 'KeepServiceFromStarting',
                               'FoldersWereAsPrepared', 'FolderMadeBySetup', 'SetupMadeFolder', 'SetupMadeProgramFolder', 'SetupMadeBin', 'SetupMadeControlPanel',
                               'PreviousInstallationFolder',
                               'ReleasePlaceholders', 'RestoreServiceStart', 'FoldersChangedLine')
# The routines the entry points reach that are not R1 to R4's own, which the rest of setup
# shares, and why they are not held here: (routine, why).
PROGRAM_FOLDER_SHARED = (
    ('CheckProgramFolderPermissions', 'the folder page\'s note of 6.3.7 on a program folder another account may write, which decides nothing'),
    ('IsServiceInstalled', 'whether the service is installed, which the rest of setup asks too'),
)
PROGRAM_FOLDER_CODE = (
    ('AccountNameOfSid', 'd7f15a73b2a52847'),
    ('AclLetsEveryUserWrite', '7fd37319681c8b53'),
    ('AclLetsOthers', '6b00430bd84cb1ab'),
    ('AddAsOwnEntry', 'f0c0620c1378f54d'),
    ('AddRestartFolder', '49938388fed2521d'),
    ('BroadGroupSid', '8b255776e8013335'),
    ('CheckFoldersBeforeTheStop', 'a2e38e9db48a13e5'),
    ('CheckProgramFolders', '7e0c3340809c7fec'),
    ('CheckWhatElseIsInside', 'f6745410cf02c64f'),
    ('CheckWhatIsInside', 'd53f9875ea5c5d75'),
    ('ControlPanelIsInstalled', '3ab47178b35c2ef7'),
    ('DatabaseFilesWhy', '385a1b53ff151af2'),
    ('DatabaseTargetIsTheAdministrators', '5aafe7086f1634b2'),
    ('DataFolderIsAllowed', 'b1088c905684623f'),
    ('DataFolderThatInherits', '66c2cd8de8d2f08b'),
    ('DataLinkLeadsWhereAllowed', '0b4799996608f6cd'),
    ('DataLinkTargetIsGrantable', '49b5e0fd9d0bf941'),
    ('EveryUserMayRead', '311a6e77de030208'),
    ('EveryUserMayReadMadeIn', 'b9e6f38e684da9c5'),
    ('FinalPathOf', '9de48f21c9043b14'),
    ('FinalPathOfHeld', '2c5642212af4ef31'),
    ('FolderAboveIsAdministratorsAlone', '932c4bfbf248554a'),
    ('FolderHoldsNothing', '7335ca15ee314789'),
    ('FolderMadeBySetup', '3ed9e1d5025dba38'),
    ('FoldersChangedLine', '1c1a75a70ac8df34'),
    ('FoldersStillAsPrepared', '4a9a3f6b11bec5ed'),
    ('FoldersWereAsPrepared', '7b86c82e306d4585'),
    ('HandsDownWriteTo', '43f307a120cfd36a'),
    ('HardenDataFolder', 'fb9e890fb43679ae'),
    ('HardenDistrustedFolder', '3463104dcc3133d8'),
    ('HardenForRestart', '13ee441b644792ec'),
    ('HardenHeldFolder', '8e38a444233c3c88'),
    ('HeldFolderIsHardened', '3e667a4213e1312e'),
    ('HeldIsLink', 'dc9e0afb79a54681'),
    ('HeldOwnerIsTrusted', '6c22996dbd870f3e'),
    ('HoldFolderChain', '3488e3586d2e893d'),
    ('HoldForSetup', '950dbbf8ac5695d2'),
    ('HoldsOnlyEmptyFolders', 'b5d86f4cfb2c4d10'),
    ('InheritsNothing', 'ba9a5f635445f6e7'),
    ('IsAtOrAboveChecked', '65db442d814071a2'),
    ('IsAtOrBelow', 'ddb47adbbef155e5'),
    ('IsBroadSid', '38c2f0d7a83df514'),
    ('IsDataFolderName', 'ce6c435a26838422'),
    ('IsProgramFile', '219188cbc96d13c5'),
    ('IsRunFolderName', '618eab586bb72741'),
    ('IsScriptFile', 'da5441e710f716d7'),
    ('IsSettingsFile', 'a4a7b5a10ddbcbd9'),
    ('IsTrustedSid', 'b1e4972ae9631bbd'),
    ('IsVolumeRoot', '1d8d1a5e9fee3a54'),
    ('ItemIsIn', '3f4a4540977f6580'),
    ('KeepServiceFromStarting', 'cdbbbdc7f29148f1'),
    ('LetGoOfHeld', '83a63b6a0c82fba3'),
    ('LetsEveryUserWrite', 'f8c242f2bf668c73'),
    ('LinkIsAdministratorsAlone', 'c2c9823696671e05'),
    ('LinkRemedy', '421eb698557d16a3'),
    ('LinkTarget', '78cb462e54e3b55c'),
    ('MadeParentCount', '98517a4e3b3fbbfe'),
    ('MadeParentOffset', '3298776306b79064'),
    ('MadeWhereNamed', '98d8f43dacc9751a'),
    ('MakeFolderIn', '34f935fa5de4000f'),
    ('MakeFolders', '5d5589bec77ff4ad'),
    ('MakePlaceholder', '0b825c9ab1d96d0e'),
    ('NoteFolderOthersCouldChange', 'f6ed4de1f7a769ea'),
    ('NoteFoldersOthersCouldChange', '0649cbf4a983effb'),
    ('NoteStaleItem', '8faf68da80c3e0e9'),
    ('NothingTurnedIn', 'f4dba71743a003e6'),
    ('OthersCouldChange', '59cd57cd4f2192fb'),
    ('OwnEntriesLetOthers', '432884347c2a0dfa'),
    ('OwnerIsAllowed', '01366659a7062c32'),
    ('PreparedFolderIsAsLeft', 'b6fb831f8e861123'),
    ('PreparedFolderIsLink', 'ba199f0e24f474c1'),
    ('PrepareProgramFolders', '737bfa20ab72e9a9'),
    ('PreviousInstallationFolder', 'e5316bef61ccd228'),
    ('ProfilesFolder', '510631cc6e496304'),
    ('ProgramFolderMadeAsNamed', '4126d0c1e6214bee'),
    ('ProgramFolderWaitsForTheFiles', 'a89df9530e50e3d1'),
    ('RebuildFolderOfUntrustedOwner', 'dedaf61436fb3d03'),
    ('RecordRestartNeeded', '235f797c1cefa764'),
    ('RelativeLinkTarget', 'e71b5f87fff8e5ca'),
    ('ReleaseHeldFolders', 'f8c80f82d876588b'),
    ('ReleasePlaceholders', '676d729ff0128f12'),
    ('RemakeEmptyFolder', 'f84455156da5337c'),
    ('RemoveAndMakeAgain', '34b0b79037557863'),
    ('RemoveFoldersMadeHere', '5d8963b8738f5484'),
    ('RemoveHeldFolder', '149850770478f53c'),
    ('RemoveMadeFolder', 'b58144954352dd5e'),
    ('RestartNeededFor', '4ef98799db2b5bf9'),
    ('RestartRecordedForWalked', '070d8af3cd79ed43'),
    ('RestoreServiceStart', '1ea970d4b76972e3'),
    ('SayWriteEntriesKept', 'd8f9b5d24a6298b6'),
    ('SetupMadeBin', '1dbd327f1d6d18a6'),
    ('SetupMadeControlPanel', '1ac337150384c97d'),
    ('SetupMadeFolder', '96f12e0fabeb6aee'),
    ('SetupMadeProgramFolder', '7f70cdaa4f086ceb'),
    ('SidInName', '2a5e78371e8a3930'),
    ('SidIs', '389072b7476600fc'),
    ('SidNameAt', 'c8f922d592759b87'),
    ('SplitAtRoot', 'a9a5495b516a0a1a'),
    ('StaleEntryLetsOthers', 'ce957ad5f77e1596'),
    ('StaleItemIn', '31bbbc5f8143d368'),
    ('TakenEntriesLetOthers', 'eab4b339385484f1'),
    ('TextOfSidAt', 'f3c4acf66f69003e'),
    ('VolumeKeepsPermissions', '7816988ef8c6a23f'),
    ('WalkedFinal', '471561a0e7f5164a'),
    ('WalksAgree', 'b3961e81811d817c'),
    ('WriteOwnEntriesOn', '1b7260df1968d584'),
)


@functools.lru_cache(maxsize=None)
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


# A [Code] comment (// to the end of the line, { }, (* *)), a string, or a run of white
# space; an external function's declaration; a routine's header; a record's. A brace that
# opens {# is no comment but the Inno Setup preprocessor's inline directive, which it
# replaces with the text of its expression before ISCC compiles the Pascal, and so stays in
# a routine's code (check 6 of R1 to R4, 28 October 2026; ISPP_PLACES holds every one).
CODE_TOKEN = re.compile(r"//[^\n]*|\{(?!#)[^}]*\}|\(\*[\s\S]*?\*\)|'(?:[^'\n]|'')*'|\s+")
EXTERNAL_DECLARATION = re.compile(r"^(?:function|procedure)\s+(\w+)\s*(?:\([^()]*\))?\s*(?::\s*\w+\s*)?;\s*external\s+'[^']*'\s*;", re.M | re.I)
ROUTINE_HEADER = re.compile(r'^(?:function|procedure)\s+(\w+)\s*[(:;]', re.M | re.I)
RECORD_HEADER = re.compile(r'^\s*(\w+)\s*=\s*record\b', re.M | re.I)


def routine_code(body):
    """A routine's code as PROGRAM_FOLDER_CODE's digest reads it: its comments
    dropped, each run of white space, a comment's place among it, one space, and every
    string as it is written."""
    pieces = []
    last = ''
    at = 0
    for match in CODE_TOKEN.finditer(body):
        between = body[at:match.start()]
        if between:
            pieces.append(between)
            last = between[-1]
        token = match.group(0)
        if token.startswith("'"):
            pieces.append(token)
            last = token[-1]
        elif pieces and last != ' ':
            pieces.append(' ')
            last = ' '
        at = match.end()
    if body[at:]:
        pieces.append(body[at:])
    return ''.join(pieces).strip()


def code_digest(body):
    """PROGRAM_FOLDER_CODE's digest of a routine: the first 16 hex digits of the SHA-256 of
    its code (routine_code) in UTF-8."""
    return hashlib.sha256(routine_code(body).encode('utf-8')).hexdigest()[:16]


def code_words(body):
    """The words of a routine's code, its comments and strings left out, in lower case."""
    return set(word.lower() for word in re.findall(r'\w+', CODE_TOKEN.sub(lambda match: "''" if match.group(0).startswith("'") else ' ', body)))


@functools.lru_cache(maxsize=4)
def routine_bodies(ext):
    """Each routine of the extension, its external declarations left out, by its name in
    lower case: (its name, the text pascal_routine gives of its first definition)."""
    externals = set(match.group(1).lower() for match in EXTERNAL_DECLARATION.finditer(ext))
    closing = re.compile(r'^end;', re.M)
    bodies = {}
    for match in ROUTINE_HEADER.finditer(ext):
        name = match.group(1)
        if name.lower() in externals or name.lower() in bodies:
            continue
        end = closing.search(ext, match.end())
        if end:
            bodies[name.lower()] = (name, ext[match.start():end.end()])
    return bodies


def program_folder_reach(ext):
    """The routines R1 to R4's entry points reach (PROGRAM_FOLDER_ENTRY_POINTS, through
    every routine but PROGRAM_FOLDER_SHARED's), in the order found; the external
    functions and the records the reached routines but the shared use; and how many
    times each routine name is defined."""
    externals = dict((match.group(1).lower(), match.group(1)) for match in EXTERNAL_DECLARATION.finditer(ext))
    defined = {}
    for match in ROUTINE_HEADER.finditer(ext):
        if match.group(1).lower() not in externals:
            defined[match.group(1).lower()] = defined.get(match.group(1).lower(), 0) + 1
    bodies = routine_bodies(ext)
    names = dict((key, value[0]) for key, value in bodies.items())
    records = dict((match.group(1).lower(), match.group(1)) for match in RECORD_HEADER.finditer(ext))
    shared = set(name.lower() for name, _ in PROGRAM_FOLDER_SHARED)
    reached = []
    calls = set()
    types = set()
    todo = [name.lower() for name in PROGRAM_FOLDER_ENTRY_POINTS]
    while todo:
        name = todo.pop(0)
        if name in [one.lower() for one in reached]:
            continue
        reached.append(names.get(name, name))
        body = bodies.get(name, (None, None))[1]
        if body is None or name in shared:
            continue
        for word in sorted(code_words(body)):
            if word == name:
                continue
            if word in externals:
                calls.add(externals[word])
            elif word in defined:
                todo.append(word)
            elif word in records:
                types.add(records[word])
    return reached, calls, types, defined


def check_program_folder_code(texts, problems):
    """Work item 78's R1 to R4, whole: every constant, Windows declaration and routine
    (PROGRAM_FOLDER_CONSTANTS, PROGRAM_FOLDER_DECLARATIONS, PROGRAM_FOLDER_CODE)."""
    ext = texts.get(EXTENSION, '')
    defined = re.findall(r'^[ \t]*(HM_\w+)[ \t]*=[ \t]*([^;\n]*);', ext, re.M)
    held = dict((name, value) for name, value, _ in PROGRAM_FOLDER_CONSTANTS)
    for name, value, meaning in PROGRAM_FOLDER_CONSTANTS:
        values = [found.strip() for one, found in defined if one == name]
        if len(values) != 1:
            problems.append('%s: %s (%s) is defined %d times, where work item 78 holds it once, to %s' % (EXTENSION, name, meaning, len(values), value))
        elif values[0] != value:
            problems.append('%s: %s (%s) is %s, where work item 78 holds it to %s' % (EXTENSION, name, meaning, values[0], value))
    for name, value in defined:
        if name not in held:
            problems.append('%s: %s = %s is a constant work item 78\'s check does not hold; hold it in PROGRAM_FOLDER_CONSTANTS and InstallerPackaging.cs' % (EXTENSION, name, value.strip()))
    trusted = re.search(r'TrustedInstallerSid\s*=\s*L"(S-1-5-80-[0-9-]+)"', texts.get(ADMIN_UPDATE_CHECKS, ''))
    if not trusted:
        problems.append('%s: TrustedInstallerSid, which HM_TRUSTED_INSTALLER_SID is held to, is not there' % ADMIN_UPDATE_CHECKS)
    elif held.get('HM_TRUSTED_INSTALLER_SID') != "'%s'" % trusted.group(1):
        problems.append('%s: HM_TRUSTED_INSTALLER_SID is held to %s, where the live update trusts %s' % (EXTENSION, held.get('HM_TRUSTED_INSTALLER_SID'), trusted.group(1)))
    # The groups of every user: BroadGroupSid's list is ServiceAccount.cpp's BroadSids_,
    # every one, and IsBroadSid counts a domain's Domain Users as IsBroad_ does (check 5
    # of R1 to R4, 27 October 2026: eight of them were left out).
    server = texts.get(SERVICE_ACCOUNT, '')
    broad = re.search(r'const wchar_t \*const BroadSids_\[\] = \{(.*?)\};', server, re.S)
    listed = re.findall(r"Result := '(S-[0-9-]+)';", pascal_routine(ext, 'BroadGroupSid') or '')
    if not broad:
        problems.append('%s: BroadSids_, which BroadGroupSid is held to, is not there' % SERVICE_ACCOUNT)
    else:
        sids = re.findall(r'L"(S-[0-9-]+)"', broad.group(1))
        if sorted(listed) != sorted(sids):
            problems.append('%s: BroadGroupSid counts %s as the groups of every user, where ServiceAccount.cpp\'s BroadSids_ counts %s'
                            % (EXTENSION, ', '.join(sorted(listed)), ', '.join(sorted(sids))))
    # A domain's Domain Users and Domain Guests (S-1-5-21-...-513 and -514): IsBroad_ counts
    # both, and IsBroadSid counts what IsBroad_ counts (check 6 of R1 to R4, 28 October 2026).
    server_rids = re.search(r'return sidText\.Left\(9\)\.CompareNoCase\(_T\("S-1-5-21-"\)\) == 0 && \((.*)\);', server)
    rids = sorted(re.findall(r'sidText\.Right\(4\) == _T\("(-\d{3})"\)', server_rids.group(1))) if server_rids else []
    if rids != ['-513', '-514']:
        problems.append('%s: IsBroad_ counts the domain groups %s among the groups of every user, where Domain Users (-513) and Domain Guests (-514) are held'
                        % (SERVICE_ACCOUNT, ', '.join(rids) or 'none'))
    installer_rids = re.search(r"Result := \(Length\(szSid\) > 13\) and \(CompareText\(Copy\(szSid, 1, 9\), 'S-1-5-21-'\) = 0\) and\s*\((.*?)\);",
                               pascal_routine(ext, 'IsBroadSid') or '', re.S)
    counted = sorted(re.findall(r"\(Copy\(szSid, Length\(szSid\) - 3, 4\) = '(-\d{3})'\)", installer_rids.group(1))) if installer_rids else []
    if counted != rids:
        problems.append('%s: IsBroadSid counts the domain groups %s, where ServiceAccount.cpp\'s IsBroad_ counts %s'
                        % (EXTENSION, ', '.join(counted) or 'none', ', '.join(rids) or 'none'))

    for declaration in PROGRAM_FOLDER_DECLARATIONS:
        count = len(re.findall(statement_pattern(declaration), ext))
        if count != 1:
            problems.append('%s: the declaration work item 78 holds to "%s" is there %d times, not once' % (EXTENSION, declaration[:90], count))
    declared = set(re.match(r'(?:function|procedure)?\s*(\w+)', declaration).group(1).lower() for declaration in PROGRAM_FOLDER_DECLARATIONS)

    reached, calls, types, counts = program_folder_reach(ext)
    for name in sorted(calls | types):
        if name.lower() not in declared:
            problems.append('%s: R1 to R4 use %s, whose declaration work item 78\'s check does not hold; hold it in PROGRAM_FOLDER_DECLARATIONS and InstallerPackaging.cs' % (EXTENSION, name))
    digests = dict((name.lower(), digest) for name, digest in PROGRAM_FOLDER_CODE)
    shared = set(name.lower() for name, _ in PROGRAM_FOLDER_SHARED)
    bodies = routine_bodies(ext)
    for name in reached:
        if name.lower() in shared:
            continue
        body = bodies.get(name.lower(), (None, None))[1]
        if body is None:
            problems.append('%s: %s, which R1 to R4 reach, is not there' % (EXTENSION, name))
        elif name.lower() not in digests:
            problems.append('%s: %s, which R1 to R4 reach, is neither held whole nor said to be shared; hold it in PROGRAM_FOLDER_CODE (its digest is %s) and '
                            'InstallerPackaging.cs, or name it in PROGRAM_FOLDER_SHARED' % (EXTENSION, name, code_digest(body)))
        elif counts.get(name.lower(), 0) != 1:
            problems.append('%s: %s is defined %d times' % (EXTENSION, name, counts.get(name.lower(), 0)))
        elif code_digest(body) != digests[name.lower()]:
            problems.append('%s: %s is not the code the reviews of work item 78 held (its digest is %s, held %s): a change of R1 to R4 is reviewed against the '
                            'work item, and its digest written in PROGRAM_FOLDER_CODE and InstallerPackaging.cs' % (EXTENSION, name, code_digest(body), digests[name.lower()]))
    for name in list(digests) + list(shared):
        if name not in [one.lower() for one in reached]:
            problems.append('%s: %s is held as a routine of R1 to R4, which no longer reach it; take it out of the check' % (EXTENSION, name))


def check_program_folders(texts, problems):
    """Work item 78's R1 to R4 (the docstring's "program folder")."""
    ext = texts.get(EXTENSION, '')
    routines = {}
    bodies = routine_bodies(ext)
    for name in PROGRAM_FOLDER_ROUTINES:
        # pascal_routine's text of each, read in one pass.
        routines[name] = bodies.get(name.lower(), (None, None))[1]
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
        # A word, not a part of one: "Depth >= 8" is not in "Depth >= 80" (check 3 of R1
        # to R4, 27 October 2026).
        if not re.search(r'(?<!\w)' + re.escape(word) + r'(?!\w)', chain):
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
    if len(re.findall(r'attributes\.SecurityDescriptor\s*:=', ext)) != 1:
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
    if 'CheckFoldersBeforeTheStop(' not in prepare or prepare.find('CheckFoldersBeforeTheStop(') > prepare.find('MakeFolders('):
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
    if ready.find('CheckFoldersBeforeTheStop(') < 0 or ready.find('CheckFoldersBeforeTheStop(') > ready.find('StopHMailServerService()'):
        problems.append('%s: the folders are checked after the service is stopped, so a refusal leaves a running server stopped' % EXTENSION)
    # PrepareToInstallRefusal, which the rest of setup shares, leaves early only with a
    # refusal, so that nothing is installed without the folders prepared (check 3 of R1
    # to R4, 27 October 2026).
    refusal = CODE_TOKEN.sub(lambda match: "'" + 'x' * (len(match.group(0)) - 2) + "'" if match.group(0).startswith("'") else ' ' * len(match.group(0)),
                             routines['PrepareToInstallRefusal'])
    for match in re.finditer(r'\bExit\s*;', refusal):
        if not re.search(r"\bResult\s*:=\s*'x+'[^;]*;\s*$", refusal[:match.start()]):
            problems.append('%s: PrepareToInstallRefusal leaves before the program folders are prepared without a refusal, so that setup installs into folders it '
                            'has not prepared' % EXTENSION)
            break

    # R1: made with its descriptor, recorded for the uninstaller, removed when not used.
    make = routines['MakeFolders']
    if not re.search(r"function\s+MakeFileRelative\(var\s+FileHandle:\s*Integer;\s*DesiredAccess:\s*cardinal;\s*var\s+ObjectAttributes:\s*HM_OBJECT_ATTRIBUTES;[^;]*;[^)]*\):\s*cardinal;\s*external\s+'NtCreateFile@ntdll\.dll stdcall';", ext):
        problems.append('%s: NtCreateFile is not declared with an OBJECT_ATTRIBUTES, so that a folder is made by its path rather than relative to the folder above it' % EXTENSION)
    if re.search(r"external\s+'CreateDirectory\w*@", ext) or re.search(r'\b(CreateDir|ForceDirectories)\(', make or ''):
        problems.append('%s: a folder of the program is made by its path (CreateDirectoryW), through any link put on the way since the walk' % EXTENSION)
    for word in ("'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)'", "'(A;OICI;0x1200a9;;;BU)'", 'EveryUserMayReadMadeIn(HeldHandle)', 'g_aFoldersMade',
                 'g_szBinMade := missing[index]', 'g_szControlPanelMade := missing[index]', 'MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status)', 'HeldIsLink(HeldHandle, bKnown)',
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
    if not re.search(r'if\s+not\s+g_bFilesInstalled\s+then\s+RemoveFoldersMadeHere\(\);\s*ReleaseHeldFolders\(\);', routines['DeinitializeSetup']):
        problems.append('%s: the empty folders setup made are not removed when the files were not installed, while the folders above them are still held' % EXTENSION)
    if not re.search(r'CurStep\s*=\s*ssPostInstall\s+then\s+begin\s+(?://[^\n]*\n\s*)*g_bFilesInstalled\s*:=\s*true;', step):
        problems.append('%s: the files are not marked installed at ssPostInstall' % EXTENSION)
    if 'DelTree(' in routines['RemoveFoldersMadeHere'] or 'DelTree(' in routines['RemoveMadeFolder']:
        problems.append('%s: RemoveFoldersMadeHere removes what is in a folder setup made' % EXTENSION)
    # Check 4 of R1 to R4 (27 October 2026): after the files nothing is removed, and before
    # them an empty folder is removed only through a handle read as that folder.
    if re.search(r'\b(?:DeleteFile|RemoveDir|DelTree|RemoveHeldFolder)\(', routines['NothingTurnedIn']):
        problems.append('%s: NothingTurnedIn removes what it finds after the files, by its path or through a folder made a junction' % EXTENSION)
    for name in ('CheckWhatIsInside', 'CheckWhatElseIsInside'):
        if re.search(r'\b(?:DeleteFile|RemoveDir|DelTree)\(', routines[name]):
            problems.append('%s: %s removes an empty folder by its path, not through a handle read as that folder (RemoveHeldFolder)' % (EXTENSION, name))
    # What another account could change is hardened, and the installation refused until
    # Windows has restarted, in one place, before the service is stopped, and only after the
    # check refused nothing; and a folder another account owns is taken there alone.
    callers = [name for name, (_, body) in bodies.items() if name != 'hardenforrestart' and re.search(r'\bHardenForRestart\(', body)]
    if callers != ['checkfoldersbeforethestop']:
        problems.append('%s: HardenForRestart is called from %s, where CheckFoldersBeforeTheStop alone may call it' % (EXTENSION, ', '.join(callers) or 'nowhere'))
    callers = [name for name, (_, body) in bodies.items() if name != 'checkprogramfolders' and re.search(r'\bCheckProgramFolders\(', body)]
    if callers != ['checkfoldersbeforethestop']:
        problems.append('%s: CheckProgramFolders is called from %s, so that what another account could change may go unhardened and unrefused' % (EXTENSION, ', '.join(callers) or 'nowhere'))
    adopting = [name for name, (_, body) in bodies.items() if re.search(r'\bHardenHeldFolder\([^;]*,\s*(?!false\s*,)[\w.]+\s*,\s*szWhy\)', body)]
    if adopting != ['hardenforrestart']:
        problems.append('%s: a folder another account owns is taken outside HardenForRestart (%s), and so installed into without a restart' % (EXTENSION, ', '.join(adopting) or 'nowhere'))
    if re.findall(r'g_bFoldersChanged\s*:=\s*false', ext) != ['g_bFoldersChanged := false'] or 'g_bFoldersChanged := false' not in check:
        problems.append('%s: whether a folder was found changed is cleared other than once, when CheckProgramFolders starts' % EXTENSION)
    # A folder made is recorded for its removal only once it is found where it was named
    # (check 3 of R1 to R4, 27 October 2026).
    named = make.find('MadeWhereNamed(')
    for word in ('g_aFoldersMade[', 'g_szBinMade :=', 'g_szControlPanelMade :=', 'g_aFoldersMadeAt[', 'g_szBinMadeAt :=', 'g_szControlPanelMadeAt :='):
        if named < 0 or not 0 <= named < make.find(word):
            problems.append('%s: MakeFolders records a folder it made (%s) before it has found it where it was named, so that one made through a link would be removed '
                            'by its path' % (EXTENSION, word.strip(' :=[')))
    # Whether Bin and ControlPanel are made again is decided before anything changes.
    if len(re.findall(r'g_bRemake(?:Bin|ControlPanel)\s*:=', ext)) != len(re.findall(r'g_bRemake(?:Bin|ControlPanel)\s*:=', check)):
        problems.append('%s: whether Bin or ControlPanel is made again is decided outside CheckProgramFolders, after something may have changed' % EXTENSION)
    # The last page runs nothing of an installation whose folders were not as setup left them.
    run = [line for _, line in section_entries(texts.get('section_run.iss', ''), 'Run') if 'postinstall' in (parameter(line, 'Flags') or '').lower().split()]
    if not run or any(parameter(line, 'Check') != 'FoldersWereAsPrepared' for line in run):
        problems.append('section_run.iss: an entry of the last page runs whether or not the folders setup prepared were as it left them (Check: FoldersWereAsPrepared)')
    if re.findall(r'g_bFoldersNotAsPrepared\s*:=\s*(\w+)', ext) != ['true']:
        problems.append('%s: whether the folders were found not as setup left them is set other than once, to true' % EXTENSION)

    # R2: in order - the program folder, then Bin and ControlPanel, then what is in them.
    order = [prepare.find(word) for word in ('HardenHeldFolder(g_szProgramFolderReal', 'HeldIsLink(g_hBin,', 'HardenHeldFolder(g_szBinReal', 'MakeFolders(g_szBinHeld',
                                             'HeldIsLink(g_hControlPanel,', 'HardenHeldFolder(g_szControlPanelReal', 'MakeFolders(g_szControlPanelHeld',
                                             'HeldIsLink(g_hProgramFolderPrepared,', 'HeldIsLink(g_hBinPrepared,', 'HeldIsLink(g_hControlPanelPrepared,',
                                             'CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true')]
    if min(order) < 0 or order != sorted(order):
        problems.append('%s: PrepareProgramFolders does not harden the program folder, then make or harden Bin and ControlPanel - each refused when it is a link '
                        'now - then read the three again for a link, then look at what is in them' % EXTENSION)
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


# What may run in CurStepChanged at ssInstall, before the files and so before the folders
# setup prepared are read again after them: the state it reads and the settings file's move.
SS_INSTALL_CALLS = ('IsNewDatabase', 'GetServiceAccountBefore', 'IsNewService', 'MoveIni')
# The sections whose entries Inno Setup processes during the install, and so may name a
# procedure to run before or after one (BeforeInstall, AfterInstall), or a function it
# calls then (Check:, {code:...}) - check 5 of R1 to R4, 27 October 2026.
INSTALL_SECTIONS = ('Files', 'Dirs', 'Icons', 'INI', 'Registry', 'Run', 'UninstallRun', 'InstallDelete', 'UninstallDelete', 'Tasks', 'Components', 'Types')
# What a [Run] entry without postinstall, which Inno Setup runs before ssPostInstall, may
# run: the .NET runtime this installer carries, from {tmp}.
RUN_BEFORE_POSTINSTALL = r'{tmp}\{#DOTNET_RUNTIME_FILE}'
# The functions of the [Code] those entries name, which Inno Setup calls during the
# install: the .NET runtime's check, the last page's, and the folders setup made, which
# section_dirs.iss gives the uninstaller.
INSTALL_TIME_FUNCTIONS = ('DotNetDesktopMissing', 'FoldersWereAsPrepared', 'FolderMadeBySetup', 'SetupMadeFolder', 'SetupMadeProgramFolder', 'SetupMadeBin',
                          'SetupMadeControlPanel')
# What lets go of what setup holds, or keeps in the folders it prepared, and where alone it
# is called: the check that starts again, the end of setup, the files setup keeps once the
# folders are read again after the files, and a folder made again or removed.
RELEASE_CALLERS = (('ReleaseHeldFolders', ('CheckProgramFolders', 'DeinitializeSetup')),
                   ('ReleasePlaceholders', ('CurStepChanged', 'DeinitializeSetup')),
                   ('LetGoOfHeld', ('RemakeEmptyFolder', 'RemoveMadeFolder')))
# The event functions Inno Setup calls between PrepareToInstall and ssPostInstall besides
# CurStepChanged, which may not be defined: CurInstallProgressChanged during the copy,
# NeedRestart after the files, icons, INI and registry entries (ProcessNeedRestartEvent),
# and RegisterPreviousData as the uninstall information is saved (RegisterUninstallInfo) -
# Inno Setup 6.7.3's Setup.Install.pas and Setup.MainForm.pas; check 7 of R1 to R4, 28
# October 2026.
# And RegisterExtraCloseApplicationsResources, which Inno Setup runs when it uses the
# Restart Manager, after PrepareToInstall returned '' and before the install
# (Setup.MainFunc.pas' RegisterResourcesWithRestartManager, reached from
# Setup.WizardForm.pas' QueryRestartManager) - check 8 of R1 to R4, 28 October 2026.
INSTALL_EVENTS_REFUSED = ('CurInstallProgressChanged', 'NeedRestart', 'RegisterPreviousData', 'RegisterExtraCloseApplicationsResources')
# The event functions of the pages up to the install, which run before the folders are
# looked at or as the install begins, CancelButtonClick, which Inno Setup runs on the
# installing page during the copy (check 8 of R1 to R4), and the functions the sections'
# entries name for Inno Setup to call during the install (INSTALL_TIME_FUNCTIONS): none runs
# a program or lets what setup holds go, itself or through any routine it calls (check 7 of
# R1 to R4).
PAGE_EVENTS_HELD = ('CurPageChanged', 'ShouldSkipPage', 'UpdateReadyMemo', 'CancelButtonClick')
# The [Setup] directives Inno Setup 6.7.3 evaluates as a Boolean expression, which may name
# functions of the [Code] (EvalDirectiveCheck): ChangesEnvironment and ChangesAssociations
# just after ssInstall, Uninstallable and CreateUninstallRegKey as the uninstall information
# is saved, before ssPostInstall, and the UsePrevious* five as the wizard starts. A function
# one names, and one a {code:...} of any [Setup] directive names (UninstallDisplayIcon,
# UninstallDisplayName and the others RegisterUninstallInfo expands before ssPostInstall),
# is held as one an entry names: it is one of INSTALL_TIME_FUNCTIONS, through every routine
# it reaches (check 8 of R1 to R4, 28 October 2026).
SETUP_EXPRESSION_DIRECTIVES = ('ChangesAssociations', 'ChangesEnvironment', 'CreateUninstallRegKey', 'Uninstallable', 'UsePreviousAppDir',
                               'UsePreviousGroup', 'UsePreviousSetupType', 'UsePreviousTasks', 'UsePreviousUserInfo')
SETUP_EXPRESSION_WORDS = ('yes', 'no', 'true', 'false', 'not', 'and', 'or')
PROGRAM_RUNNERS = ('Exec', 'ShellExec', 'ExecAsOriginalUser', 'ShellExecAsOriginalUser')


def routine_reach(bodies, name):
    """The routines of the [Code] that name calls, itself or through any routine it calls,
    and the PROGRAM_RUNNERS among them, each in lower case: read from the code of each,
    its comments and strings left out (code_words)."""
    runners = set(runner.lower() for runner in PROGRAM_RUNNERS)
    seen = set()
    todo = [name.lower()]
    while todo:
        body = bodies.get(todo.pop(), (None, ''))[1]
        for word in code_words(body):
            if word not in seen and word != name.lower() and (word in bodies or word in runners):
                seen.add(word)
                if word in bodies:
                    todo.append(word)
    return seen


def check_install_sections(texts, problems):
    """Work item 78 (check 4 of R1 to R4, 27 October 2026): what Inno Setup does outside
    the [Code] between PrepareProgramFolders and the look after the files keeps what setup
    prepared - no [Dirs] or [Files] entry under {app}, or a folder setup made, gives
    permissions (Inno Setup applies a Permissions: parameter as it makes the folder or copies
    the file, which would undo R1 and R2); no [Run] entry without postinstall, which Inno
    Setup runs before ssPostInstall, runs anything but the .NET runtime from {tmp}, with
    nothing of {app} or of a {code:} in it; no entry of any section names a procedure to
    run before or after it (BeforeInstall, AfterInstall), and the functions entries name
    for Inno Setup to call during the install (Check:, {code:...}), and those the [Setup]
    section names (a {code:...} in a directive, a function in a directive Inno Setup
    evaluates as an expression), are INSTALL_TIME_FUNCTIONS; ReleaseHeldFolders,
    ReleasePlaceholders and LetGoOfHeld are called where they are now alone
    (RELEASE_CALLERS); CurInstallProgressChanged, NeedRestart, RegisterPreviousData and
    RegisterExtraCloseApplicationsResources, which Inno Setup calls between PrepareToInstall
    and ssPostInstall, are not defined (INSTALL_EVENTS_REFUSED), and CurPageChanged,
    ShouldSkipPage, UpdateReadyMemo, CancelButtonClick and the functions the entries name
    run no program and let nothing go, by any routine they call (check 5 of R1 to R4; check
    7, 28 October 2026; check 8); every entry of the last page, in any script, is held to
    Check: FoldersWereAsPrepared (check 8); no file under {app} is
    registered during the copy (regserver, regtypelib); no folder but the eleven setup made
    is given to the uninstaller whatever is in it (uninsalwaysuninstall); and nothing runs
    at ssInstall but what reads the state and moves the settings file. And whether the
    folders were checked is set by CheckProgramFolders and cleared by PrepareProgramFolders
    alone, and RunPostInstallTasks runs /ProtectFolders."""
    for name, text in sorted(texts.items()):
        if not name.lower().endswith('.iss'):
            continue
        for section, key in (('Dirs', 'Name'), ('Files', 'DestDir')):
            for number, entry in section_entries(text, section):
                where = (parameter(entry, key) or '').lower()
                ours = where.startswith('{app}') or where.startswith('{code:')
                if ours and parameter(entry, 'Permissions') is not None:
                    problems.append('%s:%d: a [%s] entry under {app} gives permissions (%s), which Inno Setup applies before the folders are read again '
                                    'and which undo what setup prepared' % (name, number, section, parameter(entry, 'Permissions')))
                flags = (parameter(entry, 'Flags') or '').lower().split()
                if section == 'Files' and ours and any(flag in flags for flag in ('regserver', 'regtypelib', 'regserverx64', 'regtypelibx64')):
                    problems.append('%s:%d: a file under {app} is registered during the copy (%s), running it before the folders are read again' % (name, number, entry[:80]))
                if section == 'Dirs' and 'uninsalwaysuninstall' in flags and parameter(entry, 'Check') is None:
                    problems.append('%s:%d: a [Dirs] entry the uninstaller removes whatever made it (uninsalwaysuninstall with no Check of a folder setup made)' % (name, number))
        for number, entry in section_entries(text, 'Run'):
            flags = (parameter(entry, 'Flags') or '').lower().split()
            if (parameter(entry, 'Filename') or '').lower().startswith('{app}') and 'postinstall' not in flags:
                problems.append('%s:%d: a [Run] entry without postinstall runs a program from {app}, which Inno Setup runs before ssPostInstall and so before '
                                'the folders setup prepared are read again' % (name, number))
    ext = texts.get(EXTENSION, '')
    step = pascal_routine(ext, 'CurStepChanged') or ''
    start = step.find('if CurStep = ssInstall then')
    end = step.find('if CurStep = ssPostInstall then')
    if start < 0 or end < start:
        problems.append('%s: CurStepChanged has no ssInstall step before its ssPostInstall step' % EXTENSION)
    else:
        block = CODE_TOKEN.sub(' ', step[start:end])
        for call in re.findall(r'\b(\w+)\s*\(', block):
            if call not in SS_INSTALL_CALLS:
                problems.append('%s: CurStepChanged runs %s at ssInstall, before the files and before the folders setup prepared are read again' % (EXTENSION, call))
    # Check 5 of R1 to R4 (27 October 2026): what Inno Setup runs of the [Code] during the
    # install, and what it runs as a program, outside CurStepChanged. No entry of any
    # section names a procedure to run before or after it is installed (BeforeInstall,
    # AfterInstall), which would run between PrepareProgramFolders and the look after the
    # files; the functions entries name (Check:, {code:...}), which Inno Setup calls during
    # the install too, are those held here; no [Run] entry without postinstall, which runs
    # before ssPostInstall, runs anything but the .NET runtime from {tmp}, with nothing of
    # {app} or of a {code:} among its parameters; the holds and the files setup keeps are
    # let go only where they are let go now; and no event function Inno Setup calls during
    # the install but CurStepChanged is defined - CurInstallProgressChanged, NeedRestart,
    # RegisterPreviousData, RegisterExtraCloseApplicationsResources - and none of the pages'
    # (CurPageChanged, ShouldSkipPage, UpdateReadyMemo, CancelButtonClick), nor a function an
    # entry or the [Setup] section names, runs a program or lets them go, itself or through a
    # routine it calls (check 7 of R1 to R4, 28 October 2026; check 8).
    called = set()
    for name, text in sorted(texts.items()):
        if not name.lower().endswith('.iss') or '/' in name:
            continue
        for section in INSTALL_SECTIONS:
            for number, entry in section_entries(text, section):
                for hook in ('BeforeInstall', 'AfterInstall'):
                    if parameter(entry, hook) is not None:
                        problems.append('%s:%d: a [%s] entry runs %s as its %s, during the install and before the folders setup prepared are read again'
                                        % (name, number, section, parameter(entry, hook), hook))
                called.update(re.findall(r'\{code:(\w+)', entry))
                check = parameter(entry, 'Check')
                if check:
                    called.update(re.findall(r'\b(?!not\b|and\b|or\b)(\w+)\s*(?:\(|$|\s)', check.strip()))
        # The [Setup] section's functions: a {code:...} in any directive, and what a directive
        # Inno Setup evaluates as an expression names (check 8 of R1 to R4, 28 October 2026).
        for number, entry in section_entries(text, 'Setup'):
            directive = re.match(r'(\w+)\s*=\s*(.*)$', entry)
            if not directive:
                continue
            called.update(re.findall(r'\{code:(\w+)', directive.group(2)))
            if directive.group(1).lower() in [name.lower() for name in SETUP_EXPRESSION_DIRECTIVES]:
                value = re.sub(r"'(?:[^']|'')*'|\"[^\"]*\"", ' ', directive.group(2))
                called.update(word for word in re.findall(r'\b[A-Za-z_]\w*\b', value) if word.lower() not in SETUP_EXPRESSION_WORDS)
        for number, entry in section_entries(text, 'Run'):
            flags = (parameter(entry, 'Flags') or '').lower().split()
            if 'postinstall' in flags:
                # A last-page entry of any script, not of section_run.iss alone, runs nothing of
                # an installation whose folders were not as setup left them (check 8 of R1 to R4).
                if parameter(entry, 'Check') != 'FoldersWereAsPrepared':
                    problems.append('%s:%d: an entry of the last page runs %s whether or not the folders setup prepared were as it left them '
                                    '(Check: FoldersWereAsPrepared)' % (name, number, parameter(entry, 'Filename')))
                continue
            # The whole entry, since a parameter's doubled quotes end what parameter() reads.
            arguments = entry.lower()
            if (parameter(entry, 'Filename') or '') != RUN_BEFORE_POSTINSTALL or '{app}' in arguments or '{code:' in arguments:
                problems.append('%s:%d: a [Run] entry without postinstall runs %s %s, which Inno Setup runs before ssPostInstall and so before the folders setup '
                                'prepared are read again: only the .NET runtime from {tmp} may run then' % (name, number, parameter(entry, 'Filename'), arguments))
    if called != set(INSTALL_TIME_FUNCTIONS):
        problems.append('the functions entries of the installer\'s sections name for Inno Setup to call during the install are %s, where work item 78 holds them to %s'
                        % (', '.join(sorted(called)), ', '.join(sorted(INSTALL_TIME_FUNCTIONS))))
    bodies = routine_bodies(ext)
    for routine, callers in RELEASE_CALLERS:
        found = sorted(body[0] for key, body in bodies.items() if key != routine.lower() and re.search(r'\b%s\s*\(' % routine, CODE_TOKEN.sub(' ', body[1])))
        if found != sorted(callers):
            problems.append('%s: %s is called from %s, where work item 78 holds it to %s: what setup holds, or keeps in the folders it prepared, would be let go '
                            'before the look after the files' % (EXTENSION, routine, ', '.join(found) or 'nowhere', ', '.join(callers)))
    for event in INSTALL_EVENTS_REFUSED:
        if event.lower() in bodies:
            problems.append('%s: %s, which Inno Setup calls between PrepareToInstall and ssPostInstall, is defined: nothing of the [Code] runs then but '
                            'CurStepChanged\'s steps' % (EXTENSION, event))
    held = dict((call.lower(), call) for call in PROGRAM_RUNNERS + ('ProtectFolders',) + tuple(routine for routine, _ in RELEASE_CALLERS))
    for event in PAGE_EVENTS_HELD + INSTALL_TIME_FUNCTIONS:
        if event.lower() not in bodies:
            continue
        for call in sorted(routine_reach(bodies, event) & set(held)):
            problems.append('%s: %s, which Inno Setup calls before the folders are looked at, as the install begins or during it, runs %s, itself or through a '
                            'routine it calls' % (EXTENSION, event, held[call]))
    writers = {}
    for key, (routine, body) in routine_bodies(ext).items():
        for value in re.findall(r'g_bFoldersChecked\s*:=\s*(\w+)', body):
            writers.setdefault(routine, []).append(value)
    if writers != {'CheckProgramFolders': ['true'], 'PrepareProgramFolders': ['false']}:
        problems.append('%s: whether the folders were checked is set or cleared elsewhere than in CheckProgramFolders and PrepareProgramFolders (%s), so that the '
                        'check of the Ready page may be lost or run again after the service is stopped' % (EXTENSION, writers))
    if 'ProtectFolders();' not in (pascal_routine(ext, 'RunPostInstallTasks') or ''):
        problems.append('%s: RunPostInstallTasks no longer runs /ProtectFolders after the files' % EXTENSION)


def check_updater(texts, problems):
    """Work item 78 (check 4 of R1 to R4, 27 October 2026): hMailServer.Updater puts no
    earlier version back over folders the installer found changed - its exit code 12, or a
    refusal whose log has FoldersChangedLine - in either of its modes."""
    updater = texts.get(UPDATER, '')
    if not re.search(r'\bconst int InstallerFoldersChanged = 12;', updater):
        problems.append('%s: InstallerFoldersChanged is not 12, the exit code of GetCustomSetupExitCode for folders not as setup left them' % UPDATER)
    prefix = re.search(r'const std::wstring prefix = L"([^"]+)";', updater)
    line = re.search(r"function FoldersChangedLine\(\): String;\s*begin\s*Result := '([^']+)';", texts.get(EXTENSION, ''))
    if not prefix or not line or not line.group(1).startswith(prefix.group(1)):
        problems.append('%s: what FoldersFoundChanged looks for in the installer\'s log is not the start of FoldersChangedLine, so a refusal that found the folders '
                        'changed is rolled back' % UPDATER)
    rollbacks = [match.start() for match in re.finditer(r'Log\(L"Rolling back to hMailServer "', updater)]
    checks = [match.start() for match in re.finditer(r'if \(code == InstallerFoldersChanged \|\| \(code != 0 && FoldersFoundChanged\(installLog\)\)\)', updater)]
    if len(rollbacks) != 2 or len(checks) != 2 or not (checks[0] < rollbacks[0] < checks[1] < rollbacks[1]):
        problems.append('%s: an earlier version is put back over folders the installer found changed in one of the Updater\'s two modes' % UPDATER)
    came = re.search(r'bool ServiceCameBack\(int code, const Arguments &arguments\)\s*\{\s*if \(([^)]*)\)', updater)
    if not came or 'InstallerFoldersChanged' not in came.group(1):
        problems.append('%s: the Updater waits for a service the installer set not to start (exit code 12) to come back' % UPDATER)


# Work item 78 (check 6 of R1 to R4, 28 October 2026): ISCC compiles what the Inno Setup
# preprocessor makes of the scripts, not what they say, as MIDL compiles what the C
# preprocessor makes of hMailServer.idl (build/check-com-abi.py). A line whose first
# character that is not white space is '#' is the preprocessor's - #if 0 and #endif drop
# what is between them, #emit writes a line of its own, #include brings a file in, #define,
# #pragma and #sub change what the rest means - and so is every inline directive, {#...},
# which it replaces with the text of its expression wherever it stands, in a Pascal comment
# or string too. The rules above read the scripts as written; so that what they read is
# what ISCC compiles, every such line of every installer script is held to where it is
# now: the script, the routine of the [Code] it is in or else the section it is in, the
# line itself, the lines that are not blank before and after it, and how many times
# (ISPP_PLACES; --preprocessor prints the table as the tree is). One added, moved, changed
# or taken away is refused until it has been reviewed and the table written here and in
# InstallerPackaging.cs.
ISPP_LINE = re.compile(r'^[ \t]*#')
SECTION_HEADER = re.compile(r'^\s*\[([^\]]+)\]\s*$')
ISPP_PLACES = (
    ('hMailServer64.iss', '', '', '#define HMAILSERVERLIBS = GetEnv("hMailServerLibs")', '#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\\openssl-4.0.3\\out64\\bin"', 1),
    ('hMailServer64.iss', '', '#define DOTNET_CHANNEL "10.0"', '#define DOTNET_RUNTIME_FILE "windowsdesktop-runtime-" + DOTNET_CHANNEL + "-win-x64.exe"', '#include "section_setup.iss"', 1),
    ('hMailServer64.iss', '', '#define DOTNET_MAJOR "10"', '#define DOTNET_CHANNEL "10.0"', '#define DOTNET_RUNTIME_FILE "windowsdesktop-runtime-" + DOTNET_CHANNEL + "-win-x64.exe"', 1),
    ('hMailServer64.iss', '', '#define DOTNET_RUNTIME_FILE "windowsdesktop-runtime-" + DOTNET_CHANNEL + "-win-x64.exe"', '#include "section_setup.iss"', '#include "section_setup_64.iss"', 1),
    ('hMailServer64.iss', '', '#define HMAILSERVERLIBS = GetEnv("hMailServerLibs")', '#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\\openssl-4.0.3\\out64\\bin"', '#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + "\\postgresql-18.6\\builddir\\src\\interfaces\\libpq"', 1),
    ('hMailServer64.iss', '', '#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\\openssl-4.0.3\\out64\\bin"', '#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + "\\postgresql-18.6\\builddir\\src\\interfaces\\libpq"', '; The bundled .NET Desktop Runtime, defined in one place because it used to be', 1),
    ('hMailServer64.iss', '', '#include "section_components.iss"', '#include "section_files_common.iss"', '#include "section_files_64.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_custom_messages.iss"', '#include "section_languages.iss"', '#include "section_istool.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_dirs.iss"', '#include "section_run.iss"', '#include "section_uninstallrun.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_files_64.iss"', '#include "section_messages.iss"', '#include "section_ini.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_files_common.iss"', '#include "section_files_64.iss"', '#include "section_messages.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_icons.iss"', '#include "hMailServerInnoExtension.iss"', '', 1),
    ('hMailServer64.iss', '', '#include "section_ini.iss"', '#include "section_registry.iss"', '#include "section_dirs.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_istool.iss"', '#include "section_types.iss"', '#include "section_components.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_languages.iss"', '#include "section_istool.iss"', '#include "section_types.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_messages.iss"', '#include "section_ini.iss"', '#include "section_registry.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_registry.iss"', '#include "section_dirs.iss"', '#include "section_run.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_run.iss"', '#include "section_uninstallrun.iss"', '#include "section_icons.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_setup.iss"', '#include "section_setup_64.iss"', '#include "section_custom_messages.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_setup_64.iss"', '#include "section_custom_messages.iss"', '#include "section_languages.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_types.iss"', '#include "section_components.iss"', '#include "section_files_common.iss"', 1),
    ('hMailServer64.iss', '', '#include "section_uninstallrun.iss"', '#include "section_icons.iss"', '#include "hMailServerInnoExtension.iss"', 1),
    ('hMailServer64.iss', '', '; forward across minor and patch versions but never across a major version.', '#define DOTNET_MAJOR "10"', '#define DOTNET_CHANNEL "10.0"', 1),
    ('hMailServerARM64.iss', '', '#define DOTNET_CHANNEL "10.0"', '#define DOTNET_RUNTIME_FILE "windowsdesktop-runtime-" + DOTNET_CHANNEL + "-win-arm64.exe"', "; The version is read from section_setup_64.iss, so that the release's version", 1),
    ('hMailServerARM64.iss', '', '#define DOTNET_MAJOR "10"', '#define DOTNET_CHANNEL "10.0"', '#define DOTNET_RUNTIME_FILE "windowsdesktop-runtime-" + DOTNET_CHANNEL + "-win-arm64.exe"', 1),
    ('hMailServerARM64.iss', '', '#define HMAILSERVERLIBS = GetEnv("hMailServerLibs")', '#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\\openssl-4.0.3-arm64\\out64\\bin"', '#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + "\\postgresql-18.6-arm64\\builddir\\src\\interfaces\\libpq"', 1),
    ('hMailServerARM64.iss', '', '#define HM_APP_VERSION ReadIni(SourcePath + "\\section_setup_64.iss", "Setup", "AppVersion", "")', '#define HM_VERSION_INFO ReadIni(SourcePath + "\\section_setup_64.iss", "Setup", "VersionInfoVersion", "")', '#if HM_APP_VERSION == ""', 1),
    ('hMailServerARM64.iss', '', '#define HM_ARM64', '#define HMAILSERVERLIBS = GetEnv("hMailServerLibs")', '#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\\openssl-4.0.3-arm64\\out64\\bin"', 1),
    ('hMailServerARM64.iss', '', '#define HM_VERSION_INFO ReadIni(SourcePath + "\\section_setup_64.iss", "Setup", "VersionInfoVersion", "")', '#if HM_APP_VERSION == ""', '#error section_setup_64.iss names no AppVersion for the ARM64 installer to take', 1),
    ('hMailServerARM64.iss', '', '#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\\openssl-4.0.3-arm64\\out64\\bin"', '#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + "\\postgresql-18.6-arm64\\builddir\\src\\interfaces\\libpq"', '; As in hMailServer64.iss, which says why these are defined once: the ARM64 build', 1),
    ('hMailServerARM64.iss', '', '#endif', '#include "section_setup.iss"', '#include "section_setup_arm64.iss"', 1),
    ('hMailServerARM64.iss', '', '#error section_setup_64.iss names no AppVersion for the ARM64 installer to take', '#endif', '#include "section_setup.iss"', 1),
    ('hMailServerARM64.iss', '', '#if HM_APP_VERSION == ""', '#error section_setup_64.iss names no AppVersion for the ARM64 installer to take', '#endif', 1),
    ('hMailServerARM64.iss', '', '#include "section_components.iss"', '#include "section_files_common.iss"', '#include "section_files_arm64.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_custom_messages.iss"', '#include "section_languages.iss"', '#include "section_istool.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_dirs.iss"', '#include "section_run.iss"', '#include "section_uninstallrun.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_files_arm64.iss"', '#include "section_messages.iss"', '#include "section_ini.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_files_common.iss"', '#include "section_files_arm64.iss"', '#include "section_messages.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_icons.iss"', '#include "hMailServerInnoExtension.iss"', '', 1),
    ('hMailServerARM64.iss', '', '#include "section_ini.iss"', '#include "section_registry.iss"', '#include "section_dirs.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_istool.iss"', '#include "section_types.iss"', '#include "section_components.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_languages.iss"', '#include "section_istool.iss"', '#include "section_types.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_messages.iss"', '#include "section_ini.iss"', '#include "section_registry.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_registry.iss"', '#include "section_dirs.iss"', '#include "section_run.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_run.iss"', '#include "section_uninstallrun.iss"', '#include "section_icons.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_setup.iss"', '#include "section_setup_arm64.iss"', '#include "section_custom_messages.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_setup_arm64.iss"', '#include "section_custom_messages.iss"', '#include "section_languages.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_types.iss"', '#include "section_components.iss"', '#include "section_files_common.iss"', 1),
    ('hMailServerARM64.iss', '', '#include "section_uninstallrun.iss"', '#include "section_icons.iss"', '#include "hMailServerInnoExtension.iss"', 1),
    ('hMailServerARM64.iss', '', '; and build\\get-dotnet-runtime.ps1 -Platform ARM64 places this one).', '#define DOTNET_MAJOR "10"', '#define DOTNET_CHANNEL "10.0"', 1),
    ('hMailServerARM64.iss', '', '; has no ARM64 build, and an ARM64 process cannot load an x64 OLE DB provider.', '#define HM_ARM64', '#define HMAILSERVERLIBS = GetEnv("hMailServerLibs")', 1),
    ('hMailServerARM64.iss', '', '; stamp (RELEASE.md step 7) stays one file for the two installers.', '#define HM_APP_VERSION ReadIni(SourcePath + "\\section_setup_64.iss", "Setup", "AppVersion", "")', '#define HM_VERSION_INFO ReadIni(SourcePath + "\\section_setup_64.iss", "Setup", "VersionInfoVersion", "")', 1),
    ('hMailServerInnoExtension.iss', '', '#define HM_CONSTANTS_FILE AddBackslash(SourcePath) + "..\\source\\Server\\Common\\Application\\Constants.h"', '#define HM_REQUIRED_DB_VERSION ""', '#define HmConstantsHandle 0', 1),
    ('hMailServerInnoExtension.iss', '', '#define HM_REQUIRED_DB_VERSION ""', '#define HmConstantsHandle 0', '#define HmConstantsLine ""', 1),
    ('hMailServerInnoExtension.iss', '', '#define HmConstantsHandle 0', '#define HmConstantsLine ""', '#sub HmReadConstantsLine', 1),
    ('hMailServerInnoExtension.iss', '', '#define HmConstantsLine ""', '#sub HmReadConstantsLine', '#expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))', 1),
    ('hMailServerInnoExtension.iss', '', '#endif', '#endsub', '#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine', 1),
    ('hMailServerInnoExtension.iss', '', '#endif', '#if HM_REQUIRED_DB_VERSION == "" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000', '#error REQUIRED_DB_VERSION could not be read from Constants.h', 1),
    ('hMailServerInnoExtension.iss', '', '#endsub', '#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine', '#if HmConstantsHandle', 1),
    ('hMailServerInnoExtension.iss', '', '#error REQUIRED_DB_VERSION could not be read from Constants.h', '#endif', '[code]', 1),
    ('hMailServerInnoExtension.iss', '', '#expr FileClose(HmConstantsHandle)', '#endif', '#if HM_REQUIRED_DB_VERSION == "" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000', 1),
    ('hMailServerInnoExtension.iss', '', '#expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len("#define REQUIRED_DB_VERSION ") + 1))', '#endif', '#endsub', 1),
    ('hMailServerInnoExtension.iss', '', '#expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))', '#if Pos("#define REQUIRED_DB_VERSION ", HmConstantsLine) == 1', '#expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len("#define REQUIRED_DB_VERSION ") + 1))', 1),
    ('hMailServerInnoExtension.iss', '', '#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine', '#if HmConstantsHandle', '#expr FileClose(HmConstantsHandle)', 1),
    ('hMailServerInnoExtension.iss', '', '#if HM_REQUIRED_DB_VERSION == "" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000', '#error REQUIRED_DB_VERSION could not be read from Constants.h', '#endif', 1),
    ('hMailServerInnoExtension.iss', '', '#if HmConstantsHandle', '#expr FileClose(HmConstantsHandle)', '#endif', 1),
    ('hMailServerInnoExtension.iss', '', '#if Pos("#define REQUIRED_DB_VERSION ", HmConstantsLine) == 1', '#expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len("#define REQUIRED_DB_VERSION ") + 1))', '#endif', 1),
    ('hMailServerInnoExtension.iss', '', '#sub HmReadConstantsLine', '#expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))', '#if Pos("#define REQUIRED_DB_VERSION ", HmConstantsLine) == 1', 1),
    ('hMailServerInnoExtension.iss', '', '// variable of its own, gone when it returns.', '#define HM_CONSTANTS_FILE AddBackslash(SourcePath) + "..\\source\\Server\\Common\\Application\\Constants.h"', '#define HM_REQUIRED_DB_VERSION ""', 1),
    ('hMailServerInnoExtension.iss', 'CheckServiceAfterMove', "iVersion := ReadDatabaseVersion('after the move, through the service running as NT SERVICE\\hMailServer');", 'if iVersion = {#HM_REQUIRED_DB_VERSION} then', 'Exit;', 1),
    ('hMailServerInnoExtension.iss', 'CheckServiceAfterMove', 'if iVersion > 0 then', "szWhat := 'it opened its database at version ' + IntToStr(iVersion) + ', where this build needs {#HM_REQUIRED_DB_VERSION}'", 'else', 1),
    ('hMailServerInnoExtension.iss', 'CreateWizardPages', 'Checked := False;', '#endif', 'end;', 1),
    ('hMailServerInnoExtension.iss', 'CreateWizardPages', 'Checked := True;', '#endif', 'end;', 1),
    ('hMailServerInnoExtension.iss', 'CreateWizardPages', 'OnClick := @rdoUseExternal_Click;', '#ifdef HM_ARM64', '// MySQL and MariaDB need an ARM64 libmysql.dll that this installer does not carry.', 1),
    ('hMailServerInnoExtension.iss', 'CreateWizardPages', 'OnClick := @rdoUseInternal_Click;', '#ifdef HM_ARM64', '// SQL Server Compact has no ARM64 build (docs/WindowsOnArm.md).', 1),
    ('hMailServerInnoExtension.iss', 'DotNetDesktopMissing', 'Result := True;', "if not FindFirst(ExpandConstant('{pf64}\\dotnet\\shared\\Microsoft.WindowsDesktop.App\\{#DOTNET_MAJOR}.*'), findRec) then", 'Exit;', 1),
    ('hMailServerInnoExtension.iss', 'EndDatabaseStep', 'end', 'else if (g_iDatabaseVersionBefore >= {#HM_REQUIRED_DB_VERSION}) and (g_iDatabaseVersionAfter <= 0) then', 'begin', 1),
    ('hMailServerInnoExtension.iss', 'EndDatabaseStep', 'g_bDatabaseChanged := false;', "Log('Database step left the database unchanged, at version ' + IntToStr(g_iDatabaseVersionBefore) + ': this build needs version {#HM_REQUIRED_DB_VERSION}, ' +", "'which the database had reached before the step, and nothing in the step moves a database at or past it, though its version after the step could not be read.');", 1),
    ('hMailServerInnoExtension.iss', 'InitializeSetup', 'DeleteFile(RefusalFileName());', '// The .NET tools require the .NET {#DOTNET_MAJOR} Desktop Runtime; it is', '// bundled and installed automatically when missing, so no up-front framework', 1),
    ('hMailServerInnoExtension.iss', 'InitializeWizard', 'g_bUseInternal := false;', '#endif', 'OverrideInstallationFolder();', 1),
    ('hMailServerInnoExtension.iss', 'InitializeWizard', 'g_bUseInternal := false;', '#ifdef HM_ARM64', '// No built-in database on ARM64, whatever the command line says: a new', 1),
    ('hMailServerInnoExtension.iss', 'InstallDotNetRuntime', 'Exit;', "if (Exec(ExpandConstant('{tmp}\\{#DOTNET_RUNTIME_FILE}'), '/install /quiet /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) = False) then", 'begin', 1),
    ('hMailServerInnoExtension.iss', 'InstallDotNetRuntime', 'begin', "SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime could not be installed. The database setup tools will not work until it is installed.' + #13#10 +", 'SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);', 1),
    ('hMailServerInnoExtension.iss', 'InstallDotNetRuntime', 'begin', "SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime installation failed with exit code ' + IntToStr(ResultCode) + '.' + #13#10 +", "'The database setup tools will not work until the runtime is installed. ' +", 1),
    ('hMailServerInnoExtension.iss', 'InstallSQLCE', 'Exit;', '#endif', '// x64 only: ArchitecturesAllowed=x64 in section_setup_64.iss, so IsWin64 is', 1),
    ('hMailServerInnoExtension.iss', 'InstallSQLCE', 'Exit;', '#ifdef HM_ARM64', '// Unreachable: InitializeWizard never chooses the built-in database on ARM64,', 1),
    ('hMailServerInnoExtension.iss', 'PrepareToInstallRefusal', "'The service will not start until an ARM64 one is there (WindowsOnArm.md in the documentation says how to build it).', mbInformation, MB_OK, IDOK);", '#endif', 'Result := PrepareProgramFolders(PreviousInstallationFolder());', 1),
    ('hMailServerInnoExtension.iss', 'PrepareToInstallRefusal', 'end;', '#ifdef HM_ARM64', '// The ARM64 installer only (hMailServerARM64.iss).', 1),
    ('hMailServerInnoExtension.iss', 'RunPostInstallTasks', 'ProgressPage.SetProgress(1,6);', '// The database tools below are .NET {#DOTNET_MAJOR} apps; make sure the', "// runtime is in place before they are executed. section_run.iss's [Run]", 1),
    ('hMailServerInnoExtension.iss', 'RunPostInstallTasks', 'ProgressPage.Show();', "ProgressPage.SetText('Installing the .NET {#DOTNET_MAJOR} Desktop Runtime...', '');", 'ProgressPage.SetProgress(1,6);', 1),
    ('hMailServerInnoExtension.iss', 'StepsNotCompleteBeforeDatabase', 'if not bRuntimeReady then', "AppendStep(szList, 'the .NET {#DOTNET_MAJOR} Desktop Runtime');", 'if not bDatabaseBackendReady then', 1),
    ('hMailServerInnoExtension.iss', '[code]', '// Called from RunPostInstallTasks before the database tools are executed - they', '// are .NET {#DOTNET_MAJOR} apps and run from [Code] at ssPostInstall.', '//', 1),
    ('hMailServerInnoExtension.iss', '[code]', '// The current download page for the runtime, quoted in the failure message.', "DOTNET_DOWNLOAD_URL = 'https://dotnet.microsoft.com/download/dotnet/{#DOTNET_CHANNEL}';", "// The Service Control Manager's functions, as winsvc.h declares them, through their", 1),
    ('hMailServerInnoExtension.iss', '[code]', 'end;', '#include "OriginalHMailServer.iss"', '// Whether the installation this setup would replace is one of the original', 1),
    ('hMailServerInnoExtension.iss', '[code]', 'end;', '// Installs the bundled .NET {#DOTNET_MAJOR} Desktop Runtime when it is missing.', '// Called from RunPostInstallTasks before the database tools are executed - they', 1),
    ('hMailServerInnoExtension.iss', '[code]', 'end;', '// Returns true when the .NET {#DOTNET_MAJOR} Desktop Runtime is not installed, in', '// which case the bundled runtime installer is executed. Also the Check function on', 1),
    ('section_files_64.iss', '[files]', '; .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing. The server', '; component needs it too: DBSetup/DBSetupQuick/DBUpdater are .NET {#DOTNET_MAJOR}', '; apps that run during installation, from [Code] at ssPostInstall. The [Run] entry', 1),
    ('section_files_64.iss', '[files]', '; OpenSSL', 'Source: "{#OPENSSL_LIBS_PATH}\\libcrypto-4-x64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', 'Source: "{#OPENSSL_LIBS_PATH}\\libssl-4-x64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', 1),
    ('section_files_64.iss', '[files]', '; PQSQL (PostgreSQL client)', 'Source: "{#POSTGRESQL_LIBPQ_PATH}\\*.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', '', 1),
    ('section_files_64.iss', '[files]', '; versions - they already had.', 'Source: "DotNet\\{#DOTNET_RUNTIME_FILE}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Components: server controlpanel;', 'Source: "SQLCE\\SSCERuntime_x64-ENU.msi"; Flags: deleteafterinstall ; Excludes: ".svn"; DestDir: "{tmp}"; Components: server;', 1),
    ('section_files_64.iss', '[files]', 'Source: "..\\source\\Tools\\ControlPanel\\publish\\*"; DestDir: "{app}\\ControlPanel"; Flags: ignoreversion recursesubdirs; Components: controlpanel;', '; .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing. The server', '; component needs it too: DBSetup/DBSetupQuick/DBUpdater are .NET {#DOTNET_MAJOR}', 1),
    ('section_files_64.iss', '[files]', 'Source: "{#OPENSSL_LIBS_PATH}\\libcrypto-4-x64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', 'Source: "{#OPENSSL_LIBS_PATH}\\libssl-4-x64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', '; PQSQL (PostgreSQL client)', 1),
    ('section_files_arm64.iss', '[files]', '; OpenSSL, ARM64 (VC-WIN64-ARM)', 'Source: "{#OPENSSL_LIBS_PATH}\\libcrypto-4-arm64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', 'Source: "{#OPENSSL_LIBS_PATH}\\libssl-4-arm64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', 1),
    ('section_files_arm64.iss', '[files]', '; PQSQL (PostgreSQL client), ARM64', 'Source: "{#POSTGRESQL_LIBPQ_PATH}\\*.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', '', 1),
    ('section_files_arm64.iss', '[files]', "; exactly as section_files_64.iss's x64 one.", 'Source: "ARM64\\DotNet\\{#DOTNET_RUNTIME_FILE}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Components: server controlpanel;', '; No SQL Server Compact runtime: Microsoft never built one for ARM64, and the x64', 1),
    ('section_files_arm64.iss', '[files]', 'Source: "..\\source\\Tools\\ControlPanel\\publish-arm64\\*"; DestDir: "{app}\\ControlPanel"; Flags: ignoreversion recursesubdirs; Components: controlpanel;', '; The ARM64 .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing,', "; exactly as section_files_64.iss's x64 one.", 1),
    ('section_files_arm64.iss', '[files]', 'Source: "{#OPENSSL_LIBS_PATH}\\libcrypto-4-arm64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', 'Source: "{#OPENSSL_LIBS_PATH}\\libssl-4-arm64.dll"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;', '; PQSQL (PostgreSQL client), ARM64', 1),
    ('section_files_common.iss', '[files]', '#endif', '#ifndef HM_ARM64', "; Two files 6.2.3 to 6.3.6 put in Bin\\plugin beside the client's plugins, which this", 1),
    ('section_files_common.iss', '[files]', '; hMailServer64.iss these lines are what they always were.', '#ifndef HM_ARM64', 'Source: ".\\Extras\\7za.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion; Components: server;', 1),
    ('section_files_common.iss', '[files]', 'Source: ".\\Extras\\7za.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion; Components: server;', '#endif', '; Database scripts', 1),
    ('section_files_common.iss', '[files]', 'Source: ".\\Extras\\dh2048.pem"; DestDir: "{app}\\Bin";  Flags: ignoreversion; Components: server;', '#ifndef HM_ARM64', '; MySQL/MariaDB client library (x64) - MariaDB Connector/C 3.4.11, loaded at runtime as', 1),
    ('section_files_common.iss', '[files]', 'Source: ".\\Extras\\plugin\\sha256_password.dll"; DestDir: "{app}\\Bin\\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;', '#endif', '#ifndef HM_ARM64', 1),
    ('section_files_common.iss', '[installdelete]', 'Type: files; Name: "{app}\\Bin\\plugin\\authentication_windows_client.dll"', '#endif', '', 1),
    ('section_run.iss', '[run]', '; never go false.', 'Filename: "{tmp}\\{#DOTNET_RUNTIME_FILE}"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing .NET {#DOTNET_MAJOR} Desktop Runtime..."; Check: DotNetDesktopMissing; Components: server controlpanel;', '; The Control Panel on the last page runs only when the folders setup prepared were as', 1),
    ('section_run.iss', '[run]', '[Run]', '; Install the bundled .NET {#DOTNET_MAJOR} Desktop Runtime, but only when it is', '; missing. This entry is the FIRST attempt, for every component: Inno processes a', 1),
    ('section_setup_arm64.iss', '[setup]', '; server, and hMailServer64.iss is the installer for it.', 'OutputBaseFilename=hMailServer-{#HM_APP_VERSION}-arm64', 'AppVerName=hMailServer {#HM_APP_VERSION}-ARM64', 1),
    ('section_setup_arm64.iss', '[setup]', 'AppVersion={#HM_APP_VERSION}', 'VersionInfoVersion={#HM_VERSION_INFO}', "; As section_setup_64.iss: the architecture the Control Panel's install checks.", 1),
    ('section_setup_arm64.iss', '[setup]', 'ArchitecturesAllowed=arm64', 'AppVersion={#HM_APP_VERSION}', 'VersionInfoVersion={#HM_VERSION_INFO}', 1),
    ('section_setup_arm64.iss', '[setup]', 'OutputBaseFilename=hMailServer-{#HM_APP_VERSION}-arm64', 'AppVerName=hMailServer {#HM_APP_VERSION}-ARM64', 'ArchitecturesInstallIn64BitMode=arm64', 1),
)


@functools.lru_cache(maxsize=256)
def ispp_lines(text):
    """The lines of one installer script the Inno Setup preprocessor reads (ISPP_PLACES),
    as (where, the line before, the line, the line after, its number): each line stripped,
    the lines before and after the nearest that are not blank ('' at either end of the
    script), and where the routine of the [Code] the line is in, by its name, or else
    '[section]', the section it is in in lower case ('' before the first)."""
    lines = text.split('\n')
    starts = [0]
    for line in lines:
        starts.append(starts[-1] + len(line) + 1)
    routine = [''] * len(lines)
    externals = set(match.group(1).lower() for match in EXTERNAL_DECLARATION.finditer(text))
    closing = re.compile(r'^end;', re.M)
    for match in ROUTINE_HEADER.finditer(text):
        if match.group(1).lower() in externals:
            continue
        end = closing.search(text, match.end())
        if not end:
            continue
        first = text.count('\n', 0, match.start())
        last = first + text.count('\n', match.start(), end.start())
        for index in range(first, last + 1):
            if not routine[index]:
                routine[index] = match.group(1)
    found = []
    section = ''
    for index, line in enumerate(lines):
        header = SECTION_HEADER.match(line)
        if header and not routine[index]:
            section = '[%s]' % header.group(1).strip().lower()
        if not (ISPP_LINE.match(line) or '{#' in line):
            continue
        before = next((lines[other].strip() for other in range(index - 1, -1, -1) if lines[other].strip()), '')
        after = next((lines[other].strip() for other in range(index + 1, len(lines)) if lines[other].strip()), '')
        found.append((routine[index] or section, before, line.strip(), after, index + 1))
    return tuple(found)


def ispp_found(texts):
    """Every line of the installer's scripts (INSTALLATION's .iss files) the preprocessor
    reads, as {(script, where, before, line, after): (how many, the first one's number)}."""
    found = {}
    for name in sorted(texts):
        if '/' in name or not name.lower().endswith('.iss'):
            continue
        for where, before, line, after, number in ispp_lines(texts[name]):
            key = (name, where, before, line, after)
            count, first = found.get(key, (0, number))
            found[key] = (count + 1, first)
    return found


def check_preprocessor(texts, problems):
    """Every preprocessor line and inline directive of the installer's scripts where
    ISPP_PLACES holds it, as many times, and no other (the comment above ISPP_LINE)."""
    found = ispp_found(texts)
    held = dict(((name, where, before, line, after), count) for name, where, before, line, after, count in ISPP_PLACES)
    for key in sorted(set(found) | set(held)):
        have, number = found.get(key, (0, 0))
        want = held.get(key, 0)
        if have == want:
            continue
        name, where, before, line, after = key
        place = (' (%s)' % where) if where else ''
        if want == 0:
            problems.append('%s line %d%s: "%s", between "%s" and "%s", is read by the Inno Setup preprocessor, which changes what ISCC compiles, and work item 78\'s '
                            'check does not hold it there; hold it in ISPP_PLACES and InstallerPackaging.cs once reviewed (--preprocessor prints the table as the tree is)'
                            % (name, number, place, line[:160], before[:80], after[:80]))
        elif have == 0:
            problems.append('%s%s: "%s", between "%s" and "%s", which work item 78\'s check holds there for the Inno Setup preprocessor, is not there'
                            % (name, place, line[:160], before[:80], after[:80]))
        else:
            problems.append('%s line %d%s: "%s" is there %d times between the same lines, where work item 78\'s check holds it %d' % (name, number, place, line[:160], have, want))


def check(texts):
    # Each text as read() makes it: a carriage return, alone or before a line feed, ends
    # the line, as it does for ISCC (check 7 of R1 to R4, 28 October 2026).
    texts = dict((name, text.replace('\r\n', '\n').replace('\r', '\n')) for name, text in texts.items())
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
    check_program_folder_code(texts, problems)
    check_access_masks(texts, problems)
    check_updater(texts, problems)
    check_install_sections(texts, problems)
    check_preprocessor(texts, problems)
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
         altered(texts, EXTENSION, '   attributes.SecurityDescriptor := pDescriptor;\n', '   attributes.SecurityDescriptor := 0;\n')),
        ('a folder made by its path again, through any link put on the way since the walk (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '      handle := MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status);\n',
                 '      handle := -1;\n      if CreateDir(missing[index]) then\n         handle := HoldForSetup(missing[index]);\n')),
        ('each folder made relative to the folder the walk held, not the one made above it (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'handle := MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status);', 'handle := MakeFolderIn(HeldHandle, ExtractFileName(missing[index]), pDescriptor, status);')),
        ('a folder made shared for deleting (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'HM_MAKE_FOLDER_ACCESS, attributes, io, 0, 0, HM_SHARE_ALL_BUT_DELETE,', 'HM_MAKE_FOLDER_ACCESS, attributes, io, 0, 0, HM_SHARE_EVERYTHING,')),
        ('a folder made opened when it is there rather than made (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'HM_SHARE_ALL_BUT_DELETE, HM_FILE_CREATE, HM_MAKE_FOLDER_OPTIONS', 'HM_SHARE_ALL_BUT_DELETE, 3, HM_MAKE_FOLDER_OPTIONS')),
        ('a folder made not held until setup ends (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   Result := handle;\n   SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1);\n', '   Result := handle;\n   Exit;\n   SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1);\n')),
        ('a folder above made a link taken for a failure of another kind (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '         else if HeldIsLink(parent, bKnown) then\n         begin\n            g_bFoldersChanged := true;', '         else if false then\n         begin\n            g_bFoldersChanged := true;')),
        ('a folder somebody else made in the meantime taken (ERROR_ALREADY_EXISTS taken for made)',
         altered(texts, EXTENSION, 'if status = HM_STATUS_NAME_COLLISION then', 'if false then')),
        ('the folder the walk held not read again before a folder is made in it (a link put in its place gone through; the review of 26 October 2026)',
         altered(texts, EXTENSION, '   if HeldIsLink(HeldHandle, bKnown) then\n', '   if false then\n')),
        ('a folder made not read where it is (one made through a link put on the way taken)',
         altered(texts, EXTENSION, '         if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then', '         if false then')),
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
         altered(texts, EXTENSION, '      if g_bRemakeBin then\n', '      if false then\n')),
        ('an empty ControlPanel that was there hardened, not made again',
         altered(texts, EXTENSION, '      if g_bRemakeControlPanel then\n', '      if false then\n')),
        ('an empty Bin made again only when it is read, after the program folder\'s hardening, to let another account write it (check 3 of R1 to R4, 27 October 2026)',
         altered(texts, EXTENSION, 'g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal);',
                 'g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal) and (not HeldFolderIsHardened(g_hBin, false, szWhy));')),
        ('whether Bin is made again decided in PrepareProgramFolders, after the program folder is hardened',
         (lambda moved: altered(moved, EXTENSION, '      if g_bRemakeBin then\n', '      g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal);\n      if g_bRemakeBin then\n') if moved else None)(
             altered(texts, EXTENSION, '   g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal);\n', ''))),
        ('a Bin made a link since the walk hardened through its path, not refused',
         altered(texts, EXTENSION, 'else if HeldIsLink(g_hBin, bKnown) then', 'else if false then')),
        ('a ControlPanel made a link since the walk hardened through its path, not refused',
         altered(texts, EXTENSION, 'else if HeldIsLink(g_hControlPanel, bKnown) then', 'else if false then')),
        ('the program folder made a link before the last look taken',
         altered(texts, EXTENSION, '   if g_hProgramFolderPrepared <> -1 then\n      if HeldIsLink(g_hProgramFolderPrepared, bKnown) then', '   if g_hProgramFolderPrepared <> -1 then\n      if false then')),
        ('Bin made a link before the last look taken',
         altered(texts, EXTENSION, '   if g_hBinPrepared <> -1 then\n      if HeldIsLink(g_hBinPrepared, bKnown) then', '   if g_hBinPrepared <> -1 then\n      if false then')),
        ('ControlPanel made a link before the last look taken',
         altered(texts, EXTENSION, '   if g_hControlPanelPrepared <> -1 then\n      if HeldIsLink(g_hControlPanelPrepared, bKnown) then', '   if g_hControlPanelPrepared <> -1 then\n      if false then')),
        ('a folder made again not held to where it is made',
         altered(texts, EXTENSION, '   Result := MakeFolders(szParent, parent, Folder, Kind, MadeHandle, Why);\n', '   Result := CreateDir(Folder);\n')),
        ('no folder ever taken for empty',
         altered(texts, EXTENSION, '         Result := true;\n         repeat\n            if (rec.Name <> \'.\')', '         Result := false;\n         repeat\n            if (rec.Name <> \'.\')')),
        ('an empty folder in Bin or ControlPanel left once they are hardened',
         altered(texts, EXTENSION, "if Final and FolderHoldsNothing(szItem) and (not IsAtOrBelow(szItem, AddBackslash(g_szProgramFolderReal) + 'MySQL')) then", 'if false then')),
        # Repair 7 of R1 to R4 (check 7, 28 October 2026): links on the program folder's own level, and MySQL.
        ('a link on the program folder\'s own level that only the administrators control taken whatever its name, as before (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "                        else\n                           szWho := 'is a junction, a symbolic link or a mount point, and on the program folder''s own level",
                 "                        else if false then\n                           szWho := 'is a junction, a symbolic link or a mount point, and on the program folder''s own level")),
        ('a link to a data folder taken on the rule of every other link, not held to where it leads (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '                     if not bData then\n                     begin\n                        if not LinkIsAdministratorsAlone(handle, -1, szText) then',
                 '                     if true then\n                     begin\n                        if not LinkIsAdministratorsAlone(handle, -1, szText) then')),
        ('a link on the program folder\'s own level held to the data folders\' rule whatever its name (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '                     if not bData then\n                     begin\n                        if not LinkIsAdministratorsAlone(handle, -1, szText) then',
                 '                     if false then\n                     begin\n                        if not LinkIsAdministratorsAlone(handle, -1, szText) then')),
        ('MySQL, from which the uninstaller runs mysqld-nt.exe, not looked inside (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "   for index := 0 to 5 do\n   begin\n      case index of\n         0: szFolder := 'Addons';\n         1: szFolder := 'Events';\n         2: szFolder := 'DBScripts';\n         3: szFolder := 'WebAdmin';\n         4: szFolder := 'Languages';\n         5: szFolder := 'MySQL';\n      end;\n      szFolder := AddBackslash(App) + szFolder;",
                 "   for index := 0 to 4 do\n   begin\n      case index of\n         0: szFolder := 'Addons';\n         1: szFolder := 'Events';\n         2: szFolder := 'DBScripts';\n         3: szFolder := 'WebAdmin';\n         4: szFolder := 'Languages';\n      end;\n      szFolder := AddBackslash(App) + szFolder;")),
        ('MySQL never asked whether another account could change it (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "         5: szFolder := 'MySQL';\n      end;\n      szFolder := AddBackslash(g_szProgramFolderReal) + szFolder;",
                 "         5: szFolder := 'Languages';\n      end;\n      szFolder := AddBackslash(g_szProgramFolderReal) + szFolder;")),
        ('an empty folder of MySQL, the bundled MySQL\'s, removed in the last look (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "if Final and FolderHoldsNothing(szItem) and (not IsAtOrBelow(szItem, AddBackslash(g_szProgramFolderReal) + 'MySQL')) then", 'if Final and FolderHoldsNothing(szItem) then')),
        ('an empty folder of the program folder\'s own level left once it is hardened',
         altered(texts, EXTENSION, "else if Final and bFolder and (not bLink) and FolderHoldsNothing(szItem) then", 'else if false then')),
        ('an administrator\'s empty folder setup neither installs into nor makes again removed in the last look (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "and (not IsRunFolderName(rec.Name)) and (not (bData and IsComponentSelected('server'))) and", 'and false and')),
        ('the folders not read again after the files',
         altered(texts, EXTENSION, 'if not FoldersStillAsPrepared(szWhy) then', 'if false then')),
        ('the service let start when the folders are not as setup left them',
         altered(texts, EXTENSION, '\t    KeepServiceFromStarting();\n', '')),
        ('the folders not as setup left them ending in exit code 0',
         altered(texts, EXTENSION, '\t    g_bStepsIncomplete := true;\n\t    KeepServiceFromStarting();', '\t    KeepServiceFromStarting();')),
        ('a link found after the files left',
         altered(texts, EXTENSION, 'if bLink or (bFolder and FolderHoldsNothing(szItem)) then', 'if bFolder and FolderHoldsNothing(szItem) then')),
        ('Bin not held to holding the file setup installed there',
         altered(texts, EXTENSION, "szApp + 'Bin\\hMailServer.INI', true, SetupMadeBin(), Why)", "'', true, SetupMadeBin(), Why)")),
        ('a folder setup prepared not read for a link after the files',
         altered(texts, EXTENSION, '   if HeldIsLink(Handle, bKnown) then\n      Why := Why + Folder', '   if false then\n      Why := Why + Folder')),
        ('a folder setup prepared not read for its permissions after the files',
         altered(texts, EXTENSION, 'else if Hardened and (not HeldFolderIsHardened(Handle, AnyAccount, false, szWhy)) then', 'else if false then')),
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
         altered(texts, EXTENSION, '   attributes.SecurityQualityOfService := 0;\n', '   attributes.SecurityQualityOfService := 0;\n   attributes.SecurityDescriptor := 0;\n')),
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
         altered(texts, EXTENSION, 'else if RemoveDir(Folder) then', 'else if DelTree(Folder, True, True, True) then')),
        ('a Bin setup made removed with what is in it',
         altered(texts, EXTENSION, 'RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade);', 'DelTree(g_szBinMade, True, True, True);')),
        ('a folder made recorded before it is found where it was named (check 3 of R1 to R4, 27 October 2026)',
         altered(texts, EXTENSION, '      if handle <> -1 then\n      begin\n         if not MadeWhereNamed(',
                 '      if handle <> -1 then\n      begin\n         g_szBinMade := missing[index];\n         if not MadeWhereNamed(')),
        ('a folder made recorded without where it was made',
         altered(texts, EXTENSION, '               g_szBinMadeAt := szWhere;\n', '')),
        ('a folder made removed by its path whatever it leads to now',
         altered(texts, EXTENSION, "bSame := (not HeldIsLink(check, bKnown)) and (At <> '') and (CompareText(FinalPathOfHeld(check), At) = 0);", 'bSame := true;')),
        ('a folder made opened through a link to be read before it is removed',
         altered(texts, EXTENSION, 'check := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);',
                 'check := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, $02000000, 0);')),
        ('the folders setup made removed shallowest first',
         altered(texts, EXTENSION, 'for index := GetArrayLength(g_aFoldersMade) - 1 downto 0 do\n      RemoveMadeFolder(', 'for index := 0 to GetArrayLength(g_aFoldersMade) - 1 do\n      RemoveMadeFolder(')),
        ('the folders setup made removed after their holds are let go (DeinitializeSetup)',
         altered(texts, EXTENSION, '\tif not g_bFilesInstalled then\n\t\tRemoveFoldersMadeHere();\n\tReleaseHeldFolders();\n',
                 '\tReleaseHeldFolders();\n\tif not g_bFilesInstalled then\n\t\tRemoveFoldersMadeHere();\n')),
        ('what an earlier pass made removed after its holds are let go (CheckProgramFolders)',
         altered(texts, EXTENSION, '   RemoveFoldersMadeHere();\n   ReleaseHeldFolders();\n', '   ReleaseHeldFolders();\n   RemoveFoldersMadeHere();\n')),
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
        ('a program folder that is the root of a volume installed into (check 3 of R1 to R4, 27 October 2026)',
         altered(texts, EXTENSION, 'if IsVolumeRoot(g_szProgramFolderReal) then\n   begin\n      Result :=', 'if false then\n   begin\n      Result :=')),
        ('a Bin or a ControlPanel that leads to the root of a volume installed into',
         altered(texts, EXTENSION, 'if IsVolumeRoot(g_szBinReal) or IsVolumeRoot(g_szControlPanelReal) then', 'if false then')),
        ('a volume\'s root reached by its name taken for its device again (its last backslash taken off; the review of 26 October 2026)',
         altered(texts, EXTENSION, '            if CompareText(szCurrent, szRoot) = 0 then\n               Real := szRoot\n            else\n               Real := RemoveBackslashUnlessRoot(szCurrent);',
                 '            Real := RemoveBackslashUnlessRoot(szCurrent);')),
        ('any path under a volume\'s name taken for its root',
         altered(texts, EXTENSION, "         Result := Copy(Path, Length(Path) - 1, 2) = '}\\';", '         Result := true;')),
        ('the program folder\'s owner asked only of the root of a volume',
         altered(texts, EXTENSION, '   if g_bProgramFolderThere then\n      if not OwnerIsAllowed(g_szProgramFolderReal', '   if g_bProgramFolderThere and IsVolumeRoot(g_szProgramFolderReal) then\n      if not OwnerIsAllowed(g_szProgramFolderReal')),
        ('an existing Bin never hardened (its condition inverted)',
         altered(texts, EXTENSION, 'if g_bBinThere then\n   begin\n      if g_bRemakeBin then', 'if not g_bBinThere then\n   begin\n      if g_bRemakeBin then')),
        ('an existing ControlPanel never hardened (its condition inverted)',
         altered(texts, EXTENSION, 'if g_bControlPanelThere then\n   begin\n      if g_bRemakeControlPanel then',
                 'if not g_bControlPanelThere then\n   begin\n      if g_bRemakeControlPanel then')),
        ('a Bin that could not be hardened installed into (the result unused)',
         altered(texts, EXTENSION, 'if not HardenHeldFolder(g_szBinReal, g_hBin, false, szWhy) then',
                 'HardenHeldFolder(g_szBinReal, g_hBin, false, szWhy);\n      if false then')),
        ('a program folder that could not be hardened installed into',
         altered(texts, EXTENSION, 'else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) then',
                 'else if HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) and false then')),
        ('the program folder left as it is for any inheriting Data, not only the earlier installation\'s',
         altered(texts, EXTENSION, "if g_bEarlierInstallation then\n      if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data')",
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
         altered(texts, EXTENSION, 'if not HeldFolderIsHardened(HeldHandle, not bTrusted, false, Why) then', 'if false then')),
        ('a list that still inherits taken for hardened',
         altered(texts, EXTENSION, 'else if (control and HM_DACL_PROTECTED) = 0 then', 'else if false then')),
        ('a folder another account owns not given to the Administrators with their privilege (no takeown)',
         altered(texts, EXTENSION, "if not Exec(ExpandConstant('{sys}\\takeown.exe'), '/F \"' + Folder + '\" /A', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then",
                 'if false then')),
        ('a folder another account owns given three entries alone',
         altered(texts, EXTENSION, '      Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, bUsers, false, Why);',
                 "      szSid := 'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)';")),
        ('a rebuilt folder keeping the entries of its own that let another account write',
         altered(texts, EXTENSION, "else if writes and (not IsTrustedSid(pAce + 8)) and (not (Service and SidIs(pAce + 8, HM_SERVICE_SID))) then\n                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own'",
                 "else if false then\n                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own'")),
        ('a rebuilt folder keeping every inherited entry that lets another account write, as its own',
         altered(texts, EXTENSION, "else if writes and (not SidIs(pAce + 8, 'S-1-5-18')) and (not SidIs(pAce + 8, 'S-1-5-32-544')) then\n"
                 "                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s inherited';",
                 "else\n                     ok := AddAsOwnEntry(pNew, pAce);")),
        ('the service\'s inherited Read dropped from a rebuilt folder',
         altered(texts, EXTENSION, 'SidIs(pAce + 8, HM_SERVICE_SID) and (Service or (not writes))', 'false')),
        ('a data folder rebuilt without the service\'s inherited Modify (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'SidIs(pAce + 8, HM_SERVICE_SID) and (Service or (not writes))', 'SidIs(pAce + 8, HM_SERVICE_SID) and (not writes)')),
        ('a data folder rebuilt without the service\'s own Modify (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "(not (Service and SidIs(pAce + 8, HM_SERVICE_SID))) then\n                     szDropped", "true then\n                     szDropped")),
        ('a data folder another account owned rebuilt as a program folder, Users given Read and the service\'s Modify dropped (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, false, true, Why);', 'Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, true, false, Why);')),
        ('a data folder setup rebuilt not read again (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   Result := HeldFolderIsHardened(HeldHandle, true, true, Why);\n', '')),
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
         altered(texts, EXTENSION, 'else if (not IsTrustedSid(pOwner)) and (not (bKeyStore and SidIs(pOwner, HM_SERVICE_SID))) then\n                     begin\n                        if (not bCode)',
                 'else if false then\n                     begin\n                        if (not bCode)')),
        ('what is below the first level of Bin and ControlPanel not looked at',
         altered(texts, EXTENSION, 'if not CheckWhatIsInside(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Final, Scripts, Found, Why) then', 'if false then')),
        ('what is in Bin not looked at once it is hardened',
         altered(texts, EXTENSION, '   if g_bBinThere then\n      CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true, false, found, szWhy);\n', '')),
        ('what is in Bin not looked at before the service is stopped',
         altered(texts, EXTENSION, '   if g_bBinThere then\n      CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBin), 0, false, false, found, szWhy);\n', '')),
        ('a script not counted as a program in the folders that hold what is run (the review of 26 October 2026)',
         altered(texts, EXTENSION, '(IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts and IsScriptFile(rec.Name)))', '(IsProgramFile(rec.Name) or IsSettingsFile(rec.Name))')),
        ('Addons, Events, DBScripts, WebAdmin and Languages not looked at',
         altered(texts, EXTENSION, 'if not CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why) then', 'if false then')),
        ('those folders looked at with scripts not counted',
         altered(texts, EXTENSION, 'CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why)',
                 'CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, false, Found, Why)')),
        ('the program folder\'s own level and other folders not looked at before the service is stopped',
         altered(texts, EXTENSION, '   if g_bProgramFolderThere then\n      CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy);\n', '')),
        ('the program folder\'s own level and other folders not looked at once it is hardened',
         altered(texts, EXTENSION, '   if g_bProgramFolderThere then\n      CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy);\n', '')),
        ('a link in the program folder that another account controls taken',
         altered(texts, EXTENSION, 'if not LinkIsAdministratorsAlone(handle, -1, szText) then', 'if false then')),
        ('a program or a folder of the program folder another account owns taken',
         altered(texts, EXTENSION, "else if not IsTrustedSid(pOwner) then\n                           szWho := 'is owned by '", "else if false then\n                           szWho := 'is owned by '")),
        ('a data folder not held to its rule (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'if not DataFolderIsAllowed(szItem, handle, bOwner, szWho, szRemedy) then', 'if false then')),
        ('the earlier installation\'s data folder another account owns installed into without a restart (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "                           AddRestartFolder(szItem, -1, true, false, szItem + ' was ' + Copy(szWho, 4, Length(szWho)));\n", '')),
        ('a new installation\'s data folder another account owns taken as the earlier installation\'s is (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "if bOwner and g_bEarlierInstallation and (not Final) then\n                        begin\n                           AddRestartFolder(szItem",
                 "if bOwner and (not Final) then\n                        begin\n                           AddRestartFolder(szItem")),
        ('the Database\'s files not looked at (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'szWho := DatabaseFilesWhy(szItem, szRemedy);', "szWho := '';")),
        ('a file of the Database another account owns taken (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then\n                     begin\n                        Result := 'holds '",
                 "if false then\n                     begin\n                        Result := 'holds '")),
        ('a file of the Database another account may write by an entry of its own taken (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then\n                     begin\n                        Result := 'holds '",
                 "else if false then\n                     begin\n                        Result := 'holds '")),
        ('a data folder another account owns taken (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "   else if (not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID)) then\n   begin\n      Owner := true;", "   else if false then\n   begin\n      Owner := true;")),
        ('a data folder another account may write by an entry of its own taken (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "   else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then\n   begin\n      Who := 'has an entry", "   else if false then\n   begin\n      Who := 'has an entry")),
        ('where a data folder\'s link leads not held to the rule of a data folder (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then', '   if false then')),
        ('where a new installation\'s data folder\'s link leads, another account\'s, taken as the earlier installation\'s is (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "if bOwner and g_bEarlierInstallation and (not Final) then\n         AddRestartFolder(szReal", "if bOwner and (not Final) then\n         AddRestartFolder(szReal")),
        ('HardenForRestart hardening a data folder as a program folder (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'else if g_aRestartData[index] then', 'else if false then')),
        ('a program whose entry kept from where it was made lets another account change it not noted, in Bin (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'else if (not Final) and StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText) then', 'else if false then')),
        ('a program whose entry kept from where it was made lets another account change it not noted, in the program folder (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'StaleEntryLetsOthers(pDacl, pAppDescriptor, pAppDacl, false, false, szText)', 'false')),
        ('an entry the folder hands down taken for one a file kept from where it was made (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '(not HandsDownWriteTo(pFolderDescriptor, pFolderDacl, pAce + 8, inherit))', 'true')),
        # Repair 7 of R1 to R4 (check 7, 28 October 2026): a folder moved in.
        ('a folder whose entry kept from where it was made lets another account change it not noted, in Bin, the first look asking programs alone (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'else if (not Final) and StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText) then',
                 'else if bCode and (not Final) and StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText) then')),
        ('a folder asked as a file is (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText)', 'StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, false, szText)')),
        ('a folder\'s inherit-only entry, which hands a right down to the files in it, passed over (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '(Folder or ((aceFlags and HM_INHERIT_ONLY) = 0))', '((aceFlags and HM_INHERIT_ONLY) = 0)')),
        ('a folder\'s entry that applies to it held to what is handed down to files (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '            if (aceFlags and HM_INHERIT_ONLY) = 0 then\n               inherit := HM_CONTAINER_INHERIT\n',
                 '            if (aceFlags and HM_INHERIT_ONLY) = 0 then\n               inherit := HM_OBJECT_INHERIT\n')),
        ('HandsDownWriteTo asking files\' inheritance whatever its caller names (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "((((head.TypeFlagsSize shr 8) and $FF) and Inherit) <> 0) and", "((((head.TypeFlagsSize shr 8) and $FF) and HM_OBJECT_INHERIT) <> 0) and")),
        ('a folder whose list cannot be read taken to hand every right down (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   if pFolderDescriptor = 0 then\n      Exit;\n   if pFolderDacl = 0 then', '   if pFolderDacl = 0 then')),
        ('the record of a restart not looked up by where the walks ended (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   if RestartRecordedForWalked(szWhy) then\n', '   if false then\n')),
        ('the record of a restart written by {app} alone (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "         if (written = 0) and (szFinal <> '') and (not IsVolumeRoot(szFinal)) then\n", '         if false then\n')),
        ('the root of a volume recorded, so that nothing on it installs before a restart (check 5 of R1 to R4)',
         altered(texts, EXTENSION, " and (not IsVolumeRoot(szFinal)) then\n", ' then\n')),
        ('a record by a path rather than where a walk ended taken (check 5 of R1 to R4)',
         altered(texts, EXTENSION, "(CompareText(Copy(names[index], 1, 4), '\\\\?\\') = 0)", '(true)')),
        ('where the walk of a program folder not there yet ended named by the folder above it alone (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '      Result := AddBackslash(szFinal) + Copy(szReal, Length(AddBackslash(szHeld)) + 1, Length(szReal));', '      Result := szFinal;')),
        ('Bin\'s walk named by the program folder\'s handle (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '1: begin handle := g_hBin;', '1: begin handle := g_hProgramFolder;')),
        ('a stale entry found but its folder not hardened for a restart (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '      szWhat := StaleItemIn(Folder);\n', "      szWhat := '';\n")),
        ('an inherited entry that lets another account change a folder not counted for the restart rule (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '(((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8))', 'IsBroadSid(pAce + 8)')),
        ('a group of every user\'s own entry not counted for the restart rule (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '(((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8))', '((aceFlags and HM_INHERITED_ACE) <> 0)')),
        ('a named account\'s own entry, which setup keeps, counted for the restart rule, so that every run is refused (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '(((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8))', '(true)')),
        ('what an earlier pass found stale kept when the folder page is gone back to (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   SetArrayLength(g_aStaleItems, 0);\n', '')),
        ('a walk\'s handle of an earlier pass read again (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '   g_hBin := -1;\n', '')),
        ('Events not among the folders that hold what is run',
         altered(texts, EXTENSION, "1: szFolder := 'Events';", "1: szFolder := 'Data';")),
        ('an event script not counted as a script (.vbs)',
         altered(texts, EXTENSION, "(szExtension = '.vbs') or (szExtension = '.js')", "(szExtension = '.js')")),
        ('Events taken for a data folder',
         altered(texts, EXTENSION, "(CompareText(Name, 'Database') = 0);", "(CompareText(Name, 'Database') = 0) or (CompareText(Name, 'Events') = 0);")),
        ('a program, a script or a folder another account controls in the program folder said, not refused',
         altered(texts, EXTENSION, '\n                  if bLink or bCode or bFolder then', '\n                  if bLink then')),
        ('a data folder another account controls in the program folder said, not refused (check 5 of R1 to R4)',
         altered(texts, EXTENSION, '\n                  if bLink or bCode or bFolder then', '\n                  if bLink or bCode or (bFolder and (not bData)) then')),
        ('the folder of every link asked about as the program folder\'s',
         altered(texts, EXTENSION, '   if hParent = -1 then\n      Exit;\n', '   if hParent <> -2 then\n      Exit;\n')),
        ('a program an entry of whose own lets another account change it taken',
         altered(texts, EXTENSION, 'if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText) then', 'if false then')),
        # The settings files (check 3 of R1 to R4, 27 October 2026).
        ('Bin\'s settings file another account controls said, not refused (check 3 of R1 to R4, 27 October 2026)',
         altered(texts, EXTENSION, "(CompareText(Name, 'hMailServer.INI') = 0) or ", '')),
        ('the REST API\'s key store another account controls said, not refused',
         altered(texts, EXTENSION, "(CompareText(Name, 'hMailServerApiKeys.ini') = 0) or (szExtension = '.config')", "(szExtension = '.config')")),
        ('a .config beside a program said, not refused',
         altered(texts, EXTENSION, "(szExtension = '.config') or (szExtension = '.json')", "(szExtension = '.json')")),
        ('a .json beside a program said, not refused',
         altered(texts, EXTENSION, " or (szExtension = '.json') or\n", ' or\n')),
        ('the uninstaller\'s record said, not refused',
         altered(texts, EXTENSION, " or\n             ((CompareText(Copy(Name, 1, 5), 'unins') = 0) and (szExtension = '.dat'));", ';')),
        ('the settings files in Bin and ControlPanel not held to the rule of a program',
         altered(texts, EXTENSION, 'IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts', 'IsProgramFile(rec.Name) or (Scripts')),
        ('the uninstaller\'s record on the program folder\'s level not held to the rule of a program',
         altered(texts, EXTENSION, 'IsProgramFile(rec.Name) or IsScriptFile(rec.Name) or IsSettingsFile(rec.Name)', 'IsProgramFile(rec.Name) or IsScriptFile(rec.Name)')),
        ('every program or settings file the service owns taken as its key store',
         altered(texts, EXTENSION, '(not (bKeyStore and SidIs(pOwner, HM_SERVICE_SID)))', '(not SidIs(pOwner, HM_SERVICE_SID))')),
        ('the service\'s entry taken on every program and settings file',
         altered(texts, EXTENSION, 'OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText)', 'OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText)')),
        ('the service\'s entry taken whatever is asked',
         altered(texts, EXTENSION, '(not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID)))', '(not SidIs(pAce + 8, HM_SERVICE_SID))')),
        # A link on the program folder's own level.
        ('a link the administrators control at Events, Addons, DBScripts, WebAdmin or Languages gone through',
         altered(texts, EXTENSION, 'else if bFolder and IsRunFolderName(rec.Name) then', 'else if false then')),
        ('Events not among the folders that may not be a link',
         altered(texts, EXTENSION, "(CompareText(Name, 'Events') = 0) or ", '')),
        ('a data folder\'s link taken without looking where it leads',
         altered(texts, EXTENSION, 'else if not DataLinkLeadsWhereAllowed(szItem, Final, szText) then', 'else if false then')),
        ('where a data folder\'s link leads not walked',
         altered(texts, EXTENSION, 'if not HoldFolderChain(szTarget, 1, szReal, szHeld, hHeld, bWhole, szWhy) then', 'if false then')),
        ('a data folder\'s link to a folder that is not there taken',
         altered(texts, EXTENSION, "   if not bWhole then\n   begin\n      Why := 'it leads to '", "   if false then\n   begin\n      Why := 'it leads to '")),
        ('the volume a data folder\'s link leads to not asked',
         altered(texts, EXTENSION, 'if not VolumeKeepsPermissions(hHeld, szHeld, szWhy) then', 'if false then')),
        # The last page.
        ('the Control Panel run from the last page when the folders were not as setup left them',
         altered(texts, 'section_run.iss', ' Components: controlpanel; Check: FoldersWereAsPrepared;', ' Components: controlpanel;')),
        ('the folders found not as setup left them without the last page told',
         altered(texts, EXTENSION, '\t    g_bFoldersNotAsPrepared := true;\n', '')),
        ('the last page told the folders were as setup left them whatever was found',
         altered(texts, EXTENSION, 'Result := not g_bFoldersNotAsPrepared;', 'Result := true;')),
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
         altered(texts, EXTENSION, "\t\tg_szFolderRefusal := CheckFoldersBeforeTheStop(PreviousInstallationFolder());\n\t\tif g_szFolderRefusal <> '' then\n\t\t\tExit;\n", '')),
        ('whether setup stopped the service cleared by a later click on the Ready page (the review of 26 October 2026)',
         altered(texts, EXTENSION, "\t\tif bWasRunning then\n\t\t\tg_bServiceStoppedForSetup := true;\n\t\tif (StopHMailServerService() = false) then\n\t\t\tResult := false;\n",
                 "\t\tif (StopHMailServerService() = false) then\n\t\t\tResult := false\n\t\telse\n\t\t\tg_bServiceStoppedForSetup := bWasRunning;\n")),
        ('whether setup stopped the service cleared elsewhere',
         altered(texts, EXTENSION, "   Result := PrepareToInstallRefusal();\n", "   Result := PrepareToInstallRefusal();\n   g_bServiceStoppedForSetup := false;\n")),
        ('a refusal after the service was stopped not saying so',
         altered(texts, EXTENSION, "      else if g_bServiceStoppedForSetup then\n         Result := Result + ' Setup stopped",
                 "      else if false then\n         Result := Result + ' Setup stopped")),
        # The masks.
        ('the read mask without GENERIC_READ (the program folder made in Program Files with no entry for Users)',
         altered(texts, EXTENSION, 'HM_FOLDER_READ_RIGHTS       = $90000001;', 'HM_FOLDER_READ_RIGHTS       = $10000001;')),
        ('an entry\'s rights tested against a literal mask in parentheses',
         altered(texts, EXTENSION, '((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and\n',
                 '((head.Mask and ($000D0156)) <> 0) and (not IsTrustedSid(pAce + 8)) and\n')),
        ('the link rule asked about a narrowed mask',
         altered(texts, EXTENSION, 'AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho)', 'AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS and $FFFFFEFF, true, szWho)')),
        # Check 3 of R1 to R4 (27 October 2026): its changes of the constants and the
        # helpers, nineteen of which the checker passed (the twentieth, shallowest first,
        # is above).
        ('HM_TRUSTED_INSTALLER_SID set to Users\' SID, so that every user is trusted (check 3 of R1 to R4, 27 October 2026: passed)',
         altered(texts, EXTENSION, "HM_TRUSTED_INSTALLER_SID    = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464';", "HM_TRUSTED_INSTALLER_SID    = 'S-1-5-32-545';")),
        ('HM_REPARSE_POINT set to FILE_ATTRIBUTE_COMPRESSED ($800), so that no link is seen',
         altered(texts, EXTENSION, 'HM_REPARSE_POINT            = $400;', 'HM_REPARSE_POINT            = $800;')),
        ('HM_INHERIT_ONLY set to NO_PROPAGATE_INHERIT ($04), so that an entry that applies to a folder is passed over',
         altered(texts, EXTENSION, 'HM_INHERIT_ONLY             = $08;', 'HM_INHERIT_ONLY             = $04;')),
        ('HM_ACCESS_ALLOWED set to ACCESS_DENIED (1), so that no allow entry is weighed',
         altered(texts, EXTENSION, 'HM_ACCESS_ALLOWED           = 0;', 'HM_ACCESS_ALLOWED           = 1;')),
        ('HM_DRIVE_REMOTE set to DRIVE_CDROM (5), so that a network drive is installed into',
         altered(texts, EXTENSION, 'HM_DRIVE_REMOTE             = 4;', 'HM_DRIVE_REMOTE             = 5;')),
        ('HM_PERSISTENT_ACLS set to FILE_SUPPORTS_ENCRYPTION ($20000)',
         altered(texts, EXTENSION, 'HM_PERSISTENT_ACLS          = $8;', 'HM_PERSISTENT_ACLS          = $20000;')),
        ('HM_DACL_PROTECTED set to SE_DACL_AUTO_INHERITED ($400), so that a folder that inherits is taken for hardened',
         altered(texts, EXTENSION, 'HM_DACL_PROTECTED           = $1000;', 'HM_DACL_PROTECTED           = $400;')),
        ('HM_INHERITED_ACE set to OBJECT_INHERIT ($01), so that inherited entries are taken for a folder\'s own',
         altered(texts, EXTENSION, 'HM_INHERITED_ACE            = $10;', 'HM_INHERITED_ACE            = $01;')),
        ('the folders setup made above the program folder never given to the uninstaller (SetupMadeFolder false)',
         altered(texts, EXTENSION, 'Result := g_bProgramFolderNew and (MadeParentOffset() + Index < MadeParentCount());', 'Result := false;')),
        ('the program folder setup made never given to the uninstaller (SetupMadeProgramFolder false)',
         altered(texts, EXTENSION, 'function SetupMadeProgramFolder(): Boolean;\nbegin\n   Result := ProgramFolderMadeAsNamed();', 'function SetupMadeProgramFolder(): Boolean;\nbegin\n   Result := false;')),
        ('links followed eighty deep, not eight (the word rule found "Depth >= 8" in "Depth >= 80")',
         altered(texts, EXTENSION, '   if Depth >= 8 then', '   if Depth >= 80 then')),
        ('the rest of the path after a link not walked from where it leads',
         altered(texts, EXTENSION, "                     if szRest <> '' then\n                        szTarget := AddBackslash(szTarget) + szRest;\n", '')),
        ('a relative symbolic link taken from setup\'s own folder',
         altered(texts, EXTENSION, '      if not RelativeLinkTarget(Link, szName, Target, Why) then\n         Exit;\n', '      Target := ExpandFileName(szName);\n')),
        ('where a link leads read from the name shown, not the substitute name',
         altered(texts, EXTENSION, '   offset := Ord(buffer[5]);\n   size := Ord(buffer[6]);', '   offset := Ord(buffer[7]);\n   size := Ord(buffer[8]);')),
        ('every item of Bin and ControlPanel taken for a file (bFolder false)',
         altered(texts, EXTENSION, '            bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;\n            bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0;\n'
                 '            bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts',
                 '            bFolder := false;\n            bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0;\n'
                 '            bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts')),
        ('Events passed over on the program folder\'s own level, as Bin is',
         altered(texts, EXTENSION, "(CompareText(rec.Name, 'Bin') <> 0) and (CompareText(rec.Name, 'ControlPanel') <> 0) then",
                 "(CompareText(rec.Name, 'Bin') <> 0) and (CompareText(rec.Name, 'ControlPanel') <> 0) and (CompareText(rec.Name, 'Events') <> 0) then")),
        ('Users given Read on a folder made where an entry handed down to files alone reads',
         altered(texts, EXTENSION, "((((head.TypeFlagsSize shr 8) and $FF) and HM_CONTAINER_INHERIT) <> 0) and\n               ((head.Mask and HM_FOLDER_READ_RIGHTS) <> 0) then",
                 "true and\n               ((head.Mask and HM_FOLDER_READ_RIGHTS) <> 0) then")),
        ('the folders found as setup left them whatever was found after the files',
         altered(texts, EXTENSION, "   Result := Why = '';\nend;", '   Result := true;\nend;')),
        ('what an earlier pass made not removed when the folder page is gone back to',
         altered(texts, EXTENSION, "   RemoveFoldersMadeHere();\n   ReleaseHeldFolders();\n   g_szFolderFindings := '';", "   ReleaseHeldFolders();\n   g_szFolderFindings := '';")),
        # Repair 3a's code (27 October 2026), one line changed at a time.
        ('an empty Bin never made again (decided false)',
         altered(texts, EXTENSION, 'g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal);', 'g_bRemakeBin := false;')),
        ('an empty ControlPanel never made again (decided false)',
         altered(texts, EXTENSION, 'g_bRemakeControlPanel := g_bControlPanelThere and FolderHoldsNothing(g_szControlPanelReal);', 'g_bRemakeControlPanel := false;')),
        ('what an earlier pass decided kept when the folder page is gone back to',
         altered(texts, EXTENSION, '   g_bRemakeBin := false;\n', '')),
        ('Bin\'s hMailServer.INI not known as a settings file (its name misspelt)',
         altered(texts, EXTENSION, "(CompareText(Name, 'hMailServer.INI') = 0)", "(CompareText(Name, 'hMailServer.INI.old') = 0)")),
        ('the uninstaller\'s record never known (unins misspelt)',
         altered(texts, EXTENSION, "((CompareText(Copy(Name, 1, 5), 'unins') = 0)", "((CompareText(Copy(Name, 1, 5), 'uninst') = 0)")),
        ('every file of Bin and ControlPanel taken for the key store, which the service may own',
         altered(texts, EXTENSION, "bKeyStore := (not bFolder) and (CompareText(rec.Name, 'hMailServerApiKeys.ini') = 0);", 'bKeyStore := not bFolder;')),
        ('Languages taken behind an administrator\'s link',
         altered(texts, EXTENSION, " or\n             (CompareText(Name, 'Languages') = 0);", ';')),
        ('a link the administrators control at the five folders taken, the rule asked of files instead',
         altered(texts, EXTENSION, 'else if bFolder and IsRunFolderName(rec.Name) then', 'else if (not bFolder) and IsRunFolderName(rec.Name) then')),
        ('the service\'s entry taken on what is on the program folder\'s own level',
         altered(texts, EXTENSION, 'OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, false, szText)', 'OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText)')),
        ('the five folders looked through when they are links',
         altered(texts, EXTENSION, ' and ((FolderAttributes(szFolder) and HM_REPARSE_POINT) = 0)', '')),
        ('a data folder\'s link taken when it cannot be held, read or walked (Result true from the start)',
         altered(texts, EXTENSION, "begin\n   Result := false;\n   Why := '';\n   hLink := HoldForSetup(Link);", "begin\n   Result := true;\n   Why := '';\n   hLink := HoldForSetup(Link);")),
        ('a data folder\'s link taken when it is a folder now',
         altered(texts, EXTENSION, '   if not HeldIsLink(hLink, bKnown) then', '   if false then')),
        ('where a data folder\'s link leads walked one link deeper',
         altered(texts, EXTENSION, 'HoldFolderChain(szTarget, 1, szReal, szHeld, hHeld, bWhole, szWhy)', 'HoldFolderChain(szTarget, 0, szReal, szHeld, hHeld, bWhole, szWhy)')),
        ('the owner of a data folder, or of where its link leads, held to nothing (check 5 of R1 to R4)',
         altered(texts, EXTENSION, 'if (not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID)) then', 'if false then')),
        ('a folder setup made removed whatever it is now',
         altered(texts, EXTENSION, '   if not bSame then\n', '   if false then\n')),
        ('a Bin setup made not removed after a cancel',
         altered(texts, EXTENSION, "   if g_szBinMade <> '' then\n      RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade);\n", '')),
        ('where a folder was made not read through its handle when it is recorded',
         altered(texts, EXTENSION, '            szWhere := FinalPathOfHeld(handle);\n', '')),
        ('the handle of a Bin setup made not recorded',
         altered(texts, EXTENSION, '               g_hBinMade := handle;\n', '               g_hBinMade := -1;\n')),
        ('Bin not read again before the last look',
         altered(texts, EXTENSION, '   if g_hBinPrepared <> -1 then\n      if HeldIsLink(g_hBinPrepared, bKnown) then', '   if g_hBinPrepared = -2 then\n      if HeldIsLink(g_hBinPrepared, bKnown) then')),
        ('a ControlPanel that leads to the root of a volume installed into',
         altered(texts, EXTENSION, 'if IsVolumeRoot(g_szBinReal) or IsVolumeRoot(g_szControlPanelReal) then', 'if IsVolumeRoot(g_szBinReal) then')),
        ('the last page told the folders were as setup left them when they were not',
         altered(texts, EXTENSION, '\t    g_bFoldersNotAsPrepared := true;\n', '\t    g_bFoldersNotAsPrepared := false;\n')),
        ('the Control Panel run from the last page only when the folders were not as setup left them',
         altered(texts, 'section_run.iss', ' Check: FoldersWereAsPrepared;', ' Check: not FoldersWereAsPrepared;')),
        ('setup let go on, the program folders not prepared, by a way out of PrepareToInstallRefusal without a refusal',
         altered(texts, EXTENSION, '   Result := PrepareProgramFolders(PreviousInstallationFolder());\nend;', '   if WizardSilent() then\n      Exit;\n   Result := PrepareProgramFolders(PreviousInstallationFolder());\nend;')),
        # Repair 4 of R1 to R4 (check 4, 27 October 2026): what another account could change.
        ('a folder another account could change installed into with no restart (HardenForRestart answers nothing)',
         altered(texts, EXTENSION, '   if GetArrayLength(g_aRestartFolders) = 0 then\n      Exit;\n   szApp := RemoveBackslashUnlessRoot', '   Exit;\n   szApp := RemoveBackslashUnlessRoot')),
        ('a folder that lets another account change its permissions or its owner taken (Everyone Full control on a folder made beforehand)',
         altered(texts, EXTENSION, 'else if TakenEntriesLetOthers(pDacl, HM_CHANGE_PERMISSIONS_RIGHTS, szWho) then', 'else if false then')),
        ('a folder that holds something and lets another account write it taken',
         altered(texts, EXTENSION, 'else if Contents and TakenEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szWho) then', 'else if false then')),
        ('the program folder never asked whether another account could change it',
         altered(texts, EXTENSION, "   NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, g_szProgramFolderWaits <> '');\n", '')),
        ('Addons, Events, DBScripts, WebAdmin and Languages never asked whether another account could change them',
         altered(texts, EXTENSION, '            NoteFolderOthersCouldChange(szFolder, handle, false, false);', '            szFolder := szFolder;')),
        ('a folder that holds only a file asked as one that holds nothing',
         altered(texts, EXTENSION, 'if not OthersCouldChange(Folder, HeldHandle, Unhardened or (not HoldsOnlyEmptyFolders(Folder)), bDistrust, szWhat) then',
                 'if not OthersCouldChange(Folder, HeldHandle, Unhardened, bDistrust, szWhat) then')),
        # Repair 6 of R1 to R4 (check 6, 28 October 2026): the restart rule's distrusted folders and the unhardened program folder.
        ('a folder whose permissions another account could change hardened keeping the entries of its own (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '      else if g_aRestartDistrust[index] then\n', '      else if false then\n')),
        ('a folder whose permissions another account could change never noted so (check 6 of R1 to R4)',
         altered(texts, EXTENSION, "      What := Folder + ' let ' + szWho + ' change its permissions or its owner';\n      Distrust := true;\n",
                 "      What := Folder + ' let ' + szWho + ' change its permissions or its owner';\n      Distrust := false;\n")),
        ('whether a folder\'s permissions could be changed not recorded for the restart rule (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '   g_aRestartDistrust[count - 1] := Distrust;\n', '   g_aRestartDistrust[count - 1] := false;\n')),
        ('HardenDistrustedFolder keeping a named account\'s entry of its own, as HardenHeldFolder does (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '      Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, EveryUserMayRead(pDacl), false, Why);\n',
                 '      Result := HardenHeldFolder(Folder, HeldHandle, false, Why);\n')),
        ('HardenDistrustedFolder reading the folder again as a folder the administrators hardened in place (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '   Result := HeldFolderIsHardened(HeldHandle, true, false, Why);\n   if not Result then\n      Why := \'once setup had rebuilt',
                 '   Result := HeldFolderIsHardened(HeldHandle, false, false, Why);\n   if not Result then\n      Why := \'once setup had rebuilt')),
        ('the program folder setup leaves unhardened before the files asked as one that holds only empty folders (check 6 of R1 to R4)',
         altered(texts, EXTENSION, "NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, g_szProgramFolderWaits <> '');",
                 'NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, false);')),
        ('PrepareProgramFolders leaving the program folder unhardened by a rule of its own (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '   szInherits := g_szProgramFolderWaits;\n', '   szInherits := DataFolderThatInherits(g_szProgramFolderReal);\n')),
        # Repair 7 of R1 to R4 (check 7, 28 October 2026): the decision taken once.
        ('PrepareProgramFolders deciding by the answer asked after the service stop, the second call deciding (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '   szInherits := g_szProgramFolderWaits;\n', '   szInherits := ProgramFolderWaitsForTheFiles();\n')),
        ('a program folder whose data folders changed since setup looked taken, the second answer not refused (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '   if CompareText(ProgramFolderWaitsForTheFiles(), szInherits) <> 0 then\n', '   if false then\n')),
        ('a changed answer refused without being taken for a folder changed while setup prepared it (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "   begin\n      g_bFoldersChanged := true;\n      Result := 'Setup does not install into ' + szApp + ': a data folder of the earlier",
                 "   begin\n      Result := 'Setup does not install into ' + szApp + ': a data folder of the earlier")),
        ('the restart rule asking whether the program folder waits for the files again, not the stored answer (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, g_szProgramFolderWaits <> '');",
                 "NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, ProgramFolderWaitsForTheFiles() <> '');")),
        ('whether the program folder waits for the files decided after the restart rule looked (check 7 of R1 to R4)',
         (lambda moved: altered(moved, EXTENSION, '   NoteFoldersOthersCouldChange();\nend;', '   NoteFoldersOthersCouldChange();\n   if g_bProgramFolderThere then\n      g_szProgramFolderWaits := ProgramFolderWaitsForTheFiles();\nend;') if moved else None)(
             altered(texts, EXTENSION, '   if g_bProgramFolderThere then\n      g_szProgramFolderWaits := ProgramFolderWaitsForTheFiles();\n', ''))),
        ('the stored answer kept from an earlier pass when the folder page is gone back to (check 7 of R1 to R4)',
         altered(texts, EXTENSION, "   g_szProgramFolderWaits := '';\n   g_bFoldersChanged := false;\n", "   g_bFoldersChanged := false;\n")),
        ('an entry of its own for a named account on the program folder left unhardened not said (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '      SayWriteEntriesKept(g_szProgramFolderReal, g_hProgramFolder);\n   end\n', '   end\n')),
        ('what another account could change never noted by the check',
         altered(texts, EXTENSION, '   NoteFoldersOthersCouldChange();\nend;', 'end;')),
        ('the restart not recorded when setup hardens what another account could change',
         altered(texts, EXTENSION, "   RecordRestartNeeded(szApp, 'hardened folders", "   Log(szApp + 'hardened folders")),
        ('the restart recorded in a key a restart does not take away',
         altered(texts, EXTENSION, 'HM_REG_OPTION_VOLATILE, HM_KEY_SET_VALUE_64', '0, HM_KEY_SET_VALUE_64')),
        ('what setup recorded until the restart not read before the check',
         altered(texts, EXTENSION, '   if RestartNeededFor(szApp, szWhy) then\n', '   if false then\n')),
        ('a refusal whose remedy ends in a restart not recorded',
         altered(texts, EXTENSION, "      if Pos('restarts Windows', Result) > 0 then\n", '      if false then\n')),
        ('the Ready page checking without hardening what another account could change',
         altered(texts, EXTENSION, "\t\tg_szFolderRefusal := CheckFoldersBeforeTheStop(PreviousInstallationFolder());", "\t\tg_szFolderRefusal := CheckProgramFolders(PreviousInstallationFolder());")),
        ('a Bin another account owns taken in PrepareProgramFolders, where no restart follows',
         altered(texts, EXTENSION, 'HardenHeldFolder(g_szBinReal, g_hBin, false, szWhy)', 'HardenHeldFolder(g_szBinReal, g_hBin, g_bEarlierInstallation, szWhy)')),
        ('whether a folder was found changed cleared after the check',
         altered(texts, EXTENSION, '   g_hProgramFolderPrepared := g_hProgramFolder;\n\n   // Bin and ControlPanel:', '   g_hProgramFolderPrepared := g_hProgramFolder;\n   g_bFoldersChanged := false;\n\n   // Bin and ControlPanel:')),
        # The file setup keeps in a folder it hardened in place.
        ('no file kept in a Bin hardened in place, which an old handle can then make a junction once it is emptied',
         altered(texts, EXTENSION, '      else if not MakePlaceholder(g_szBinReal, g_hBin, szWhy) then', '      else if false then')),
        ('no file kept in a program folder hardened in place',
         altered(texts, EXTENSION, '   else if not MakePlaceholder(g_szProgramFolderReal, g_hProgramFolder, szWhy) then', '   else if false then')),
        ('the file kept shared for deleting',
         altered(texts, EXTENSION, 'HM_PLACEHOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_CREATE_NEW', 'HM_PLACEHOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_CREATE_NEW')),
        ('the file kept taken where a link put the folder elsewhere',
         altered(texts, EXTENSION, '   if not MadeWhereNamed(HeldHandle, handle, HM_PLACEHOLDER_NAME, szWhere) then', '   if false then')),
        ('the files kept not let go after the files',
         altered(texts, EXTENSION, '\t  ReleasePlaceholders();\n\t  RestoreServiceStart();', '\t  RestoreServiceStart();')),
        ('the files kept not let go when setup ends before the files',
         altered(texts, EXTENSION, '\tReleasePlaceholders();\n\tif not g_bFilesInstalled then', '\tif not g_bFilesInstalled then')),
        # Removal and walks through handles.
        ('a link found after the files removed by its path',
         altered(texts, EXTENSION, "                  Found := Found + 1;\n                  Why := Why + szItem + ' ' + szWhat", "                  RemoveDir(szItem);\n                  Found := Found + 1;\n                  Why := Why + szItem + ' ' + szWhat")),
        ('an empty folder of Bin removed by its path',
         altered(texts, EXTENSION, 'if RemoveHeldFolder(szItem, szFinal, szRemoved) then', 'if RemoveDir(szItem) then')),
        ('an empty folder of the program folder removed by its path',
         altered(texts, EXTENSION, 'if RemoveAndMakeAgain(szItem, AppHandle, szAppFinal, szRemoved) then', 'if RemoveDir(szItem) then')),
        ('an empty folder of the program folder removed by its path before it is made again (check 8 of R1 to R4)',
         altered(texts, EXTENSION, '   if not RemoveHeldFolder(Item, AppFinal, Why) then\n', '   if not RemoveDir(Item) then\n')),
        ('a folder walked after the files whatever its handle says it is',
         altered(texts, EXTENSION, "   if (szFinal = '') or (CompareText(szFinal, Expected) <> 0) then", '   if false then')),
        ('a folder of Bin walked before the files as a link or elsewhere',
         altered(texts, EXTENSION, "   if (szFinal = '') or ((Expected <> '') and (CompareText(szFinal, Expected) <> 0)) then", '   if false then')),
        ('Bin walked after the files when it was not found as setup left it',
         altered(texts, EXTENSION, "      if PreparedFolderIsAsLeft(g_szBinReal, g_hBinPrepared, szApp + 'Bin\\hMailServer.INI', true, SetupMadeBin(), Why) then\n",
                 "      PreparedFolderIsAsLeft(g_szBinReal, g_hBinPrepared, szApp + 'Bin\\hMailServer.INI', true, SetupMadeBin(), Why);\n")),
        ('the five folders walked after the files when the program folder was not found as setup left it',
         altered(texts, EXTENSION, "   if szAppFinal <> '' then\n      for index := 0 to 4 do", '   for index := 0 to 4 do')),
        ('a folder setup made held after the files to the rule of one it hardened, which keeps other accounts\' entries',
         altered(texts, EXTENSION, "szApp + 'Bin', g_bProgramFolderHardened, g_bProgramFolderNew, Why)", "szApp + 'Bin', g_bProgramFolderHardened, false, Why)")),
        ('an empty folder removed through a handle that is not the folder of its name',
         altered(texts, EXTENSION, "   else if (szFinal = '') or (ParentFinal = '') or (CompareText(szFinal, AddBackslash(ParentFinal) + ExtractFileName(Folder)) <> 0) then",
                 "   else if szFinal = '' then")),
        # The exit code and the service when the folders were changed.
        ('folders found not as setup left them ending in 10, which the Updater rolls back',
         altered(texts, EXTENSION, '\tif g_bFoldersNotAsPrepared then\n\t\tResult := 12\n\telse if', '\tif')),
        ('a folder found changed while setup prepared it leaving the service to start',
         altered(texts, EXTENSION, '      if g_bFoldersChanged then\n      begin\n         KeepServiceFromStarting();\n', '      if g_bFoldersChanged then\n      begin\n')),
        ('the Updater not told after the files that the folders were changed',
         altered(texts, EXTENSION, '\t    Log(FoldersChangedLine());\n', '')),
        ('something new in the last look not taken for a folder changed',
         altered(texts, EXTENSION, "      g_bFoldersChanged := true;\n      Result := 'Setup does not install into ' + szApp + ', since the service",
                 "      Result := 'Setup does not install into ' + szApp + ', since the service")),
        ('a service setup set not to start left so for good (no mark)',
         altered(texts, EXTENSION, "      RegWriteDWordValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting', 1);\n", '')),
        # Where a data folder's link leads.
        ('a data folder\'s link not held to what the grant after the files refuses',
         altered(texts, EXTENSION, '   if not DataLinkTargetIsGrantable(hHeld, szReal, szWhy) then', '   if false then')),
        ('a Database link\'s target not held to the rule of a settings file',
         altered(texts, EXTENSION, '      if not DatabaseTargetIsTheAdministrators(hHeld, szReal, szWhy) then', '      if false then')),
        ('a Database link\'s target that a group of every user may write taken',
         altered(texts, EXTENSION, "   else if AclLetsEveryUserWrite(pDacl) then\n      Why := 'it leads to '", "   else if false then\n      Why := 'it leads to '")),
        ('a data folder\'s link into the program folder\'s Bin taken',
         altered(texts, EXTENSION, '   if IsAtOrBelow(szFinal, szApp) then\n      if not (IsAtOrBelow(szFinal,', '   if false then\n      if not (IsAtOrBelow(szFinal,')),
        # What Inno Setup does outside the [Code] (check 4 of R1 to R4, 27 October 2026,
        # its mutations 4.01 to 4.12).
        ('a later [Dirs] entry giving Bin Users Modify',
         altered(texts, DIRS, 'Name: "{app}\\Temp"; Components: server;', 'Name: "{app}\\Temp"; Components: server;\nName: "{app}\\Bin"; Permissions: users-modify')),
        ('a later [Dirs] entry giving the program folder Everyone Modify',
         altered(texts, DIRS, 'Name: "{app}\\Temp"; Components: server;', 'Name: "{app}\\Temp"; Components: server;\nName: "{app}"; Permissions: everyone-modify')),
        ('[Dirs] giving Events Users Modify',
         altered(texts, DIRS, 'Name: "{app}\\Events"; Components: server;', 'Name: "{app}\\Events"; Components: server; Permissions: users-modify')),
        ('[Files] giving Bin\\hMailServer.exe Users Modify',
         altered(texts, 'section_files_64.iss', 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete;',
                 'hMailServer.exe"; DestDir: "{app}\\Bin"; Permissions: users-modify; Flags: ignoreversion uninsrestartdelete;')),
        ('a [Run] entry without postinstall running Bin\\hMailServer.exe before ssPostInstall',
         altered(texts, 'section_run.iss', 'Filename: "{app}\\ControlPanel\\hMailCP.exe";',
                 'Filename: "{app}\\Bin\\hMailServer.exe"; Parameters: "/RegisterTypeLib"; Flags: runhidden; Components: server;\nFilename: "{app}\\ControlPanel\\hMailCP.exe";')),
        ('a Bin program registered during the copy (regserver)',
         altered(texts, 'section_files_64.iss', 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete;',
                 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete regserver;')),
        ('IsServiceInstalled always false, so that no service found not as prepared is kept from starting',
         altered(texts, EXTENSION, '   service := OpenNamedService(Name, SERVICE_QUERY_CONFIG, manager);\n   Result := (service <> 0);',
                 '   Result := false;\n   Exit;\n   service := OpenNamedService(Name, SERVICE_QUERY_CONFIG, manager);\n   Result := (service <> 0);')),
        ('OpenNamedService answering the connection to the service manager for the service, so that IsServiceInstalled is true of a service that is not there',
         altered(texts, EXTENSION, '   if (Manager = 0) then\n      Exit;\n   Result := OpenServiceW(Manager, Name, Access);',
                 '   if (Manager = 0) then\n      Exit;\n   Result := Manager;\n   Exit;\n   Result := OpenServiceW(Manager, Name, Access);')),
        ('CloseServiceAndManager not letting go of the service, so that each IsServiceInstalled leaves a handle to it open',
         altered(texts, EXTENSION, '   if (Service <> 0) then\n      CloseServiceHandle(Service);', '   if (Service <> 0) then\n      Service := 0;')),
        # Check 9 of R1 to R4 (31 October 2026), its 9.01 to 9.03, and the declarations
        # those routines rest on.
        # Check 9 of R1 to R4 (31 October 2026): a program folder path Windows reads as another.
        ('a program folder path Windows reads as another not refused',
         altered(texts, EXTENSION, '   if CompareText(szWhy, szApp) <> 0 then\n   begin\n      Result := \'Setup does not install into \' + szApp + \': Windows reads that path as \'',
                 '   if false then\n   begin\n      Result := \'Setup does not install into \' + szApp + \': Windows reads that path as \'')),
        ('the program folder path compared with itself rather than with the path Windows reads',
         altered(texts, EXTENSION, '   szWhy := RemoveBackslashUnlessRoot(ExpandFileName(szApp));\n', '   szWhy := RemoveBackslashUnlessRoot(szApp);\n')),
        ('MakeFolders making a name that ends in a dot',
         altered(texts, EXTENSION, "      if (szName <> '') and ((Copy(szName, Length(szName), 1) = '.') or (Copy(szName, Length(szName), 1) = ' ')) then\n",
                 "      if (szName <> '') and (Copy(szName, Length(szName), 1) = ' ') then\n")),
        ('MakeFolders making a name that ends in a dot or a space, the guard gone',
         altered(texts, EXTENSION, "      if (szName <> '') and ((Copy(szName, Length(szName), 1) = '.') or (Copy(szName, Length(szName), 1) = ' ')) then\n      begin\n         Why := ",
                 "      if false then\n      begin\n         Why := ")),
        # Check 9 of R1 to R4 (31 October 2026): an empty folder made again whose list inherited.
        ('an empty folder made again whose list inherited given that list as read, an entry it kept from where it was made carried over',
         altered(texts, EXTENSION, '      made := MakeFolderIn(AppHandle, ExtractFileName(Item), 0, status)\n', '      made := MakeFolderIn(AppHandle, ExtractFileName(Item), pDescriptor, status)\n')),
        ('every empty folder made again taken for one whose list inherited nothing',
         altered(texts, EXTENSION, '      bInherits := (control and HM_DACL_PROTECTED) = 0;\n', '      bInherits := false;\n')),
        ('an empty folder made again whose list inherited not given its entries of its own',
         altered(texts, EXTENSION, '      if not WriteOwnEntriesOn(AppHandle, made, Item, ExtractFileName(Item), pDacl, Why) then\n', '      if false then\n')),
        ('the entries an empty folder had marked inherited written on the folder made again as well as its own',
         altered(texts, EXTENSION, '((pass = 1) and bOwn and (not bDeny))', '((pass = 1) and (not bDeny))')),
        ('the entries of its own written on the folder made again through a link',
         altered(texts, EXTENSION, "   if HeldIsLink(handle, bKnown) then\n      Why := 'it is not the folder setup made: it is a junction",
                 "   if false then\n      Why := 'it is not the folder setup made: it is a junction")),
        ('the entries of its own written on whatever folder the path names, not the one setup made',
         altered(texts, EXTENSION, '(openedInformation.nFileIndexLow <> heldInformation.nFileIndexLow) then', '(openedInformation.nFileIndexLow = heldInformation.nFileIndexLow) and false then')),
        ('the entries of its own written with the list protected, what the program folder hands down gone',
         altered(texts, EXTENSION, 'written := WriteHeldSecurity(handle, HM_FILE_OBJECT, HM_UNPROTECTED_DACL_INFORMATION, 0, 0, pNew, 0);',
                 'written := WriteHeldSecurity(handle, HM_FILE_OBJECT, HM_PROTECTED_DACL_INFORMATION, 0, 0, pNew, 0);')),
        ('a folder made again whose entries of its own are not all there when read again taken',
         altered(texts, EXTENSION, '   if found <> own then\n', '   if false then\n')),
        ('OpenServiceManager answering 0 without asking the service manager, so that IsServiceInstalled is always false',
         altered(texts, EXTENSION, '   Result := OpenSCManagerW(0, 0, SC_MANAGER_CONNECT);\n', '   Result := 0;\n')),
        ('IsServiceRunning always false, so that the Ready page neither stops a running service nor says it stopped it',
         altered(texts, EXTENSION, '   Result := (GetServiceState(Name) = SERVICE_RUNNING);\n', '   Result := false;\n')),
        ('GetServiceState answering SERVICE_STOPPED whatever state the service is in',
         altered(texts, EXTENSION, '      Result := status.dwCurrentState;\n', '      Result := SERVICE_STOPPED;\n')),
        ('IsServiceStopped always true, so that a running service is not asked to stop',
         altered(texts, EXTENSION, '   Result := (GetServiceState(Name) = SERVICE_STOPPED);\n', '   Result := true;\n')),
        ('SERVICE_RUNNING given the value of SERVICE_START_PENDING, so that no running service is taken for running',
         altered(texts, EXTENSION, '\tSERVICE_RUNNING             = $00000004;', '\tSERVICE_RUNNING             = $00000002;')),
        ('QueryServiceStatusProcess declared from QueryServiceStatus, which fills the shorter SERVICE_STATUS',
         altered(texts, EXTENSION, "external 'QueryServiceStatusEx@advapi32.dll stdcall';", "external 'QueryServiceStatus@advapi32.dll stdcall';")),
        ('RunPostInstallTasks no longer running /ProtectFolders',
         altered(texts, EXTENSION, '         ProtectFolders();\n', '')),
        ('folders not as prepared ending in 0 (steps incomplete taken for complete)',
         altered(texts, EXTENSION, '\telse if g_bStepsIncomplete then\n\t\tResult := 10', '\telse if g_bStepsIncomplete then\n\t\tResult := 0')),
        ('/ProtectFolders run from Bin at ssInstall, before the files',
         altered(texts, EXTENSION, '\t   MoveIni();\n', '\t   MoveIni();\n\t   ProtectFolders();\n')),
        ('the folders setup made removed after a successful installation too',
         altered(texts, EXTENSION, '\tReleaseHeldFolders();\nend;', '\tReleaseHeldFolders();\n\tRemoveFoldersMadeHere();\nend;')),
        ('the Ready page\'s check cleared after it, so that PrepareToInstall checks again after the stop',
         altered(texts, EXTENSION, "\t\tif g_szFolderRefusal <> '' then\n\t\t\tExit;\n", "\t\tif g_szFolderRefusal <> '' then\n\t\t\tExit;\n\t\tg_bFoldersChecked := false;\n")),
        ('a data folder given to the uninstaller whatever is in it',
         altered(texts, DIRS, 'Name: "{app}\\Data"; Components: server;', 'Name: "{app}\\Data"; Flags: uninsalwaysuninstall; Components: server;')),
        # The Updater (check 4 of R1 to R4, 27 October 2026).
        ('the Updater\'s code for folders found changed not 12',
         altered(texts, UPDATER, 'const int InstallerFoldersChanged = 12;', 'const int InstallerFoldersChanged = 13;')),
        ('the Updater looking in the log for another line than the installer writes',
         altered(texts, UPDATER, 'const std::wstring prefix = L"Setup found the folders it prepares changed";', 'const std::wstring prefix = L"Setup found the folders changed";')),
        ('the administrator\'s install putting the earlier version back over folders found changed',
         altered(texts, UPDATER, '      if (code == InstallerFoldersChanged || (code != 0 && FoldersFoundChanged(installLog)))\n', '      if (false)\n')),
        ('the unattended apply putting the earlier version back over folders found changed',
         altered(texts, UPDATER, '   if (code == InstallerFoldersChanged || (code != 0 && FoldersFoundChanged(installLog)))\n   {', '   if (false)\n   {')),
        ('the Updater waiting for a service the installer set not to start',
         altered(texts, UPDATER, 'code == InstallerDatabaseUpgraded || code == InstallerFoldersChanged)', 'code == InstallerDatabaseUpgraded)')),
        ('the installer\'s line for the Updater changed',
         altered(texts, EXTENSION, "Result := 'Setup found the folders it prepares changed (work item 78)", "Result := 'Setup found its folders changed (work item 78)")),
        # Bin and ControlPanel in a new program folder.
        ('Bin not made in a program folder setup made, so that a cancel leaves it and the program folder behind',
         altered(texts, EXTENSION, "      if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'Bin', 1, hInside, szWhy) then", '      if false then')),
        ('a refusal of PrepareToInstallRefusal\'s, before the program folders are prepared, made a note',
         altered(texts, EXTENSION, "         Result := 'The database given with ' + g_szDatabaseFrom", "         Log('The database given with ' + g_szDatabaseFrom")),
        # What Inno Setup runs outside the [Code]'s steps during the install (check 5 of R1
        # to R4, 27 October 2026: its mutations 5.01 to 5.06 and 5.29 to 5.31 passed).
        ('a [Files] entry running hMailServer.exe /ProtectFolders as soon as it is copied (AfterInstall: ProtectFolders)',
         altered(texts, 'section_files_64.iss', 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;',
                 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; AfterInstall: ProtectFolders; Components: server admintools;')),
        ('a [Files] entry letting every hold go during the copy (AfterInstall: ReleaseHeldFolders)',
         altered(texts, 'section_files_64.iss', 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;',
                 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; AfterInstall: ReleaseHeldFolders; Components: server admintools;')),
        ('a [Files] entry letting the files setup keeps go before the first file (BeforeInstall: ReleasePlaceholders)',
         altered(texts, 'section_files_64.iss', 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;',
                 'hMailServer.exe"; DestDir: "{app}\\Bin"; Flags: ignoreversion uninsrestartdelete; BeforeInstall: ReleasePlaceholders; Components: server admintools;')),
        ('CurInstallProgressChanged letting every hold and the files setup keeps go during the copy',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 'procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);\nbegin\n\tReleasePlaceholders();\n\tReleaseHeldFolders();\nend;\n\nprocedure DeinitializeSetup();\nbegin')),
        ('CurInstallProgressChanged defined at all, doing nothing of R1 to R4',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 'procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);\nbegin\n\tLog(IntToStr(CurProgress));\nend;\n\nprocedure DeinitializeSetup();\nbegin')),
        # Repair 7 of R1 to R4 (check 7, 28 October 2026): Inno Setup's other event functions, and a lone carriage return.
        ('7.01 NeedRestart, which Inno Setup calls after the files, running /ProtectFolders before the folders are read again (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 'function NeedRestart(): Boolean;\nbegin\n   ProtectFolders();\n   Result := false;\nend;\n\nprocedure DeinitializeSetup();\nbegin')),
        ('7.02 NeedRestart running {app}\\Bin\\hMailServer.exe itself (Exec) (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 "function NeedRestart(): Boolean;\nvar\n   ResultCode: Integer;\nbegin\n   Exec(ExpandConstant('{app}\\Bin\\hMailServer.exe'), '/RegisterTypeLib', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);\n"
                 "   Result := false;\nend;\n\nprocedure DeinitializeSetup();\nbegin")),
        ('7.03 RegisterPreviousData, which Inno Setup calls as it saves the uninstall information, running /RegisterTypeLib (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 'procedure RegisterPreviousData(PreviousDataKey: Integer);\nbegin\n   RegisterTypeLib();\nend;\n\nprocedure DeinitializeSetup();\nbegin')),
        ('NeedRestart defined at all, doing nothing of R1 to R4 (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 'function NeedRestart(): Boolean;\nbegin\n   Result := false;\nend;\n\nprocedure DeinitializeSetup();\nbegin')),
        ('CurPageChanged running /RegisterTypeLib through the routine that runs it, as the install begins (check 7 of R1 to R4)',
         altered(texts, EXTENSION, '   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then\n      Exit;',
                 '   RegisterTypeLib();\n   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then\n      Exit;')),
        ('UpdateReadyMemo defined and running /ProtectFolders before the folders are looked at (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'procedure DeinitializeSetup();\nbegin',
                 "function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;\nbegin\n"
                 "   ProtectFolders();\n   Result := MemoDirInfo;\nend;\n\nprocedure DeinitializeSetup();\nbegin")),
        ('ShouldSkipPage letting the holds go through a routine it calls (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'function ShouldSkipPage(PageID: Integer): Boolean;\nbegin\n   Result := false;\n',
                 'procedure LetEverythingGo();\nbegin\n   ReleaseHeldFolders();\nend;\n\nfunction ShouldSkipPage(PageID: Integer): Boolean;\nbegin\n   LetEverythingGo();\n   Result := false;\n')),
        ('7.06 a lone carriage return in a // comment of LinkIsAdministratorsAlone, then "Result := true; Exit;" (check 7 of R1 to R4)',
         altered(texts, EXTENSION, 'function LinkIsAdministratorsAlone(hLink: Integer; hParent: Integer; var Why: String): Boolean;\nvar\n   pOwner: cardinal;\n   pDacl: cardinal;\n   pDescriptor: cardinal;\n   readResult: cardinal;\n   szWho: String;\nbegin\n',
                 'function LinkIsAdministratorsAlone(hLink: Integer; hParent: Integer; var Why: String): Boolean;\nvar\n   pOwner: cardinal;\n   pDacl: cardinal;\n   pDescriptor: cardinal;\n   readResult: cardinal;\n   szWho: String;\nbegin\n'
                 '   // what follows is a comment\rResult := true; Exit;\n')),
        ('7.07 a lone carriage return after a comment line of section_dirs.iss, then #if 0 around the {app} entry (check 7 of R1 to R4)',
         altered(texts, DIRS, 'Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder\n',
                 '; the program folder\r#if 0\nName: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder\n; end\r#endif\n')),
        ('7.08 a lone carriage return after a comment line of section_dirs.iss, then a [Dirs] entry giving Bin Users Modify (check 7 of R1 to R4)',
         altered(texts, DIRS, 'Name: "{app}\\Temp"; Components: server;', 'Name: "{app}\\Temp"; Components: server;\n; Bin\rName: "{app}\\Bin"; Permissions: users-modify')),
        ('a [Run] entry without postinstall starting Bin\\hMailServer.exe through {cmd} before ssPostInstall',
         altered(texts, 'section_run.iss', 'Filename: "{app}\\ControlPanel\\hMailCP.exe";',
                 'Filename: "{cmd}"; Parameters: "/c ""{app}\\Bin\\hMailServer.exe"" /RegisterTypeLib"; Flags: runhidden; Components: server;\nFilename: "{app}\\ControlPanel\\hMailCP.exe";')),
        ('a [Run] entry without postinstall starting Bin\\DBSetupQuick.exe through {sys}\\cmd.exe before ssPostInstall',
         altered(texts, 'section_run.iss', 'Filename: "{app}\\ControlPanel\\hMailCP.exe";',
                 'Filename: "{sys}\\cmd.exe"; Parameters: "/c ""{app}\\Bin\\DBSetupQuick.exe"""; Flags: runhidden; Components: server;\nFilename: "{app}\\ControlPanel\\hMailCP.exe";')),
        ('a [Run] entry without postinstall starting Bin\\hMailServer.exe by a {code:} file name',
         altered(texts, 'section_run.iss', 'Filename: "{app}\\ControlPanel\\hMailCP.exe";',
                 'Filename: "{code:FolderMadeBySetup|0}\\Bin\\hMailServer.exe"; Parameters: "/RegisterTypeLib"; Flags: runhidden; Components: server;\nFilename: "{app}\\ControlPanel\\hMailCP.exe";')),
        ('the .NET runtime\'s [Run] entry given a parameter of {app}',
         altered(texts, 'section_run.iss', 'Parameters: "/install /quiet /norestart";', 'Parameters: "/install /quiet /norestart /log ""{app}\\Logs\\dotnet.log""";')),
        ('an [INI] entry running /ProtectFolders during the install (AfterInstall: ProtectFolders)',
         altered(texts, 'section_ini.iss', 'Filename: "{app}\\Bin\\hMailServer.INI"; Section: "Directories"; Key: "ProgramFolder"; String: "{app}";',
                 'Filename: "{app}\\Bin\\hMailServer.INI"; Section: "Directories"; Key: "ProgramFolder"; String: "{app}"; AfterInstall: ProtectFolders;')),
        ('an [Icons] entry letting the files setup keeps go during the install (AfterInstall: ReleasePlaceholders)',
         altered(texts, 'section_icons.iss', 'Name: "{group}\\hMailServer Database Setup"; Filename: "{app}\\Bin\\DBSetup.exe"; Components: server;',
                 'Name: "{group}\\hMailServer Database Setup"; Filename: "{app}\\Bin\\DBSetup.exe"; Components: server; AfterInstall: ReleasePlaceholders')),
        ('a [Dirs] entry letting every hold go as Inno Setup makes Temp (AfterInstall: ReleaseHeldFolders)',
         altered(texts, DIRS, 'Name: "{app}\\Temp"; Components: server;', 'Name: "{app}\\Temp"; Components: server; AfterInstall: ReleaseHeldFolders')),
        ('a [Dirs] entry\'s Check a function the check does not hold, which Inno Setup calls during the install',
         altered(texts, DIRS, 'Name: "{app}\\Temp"; Components: server;', 'Name: "{app}\\Temp"; Components: server; Check: ReleaseAndAllow')),
        ('ReleaseHeldFolders called from CurPageChanged, as the install begins',
         altered(texts, EXTENSION, '   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then\n      Exit;',
                 '   ReleaseHeldFolders();\n   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then\n      Exit;')),
        ('CurPageChanged running a program as the install begins',
         altered(texts, EXTENSION, '   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then\n      Exit;',
                 '   Exec(\'cmd.exe\', \'\', \'\', SW_HIDE, ewNoWait, code);\n   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then\n      Exit;')),
        ('ReleasePlaceholders called from RunPostInstallTasks too',
         altered(texts, EXTENSION, '      szCreate: String;\nbegin\n   Result := true;\n   bRuntimeReady := true;\n',
                 '      szCreate: String;\nbegin\n   ReleasePlaceholders();\n   Result := true;\n   bRuntimeReady := true;\n')),
        ('LetGoOfHeld called from HardenHeldFolder, letting a folder go before it is hardened',
         altered(texts, EXTENSION, '   bUsers := EveryUserMayRead(pDacl);\n', '   LetGoOfHeld(HeldHandle);\n   bUsers := EveryUserMayRead(pDacl);\n')),
        # Repair 6 of R1 to R4 (check 6, 28 October 2026): what the Inno Setup preprocessor makes of the scripts - check 6's
        # mutations6.py 6.01 to 6.06, 6.10 and 6.15, which the checker passed, and more.
        ('LinkIsAdministratorsAlone taking every link by an inline directive at its start (check 6\'s 6.01)',
         altered(texts, EXTENSION, '   szWho: String;\nbegin\n   Result := false;\n   Why := \'\';\n   pOwner := 0;\n   pDacl := 0;\n   pDescriptor := 0;\n   readResult := ReadHeldSecurity(hLink,',
                 '   szWho: String;\nbegin\n   {#"Result := true; Exit;"}\n   Result := false;\n   Why := \'\';\n   pOwner := 0;\n   pDacl := 0;\n   pDescriptor := 0;\n   readResult := ReadHeldSecurity(hLink,')),
        ('FolderAboveIsAdministratorsAlone taking every folder above the program folder by an inline directive (check 6\'s 6.02)',
         altered(texts, EXTENSION, '   szSid: String;\n   szRemedy: String;\nbegin\n', '   szSid: String;\n   szRemedy: String;\nbegin\n   {#"Result := true; Why := \'\'; Exit;"}\n')),
        ('OwnerIsAllowed allowing every owner by an inline directive (check 6\'s 6.03)',
         altered(texts, EXTENSION, "   Result := HeldOwnerIsTrusted(HeldHandle, bKnown, szWho);\n   Why := '';\n   if Result then",
                 "   Result := HeldOwnerIsTrusted(HeldHandle, bKnown, szWho);\n   Why := '';\n   {#\"Result := true;\"}\n   if Result then")),
        ('MakeFolders making the program folder with Everyone Full control by an inline directive (check 6\'s 6.04)',
         altered(texts, EXTENSION, "   pDescriptor := 0;\n   if not SecurityDescriptorOfText(szDescriptor, 1, pDescriptor, 0) then",
                 "   {#\"szDescriptor := 'O:BAD:PAI(A;OICI;FA;;;WD)';\"}\n   pDescriptor := 0;\n   if not SecurityDescriptorOfText(szDescriptor, 1, pDescriptor, 0) then")),
        ('HardenHeldFolder granting Everyone Full control by an inline directive (check 6\'s 6.05)',
         altered(texts, EXTENSION, "      if not Exec(ExpandConstant('{sys}\\icacls.exe'), '\"' + Folder + '\" /inheritance:r /grant:r",
                 "      {#\"szUsers := szUsers + ' *S-1-1-0:(OI)(CI)F';\"}\n      if not Exec(ExpandConstant('{sys}\\icacls.exe'), '\"' + Folder + '\" /inheritance:r /grant:r")),
        ('FoldersStillAsPrepared answering true by an inline directive (check 6\'s 6.06)',
         altered(texts, EXTENSION, "   Why := '';\n   found := 0;\n   szApp := AddBackslash(ExpandConstant('{app}'));",
                 "   Why := '';\n   {#\"Result := true; Exit;\"}\n   found := 0;\n   szApp := AddBackslash(ExpandConstant('{app}'));")),
        ('CurStepChanged letting every hold and placeholder go before the files by an inline directive (check 6\'s 6.10)',
         altered(texts, EXTENSION, '\t   // Move hMailServer.ini before files are copied\n\t   MoveIni();\n\tend;',
                 '\t   // Move hMailServer.ini before files are copied\n\t   MoveIni();\n\t   {#"ReleasePlaceholders(); ReleaseHeldFolders();"}\n\tend;')),
        ('the [Dirs] entry of the program folder setup made inside #if 0 ... #endif, never given to the uninstaller (check 6\'s 6.15)',
         altered(texts, DIRS, 'Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder\n',
                 '#if 0\nName: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder\n#endif\n')),
        ('an inline directive in a comment of LinkIsAdministratorsAlone, which the preprocessor expands there too (check 6 of R1 to R4)',
         altered(texts, EXTENSION, '   szWho: String;\nbegin\n   Result := false;\n   Why := \'\';\n   pOwner := 0;\n   pDacl := 0;\n   pDescriptor := 0;\n   readResult := ReadHeldSecurity(hLink,',
                 '   szWho: String;\nbegin\n   // {#"x"}\n   Result := false;\n   Why := \'\';\n   pOwner := 0;\n   pDacl := 0;\n   pDescriptor := 0;\n   readResult := ReadHeldSecurity(hLink,')),
        ('an inline directive in a string of OwnerIsAllowed, which the preprocessor expands there too (check 6 of R1 to R4)',
         altered(texts, EXTENSION, "   Result := HeldOwnerIsTrusted(HeldHandle, bKnown, szWho);\n   Why := '';\n   if Result then",
                 "   Result := HeldOwnerIsTrusted(HeldHandle, bKnown, szWho);\n   Why := '{#\"'; Result := true; Why := '\"}';\n   if Result then")),
        ('the ARM64 block\'s #endif moved below the call of PrepareProgramFolders in PrepareToInstallRefusal, so that the x64 installer never prepares the folders (check 6 of R1 to R4)',
         altered(texts, EXTENSION, "#endif\n\n   Result := PrepareProgramFolders(PreviousInstallationFolder());\nend;",
                 "\n   Result := PrepareProgramFolders(PreviousInstallationFolder());\n#endif\nend;")),
        ('an ARM64 block moved to wrap the call of OverrideInstallationFolder (check 6 of R1 to R4)',
         altered(texts, EXTENSION, "#ifdef HM_ARM64\n   // No built-in database on ARM64, whatever the command line says: a new\n   // installation is set up against an external database by DBSetup.\n   g_bUseInternal := false;\n#endif\n\n   OverrideInstallationFolder();\n",
                 "   // No built-in database on ARM64, whatever the command line says: a new\n   // installation is set up against an external database by DBSetup.\n   g_bUseInternal := false;\n\n#ifdef HM_ARM64\n   OverrideInstallationFolder();\n#endif\n")),
        ('HM_ARM64 defined in the x64 installer, which then drops what the ARM64 blocks leave out (check 6 of R1 to R4)',
         altered(texts, 'hMailServer64.iss', '#define DOTNET_MAJOR "10"\n', '#define HM_ARM64\n#define DOTNET_MAJOR "10"\n')),
        ('the preprocessor\'s inline directive started otherwise (#pragma inlinestart) in the x64 installer (check 6 of R1 to R4)',
         altered(texts, 'hMailServer64.iss', '#define DOTNET_MAJOR "10"\n', '#pragma inlinestart "<%"\n#define DOTNET_MAJOR "10"\n')),
        ('section_dirs.iss no longer included in the x64 installer (check 6 of R1 to R4)',
         altered(texts, 'hMailServer64.iss', '#include "section_dirs.iss"\n', '')),
        ('a held routine defined again inside #else, the reviewed one inside #if 0 (check 6\'s 6.17)',
         altered(texts, EXTENSION, "function FoldersWereAsPrepared(): Boolean;\nbegin\n   Result := not g_bFoldersNotAsPrepared;\nend;\n",
                 "#if 0\nfunction FoldersWereAsPrepared(): Boolean;\nbegin\n   Result := not g_bFoldersNotAsPrepared;\nend;\n#else\nfunction FoldersWereAsPrepared(): Boolean;\nbegin\n"
                 "   Result := true;\nend;\n#endif\n")),
    ]

    # Every constant, Windows declaration and routine work item 78's check holds whole,
    # each changed (check 3 of R1 to R4, 27 October 2026).
    ext = texts.get(EXTENSION, '')
    for name, value, _ in PROGRAM_FOLDER_CONSTANTS:
        match = re.search(r'^([ \t]*' + re.escape(name) + r'[ \t]*=[ \t]*)' + re.escape(value) + r'([ \t]*;)', ext, re.M)
        if value.startswith("'"):
            other = value[:-2] + ('2' if value[-2] == '1' else '1') + "'"
        elif value.startswith('$'):
            other = '$' + format(int(value[1:], 16) ^ 1, '0%dX' % (len(value) - 1))
        else:
            other = str(int(value) + 1)
        controls.append(('%s held to %s, not %s' % (name, other, value),
                         altered(texts, EXTENSION, match.group(0), match.group(1) + other + match.group(2)) if match else None))
    controls.append(('a constant the check does not hold',
                     altered(texts, EXTENSION, 'HM_VOLUME_NAME_GUID         = 1;\n', 'HM_VOLUME_NAME_GUID         = 1;\n\tHM_VOLUME_NAME_NT           = 2;\n')))
    controls.append(('a group of every user ServiceAccount.cpp counts left out of BroadGroupSid (check 5 of R1 to R4)',
                     altered(texts, EXTENSION, "      15: begin Result := 'S-1-15-2-1'; Name := 'ALL APPLICATION PACKAGES'; end;\n", '')))
    controls.append(('BroadGroupSid counting a group ServiceAccount.cpp does not (check 5 of R1 to R4)',
                     altered(texts, EXTENSION, "      13: begin Result := 'S-1-5-6'; Name := 'SERVICE'; end;", "      13: begin Result := 'S-1-5-19'; Name := 'LOCAL SERVICE'; end;")))
    controls.append(('a domain\'s Domain Users not counted among the groups of every user (check 5 of R1 to R4)',
                     altered(texts, EXTENSION, "(Copy(szSid, Length(szSid) - 3, 4) = '-513')", "(Copy(szSid, Length(szSid) - 3, 4) = '-512')")))
    controls.append(('a domain\'s Domain Guests not counted among the groups of every user (check 6 of R1 to R4)',
                     altered(texts, EXTENSION, " or (Copy(szSid, Length(szSid) - 3, 4) = '-514')", '')))
    controls.append(('ServiceAccount.cpp not counting a domain\'s Domain Guests (check 6 of R1 to R4)',
                     altered(texts, SERVICE_ACCOUNT, ' || sidText.Right(4) == _T("-514")', '')))
    controls.append(('ServiceAccount.cpp and the installer counting a domain\'s Domain Admins as a group of every user (check 6 of R1 to R4)',
                     (lambda server: altered(server, EXTENSION, "(Copy(szSid, Length(szSid) - 3, 4) = '-514')", "(Copy(szSid, Length(szSid) - 3, 4) = '-512')") if server else None)(
                         altered(texts, SERVICE_ACCOUNT, 'sidText.Right(4) == _T("-514")', 'sidText.Right(4) == _T("-512")'))))
    controls.append(('ALL SERVICES left out of the groups of every user, in ServiceAccount.cpp and the installer alike (check 6 of R1 to R4)',
                     (lambda server: altered(server, EXTENSION, "      20: begin Result := 'S-1-5-80-0'; Name := 'ALL SERVICES'; end;\n", '') if server else None)(
                         altered(texts, SERVICE_ACCOUNT, '         L"S-1-5-80-0",    // ALL SERVICES\n', ''))))
    controls.append(('ServiceAccount.cpp counting a group of every user the installer does not (check 5 of R1 to R4)',
                     altered(texts, SERVICE_ACCOUNT, '         L"S-1-15-2-2",    // ALL RESTRICTED APPLICATION PACKAGES\n',
                             '         L"S-1-15-2-2",    // ALL RESTRICTED APPLICATION PACKAGES\n         L"S-1-5-1",       // DIALUP\n')))
    controls.append(('the live update\'s TrustedInstaller SID not the installer\'s',
                     altered(texts, ADMIN_UPDATE_CHECKS, 'L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"', 'L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478465"')))
    for declaration in PROGRAM_FOLDER_DECLARATIONS:
        name = re.match(r'(?:function|procedure)?\s*(\w+)', declaration).group(1)
        match = re.search(statement_pattern(declaration), ext)
        old = match.group(0) if match else ''
        if '@kernel32.dll' in old:
            new = old.replace('@kernel32.dll', '@advapi32.dll')
        elif '@advapi32.dll' in old:
            new = old.replace('@advapi32.dll', '@kernel32.dll')
        elif re.match(r'\w+ = [$0-9A-Fa-f]+;\Z', declaration):
            # A constant (the Service Control Manager's, check 9 of R1 to R4): one more.
            value = re.search(r'(\$?)([0-9A-Fa-f]+);\Z', old)
            if value.group(1):
                digits = '%0*X' % (len(value.group(2)), int(value.group(2), 16) + 1)
            else:
                digits = str(int(value.group(2)) + 1)
            new = old[:value.start(2)] + digits + ';'
        else:
            new = old.replace('cardinal', 'Word', 1)
        controls.append(('%s declared otherwise (another DLL, a field of another width, or a constant of another value)' % name, altered(texts, EXTENSION, old, new) if old else None))
    controls.append(('a Windows function R1 to R4 call that the check does not hold',
                     (lambda declared: altered(declared, EXTENSION, "   Result := false;\n   if FindFirst(AddBackslash(Folder) + '*', rec) then",
                                               "   Result := false;\n   if FolderIsEmpty(Folder) and FindFirst(AddBackslash(Folder) + '*', rec) then") if declared else None)(
                         altered(texts, EXTENSION, 'function FinalPathOfHandle(', "function FolderIsEmpty(pszPath: String): Boolean;\nexternal 'PathIsDirectoryEmptyW@shlwapi.dll stdcall';\n"
                                 'function FinalPathOfHandle('))))
    bodies = routine_bodies(ext)
    for name, _ in PROGRAM_FOLDER_CODE:
        body = bodies.get(name.lower(), (None, None))[1]
        begin = re.search(r'\bbegin\b', CODE_TOKEN.sub(lambda match: ' ' * len(match.group(0)), body)) if body else None
        controls.append(('%s changed by one statement (an Exit at its start)' % name,
                         altered(texts, EXTENSION, body, body[:begin.end()] + ' Exit;' + body[begin.end():]) if begin else None))
    controls.append(('a helper of R1 to R4 neither held nor said to be shared',
                     altered(texts, EXTENSION, 'function FoldersWereAsPrepared(): Boolean;\nbegin\n   Result := not g_bFoldersNotAsPrepared;\nend;',
                             'function FolderWasPrepared(): Boolean;\nbegin\n   Result := true;\nend;\n\n'
                             'function FoldersWereAsPrepared(): Boolean;\nbegin\n   Result := FolderWasPrepared() and (not g_bFoldersNotAsPrepared);\nend;')))
    trusted = bodies.get('istrustedsid', (None, ''))[1]
    controls.append(('a routine of R1 to R4 defined twice', altered(texts, EXTENSION, trusted, trusted + '\n\n' + trusted) if trusted else None))
    # Rebase repair 1 of R1 to R4 (check 8, 28 October 2026): what Inno Setup runs of the [Code], or of the installation,
    # between PrepareToInstall and the look after the files that the rules above did not name - check 8's mutations8.py 8.01,
    # 8.02 and 8.04 to 8.07b, which passed the checker and compiled, and more.
    deinit = 'procedure DeinitializeSetup();\nbegin'
    controls.append(('an earlier installation\'s empty data folder removed in the last look and left for Inno Setup to make again with what the program folder '
                     'hands down, as before check 8 of R1 to R4',
                     altered(texts, EXTENSION, '                  if RemoveAndMakeAgain(szItem, AppHandle, szAppFinal, szRemoved) then\n',
                             '                  if RemoveHeldFolder(szItem, szAppFinal, szRemoved) then\n')))
    controls.append(('an empty folder removed in the last look made again with the program folder\'s permissions, its own not read (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, '   made := MakeFolderIn(AppHandle, ExtractFileName(Item), pDescriptor, status);\n',
                             '   made := MakeFolderIn(AppHandle, ExtractFileName(Item), 0, status);\n')))
    controls.append(('an empty folder removed in the last look that could not be made again taken (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, "      Why := 'it was removed, but could not be made again with the permissions it had (NTSTATUS ' + Format('%.8x', [status]) + ')';\n      Exit;\n",
                             "      Log('it was removed, but could not be made again');\n")))
    controls.append(('a relative symbolic link whose name starts with a separator taken from the folder the link is in, as GetFullPathName reads the '
                     'joined path (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, "   if Copy(Name, 1, 1) = '\\' then\n      szPath := Name\n   else\n      szPath := szRest + '\\..\\' + Name;\n",
                             "   szPath := szRest + '\\..\\' + Name;\n")))
    controls.append(('a relative symbolic link that goes above the root of its volume taken at the root (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, "         if count = 0 then\n         begin\n            Why := 'it leads to ' + Name + ', above the root of its volume';\n"
                             "            Exit;\n         end;\n         count := count - 1;\n",
                             "         if count > 0 then\n            count := count - 1;\n")))
    controls.append(('a relative symbolic link taken by GetFullPathName again, as before check 8 of R1 to R4',
                     altered(texts, EXTENSION, '      if not RelativeLinkTarget(Link, szName, Target, Why) then\n         Exit;\n',
                             '      Target := ExpandFileName(AddBackslash(ExtractFileDir(Link)) + szName);\n')))
    controls.append(('8.01 RegisterExtraCloseApplicationsResources, which Inno Setup runs after PrepareToInstall and before the files, running '
                     '/ProtectFolders (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, deinit, 'procedure RegisterExtraCloseApplicationsResources();\nbegin\n   ProtectFolders();\nend;\n\n' + deinit)))
    controls.append(('8.02 RegisterExtraCloseApplicationsResources running {app}\\Bin\\hMailServer.exe itself (Exec) (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, deinit, "procedure RegisterExtraCloseApplicationsResources();\nvar\n   ResultCode: Integer;\nbegin\n"
                             "   Exec(ExpandConstant('{app}\\Bin\\hMailServer.exe'), '/RegisterTypeLib', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);\nend;\n\n" + deinit)))
    controls.append(('RegisterExtraCloseApplicationsResources defined at all, doing nothing of R1 to R4 (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, deinit, "procedure RegisterExtraCloseApplicationsResources();\nbegin\n   Log('registering');\nend;\n\n" + deinit)))
    controls.append(('8.04 CancelButtonClick running /RegisterTypeLib on the installing page, during the copy (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, deinit, 'procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);\nbegin\n'
                             '   if CurPageID = wpInstalling then\n      RegisterTypeLib();\nend;\n\n' + deinit)))
    controls.append(('CancelButtonClick letting every hold go through a routine it calls (check 8 of R1 to R4)',
                     altered(texts, EXTENSION, deinit, 'procedure LetEverythingGoOnCancel();\nbegin\n   ReleaseHeldFolders();\nend;\n\n'
                             'procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);\nbegin\n   LetEverythingGoOnCancel();\nend;\n\n' + deinit)))
    controls.append(('8.05 [Setup] UninstallDisplayIcon={code:...}, which Inno Setup expands as it saves the uninstall information before '
                     'ssPostInstall, its function running /ProtectFolders (check 8 of R1 to R4)',
                     (lambda setup: altered(setup, EXTENSION, deinit, "function HmUninstallIcon(Param: String): String;\nbegin\n   ProtectFolders();\n"
                                                                      "   Result := ExpandConstant('{app}\\Bin\\hMailServer.exe');\nend;\n\n" + deinit) if setup else None)(
                         altered(texts, 'section_setup.iss', 'AppId=hMailServer\n', 'AppId=hMailServer\nUninstallDisplayIcon={code:HmUninstallIcon}\n'))))
    controls.append(('8.06 [Setup] ChangesEnvironment=<a function>, which Inno Setup evaluates just after ssInstall, before the files, running '
                     '/RegisterTypeLib (check 8 of R1 to R4)',
                     (lambda setup: altered(setup, EXTENSION, deinit, 'function HmChangesEnvironment(): Boolean;\nbegin\n   RegisterTypeLib();\n'
                                                                      '   Result := false;\nend;\n\n' + deinit) if setup else None)(
                         altered(texts, 'section_setup.iss', 'AppId=hMailServer\n', 'AppId=hMailServer\nChangesEnvironment=HmChangesEnvironment\n'))))
    controls.append(('[Setup] Uninstallable=<an expression naming a function>, which Inno Setup evaluates before ssPostInstall (check 8 of R1 to R4)',
                     (lambda setup: altered(setup, EXTENSION, deinit, 'function HmUninstallable(): Boolean;\nbegin\n   Result := true;\nend;\n\n' + deinit)
                      if setup else None)(altered(texts, 'section_setup.iss', 'Uninstallable=true\n', 'Uninstallable=not WizardSilent or HmUninstallable\n'))))
    controls.append(('8.07b a last-page [Run] entry (postinstall) at the end of section_icons.iss running {app}\\Bin\\DBSetup.exe with no Check: '
                     'FoldersWereAsPrepared (check 8 of R1 to R4)',
                     altered(texts, 'section_icons.iss', 'Parameters: "STOP hMailServer"; Components: server;\n',
                             'Parameters: "STOP hMailServer"; Components: server;\n\n[Run]\nFilename: "{app}\\Bin\\DBSetup.exe"; Flags: postinstall nowait; Description: Run DBSetup\n')))
    controls.append(('a last-page [Run] entry outside section_run.iss held to the opposite of FoldersWereAsPrepared (check 8 of R1 to R4)',
                     altered(texts, 'section_icons.iss', 'Parameters: "STOP hMailServer"; Components: server;\n',
                             'Parameters: "STOP hMailServer"; Components: server;\n\n[Run]\nFilename: "{app}\\Bin\\DBSetup.exe"; Flags: postinstall nowait; '
                             'Description: Run DBSetup; Check: not FoldersWereAsPrepared\n')))

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
    if '--digests' in sys.argv[1:]:
        # PROGRAM_FOLDER_CODE as the tree is, for a reviewed change of R1 to R4.
        reached, _, _, _ = program_folder_reach(texts.get(EXTENSION, ''))
        shared = set(name.lower() for name, _ in PROGRAM_FOLDER_SHARED)
        for name in sorted(reached, key=str.lower):
            body = pascal_routine(texts.get(EXTENSION, ''), name)
            if name.lower() not in shared and body is not None:
                print("    ('%s', '%s')," % (name, code_digest(body)))
        return 0
    if '--preprocessor' in sys.argv[1:]:
        # ISPP_PLACES as the tree is, for a reviewed change of a preprocessor line.
        for (name, where, before, line, after), (count, _) in sorted(ispp_found(texts).items()):
            print('    (%r, %r, %r, %r, %r, %d),' % (name, where, before, line, after, count))
        return 0
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
          'what is in Bin and ControlPanel is held to the live update\'s rule, each decision is held to its statement, and every constant, '
          'Windows declaration and routine of R1 to R4 is held whole; %d negative controls refused' % (entries, controls))
    return 0


if __name__ == '__main__':
    sys.exit(main())
