# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The installer smoke test: the real Windows installer installed, upgraded and
   uninstalled on a throwaway machine, and every promise it makes checked. It runs ON
   that machine - a Windows Server VM reverted to a clean snapshot before and after
   (build/ci/installer-smoke-run.sh does that from the VMware host) - never on a
   machine anybody depends on: it registers a service, a COM server and ports 25, 110
   and 143, and it removes them again.

   It replaces the GitHub workflow installer-smoke.yml (in the history at v6.3.3)
   and keeps what that
   checked: a silent install bounded in time with Inno's log kept, the database
   step's own error when the install fails, the service registered on the installed
   hMailServer.exe, listening on 25, 110 and 143, and the COM object creatable. To
   that it adds the rest of what the installer promises:

     install    the Authenticode signature, the version the installer claims (the
                uninstall entry, hMailServer.exe and the COM object agree), the
                service running and automatic with its recovery actions, the three
                banners, the database made at the schema the build requires, the
                registry value and the COM registrations, the prerequisites, and no
                error log;
     upgrade    (when -UpgradeInstaller is given) a domain, an account with its
                settings and a delivered message made before, and after the newer
                installer the same checks at its version, the schema moved to the
                newer build's, and every one of those things still there: the
                message read over POP3 and IMAP with the account's password, the
                server setting in the SMTP banner, the ini's database and security
                sections unchanged, the components kept;
     uninstall  the service, the program files, the uninstall entry, the registry
                value and every COM registration gone, and exactly what the
                installer keeps kept (section_files_common.iss, section_ini.iss,
                section_dirs.iss): hMailServer.INI, the database, the message store,
                the logs, the event scripts and the addon scripts, and the REST API's
                key store beside the INI, which the server writes. Registry keys the
                run left anywhere else are listed, and any that names hMailServer
                fails.

   And what docs/SecureDefaults.md says an installation on a clean machine is to
   check, named as that document names each property:

     a new installation, when the first installer is 6.3.4 or later: the certificate
                record "Self-signed (made at installation)" over COM, its two files
                under Data\Certificates, the key's ACL granting nothing to
                BUILTIN\Users, the certificate RSA 2048 for 825 days naming the host
                and where it came from, each of 25, 587, 110 and 143 with optional
                STARTTLS and that certificate, Internet requiring TLS for
                authentication and My computer not, no NewInstallDefaultsPending row
                left, the verb's exit 0 in Inno's log, its rows on the audit trail,
                and the setup checklist; then, with the account made, STARTTLS
                offered and completing from 127.0.0.1 on all four with that
                certificate, from this machine's own LAN address a password refused
                before STARTTLS and taken after it on SMTP 587, IMAP and POP3, mail
                taken on 25 without TLS, and the sign-ins over 127.0.0.1 taking a
                password without TLS as before;
     an upgrade to 6.3.4 or later: the verb not run by the installer, every port's
                security and both ranges exactly as they were, no certificate record
                made, no mark, the checklist's sign_in_no_tls for Internet with no
                fix when the older release had no such defaults, and the verb run by
                hand leaving the upgraded server as it is (exit 5);
     the uninstall of a new installation: Data\Certificates kept with the rest of
                the data directory.

   And what docs/ServiceAccount.md says hMailServer.exe /ProtectFolders makes of the
   program's own folders ("What the other users of the computer may read"), after an
   installer of 6.3.7 or later: the program folder, Bin and ControlPanel inheriting
   nothing, SYSTEM and the Administrators with full control, BUILTIN\Users with Read &
   execute where every user could read them before the installer ran and no entry
   letting every user read them where none could, the owner SYSTEM, the Administrators
   or TrustedInstaller; no group of every user able to change the program folder or any
   folder or file in it - Addons, Events, the data folders and the rest as well as Bin
   and ControlPanel - and the account that ran the installer with no entry of its own
   that lets it change, and not the owner of, a folder, program or library there
   outside the four folders the service writes; the REST API's key store still the
   service's to write when the service runs as NT SERVICE\hMailServer; /ProtectFolders'
   exit 0 in Inno's log; and a second /ProtectFolders, run here, finding all three
   "already hardened" and ending in 0. After an earlier installer the run says, as
   INFO, which group of every user may change them. Whatever the installer, what icacls
   shows of everything in the program folder is kept in the logs (icacls-PHASE.txt).

   -InstallDirectory DIR installs the first installer into DIR (/DIR=) rather than
   Program Files; an upgrade keeps it. With -WeakParent the folder DIR is made in is
   made, when it is not there, and given Authenticated Users' Modify handed down - what
   the root of a drive hands every folder made in it on a Windows client, which this
   Windows Server VM's own root does not - so that the run sees an installation in such
   a folder hardened: a new one by the new installer alone, and one an earlier
   installer left there by the upgrade. When that folder is a drive's root
   (-InstallDirectory C:\hMailServer) it is given the entry a Windows client's root
   carries, inherit-only, and every folder below it that inherits gets it. Both are for
   this throwaway machine only, which is reverted after the run.

   And what docs/ServiceAccount.md says setup does before the files (work item 78),
   held to an installer after 6.3.7, or to a build that still says 6.3.7 with
   -HardenedBeforeFiles, and said as INFO of any other: a program folder the
   installer makes had permissions of its own from the first moment it was there -
   read by a watch in a runspace of its own as soon as the folder appears - and
   setup, not Inno Setup, made it (R1); Bin and ControlPanel, there before the
   installer, inherited nothing and let no group of every user change them at the
   moment the first new file appeared in each, and the setup log hardens each before
   the first file it copies there - or, one that held nothing, says setup removed it
   and made it again, not hardened in place - (R2) - an upgrade, and a folder made
   before the first installer (-FolderCase precreated-admin, data-inherits); and a Bin and a
   ControlPanel that were not there in a program folder that was were made by setup
   with permissions of their own before their first file, the setup log making each
   before the first file it copies there (R1, R2).

   -FolderCase NAME makes, before the first installer, what the program folder
   (-InstallDirectory) is to be found as - as the administrator; what a standard
   account owns is made and given to the account hmsmokeuser, which the run makes:
     precreated-admin  the program folder and Bin, inheriting from the folder above:
                       the program folder hardened before the files, Bin, which holds
                       nothing, removed and made again by setup before its first file
                       - the setup log saying so, a hardening in place not enough -
                       ControlPanel made by setup (R2);
     precreated-user   the same owned by hmsmokeuser, with Full control of its own,
                       and Bin\notes.txt it owns: refused, exit code 7 - the account
                       may hold its folder open - nothing written (R2);
     precreated-user-locked  the program folder made by the administrator, Bin owned
                       by hmsmokeuser with a list of its own that leaves SYSTEM and
                       the Administrators no right to change it: refused, exit 7 (R2);
     earlier-user-locked  the same, with Bin\hMailServer.INI the administrator
                       made, the program folder recorded as the earlier
                       installation's (InstallLocation): Bin given to the
                       Administrators with takeown /A and rebuilt, and the
                       installation refused until Windows has restarted, exit 7, a
                       second run refused by the record, and, the record taken away
                       as a restart takes it, installed, exit 0 (R2);
     data-inherits     the program folder, Bin and an inheriting Data made by the
                       administrator, no ControlPanel: the program folder hardened
                       before the files, Bin, which holds nothing, made again,
                       ControlPanel made by setup (R2);
     foreign-dll       the program folder and Bin made by the administrator, and
                       Bin\version.dll hmsmokeuser owns: refused, exit 7, nothing
                       written (R4);
     broad-entry       the program folder with an entry of its own giving
                       Authenticated Users Modify, and Bin\notes.txt hmsmokeuser
                       owns: the entry taken away and the installation refused until
                       Windows has restarted, exit 7, refused again by the record, then
                       installed with the file said in the setup log (R2, R4);
     link-user         the program folder a junction to C:\hm-linktarget that
                       hmsmokeuser owns: refused, exit code 7, nothing written
                       through it (R3);
     link-admin        the same made by the administrator, its list as the folder
                       above handed it - which is not the administrators' alone:
                       refused, exit code 7, with the remedy (R3);
     link-admin-only   the same given the list the remedy gives it: installed
                       through, its target hardened (R3);
     bin-link-user     the program folder made by the administrator and Bin a
                       junction hmsmokeuser owns: refused, exit code 7 (R3);
     held-open         the program folder and an empty Bin, inheriting from the
                       folder above, and Bin held open for writing by this script
                       before the installer, which makes it a junction to
                       C:\hm-linktarget through that handle the moment the program
                       folder's Data appears - after PrepareToInstall, before any
                       file is copied: setup removed Bin and made it again before
                       its first file, and nothing was copied through the junction
                       (R2); the base, which does not harden Bin before the files,
                       copies through it;
     held-full         the program folder, Bin and Bin\notes.txt, inheriting from the
                       folder above, Bin held open for writing and notes.txt for
                       deleting before the installer, the file deleted and Bin made a
                       junction through those handles the moment Data appears: Bin
                       and the program folder hardened and the installation refused
                       until Windows has restarted, exit 7, nothing copied through,
                       refused again by the record, then, the handles closed and the
                       record taken away, installed into the folder Bin is (R2); the
                       base copies through the junction;
     dac-handle        the program folder with an entry of its own giving Everyone
                       Full control, held open with WRITE_DAC before the installer:
                       hardened and refused until Windows has restarted, exit 7,
                       refused again, then installed with no entry for Everyone (R2,
                       R3); the base installs at once;
     copy-fails        -InstallDirectory two folders below a drive's root, a 64 MB
                       volume mounted on the folder between: setup makes the program
                       folder, Bin and ControlPanel there, the copy fails for want of
                       space, and setup leaves nothing it made (R1); the base leaves
                       Bin, and so the program folder;
     nested-new        nothing: -InstallDirectory is two or more folders below one
                       that is there, and every folder setup makes has permissions of
                       its own; with -Components controlpanel the uninstall leaves
                       none of them (R1);
     settings-user     the program folder and Bin made by the administrator, and
                       Bin\hMailServer.INI hmsmokeuser owns: refused, exit 7, the
                       file named with the /setowner remedy, nothing written (R4);
     events-link       the program folder made by the administrator and Events a
                       junction to C:\hm-linktarget only the administrators control:
                       refused, exit 7, with the rmdir remedy, nothing written
                       through it (R4);
     volume-root       the program folder a mount point - icacls /L giving it to the
                       administrators alone - of a volume of its own, a 64 MB VHD
                       diskpart makes, attaches, formats NTFS and mounts there:
                       refused, exit 7, the root of a volume named, with the remedy
                       of a folder on that volume (R2);
     weak-above        -InstallDirectory two folders below a drive's root, the root
                       given the Authenticated Users' Modify a Windows client's root
                       hands down and the folder between made by the administrator,
                       which inherits it: refused, exit 7, that folder named with the
                       icacls /inheritance:d and /remove:g *S-1-5-11 remedy, nothing
                       made (R3); the base installs there;
     weak-above-remedied  the same, the folder between given that remedy first:
                       installed below it, exit 0 (R3).
   -Components LIST installs the first installer with /COMPONENTS=LIST rather than
   /TYPE=full - controlpanel for the Control Panel alone. A run with -Components, or
   one of the cases a whole server installation does not run (all but
   precreated-admin), is one installer - the same one three times for a case refused until a
   restart - with the case's checks and an uninstall, without the server's.

   A new installation and an upgrade from a release without the defaults are two
   runs, each on the clean machine: the new installer alone, and the previous
   release's installer followed by the new one.

   Every check prints one line, PASS, FAIL, SKIP or INFO, then a dash and what it
   found. The exit code is 0 when nothing failed, 1 when a check failed and 2 when the
   machine was not clean to begin with. The lines also go to smoke.log in
   -LogDirectory, beside Inno's logs and copies of the server's.

   The silent switches are the installer's own (hMailServerInnoExtension.iss):
   /VERYSILENT /SUPPRESSMSGBOXES /NORESTART, /TYPE=full on the first install as the
   GitHub workflow passed it, and the administrator password, which a silent install
   takes with /adminpassword= or, from 6.3.4, in HM_ADMIN_PASSWORD. An upgrade that
   moves the schema needs it: DBUpdater authenticates as the administrator before it
   upgrades, and under /silent it fails rather than asks. The upgrade gets no /TYPE,
   as winget and Chocolatey pass none (docs/PackageManagers.md): Inno keeps the
   components already installed. The password is made here, used over COM on this
   machine, and never printed. When the first installer is 6.3.4 or later it carries
   a space, a double quote and backslashes before one, and goes in HM_ADMIN_PASSWORD,
   since Inno drops every double quote from its command line; before 6.3.4 the
   installer put the password on DBSetupQuick's command line unquoted, so it is
   letters and digits and goes with /adminpassword=.

   -AllowUnsigned is for a freshly built installer, which nobody has signed: an
   installer with no signature at all is then a SKIP, not a FAIL. A signature that is
   present and not valid fails either way.

   -DatabaseSettings FILE is a new installation on an external database, set up with
   no window by DBSetup /unattended (docs/WindowsOnArm.md, "A new installation without
   a window"): the only way an ARM64 installation can finish unattended, and on x64 the
   same path. FILE holds NAME=value lines of the variables DBSetup reads - HM_DB_TYPE,
   HM_DB_HOST, HM_DB_PORT, HM_DB_NAME, HM_DB_USER, HM_DB_PASSWORD or
   HM_DB_PASSWORD_FILE (a path on this machine), HM_CREATE_DATABASE, HM_DB_SERVICE -
   and '#' comments. Each is given to the installer as its switch (/dbtype=,
   /dbhost=, ..., /dbpasswordfile=), and HM_DB_PASSWORD in the environment, never on
   the command line; with -DatabaseInEnvironment every one goes in the environment
   instead, the installer's other route. The built-in database's checks then check
   the external one: [Database] naming it, its schema the build's over COM, no SQL
   Server Compact installed, and DBSetup /unattended's exit 0 in Inno's log. The two
   secure-defaults checks that read the database file directly are skipped. It is
   for a new installation only, so it does not go with -UpgradeInstaller: an
   installer before this one has no unattended database step. The database server is
   the run's to provide, reachable from this machine; the settings are never printed,
   the password least of all.

   Windows PowerShell 5.1, which is what a clean Windows Server has:
     powershell -NoProfile -ExecutionPolicy Bypass -File installer-smoke.ps1 `
        -Installer C:\smoke\first\hMailServer-6.3.2-x64.exe `
        -UpgradeInstaller C:\smoke\second\hMailServer-6.3.3-x64.exe
#>

[CmdletBinding()]
param(
   [Parameter(Mandatory = $true)][string] $Installer,
   [string] $UpgradeInstaller = '',
   [string] $LogDirectory = 'C:\smoke\logs',
   [switch] $AllowUnsigned,
   [int] $TimeoutMinutes = 10,
   [string] $DatabaseSettings = '',
   [switch] $DatabaseInEnvironment,
   [string] $InstallDirectory = '',
   [switch] $WeakParent,
   [ValidateSet('', 'precreated-admin', 'precreated-user', 'precreated-user-locked', 'earlier-user-locked', 'data-inherits', 'foreign-dll', 'broad-entry', 'link-user', 'link-admin', 'link-admin-only', 'bin-link-user', 'nested-new', 'held-open',
                'settings-user', 'events-link', 'volume-root', 'weak-above', 'weak-above-remedied', 'dac-handle', 'held-full', 'copy-fails')]
   [string] $FolderCase = '',
   [string] $Components = '',
   [switch] $HardenedBeforeFiles
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# C:/smoke/... from the run script becomes C:\smoke\...: Inno's /LOG= is given a path
# made from these.
$Installer = [System.IO.Path]::GetFullPath($Installer)
if ($UpgradeInstaller) { $UpgradeInstaller = [System.IO.Path]::GetFullPath($UpgradeInstaller) }
$LogDirectory = [System.IO.Path]::GetFullPath($LogDirectory)
if ($DatabaseSettings) { $DatabaseSettings = [System.IO.Path]::GetFullPath($DatabaseSettings) }
if ($InstallDirectory) { $InstallDirectory = [System.IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\') }

# -DatabaseSettings: each variable DBSetup /unattended reads, and the installer's
# switch for it (hMailServerInnoExtension.iss). The password has none.
$DatabaseVariables = [ordered]@{
   HM_DB_TYPE = 'dbtype'; HM_DB_HOST = 'dbhost'; HM_DB_PORT = 'dbport'; HM_DB_NAME = 'dbname'; HM_DB_USER = 'dbuser'
   HM_DB_PASSWORD_FILE = 'dbpasswordfile'; HM_CREATE_DATABASE = 'createdatabase'; HM_DB_SERVICE = 'dbservice'; HM_DB_PASSWORD = ''
}

$ServiceName = 'hMailServer'
$UninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1'
# hMailServer.idl's library, hMailServer.cpp's DECLARE_REGISTRY_APPID_RESOURCEID and
# InterfaceApplication.rgs: the three COM identities the registration writes.
$LibId = '{DB241B59-A1B1-4C59-98FC-8D101A2995F2}'
$AppIdGuid = '{5EDEC473-39E0-43F6-A234-1947071721C8}'
$ApplicationClsid = '{D6567EF8-0A6C-48E7-9288-A2463123C2F3}'
$StartMenuGroup = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\hMailServer'

$SmokeDomain = 'smoke.test'
$SmokeAccount = 'user@smoke.test'
$SmokeSender = 'sender@example.org'

# docs/SecureDefaults.md: the first release whose installer secures a new
# installation, and the names the code gives what it makes
# (Common/Application/NewInstallDefaults.cpp): the certificate record, the
# organisation in the certificate's subject, the two files under the data
# directory's Certificates folder, the hm_settings row the create step marks a new
# database with, and the name the verb writes on the audit trail under.
$SecureDefaultsVersion = [version] '6.3.4'
$InstallationCertificateName = 'Self-signed (made at installation)'
$InstallationCertificateOrganization = 'hMailServer, self-signed at installation'
$InstallationCertificateFile = 'self-signed-certificate.pem'
$InstallationKeyFile = 'self-signed-key.pem'
$PendingMarkName = 'NewInstallDefaultsPending'
$VerbActor = 'hmailserver --apply-new-install-defaults'
# The four listeners a new database has (CreateTables*.sql), by eSessionType and
# number - 1 SMTP, 3 POP3, 5 IMAP - and eConnectionSecurity's STARTTLS (optional).
$ShippedListeners = @(
   @{ Protocol = 1; Number = 25; Name = 'SMTP' },
   @{ Protocol = 1; Number = 587; Name = 'SMTP' },
   @{ Protocol = 3; Number = 110; Name = 'POP3' },
   @{ Protocol = 5; Number = 143; Name = 'IMAP' })
$StartTlsOptional = 2

# Letters and digits only: the account password travels in POP3's PASS and in an IMAP
# quoted string, and the administrator password on Inno's command line when the first
# installer predates 6.3.4 (Set-AdminPasswordFor), and none of them needs escaping for
# these.
function New-Secret([int] $Length) {
   $alphabet = 'abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789'
   $bytes = New-Object byte[] $Length
   $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
   $rng.GetBytes($bytes)
   $rng.Dispose()
   $chars = foreach ($b in $bytes) { $alphabet[$b % $alphabet.Length] }
   return 'Sm0ke' + (-join $chars)
}

$AdminPassword = New-Secret 20
$AccountPassword = New-Secret 16

# What the administrator password gains when the installers can carry it: a space, a
# double quote, two backslashes before one and a trailing backslash - each of which
# the C runtime's splitting of a command line changes (Set-AdminPasswordFor).
$AdminPasswordHardPart = ' q"t \\" e\'

# Everything the checks learn and later checks need: a hashtable, so that a check's
# script block can write to it from its own scope.
$State = @{
   App = $(if ($InstallDirectory) { $InstallDirectory } else { Join-Path $env:ProgramFiles 'hMailServer' })
   Installed = $false
   Seeded = $false
   SchemaBefore = 0
   Banner = ''
   Subject = ''
   MessageFile = ''
   MessageHash = ''
   IniDatabase = ''
   IniSecurity = ''
   KeptScripts = @()
   RegistryBaseline = $null
   ComAfterInstall = 0
   SchemaNow = 0
   ErrorLinesSeen = New-Object 'System.Collections.Generic.HashSet[string]'
   PasswordInEnvironment = $false
   CertificateId = 0
   CertificateThumbprint = ''
   CertificateFiles = @()
   ListenersBefore = $null
   # Whether every user could read the program folder, Bin and ControlPanel just
   # before the last installer ran (Get-ProgramFoldersReadable), by full path.
   ProgramFoldersReadable = $null
   # -DatabaseSettings: what the file gives, and the [Database] Type and create script
   # of that database (Read-DatabaseSettings). Empty for the built-in database.
   Database = $null
   DatabaseIniType = ''
   DatabaseScript = ''
   # -FolderCase: the standard account the run makes, the topmost folder setup is to
   # make (nested-new), and the files the case puts in the program folder before the
   # first installer, as paths below it (Bin\notes.txt): not the installer's, so an
   # uninstall leaves them, as Inno Setup leaves every file it did not install.
   SmokeAccountSid = ''
   TopMade = ''
   CaseFiles = @()
   # weak-above and weak-above-remedied: the folder above the program folder the case made.
   AboveMade = ''
}

New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$SummaryPath = Join-Path $LogDirectory 'smoke.log'
Set-Content -Path $SummaryPath -Value '' -Encoding ASCII
$script:Passed = 0
$script:Failed = 0

# ------------------------------------------------------------------ reporting

# Through the console stream and flushed: over SSH, Write-Host's output is held back
# until the script ends, and a run that takes ten minutes should say where it is.
function Write-Line([string] $Text) {
   [Console]::Out.WriteLine($Text)
   [Console]::Out.Flush()
   Add-Content -Path $SummaryPath -Value $Text -Encoding ASCII
}

function Write-Section([string] $Title) {
   Write-Line ''
   Write-Line "== $Title"
}

function Write-Result([string] $Status, [string] $Name, [string] $Detail) {
   $text = "$Status $Name"
   if ($Detail) { $text += " - $Detail" }
   if ($Status -eq 'FAIL') { $script:Failed++ }
   if ($Status -eq 'PASS') { $script:Passed++ }
   Write-Line $text
}

# Indented lines under a result: a log's tail, a list of leftovers.
function Write-Detail([string[]] $Lines, [int] $Limit = 40) {
   $all = @($Lines | Where-Object { $_ -ne $null })
   foreach ($line in ($all | Select-Object -First $Limit)) {
      Write-Line ('      ' + ($line -replace '[^\x20-\x7E]', '?'))
   }
   if ($all.Count -gt $Limit) { Write-Line "      ... and $($all.Count - $Limit) more" }
}

# One check. The block returns what it found, as the line's detail, or throws what is
# wrong, which fails the check with that message. Returns whether it passed.
function Test-Step([string] $Name, [scriptblock] $Body) {
   try {
      $found = @(& $Body) | Where-Object { $_ -ne $null }
      Write-Result 'PASS' $Name ((@($found) | ForEach-Object { [string] $_ }) -join ' ')
      return $true
   }
   catch {
      Write-Result 'FAIL' $Name $_.Exception.Message
      return $false
   }
}

function Write-Info([string] $Name, [string] $Detail) { Write-Result 'INFO' $Name $Detail }

# ------------------------------------------------------------------ helpers

function Open-LocalMachine([string] $View) {
   return [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView] $View)
}

function Test-RegistryKey([string] $View, [string] $Path) {
   $base = Open-LocalMachine $View
   try {
      $key = $base.OpenSubKey($Path)
      if ($null -eq $key) { return $false }
      $key.Close()
      return $true
   }
   finally { $base.Close() }
}

function Get-RegistryValue([string] $View, [string] $Path, [string] $Name) {
   $base = Open-LocalMachine $View
   try {
      $key = $base.OpenSubKey($Path)
      if ($null -eq $key) { return $null }
      try { return $key.GetValue($Name) } finally { $key.Close() }
   }
   finally { $base.Close() }
}

# Inno's uninstall entry, hMailServer_is1 (AppId=hMailServer), in whichever registry
# view a 64-bit installation put it.
function Get-UninstallEntry {
   foreach ($view in 'Registry64', 'Registry32') {
      if (Test-RegistryKey $view $UninstallKey) {
         return [pscustomobject] @{
            View            = $view
            DisplayName     = [string] (Get-RegistryValue $view $UninstallKey 'DisplayName')
            DisplayVersion  = [string] (Get-RegistryValue $view $UninstallKey 'DisplayVersion')
            InstallLocation = [string] (Get-RegistryValue $view $UninstallKey 'InstallLocation')
            UninstallString = [string] (Get-RegistryValue $view $UninstallKey 'UninstallString')
         }
      }
   }
   return $null
}

function Get-InstallDirectory {
   $entry = Get-UninstallEntry
   if ($null -ne $entry -and $entry.InstallLocation) { return $entry.InstallLocation.TrimEnd('\') }
   return $State.App
}

# A native command's output and exit code, with its stderr folded in by cmd.exe:
# PowerShell 5.1 turns a native stderr line into a terminating error under Stop.
function Invoke-Native([string] $CommandLine) {
   $output = & cmd.exe /c "$CommandLine 2>&1"
   return [pscustomobject] @{ ExitCode = $LASTEXITCODE; Output = (@($output) -join "`n") }
}

function Get-HmService {
   return Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'"
}

function Wait-Until([int] $Seconds, [scriptblock] $Condition) {
   $deadline = (Get-Date).AddSeconds($Seconds)
   do {
      if (& $Condition) { return $true }
      Start-Sleep -Seconds 2
   } while ((Get-Date) -lt $deadline)
   return [bool] (& $Condition)
}

function Read-IniSection([string] $Path, [string] $Section) {
   $values = New-Object System.Collections.Generic.List[string]
   $inside = $false
   foreach ($line in Get-Content -LiteralPath $Path) {
      if ($line -match '^\s*\[(.+)\]\s*$') { $inside = ($Matches[1] -eq $Section); continue }
      if ($inside -and $line -match '^\s*([^=;]+?)\s*=\s*(.*?)\s*$') { $values.Add("$($Matches[1])=$($Matches[2])") }
   }
   return ($values -join ';')
}

function Get-IniValue([string] $Path, [string] $Section, [string] $Key) {
   foreach ($pair in ((Read-IniSection $Path $Section) -split ';')) {
      $name, $value = $pair -split '=', 2
      if ($name -eq $Key) { return $value }
   }
   return ''
}

# A key's value in a section Read-IniSection has already read.
function Get-SectionValue([string] $SectionText, [string] $Key) {
   foreach ($pair in ($SectionText -split ';')) {
      $pairKey, $pairValue = $pair -split '=', 2
      if ($pairKey -eq $Key) { return $pairValue }
   }
   return ''
}

function Get-ServerErrorLines([string] $App) {
   $logs = Join-Path $App 'Logs'
   if (-not (Test-Path -LiteralPath $logs)) { return @() }
   $lines = foreach ($file in Get-ChildItem -LiteralPath $logs -Filter 'ERROR_*.log' -File) {
      Get-Content -LiteralPath $file.FullName | Where-Object { $_.Trim() } | ForEach-Object { "$($file.Name): $_" }
   }
   return @($lines)
}

# What the server wrote to the Application event log: WindowsEventLog.cpp sends it the
# errors ErrorManager reports (a crash, a failed backup, an impaired auto-ban, the disk
# floor, and every High or Critical error), registering the source hMailServer the
# first time. The error log file is not always where such an error lands: an error
# while the Logs folder is gone has nowhere else to go.
function Get-ServerEvents {
   $lines = @()
   try {
      $lines = @(Get-EventLog -LogName Application -Source $ServiceName -ErrorAction SilentlyContinue | Sort-Object Index | ForEach-Object {
            $first = ((([string] $_.Message) -split "`n")[0]).Trim()
            "Application log #$($_.Index), $($_.TimeGenerated.ToString('HH:mm:ss')), $($_.EntryType), event $($_.EventID): $first"
         })
   }
   catch { }
   return $lines
}

function Copy-ServerLogs([string] $App, [string] $Phase) {
   $logs = Join-Path $App 'Logs'
   if (-not (Test-Path -LiteralPath $logs)) { return }
   if (@(Get-ChildItem -LiteralPath $logs -Force).Count -eq 0) { return }
   $target = Join-Path $LogDirectory "server-logs-$Phase"
   New-Item -ItemType Directory -Force -Path $target | Out-Null
   Copy-Item -Path (Join-Path $logs '*') -Destination $target -Recurse -Force -ErrorAction SilentlyContinue
}

function Remove-ComReference([object] $Object) {
   if ($null -ne $Object) {
      try { [void] [System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($Object) } catch { }
   }
   [GC]::Collect()
   [GC]::WaitForPendingFinalizers()
}

function Connect-Server {
   $application = New-Object -ComObject hMailServer.Application
   $administrator = $application.Authenticate('Administrator', $AdminPassword)
   if ($null -eq $administrator) { throw 'Authenticate(Administrator) was refused with the password the installer was given' }
   return $application
}

# ------------------------------------------------------------------ the protocols

function Open-Conversation([int] $Port, [string] $Address = '127.0.0.1') {
   $client = New-Object System.Net.Sockets.TcpClient
   $client.ReceiveTimeout = 30000
   $client.SendTimeout = 30000
   $client.Connect($Address, $Port)
   $conversation = [pscustomobject] @{ Client = $client; Reader = $null; Writer = $null; Certificate = $null; Tls = '' }
   Set-ConversationStream $conversation $client.GetStream()
   return $conversation
}

function Set-ConversationStream($Conversation, [System.IO.Stream] $Stream) {
   $Conversation.Reader = New-Object System.IO.StreamReader($Stream, [System.Text.Encoding]::ASCII)
   $writer = New-Object System.IO.StreamWriter($Stream, [System.Text.Encoding]::ASCII)
   $writer.NewLine = "`r`n"
   $writer.AutoFlush = $true
   $Conversation.Writer = $writer
}

# TLS started on a conversation whose server has just agreed to it (STARTTLS, STLS).
# Any certificate is taken - the self-signed one a new installation makes is the one
# being checked - and kept, with the protocol, for the caller. Nothing is read ahead
# of the server's agreement, so the plain reader holds nothing the TLS one needs.
function Start-ConversationTls($Conversation) {
   $accept = [System.Net.Security.RemoteCertificateValidationCallback] { param($sender, $certificate, $chain, $errors) return $true }
   $ssl = New-Object System.Net.Security.SslStream($Conversation.Client.GetStream(), $false, $accept)
   $protocols = [System.Security.Authentication.SslProtocols]::Tls12 -bor [System.Security.Authentication.SslProtocols]::Tls13
   $ssl.AuthenticateAsClient('localhost', $null, $protocols, $false)
   $Conversation.Certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($ssl.RemoteCertificate)
   $Conversation.Tls = [string] $ssl.SslProtocol
   Set-ConversationStream $Conversation $ssl
}

# An SMTP command, or none for the greeting, and every line of the reply.
function Invoke-SmtpLines($Conversation, [string] $Command) {
   if ($Command) { $Conversation.Writer.WriteLine($Command) }
   $lines = @()
   do { $line = Read-Reply $Conversation; $lines += $line } while ($line -match '^\d{3}-')
   return , $lines
}

# The EHLO keyword lines of a reply: "250-AUTH LOGIN PLAIN" gives "AUTH LOGIN PLAIN".
function Get-EhloKeywords([string[]] $Reply) {
   return @($Reply | Select-Object -Skip 1 | ForEach-Object { $_.Substring(4) })
}

# AUTH LOGIN with the account's name and password; the reply must be 235. What an
# error says of the two answers is their names, never their base64.
function Invoke-SmtpAuthLogin($Conversation) {
   $encode = { param($text) [Convert]::ToBase64String([System.Text.Encoding]::ASCII.GetBytes($text)) }
   [void] (Invoke-SmtpCommand $Conversation 'AUTH LOGIN' '334')
   [void] (Invoke-SmtpCommand $Conversation (& $encode $SmokeAccount) '334' 'the user name')
   return (Invoke-SmtpCommand $Conversation (& $encode $AccountPassword) '235' 'the password')
}

# An IMAP command's tagged answer, whatever it is, and the untagged lines before it.
function Invoke-ImapExchange($Conversation, [string] $Tag, [string] $Command) {
   $Conversation.Writer.WriteLine("$Tag $Command")
   $untagged = @()
   while ($true) {
      $line = Read-Reply $Conversation
      if ($line.StartsWith("$Tag ")) { return [pscustomobject] @{ Tagged = $line; Untagged = $untagged } }
      $untagged += $line
   }
}

# POP3 CAPA: the capability lines between +OK and the lone dot.
function Get-Pop3Capabilities($Conversation) {
   $Conversation.Writer.WriteLine('CAPA')
   $first = Read-Reply $Conversation
   if (-not $first.StartsWith('+OK')) { throw "POP3 CAPA was answered '$first'" }
   $lines = @()
   while (($line = Read-Reply $Conversation) -ne '.') { $lines += $line }
   return , $lines
}

# A POP3 command whose reply must start with $Expected ('+OK' or '-ERR'); $Shown is
# what an error may say of the command.
function Invoke-Pop3Command($Conversation, [string] $Command, [string] $Expected, [string] $Shown = $Command) {
   $Conversation.Writer.WriteLine($Command)
   $reply = Read-Reply $Conversation
   if (-not $reply.StartsWith($Expected)) { throw "POP3 '$Shown' was answered '$reply'" }
   return $reply
}

function Read-Reply($Conversation) {
   $line = $Conversation.Reader.ReadLine()
   if ($null -eq $line) { throw 'the server closed the connection' }
   return $line
}

function Read-Banner([int] $Port) {
   $conversation = Open-Conversation $Port
   try { return Read-Reply $conversation } finally { $conversation.Client.Close() }
}

# An SMTP command and the reply code it must get; $Shown is what an error may say of
# the command.
function Invoke-SmtpCommand($Conversation, [string] $Command, [string] $Code, [string] $Shown = $Command) {
   if ($Command) { $Conversation.Writer.WriteLine($Command) }
   do { $line = Read-Reply $Conversation } while ($line -match '^\d{3}-')
   if (-not $line.StartsWith($Code)) { throw "SMTP '$Shown' was answered '$line'" }
   return $line
}

function Send-SmokeMessage([string] $Subject) {
   $conversation = Open-Conversation 25
   try {
      [void] (Invoke-SmtpCommand $conversation '' '220' 'banner')
      [void] (Invoke-SmtpCommand $conversation 'EHLO smoke.client' '250')
      [void] (Invoke-SmtpCommand $conversation "MAIL FROM:<$SmokeSender>" '250')
      [void] (Invoke-SmtpCommand $conversation "RCPT TO:<$SmokeAccount>" '250')
      [void] (Invoke-SmtpCommand $conversation 'DATA' '354')
      $date = (Get-Date).ToUniversalTime().ToString('ddd, dd MMM yyyy HH:mm:ss +0000', [System.Globalization.CultureInfo]::InvariantCulture)
      foreach ($line in @(
            "From: <$SmokeSender>",
            "To: <$SmokeAccount>",
            "Subject: $Subject",
            "Date: $date",
            "Message-ID: <$([guid]::NewGuid().ToString('N'))@smoke.client>",
            '',
            'The installer smoke test delivered this message before the upgrade, and',
            'expects to find it again after the upgrade and after the uninstall.')) {
         $conversation.Writer.WriteLine($line)
      }
      $accepted = Invoke-SmtpCommand $conversation '.' '250' 'end of data'
      [void] (Invoke-SmtpCommand $conversation 'QUIT' '221')
      return $accepted
   }
   finally { $conversation.Client.Close() }
}

# POP3 USER/PASS and STAT, and nothing that deletes: the message is counted again later.
function Get-Pop3Stat {
   $conversation = Open-Conversation 110
   try {
      $banner = Read-Reply $conversation
      if (-not $banner.StartsWith('+OK')) { throw "POP3 banner '$banner'" }
      $conversation.Writer.WriteLine("USER $SmokeAccount")
      $reply = Read-Reply $conversation
      if (-not $reply.StartsWith('+OK')) { throw "POP3 USER was answered '$reply'" }
      $conversation.Writer.WriteLine("PASS $AccountPassword")
      $reply = Read-Reply $conversation
      if (-not $reply.StartsWith('+OK')) { throw "POP3 PASS was answered '$reply'" }
      $conversation.Writer.WriteLine('STAT')
      $stat = Read-Reply $conversation
      if (-not $stat.StartsWith('+OK')) { throw "POP3 STAT was answered '$stat'" }
      $conversation.Writer.WriteLine('QUIT')
      [void] (Read-Reply $conversation)
      return $stat
   }
   finally { $conversation.Client.Close() }
}

function Invoke-ImapCommand($Conversation, [string] $Tag, [string] $Command, [string] $Shown) {
   $Conversation.Writer.WriteLine("$Tag $Command")
   $untagged = New-Object System.Collections.Generic.List[string]
   while ($true) {
      $line = Read-Reply $Conversation
      if ($line.StartsWith("$Tag ")) {
         if ($line -notmatch "^$Tag OK") { throw "IMAP '$Shown' was answered '$line'" }
         return $untagged
      }
      $untagged.Add($line)
   }
}

# IMAP LOGIN and SELECT INBOX: the number of messages the mailbox EXISTS with.
function Get-ImapExists {
   $conversation = Open-Conversation 143
   try {
      $banner = Read-Reply $conversation
      if (-not $banner.StartsWith('* OK')) { throw "IMAP banner '$banner'" }
      [void] (Invoke-ImapCommand $conversation 'a1' "LOGIN `"$SmokeAccount`" `"$AccountPassword`"" 'LOGIN')
      $selected = Invoke-ImapCommand $conversation 'a2' 'SELECT INBOX' 'SELECT INBOX'
      $exists = -1
      foreach ($line in $selected) { if ($line -match '^\* (\d+) EXISTS') { $exists = [int] $Matches[1] } }
      [void] (Invoke-ImapCommand $conversation 'a3' 'LOGOUT' 'LOGOUT')
      return $exists
   }
   finally { $conversation.Client.Close() }
}

# ------------------------------------------------------------------ what is on the machine

# The COM registrations of hMailServer.exe, in both registry views: the ProgIDs, every
# class whose LocalServer32 is hMailServer.exe, the type library, every interface the
# type library registered, and the AppID. Empty before the install and after the
# uninstall; after the install it must hold the Application class.
function Get-ComFootprint {
   $found = New-Object System.Collections.Generic.List[string]
   foreach ($view in 'Registry64', 'Registry32') {
      $base = Open-LocalMachine $view
      try {
         $classes = $base.OpenSubKey('SOFTWARE\Classes')
         foreach ($name in $classes.GetSubKeyNames()) {
            if ($name -like 'hMailServer.*') { $found.Add("$view HKCR\$name") }
         }
         foreach ($name in @("TypeLib\$LibId", "AppID\$AppIdGuid", 'AppID\hMailServer.exe')) {
            $key = $classes.OpenSubKey($name)
            if ($null -ne $key) { $found.Add("$view HKCR\$name"); $key.Close() }
         }
         $clsids = $classes.OpenSubKey('CLSID')
         if ($null -ne $clsids) {
            foreach ($name in $clsids.GetSubKeyNames()) {
               $server = $clsids.OpenSubKey("$name\LocalServer32")
               if ($null -eq $server) { continue }
               $value = [string] $server.GetValue('')
               $server.Close()
               if ($value -match 'hMailServer\.exe') { $found.Add("$view HKCR\CLSID\$name") }
            }
            $clsids.Close()
         }
         $interfaces = $classes.OpenSubKey('Interface')
         if ($null -ne $interfaces) {
            foreach ($name in $interfaces.GetSubKeyNames()) {
               $library = $interfaces.OpenSubKey("$name\TypeLib")
               if ($null -eq $library) { continue }
               $value = [string] $library.GetValue('')
               $library.Close()
               if ($value -eq $LibId) { $found.Add("$view HKCR\Interface\$name") }
            }
            $interfaces.Close()
         }
         $classes.Close()
      }
      finally { $base.Close() }
   }
   return , $found
}

# The names under the keys an installation, a service or a COM server writes into,
# for the difference between the clean machine and the uninstalled one.
$SnapshotKeys = @(
   @('LocalMachine', 'Registry64', 'SOFTWARE'),
   @('LocalMachine', 'Registry32', 'SOFTWARE'),
   @('LocalMachine', 'Registry64', 'SOFTWARE\Classes'),
   @('LocalMachine', 'Registry64', 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'),
   @('LocalMachine', 'Registry32', 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'),
   @('LocalMachine', 'Registry64', 'SYSTEM\CurrentControlSet\Services'),
   @('LocalMachine', 'Registry64', 'SYSTEM\CurrentControlSet\Services\EventLog\Application'),
   @('Users', 'Registry64', 'S-1-5-18\Software'),
   @('CurrentUser', 'Registry64', 'Software')
)

function Get-RegistrySnapshot {
   $names = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
   foreach ($spec in $SnapshotKeys) {
      $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive] $spec[0], [Microsoft.Win32.RegistryView] $spec[1])
      try {
         $key = $base.OpenSubKey($spec[2])
         if ($null -eq $key) { continue }
         foreach ($name in $key.GetSubKeyNames()) { [void] $names.Add("$($spec[0])($($spec[1]))\$($spec[2])\$name") }
         $key.Close()
      }
      finally { $base.Close() }
   }
   return , $names
}

function Get-NewRegistryKeys($Baseline) {
   $now = Get-RegistrySnapshot
   return @($now | Where-Object { -not $Baseline.Contains($_) } | Sort-Object)
}

# ------------------------------------------------------------------ the installer

# What the installer claims to be: its version resource's ProductVersion, which Inno
# fills from AppVersion (section_setup_64.iss), the same string the uninstall entry
# records as DisplayVersion.
function Get-ClaimedVersion([string] $Path) {
   $version = (Get-Item -LiteralPath $Path).VersionInfo.ProductVersion
   if (-not $version) { throw "$Path has no ProductVersion in its version resource" }
   return $version.Trim()
}

# The release a version names - 6.3.4 of "6.3.4" or "6.3.4-B43" - or $null.
function Get-ReleaseVersion([string] $Version) {
   if ($Version -match '^(\d+)\.(\d+)\.(\d+)') { return [version] "$($Matches[1]).$($Matches[2]).$($Matches[3])" }
   return $null
}

# Whether an installer of this version secures a new installation
# (docs/SecureDefaults.md), and so leaves an upgrade exactly as it was.
function Test-HasSecureDefaults([string] $Version) {
   $release = Get-ReleaseVersion $Version
   return ($null -ne $release) -and ($release -ge $SecureDefaultsVersion)
}

function Test-InstallerFile([string] $Label, [string] $Path) {
   Write-Info "$Label installer" ("$(Split-Path -Leaf $Path), {0:N0} bytes, SHA-256 {1}" -f (Get-Item -LiteralPath $Path).Length, (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash)
   $claimed = Get-ClaimedVersion $Path
   Write-Info "$Label installer claims version" $claimed

   $signature = Get-AuthenticodeSignature -LiteralPath $Path
   if ($signature.Status -eq 'NotSigned' -and $AllowUnsigned) {
      Write-Result 'SKIP' "$Label installer's Authenticode signature" 'the installer is not signed, and -AllowUnsigned accepts that'
   }
   else {
      [void] (Test-Step "$Label installer's Authenticode signature is valid" {
            if ($signature.Status -ne 'Valid') { throw "status $($signature.Status): $($signature.StatusMessage)" }
            $stamp = if ($signature.TimeStamperCertificate) { 'timestamped' } else { 'NOT timestamped' }
            "signed by $($signature.SignerCertificate.Subject), $stamp"
         })
   }
   return $claimed
}

# ------------------------------------------------------------------ the program's own folders

# docs/ServiceAccount.md, "What the other users of the computer may read": from 6.3.7
# hMailServer.exe /ProtectFolders, which the installer runs at every installation and
# upgrade, hardens the program folder, Bin and ControlPanel. The groups of every user,
# as SIDs (ServiceAccount.cpp's BroadSids_ that a folder names in practice), and the
# rights that change a file or a folder, what it is called or who may change it
# (AdminUpdateChecks::WriteRights) with GENERIC_ALL and GENERIC_WRITE, which an entry
# may carry unmapped.
$HardenedFoldersVersion = [version] '6.3.7'
$BroadGroupSids = @('S-1-1-0', 'S-1-2-0', 'S-1-5-4', 'S-1-5-6', 'S-1-5-11', 'S-1-5-13', 'S-1-5-14', 'S-1-5-32-545', 'S-1-5-32-546', 'S-1-5-32-547', 'S-1-5-32-555')
$WriteRightsMask = 0x500D0156
$TrustedOwnerSids = @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
$ServiceSid = 'S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809'
# The account this script runs as, which runs the installers: an administrator, whose
# own SID is enabled even in what it runs without elevation.
$InstallingAccountSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value

function Test-HasHardenedFolders([string] $Version) {
   $release = Get-ReleaseVersion $Version
   return ($null -ne $release) -and ($release -ge $HardenedFoldersVersion)
}

# The groups whose Read on a program folder /ProtectFolders asks for before it gives
# BUILTIN\Users Read & execute there (ServiceAccount.cpp's EveryUserReads_, the
# installer's EveryUserMayRead): Everyone, Authenticated Users and Users; and the
# rights that read - listing a folder (FILE_READ_DATA), or GENERIC_ALL or GENERIC_READ
# unmapped. 0x90000001 is negative to PowerShell, as GENERIC_READ is in an access
# rule, so the two meet as one sign-extended long.
$EveryUserSids = @('S-1-1-0', 'S-1-5-11', 'S-1-5-32-545')
$ReadRightsMask = 0x90000001

# The allow entries for those groups that let them read a folder, as "SID rights": on
# a folder that is there, the entries that apply to it; on one that is not, the
# entries a folder made there would inherit from the nearest folder above it that is
# there - handed down to folders, and, more than one level down, not only to the
# first.
function Get-EveryUserReaders([string] $Path) {
   $levels = 0
   $folder = $Path
   while ($folder -and -not (Test-Path -LiteralPath $folder)) { $folder = Split-Path -Parent $folder; $levels++ }
   if (-not $folder) { return @() }
   $found = @()
   foreach ($rule in (Get-Acl -LiteralPath $folder).GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
      if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) { continue }
      if ($EveryUserSids -notcontains $rule.IdentityReference.Value) { continue }
      if ((([long] $rule.FileSystemRights) -band [long] $ReadRightsMask) -eq 0) { continue }
      if ($levels -eq 0) {
         if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
      }
      else {
         if (($rule.InheritanceFlags -band [System.Security.AccessControl.InheritanceFlags]::ContainerInherit) -eq 0) { continue }
         if ($levels -gt 1 -and ($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::NoPropagateInherit) -ne 0) { continue }
      }
      $found += "$($rule.IdentityReference.Value) $($rule.FileSystemRights)"
   }
   return $found
}

# Whether every user could read each of the program folder, Bin and ControlPanel just
# before an installer runs, by full path: what /ProtectFolders gives BUILTIN\Users
# Read & execute on after it, and what it gives no Users entry on. A Bin or
# ControlPanel that is not there yet is made in a program folder the installer gives
# Users Read & execute when, and only when, every user could read the program folder
# (PrepareProgramFolders), and so goes with it.
function Get-ProgramFoldersReadable([string] $App) {
   $readable = @{}
   $appReadable = (@(Get-EveryUserReaders $App).Count -gt 0)
   $readable[$App] = $appReadable
   foreach ($name in 'Bin', 'ControlPanel') {
      $folder = Join-Path $App $name
      $readable[$folder] = $(if (Test-Path -LiteralPath $folder) { @(Get-EveryUserReaders $folder).Count -gt 0 } else { $appReadable })
   }
   return $readable
}

# The allow entries of a file or folder that apply to it and give a group of every
# user - or, given Sids, one of those - a right to change it, as "SID rights".
function Get-BroadWriters([string] $Path, [string[]] $Sids = $BroadGroupSids) {
   $acl = Get-Acl -LiteralPath $Path
   $found = @()
   foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
      if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) { continue }
      if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
      if ($Sids -notcontains $rule.IdentityReference.Value) { continue }
      if (([long] $rule.FileSystemRights -band $WriteRightsMask) -ne 0) { $found += "$($rule.IdentityReference.Value) $($rule.FileSystemRights)" }
   }
   return $found
}

# The allow entries of a folder above the program folder that apply to it and give a
# group of every user a right the installer refuses there (HM_REPLACE_RIGHTS,
# FolderAboveIsAdministratorsAlone; AdminUpdateChecks.cpp's ReplaceRights_): renaming or
# removing it, removing what is in it, changing who may, or GENERIC_ALL unmapped - not
# adding to it. A folder given the refusal's remedy (icacls /inheritance:d, then
# /remove:g *S-1-5-11) keeps the drive root's BUILTIN\Users (CI)(AD) and (CI)(IO)(WD) as
# entries of its own, which let Users make a folder or a file in it and nothing more,
# and which the installer, rightly, does not count (smoke run 4, 27 October 2026: the
# case's own check counted every write right and refused the remedy the installer takes).
$ReplaceRightsMask = 0x100D0040
function Get-BroadReplacers([string] $Path) {
   $acl = Get-Acl -LiteralPath $Path
   $found = @()
   foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
      if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) { continue }
      if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
      if ($BroadGroupSids -notcontains $rule.IdentityReference.Value) { continue }
      if (([long] $rule.FileSystemRights -band $ReplaceRightsMask) -ne 0) { $found += "$($rule.IdentityReference.Value) $($rule.FileSystemRights)" }
   }
   return $found
}

# Whether sid has an allow entry on the folder that gives every one of rights and
# reaches both the folders and the files below it.
function Test-FolderGrant($Acl, [string] $Sid, [long] $Rights) {
   foreach ($rule in $Acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
      if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow -or $rule.IdentityReference.Value -ne $Sid) { continue }
      $both = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
      if (($rule.InheritanceFlags -band $both) -ne $both) { continue }
      if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
      if ((([long] $rule.FileSystemRights) -band $Rights) -eq $Rights) { return $true }
   }
   return $false
}

function Test-ProgramFoldersHardened([string] $Phase, [string] $Claimed, [string] $LogPath) {
   $app = Get-InstallDirectory
   $folders = @(@($app, (Join-Path $app 'Bin'), (Join-Path $app 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ })

   # For the record, whatever the installer: the owner and what icacls shows of the
   # program folder, Bin, hMailServer.exe, ControlPanel and the REST API's key store.
   foreach ($object in @($app, (Join-Path $app 'Bin'), (Join-Path $app 'Bin\hMailServer.exe'), (Join-Path $app 'ControlPanel'), (Join-Path $app 'Bin\hMailServerApiKeys.ini'))) {
      if (-not (Test-Path -LiteralPath $object)) { Write-Info "${Phase}: icacls" "$object is not there"; continue }
      $lines = @(& icacls.exe $object 2>&1 | ForEach-Object { "$_".Trim() } | Where-Object { $_ -and $_ -notmatch '^Successfully processed' })
      Write-Info "${Phase}: icacls (owner $((Get-Acl -LiteralPath $object).Owner))" ($lines -join ' | ')
   }
   # And of everything in the program folder, into the logs: a line of icacls' that
   # goes to its error output is a line of the file, not the end of the run.
   if (Test-Path -LiteralPath $app) {
      $dump = Join-Path $LogDirectory "icacls-$Phase.txt"
      $ErrorActionPreference = 'Continue'
      $tree = @(& icacls.exe $app /T /C /Q 2>&1 | ForEach-Object { "$_" })
      $treeExit = $LASTEXITCODE
      $ErrorActionPreference = 'Stop'
      Set-Content -LiteralPath $dump -Value $tree -Encoding UTF8
      Write-Info "${Phase}: icacls of everything in the program folder" "$dump, $($tree.Count) lines, icacls returned $treeExit"
   }

   # By its version, or - for a build not yet stamped with one - by what its
   # /ProtectFolders said in Inno's log.
   $hardens = (Test-HasHardenedFolders $Claimed) -or
              ((Test-Path -LiteralPath $LogPath) -and (Select-String -LiteralPath $LogPath -Pattern 'Each of these is hardened' -Quiet))
   if (-not $hardens) {
      $writers = @()
      foreach ($folder in $folders) { foreach ($writer in @(Get-BroadWriters $folder)) { $writers += "$folder ($writer)" } }
      Write-Info "${Phase}: the program's own folders" $(if ($writers.Count -gt 0) { "$Claimed does not harden them, and a group of every user may change: $($writers -join '; ')" } else { "$Claimed does not harden them; no group of every user may change them here" })
      return
   }

   # BUILTIN\Users' Read & execute where every user could read the folder before the
   # installer ran (Get-ProgramFoldersReadable, read then), and no entry that lets
   # every user read it where none could: ServiceAccount.cpp's RebuiltAcl_ asks
   # EveryUserReads_, so that a folder only the administrators could open does not
   # become every user's to read.
   [void] (Test-Step "${Phase}: the program folder, Bin and ControlPanel are hardened - nothing inherited, SYSTEM and the Administrators full control, Users Read & execute where every user could read them before, a trusted owner" {
         if ($folders.Count -ne 3) { throw "only $($folders -join ', ') are there" }
         $said = @()
         foreach ($folder in $folders) {
            $acl = Get-Acl -LiteralPath $folder
            if (-not $acl.AreAccessRulesProtected) { throw "$folder still inherits from the folder above it" }
            foreach ($full in 'S-1-5-18', 'S-1-5-32-544') {
               if (-not (Test-FolderGrant $acl $full 0x1F01FF)) { throw "$folder does not give $full full control handed down" }
            }
            if ($null -eq $State.ProgramFoldersReadable -or -not $State.ProgramFoldersReadable.ContainsKey($folder)) { throw "whether every user could read $folder before the installer was not read" }
            if ($State.ProgramFoldersReadable[$folder]) {
               if (-not (Test-FolderGrant $acl 'S-1-5-32-545' 0x1200A9)) { throw "$folder does not give BUILTIN\Users Read & execute handed down, though every user could read it before" }
               $said += "$(Split-Path -Leaf $folder) Users RX"
            }
            else {
               $readers = @(Get-EveryUserReaders $folder)
               if ($readers.Count -gt 0) { throw "$folder lets every user read it ($($readers -join '; ')), which none could before" }
               $said += "$(Split-Path -Leaf $folder) no Users entry (none could read it before)"
            }
            $owner = $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value
            if ($TrustedOwnerSids -notcontains $owner) { throw "$folder is owned by $owner" }
         }
         "$($folders -join ', '): $($said -join ', ')"
      })

   # Every folder and file in the program folder, not Bin and ControlPanel alone: the
   # programs and libraries in Addons, the scripts an administrator runs from Addons
   # and the service from Events, and what the data folders hold all inherit from the
   # program folder or were made private, and none may be every user's to change. And
   # the account that ran the installer - an administrator, whose own SID, unlike the
   # Administrators', is enabled in what it runs without elevation - with no entry of
   # its own that lets it change, and not the owner of, the program folder or a folder,
   # program or library in it outside the four folders the service writes, where the
   # installation's self-signed key gives it an entry on purpose. A file the service
   # removes while the check runs (Temp) is passed over.
   [void] (Test-Step "${Phase}: no group of every user may change the program folder or a folder or file in it, nor the account that ran the installer a folder, program or library in it" {
         $installerSid = $InstallingAccountSid
         $serviceFolders = @('Data', 'Logs', 'Temp', 'Database') | ForEach-Object { (Join-Path $app $_) + '\' }
         $listed = @()
         $listing = @()
         $listed += @(Get-ChildItem -LiteralPath $app -Recurse -Force -ErrorAction SilentlyContinue -ErrorVariable listing)
         if ($listing.Count -gt 0) { Write-Detail @($listing | ForEach-Object { "$($_.Exception.Message)" }) 10; throw "$($listing.Count) folders could not be listed (above)" }
         $objects = @(@($folders) + @($listed | ForEach-Object { $_.FullName }) | Sort-Object -Unique)
         $checked = 0
         $mine = 0
         $writers = @()
         foreach ($object in $objects) {
            $item = Get-Item -LiteralPath $object -Force -ErrorAction SilentlyContinue
            if ($null -eq $item) { continue }
            try {
               foreach ($writer in @(Get-BroadWriters $object)) { $writers += "$object ($writer)" }
               $outsideService = -not @($serviceFolders | Where-Object { ($object + '\').StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }).Count
               if ($outsideService -and ($item.PSIsContainer -or $item.Extension -eq '.exe' -or $item.Extension -eq '.dll')) {
                  foreach ($writer in @(Get-BroadWriters $object @($installerSid))) { $writers += "$object (the account that ran the installer: $writer)" }
                  $owner = (Get-Acl -LiteralPath $object).GetOwner([System.Security.Principal.SecurityIdentifier]).Value
                  if ($owner -eq $installerSid) { $writers += "$object (owned by the account that ran the installer, $owner)" }
                  $mine++
               }
            }
            catch {
               if (Test-Path -LiteralPath $object) { throw }
               continue
            }
            $checked++
         }
         if ($writers.Count -gt 0) { Write-Detail $writers 20; throw "$($writers.Count) entries (above)" }
         "$checked folders and files, $mine of them folders, programs and libraries outside $(($serviceFolders | ForEach-Object { Split-Path -Leaf $_.TrimEnd('\') }) -join ', ') also for $installerSid"
      })

   # The one file in Bin the service writes, whatever the folder's hardening.
   $service = Get-HmService
   if ($null -ne $service -and $service.StartName -eq 'NT SERVICE\hMailServer') {
      [void] (Test-Step "${Phase}: the REST API's key store is still the service's to write" {
            $keys = Join-Path $app 'Bin\hMailServerApiKeys.ini'
            if (-not (Test-Path -LiteralPath $keys)) { throw "$keys is not there" }
            $acl = Get-Acl -LiteralPath $keys
            $modify = $false
            foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
               if ($rule.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow -and $rule.IdentityReference.Value -eq $ServiceSid -and
                   (([long] $rule.FileSystemRights) -band 0x1301BF) -eq 0x1301BF) { $modify = $true }
            }
            if (-not $modify) { throw 'NT SERVICE\hMailServer may not write it' }
            'NT SERVICE\hMailServer Modify'
         })
   }

   [void] (Test-Step "${Phase}: hMailServer.exe /ProtectFolders returned 0 (Inno's log)" {
         $line = Select-String -LiteralPath $LogPath -Pattern 'hMailServer\.exe /ProtectFolders returned (-?\d+)' | Select-Object -Last 1
         if ($null -eq $line) { throw "Inno's log has no 'hMailServer.exe /ProtectFolders returned'" }
         if ($line.Matches[0].Groups[1].Value -ne '0') { throw "it returned $($line.Matches[0].Groups[1].Value)" }
         'returned 0'
      })

   [void] (Test-Step "${Phase}: a second hMailServer.exe /ProtectFolders finds the three hardened already and returns 0" {
         $said = Join-Path $LogDirectory "protect-folders-$Phase.txt"
         $saidErrors = Join-Path $LogDirectory "protect-folders-$Phase.errors.txt"
         $process = Start-Process -FilePath (Join-Path $app 'Bin\hMailServer.exe') -ArgumentList '/ProtectFolders' -RedirectStandardOutput $said -RedirectStandardError $saidErrors -NoNewWindow -PassThru
         $null = $process.Handle
         if (-not $process.WaitForExit(600000)) { try { $process.Kill() } catch { }; throw 'it did not finish within ten minutes' }
         $process.WaitForExit()
         $lines = @(Get-Content -LiteralPath $said) + @(Get-Content -LiteralPath $saidErrors)
         $already = @($lines | Where-Object { $_ -match '^\s*already hardened ' }).Count
         if ($process.ExitCode -ne 0) { Write-Detail $lines 30; throw "exit code $($process.ExitCode)" }
         if ($already -ne 3) { Write-Detail $lines 30; throw "$already lines say 'already hardened', not 3" }
         $checks = @($lines | Where-Object { $_ -match '^\s*CHECK ' })
         if ($checks.Count -gt 0) { Write-Detail $checks 10 }
         "exit code 0, 3 already hardened, $($checks.Count) CHECK lines"
      })
}

# Runs Inno's setup or uninstaller, bounded in time: the 6.2.19 installer once hung for
# ninety minutes in CI and said nothing about where. The log names the last step it
# reached, so its tail is shown whenever the run did not end in 0.
function Invoke-Setup([string] $Path, [string[]] $Arguments, [string] $LogPath) {
   $shown = ($Arguments | ForEach-Object { if ($_ -like '/adminpassword=*') { '/adminpassword=(hidden)' } else { $_ } }) -join ' '
   Write-Line "   $(Split-Path -Leaf $Path) $shown"
   $started = Get-Date
   $process = Start-Process -FilePath $Path -ArgumentList $Arguments -PassThru
   # Reading Handle keeps the process object able to report ExitCode after the exit.
   $null = $process.Handle
   if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
      try { $process.Kill() } catch { }
      Write-LogTail $LogPath
      throw "did not finish within $TimeoutMinutes minutes and was stopped"
   }
   $seconds = [int] ((Get-Date) - $started).TotalSeconds
   if ($process.ExitCode -ne 0) {
      Write-LogTail $LogPath
      throw "exit code $($process.ExitCode) after $seconds s"
   }
   return "exit code 0 after $seconds s"
}

# The administrator password, chosen once the first installer's version is known. From
# 6.3.4 the installer takes it in HM_ADMIN_PASSWORD and hands it to DBSetupQuick and
# DBUpdater there, so it gains a space, a double quote and backslashes before one, and
# goes in the environment: Inno drops every double quote from its own command line,
# so /adminpassword= cannot carry it. Before 6.3.4 the installer appended it unquoted
# to DBSetupQuick's command line, and DBSetupQuick to DBUpdater's, so it stays letters
# and digits and goes with /adminpassword=. The upgrade takes it the same way, being
# the later installer.
function Set-AdminPasswordFor([string] $FirstVersion) {
   $release = Get-ReleaseVersion $FirstVersion
   $takesAny = ($null -ne $release) -and ($release -ge [version] '6.3.4')
   if ($takesAny) {
      $script:AdminPassword = $script:AdminPassword + $AdminPasswordHardPart
      $State.PasswordInEnvironment = $true
      return 'letters and digits, a space, a double quote and backslashes before one, in HM_ADMIN_PASSWORD'
   }
   $State.PasswordInEnvironment = $false
   return "letters and digits, with /adminpassword=: the first installer, $FirstVersion, predates 6.3.4, which is the first to hand any password to the database tools unchanged"
}

# Runs a script block with the administrator password in HM_ADMIN_PASSWORD when it
# goes there, and removes the variable again, whatever the block did. Removed with
# Remove-Item: PowerShell 7 hands a .NET string parameter "" for $null, which would
# leave the variable set to the empty string.
function Invoke-WithAdminPassword([scriptblock] $Block) {
   if (-not $State.PasswordInEnvironment) { return & $Block }
   [Environment]::SetEnvironmentVariable('HM_ADMIN_PASSWORD', $AdminPassword)
   try { return & $Block }
   finally { Remove-Item -LiteralPath Env:HM_ADMIN_PASSWORD -ErrorAction SilentlyContinue }
}

# An install or an upgrade, given the administrator password the way the installer
# takes it (Set-AdminPasswordFor).
function Invoke-SetupWithAdminPassword([string] $Path, [string[]] $Arguments, [string] $LogPath) {
   if (-not $State.PasswordInEnvironment) { $Arguments = $Arguments + "/adminpassword=$AdminPassword" }
   return Invoke-WithAdminPassword { Invoke-Setup $Path $Arguments $LogPath }
}

# -DatabaseSettings: the file's NAME=value lines, only the names DBSetup reads, and the
# [Database] Type the server writes for that database and the create script it runs.
# A line is never repeated in a message: one of them is the password.
function Read-DatabaseSettings([string] $Path) {
   $settings = [ordered]@{}
   $number = 0
   foreach ($line in Get-Content -LiteralPath $Path) {
      $number++
      $text = $line.Trim()
      if ($text -eq '' -or $text.StartsWith('#')) { continue }
      $at = $text.IndexOf('=')
      if ($at -le 0) { throw "line $number of $Path is not NAME=value" }
      $name = $text.Substring(0, $at).Trim().ToUpperInvariant()
      if (-not $DatabaseVariables.Contains($name)) { throw "line $number of $Path names $name, which is none of $($DatabaseVariables.Keys -join ', ')" }
      $value = $line.Substring($line.IndexOf('=') + 1)
      if ($name -ne 'HM_DB_PASSWORD') { $value = $value.Trim() }
      $settings[$name] = $value
   }
   if (-not $settings.Contains('HM_DB_TYPE')) { throw "$Path gives no HM_DB_TYPE" }
   switch ($settings['HM_DB_TYPE'].ToLowerInvariant()) {
      { $_ -in @('postgresql', 'postgres', 'pgsql') } { $State.DatabaseIniType = 'PostgreSQL'; $State.DatabaseScript = 'CreateTablesPGSQL.sql' }
      { $_ -in @('mysql', 'mariadb') } { $State.DatabaseIniType = 'MYSQL'; $State.DatabaseScript = 'CreateTablesMySQL.sql' }
      'mssql' { $State.DatabaseIniType = 'MSSQL'; $State.DatabaseScript = 'CreateTablesMSSQL.sql' }
      default { throw "HM_DB_TYPE in $Path is '$($settings['HM_DB_TYPE'])', none of PostgreSQL, MySQL, MariaDB or MSSQL" }
   }
   $State.Database = $settings
}

# The installer's switches for the database, the password's excepted: none with
# -DatabaseInEnvironment. Quoted when a value holds a space, as Inno reads a command line.
function Get-DatabaseSwitches {
   $switches = @()
   if (($null -eq $State.Database) -or $DatabaseInEnvironment) { return $switches }
   foreach ($name in $State.Database.Keys) {
      $switch = $DatabaseVariables[$name]
      if (-not $switch) { continue }
      $argument = "/$switch=$($State.Database[$name])"
      if ($argument.Contains(' ')) { $argument = '"' + $argument + '"' }
      $switches += $argument
   }
   return $switches
}

# Runs a script block with the database's variables in this process's environment -
# every one with -DatabaseInEnvironment, else HM_DB_PASSWORD alone, which has no
# switch - and removes them again, whatever the block did.
function Invoke-WithDatabaseVariables([scriptblock] $Block) {
   if ($null -eq $State.Database) { return & $Block }
   $set = @()
   foreach ($name in $State.Database.Keys) {
      if ($DatabaseInEnvironment -or -not $DatabaseVariables[$name]) {
         [Environment]::SetEnvironmentVariable($name, $State.Database[$name])
         $set += $name
      }
   }
   try { return & $Block }
   finally { foreach ($name in $set) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue } }
}

function Write-LogTail([string] $LogPath) {
   if (Test-Path -LiteralPath $LogPath) {
      Write-Line "      ----- the last 80 lines of $(Split-Path -Leaf $LogPath) -----"
      Write-Detail (Get-Content -LiteralPath $LogPath -Tail 80) 80
   }
   else {
      Write-Line "      no log was written at $LogPath"
   }
}

# When the install fails, the database step says why only on its own stderr: the
# installer's message is "exit code 1". The files are on disk by then, so the tool is
# simply run again - as the GitHub workflow's "Why did the database setup fail" did.
function Write-DatabaseDiagnosis([string] $App) {
   # An external database: DBSetup /unattended's own lines are in Inno's log, which
   # the failed step printed the tail of; running it again would try again to create.
   if ($null -ne $State.Database) {
      Write-Line "      The database step was DBSetup.exe /unattended: its lines are in Inno's log above, after 'Setting up the $($State.DatabaseIniType) database'."
      return
   }
   $tool = Join-Path $App 'Bin\DBSetupQuick.exe'
   if (-not (Test-Path -LiteralPath $tool)) {
      Write-Line '      DBSetupQuick.exe is not installed: the failure was earlier than the database step.'
      return
   }
   $out = Join-Path $LogDirectory 'dbsetup-stdout.txt'
   $err = Join-Path $LogDirectory 'dbsetup-stderr.txt'
   # The password as the installer gave it: in HM_ADMIN_PASSWORD, or while it is
   # letters and digits as password:VALUE, which every DBSetupQuick takes.
   $arguments = @('/silent')
   if (-not $State.PasswordInEnvironment) { $arguments += "password:$AdminPassword" }
   $process = Invoke-WithAdminPassword { Start-Process -FilePath $tool -ArgumentList $arguments -PassThru -RedirectStandardOutput $out -RedirectStandardError $err }
   $null = $process.Handle
   if (-not $process.WaitForExit(3 * 60 * 1000)) {
      try { $process.Kill() } catch { }
      Write-Line '      DBSetupQuick did not finish within three minutes.'
   }
   else {
      Write-Line "      DBSetupQuick run again: exit code $($process.ExitCode)"
   }
   foreach ($file in @($out, $err)) {
      if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).Length -gt 0) {
         Write-Line "      ----- $(Split-Path -Leaf $file) -----"
         Write-Detail (Get-Content -LiteralPath $file) 60
      }
   }
   $ini = Join-Path $App 'Bin\hMailServer.INI'
   if (Test-Path -LiteralPath $ini) {
      Write-Line "      [Database] as the installer left it: $((Read-IniSection $ini 'Database') -replace 'Password=[^;]*', 'Password=(hidden)')"
   }
}

# ------------------------------------------------------------------ the checks of an installed server

function Test-Installed([string] $Phase, [string] $Claimed) {
   $app = Get-InstallDirectory
   $State.App = $app
   $exe = Join-Path $app 'Bin\hMailServer.exe'
   $ini = Join-Path $app 'Bin\hMailServer.INI'
   Write-Info "${Phase}: installation directory" $app

   [void] (Test-Step "${Phase}: the uninstall entry records version $Claimed" {
         $entry = Get-UninstallEntry
         if ($null -eq $entry) { throw "there is no uninstall entry $UninstallKey" }
         if ($entry.DisplayVersion -ne $Claimed) { throw "DisplayVersion is '$($entry.DisplayVersion)'" }
         "DisplayVersion $($entry.DisplayVersion), '$($entry.DisplayName)', $($entry.View) view"
      })

   [void] (Test-Step "${Phase}: hMailServer.exe is version $Claimed" {
         if (-not (Test-Path -LiteralPath $exe)) { throw "$exe is not there" }
         $file = (Get-Item -LiteralPath $exe).VersionInfo
         $numeric = '{0}.{1}.{2}.{3}' -f $file.FileMajorPart, $file.FileMinorPart, $file.FileBuildPart, $file.FilePrivatePart
         if (-not $numeric.StartsWith("$Claimed.")) { throw "its file version is $numeric" }
         "file version $numeric"
      })

   [void] (Test-Step "${Phase}: the service is registered on the installed hMailServer.exe" {
         $service = Get-HmService
         if ($null -eq $service) { throw 'there is no hMailServer service' }
         if ($service.PathName.IndexOf($exe, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { throw "its image path is $($service.PathName)" }
         "image path $($service.PathName), logs on as $($service.StartName)"
      })

   [void] (Test-Step "${Phase}: the service is set to start automatically" {
         $service = Get-HmService
         if ($null -eq $service) { throw 'there is no hMailServer service' }
         if ($service.StartMode -ne 'Auto') { throw "its start mode is $($service.StartMode)" }
         "start mode $($service.StartMode)"
      })

   # ConfigureServiceRecovery in hMailServerInnoExtension.iss: sc failure, three restarts.
   [void] (Test-Step "${Phase}: the service restarts itself after a failure" {
         $answer = Invoke-Native "sc.exe qfailure $ServiceName"
         $restarts = @([regex]::Matches($answer.Output, 'RESTART')).Count
         if ($restarts -lt 1) { throw "sc qfailure shows no restart action: $($answer.Output -replace '\s+', ' ')" }
         "$restarts restart actions"
      })

   # The installer starts the service itself (RunPostInstallTasks); nothing here starts it.
   [void] (Test-Step "${Phase}: the service is running" {
         $running = Wait-Until 90 { $s = Get-HmService; $null -ne $s -and $s.State -eq 'Running' }
         if (-not $running) { $s = Get-HmService; throw "its state is $(if ($s) { $s.State } else { 'unknown: no service' }) after 90 s" }
         'state Running'
      })

   foreach ($port in @(
         @{ Number = 25; Protocol = 'SMTP'; Pattern = '^220 ' },
         @{ Number = 110; Protocol = 'POP3'; Pattern = '^\+OK' },
         @{ Number = 143; Protocol = 'IMAP'; Pattern = '^\* OK' })) {
      [void] (Test-Step "${Phase}: $($port.Protocol) answers its banner on port $($port.Number)" {
            $listening = Wait-Until 90 { $null -ne (Get-NetTCPConnection -LocalPort $port.Number -State Listen -ErrorAction SilentlyContinue) }
            if (-not $listening) { throw "nothing listens on $($port.Number) after 90 s" }
            $banner = Read-Banner $port.Number
            if ($banner -notmatch $port.Pattern) { throw "the banner is '$banner'" }
            "'$banner'"
         })
   }

   [void] (Test-Step "${Phase}: the COM object hMailServer.Application is creatable and says version $Claimed" {
         $application = New-Object -ComObject hMailServer.Application
         try {
            $version = [string] $application.Version
            if (-not $version.StartsWith("$Claimed-B")) { throw "it says version '$version'" }
            "Version $version, $($application.VersionArchitecture)"
         }
         finally { Remove-ComReference $application }
      })

   [void] (Test-Step "${Phase}: the administrator password given to the installer authenticates" {
         $application = Connect-Server
         Remove-ComReference $application
         'Authenticate(Administrator) accepted'
      })

   # -DatabaseSettings: the external database DBSetup /unattended set up, in place of
   # the built-in one - named in [Database], at the build's schema over COM, and at
   # the schema the shipped create script for it makes.
   if ($null -ne $State.Database) {
      [void] (Test-Step "${Phase}: the $($State.DatabaseIniType) database given is there, at the schema this build requires" {
            if (-not (Test-Path -LiteralPath $ini)) { throw "$ini is not there" }
            $type = Get-IniValue $ini 'Database' 'Type'
            if ($type -ne $State.DatabaseIniType) { throw "hMailServer.INI says database type '$type', not $($State.DatabaseIniType)" }
            $server = Get-IniValue $ini 'Database' 'Server'
            if ($server -ne $State.Database['HM_DB_HOST']) { throw "hMailServer.INI names the server '$server', not the one given" }
            $name = if ($State.Database.Contains('HM_DB_NAME') -and $State.Database['HM_DB_NAME']) { $State.Database['HM_DB_NAME'] } else { 'hMailServer' }
            if ((Get-IniValue $ini 'Database' 'Database') -ne $name) { throw "hMailServer.INI names the database '$(Get-IniValue $ini 'Database' 'Database')', not $name" }
            if (@(Get-ChildItem -LiteralPath (Join-Path $app 'Database') -Filter '*.sdf' -File -ErrorAction SilentlyContinue).Count -gt 0) { throw 'a built-in database (.sdf) was made as well' }
            $script = Join-Path $app "DBScripts\$($State.DatabaseScript)"
            $shipped = 0
            if (Test-Path -LiteralPath $script) {
               $insert = Select-String -LiteralPath $script -Pattern 'insert into hm_dbversion values \((\d+)\)' | Select-Object -First 1
               if ($insert) { $shipped = [int] $insert.Matches[0].Groups[1].Value }
            }
            $application = Connect-Server
            try {
               $current = [int] $application.Database.CurrentVersion
               $required = [int] $application.Database.RequiredVersion
               $needsUpgrade = [bool] $application.Database.RequiresUpgrade
            }
            finally { Remove-ComReference $application }
            if ($current -ne $required) { throw "the database is at $current and the server requires $required" }
            if ($needsUpgrade) { throw 'the server says the database requires an upgrade' }
            if ($shipped -and $shipped -ne $current) { throw "the database is at $current and the installed $($State.DatabaseScript) makes $shipped" }
            $State.SchemaNow = $current
            "type $type, database $name on the server given, schema $current = required $required = the shipped create script's $shipped"
         })
   }
   else {
   [void] (Test-Step "${Phase}: the built-in database is there, at the schema this build requires" {
         if (-not (Test-Path -LiteralPath $ini)) { throw "$ini is not there" }
         $type = Get-IniValue $ini 'Database' 'Type'
         if ($type -ne 'MSSQLCE') { throw "hMailServer.INI says database type '$type'" }
         $files = @(Get-ChildItem -LiteralPath (Join-Path $app 'Database') -Filter '*.sdf' -File -ErrorAction SilentlyContinue)
         if ($files.Count -eq 0) { throw 'there is no .sdf in the Database folder' }
         $script = Join-Path $app 'DBScripts\CreateTablesMSSQL.sql'
         $shipped = 0
         if (Test-Path -LiteralPath $script) {
            $insert = Select-String -LiteralPath $script -Pattern 'insert into hm_dbversion values \((\d+)\)' | Select-Object -First 1
            if ($insert) { $shipped = [int] $insert.Matches[0].Groups[1].Value }
         }
         $application = Connect-Server
         try {
            $database = $application.Database
            $current = [int] $database.CurrentVersion
            $required = [int] $database.RequiredVersion
            $needsUpgrade = [bool] $database.RequiresUpgrade
         }
         finally { Remove-ComReference $application }
         if ($current -ne $required) { throw "the database is at $current and the server requires $required" }
         if ($needsUpgrade) { throw 'the server says the database requires an upgrade' }
         if ($shipped -and $shipped -ne $current) { throw "the database is at $current and the installed create script makes $shipped" }
         $State.SchemaNow = $current
         "type MSSQLCE, $($files[0].Name) ($([int] ($files[0].Length / 1KB)) KB), schema $current = required $required = the shipped create script's $shipped"
      })
   }

   # section_registry.iss: InstallLocation in the 32-bit view, which the 64-bit server reads.
   [void] (Test-Step "${Phase}: Software\hMailServer InstallLocation names the installation" {
         $value = [string] (Get-RegistryValue 'Registry32' 'SOFTWARE\hMailServer' 'InstallLocation')
         if (-not $value) { throw 'the value is not there (32-bit view)' }
         if ($value.TrimEnd('\') -ne $app) { throw "it says '$value'" }
         "'$value' (32-bit view)"
      })

   [void] (Test-Step "${Phase}: the COM server is registered on the installed hMailServer.exe" {
         $progId = [string] (Get-RegistryValue 'Registry64' 'SOFTWARE\Classes\hMailServer.Application\CLSID' '')
         if ($progId -ne $ApplicationClsid) { throw "the ProgID hMailServer.Application names class '$progId'" }
         $server = [string] (Get-RegistryValue 'Registry64' "SOFTWARE\Classes\CLSID\$ApplicationClsid\LocalServer32" '')
         if ($server.IndexOf($exe, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { throw "the class's LocalServer32 is '$server'" }
         if (-not (Test-RegistryKey 'Registry64' "SOFTWARE\Classes\TypeLib\$LibId")) { throw 'the type library is not registered' }
         $service = [string] (Get-RegistryValue 'Registry64' "SOFTWARE\Classes\AppID\$AppIdGuid" 'LocalService')
         if ($service -ne $ServiceName) { throw "the AppID's LocalService is '$service'" }
         $footprint = Get-ComFootprint
         $State.ComAfterInstall = $footprint.Count
         "LocalServer32 $server, type library and AppID registered; $($footprint.Count) COM registry keys in all"
      })

   [void] (Test-Step "${Phase}: the Start menu group is there" {
         if (-not (Test-Path -LiteralPath $StartMenuGroup)) { throw "$StartMenuGroup is not there" }
         "$(@(Get-ChildItem -LiteralPath $StartMenuGroup -Recurse -Filter '*.lnk').Count) shortcuts"
      })

   [void] (Test-Step "${Phase}: the prerequisites the installer carries are installed" {
         $desktop = @(Get-ChildItem -Path (Join-Path $env:ProgramFiles 'dotnet\shared\Microsoft.WindowsDesktop.App') -Directory -Filter '10.*' -ErrorAction SilentlyContinue)
         if ($desktop.Count -eq 0) { throw 'there is no .NET 10 Desktop Runtime' }
         $compact = Join-Path $env:ProgramFiles 'Microsoft SQL Server Compact Edition\v4.0'
         # The built-in database's engine is installed for the built-in database alone
         # (InstallSQLCE): with an external one given, on a clean machine, none.
         if ($null -ne $State.Database) {
            if (Test-Path -LiteralPath $compact) { throw "SQL Server Compact 4.0 was installed ($compact), although the database is external" }
            return ".NET Desktop Runtime $(($desktop | ForEach-Object Name) -join ', '); no SQL Server Compact, the database being external"
         }
         if (-not (Test-Path -LiteralPath $compact)) { throw "there is no SQL Server Compact 4.0 ($compact)" }
         ".NET Desktop Runtime $(($desktop | ForEach-Object Name) -join ', '); SQL Server Compact 4.0"
      })

   # Only the lines this phase added: an error the install logged is reported once.
   [void] (Test-Step "${Phase}: the server has logged no error" {
         $lines = @(@(Get-ServerErrorLines $app) + @(Get-ServerEvents) | Where-Object { $State.ErrorLinesSeen.Add($_) })
         if ($lines.Count -gt 0) {
            Write-Detail $lines 20
            throw "$($lines.Count) new errors in ERROR_*.log or the Application event log (above)"
         }
         'nothing new in ERROR_*.log or the Application event log'
      })

   Copy-ServerLogs $app $Phase
}

# ------------------------------------------------------------------ the data an upgrade must keep

function New-SmokeData {
   $application = Connect-Server
   try {
      $domain = $application.Domains.Add()
      $domain.Name = $SmokeDomain
      $domain.Active = $true
      $domain.MaxMessageSize = 4321
      $domain.Save()

      $account = $domain.Accounts.Add()
      $account.Address = $SmokeAccount
      $account.Password = $AccountPassword
      $account.Active = $true
      $account.MaxSize = 321
      $account.Save()

      # A server setting, kept in the database, that the upgrade must not reset: the
      # SMTP greeting, which the banner check after the upgrade reads back.
      $State.Banner = 'smoke-banner-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
      $application.Settings.WelcomeSMTP = $State.Banner
   }
   finally { Remove-ComReference $application }

   $State.Subject = 'Installer smoke ' + [guid]::NewGuid().ToString('N')
   $accepted = Send-SmokeMessage $State.Subject

   $message = $null
   $delivered = Wait-Until 90 {
      $application = Connect-Server
      try { $count = [int] $application.Domains.ItemByName($SmokeDomain).Accounts.ItemByAddress($SmokeAccount).Messages.Count }
      finally { Remove-ComReference $application }
      $count -ge 1
   }
   if (-not $delivered) { throw "SMTP accepted the message ('$accepted') and it was not in the mailbox after 90 s" }

   $application = Connect-Server
   try {
      $message = $application.Domains.ItemByName($SmokeDomain).Accounts.ItemByAddress($SmokeAccount).Messages.Item(0)
      $State.MessageFile = [string] $message.Filename
      $subject = [string] $message.Subject
      $State.SchemaBefore = [int] $application.Database.CurrentVersion
   }
   finally { Remove-ComReference $message; Remove-ComReference $application }
   if ($subject -ne $State.Subject) { throw "the delivered message's subject is '$subject'" }
   if (-not (Test-Path -LiteralPath $State.MessageFile)) { throw "the message's file $($State.MessageFile) is not there" }
   $State.MessageHash = (Get-FileHash -LiteralPath $State.MessageFile -Algorithm SHA256).Hash

   $ini = Join-Path $State.App 'Bin\hMailServer.INI'
   $State.IniDatabase = Read-IniSection $ini 'Database'
   $State.IniSecurity = Read-IniSection $ini 'Security'

   # What section_files_common.iss marks uninsneveruninstall: the event scripts and
   # the addon scripts, which the uninstall must leave.
   $kept = New-Object System.Collections.Generic.List[string]
   foreach ($folder in 'Events', 'Addons') {
      $root = Join-Path $State.App $folder
      if (-not (Test-Path -LiteralPath $root)) { continue }
      foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File) {
         $relative = $file.FullName.Substring($State.App.Length + 1)
         if ($relative -notmatch '^Addons\\(DataDirectorySynchronizer|ImportTool)\\') { $kept.Add($relative) }
      }
   }
   $State.KeptScripts = $kept.ToArray()
   $State.Seeded = $true
   return "domain $SmokeDomain, account $SmokeAccount, message '$($State.Subject)' delivered to $(Split-Path -Leaf $State.MessageFile), schema $($State.SchemaBefore)"
}

function Test-SmokeDataKept([string] $Phase) {
   [void] (Test-Step "${Phase}: the domain, the account and their settings are kept" {
         $application = Connect-Server
         try {
            $domain = $application.Domains.ItemByName($SmokeDomain)
            if ($null -eq $domain) { throw "there is no domain $SmokeDomain" }
            $account = $domain.Accounts.ItemByAddress($SmokeAccount)
            if ($null -eq $account) { throw "there is no account $SmokeAccount" }
            $found = "domain MaxMessageSize $($domain.MaxMessageSize), account MaxSize $($account.MaxSize), active $($account.Active)"
            if ($domain.MaxMessageSize -ne 4321 -or $account.MaxSize -ne 321 -or -not $account.Active) { throw "expected 4321, 321 and active: $found" }
            $messages = $account.Messages
            $count = [int] $messages.Count
            $subject = if ($count -ge 1) { [string] $messages.Item(0).Subject } else { '' }
            $banner = [string] $application.Settings.WelcomeSMTP
         }
         finally { Remove-ComReference $application }
         if ($count -ne 1) { throw "the account holds $count messages, not 1" }
         if ($subject -ne $State.Subject) { throw "the message's subject is '$subject'" }
         if ($banner -ne $State.Banner) { throw "the server's WelcomeSMTP setting is '$banner'" }
         "$found; the message, and the server setting WelcomeSMTP"
      })

   [void] (Test-Step "${Phase}: the message's file is unchanged on disk" {
         if (-not (Test-Path -LiteralPath $State.MessageFile)) { throw "$($State.MessageFile) is not there" }
         $hash = (Get-FileHash -LiteralPath $State.MessageFile -Algorithm SHA256).Hash
         if ($hash -ne $State.MessageHash) { throw "its SHA-256 is $hash, it was $($State.MessageHash)" }
         "$(Split-Path -Leaf $State.MessageFile), SHA-256 as before"
      })

   [void] (Test-Step "${Phase}: POP3 logs the account on with its password and finds the message" {
         $stat = Get-Pop3Stat
         if ($stat -notmatch '^\+OK 1 ') { throw "STAT answered '$stat'" }
         "STAT '$stat'"
      })

   [void] (Test-Step "${Phase}: IMAP logs the account on with its password and finds the message" {
         $exists = Get-ImapExists
         if ($exists -ne 1) { throw "SELECT INBOX says $exists EXISTS" }
         'SELECT INBOX: 1 EXISTS'
      })

   [void] (Test-Step "${Phase}: the SMTP banner is the server setting made before" {
         $banner = Read-Banner 25
         if ($banner.IndexOf($State.Banner) -lt 0) { throw "the banner is '$banner'" }
         "'$banner'"
      })

   [void] (Test-Step "${Phase}: hMailServer.INI keeps its database and security sections" {
         $ini = Join-Path $State.App 'Bin\hMailServer.INI'
         if ((Read-IniSection $ini 'Database') -ne $State.IniDatabase) { throw '[Database] changed' }
         # AdministratorPassword may be written again, as PBKDF2 ($h1$): the server
         # re-hashes an older MD5 or SHA-256 value at the next sign-in
         # (IniFileSettings::UpgradeAdministratorPasswordHash, docs/SecretsAtRest.md), and
         # an installer from 6.3.4 writes the password it is given as PBKDF2. Anything
         # else in [Security] must be as it was. That the new hash still verifies the
         # password is the COM sign-in this phase has already made.
         $others = { param($section) (($section -split ';') | Where-Object { $_ -notlike 'AdministratorPassword=*' }) -join ';' }
         $security = Read-IniSection $ini 'Security'
         if ((& $others $security) -ne (& $others $State.IniSecurity)) { throw '[Security] changed beyond AdministratorPassword' }
         $before = Get-SectionValue $State.IniSecurity 'AdministratorPassword'
         $after = Get-SectionValue $security 'AdministratorPassword'
         if ($after -ne $before -and -not $after.StartsWith('$h1$')) { throw 'AdministratorPassword changed to something that is not a PBKDF2 hash' }
         if ($after -ne $before) { '[Database] as it was; [Security] as it was but for AdministratorPassword, written again as PBKDF2' }
         else { '[Database] and [Security] as they were' }
      })
}

# ------------------------------------------------------------------ the secure defaults (docs/SecureDefaults.md)

# The data directory hMailServer.INI names (section_ini.iss writes {app}\Data), under
# which the verb writes the certificate (NewInstallDefaults::CertificateDirectory).
function Get-DataDirectory {
   $folder = Get-IniValue (Join-Path $State.App 'Bin\hMailServer.INI') 'Directories' 'DataFolder'
   if (-not $folder) { $folder = Join-Path $State.App 'Data' }
   return $folder.TrimEnd('\')
}

# What a query selects from the built-in database, as a DataTable, read the way the
# server reads it (SQLCEConnection.cpp): through SQL Server Compact's OLE DB provider,
# from the .sdf in the Database folder, with the password hMailServer.INI keeps -
# under this machine's DPAPI (PasswordEncryption 6, IniFileSettings::SetPassword) or,
# as an older release wrote it, Blowfish (1), which the COM object decrypts. The
# password is used here and nowhere else, and an error that carried it says (hidden)
# instead. The connection is shared, as the running service's is: nothing is
# stopped for a read.
function Invoke-DatabaseQuery([string] $Sql) {
   $ini = Join-Path $State.App 'Bin\hMailServer.INI'
   $type = Get-IniValue $ini 'Database' 'Type'
   if ($type -ne 'MSSQLCE') { throw "hMailServer.INI says database type '$type'; only the built-in database is read here" }
   $file = @(Get-ChildItem -LiteralPath (Join-Path $State.App 'Database') -Filter '*.sdf' -File -ErrorAction SilentlyContinue) | Select-Object -First 1
   if ($null -eq $file) { throw 'there is no .sdf in the Database folder' }

   $stored = Get-IniValue $ini 'Database' 'Password'
   $scheme = Get-IniValue $ini 'Database' 'PasswordEncryption'
   $password = ''
   if ($stored -and $scheme -eq '6') {
      Add-Type -AssemblyName System.Security
      $bytes = [System.Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($stored), $null, [System.Security.Cryptography.DataProtectionScope]::LocalMachine)
      $password = [System.Text.Encoding]::UTF8.GetString($bytes)
   }
   elseif ($stored -and $scheme -eq '1') {
      $application = Connect-Server
      try { $password = [string] $application.Utilities.BlowfishDecrypt($stored) } finally { Remove-ComReference $application }
   }
   elseif ($stored) { $password = $stored }

   $builder = New-Object System.Data.OleDb.OleDbConnectionStringBuilder
   $builder['Provider'] = 'Microsoft.SQLSERVER.CE.OLEDB.4.0'
   $builder['Data Source'] = $file.FullName
   $builder['SSCE:Database Password'] = $password
   $connection = New-Object System.Data.OleDb.OleDbConnection($builder.ConnectionString)
   try {
      $connection.Open()
      $command = $connection.CreateCommand()
      $command.CommandText = $Sql
      $table = New-Object System.Data.DataTable
      $reader = $command.ExecuteReader()
      try { $table.Load($reader) } finally { $reader.Close() }
      return , $table
   }
   catch {
      $message = $_.Exception.Message
      if ($password) { $message = $message.Replace($password, '(hidden)') }
      throw "the database could not be read: $message"
   }
   finally {
      $connection.Dispose()
      $builder.Clear()
      $password = $null
   }
}

# The ports, the IP ranges and the certificate records over COM, each as one line of
# every property the COM object has for it, sorted: what an upgrade must leave as it
# was. The ports and ranges are keyed by what they are, not by their ids.
$PortProperties = @('Protocol', 'PortNumber', 'Address', 'ConnectionSecurity', 'SSLCertificateID', 'UseSSL', 'ClientCertificatePolicy', 'ClientCertificateCAFile')
$RangeProperties = @('Name', 'LowerIP', 'UpperIP', 'Priority', 'Expires', 'RequireSSLTLSForAuth',
   'AllowSMTPConnections', 'AllowPOP3Connections', 'AllowIMAPConnections',
   'RequireSMTPAuthLocalToLocal', 'RequireSMTPAuthLocalToExternal', 'RequireSMTPAuthExternalToLocal', 'RequireSMTPAuthExternalToExternal',
   'AllowDeliveryFromLocalToLocal', 'AllowDeliveryFromLocalToRemote', 'AllowDeliveryFromRemoteToLocal', 'AllowDeliveryFromRemoteToRemote',
   'EnableSpamProtection', 'EnableAntiVirus', 'IsForwardingRelay')

function Get-ListenerState {
   $application = Connect-Server
   try {
      $settings = $application.Settings
      $ports = @()
      $collection = $settings.TCPIPPorts
      for ($i = 0; $i -lt $collection.Count; $i++) {
         $item = $collection.Item($i)
         $ports += (($PortProperties | ForEach-Object { "$_=$($item.$_)" }) -join ' | ')
      }
      $ranges = @()
      $collection = $settings.SecurityRanges
      for ($i = 0; $i -lt $collection.Count; $i++) {
         $item = $collection.Item($i)
         $ranges += (($RangeProperties | ForEach-Object { "$_=$($item.$_)" }) -join ' | ')
      }
      $certificates = @()
      $collection = $settings.SSLCertificates
      for ($i = 0; $i -lt $collection.Count; $i++) {
         $item = $collection.Item($i)
         $certificates += "ID=$($item.ID) | Name=$($item.Name) | CertificateFile=$($item.CertificateFile) | PrivateKeyFile=$($item.PrivateKeyFile)"
      }
   }
   finally { Remove-ComReference $application }
   return [pscustomobject] @{
      Ports        = @($ports | Sort-Object)
      Ranges       = @($ranges | Sort-Object)
      Certificates = @($certificates | Sort-Object)
   }
}

# One property of a line Get-ListenerState made: "Name=My computer | ..." gives
# "My computer" for Name.
function Get-LineValue([string] $Line, [string] $Name) {
   foreach ($part in ($Line -split ' \| ')) {
      if ($part.StartsWith("$Name=")) { return $part.Substring($Name.Length + 1) }
   }
   return ''
}

# The lines of one list the other has not, each marked with the side it is on.
# Compare-Object is not used: it refuses an empty list, and a release before 6.3.4
# has no certificate record at all.
function Compare-Lines([string[]] $Before, [string[]] $After) {
   $old = @($Before | Where-Object { $_ })
   $new = @($After | Where-Object { $_ })
   $gone = @($old | Where-Object { $new -notcontains $_ } | ForEach-Object { "before: $_" })
   $added = @($new | Where-Object { $old -notcontains $_ } | ForEach-Object { "after:  $_" })
   return @($gone + $added)
}

# The setup checklist over COM (Utilities.GetSetupChecklist(false): no DNS lookups),
# and one of its items by key.
function Get-SetupChecklist {
   $application = Connect-Server
   try { $json = [string] $application.Utilities.GetSetupChecklist($false) } finally { Remove-ComReference $application }
   return ($json | ConvertFrom-Json)
}

function Get-ChecklistItem($Checklist, [string] $Key) {
   $item = @($Checklist.items | Where-Object { $_.key -eq $Key }) | Select-Object -First 1
   if ($null -eq $item) { throw "the checklist has no item $Key" }
   return $item
}

function Format-Finding($Finding) {
   $text = "$($Finding.code) ($($Finding.state)) [$(@($Finding.args) -join ', ')]"
   if ($null -ne $Finding.PSObject.Properties['fix']) { $text += ' with a fix' }
   return $text
}

# A PEM certificate file, the first certificate in it.
function Read-PemCertificate([string] $Path) {
   $text = [System.IO.File]::ReadAllText($Path)
   if ($text -notmatch '-----BEGIN CERTIFICATE-----([A-Za-z0-9+/=\s]+?)-----END CERTIFICATE-----') { throw "$Path holds no PEM certificate" }
   $bytes = [Convert]::FromBase64String(($Matches[1] -replace '\s', ''))
   return [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes)
}

# The name the certificate is made for, as NewInstallDefaults.cpp's
# CertificateHostName chooses it: the host name setting, else the machine's fully
# qualified DNS name; lower case, and only what a DNS name holds.
function Get-ExpectedCertificateHost {
   $application = Connect-Server
   try { $name = ([string] $application.Settings.HostName).Trim() } finally { Remove-ComReference $application }
   if (-not $name) {
      $properties = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties()
      $name = $properties.HostName
      if ($properties.DomainName) { $name += '.' + $properties.DomainName }
   }
   $name = ($name.ToLowerInvariant() -replace '[^a-z0-9.-]', '').Trim('.', '-')
   if (-not $name -or $name.Length -gt 253) { $name = 'localhost' }
   return $name
}

# This machine's own IPv4 address on its network: a connection from it to itself
# comes from that address, which the Internet range covers and My computer
# (127.0.0.1) does not - the view a mail client on another machine has.
function Get-LanAddress {
   $address = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
         Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.AddressState -eq 'Preferred' } |
         Sort-Object InterfaceIndex) | Select-Object -First 1
   if ($null -eq $address) { throw 'this machine has no IPv4 address but the loopback' }
   return $address.IPAddress
}

# The lines Inno's log holds for the verb: ApplyNewInstallDefaults logs its exit code.
function Get-VerbLogLines([string] $InnoLog) {
   if (-not (Test-Path -LiteralPath $InnoLog)) { throw "$InnoLog is not there" }
   return @(Select-String -LiteralPath $InnoLog -SimpleMatch 'hMailServer.exe --apply-new-install-defaults' | ForEach-Object { $_.Line.Trim() })
}

# After a new installation: what the installer made (docs/SecureDefaults.md, "What an
# installation on a clean machine is to check").
function Test-NewInstallDefaults([string] $Phase, [string] $InnoLog) {
   $directory = Join-Path (Get-DataDirectory) 'Certificates'
   $certificateFile = Join-Path $directory $InstallationCertificateFile
   $keyFile = Join-Path $directory $InstallationKeyFile

   [void] (Test-Step "${Phase}: Inno's log has the verb's result, --apply-new-install-defaults exited 0" {
         $lines = @(Get-VerbLogLines $InnoLog)
         # The installer's line: 'returned N:' with what the verb said after it, or
         # 'returned N and said nothing.' (1408acf3b, 6.3.6); 'exited N.' before it.
         $exitPattern = '(?:exited (-?\d+)\.|returned (-?\d+)(?::| and said nothing\.))$'
         $exited = @($lines | Where-Object { $_ -match $exitPattern })
         if ($exited.Count -ne 1) { throw "Inno's log has $($exited.Count) exit lines for it: $($lines -join ' | ')" }
         if ($exited[0] -notmatch '(?:exited 0\.|returned 0(?::| and said nothing\.))$') { throw "Inno's log says: $($exited[0])" }
         "'$($exited[0] -replace '^.*(hMailServer\.exe --apply)', '$1')'"
      })

   [void] (Test-Step "${Phase}: the certificate record '$InstallationCertificateName' over COM, and no other" {
         $application = Connect-Server
         try {
            $collection = $application.Settings.SSLCertificates
            $records = @()
            for ($i = 0; $i -lt $collection.Count; $i++) {
               $item = $collection.Item($i)
               $records += [pscustomobject] @{ ID = [int] $item.ID; Name = [string] $item.Name; CertificateFile = [string] $item.CertificateFile; PrivateKeyFile = [string] $item.PrivateKeyFile }
            }
         }
         finally { Remove-ComReference $application }
         if ($records.Count -ne 1) { throw "there are $($records.Count) certificate records: $(($records | ForEach-Object { "'$($_.Name)'" }) -join ', ')" }
         $record = $records[0]
         if ($record.Name -ne $InstallationCertificateName) { throw "the record is '$($record.Name)'" }
         if ($record.CertificateFile -ne $certificateFile) { throw "its certificate file is '$($record.CertificateFile)', not $certificateFile" }
         if ($record.PrivateKeyFile -ne $keyFile) { throw "its key file is '$($record.PrivateKeyFile)', not $keyFile" }
         $State.CertificateId = $record.ID
         "ID $($record.ID): $($record.CertificateFile), $($record.PrivateKeyFile)"
      })

   [void] (Test-Step "${Phase}: its two files under Data\Certificates" {
         foreach ($file in $certificateFile, $keyFile) {
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "$file is not there" }
         }
         $State.CertificateFiles = @($certificateFile, $keyFile)
         # The key's first line only: what kind of key it is, never the key.
         $first = (Get-Content -LiteralPath $keyFile -TotalCount 1).Trim()
         if ($first -ne '-----BEGIN PRIVATE KEY-----') { throw "the key file begins '$($first.Substring(0, [Math]::Min(40, $first.Length)))', not a PKCS#8 private key" }
         $certificate = Read-PemCertificate $certificateFile
         $State.CertificateThumbprint = $certificate.Thumbprint
         "$directory\$InstallationCertificateFile (SHA-1 $($certificate.Thumbprint)) and $InstallationKeyFile (a PKCS#8 private key)"
      })

   # ProtectedFile's DACL: protected, SYSTEM, the Administrators group and the account
   # that wrote it - the administrator who ran the installer, who runs this too - and
   # the service's own account when it is not LocalSystem: the verb runs while the
   # service is still LocalSystem's, and the move to NT SERVICE\hMailServer after the
   # database step grants that account Read on the key (/GrantServiceAccess, which
   # grants every certificate's key the database names).
   [void] (Test-Step "${Phase}: the key's ACL granting nothing to BUILTIN\Users" {
         $acl = Get-Acl -LiteralPath $keyFile
         if (-not $acl.AreAccessRulesProtected) { throw 'its ACL inherits from the folder' }
         $service = Get-HmService
         $serviceSid = ''
         if ($null -ne $service -and $service.StartName -and $service.StartName -notmatch '^(LocalSystem|\.\\LocalSystem|NT AUTHORITY\\SYSTEM)$') {
            $serviceSid = (New-Object System.Security.Principal.NTAccount($service.StartName)).Translate([System.Security.Principal.SecurityIdentifier]).Value
         }
         $installer = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
         $allowed = @('S-1-5-18', 'S-1-5-32-544', $installer)
         if ($serviceSid) { $allowed += $serviceSid }
         $entries = @()
         $strangers = @()
         $usersGranted = $false
         $serviceReads = $false
         foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
            $sid = $rule.IdentityReference.Value
            $name = $sid
            try { $name = $rule.IdentityReference.Translate([System.Security.Principal.NTAccount]).Value } catch { }
            $entries += "$name $($rule.AccessControlType) $($rule.FileSystemRights)"
            if ([string] $rule.AccessControlType -ne 'Allow') { continue }
            if ($sid -eq 'S-1-5-32-545') { $usersGranted = $true }
            if ($allowed -notcontains $sid) { $strangers += "$name ($sid)" }
            # Read: FILE_READ_DATA, or GENERIC_READ or GENERIC_ALL as an entry may
            # keep them unmapped.
            $mask = [int] $rule.FileSystemRights
            if ($serviceSid -and $sid -eq $serviceSid -and (($mask -band 1) -ne 0 -or ($mask -band 0x10000000) -ne 0 -or $mask -lt 0)) { $serviceReads = $true }
         }
         if ($usersGranted) { throw "BUILTIN\Users is granted: $($entries -join '; ')" }
         if ($strangers.Count -gt 0) { throw "granted to $($strangers -join ', '), beyond SYSTEM, the Administrators, the installing account and the service's: $($entries -join '; ')" }
         if ($serviceSid -and -not $serviceReads) { throw "the service logs on as $($service.StartName), which may not read it: $($entries -join '; ')" }
         "protected, the service logs on as $($service.StartName): $($entries -join '; ')"
      })

   [void] (Test-Step "${Phase}: the certificate RSA 2048, for 825 days" {
         $certificate = Read-PemCertificate $certificateFile
         $rsa = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($certificate)
         if ($null -eq $rsa) { throw "its key is $($certificate.PublicKey.Oid.FriendlyName), not RSA" }
         if ($rsa.KeySize -ne 2048) { throw "its key is RSA $($rsa.KeySize)" }
         # NotBefore is five minutes before it was made, NotAfter 825 days after
         # (NewInstallDefaults::Apply_). The clock of a machine just reverted to a
         # snapshot may still be catching up, so when it was made is shown, not held
         # to this run's.
         $made = $certificate.NotBefore.ToUniversalTime().AddMinutes(5)
         $days = ($certificate.NotAfter.ToUniversalTime() - $made).TotalDays
         if ([Math]::Abs($days - 825) -gt (2.0 / 1440)) { throw "it runs $([Math]::Round($days, 4)) days, from $($made.ToString('u')) to $($certificate.NotAfter.ToUniversalTime().ToString('u'))" }
         "RSA $($rsa.KeySize), $($certificate.SignatureAlgorithm.FriendlyName), made $($made.ToString('u')), valid to $($certificate.NotAfter.ToUniversalTime().ToString('u')): 825 days"
      })

   [void] (Test-Step "${Phase}: the certificate's subject naming the host, as its common name and its subjectAltName, and the organisation '$InstallationCertificateOrganization'" {
         $certificate = Read-PemCertificate $certificateFile
         $expected = Get-ExpectedCertificateHost
         $parts = @{}
         foreach ($line in ($certificate.SubjectName.Format($true) -split "`r?`n")) {
            if ($line -match '^\s*([A-Za-z]+)=(.*)$') { $parts[$Matches[1]] = $Matches[2].Trim().Trim('"') }
         }
         $commonName = if ($parts.ContainsKey('CN')) { $parts['CN'] } else { '' }
         $organization = if ($parts.ContainsKey('O')) { $parts['O'] } else { '' }
         $wantedCommonName = if ($expected.Length -le 64) { $expected } else { 'hMailServer' }
         if ($commonName -ne $wantedCommonName) { throw "its common name is '$commonName', and the host is '$expected'" }
         if ($organization -ne $InstallationCertificateOrganization) { throw "its organisation is '$organization'" }
         $alternative = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.17' }) | Select-Object -First 1
         if ($null -eq $alternative) { throw 'it has no subjectAltName' }
         $names = $alternative.Format($false)
         if (@($names -split ',\s*' | Where-Object { $_ -eq "DNS Name=$expected" }).Count -ne 1) { throw "its subjectAltName is '$names', and the host is '$expected'" }
         if ($certificate.Issuer -ne $certificate.Subject) { throw "its issuer is '$($certificate.Issuer)'" }
         "subject '$($certificate.Subject)', subjectAltName '$names'"
      })

   [void] (Test-Step "${Phase}: each of 25, 587, 110 and 143 with optional STARTTLS and that certificate" {
         $application = Connect-Server
         try {
            $collection = $application.Settings.TCPIPPorts
            $ports = @()
            for ($i = 0; $i -lt $collection.Count; $i++) {
               $item = $collection.Item($i)
               $ports += [pscustomobject] @{ Protocol = [int] $item.Protocol; Number = [int] $item.PortNumber; Security = [int] $item.ConnectionSecurity; Certificate = [int] $item.SSLCertificateID; Address = [string] $item.Address }
            }
            $certificates = $application.Settings.SSLCertificates
            $certificateId = 0
            for ($i = 0; $i -lt $certificates.Count; $i++) {
               if ([string] $certificates.Item($i).Name -eq $InstallationCertificateName) { $certificateId = [int] $certificates.Item($i).ID }
            }
         }
         finally { Remove-ComReference $application }
         if ($certificateId -eq 0) { throw "there is no certificate record '$InstallationCertificateName' for the ports to name" }
         if ($ports.Count -ne $ShippedListeners.Count) { throw "there are $($ports.Count) ports: $(($ports | ForEach-Object { $_.Number }) -join ', ')" }
         $found = @()
         foreach ($shipped in $ShippedListeners) {
            $port = @($ports | Where-Object { $_.Protocol -eq $shipped.Protocol -and $_.Number -eq $shipped.Number }) | Select-Object -First 1
            if ($null -eq $port) { throw "there is no $($shipped.Name) port $($shipped.Number)" }
            if ($port.Security -ne $StartTlsOptional) { throw "port $($port.Number)'s connection security is $($port.Security), not $StartTlsOptional (STARTTLS, optional)" }
            if ($port.Certificate -ne $certificateId) { throw "port $($port.Number) names certificate $($port.Certificate), not $certificateId" }
            $found += "$($port.Number) $($shipped.Name)"
         }
         "$($found -join ', '): STARTTLS (optional) with certificate $certificateId"
      })

   [void] (Test-Step "${Phase}: Internet requiring TLS for authentication and My computer not" {
         $application = Connect-Server
         try {
            $collection = $application.Settings.SecurityRanges
            $ranges = @()
            for ($i = 0; $i -lt $collection.Count; $i++) {
               $item = $collection.Item($i)
               $ranges += [pscustomobject] @{ Name = [string] $item.Name; Lower = [string] $item.LowerIP; Upper = [string] $item.UpperIP; Requires = [bool] $item.RequireSSLTLSForAuth }
            }
         }
         finally { Remove-ComReference $application }
         if ($ranges.Count -ne 2) { throw "there are $($ranges.Count) IP ranges: $(($ranges | ForEach-Object { $_.Name }) -join ', ')" }
         $internet = @($ranges | Where-Object { $_.Name -eq 'Internet' }) | Select-Object -First 1
         $local = @($ranges | Where-Object { $_.Name -eq 'My computer' }) | Select-Object -First 1
         if ($null -eq $internet -or $null -eq $local) { throw "the ranges are $(($ranges | ForEach-Object { "'$($_.Name)'" }) -join ', ')" }
         if (-not $internet.Requires) { throw "Internet ($($internet.Lower) to $($internet.Upper)) takes a password without TLS" }
         if ($local.Requires) { throw "My computer ($($local.Lower)) requires TLS for authentication" }
         "Internet ($($internet.Lower) to $($internet.Upper)) RequireSSLTLSForAuth True; My computer ($($local.Lower) to $($local.Upper)) False"
      })

   # These two read the database file itself (Invoke-DatabaseQuery), which only the
   # built-in database has here: an external one is read over COM, as above, and the
   # verb's exit 0 and what it made are checked all the same.
   if ($null -ne $State.Database) {
      Write-Result 'SKIP' "${Phase}: no $PendingMarkName row left" "the $($State.DatabaseIniType) database is not read directly here"
      Write-Result 'SKIP' "${Phase}: each change on the audit trail under '$VerbActor'" "the $($State.DatabaseIniType) database is not read directly here"
   }
   else {
   [void] (Test-Step "${Phase}: no $PendingMarkName row left" {
         $table = Invoke-DatabaseQuery "select count(*) as marks from hm_settings where settingname = '$PendingMarkName'"
         $marks = [int] $table.Rows[0]['marks']
         if ($marks -ne 0) { throw "hm_settings holds $marks $PendingMarkName rows" }
         'hm_settings holds none'
      })

   [void] (Test-Step "${Phase}: each change on the audit trail under '$VerbActor', as the certificate, port and IP range rows it wrote" {
         $table = Invoke-DatabaseQuery "select auditobjecttype, auditobjectname, auditaction from hm_audit where auditactor = '$VerbActor'"
         $rows = @($table.Rows | ForEach-Object { "$($_['auditobjecttype']) '$($_['auditobjectname'])' $($_['auditaction'])" })
         $count = { param($type) @($table.Rows | Where-Object { $_['auditobjecttype'] -eq $type }).Count }
         if ((& $count 'ssl certificate') -ne 1 -or (& $count 'tcp/ip port') -ne 4 -or (& $count 'ip range') -ne 1) {
            throw "$($rows.Count) rows: $($rows -join '; ')"
         }
         "$($rows.Count) rows: $($rows -join '; ')"
      })
   }

   [void] (Test-Step "${Phase}: the checklist over COM answering sign_in_tls done (sign_in_tls_required for Internet), and submission_self_signed for 587" {
         $checklist = Get-SetupChecklist
         $signIn = Get-ChecklistItem $checklist 'sign_in_tls'
         $submission = Get-ChecklistItem $checklist 'submission'
         $said = "sign_in_tls $($signIn.state): $((@($signIn.findings) | ForEach-Object { Format-Finding $_ }) -join '; '); submission $($submission.state): $((@($submission.findings) | ForEach-Object { Format-Finding $_ }) -join '; ')"
         $required = @($signIn.findings | Where-Object { $_.code -eq 'sign_in_tls_required' -and @($_.args)[0] -eq 'Internet' })
         if ($signIn.state -ne 'done' -or $required.Count -ne 1) { throw $said }
         $selfSigned = @($submission.findings | Where-Object { $_.code -eq 'submission_self_signed' -and @($_.args)[0] -eq '587' -and $_.state -eq 'action_needed' })
         if ($selfSigned.Count -ne 1) { throw $said }
         $said
      })
}

# After a new installation, with the account made: STARTTLS from 127.0.0.1 on each
# listener, a password refused before it from this machine's LAN address and taken
# after it, and the sign-ins over 127.0.0.1 as they always were.
function Test-SignInOverTls([string] $Phase) {
   $lan = ''
   try { $lan = Get-LanAddress; Write-Info "${Phase}: this machine's LAN address" "$lan, which the Internet range covers and My computer does not" }
   catch { Write-Result 'FAIL' "${Phase}: this machine has a LAN address to connect from" $_.Exception.Message }

   # The certificate the listener shows: the one on disk, and a subject that says
   # where it came from.
   $checkPeer = {
      param($Conversation)
      $peer = $Conversation.Certificate
      if ($null -eq $peer) { throw 'TLS came up with no peer certificate' }
      if ($peer.Subject -notmatch [regex]::Escape($InstallationCertificateOrganization)) { throw "the peer certificate's subject is '$($peer.Subject)'" }
      if ($State.CertificateThumbprint -and $peer.Thumbprint -ne $State.CertificateThumbprint) { throw "the peer certificate is $($peer.Thumbprint), not $InstallationCertificateFile's $($State.CertificateThumbprint)" }
      "$($Conversation.Tls), peer '$($peer.Subject)'"
   }

   foreach ($port in @(25, 587)) {
      [void] (Test-Step "${Phase}: from 127.0.0.1, STARTTLS offered and completing on $port (EHLO), with a peer certificate whose subject says self-signed at installation" {
            $conversation = Open-Conversation $port
            try {
               [void] (Invoke-SmtpCommand $conversation '' '220' 'banner')
               $ehlo = Invoke-SmtpLines $conversation 'EHLO smoke.client'
               if ($ehlo[-1] -notmatch '^250 ') { throw "EHLO was answered '$($ehlo[-1])'" }
               if ((Get-EhloKeywords $ehlo) -notcontains 'STARTTLS') { throw "EHLO offers $((Get-EhloKeywords $ehlo) -join ', ')" }
               [void] (Invoke-SmtpCommand $conversation 'STARTTLS' '220')
               Start-ConversationTls $conversation
               $peer = & $checkPeer $conversation
               [void] (Invoke-SmtpCommand $conversation 'EHLO smoke.client' '250' 'EHLO after STARTTLS')
               [void] (Invoke-SmtpCommand $conversation 'QUIT' '221')
            }
            finally { $conversation.Client.Close() }
            "STARTTLS in EHLO; $peer; EHLO again answered 250"
         })
   }

   [void] (Test-Step "${Phase}: from 127.0.0.1, STARTTLS offered and completing on 143 (IMAP CAPABILITY), with a peer certificate whose subject says self-signed at installation" {
         $conversation = Open-Conversation 143
         try {
            $banner = Read-Reply $conversation
            if (-not $banner.StartsWith('* OK')) { throw "IMAP banner '$banner'" }
            $capability = Invoke-ImapExchange $conversation 'c1' 'CAPABILITY'
            $line = @($capability.Untagged | Where-Object { $_ -like '* CAPABILITY *' }) | Select-Object -First 1
            if ($capability.Tagged -notmatch '^c1 OK' -or $line -notmatch '\bSTARTTLS\b') { throw "CAPABILITY was answered '$line' '$($capability.Tagged)'" }
            $start = Invoke-ImapExchange $conversation 'c2' 'STARTTLS'
            if ($start.Tagged -notmatch '^c2 OK') { throw "STARTTLS was answered '$($start.Tagged)'" }
            Start-ConversationTls $conversation
            $peer = & $checkPeer $conversation
            $again = Invoke-ImapExchange $conversation 'c3' 'CAPABILITY'
            if ($again.Tagged -notmatch '^c3 OK') { throw "CAPABILITY after STARTTLS was answered '$($again.Tagged)'" }
            [void] (Invoke-ImapExchange $conversation 'c4' 'LOGOUT')
         }
         finally { $conversation.Client.Close() }
         "STARTTLS in CAPABILITY; $peer; CAPABILITY again answered OK"
      })

   [void] (Test-Step "${Phase}: from 127.0.0.1, STARTTLS offered and completing on 110 (POP3 CAPA with STLS), with a peer certificate whose subject says self-signed at installation" {
         $conversation = Open-Conversation 110
         try {
            $banner = Read-Reply $conversation
            if (-not $banner.StartsWith('+OK')) { throw "POP3 banner '$banner'" }
            $capabilities = Get-Pop3Capabilities $conversation
            if ($capabilities -notcontains 'STLS') { throw "CAPA offers $($capabilities -join ', ')" }
            [void] (Invoke-Pop3Command $conversation 'STLS' '+OK')
            Start-ConversationTls $conversation
            $peer = & $checkPeer $conversation
            [void] (Get-Pop3Capabilities $conversation)
            [void] (Invoke-Pop3Command $conversation 'QUIT' '+OK')
         }
         finally { $conversation.Client.Close() }
         "STLS in CAPA; $peer; CAPA again answered +OK"
      })

   if ($lan) {
      [void] (Test-Step "${Phase}: from the machine's own LAN address, $lan, no AUTH in EHLO on 587, AUTH LOGIN answered 530, and the account's password taken after STARTTLS" {
            $conversation = Open-Conversation 587 $lan
            try {
               [void] (Invoke-SmtpCommand $conversation '' '220' 'banner')
               $ehlo = Invoke-SmtpLines $conversation 'EHLO smoke.client'
               $keywords = Get-EhloKeywords $ehlo
               if (@($keywords | Where-Object { $_ -match '^AUTH\b' }).Count -gt 0) { throw "EHLO offers AUTH before STARTTLS: $($keywords -join ', ')" }
               if ($keywords -notcontains 'STARTTLS') { throw "EHLO offers no STARTTLS: $($keywords -join ', ')" }
               $refused = Invoke-SmtpLines $conversation 'AUTH LOGIN'
               if ($refused[-1] -notmatch '^530 ') { throw "AUTH LOGIN before STARTTLS was answered '$($refused[-1])'" }
               [void] (Invoke-SmtpCommand $conversation 'STARTTLS' '220')
               Start-ConversationTls $conversation
               $after = Get-EhloKeywords (Invoke-SmtpLines $conversation 'EHLO smoke.client')
               if (@($after | Where-Object { $_ -match '^AUTH\b' }).Count -eq 0) { throw "EHLO after STARTTLS offers no AUTH: $($after -join ', ')" }
               $accepted = Invoke-SmtpAuthLogin $conversation
               [void] (Invoke-SmtpCommand $conversation 'QUIT' '221')
            }
            finally { $conversation.Client.Close() }
            "EHLO: $($keywords -join ', '); AUTH LOGIN answered '$($refused[-1])'; after STARTTLS ($($conversation.Tls)) '$accepted'"
         })

      [void] (Test-Step "${Phase}: from $lan, IMAP LOGIN answered BAD, and the account's password taken after STARTTLS" {
            $conversation = Open-Conversation 143 $lan
            try {
               $banner = Read-Reply $conversation
               if (-not $banner.StartsWith('* OK')) { throw "IMAP banner '$banner'" }
               $capability = Invoke-ImapExchange $conversation 'l1' 'CAPABILITY'
               $line = [string] (@($capability.Untagged | Where-Object { $_ -like '* CAPABILITY *' }) | Select-Object -First 1)
               if ($line -notmatch '\bSTARTTLS\b' -or $line -notmatch '\bLOGINDISABLED\b') { throw "CAPABILITY before STARTTLS was answered '$line'" }
               $refused = Invoke-ImapExchange $conversation 'l2' "LOGIN `"$SmokeAccount`" `"$AccountPassword`""
               if ($refused.Tagged -notmatch '^l2 BAD ') { throw "LOGIN before STARTTLS was answered '$($refused.Tagged)'" }
               $start = Invoke-ImapExchange $conversation 'l3' 'STARTTLS'
               if ($start.Tagged -notmatch '^l3 OK') { throw "STARTTLS was answered '$($start.Tagged)'" }
               Start-ConversationTls $conversation
               $accepted = Invoke-ImapExchange $conversation 'l4' "LOGIN `"$SmokeAccount`" `"$AccountPassword`""
               if ($accepted.Tagged -notmatch '^l4 OK') { throw "LOGIN after STARTTLS was answered '$($accepted.Tagged)'" }
               [void] (Invoke-ImapExchange $conversation 'l5' 'LOGOUT')
            }
            finally { $conversation.Client.Close() }
            "CAPABILITY with LOGINDISABLED; LOGIN answered '$($refused.Tagged)'; after STARTTLS ($($conversation.Tls)) '$($accepted.Tagged)'"
         })

      [void] (Test-Step "${Phase}: from $lan, POP3 USER answered -ERR, and the account's password taken after STLS" {
            $conversation = Open-Conversation 110 $lan
            try {
               $banner = Read-Reply $conversation
               if (-not $banner.StartsWith('+OK')) { throw "POP3 banner '$banner'" }
               $capabilities = Get-Pop3Capabilities $conversation
               if ($capabilities -notcontains 'STLS' -or $capabilities -contains 'USER') { throw "CAPA before STLS offers $($capabilities -join ', ')" }
               $refused = Invoke-Pop3Command $conversation "USER $SmokeAccount" '-ERR'
               [void] (Invoke-Pop3Command $conversation 'STLS' '+OK')
               Start-ConversationTls $conversation
               [void] (Invoke-Pop3Command $conversation "USER $SmokeAccount" '+OK')
               $accepted = Invoke-Pop3Command $conversation "PASS $AccountPassword" '+OK' 'PASS'
               [void] (Invoke-Pop3Command $conversation 'QUIT' '+OK')
            }
            finally { $conversation.Client.Close() }
            "CAPA without USER; USER answered '$refused'; after STLS ($($conversation.Tls)) PASS answered '$accepted'"
         })

      [void] (Test-Step "${Phase}: from $lan, mail on port 25 taken with or without TLS, as before" {
            $conversation = Open-Conversation 25 $lan
            try {
               [void] (Invoke-SmtpCommand $conversation '' '220' 'banner')
               $keywords = Get-EhloKeywords (Invoke-SmtpLines $conversation 'EHLO smoke.client')
               if (@($keywords | Where-Object { $_ -match '^AUTH\b' }).Count -gt 0) { throw "EHLO offers AUTH before STARTTLS: $($keywords -join ', ')" }
               # Taken as far as the recipient and reset, so that nothing is
               # delivered beside the one message the uninstall checks.
               [void] (Invoke-SmtpCommand $conversation "MAIL FROM:<$SmokeSender>" '250')
               $plain = Invoke-SmtpCommand $conversation "RCPT TO:<$SmokeAccount>" '250'
               [void] (Invoke-SmtpCommand $conversation 'RSET' '250')
               [void] (Invoke-SmtpCommand $conversation 'STARTTLS' '220')
               Start-ConversationTls $conversation
               [void] (Invoke-SmtpCommand $conversation 'EHLO smoke.client' '250' 'EHLO after STARTTLS')
               [void] (Invoke-SmtpCommand $conversation "MAIL FROM:<$SmokeSender>" '250')
               $secured = Invoke-SmtpCommand $conversation "RCPT TO:<$SmokeAccount>" '250'
               [void] (Invoke-SmtpCommand $conversation 'RSET' '250')
               [void] (Invoke-SmtpCommand $conversation 'QUIT' '221')
            }
            finally { $conversation.Client.Close() }
            "no AUTH in EHLO; RCPT TO:<$SmokeAccount> answered '$plain' without TLS and '$secured' after STARTTLS ($($conversation.Tls))"
         })
   }

   [void] (Test-Step "${Phase}: the existing sign-ins over 127.0.0.1 passing without TLS as before" {
         $conversation = Open-Conversation 587
         try {
            [void] (Invoke-SmtpCommand $conversation '' '220' 'banner')
            $keywords = Get-EhloKeywords (Invoke-SmtpLines $conversation 'EHLO smoke.client')
            if (@($keywords | Where-Object { $_ -match '^AUTH\b' }).Count -eq 0) { throw "EHLO on 587 offers no AUTH: $($keywords -join ', ')" }
            $smtp = Invoke-SmtpAuthLogin $conversation
            [void] (Invoke-SmtpCommand $conversation 'QUIT' '221')
         }
         finally { $conversation.Client.Close() }
         $stat = Get-Pop3Stat
         $exists = Get-ImapExists
         if ($exists -lt 0) { throw 'IMAP SELECT INBOX gave no EXISTS' }
         "SMTP AUTH LOGIN on 587 '$smtp'; POP3 USER/PASS, STAT '$stat'; IMAP LOGIN, SELECT INBOX $exists EXISTS"
      })
}

# hMailServer.exe --apply-new-install-defaults run by hand, as docs/SecureDefaults.md
# tells an administrator to run it: the service stopped and its process gone - the
# verb refuses a running server (exit 4) - and started again afterwards, whatever
# the verb did. Its exit code and the lines it wrote.
function Invoke-NewInstallDefaultsVerb {
   $exe = Join-Path $State.App 'Bin\hMailServer.exe'
   $out = Join-Path $LogDirectory 'apply-new-install-defaults.stdout.txt'
   $err = Join-Path $LogDirectory 'apply-new-install-defaults.stderr.txt'
   [GC]::Collect()
   [GC]::WaitForPendingFinalizers()
   Stop-Service -Name $ServiceName -ErrorAction Stop -WarningAction SilentlyContinue
   try {
      if (-not (Wait-Until 60 { @(Get-Process -Name $ServiceName -ErrorAction SilentlyContinue).Count -eq 0 })) { throw 'hMailServer.exe was still running 60 s after the service stopped' }
      $process = Start-Process -FilePath $exe -ArgumentList '--apply-new-install-defaults' -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
      $null = $process.Handle
      if (-not $process.WaitForExit(5 * 60 * 1000)) {
         try { $process.Kill() } catch { }
         throw 'it did not finish within five minutes'
      }
      $lines = @(@(Get-Content -LiteralPath $out -ErrorAction SilentlyContinue) + @(Get-Content -LiteralPath $err -ErrorAction SilentlyContinue) | Where-Object { $_ -and $_.Trim() })
      return [pscustomobject] @{ ExitCode = $process.ExitCode; Lines = $lines }
   }
   finally {
      Start-Service -Name $ServiceName -ErrorAction SilentlyContinue -WarningAction SilentlyContinue
      [void] (Wait-Until 90 { $null -ne (Get-NetTCPConnection -LocalPort 25 -State Listen -ErrorAction SilentlyContinue) })
   }
}

# After an upgrade: the listeners, the ranges and the certificates exactly as the
# older release left them, and nothing that a new installation is given
# (docs/SecureDefaults.md, "An upgrade is never changed").
function Test-UpgradeKeepsListeners([string] $Phase, [string] $InnoLog, [bool] $FromWithout) {
   [void] (Test-Step "${Phase}: the installer not even running the verb for an upgrade (Inno's log)" {
         $lines = @(Get-VerbLogLines $InnoLog)
         if ($lines.Count -gt 0) { throw "Inno's log says: $($lines -join ' | ')" }
         "no line for it: IsNewDatabase is false where the settings file already names a database"
      })

   [void] (Test-Step "${Phase}: every port's security exactly as it was" {
         if ($null -eq $State.ListenersBefore) { throw 'the ports were not read before the upgrade' }
         $ports = (Get-ListenerState).Ports
         $difference = @(Compare-Lines $State.ListenersBefore.Ports $ports)
         if ($difference.Count -gt 0) { Write-Detail $difference; throw "$($difference.Count) lines differ (above)" }
         "$($ports.Count) ports, every property as it was: $(($ports | ForEach-Object { "$(Get-LineValue $_ 'PortNumber') ConnectionSecurity $(Get-LineValue $_ 'ConnectionSecurity') certificate $(Get-LineValue $_ 'SSLCertificateID')" }) -join '; ')"
      })

   [void] (Test-Step "${Phase}: both ranges exactly as they were" {
         if ($null -eq $State.ListenersBefore) { throw 'the ranges were not read before the upgrade' }
         $ranges = (Get-ListenerState).Ranges
         $difference = @(Compare-Lines $State.ListenersBefore.Ranges $ranges)
         if ($difference.Count -gt 0) { Write-Detail $difference; throw "$($difference.Count) lines differ (above)" }
         "$($ranges.Count) ranges, every property as it was: $(($ranges | ForEach-Object { "$(Get-LineValue $_ 'Name') RequireSSLTLSForAuth $(Get-LineValue $_ 'RequireSSLTLSForAuth')" }) -join '; ')"
      })

   [void] (Test-Step "${Phase}: no certificate record made" {
         if ($null -eq $State.ListenersBefore) { throw 'the certificates were not read before the upgrade' }
         $certificates = (Get-ListenerState).Certificates
         $difference = @(Compare-Lines $State.ListenersBefore.Certificates $certificates)
         if ($difference.Count -gt 0) { Write-Detail $difference; throw "$($difference.Count) lines differ (above)" }
         $key = Join-Path (Join-Path (Get-DataDirectory) 'Certificates') $InstallationKeyFile
         if ($FromWithout -and (Test-Path -LiteralPath $key)) { throw "$key is there" }
         "$($certificates.Count) certificate records, as before$(if ($FromWithout) { "; no $key" })"
      })

   [void] (Test-Step "${Phase}: no $PendingMarkName row, since its database has no mark" {
         $table = Invoke-DatabaseQuery "select count(*) as marks from hm_settings where settingname = '$PendingMarkName'"
         $marks = [int] $table.Rows[0]['marks']
         if ($marks -ne 0) { throw "hm_settings holds $marks $PendingMarkName rows" }
         'hm_settings holds none: an upgrade writes no mark'
      })

   if ($FromWithout) {
      [void] (Test-Step "${Phase}: the checklist over COM answering sign_in_tls as action needed with sign_in_no_tls for Internet, and no fix" {
            $signIn = Get-ChecklistItem (Get-SetupChecklist) 'sign_in_tls'
            $said = "sign_in_tls $($signIn.state): $((@($signIn.findings) | ForEach-Object { Format-Finding $_ }) -join '; ')"
            $internet = @($signIn.findings | Where-Object { @($_.args)[0] -eq 'Internet' })
            if ($signIn.state -ne 'action_needed' -or $internet.Count -ne 1) { throw $said }
            $finding = $internet[0]
            if ($finding.code -ne 'sign_in_no_tls' -or $finding.state -ne 'action_needed') { throw $said }
            if ($null -ne $finding.PSObject.Properties['fix']) { throw "the finding carries a fix: $said" }
            $said
         })
   }
}

# After an upgrade: the verb run by hand refuses the upgraded server, whose database
# carries no mark, and changes nothing (docs/SecureDefaults.md: "An upgraded server
# cannot be given these defaults by the verb").
function Test-VerbLeavesUpgradeAlone([string] $Phase) {
   [void] (Test-Step "${Phase}: an upgraded server cannot be given these defaults by the verb run by hand (exit 5, nothing changed)" {
         $before = Get-ListenerState
         $result = Invoke-NewInstallDefaultsVerb
         $said = ($result.Lines -join ' ') -replace '\s+', ' '
         if ($result.ExitCode -ne 5) { throw "exit code $($result.ExitCode): $said" }
         if ($said -notmatch "has no $PendingMarkName mark") { throw "exit 5 for another reason: $said" }
         $running = Wait-Until 90 { $s = Get-HmService; $null -ne $s -and $s.State -eq 'Running' }
         if (-not $running) { throw 'the service did not start again after the verb' }
         $after = Get-ListenerState
         $difference = @(Compare-Lines $before.Ports $after.Ports) + @(Compare-Lines $before.Ranges $after.Ranges) + @(Compare-Lines $before.Certificates $after.Certificates)
         if ($difference.Count -gt 0) { Write-Detail $difference; throw "$($difference.Count) lines changed (above)" }
         "exit 5: '$($said.Substring(0, [Math]::Min(220, $said.Length)))'; the ports, the ranges and the certificates as they were; the service running again"
      })
}

# ------------------------------------------------------------------ the uninstall

# The server registers the Application log's event source the first time it writes an
# event (WindowsEventLog::EnsureSourceRegistered_), and hMailServer.exe /Unregister,
# which the uninstaller runs, removes it. A run in which the server wrote no event -
# the normal case since the upgrade stopped reporting HM5011 on 25 September 2026 -
# would leave that removal unchecked, so a missing source is registered here the way
# the server registers it: the .NET Framework's EventLogMessages.dll as the message
# file and the three event types.
function Register-EventSourceIfMissing {
   $path = "SYSTEM\CurrentControlSet\Services\EventLog\Application\$ServiceName"
   if (Test-RegistryKey 'Registry64' $path) { return 'the server registered it during the run' }
   $base = Open-LocalMachine 'Registry64'
   try {
      $key = $base.CreateSubKey($path)
      try {
         $key.SetValue('EventMessageFile', '%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\EventLogMessages.dll', [Microsoft.Win32.RegistryValueKind]::ExpandString)
         $key.SetValue('TypesSupported', 7, [Microsoft.Win32.RegistryValueKind]::DWord)
      }
      finally { $key.Close() }
   }
   finally { $base.Close() }
   return 'the server wrote no event during the run, so the test registered the source as the server does, for the uninstall to remove'
}

function Invoke-Uninstall {
   $entry = Get-UninstallEntry
   if ($null -eq $entry -or -not $entry.UninstallString) { throw 'there is no uninstall entry to run' }
   $uninstaller = $entry.UninstallString.Trim('"')
   $log = Join-Path $LogDirectory 'uninstall.log'
   $result = Invoke-Setup $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$log") $log
   # unins000.exe may hand the work to a copy of itself in %TEMP% (_iu*.tmp), which
   # removes the files and the entry: wait for it to finish.
   $done = Wait-Until ($TimeoutMinutes * 60) {
      $copies = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like '_iu*' -or $_.ProcessName -like 'unins*' })
      $copies.Count -eq 0 -and $null -eq (Get-UninstallEntry)
   }
   if (-not $done) {
      Write-LogTail $log
      throw "$result, and after $TimeoutMinutes minutes the uninstaller was still running or its entry still there"
   }
   # unins000.exe itself goes last, deleted once it has exited. If it stays, the
   # program files check says so.
   [void] (Wait-Until 60 { -not (Test-Path -LiteralPath $uninstaller) })
   return $result
}

function Test-Uninstalled {
   $app = $State.App

   [void] (Test-Step 'uninstall: the service is gone' {
         $service = Get-HmService
         if ($null -ne $service) { throw "the service is still registered, state $($service.State)" }
         if (Test-RegistryKey 'Registry64' "SYSTEM\CurrentControlSet\Services\$ServiceName") { throw "its key Services\$ServiceName is still there" }
         $answer = Invoke-Native "sc.exe query $ServiceName"
         if ($answer.ExitCode -ne 1060) { throw "sc query answered $($answer.ExitCode): $($answer.Output -replace '\s+', ' ')" }
         'sc query: 1060, the service does not exist'
      })

   [void] (Test-Step 'uninstall: no hMailServer process is left running' {
         $processes = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'hMailServer*' })
         if ($processes.Count -gt 0) { throw "running: $(($processes | ForEach-Object ProcessName) -join ', ')" }
         'none'
      })

   [void] (Test-Step 'uninstall: the server logged no error while it was stopped and removed' {
         $lines = @(@(Get-ServerErrorLines $app) + @(Get-ServerEvents) | Where-Object { $State.ErrorLinesSeen.Add($_) })
         if ($lines.Count -gt 0) {
            Write-Detail $lines 20
            throw "$($lines.Count) new errors in ERROR_*.log or the Application event log (above)"
         }
         'nothing new in ERROR_*.log or the Application event log'
      })

   # Every file left under the installation directory must be one the installer keeps
   # on purpose. What it keeps, from the .iss: [INI] entries carry no uninsdeleteentry,
   # so hMailServer.INI stays; the Events and Addons scripts are uninsneveruninstall;
   # the database, the message store and the logs were written by the server and the
   # database tools, not by the installer, and [Dirs] removes a folder only when it is
   # empty. So is Bin\hMailServerApiKeys.ini, the REST API's key store beside
   # hMailServer.INI (RestApiServer::GetApiKeyStoreFile): the server writes it, and
   # /GrantServiceAccess makes it empty when a new installation's service moves to
   # NT SERVICE\hMailServer (docs/ServiceAccount.md), so an installation whose service
   # stays LocalSystem - every run until the new installation of 6.3.4 on 27
   # September 2026 - has none. And the files a -FolderCase put in the program folder
   # before the first installer ($State.CaseFiles, the precreated-user case's
   # Bin\notes.txt) were never the installer's: Inno Setup removes only what it
   # installed, so an uninstall leaves them where they were, which is what is held -
   # each of them still there. Everything else is a program file, and must be gone.
   $keptPattern = '^(Bin\\hMailServer\.ini|Bin\\hMailServerApiKeys\.ini|Database\\.+|Data\\.+|Logs\\.+|Events\\.+|Addons\\(?!(DataDirectorySynchronizer|ImportTool)\\).+)$'
   $left = @()
   if (Test-Path -LiteralPath $app) {
      $left = @(Get-ChildItem -LiteralPath $app -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($app.Length + 1) })
   }
   $caseFiles = @($State.CaseFiles)
   $programFiles = @($left | Where-Object { $_ -notmatch $keptPattern -and $caseFiles -notcontains $_ })
   $keptFiles = @($left | Where-Object { $_ -match $keptPattern })

   [void] (Test-Step 'uninstall: the program files are gone' {
         $gone = @($caseFiles | Where-Object { $left -notcontains $_ })
         if ($gone.Count -gt 0) { throw "the uninstall removed $($gone -join ', '), which the case made before the installer and the installer did not install" }
         if ($programFiles.Count -gt 0) {
            Write-Detail $programFiles 30
            $log = Join-Path $LogDirectory 'uninstall.log'
            $inUse = 0
            if (Test-Path -LiteralPath $log) { $inUse = @(Select-String -LiteralPath $log -SimpleMatch 'Failed to delete the file; it may be in use').Count }
            throw "$($programFiles.Count) files are left that the installer does not keep (above); the uninstall log says 'Failed to delete the file; it may be in use' $inUse times"
         }
         foreach ($folder in 'ControlPanel', 'WebAdmin', 'DBScripts', 'Languages', 'Addons\DataDirectorySynchronizer', 'Addons\ImportTool') {
            if (Test-Path -LiteralPath (Join-Path $app $folder)) { throw "$folder is still there" }
         }
         # Filtered by extension here, not with -Include: Windows PowerShell 5.1 ignores
         # -Include beside -LiteralPath and lists every file, so the kept scripts, the
         # INI, the database and the message were reported as "binaries" (the rehearsal
         # of 26 September 2026).
         $binaries = @(Get-ChildItem -LiteralPath $app -Recurse -File -Force -ErrorAction SilentlyContinue |
               Where-Object { @('.exe', '.dll', '.tlb') -contains $_.Extension.ToLowerInvariant() })
         if ($binaries.Count -gt 0) { throw "binaries are left: $(($binaries | ForEach-Object { $_.FullName.Substring($app.Length + 1) }) -join ', ')" }
         "$($left.Count) files left under $app, every one of them kept on purpose$(if ($caseFiles.Count -gt 0) { " or made by the case before the installer ($($caseFiles -join ', '))" })"
      })

   Write-Info 'uninstall: what is left under the installation directory' (($keptFiles | ForEach-Object { ($_ -split '\\')[0] } | Group-Object | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ')

   [void] (Test-Step 'uninstall: hMailServer.INI is kept, with its database settings' {
         $ini = Join-Path $app 'Bin\hMailServer.INI'
         if (-not (Test-Path -LiteralPath $ini)) { throw "$ini is gone" }
         if ($State.IniDatabase -and (Read-IniSection $ini 'Database') -ne $State.IniDatabase) { throw '[Database] changed' }
         "Bin\hMailServer.INI, [Database] type $(Get-IniValue $ini 'Database' 'Type')"
      })

   # An external database is on its own server, which an uninstall does not touch;
   # [Database] still naming it is the check above.
   if ($null -ne $State.Database) {
      Write-Result 'SKIP' 'uninstall: the database is kept' "the $($State.DatabaseIniType) database is on its own server, which the uninstaller does not touch"
   }
   else {
   [void] (Test-Step 'uninstall: the database is kept' {
         $files = @(Get-ChildItem -LiteralPath (Join-Path $app 'Database') -Filter '*.sdf' -File -ErrorAction SilentlyContinue)
         if ($files.Count -eq 0) { throw 'there is no .sdf left in Database' }
         ($files | ForEach-Object { "Database\$($_.Name)" }) -join ', '
      })
   }

   # docs/SecureDefaults.md: the certificate a new installation made is data, kept
   # as the rest of the data directory is, and the database kept beside it still
   # names it.
   if ($State.CertificateFiles.Count -gt 0) {
      [void] (Test-Step 'uninstall: Data\Certificates kept with the rest of the data directory' {
            $gone = @($State.CertificateFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
            if ($gone.Count -gt 0) { throw "gone: $($gone -join ', ')" }
            "$(($State.CertificateFiles | ForEach-Object { $_.Substring($app.Length + 1) }) -join ', ')"
         })
   }

   if ($State.Seeded) {
      [void] (Test-Step 'uninstall: the message store is kept, the message unchanged' {
            if (-not (Test-Path -LiteralPath $State.MessageFile)) { throw "$($State.MessageFile) is gone" }
            $hash = (Get-FileHash -LiteralPath $State.MessageFile -Algorithm SHA256).Hash
            if ($hash -ne $State.MessageHash) { throw "its SHA-256 is $hash, it was $($State.MessageHash)" }
            "$($State.MessageFile.Substring($app.Length + 1)), SHA-256 as delivered"
         })

      [void] (Test-Step 'uninstall: the event and addon scripts are kept' {
            $missing = @($State.KeptScripts | Where-Object { -not (Test-Path -LiteralPath (Join-Path $app $_)) })
            if ($State.KeptScripts.Count -eq 0) { throw 'the install had put no event or addon script to keep' }
            if ($missing.Count -gt 0) { throw "gone: $($missing -join ', ')" }
            "all $($State.KeptScripts.Count): $($State.KeptScripts -join ', ')"
         })
   }

   [void] (Test-Step 'uninstall: the Start menu group is gone' {
         if (Test-Path -LiteralPath $StartMenuGroup) { throw "$StartMenuGroup is still there" }
         'gone'
      })

   [void] (Test-Step 'uninstall: the uninstall entry and Software\hMailServer are gone' {
         $left = New-Object System.Collections.Generic.List[string]
         foreach ($view in 'Registry64', 'Registry32') {
            foreach ($path in $UninstallKey, 'SOFTWARE\hMailServer') {
               if (Test-RegistryKey $view $path) { $left.Add("HKLM\$path ($view)") }
            }
         }
         if ($left.Count -gt 0) { throw "still there: $($left -join ', ')" }
         'both gone from both registry views'
      })

   [void] (Test-Step 'uninstall: every COM registration is gone' {
         $footprint = Get-ComFootprint
         if ($footprint.Count -gt 0) {
            Write-Detail $footprint 30
            throw "$($footprint.Count) of the $($State.ComAfterInstall) keys the install registered are left (above)"
         }
         "none of the $($State.ComAfterInstall) keys is left"
      })

   [void] (Test-Step 'uninstall: the hMailServer event log source is gone' {
         $path = "SYSTEM\CurrentControlSet\Services\EventLog\Application\$ServiceName"
         if (Test-RegistryKey 'Registry64' $path) {
            $file = [string] (Get-RegistryValue 'Registry64' $path 'EventMessageFile')
            throw "HKLM\$path is still there (EventMessageFile '$file'): the server registers it the first time it writes to the Application log, and hMailServer.exe /Unregister, which the uninstaller runs, did not remove it"
         }
         'no Application log source hMailServer'
      })

   [void] (Test-Step 'uninstall: no scheduled task names hMailServer' {
         $tasks = @(Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like '*hMail*' -or $_.TaskPath -like '*hMail*' })
         if ($tasks.Count -gt 0) { throw "left: $(($tasks | ForEach-Object { $_.TaskPath + $_.TaskName }) -join ', ')" }
         'none'
      })

   if ($null -ne $State.RegistryBaseline) {
      $added = Get-NewRegistryKeys $State.RegistryBaseline
      $ours = @($added | Where-Object { $_ -match 'hmail' })
      $others = @($added | Where-Object { $_ -notmatch 'hmail' })
      [void] (Test-Step 'uninstall: no registry key naming hMailServer is left that the clean machine did not have' {
            if ($ours.Count -gt 0) {
               Write-Detail $ours 30
               throw "$($ours.Count) left (above)"
            }
            "none, under the $($SnapshotKeys.Count) keys compared with the clean machine"
         })
      if ($others.Count -gt 0) {
         Write-Info 'uninstall: other registry keys the clean machine did not have' "$($others.Count), left by the prerequisites the installer carries and never removes, or by Windows"
         Write-Detail $others 25
      }
   }

   $leftovers = New-Object System.Collections.Generic.List[string]
   if (@(Get-ChildItem -Path (Join-Path $env:ProgramFiles 'dotnet\shared\Microsoft.WindowsDesktop.App') -Directory -Filter '10.*' -ErrorAction SilentlyContinue).Count -gt 0) { $leftovers.Add('.NET 10 Desktop Runtime') }
   if (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Microsoft SQL Server Compact Edition\v4.0')) { $leftovers.Add('SQL Server Compact 4.0') }
   if ($leftovers.Count -gt 0) {
      Write-Info 'uninstall: prerequisites still installed' "$($leftovers -join ', '): shared runtimes the installer installs when missing and never removes"
   }
}

# ------------------------------------------------------------------ work item 78: the program folder before the files

# Whether an installer makes the program folder with permissions of its own, and
# hardens one that is there with Bin and ControlPanel, before any file is copied
# (docs/ServiceAccount.md, "Before the files"): the release after 6.3.7, and, when
# -HardenedBeforeFiles says so, a build that still says 6.3.7 - never an installer
# that says an earlier version, such as the first of an upgrade run from 6.3.6. For
# one that is not held to it, what the watch saw is said as INFO.
$HardenedBeforeFilesVersion = [version] '6.3.7'
function Test-HardensBeforeFiles([string] $Version) {
   $release = Get-ReleaseVersion $Version
   if ($null -eq $release) { return [bool] $HardenedBeforeFiles }
   if ($release -gt $HardenedBeforeFilesVersion) { return $true }
   return $HardenedBeforeFiles -and ($release -eq $HardenedBeforeFilesVersion)
}

# What a folder's list is at one moment: whether it inherits nothing, how many of its
# entries are inherited, its owner, the allow entries that let a group of every user
# change it, and the list in SDDL - for the record.
function Get-FolderMoment([string] $Path) {
   try {
      $acl = Get-Acl -LiteralPath $Path
      $inherited = 0
      $broad = @()
      foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
         if ($rule.IsInherited) { $inherited++ }
         if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) { continue }
         if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
         if ($BroadGroupSids -notcontains $rule.IdentityReference.Value) { continue }
         if (([long] $rule.FileSystemRights -band $WriteRightsMask) -ne 0) { $broad += "$($rule.IdentityReference.Value) $($rule.FileSystemRights)" }
      }
      return [pscustomobject] @{ Protected = $acl.AreAccessRulesProtected; Inherited = $inherited; Broad = $broad
                                 Owner = $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value; Sddl = $acl.Sddl; Error = '' }
   }
   catch { return [pscustomobject] @{ Protected = $false; Inherited = 0; Broad = @(); Owner = ''; Sddl = ''; Error = $_.Exception.Message } }
}

# A watch on the program folder while an installer runs, in a runspace of its own:
# the program folder's list at the first moment it is there, when it was not before,
# and Bin's and ControlPanel's - and the program folder's again - at the moment the
# first file that was not there before appears in each (Inno Setup writes each file
# under a temporary name in its folder first). The moments are read with Get-Acl as
# soon as the loop sees them, a millisecond or two after they happen; setup's own
# window, before it made the folder with its permissions, was about 45 ms
# (6 October 2026).
$FolderWatchScript = {
   param($App, $Sync, $Broad, $Mask)
   function Moment([string] $Path) {
      try {
         $acl = [System.IO.Directory]::GetAccessControl($Path)
         $inherited = 0
         $writers = @()
         foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
            if ($rule.IsInherited) { $inherited++ }
            if ($rule.AccessControlType -ne 'Allow') { continue }
            if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
            if ($Broad -notcontains $rule.IdentityReference.Value) { continue }
            if (([long] $rule.FileSystemRights -band $Mask) -ne 0) { $writers += "$($rule.IdentityReference.Value) $($rule.FileSystemRights)" }
         }
         return @{ Protected = $acl.AreAccessRulesProtected; Inherited = $inherited; Broad = $writers
                   Owner = $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value; Sddl = $acl.GetSecurityDescriptorSddlForm('Owner, Access'); Error = '' }
      }
      catch { return @{ Protected = $false; Inherited = 0; Broad = @(); Owner = ''; Sddl = ''; Error = $_.Exception.Message } }
   }
   $folders = @((Join-Path $App 'Bin'), (Join-Path $App 'ControlPanel'))
   $before = @{}
   foreach ($folder in $folders) {
      $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
      if ([System.IO.Directory]::Exists($folder)) { foreach ($entry in [System.IO.Directory]::GetFileSystemEntries($folder)) { [void] $names.Add($entry) } }
      $before[$folder] = $names
   }
   $Sync['AppBefore'] = [System.IO.Directory]::Exists($App)
   $Sync['Ready'] = $true
   $deadline = [DateTime]::UtcNow.AddMinutes(30)
   while (-not $Sync['Stop'] -and [DateTime]::UtcNow -lt $deadline) {
      if (-not $Sync.ContainsKey('AppFirst') -and [System.IO.Directory]::Exists($App)) {
         $Sync['AppFirst'] = Moment $App
      }
      foreach ($folder in $folders) {
         if ($Sync.ContainsKey("First:$folder") -or -not [System.IO.Directory]::Exists($folder)) { continue }
         foreach ($entry in [System.IO.Directory]::GetFileSystemEntries($folder)) {
            if (-not $before[$folder].Contains($entry)) {
               $Sync["First:$folder"] = Moment $folder
               $Sync["AppAt:$folder"] = Moment $App
               $Sync["FirstName:$folder"] = $entry
               break
            }
         }
      }
      [System.Threading.Thread]::Sleep(0)
   }
}

function Start-FolderWatch([string] $App) {
   $sync = [hashtable]::Synchronized(@{ Stop = $false })
   $runspace = [runspacefactory]::CreateRunspace()
   $runspace.Open()
   $shell = [powershell]::Create()
   $shell.Runspace = $runspace
   [void] $shell.AddScript($FolderWatchScript).AddArgument($App).AddArgument($sync).AddArgument($BroadGroupSids).AddArgument([long] $WriteRightsMask)
   $handle = $shell.BeginInvoke()
   [void] (Wait-Until 30 { $sync.ContainsKey('Ready') })
   return [pscustomobject] @{ Shell = $shell; Runspace = $runspace; Handle = $handle; Sync = $sync; App = $App }
}

function Stop-FolderWatch($Watch) {
   $Watch.Sync['Stop'] = $true
   try { [void] $Watch.Shell.EndInvoke($Watch.Handle) } catch { }
   $Watch.Shell.Dispose()
   $Watch.Runspace.Close()
   return $Watch.Sync
}

# Work item 78's R2, the held-open case: a program that opened a folder for writing
# while it let others write keeps that right after setup hardens it, and may make the
# folder, while it is empty, a junction. A runspace of its own opens Folder for writing
# its data (FILE_WRITE_DATA, every sharing, the folder itself), before the installer,
# then waits for Marker - the program folder's Data, which Inno Setup makes ([Dirs])
# after PrepareToInstall and before it copies any file, about 70 ms before Bin's first
# on the VM (check 3 of R1 to R4, 27 October 2026: the uninstaller, the marker before,
# came after Bin's first file, so that the junction was never made) - and makes Folder a
# junction to Target through that handle, holding it until it is stopped. What it did
# is in its Sync.
$HeldOpenTurnScript = {
   param([string] $Folder, [string] $Marker, [string] $Target, [int] $Minutes, $Sync, [string] $DeleteFirst)
   try {
      Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class HmHeldOpenTurn {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inb, int inl, IntPtr outb, int outl, out int ret, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool SetFileInformationByHandle(IntPtr h, int infoClass, ref uint info, int size);
  public static int Delete(IntPtr h) {
    uint posix = 3;
    if (SetFileInformationByHandle(h, 21, ref posix, 4)) return 0;
    uint classic = 1;
    return SetFileInformationByHandle(h, 4, ref classic, 1) ? 0 : Marshal.GetLastWin32Error();
  }
  public static int Turn(IntPtr h, string target) {
    byte[] s = Encoding.Unicode.GetBytes("\\??\\" + target);
    byte[] p = Encoding.Unicode.GetBytes(target);
    int pathBuf = s.Length + 2 + p.Length + 2;
    byte[] b = new byte[16 + pathBuf];
    BitConverter.GetBytes((uint)0xA0000003).CopyTo(b, 0);
    BitConverter.GetBytes((ushort)(8 + pathBuf)).CopyTo(b, 4);
    BitConverter.GetBytes((ushort)s.Length).CopyTo(b, 10);
    BitConverter.GetBytes((ushort)(s.Length + 2)).CopyTo(b, 12);
    BitConverter.GetBytes((ushort)p.Length).CopyTo(b, 14);
    s.CopyTo(b, 16);
    p.CopyTo(b, 16 + s.Length + 2);
    int r;
    return DeviceIoControl(h, 0x000900A4, b, b.Length, IntPtr.Zero, 0, out r, IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
  }
}
'@
      $handle = [HmHeldOpenTurn]::CreateFileW($Folder, 0x2, 7, [IntPtr]::Zero, 3, 0x02200000, [IntPtr]::Zero)
      if ($handle -eq [IntPtr]-1) { $Sync['Error'] = "$Folder could not be opened for writing: Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; $Sync['Ready'] = $true; return }
      # held-full: a file in Folder held open for deleting too, before the installer,
      # deleted through that handle when the marker appears, so that Folder is empty
      # for the junction (check 4 of R1 to R4, 27 October 2026).
      $doomed = [IntPtr]::Zero
      if ($DeleteFirst) {
         $doomed = [HmHeldOpenTurn]::CreateFileW($DeleteFirst, 0x10000, 7, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
         if ($doomed -eq [IntPtr]-1) { $Sync['Error'] = "$DeleteFirst could not be opened for deleting: Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; $Sync['Ready'] = $true; return }
      }
      $Sync['Ready'] = $true
      $deadline = (Get-Date).AddMinutes($Minutes)
      while (-not $Sync['Stop'] -and (Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $Marker)) { [System.Threading.Thread]::Sleep(2) }
      if (Test-Path -LiteralPath $Marker) {
         $Sync['MarkerAt'] = (Get-Date).ToString('HH:mm:ss.fff')
         if ($doomed -ne [IntPtr]::Zero) {
            $deleted = [HmHeldOpenTurn]::Delete($doomed)
            [void] [HmHeldOpenTurn]::CloseHandle($doomed)
            $doomed = [IntPtr]::Zero
            $Sync['Deleted'] = $(if ($deleted -eq 0) { "deleted $DeleteFirst through its handle" } else { "could not delete $DeleteFirst through its handle, Windows error $deleted" })
         }
         $result = [HmHeldOpenTurn]::Turn($handle, $Target)
         $Sync['Turned'] = ($result -eq 0)
         $Sync['TurnResult'] = $(if ($result -eq 0) { 'made a junction' } else { "refused, Windows error $result" })
      }
      else { $Sync['TurnResult'] = "$Marker never appeared" }
      while (-not $Sync['Stop'] -and (Get-Date) -lt $deadline) { [System.Threading.Thread]::Sleep(50) }
      if ($doomed -ne [IntPtr]::Zero) { [void] [HmHeldOpenTurn]::CloseHandle($doomed) }
      [void] [HmHeldOpenTurn]::CloseHandle($handle)
   }
   catch { $Sync['Error'] = "$_"; $Sync['Ready'] = $true }
}

# dac-handle: a handle on Folder opened with the right to change its permissions
# (WRITE_DAC | READ_CONTROL, every sharing, the folder itself) while an entry gave it,
# before the installer, and held until the watch is stopped - what a program another
# account started would keep after any change of the permissions (check 4 of R1 to R4,
# 27 October 2026).
$HeldDacScript = {
   param([string] $Folder, [int] $Minutes, $Sync)
   try {
      Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HmHeldDac {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool CloseHandle(IntPtr h);
}
'@
      $handle = [HmHeldDac]::CreateFileW($Folder, 0x60000, 7, [IntPtr]::Zero, 3, 0x02200000, [IntPtr]::Zero)
      if ($handle -eq [IntPtr]-1) { $Sync['Error'] = "$Folder could not be opened with WRITE_DAC: Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; $Sync['Ready'] = $true; return }
      $Sync['Ready'] = $true
      $deadline = (Get-Date).AddMinutes($Minutes)
      while (-not $Sync['Stop'] -and (Get-Date) -lt $deadline) { [System.Threading.Thread]::Sleep(50) }
      [void] [HmHeldDac]::CloseHandle($handle)
   }
   catch { $Sync['Error'] = "$_"; $Sync['Ready'] = $true }
}

function Start-HeldDac([string] $Folder) {
   $sync = [hashtable]::Synchronized(@{ Stop = $false })
   $runspace = [runspacefactory]::CreateRunspace()
   $runspace.Open()
   $shell = [powershell]::Create()
   $shell.Runspace = $runspace
   [void] $shell.AddScript($HeldDacScript).AddArgument($Folder).AddArgument($TimeoutMinutes * 4).AddArgument($sync)
   $handle = $shell.BeginInvoke()
   [void] (Wait-Until 60 { $sync.ContainsKey('Ready') })
   return [pscustomobject] @{ Shell = $shell; Runspace = $runspace; Handle = $handle; Sync = $sync }
}

# Work item 78 (check 4 of R1 to R4, 27 October 2026): the volatile key in which setup
# records, until Windows restarts, that it is not to install into a program folder
# again before then (RecordRestartNeeded), and what it recorded for one; and the key
# taken away, as a restart takes it away - the stand-in the run uses for a restart of
# the VM, whose own effect on handles is Windows'.
$RestartKey = 'HKLM:\SOFTWARE\hMailServerSetupRestart'
function Get-RestartRecord([string] $App) {
   $item = Get-ItemProperty -LiteralPath $RestartKey -ErrorAction SilentlyContinue
   if ($null -eq $item) { return '' }
   $value = $item.PSObject.Properties | Where-Object { $_.Name -ieq $App } | Select-Object -First 1
   if ($null -eq $value) { return '' }
   return [string] $value.Value
}
function Clear-RestartRecord {
   Remove-Item -LiteralPath $RestartKey -Recurse -Force -ErrorAction SilentlyContinue
   return $(if (Test-Path -LiteralPath $RestartKey) { "$RestartKey is still there" } else { "$RestartKey taken away, as a restart of Windows takes a volatile key away" })
}

function Start-HeldOpenTurn([string] $Folder, [string] $Marker, [string] $Target, [string] $DeleteFirst = '') {
   $sync = [hashtable]::Synchronized(@{ Stop = $false })
   $runspace = [runspacefactory]::CreateRunspace()
   $runspace.Open()
   $shell = [powershell]::Create()
   $shell.Runspace = $runspace
   [void] $shell.AddScript($HeldOpenTurnScript).AddArgument($Folder).AddArgument($Marker).AddArgument($Target).AddArgument($TimeoutMinutes).AddArgument($sync).AddArgument($DeleteFirst)
   $handle = $shell.BeginInvoke()
   [void] (Wait-Until 60 { $sync.ContainsKey('Ready') })
   return [pscustomobject] @{ Shell = $shell; Runspace = $runspace; Handle = $handle; Sync = $sync }
}

# Work item 78's R1: a program folder the installer made had permissions of its own
# from the first moment it was there - no inherited entry, nothing that lets a group
# of every user change it - and Inno Setup did not make it: setup did, in
# PrepareToInstall, and said so in its log.
function Test-ProgramFolderMadeWithItsPermissions([string] $Phase, [string] $Claimed, $Watched, [string] $LogPath) {
   if ($Watched['AppBefore']) { return }
   $name = "${Phase}: the program folder had permissions of its own from the first moment it was there, and setup made it, not Inno Setup (work item 78, R1)"
   $first = $null
   if ($Watched.ContainsKey('AppFirst')) { $first = $Watched['AppFirst'] }
   $found = if ($null -eq $first) { 'the watch never saw it made' } elseif ($first.Error) { "it could not be read when it appeared: $($first.Error)" } else { "first seen: inherits nothing $($first.Protected), $($first.Inherited) inherited entries, owner $($first.Owner), $($first.Sddl)" }
   if (-not (Test-HardensBeforeFiles $Claimed)) { Write-Info $name "$Claimed is not held to it; $found"; return }
   [void] (Test-Step $name {
         if ($null -eq $first) { throw 'the watch never saw the program folder made' }
         if ($first.Error) { throw "it could not be read when it appeared: $($first.Error)" }
         if (-not $first.Protected -or $first.Inherited -gt 0) { throw "when it appeared it inherited from the folder above: $($first.Sddl)" }
         if ($first.Broad.Count -gt 0) { throw "when it appeared a group of every user could change it: $($first.Broad -join '; ')" }
         if ($TrustedOwnerSids -notcontains $first.Owner) { throw "when it appeared it was owned by $($first.Owner)" }
         $app = [regex]::Escape($State.App)
         if (Select-String -LiteralPath $LogPath -Pattern "Creating directory: $app\s*$" -Quiet) { throw "Inno's log says Inno Setup made it ('Creating directory: $($State.App)'), with what the folder above hands down" }
         if (-not (Select-String -LiteralPath $LogPath -Pattern "Setup made $app with permissions of its own" -Quiet)) { throw "Inno's log does not say that setup made it with permissions of its own" }
         $found
      })
}

# Work item 78's R2: Bin and ControlPanel, there before the installer, were hardened
# before their files were copied - inheriting nothing, no group of every user able to
# change them, a trusted owner, at the moment the first new file appeared in each - and
# the setup log says each was hardened before the first file it copied there; and the
# program folder too, when ProgramFolderToo says so (its data folders inherited
# nothing from it before the installer, or it held no server installation). Remade
# names those of There that held nothing before the installer, which setup removes and
# makes again rather than hardens (RemakeEmptyFolder): for each the setup log must say,
# before the first file it copies there, that setup removed it and made it again with
# permissions of its own - a hardening in place is not enough, since a program that
# opened it before keeps what it opened it for (check 3 of R1 to R4, 27 October 2026).
function Test-FoldersHardenedBeforeTheFiles([string] $Phase, [string] $Claimed, $Watched, [string] $LogPath, [string[]] $There, [bool] $ProgramFolderToo, [string[]] $Remade = @()) {
   if ($There.Count -eq 0) { return }
   # An empty list may reach here as nothing at all (PowerShell 5.1's StrictMode 2.0 then
   # has no Count for it, as smoke run 2 found for Missing).
   $Remade = @($Remade | Where-Object { $_ })
   $name = "${Phase}: $(($There | ForEach-Object { Split-Path -Leaf $_ }) -join ' and '), there before the installer, were $(if ($Remade.Count -gt 0) { "hardened or, held nothing ($(($Remade | ForEach-Object { Split-Path -Leaf $_ }) -join ' and ')), made again" } else { 'hardened' }) before their files were copied$(if ($ProgramFolderToo) { ', and the program folder with them' }) (work item 78, R2)"
   $said = @()
   foreach ($folder in $There) {
      $moment = $null
      if ($Watched.ContainsKey("First:$folder")) { $moment = $Watched["First:$folder"] }
      $said += $(if ($null -eq $moment) { "$folder no new file seen" } else { "$folder at $(Split-Path -Leaf $Watched["FirstName:$folder"]): inherits nothing $($moment.Protected), broad writers $($moment.Broad.Count), owner $($moment.Owner)" })
   }
   if (-not (Test-HardensBeforeFiles $Claimed)) { Write-Info $name "$Claimed is not held to it; $($said -join '; ')"; return }
   [void] (Test-Step $name {
         $lines = @(Get-Content -LiteralPath $LogPath)
         foreach ($folder in $There) {
            if (-not $Watched.ContainsKey("First:$folder")) { throw "the watch saw no file copied into $folder" }
            $moment = $Watched["First:$folder"]
            if ($moment.Error) { throw "$folder could not be read when its first file appeared: $($moment.Error)" }
            if (-not $moment.Protected) { throw "$folder still inherited from the folder above when $($Watched["FirstName:$folder"]) appeared in it: $($moment.Sddl)" }
            if ($moment.Broad.Count -gt 0) { throw "a group of every user could change $folder when its first file appeared: $($moment.Broad -join '; ')" }
            if ($TrustedOwnerSids -notcontains $moment.Owner) { throw "$folder was owned by $($moment.Owner) when its first file appeared" }
            $escaped = [regex]::Escape($folder)
            $hardened = -1
            $copied = -1
            $removed = -1
            $remadeAt = -1
            for ($index = 0; $index -lt $lines.Count; $index++) {
               # Hardened, rebuilt, or - one that held nothing - removed and made again
               # by setup (RemakeEmptyFolder).
               if ($hardened -lt 0 -and $lines[$index] -match "(icacls hardened $escaped before the files|$escaped was owned by an account other than SYSTEM|Setup made $escaped with permissions of its own)") { $hardened = $index }
               if ($removed -lt 0 -and $lines[$index] -match "Setup removed $escaped, which was there and held nothing") { $removed = $index }
               if ($removed -ge 0 -and $remadeAt -lt 0 -and $index -gt $removed -and $lines[$index] -match "Setup made $escaped with permissions of its own") { $remadeAt = $index }
               if ($copied -lt 0 -and $lines[$index] -match "Dest filename: $escaped\\") { $copied = $index }
            }
            if ($Remade -contains $folder) {
               if ($removed -lt 0) { throw "$folder held nothing before the installer, and Inno's log does not say setup removed it to make it again ('Setup removed $folder, which was there and held nothing')" }
               if ($remadeAt -lt 0) { throw "Inno's log says setup removed $folder (line $($removed + 1)) and not that it made it again" }
               if ($copied -ge 0 -and $copied -lt $remadeAt) { throw "Inno's log copies a file into $folder (line $($copied + 1)) before setup made it again (line $($remadeAt + 1))" }
               $hardened = $remadeAt
            }
            if ($hardened -lt 0) { throw "Inno's log does not say $folder was hardened before the files" }
            if ($copied -ge 0 -and $copied -lt $hardened) { throw "Inno's log copies a file into $folder (line $($copied + 1)) before it hardens it (line $($hardened + 1))" }
            if ($ProgramFolderToo) {
               $app = $Watched["AppAt:$folder"]
               if (-not $app.Protected -or $app.Broad.Count -gt 0) { throw "the program folder still inherited, or let a group of every user change it, when the first file appeared in ${folder}: $($app.Sddl)" }
            }
         }
         $said -join '; '
      })
}

# Work item 78's R1 and R2: a Bin and a ControlPanel that were not there in a program
# folder that was - one made before the installer - made by setup with permissions of
# their own before Inno Setup copied anything into them: at the moment the first file
# appeared in each, inheriting nothing, no group of every user able to change it, a
# trusted owner; and the setup log makes each before the first file it copies there.
# Missing names the folders the installation installs into that were not there.
function Test-FoldersMadeBeforeTheFiles([string] $Phase, [string] $Claimed, $Watched, [string] $LogPath, [string[]] $Missing) {
   if (-not $Missing) { return }
   $name = "${Phase}: $(($Missing | ForEach-Object { Split-Path -Leaf $_ }) -join ' and '), not there in the program folder that was, were made by setup with permissions of their own before their files (work item 78, R1 and R2)"
   $said = @()
   foreach ($folder in $Missing) {
      $moment = $null
      if ($Watched.ContainsKey("First:$folder")) { $moment = $Watched["First:$folder"] }
      $said += $(if ($null -eq $moment) { "$folder no new file seen" } else { "$folder at $(Split-Path -Leaf $Watched["FirstName:$folder"]): inherits nothing $($moment.Protected), $($moment.Inherited) inherited, broad writers $($moment.Broad.Count), owner $($moment.Owner)" })
   }
   if (-not (Test-HardensBeforeFiles $Claimed)) { Write-Info $name "$Claimed is not held to it; $($said -join '; ')"; return }
   [void] (Test-Step $name {
         $lines = @(Get-Content -LiteralPath $LogPath)
         foreach ($folder in $Missing) {
            if (-not $Watched.ContainsKey("First:$folder")) { throw "the watch saw no file copied into $folder" }
            $moment = $Watched["First:$folder"]
            if ($moment.Error) { throw "$folder could not be read when its first file appeared: $($moment.Error)" }
            if (-not $moment.Protected -or $moment.Inherited -gt 0) { throw "$folder inherited from the program folder when $($Watched["FirstName:$folder"]) appeared in it: $($moment.Sddl)" }
            if ($moment.Broad.Count -gt 0) { throw "a group of every user could change $folder when its first file appeared: $($moment.Broad -join '; ')" }
            if ($TrustedOwnerSids -notcontains $moment.Owner) { throw "$folder was owned by $($moment.Owner) when its first file appeared" }
            $escaped = [regex]::Escape($folder)
            # Setup names a folder it made by the path it made it at: where a link the
            # administrators control leads, when the program folder is one (link-admin-only).
            $madeAt = @($escaped)
            $above = Get-Item -LiteralPath (Split-Path -Parent $folder) -Force
            if ($above.LinkType) { $madeAt += [regex]::Escape((Join-Path (@($above.Target)[0]) (Split-Path -Leaf $folder))) }
            $made = -1
            $copied = -1
            for ($index = 0; $index -lt $lines.Count; $index++) {
               if ($made -lt 0 -and $lines[$index] -match "Setup made ($($madeAt -join '|')) with permissions of its own") { $made = $index }
               if ($copied -lt 0 -and $lines[$index] -match "Dest filename: $escaped\\") { $copied = $index }
            }
            if ($made -lt 0) { throw "Inno's log does not say setup made $folder" }
            if ($copied -ge 0 -and $copied -lt $made) { throw "Inno's log copies a file into $folder (line $($copied + 1)) before setup makes it (line $($made + 1))" }
         }
         $said -join '; '
      })
}

# Bin, and ControlPanel when the installation installs it, that are not there in a
# program folder that is: the folders setup is to make before the files.
function Get-FoldersToMake([string] $App, [string] $ComponentList) {
   if (-not (Test-Path -LiteralPath $App)) { return @() }
   $names = @('Bin')
   if (-not $ComponentList -or (@($ComponentList -split ',' | ForEach-Object { $_.Trim() }) -contains 'controlpanel')) { $names += 'ControlPanel' }
   return @($names | ForEach-Object { Join-Path $App $_ } | Where-Object { -not (Test-Path -LiteralPath $_) })
}

# Those of Folders that are there and hold nothing: what an installer after 6.3.7
# removes and makes again before the files rather than hardens (RemakeEmptyFolder).
function Get-FoldersThatHoldNothing([string[]] $Folders) {
   return @($Folders | Where-Object { (Test-Path -LiteralPath $_) -and @(Get-ChildItem -LiteralPath $_ -Force -ErrorAction SilentlyContinue).Count -eq 0 })
}

# What is in the program folder, every file and folder, as full paths: what a refused
# installer is held to leave as it was.
function Get-ProgramFolderItems([string] $App) {
   if (-not (Test-Path -LiteralPath $App)) { return @() }
   return @(Get-ChildItem -LiteralPath $App -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName } | Sort-Object)
}

# Whether Data, Logs, Temp, Database and Events, where they are there, inherit nothing
# from the program folder: what an installer after 6.3.7 asks before it hardens the
# program folder of a server installation before the files.
function Test-DataFoldersInheritNothing([string] $App) {
   foreach ($name in 'Data', 'Logs', 'Temp', 'Database', 'Events') {
      $folder = Join-Path $App $name
      if ((Test-Path -LiteralPath $folder) -and -not (Get-Acl -LiteralPath $folder).AreAccessRulesProtected) { return $false }
   }
   return $true
}

# ---- the folder cases (-FolderCase): what the program folder is before the first
# installer, made here as an administrator. A standard account's folder, file or
# junction is made by the administrator and given to the account - its owner, with
# Full control of its own - since a junction made as that account over SSH fails
# (0xC0000142, 6 October 2026). The account, hmsmokeuser, is made for the run with a
# password made here and never printed; the VM is reverted after the run.
$SmokeAccountName = 'hmsmokeuser'
$LinkTarget = 'C:\hm-linktarget'

function New-SmokeAccount {
   if ($State.ContainsKey('SmokeAccountSid') -and $State.SmokeAccountSid) { return $State.SmokeAccountSid }
   $password = 'Hm' + [Guid]::NewGuid().ToString('N').Substring(0, 18) + '!a1'
   $answer = Invoke-Native "net.exe user $SmokeAccountName $password /add /y"
   if ($answer.ExitCode -ne 0) { throw "net user $SmokeAccountName /add returned $($answer.ExitCode)" }
   $State.SmokeAccountSid = (New-Object System.Security.Principal.NTAccount($SmokeAccountName)).Translate([System.Security.Principal.SecurityIdentifier]).Value
   return $State.SmokeAccountSid
}

# Path given to the standard account: its owner, with Full control of its own - of
# the link itself with -Link (icacls /L).
function Set-OwnedByAccount([string] $Path, [switch] $Link) {
   $sid = New-SmokeAccount
   $l = @()
   if ($Link) { $l = @('/L') }
   $grant = if ($Link) { "*${sid}:F" } elseif ((Get-Item -LiteralPath $Path -Force).PSIsContainer) { "*${sid}:(OI)(CI)F" } else { "*${sid}:F" }
   $answer = @(& icacls.exe $Path /setowner "*$sid" @l 2>&1)
   if ($LASTEXITCODE -ne 0) { throw "icacls /setowner on $Path returned ${LASTEXITCODE}: $($answer -join ' ')" }
   $answer = @(& icacls.exe $Path /grant $grant @l 2>&1)
   if ($LASTEXITCODE -ne 0) { throw "icacls /grant on $Path returned ${LASTEXITCODE}: $($answer -join ' ')" }
}

# A folder given to the standard account with a list of its own that leaves SYSTEM and
# the Administrators what an installation needs and no right to change its owner or
# its permissions: the account Full control, SYSTEM and the Administrators Read &
# execute and write on the folder and its subfolders, and Full control of the files
# made in it - so that setup can take it only with the Administrators' privilege.
function Set-LockedByAccount([string] $Path) {
   $sid = New-SmokeAccount
   $answer = @(& icacls.exe $Path /setowner "*$sid" 2>&1)
   if ($LASTEXITCODE -ne 0) { throw "icacls /setowner on $Path returned ${LASTEXITCODE}: $($answer -join ' ')" }
   $answer = @(& icacls.exe $Path /inheritance:r /grant:r "*${sid}:(OI)(CI)F" '*S-1-5-18:(CI)(RX,W)' '*S-1-5-32-544:(CI)(RX,W)' '*S-1-5-18:(OI)(IO)F' '*S-1-5-32-544:(OI)(IO)F' 2>&1)
   if ($LASTEXITCODE -ne 0) { throw "icacls /inheritance:r /grant:r on $Path returned ${LASTEXITCODE}: $($answer -join ' ')" }
}

function New-LinkTarget([string] $Target) {
   New-Item -ItemType Directory -Force -Path $Target | Out-Null
   $answer = @(& icacls.exe $Target /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' 2>&1)
   if ($LASTEXITCODE -ne 0) { throw "icacls on $Target returned ${LASTEXITCODE}: $($answer -join ' ')" }
}

function New-Junction([string] $Link, [string] $Target) {
   $answer = Invoke-Native "mklink /J `"$Link`" `"$Target`""
   if ($answer.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Link)) { throw "mklink /J $Link returned $($answer.ExitCode): $($answer.Output)" }
}

function Get-LinkList([string] $Link) {
   return ((@(& icacls.exe $Link /L 2>&1) | ForEach-Object { "$_".Trim() } | Where-Object { $_ -and $_ -notmatch '^Successfully processed' }) -join ' | ')
}

# The fixture of each case, before the first installer.
function New-FolderCase {
   $app = $State.App
   switch ($FolderCase) {
      'precreated-admin' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         "$app and $app\Bin made by the administrator, inheriting from the folder above"
      }
      'precreated-user' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         Set-OwnedByAccount $app
         Set-OwnedByAccount (Join-Path $app 'Bin')
         Set-Content -LiteralPath (Join-Path $app 'Bin\notes.txt') -Value 'placed before the installation' -Encoding ASCII
         Set-OwnedByAccount (Join-Path $app 'Bin\notes.txt')
         $State.CaseFiles = @('Bin\notes.txt')
         "$app, $app\Bin and $app\Bin\notes.txt owned by $SmokeAccountName ($($State.SmokeAccountSid)) with Full control of its own"
      }
      'precreated-user-locked' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         Set-LockedByAccount (Join-Path $app 'Bin')
         "$app made by the administrator; $app\Bin owned by $SmokeAccountName ($($State.SmokeAccountSid)), its list its own: $(Get-LinkList (Join-Path $app 'Bin'))"
      }
      'earlier-user-locked' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         Set-LockedByAccount (Join-Path $app 'Bin')
         # An earlier installation's Bin holds its settings file: a Bin that holds
         # nothing setup makes again rather than takes (RemakeEmptyFolder).
         Set-Content -LiteralPath (Join-Path $app 'Bin\hMailServer.INI') -Value '[Directories]' -Encoding ASCII
         # The earlier installation's folder as section_registry.iss records it, in the
         # 32-bit view (PreviousInstallationFolder): the VM is reverted after the run.
         New-Item -Path 'HKLM:\SOFTWARE\WOW6432Node\hMailServer' -Force | Out-Null
         Set-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\hMailServer' -Name 'InstallLocation' -Value $app
         "$app recorded as the earlier installation's (HKLM\SOFTWARE\WOW6432Node\hMailServer InstallLocation); $app\Bin owned by $SmokeAccountName, its list its own: $(Get-LinkList (Join-Path $app 'Bin'))"
      }
      'data-inherits' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Data') | Out-Null
         "$app, $app\Bin and $app\Data made by the administrator, inheriting from the folder above; no ControlPanel"
      }
      'foreign-dll' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         Set-Content -LiteralPath (Join-Path $app 'Bin\version.dll') -Value 'placed before the installation' -Encoding ASCII
         Set-OwnedByAccount (Join-Path $app 'Bin\version.dll')
         $State.CaseFiles = @('Bin\version.dll')
         "$app and $app\Bin made by the administrator, inheriting from the folder above; $app\Bin\version.dll owned by $SmokeAccountName"
      }
      'broad-entry' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         $answer = @(& icacls.exe $app /grant '*S-1-5-11:(OI)(CI)M' 2>&1)
         if ($LASTEXITCODE -ne 0) { throw "icacls /grant on $app returned ${LASTEXITCODE}: $($answer -join ' ')" }
         Set-Content -LiteralPath (Join-Path $app 'Bin\notes.txt') -Value 'placed before the installation' -Encoding ASCII
         Set-OwnedByAccount (Join-Path $app 'Bin\notes.txt')
         $State.CaseFiles = @('Bin\notes.txt')
         "$app made by the administrator with an entry of its own giving Authenticated Users Modify; $app\Bin\notes.txt owned by $SmokeAccountName"
      }
      'link-user' {
         New-LinkTarget $LinkTarget
         New-Junction $app $LinkTarget
         Set-OwnedByAccount $app -Link
         "$app a junction to $LinkTarget, owned by $SmokeAccountName with Full control of the link itself: $(Get-LinkList $app)"
      }
      'link-admin' {
         New-LinkTarget $LinkTarget
         New-Junction $app $LinkTarget
         "$app a junction to $LinkTarget made by the administrator, its list as the folder above handed it: $(Get-LinkList $app)"
      }
      'link-admin-only' {
         New-LinkTarget $LinkTarget
         New-Junction $app $LinkTarget
         foreach ($arguments in @(@($app, '/L', '/setowner', '*S-1-5-32-544'), @($app, '/L', '/inheritance:r', '/grant:r', '*S-1-5-18:F', '*S-1-5-32-544:F', '*S-1-5-32-545:RX'))) {
            $answer = @(& icacls.exe @arguments 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "icacls $($arguments -join ' ') returned ${LASTEXITCODE}: $($answer -join ' ')" }
         }
         "$app a junction to $LinkTarget made by the administrator and given the list the refusal's remedy gives it: $(Get-LinkList $app)"
      }
      'bin-link-user' {
         New-Item -ItemType Directory -Force -Path $app | Out-Null
         New-LinkTarget $LinkTarget
         New-Junction (Join-Path $app 'Bin') $LinkTarget
         Set-OwnedByAccount (Join-Path $app 'Bin') -Link
         "$app made by the administrator, $app\Bin a junction to $LinkTarget owned by ${SmokeAccountName}: $(Get-LinkList (Join-Path $app 'Bin'))"
      }
      'held-open' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         New-LinkTarget $LinkTarget
         "$app and $app\Bin made by the administrator, inheriting from the folder above; Bin to be held open for writing before the installer, and made a junction through that handle when $app\Data appears"
      }
      'settings-user' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         Set-Content -LiteralPath (Join-Path $app 'Bin\hMailServer.INI') -Value '[Directories]' -Encoding ASCII
         Set-OwnedByAccount (Join-Path $app 'Bin\hMailServer.INI')
         $State.CaseFiles = @('Bin\hMailServer.INI')
         "$app and $app\Bin made by the administrator, inheriting from the folder above; $app\Bin\hMailServer.INI owned by $SmokeAccountName"
      }
      'events-link' {
         New-Item -ItemType Directory -Force -Path $app | Out-Null
         New-LinkTarget $LinkTarget
         $events = Join-Path $app 'Events'
         New-Junction $events $LinkTarget
         # The link itself the administrators' alone, as a link's remedy makes it: the
         # rule refused is that of the folders setup installs into, not that of a link.
         foreach ($arguments in @(@($events, '/L', '/setowner', '*S-1-5-32-544'), @($events, '/L', '/inheritance:r', '/grant:r', '*S-1-5-18:F', '*S-1-5-32-544:F', '*S-1-5-32-545:RX'))) {
            $answer = @(& icacls.exe @arguments 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "icacls $($arguments -join ' ') returned ${LASTEXITCODE}: $($answer -join ' ')" }
         }
         "$app made by the administrator, $events a junction to $LinkTarget only the administrators control: $(Get-LinkList $events)"
      }
      'volume-root' {
         # A volume of its own - a VHD made and attached by diskpart, formatted NTFS -
         # mounted on the program folder's path, so that the program folder is the root
         # of a volume reached through a mount point the administrators alone control.
         $vhd = 'C:\hm-volume.vhdx'
         New-Item -ItemType Directory -Force -Path $app | Out-Null
         $script = Join-Path $env:TEMP 'hm-volume-diskpart.txt'
         Set-Content -LiteralPath $script -Encoding ASCII -Value @("create vdisk file=`"$vhd`" maximum=64 type=expandable", "select vdisk file=`"$vhd`"", 'attach vdisk',
                                                                    'create partition primary', 'format fs=ntfs quick label=hmvolume', "assign mount=`"$app`"")
         $answer = Invoke-Native "diskpart.exe /s `"$script`""
         if ($answer.ExitCode -ne 0) { throw "diskpart /s returned $($answer.ExitCode): $($answer.Output)" }
         if (((Get-Item -LiteralPath $app -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) { throw "$app is not a mount point after diskpart: $($answer.Output)" }
         foreach ($arguments in @(@($app, '/L', '/setowner', '*S-1-5-32-544'), @($app, '/L', '/inheritance:r', '/grant:r', '*S-1-5-18:F', '*S-1-5-32-544:F', '*S-1-5-32-545:RX'))) {
            $answer = @(& icacls.exe @arguments 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "icacls $($arguments -join ' ') returned ${LASTEXITCODE}: $($answer -join ' ')" }
         }
         "$app a mount point of a volume of its own ($vhd, NTFS), its list the administrators' alone: $(Get-LinkList $app)"
      }
      { $_ -in @('weak-above', 'weak-above-remedied') } {
         # A folder an administrator made in the root of a Windows client's drive, which
         # hands Authenticated Users Modify down to what is made in it: the folder above
         # the program folder, which that group may then rename or empty.
         $above = Split-Path -Parent $app
         $root = (Split-Path -Qualifier $app) + '\'
         if ((Split-Path -Parent $above) -ne $root) { throw "-InstallDirectory $app is not two folders below the root of its drive: $FolderCase makes the one between" }
         $answer = @(& icacls.exe $root /grant '*S-1-5-11:(OI)(CI)(IO)M' 2>&1)
         if ($LASTEXITCODE -ne 0) { throw "icacls /grant on $root returned ${LASTEXITCODE}: $($answer -join ' ')" }
         New-Item -ItemType Directory -Force -Path $above | Out-Null
         $writers = @(Get-BroadWriters $above)
         if ($writers.Count -eq 0) { throw "$above, made in $root, does not let Authenticated Users change it" }
         $State.AboveMade = $above
         if ($FolderCase -eq 'weak-above') { return "$above made by the administrator in $root, which hands down Authenticated Users' Modify: $($writers -join ', ')" }
         # The refusal's remedy, as it gives it.
         foreach ($arguments in @(@($above, '/inheritance:d'), @($above, '/remove:g', '*S-1-5-11'))) {
            $answer = @(& icacls.exe @arguments 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "icacls $($arguments -join ' ') returned ${LASTEXITCODE}: $($answer -join ' ')" }
         }
         $left = @(Get-BroadReplacers $above)
         if ($left.Count -gt 0) { throw "$above still lets a group of every user rename it, remove what is in it or change who may after the remedy: $($left -join ', ')" }
         "$above made by the administrator in $root, which hands down Authenticated Users' Modify, and given the refusal's remedy (icacls /inheritance:d, then /remove:g *S-1-5-11): $((@(& icacls.exe $above) | Where-Object { $_.Trim() -and $_ -notmatch '^Successfully processed' }) -join ' | ')"
      }
      'dac-handle' {
         New-Item -ItemType Directory -Force -Path $app | Out-Null
         $answer = @(& icacls.exe $app /grant '*S-1-1-0:(OI)(CI)F' 2>&1)
         if ($LASTEXITCODE -ne 0) { throw "icacls /grant on $app returned ${LASTEXITCODE}: $($answer -join ' ')" }
         "$app made by the administrator with an entry of its own giving Everyone Full control, to be held open with the right to change its permissions before the installer"
      }
      'held-full' {
         New-Item -ItemType Directory -Force -Path (Join-Path $app 'Bin') | Out-Null
         Set-Content -LiteralPath (Join-Path $app 'Bin\notes.txt') -Value 'placed before the installation' -Encoding ASCII
         New-LinkTarget $LinkTarget
         $State.CaseFiles = @('Bin\notes.txt')
         "$app, $app\Bin and $app\Bin\notes.txt made by the administrator, inheriting from the folder above; Bin held open for writing and notes.txt for deleting before the installer, the file deleted and Bin made a junction to $LinkTarget through those handles the moment $app\Data appears"
      }
      'copy-fails' {
         # A volume of its own, too small for the installation, mounted on the folder above
         # the program folder: setup makes the program folder, Bin and ControlPanel there,
         # the copy fails for want of space, and nothing setup made is to be left.
         $mount = Split-Path -Parent $app
         if ((Split-Path -Parent $mount) -ne ((Split-Path -Qualifier $app) + '\')) { throw "-InstallDirectory $app is not two folders below the root of its drive: copy-fails mounts a small volume on the one between" }
         $vhd = 'C:\hm-small.vhdx'
         New-Item -ItemType Directory -Force -Path $mount | Out-Null
         $script = Join-Path $env:TEMP 'hm-small-diskpart.txt'
         Set-Content -LiteralPath $script -Encoding ASCII -Value @("create vdisk file=`"$vhd`" maximum=64 type=expandable", "select vdisk file=`"$vhd`"", 'attach vdisk',
                                                                    'create partition primary', 'format fs=ntfs quick label=hmsmall', "assign mount=`"$mount`"")
         $answer = Invoke-Native "diskpart.exe /s `"$script`""
         if ($answer.ExitCode -ne 0) { throw "diskpart /s returned $($answer.ExitCode): $($answer.Output)" }
         if (((Get-Item -LiteralPath $mount -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) { throw "$mount is not a mount point after diskpart: $($answer.Output)" }
         foreach ($arguments in @(@($mount, '/L', '/setowner', '*S-1-5-32-544'), @($mount, '/L', '/inheritance:r', '/grant:r', '*S-1-5-18:F', '*S-1-5-32-544:F', '*S-1-5-32-545:RX'))) {
            $answer = @(& icacls.exe @arguments 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "icacls $($arguments -join ' ') returned ${LASTEXITCODE}: $($answer -join ' ')" }
         }
         $State.SmallVolume = $mount
         "$mount a mount point of a 64 MB volume of its own ($vhd, NTFS), its list the administrators' alone; $app to be made on it, where the files do not fit"
      }
      'nested-new' {
         $top = $app
         while ((Split-Path -Parent $top) -and -not (Test-Path -LiteralPath (Split-Path -Parent $top))) { $top = Split-Path -Parent $top }
         if ($top -eq $app) { throw "-InstallDirectory $app has a folder above it that is there: nested-new wants two or more folders made" }
         $State.TopMade = $top
         "nothing made: setup makes $app and the folders above it from $top"
      }
   }
}

# Runs an installer and answers its exit code, bounded in time as Invoke-Setup is.
function Invoke-SetupForExitCode([string] $Path, [string[]] $Arguments, [string] $LogPath) {
   Write-Line "   $(Split-Path -Leaf $Path) $(($Arguments | ForEach-Object { if ($_ -like '/adminpassword=*') { '/adminpassword=(hidden)' } else { $_ } }) -join ' ')"
   $process = Start-Process -FilePath $Path -ArgumentList $Arguments -PassThru
   $null = $process.Handle
   if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
      try { $process.Kill() } catch { }
      Write-LogTail $LogPath
      throw "did not finish within $TimeoutMinutes minutes and was stopped"
   }
   return $process.ExitCode
}

# The cases a whole server installation does not run: the Control Panel alone
# (-Components), and the folder cases whose installer is refused, or whose program
# folder is kept as it was, or which check what the uninstall leaves. One installer,
# its exit code held to the case, what it wrote checked, and an uninstall when
# anything was installed.
$FocusedCases = @('precreated-user', 'precreated-user-locked', 'earlier-user-locked', 'data-inherits', 'foreign-dll', 'broad-entry', 'link-user', 'link-admin', 'link-admin-only', 'bin-link-user', 'nested-new', 'held-open',
                  'settings-user', 'events-link', 'volume-root', 'weak-above', 'weak-above-remedied', 'dac-handle', 'held-full', 'copy-fails')
# The cases whose folders another account could change before the installer - their
# permissions, or what is in one that holds something - which setup hardens and then
# refuses until Windows has restarted (work item 78, check 4 of R1 to R4, 27 October
# 2026): the installer is run three times, refused, refused again by what setup
# recorded, and, the record taken away as a restart takes it away and every handle the
# case held closed, run to the end.
$RestartCases = @('broad-entry', 'earlier-user-locked', 'dac-handle', 'held-full')
# What a restart case's refusal names, and the folders setup is to have hardened by then.
function Get-RestartPattern {
   $bin = Join-Path $State.App 'Bin'
   switch ($FolderCase) {
      'broad-entry' { return @(([regex]::Escape($State.App) + ' let [^;]*S-1-5-11[^;]* change what is in it'), @($State.App, $bin)) }
      'earlier-user-locked' { return @(([regex]::Escape($bin) + ' was owned by [^;]*' + [regex]::Escape($State.SmokeAccountSid)), @($bin)) }
      'dac-handle' { return @(([regex]::Escape($State.App) + ' let [^;]*S-1-1-0[^;]* change its permissions or its owner'), @($State.App)) }
      'held-full' { return @(([regex]::Escape($bin) + ' let [^;]*S-1-5-11[^;]* change what is in it'), @($State.App, $bin)) }
   }
}
$RefusedCases = @('precreated-user', 'precreated-user-locked', 'foreign-dll', 'link-user', 'link-admin', 'bin-link-user', 'settings-user', 'events-link', 'volume-root', 'weak-above')
$LinkCases = @('link-user', 'link-admin', 'bin-link-user', 'events-link')
# Which of work item 78's R1 to R4 a refused case is for.
function Get-CaseItem {
   if ($FolderCase -in @('link-user', 'link-admin', 'bin-link-user', 'weak-above', 'dac-handle')) { return 'R3' }
   if ($FolderCase -in @('foreign-dll', 'settings-user', 'events-link')) { return 'R4' }
   return 'R2'
}
# What a refused case's refusal says, and the remedy it gives (Inno's log, "Refused:").
function Get-RefusalPattern {
   switch ($FolderCase) {
      'precreated-user' { return @(('was made by .*' + [regex]::Escape($State.SmokeAccountSid) + '.*, who may still hold it open'), 'remove or rename it') }
      'precreated-user-locked' { return @(('Bin was made by .*' + [regex]::Escape($State.SmokeAccountSid) + '.*, who may still hold it open'), 'remove or rename it') }
      'foreign-dll' { return @(('would run programs there that another account controls: .*' + [regex]::Escape('Bin\version.dll') + ' is owned by .*' + [regex]::Escape($State.SmokeAccountSid)), '/setowner \*S-1-5-32-544') }
      'settings-user' { return @(('would run programs there that another account controls: .*' + [regex]::Escape('Bin\hMailServer.INI') + ' is owned by .*' + [regex]::Escape($State.SmokeAccountSid)), '/setowner \*S-1-5-32-544') }
      'events-link' { return @(([regex]::Escape((Join-Path $State.App 'Events')) + ' is a junction, a symbolic link or a mount point, and setup installs into it what the service or the administrators run'),
                               ('rmdir "' + [regex]::Escape((Join-Path $State.App 'Events')) + '"')) }
      'volume-root' { return @('it is the root of a volume \(\\\\\?\\Volume\{', 'Install hMailServer into a folder on that volume') }
      'weak-above' { return @(([regex]::Escape($State.AboveMade) + ', a folder on the way to it, lets .* rename it, remove what is in it or change who may'),
                              [regex]::Escape("icacls `"$($State.AboveMade)`" /inheritance:d, then icacls `"$($State.AboveMade)`" /remove:g *S-1-5-11")) }
      default { return @('is a junction, a symbolic link or a mount point, and ', '/L /setowner \*S-1-5-32-544') }
   }
}
function Test-IsFocusedRun { return [bool] $Components -or ($FocusedCases -contains $FolderCase) }

function Invoke-FocusedRun([string] $Version) {
   $app = $State.App
   $log = Join-Path $LogDirectory 'install.log'
   $refused = $RefusedCases -contains $FolderCase
   $there = @(@((Join-Path $app 'Bin'), (Join-Path $app 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ })
   $remade = @(Get-FoldersThatHoldNothing $there)
   $missing = @(Get-FoldersToMake $app $Components)
   $appBefore = Test-Path -LiteralPath $app
   $itemsBefore = @(Get-ProgramFolderItems $app)
   $State.ProgramFoldersReadable = Get-ProgramFoldersReadable $app
   $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$log", "/DIR=$app")
   if ($Components) { $arguments += "/COMPONENTS=$Components" } else { $arguments += '/TYPE=full' }
   if (-not $State.PasswordInEnvironment) { $arguments += "/adminpassword=$AdminPassword" }
   $turn = $null
   $dac = $null
   if ($FolderCase -eq 'held-open') {
      $turn = Start-HeldOpenTurn (Join-Path $app 'Bin') (Join-Path $app 'Data') $LinkTarget
      if ($turn.Sync['Error']) { Write-Result 'FAIL' "case ${FolderCase}: Bin held open before the installer" $turn.Sync['Error'] }
   }
   if ($FolderCase -eq 'held-full') {
      $turn = Start-HeldOpenTurn (Join-Path $app 'Bin') (Join-Path $app 'Data') $LinkTarget (Join-Path $app 'Bin\notes.txt')
      if ($turn.Sync['Error']) { Write-Result 'FAIL' "case ${FolderCase}: Bin and Bin\notes.txt held open before the installer" $turn.Sync['Error'] }
   }
   if ($FolderCase -eq 'dac-handle') {
      $dac = Start-HeldDac $app
      if ($dac.Sync['Error']) { Write-Result 'FAIL' "case ${FolderCase}: the program folder held open with WRITE_DAC before the installer" $dac.Sync['Error'] }
   }
   $watch = Start-FolderWatch $app
   $code = -1
   try { $code = Invoke-WithAdminPassword { Invoke-SetupForExitCode $Installer $arguments $log } }
   finally {
      $watched = Stop-FolderWatch $watch
      if ($turn -and -not ($RestartCases -contains $FolderCase)) { $turned = Stop-FolderWatch $turn }
   }

   # Work item 78 (check 4 of R1 to R4, 27 October 2026): a folder another account could
   # change - its permissions, or what is in one that holds something - hardened by setup
   # and the installation refused until Windows has restarted; a second run before then
   # refused by what setup recorded; and, every handle the case held closed and the
   # volatile key taken away, as a restart closes and takes them, the installation run
   # once more and held to the case's checks.
   if ($RestartCases -contains $FolderCase) {
      $restart = Get-RestartPattern
      $State.FirstLog = $log
      [void] (Test-Step "case ${FolderCase}: setup hardened what another account could change before the installer and refused until Windows has restarted - exit code 7, PrepareToInstall's (work item 78, $(Get-CaseItem))" {
            if ($code -ne 7) { throw "it returned $code" }
            $line = Select-String -LiteralPath $log -Pattern ('Refused: Setup does not install into ' + [regex]::Escape($app) + ' until Windows has restarted: before this installation, .*' + $restart[0]) | Select-Object -First 1
            if ($null -eq $line) { throw "Inno's log does not refuse until a restart, naming what another account could change (/$($restart[0])/)" }
            if ($line.Line -notmatch 'Restart Windows, and run setup again') { throw 'the refusal does not say to restart Windows' }
            if ($line.Line -match 'Setup stopped the hMailServer service') { throw 'the refusal says setup stopped the service: it refuses before the stop' }
            if (-not (Get-RestartRecord $app)) { throw "$RestartKey does not record $app" }
            if (Test-Path -LiteralPath (Join-Path $app 'Bin\hMailServer.exe')) { throw "$app\Bin\hMailServer.exe is there: something was installed" }
            $said = @()
            foreach ($folder in $restart[1]) {
               $moment = Get-FolderMoment $folder
               if ($moment.Error) { throw "the permissions of $folder could not be read: $($moment.Error)" }
               if (-not $moment.Protected -or $moment.Broad.Count -gt 0 -or $TrustedOwnerSids -notcontains $moment.Owner) { throw "$folder was not hardened: $($moment.Sddl)" }
               foreach ($sid in @('S-1-1-0', 'S-1-5-11', $State.SmokeAccountSid) | Where-Object { $_ }) {
                  if (@((Get-Acl -LiteralPath $folder).GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier]) | Where-Object { $_.IdentityReference.Value -eq $sid }).Count -gt 0) { throw "$folder still has an entry for $sid" }
               }
               $said += "$folder owner $($moment.Owner)"
            }
            "exit code 7, recorded ($(Get-RestartRecord $app)); hardened: $($said -join '; ')"
         })
      if ($turn) {
         [void] (Test-Step "case ${FolderCase}: nothing was copied through a junction the handles opened before the installer could make (work item 78, R2)" {
               $written = @(Get-ChildItem -LiteralPath $LinkTarget -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
               if ($written.Count -gt 0) { Write-Detail $written 10; throw "$($written.Count) files and folders are in $LinkTarget" }
               $item = Get-Item -LiteralPath (Join-Path $app 'Bin') -Force
               if ($item.LinkType) { throw "$app\Bin is a $($item.LinkType)" }
               "$LinkTarget is empty, Bin a folder; the turn: $(if ($turn.Sync['TurnResult']) { $turn.Sync['TurnResult'] } else { 'not taken: the marker never appeared' })"
            })
      }
      $before = Join-Path $LogDirectory 'install-before-restart.log'
      $againArguments = @($arguments | ForEach-Object { if ($_ -like '/LOG=*') { "/LOG=$before" } else { $_ } })
      $againCode = Invoke-WithAdminPassword { Invoke-SetupForExitCode $Installer $againArguments $before }
      [void] (Test-Step "case ${FolderCase}: the installer run again before the restart was refused by what setup recorded - exit code 7 (work item 78)" {
            if ($againCode -ne 7) { throw "it returned $againCode" }
            if (-not (Select-String -LiteralPath $before -Pattern ('Refused: Setup does not install into ' + [regex]::Escape($app) + ' until Windows has restarted: since Windows last started, setup has recorded') -Quiet)) {
               throw "Inno's log does not refuse by the record"
            }
            'exit code 7, by the record'
         })
      # The restart, stood in for: the handles the case held closed, the volatile key gone.
      if ($turn) { $turned = Stop-FolderWatch $turn; $turn = $null }
      if ($dac) { [void] (Stop-FolderWatch $dac); $dac = $null }
      Write-Info "case ${FolderCase}: the restart, stood in for" (Clear-RestartRecord)
      $there = @(@((Join-Path $app 'Bin'), (Join-Path $app 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ })
      $remade = @(Get-FoldersThatHoldNothing $there)
      $missing = @(Get-FoldersToMake $app $Components)
      $log = Join-Path $LogDirectory 'install-after-restart.log'
      $arguments = @($arguments | ForEach-Object { if ($_ -like '/LOG=*') { "/LOG=$log" } else { $_ } })
      $watch = Start-FolderWatch $app
      $code = -1
      try { $code = Invoke-WithAdminPassword { Invoke-SetupForExitCode $Installer $arguments $log } }
      finally { $watched = Stop-FolderWatch $watch }
   }
   elseif ($dac) { [void] (Stop-FolderWatch $dac) }

   # Work item 78's R1 (check 4 of R1 to R4, 27 October 2026): a copy that fails leaves
   # nothing setup made - Bin and ControlPanel, which setup now makes and records in a
   # program folder it made, removed with it.
   if ($FolderCase -eq 'copy-fails') {
      [void] (Test-Step "case ${FolderCase}: the installation failed for want of space, and setup left nothing it made on the volume (work item 78, R1)" {
            if ($code -eq 0) { throw 'it returned 0: the files fitted, and the case shows nothing' }
            if (Test-Path -LiteralPath $app) { throw "$app is still there: $((@(Get-ChildItem -LiteralPath $app -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }) | Select-Object -First 10) -join ', ')" }
            foreach ($folder in @((Join-Path $app 'Bin'), $app)) {
               if (-not (Select-String -LiteralPath $log -Pattern ('Setup removed ' + [regex]::Escape($folder) + ', which it had made and nothing was installed into') -Quiet)) { throw "Inno's log does not say setup removed $folder" }
            }
            "exit code ${code}; $app is gone, Bin and the program folder removed by setup"
         })
      return
   }

   if ($refused) {
      $pattern = Get-RefusalPattern
      [void] (Test-Step "case ${FolderCase}: the installer was refused before it changed anything - exit code 7, PrepareToInstall's (work item 78, $(Get-CaseItem))" {
            if ($code -ne 7) { throw "it returned $code" }
            $line = Select-String -LiteralPath $log -Pattern ('Refused: Setup does not install into .*' + $pattern[0]) | Select-Object -First 1
            if ($null -eq $line) { throw "Inno's log does not say what was refused, and why (/$($pattern[0])/)" }
            if ($line.Line -notmatch $pattern[1]) { throw "the refusal does not give the remedy (/$($pattern[1])/)" }
            if ($line.Line -match 'Setup stopped the hMailServer service') { throw 'the refusal says setup stopped the service, which a new installation does not have' }
            "exit code 7: $($line.Line.Substring($line.Line.IndexOf('Refused:')))"
         })
      if ($LinkCases -contains $FolderCase) {
         [void] (Test-Step "case ${FolderCase}: nothing was written through the link" {
               $written = @(Get-ChildItem -LiteralPath $LinkTarget -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
               if ($written.Count -gt 0) { Write-Detail $written 10; throw "$($written.Count) files and folders are in $LinkTarget" }
               if ($null -ne (Get-UninstallEntry)) { throw 'there is an uninstall entry' }
               if ($null -ne (Get-HmService)) { throw 'the hMailServer service is registered' }
               "$LinkTarget is empty; no uninstall entry, no service"
            })
         Write-Info "case ${FolderCase}: the link afterwards" (Get-LinkList $(if ($FolderCase -eq 'bin-link-user') { Join-Path $app 'Bin' } elseif ($FolderCase -eq 'events-link') { Join-Path $app 'Events' } else { $app }))
      }
      if (-not ($LinkCases -contains $FolderCase) -or $FolderCase -eq 'events-link') {
         [void] (Test-Step "case ${FolderCase}: nothing was written, and the folders the case made are as they were" {
               # A program folder that was not there is not made by a refused installer
               # (weak-above): the refusal comes before anything is changed.
               if (-not $appBefore -and (Test-Path -LiteralPath $app)) { throw "$app was made, where the installer was refused before it changed anything" }
               if ($State.AboveMade) {
                  $above = @(Get-ChildItem -LiteralPath $State.AboveMade -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
                  if ($above.Count -gt 0) { Write-Detail $above 10; throw "$($above.Count) files and folders were written into $($State.AboveMade)" }
               }
               $after = @(Get-ProgramFolderItems $app)
               $new = @($after | Where-Object { $itemsBefore -notcontains $_ })
               $gone = @($itemsBefore | Where-Object { $after -notcontains $_ })
               if ($new.Count -gt 0) { Write-Detail $new 10; throw "$($new.Count) files and folders were written into $app" }
               if ($gone.Count -gt 0) { Write-Detail $gone 10; throw "$($gone.Count) of the case's files and folders are gone" }
               if ($null -ne (Get-UninstallEntry)) { throw 'there is an uninstall entry' }
               if ($null -ne (Get-HmService)) { throw 'the hMailServer service is registered' }
               "$(if ($appBefore) { "$app holds the $($itemsBefore.Count) items the case made, and nothing else" } else { "$app was not made" }); no uninstall entry, no service"
            })
         Write-Info "case ${FolderCase}: the folders afterwards" ((@($app, (Join-Path $app 'Bin')) | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object { "$_ owner $((Get-FolderMoment $_).Owner): $(Get-LinkList $_)" }) -join ' || ')
      }
      return
   }

   [void] (Test-Step "case ${FolderCase}: $(Split-Path -Leaf $Installer) ($(if ($Components) { "/COMPONENTS=$Components" } else { '/TYPE=full' })) returned 0" {
         if ($code -ne 0) { Write-LogTail $log; throw "it returned $code" }
         'exit code 0'
      })
   if (-not $appBefore) { Test-ProgramFolderMadeWithItsPermissions "case $FolderCase" $Version $watched $log }
   Test-FoldersHardenedBeforeTheFiles "case $FolderCase" $Version $watched $log $there $true $remade
   Test-FoldersMadeBeforeTheFiles "case $FolderCase" $Version $watched $log $missing

   switch ($FolderCase) {
      'held-open' {
         Write-Info "case ${FolderCase}: the handle opened before the installer" ("$(if ($turned['MarkerAt']) { "at $($turned['MarkerAt']), when the program folder's Data appeared: " })$($turned['TurnResult'])$(if ($turned['Error']) { "; $($turned['Error'])" })")
         [void] (Test-Step "case ${FolderCase}: nothing was copied through the junction the handle opened before the installer made, and Bin is the folder setup made again (work item 78, R2)" {
               $bin = Join-Path $app 'Bin'
               $written = @(Get-ChildItem -LiteralPath $LinkTarget -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
               if ($written.Count -gt 0) { Write-Detail $written 10; throw "$($written.Count) files and folders are in $LinkTarget, where the junction made through the handle leads" }
               $item = Get-Item -LiteralPath $bin -Force
               if ($item.LinkType) { throw "$bin is a $($item.LinkType) to $(@($item.Target)[0])" }
               if (-not (Test-Path -LiteralPath (Join-Path $bin 'hMailServer.exe'))) { throw "$bin holds no hMailServer.exe" }
               if (-not (Select-String -LiteralPath $log -Pattern ('Setup removed ' + [regex]::Escape($bin) + ', which was there and held nothing') -Quiet)) { throw 'the setup log does not say setup removed Bin to make it again' }
               "$LinkTarget is empty; $bin is a folder holding the installation's files, made again by setup"
            })
      }
      'broad-entry' {
         [void] (Test-Step "case ${FolderCase}: the entry of the program folder's own that let Authenticated Users change it was taken away before the files, and the setup log says so and who owns Bin\notes.txt (work item 78, R2 and R4)" {
               $taken = Select-String -LiteralPath $State.FirstLog -Pattern ('icacls took away the entries of ' + [regex]::Escape($app) + '''s own for \*S-1-5-11, a group of every user') | Select-Object -First 1
               $owned = Select-String -LiteralPath $log -Pattern ('The program folder: ' + [regex]::Escape((Join-Path $app 'Bin\notes.txt')) + ' is owned by ') | Select-Object -First 1
               if ($null -eq $taken) { throw 'the setup log does not say the entry was taken away' }
               if ($null -eq $owned) { throw 'the setup log does not say who owns Bin\notes.txt' }
               if ($owned.Line -notmatch [regex]::Escape($State.SmokeAccountSid)) { throw "it does not name $($State.SmokeAccountSid)" }
               $left = @((Get-Acl -LiteralPath $app).GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier]) | Where-Object { $_.IdentityReference.Value -eq 'S-1-5-11' })
               if ($left.Count -gt 0) { throw "$app still has an entry for Authenticated Users: $((Get-Acl -LiteralPath $app).Sddl)" }
               'taken away, and both said'
            })
         Write-Info "case ${FolderCase}: the program folder afterwards" ((@(& icacls.exe $app) | Where-Object { $_.Trim() -and $_ -notmatch '^Successfully processed' }) -join ' | ')
      }
      'held-full' {
         [void] (Test-Step "case ${FolderCase}: after the restart, nothing was copied through the junction, and Bin is the folder setup hardened, holding the installation's files (work item 78, R2)" {
               $bin = Join-Path $app 'Bin'
               $written = @(Get-ChildItem -LiteralPath $LinkTarget -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
               if ($written.Count -gt 0) { Write-Detail $written 10; throw "$($written.Count) files and folders are in $LinkTarget, where the junction made through the handles leads" }
               $item = Get-Item -LiteralPath $bin -Force
               if ($item.LinkType) { throw "$bin is a $($item.LinkType) to $(@($item.Target)[0])" }
               if (-not (Test-Path -LiteralPath (Join-Path $bin 'hMailServer.exe'))) { throw "$bin holds no hMailServer.exe" }
               "$LinkTarget is empty; $bin a folder holding the installation's files"
            })
      }
      'dac-handle' {
         [void] (Test-Step "case ${FolderCase}: after the restart, the program folder is the Administrators' with no entry for Everyone (work item 78, R2 and R3)" {
               $moment = Get-FolderMoment $app
               if (-not $moment.Protected -or $TrustedOwnerSids -notcontains $moment.Owner) { throw "$app is not hardened: $($moment.Sddl)" }
               if (@((Get-Acl -LiteralPath $app).GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier]) | Where-Object { $_.IdentityReference.Value -eq 'S-1-1-0' }).Count -gt 0) { throw "$app still has an entry for Everyone: $($moment.Sddl)" }
               "$app owner $($moment.Owner): $($moment.Sddl)"
            })
      }
      'link-admin-only' {
         [void] (Test-Step "case ${FolderCase}: the installation went through the junction only the administrators control, and its target is hardened (work item 78, R3)" {
               if (-not (Select-String -LiteralPath $log -Pattern ([regex]::Escape($app) + ' is a link that only SYSTEM, the Administrators and TrustedInstaller control') -Quiet)) { throw 'the setup log does not say the link was gone through' }
               $files = @(Get-ChildItem -LiteralPath $LinkTarget -Recurse -Force -File -ErrorAction SilentlyContinue)
               if ($files.Count -eq 0) { throw "nothing was installed in $LinkTarget" }
               # The target - the program folder - hardened before the files: nothing
               # inherited. Bin and ControlPanel, which the case does not make: in an
               # installation of the server /ProtectFolders hardens them after the files, so
               # they inherit nothing either; in an installation of the Control Panel
               # alone, which runs no /ProtectFolders, Inno Setup makes them in the
               # target setup has already hardened (PrepareProgramFolders hardens a Bin
               # and a ControlPanel that are there, and these are not), so they hold
               # what the target hands down and nothing of their own - which lets no
               # group of every user change them, as the target does not.
               $withServer = (-not $Components) -or (@($Components -split ',' | ForEach-Object { $_.Trim() }) -contains 'server')
               $writers = @()
               $said = @()
               foreach ($folder in @($LinkTarget, (Join-Path $LinkTarget 'Bin'), (Join-Path $LinkTarget 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ }) {
                  $moment = Get-FolderMoment $folder
                  if ($moment.Error) { throw "the permissions of $folder could not be read: $($moment.Error)" }
                  if ($TrustedOwnerSids -notcontains $moment.Owner) { throw "$folder is owned by $($moment.Owner)" }
                  if ($moment.Protected -and $moment.Inherited -eq 0) { $said += "$folder its own" }
                  elseif ($folder -eq $LinkTarget -or $withServer) { throw "$folder inherits from the folder above: $($moment.Sddl)" }
                  else {
                     $own = @((Get-Acl -LiteralPath $folder).GetAccessRules($true, $false, [System.Security.Principal.SecurityIdentifier]))
                     if ($own.Count -gt 0) { throw "$folder, made by Inno Setup in the hardened $LinkTarget, has entries of its own besides what it inherits: $($moment.Sddl)" }
                     $said += "$folder what $LinkTarget hands down"
                  }
                  foreach ($writer in $moment.Broad) { $writers += "$folder ($writer)" }
               }
               if ($writers.Count -gt 0) { throw "a group of every user may change: $($writers -join '; ')" }
               "$($files.Count) files in $LinkTarget ($($said -join '; ')); the link: $(Get-LinkList $app)"
            })
      }
      'earlier-user-locked' {
         [void] (Test-Step "case ${FolderCase}: Bin, the earlier installation's and owned by $SmokeAccountName with a list that left the Administrators no right to change it, was given to the Administrators with takeown /A and rebuilt before the files (work item 78, R2)" {
               $bin = Join-Path $app 'Bin'
               if (-not (Select-String -LiteralPath $State.FirstLog -Pattern ([regex]::Escape($bin) + ' was owned by an account other than SYSTEM, the Administrators or TrustedInstaller') -Quiet)) { throw 'the setup log of the run refused until the restart does not say Bin was rebuilt' }
               $moment = Get-FolderMoment $bin
               if ($moment.Owner -ne 'S-1-5-32-544') { throw "$bin is owned by $($moment.Owner)" }
               if (-not $moment.Protected) { throw "$bin inherits: $($moment.Sddl)" }
               if ($moment.Sddl -match [regex]::Escape($State.SmokeAccountSid)) { throw "$bin still names ${SmokeAccountName}: $($moment.Sddl)" }
               "$bin owner $($moment.Owner): $($moment.Sddl)"
            })
      }
      'data-inherits' {
         [void] (Test-Step "case ${FolderCase}: the program folder, which holds a Data that inherits and is not the earlier installation's, was hardened before the files (work item 78, R2)" {
               $lines = @(Get-Content -LiteralPath $log)
               $escaped = [regex]::Escape($app)
               $hardened = -1
               $copied = -1
               for ($index = 0; $index -lt $lines.Count; $index++) {
                  if ($hardened -lt 0 -and $lines[$index] -match "icacls hardened $escaped before the files") { $hardened = $index }
                  if ($copied -lt 0 -and $lines[$index] -match 'Dest filename: ') { $copied = $index }
               }
               if ($hardened -lt 0) { throw "Inno's log does not say the program folder was hardened before the files" }
               if ($copied -ge 0 -and $copied -lt $hardened) { throw "Inno's log copies a file (line $($copied + 1)) before it hardens the program folder (line $($hardened + 1))" }
               if (Select-String -LiteralPath $log -Pattern 'is not hardened before the files' -Quiet) { throw 'the setup log leaves the program folder to /ProtectFolders' }
               "hardened at line $($hardened + 1), the first file at line $($copied + 1)"
            })
      }
      'weak-above-remedied' {
         [void] (Test-Step "case ${FolderCase}: the folder above the program folder, given the refusal's remedy, was gone through and the installation made below it (work item 78, R3)" {
               if (Select-String -LiteralPath $log -Pattern 'Refused: ' -Quiet) { throw "Inno's log has a refusal" }
               if (-not (Test-Path -LiteralPath (Join-Path $app 'Bin\hMailServer.exe'))) { throw "$app\Bin holds no hMailServer.exe" }
               $writers = @(Get-BroadReplacers $State.AboveMade)
               if ($writers.Count -gt 0) { throw "$($State.AboveMade) lets a group of every user rename it, remove what is in it or change who may: $($writers -join ', ')" }
               "$app installed below $($State.AboveMade), which no group of every user may rename, empty or change who may"
            })
      }
      'nested-new' {
         [void] (Test-Step "case ${FolderCase}: every folder setup made above the program folder had permissions of its own, and the owner the Administrators (work item 78, R1)" {
               $made = @()
               $folder = Split-Path -Parent $app
               while ($folder.Length -ge $State.TopMade.Length) {
                  $moment = Get-FolderMoment $folder
                  if (-not $moment.Protected -or $moment.Inherited -gt 0) { throw "$folder inherits from the folder above: $($moment.Sddl)" }
                  if ($moment.Owner -ne 'S-1-5-32-544') { throw "$folder is owned by $($moment.Owner)" }
                  if (-not (Select-String -LiteralPath $log -Pattern ('Setup made ' + [regex]::Escape($folder) + ' with permissions of its own') -Quiet)) { throw "the setup log does not say setup made $folder" }
                  $made += $folder
                  $folder = Split-Path -Parent $folder
               }
               $made -join ', '
            })
      }
   }

   if ($Components -and $FolderCase -in @('precreated-admin', 'data-inherits')) {
      [void] (Test-Step "case ${FolderCase}: the program folder and Bin are the Administrators', and $SmokeAccountName has no entry on them" {
            $said = @()
            foreach ($folder in @($app, (Join-Path $app 'Bin'))) {
               $moment = Get-FolderMoment $folder
               if ($moment.Owner -ne 'S-1-5-32-544' -and $TrustedOwnerSids -notcontains $moment.Owner) { throw "$folder is owned by $($moment.Owner)" }
               if ($State.ContainsKey('SmokeAccountSid') -and $State.SmokeAccountSid -and $moment.Sddl -match [regex]::Escape($State.SmokeAccountSid)) { throw "$folder still names $SmokeAccountName" }
               $said += "$folder owner $($moment.Owner)"
            }
            $said -join '; '
         })
   }

   if ($null -ne (Get-UninstallEntry)) {
      [void] (Test-Step "case ${FolderCase}: the uninstaller ran silently and returned 0" { Invoke-Uninstall })
      if ($FolderCase -eq 'nested-new') {
         # Every folder setup made, from the program folder up to the topmost, was
         # given to the uninstaller (section_dirs.iss), which tries to remove each:
         # its log says so. Bin\hMailServer.INI, which every installation writes and
         # every uninstall keeps on purpose ([INI] has no uninsdeleteentry; "the
         # program files are gone" keeps it in a whole run), keeps the folders on its
         # way, so those - the folders setup made, the program folder and Bin - with
         # that one file in them are what may be left, and nothing else; without it,
         # nothing at all.
         [void] (Test-Step "case ${FolderCase}: the uninstall removed every folder setup made but those that hold the kept Bin\hMailServer.INI, and left nothing else" {
               $uninstallLog = Join-Path $LogDirectory 'uninstall.log'
               $made = @()
               $folder = $app
               while ($folder.Length -ge $State.TopMade.Length) { $made += $folder; $folder = Split-Path -Parent $folder }
               foreach ($folder in $made) {
                  if (-not (Select-String -LiteralPath $uninstallLog -Pattern ('Deleting directory: ' + [regex]::Escape($folder) + '$') -Quiet)) { throw "the uninstall log does not say the uninstaller tried to remove $folder, which setup made" }
               }
               if (-not (Test-Path -LiteralPath $State.TopMade)) { return "$($State.TopMade) is gone" }
               $ini = Join-Path $app 'Bin\hMailServer.INI'
               $allowed = @($made) + @((Join-Path $app 'Bin'), $ini)
               $left = @(Get-ChildItem -LiteralPath $State.TopMade -Recurse -Force | ForEach-Object { $_.FullName })
               $other = @($left | Where-Object { $allowed -notcontains $_ })
               if ($other.Count -gt 0) { Write-Detail $other 10; throw "$($other.Count) files and folders are left under $($State.TopMade) besides the kept Bin\hMailServer.INI and the folders on its way" }
               if ($left -notcontains $ini) { throw "$($State.TopMade) is still there with no Bin\hMailServer.INI to keep it: $($left -join ', ')" }
               "the uninstaller tried to remove each of $($made -join ', '); left: the kept $ini and the folders on its way"
            })
      }
   }
}

# ------------------------------------------------------------------ the run

$runStarted = Get-Date
Write-Section 'Installer smoke test'
Write-Info 'machine' "$env:COMPUTERNAME, $((Get-CimInstance Win32_OperatingSystem).Caption), PowerShell $($PSVersionTable.PSVersion)"
Write-Info 'log directory' $LogDirectory

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
   Write-Result 'FAIL' 'the smoke test runs elevated' 'it is not: run it as an administrator'
   exit 2
}

$clean = Test-Step 'preflight: the machine is clean - no hMailServer service, installation, uninstall entry or COM registration' {
   $found = New-Object System.Collections.Generic.List[string]
   if ($null -ne (Get-HmService)) { $found.Add('the hMailServer service') }
   if ($null -ne (Get-UninstallEntry)) { $found.Add('the uninstall entry') }
   if (Test-Path -LiteralPath $State.App) { $found.Add($State.App) }
   $footprint = Get-ComFootprint
   if ($footprint.Count -gt 0) { $found.Add("$($footprint.Count) COM registry keys") }
   if ($found.Count -gt 0) { throw "found $($found -join ', '): revert the machine to its clean snapshot first" }
   'clean'
}
if (-not $clean) { exit 2 }
$State.RegistryBaseline = Get-RegistrySnapshot

if ($WeakParent) {
   $weakened = Test-Step 'preflight: the folder -InstallDirectory is made in hands down Authenticated Users'' Modify, as a Windows client''s drive root does' {
      if (-not $InstallDirectory) { throw '-WeakParent goes with -InstallDirectory' }
      $parent = Split-Path -Parent $InstallDirectory
      if (-not $parent) { throw "-InstallDirectory $InstallDirectory has no folder above it" }
      # A drive's root is given the entry a Windows client's root has, Authenticated
      # Users' Modify for what is below it and not for the root itself (inherit
      # only); any other folder, made when it is not there, Modify on itself too.
      $grant = '*S-1-5-11:(OI)(CI)M'
      if ($parent.Length -le 3) { $grant = '*S-1-5-11:(OI)(CI)(IO)M' }
      else { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
      Write-Info 'preflight: the folder above -InstallDirectory before -WeakParent' ((@(& icacls.exe $parent) | Where-Object { $_.Trim() -and $_ -notmatch '^Successfully processed' }) -join ' | ')
      $answer = @(& icacls.exe $parent /grant $grant)
      if ($LASTEXITCODE -ne 0) { throw "icacls returned ${LASTEXITCODE}: $($answer -join ' ')" }
      $probe = Join-Path $parent 'smoke-probe'
      New-Item -ItemType Directory -Force -Path $probe | Out-Null
      $writers = @(Get-BroadWriters $probe)
      Remove-Item -LiteralPath $probe -Force
      if ($writers.Count -eq 0) { throw "a folder made in $parent does not let Authenticated Users change it" }
      "$parent hands down: $($writers -join ', ')"
   }
   if (-not $weakened) { exit 2 }
}

if ($FolderCase) {
   $made = Test-Step "preflight: case $FolderCase, made before the first installer" {
      if (-not $InstallDirectory) { throw '-FolderCase goes with -InstallDirectory' }
      New-FolderCase
   }
   if (-not $made) { exit 2 }
}

foreach ($path in @($Installer, $UpgradeInstaller)) {
   if ($path -and -not (Test-Path -LiteralPath $path)) {
      Write-Result 'FAIL' 'the installers are there' "$path is not"
      exit 1
   }
}

if ($DatabaseSettings) {
   $read = Test-Step 'the external database: -DatabaseSettings names one, for a new installation' {
      if ($UpgradeInstaller) { throw '-DatabaseSettings is a new installation on an external database, and does not go with -UpgradeInstaller' }
      if (-not (Test-Path -LiteralPath $DatabaseSettings)) { throw "$DatabaseSettings is not there" }
      Read-DatabaseSettings $DatabaseSettings
      $route = if ($DatabaseInEnvironment) { 'every setting in the environment' } else { 'the installer''s switches, the password in the environment' }
      "$($State.DatabaseIniType), $(@($State.Database.Keys) -join ', '), by $route"
   }
   if (-not $read) { exit 1 }
}

Write-Section 'The installers'
$firstVersion = Test-InstallerFile 'first' $Installer
$secondVersion = ''
if ($UpgradeInstaller) { $secondVersion = Test-InstallerFile 'upgrade' $UpgradeInstaller }
Write-Info 'the administrator password' (Set-AdminPasswordFor $firstVersion)
$firstSecures = Test-HasSecureDefaults $firstVersion
$secondSecures = [bool] $UpgradeInstaller -and (Test-HasSecureDefaults $secondVersion)

# The Control Panel alone, and the folder cases a whole server installation does not
# run: one installer, the case's checks, an uninstall.
if (Test-IsFocusedRun) {
   if ($UpgradeInstaller -or $DatabaseSettings) { Write-Result 'FAIL' 'the run' '-Components and every case but precreated-admin are one installer: no -UpgradeInstaller or -DatabaseSettings'; exit 2 }
   Write-Section "Case $(if ($FolderCase) { $FolderCase } else { 'none' }), $(if ($Components) { "/COMPONENTS=$Components" } else { '/TYPE=full' }), $firstVersion"
   Invoke-FocusedRun $firstVersion
   Write-Section 'Summary'
   Write-Line "$script:Passed passed, $script:Failed failed, in $([math]::Round(((Get-Date) - $runStarted).TotalMinutes, 1)) min"
   if ($script:Failed -gt 0) { exit 1 }
   exit 0
}

# ---- install
Write-Section "Install $firstVersion"
$installLog = Join-Path $LogDirectory 'install.log'
$State.ProgramFoldersReadable = Get-ProgramFoldersReadable $State.App
Write-Info 'install: every user could read, before the installer' (($State.ProgramFoldersReadable.Keys | Sort-Object | ForEach-Object { "$_ $(if ($State.ProgramFoldersReadable[$_]) { 'yes' } else { 'no' })" }) -join ', ')
# Work item 78: the program folder watched while the installer runs - its first
# moment when it is not there yet, and Bin's and ControlPanel's when they are.
$installThere = @(@((Join-Path $State.App 'Bin'), (Join-Path $State.App 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ })
$installRemade = @(Get-FoldersThatHoldNothing $installThere)
$installMissing = @(Get-FoldersToMake $State.App '')
$installWatch = Start-FolderWatch $State.App
try {
   $installed = Test-Step "install: $(Split-Path -Leaf $Installer) ran silently and returned 0" {
      $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TYPE=full', "/LOG=$installLog") + @(Get-DatabaseSwitches)
      if ($InstallDirectory) { $arguments += "/DIR=$InstallDirectory" }
      Invoke-WithDatabaseVariables { Invoke-SetupWithAdminPassword $Installer $arguments $installLog }
   }
}
finally { $installWatched = Stop-FolderWatch $installWatch }
if (-not $installed) { Write-DatabaseDiagnosis (Get-InstallDirectory) }
$State.Installed = ($null -ne (Get-UninstallEntry)) -or (Test-Path -LiteralPath (Join-Path (Get-InstallDirectory) 'Bin\hMailServer.exe'))
if ($State.Installed) {
   Test-ProgramFolderMadeWithItsPermissions 'install' $firstVersion $installWatched $installLog
   Test-FoldersHardenedBeforeTheFiles 'install' $firstVersion $installWatched $installLog $installThere $true $installRemade
   Test-FoldersMadeBeforeTheFiles 'install' $firstVersion $installWatched $installLog $installMissing
}

if ($State.Installed -and ($null -ne $State.Database)) {
   [void] (Test-Step 'install: the database step was DBSetup /unattended, which returned 0 (Inno''s log)' {
         $line = Select-String -LiteralPath $installLog -Pattern 'DBSetup\.exe /unattended returned (-?\d+)' | Select-Object -Last 1
         if ($null -eq $line) { throw "Inno's log has no 'DBSetup.exe /unattended returned': the installer did not take the database given" }
         if ($line.Matches[0].Groups[1].Value -ne '0') { throw "it returned $($line.Matches[0].Groups[1].Value)" }
         # DBSetup's own lines: the unstamped ones the installer logged after that line,
         # up to Inno's next stamped one. A search of the whole log for "Creating the"
         # found Inno's "Creating the icon." instead (the pretag smoke run of 2 October
         # 2026).
         $lines = @(Get-Content -LiteralPath $installLog)
         $said = @()
         for ($index = $line.LineNumber; $index -lt $lines.Count -and $lines[$index] -notmatch '^\d{4}-\d{2}-\d{2} \d'; $index++) {
            if ($lines[$index].Trim()) { $said += $lines[$index].Trim() }
         }
         if ($said.Count -eq 0) { throw "Inno's log has no line of DBSetup's after 'DBSetup.exe /unattended returned 0'" }
         "returned 0: $($said -join ' ')"
      })
}

if ($State.Installed) {
   Test-Installed 'install' $firstVersion
   Test-ProgramFoldersHardened 'install' $firstVersion $installLog

   # docs/SecureDefaults.md, "What an installation on a clean machine is to check".
   if ($firstSecures) {
      Write-Section "The secure defaults of a new installation (docs/SecureDefaults.md)"
      Test-NewInstallDefaults 'install' $installLog
   }
   else {
      Write-Info 'install: the secure defaults of a new installation' "none: $firstVersion is older than $SecureDefaultsVersion, whose installer first gives them"
   }

   Write-Section 'A domain, an account and a message'
   [void] (Test-Step 'seed: a domain, an account and a delivered message' { New-SmokeData })
   if ($State.Seeded) {
      [void] (Test-Step 'seed: POP3 logs the account on and finds the message' {
            $stat = Get-Pop3Stat
            if ($stat -notmatch '^\+OK 1 ') { throw "STAT answered '$stat'" }
            "STAT '$stat'"
         })
      [void] (Test-Step 'seed: IMAP logs the account on and finds the message' {
            $exists = Get-ImapExists
            if ($exists -ne 1) { throw "SELECT INBOX says $exists EXISTS" }
            'SELECT INBOX: 1 EXISTS'
         })
   }

   # With the account made: a password only over TLS, but from this computer.
   if ($firstSecures -and $State.Seeded) {
      Write-Section 'A password only over TLS, but from this computer (docs/SecureDefaults.md)'
      Test-SignInOverTls 'install'
   }

   # ---- upgrade
   if ($UpgradeInstaller) {
      # What the upgrade must leave exactly as it was (docs/SecureDefaults.md).
      [void] (Test-Step 'seed: the ports, the IP ranges and the certificate records, read before the upgrade' {
            $State.ListenersBefore = Get-ListenerState
            "$($State.ListenersBefore.Ports.Count) ports ($(($State.ListenersBefore.Ports | ForEach-Object { "$(Get-LineValue $_ 'PortNumber') ConnectionSecurity $(Get-LineValue $_ 'ConnectionSecurity')" }) -join ', ')), $($State.ListenersBefore.Ranges.Count) ranges ($(($State.ListenersBefore.Ranges | ForEach-Object { "$(Get-LineValue $_ 'Name') RequireSSLTLSForAuth $(Get-LineValue $_ 'RequireSSLTLSForAuth')" }) -join ', ')), $($State.ListenersBefore.Certificates.Count) certificate records"
         })

      Write-Section "Upgrade $firstVersion to $secondVersion"
      $upgradeLog = Join-Path $LogDirectory 'upgrade.log'
      $State.ProgramFoldersReadable = Get-ProgramFoldersReadable (Get-InstallDirectory)
      Write-Info 'upgrade: every user could read, before the installer' (($State.ProgramFoldersReadable.Keys | Sort-Object | ForEach-Object { "$_ $(if ($State.ProgramFoldersReadable[$_]) { 'yes' } else { 'no' })" }) -join ', ')
      # Work item 78's R2: Bin and ControlPanel watched while the upgrade runs, and
      # the program folder with them when its data folders inherit nothing.
      $upgradeThere = @(@((Join-Path (Get-InstallDirectory) 'Bin'), (Join-Path (Get-InstallDirectory) 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ })
      $upgradeProgramFolderToo = Test-DataFoldersInheritNothing (Get-InstallDirectory)
      Write-Info 'upgrade: the data folders inherit nothing from the program folder, before the installer' $(if ($upgradeProgramFolderToo) { 'yes: the program folder is to be hardened before the files too' } else { 'no: the program folder is left to /ProtectFolders, after the files' })
      $upgradeArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$upgradeLog")
      if (-not $State.PasswordInEnvironment) { $upgradeArguments += "/adminpassword=$AdminPassword" }
      $upgradeWatch = Start-FolderWatch (Get-InstallDirectory)
      $upgradeCode = -1
      try { $upgradeCode = Invoke-WithAdminPassword { Invoke-SetupForExitCode $UpgradeInstaller $upgradeArguments $upgradeLog } }
      finally { $upgradeWatched = Stop-FolderWatch $upgradeWatch }
      # Work item 78 (check 4 of R1 to R4, 27 October 2026): an earlier installation whose
      # folders another account could change - 6.3.6's, in a folder that hands Authenticated
      # Users Modify down - hardened and refused until Windows has restarted, a second run
      # refused by the record, and the upgrade run again once the record is taken away, as
      # a restart takes it away.
      if ($upgradeCode -eq 7 -and (Test-HardensBeforeFiles $secondVersion) -and (Select-String -LiteralPath $upgradeLog -Pattern 'until Windows has restarted: before this installation' -Quiet)) {
         $upgradeApp = Get-InstallDirectory
         [void] (Test-Step "upgrade: setup hardened what another account could change in $firstVersion's folders and refused until Windows has restarted - exit code 7, the service left running (work item 78, R2)" {
               $line = Select-String -LiteralPath $upgradeLog -Pattern 'Refused: Setup does not install into .* until Windows has restarted: before this installation' | Select-Object -First 1
               if ($line.Line -notmatch 'Restart Windows, and run setup again') { throw 'the refusal does not say to restart Windows' }
               if (-not (Get-RestartRecord $upgradeApp)) { throw "$RestartKey does not record $upgradeApp" }
               $service = Get-HmService
               if ($null -eq $service -or $service.State -ne 'Running') { throw "the service is $(if ($service) { $service.State } else { 'not there' }): the refusal comes before the stop" }
               $said = @()
               foreach ($folder in @($upgradeApp, (Join-Path $upgradeApp 'Bin')) | Where-Object { Test-Path -LiteralPath $_ }) {
                  $moment = Get-FolderMoment $folder
                  if (-not $moment.Protected -or $moment.Broad.Count -gt 0 -or $TrustedOwnerSids -notcontains $moment.Owner) { throw "$folder was not hardened: $($moment.Sddl)" }
                  $said += "$folder owner $($moment.Owner)"
               }
               "$($line.Line.Substring($line.Line.IndexOf('Refused:'), [math]::Min(300, $line.Line.Length - $line.Line.IndexOf('Refused:'))))...; hardened: $($said -join '; ')"
            })
         $before = Join-Path $LogDirectory 'upgrade-before-restart.log'
         $againCode = Invoke-WithAdminPassword { Invoke-SetupForExitCode $UpgradeInstaller @($upgradeArguments | ForEach-Object { if ($_ -like '/LOG=*') { "/LOG=$before" } else { $_ } }) $before }
         [void] (Test-Step 'upgrade: the upgrade run again before the restart was refused by what setup recorded - exit code 7 (work item 78)' {
               if ($againCode -ne 7) { throw "it returned $againCode" }
               if (-not (Select-String -LiteralPath $before -Pattern 'until Windows has restarted: since Windows last started, setup has recorded' -Quiet)) { throw "Inno's log does not refuse by the record" }
               'exit code 7, by the record'
            })
         Write-Info 'upgrade: the restart, stood in for' (Clear-RestartRecord)
         $upgradeThere = @(@((Join-Path $upgradeApp 'Bin'), (Join-Path $upgradeApp 'ControlPanel')) | Where-Object { Test-Path -LiteralPath $_ })
         $upgradeProgramFolderToo = Test-DataFoldersInheritNothing $upgradeApp
         $upgradeLog = Join-Path $LogDirectory 'upgrade-after-restart.log'
         $upgradeArguments = @($upgradeArguments | ForEach-Object { if ($_ -like '/LOG=*') { "/LOG=$upgradeLog" } else { $_ } })
         $upgradeWatch = Start-FolderWatch $upgradeApp
         try { $upgradeCode = Invoke-WithAdminPassword { Invoke-SetupForExitCode $UpgradeInstaller $upgradeArguments $upgradeLog } }
         finally { $upgradeWatched = Stop-FolderWatch $upgradeWatch }
      }
      $upgraded = Test-Step "upgrade: $(Split-Path -Leaf $UpgradeInstaller) ran silently over $firstVersion and returned 0" {
         if ($upgradeCode -ne 0) { Write-LogTail $upgradeLog; throw "it returned $upgradeCode" }
         'exit code 0'
      }
      if (-not $upgraded) { Write-DatabaseDiagnosis (Get-InstallDirectory) }

      Test-Installed 'upgrade' $secondVersion
      Test-FoldersHardenedBeforeTheFiles 'upgrade' $secondVersion $upgradeWatched $upgradeLog $upgradeThere $upgradeProgramFolderToo
      Test-ProgramFoldersHardened 'upgrade' $secondVersion $upgradeLog

      [void] (Test-Step "upgrade: the schema moved to the newer build's" {
            if (-not $State.Seeded) { throw 'the schema before the upgrade was not read' }
            $now = 0
            $application = Connect-Server
            try { $now = [int] $application.Database.CurrentVersion } finally { Remove-ComReference $application }
            if ($now -lt $State.SchemaBefore) { throw "it went from $($State.SchemaBefore) to $now" }
            "$($State.SchemaBefore) to $now"
         })

      [void] (Test-Step 'upgrade: one installation, one uninstaller, the components kept' {
            $uninstallers = @(Get-ChildItem -LiteralPath $State.App -Filter 'unins*.exe' -File)
            if ($uninstallers.Count -ne 1) { throw "uninstallers: $(($uninstallers | ForEach-Object Name) -join ', ')" }
            if (-not (Test-Path -LiteralPath (Join-Path $State.App 'ControlPanel\hMailCP.exe'))) { throw 'the Control Panel, installed by the first install, is gone' }
            if (-not (Test-Path -LiteralPath (Join-Path $State.App 'Bin\DBSetupQuick.exe'))) { throw 'the server component is incomplete: no DBSetupQuick.exe' }
            "$($uninstallers[0].Name); the server and the Control Panel"
         })

      if ($State.Seeded) { Test-SmokeDataKept 'upgrade' }

      # docs/SecureDefaults.md: an upgrade is never changed.
      if ($secondSecures) {
         Write-Section "The upgrade keeps what $firstVersion had (docs/SecureDefaults.md)"
         Test-UpgradeKeepsListeners 'upgrade' $upgradeLog (-not $firstSecures)
         Test-VerbLeavesUpgradeAlone 'upgrade'
      }
   }
}
else {
   Write-Result 'FAIL' 'install: something was installed' 'there is neither an uninstall entry nor hMailServer.exe; nothing to check or uninstall'
}

# ---- uninstall, whatever happened above, when anything is there to uninstall
if ($null -ne (Get-UninstallEntry)) {
   Write-Section 'Uninstall'
   Copy-ServerLogs $State.App 'before-uninstall'
   try { Write-Info 'uninstall: the hMailServer event log source before the uninstall' (Register-EventSourceIfMissing) }
   catch { Write-Info 'uninstall: the hMailServer event log source before the uninstall' "could not be registered by the test: $($_.Exception.Message)" }
   [GC]::Collect()
   [GC]::WaitForPendingFinalizers()
   [void] (Test-Step 'uninstall: the uninstaller ran silently and returned 0' { Invoke-Uninstall })
   Test-Uninstalled
}

Write-Section 'Summary'
$minutes = [math]::Round(((Get-Date) - $runStarted).TotalMinutes, 1)
Write-Line "$script:Passed passed, $script:Failed failed, in $minutes min"
if ($script:Failed -gt 0) { exit 1 }
exit 0
