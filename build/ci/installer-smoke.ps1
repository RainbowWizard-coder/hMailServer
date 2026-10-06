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
   or TrustedInstaller, and no group of every user able to change them or a program or
   library in them; the REST API's key store still the service's to write when the
   service runs as NT SERVICE\hMailServer; /ProtectFolders' exit 0 in Inno's log; and a
   second /ProtectFolders, run here, finding all three "already hardened" and ending in
   0. After an earlier installer the run says, as INFO, which group of every user may
   change them.

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
   [switch] $WeakParent
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
# (HardenNewProgramFolder), and so goes with it.
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
# user a right to change it, as "SID rights".
function Get-BroadWriters([string] $Path) {
   $acl = Get-Acl -LiteralPath $Path
   $found = @()
   foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
      if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) { continue }
      if (($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
      if ($BroadGroupSids -notcontains $rule.IdentityReference.Value) { continue }
      if (([long] $rule.FileSystemRights -band $WriteRightsMask) -ne 0) { $found += "$($rule.IdentityReference.Value) $($rule.FileSystemRights)" }
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

   [void] (Test-Step "${Phase}: no group of every user may change the program's folders or a program or library in them" {
         $checked = 0
         $writers = @()
         $objects = @($folders)
         foreach ($folder in @((Join-Path $app 'Bin'), (Join-Path $app 'ControlPanel'))) {
            $objects += @(Get-ChildItem -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue |
               Where-Object { $_.PSIsContainer -or $_.Extension -eq '.exe' -or $_.Extension -eq '.dll' } | ForEach-Object { $_.FullName })
         }
         $objects += @(Get-ChildItem -LiteralPath $app -Filter 'unins*.exe' -File | ForEach-Object { $_.FullName })
         foreach ($object in $objects) {
            $checked++
            foreach ($writer in @(Get-BroadWriters $object)) { $writers += "$object ($writer)" }
         }
         if ($writers.Count -gt 0) { Write-Detail $writers 20; throw "$($writers.Count) entries (above)" }
         "$checked folders, programs and libraries"
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
   # September 2026 - has none. Everything else is a program file, and must be gone.
   $keptPattern = '^(Bin\\hMailServer\.ini|Bin\\hMailServerApiKeys\.ini|Database\\.+|Data\\.+|Logs\\.+|Events\\.+|Addons\\(?!(DataDirectorySynchronizer|ImportTool)\\).+)$'
   $left = @()
   if (Test-Path -LiteralPath $app) {
      $left = @(Get-ChildItem -LiteralPath $app -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($app.Length + 1) })
   }
   $programFiles = @($left | Where-Object { $_ -notmatch $keptPattern })
   $keptFiles = @($left | Where-Object { $_ -match $keptPattern })

   [void] (Test-Step 'uninstall: the program files are gone' {
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
         "$($left.Count) files left under $app, every one of them kept on purpose"
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

# ---- install
Write-Section "Install $firstVersion"
$installLog = Join-Path $LogDirectory 'install.log'
$State.ProgramFoldersReadable = Get-ProgramFoldersReadable $State.App
Write-Info 'install: every user could read, before the installer' (($State.ProgramFoldersReadable.Keys | Sort-Object | ForEach-Object { "$_ $(if ($State.ProgramFoldersReadable[$_]) { 'yes' } else { 'no' })" }) -join ', ')
$installed = Test-Step "install: $(Split-Path -Leaf $Installer) ran silently and returned 0" {
   $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TYPE=full', "/LOG=$installLog") + @(Get-DatabaseSwitches)
   if ($InstallDirectory) { $arguments += "/DIR=$InstallDirectory" }
   Invoke-WithDatabaseVariables { Invoke-SetupWithAdminPassword $Installer $arguments $installLog }
}
if (-not $installed) { Write-DatabaseDiagnosis (Get-InstallDirectory) }
$State.Installed = ($null -ne (Get-UninstallEntry)) -or (Test-Path -LiteralPath (Join-Path (Get-InstallDirectory) 'Bin\hMailServer.exe'))

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
      $upgraded = Test-Step "upgrade: $(Split-Path -Leaf $UpgradeInstaller) ran silently over $firstVersion and returned 0" {
         Invoke-SetupWithAdminPassword $UpgradeInstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$upgradeLog") $upgradeLog
      }
      if (-not $upgraded) { Write-DatabaseDiagnosis (Get-InstallDirectory) }

      Test-Installed 'upgrade' $secondVersion
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
