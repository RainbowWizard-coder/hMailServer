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
                the logs, the event scripts and the addon scripts. Registry keys the
                run left anywhere else are listed, and any that names hMailServer
                fails.

   Every check prints one line, PASS, FAIL, SKIP or INFO, then a dash and what it
   found. The exit code is 0 when nothing failed, 1 when a check failed and 2 when the
   machine was not clean to begin with. The lines also go to smoke.log in
   -LogDirectory, beside Inno's logs and copies of the server's.

   The silent switches are the installer's own (hMailServerInnoExtension.iss):
   /VERYSILENT /SUPPRESSMSGBOXES /NORESTART, /TYPE=full on the first install as the
   GitHub workflow passed it, and /adminpassword=, the only way a silent install sets
   the administrator password. An upgrade that moves the schema needs it: DBUpdater
   authenticates as the administrator before it upgrades, and under /silent it fails
   rather than asks. The upgrade gets no /TYPE, as winget and Chocolatey pass none
   (docs/PackageManagers.md): Inno keeps the components already installed. The
   password is made here, used over COM, POP3 and IMAP on this machine, and never
   printed.

   -AllowUnsigned is for a freshly built installer, which nobody has signed: an
   installer with no signature at all is then a SKIP, not a FAIL. A signature that is
   present and not valid fails either way.

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
   [int] $TimeoutMinutes = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# C:/smoke/... from the run script becomes C:\smoke\...: Inno's /LOG= is given a path
# made from these.
$Installer = [System.IO.Path]::GetFullPath($Installer)
if ($UpgradeInstaller) { $UpgradeInstaller = [System.IO.Path]::GetFullPath($UpgradeInstaller) }
$LogDirectory = [System.IO.Path]::GetFullPath($LogDirectory)

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

# Letters and digits only: the value travels on Inno's command line, in DBSetupQuick's
# "password:VALUE" argument and in an IMAP quoted string, and none of them needs
# escaping for these.
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

# Everything the checks learn and later checks need: a hashtable, so that a check's
# script block can write to it from its own scope.
$State = @{
   App = Join-Path $env:ProgramFiles 'hMailServer'
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

function Open-Conversation([int] $Port) {
   $client = New-Object System.Net.Sockets.TcpClient
   $client.ReceiveTimeout = 30000
   $client.SendTimeout = 30000
   $client.Connect('127.0.0.1', $Port)
   $stream = $client.GetStream()
   $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::ASCII)
   $writer = New-Object System.IO.StreamWriter($stream, [System.Text.Encoding]::ASCII)
   $writer.NewLine = "`r`n"
   $writer.AutoFlush = $true
   return [pscustomobject] @{ Client = $client; Reader = $reader; Writer = $writer }
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
   $tool = Join-Path $App 'Bin\DBSetupQuick.exe'
   if (-not (Test-Path -LiteralPath $tool)) {
      Write-Line '      DBSetupQuick.exe is not installed: the failure was earlier than the database step.'
      return
   }
   $out = Join-Path $LogDirectory 'dbsetup-stdout.txt'
   $err = Join-Path $LogDirectory 'dbsetup-stderr.txt'
   $arguments = @('/silent', "password:$AdminPassword")
   $process = Start-Process -FilePath $tool -ArgumentList $arguments -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
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
         if ((Read-IniSection $ini 'Security') -ne $State.IniSecurity) { throw '[Security] changed' }
         '[Database] and [Security] as they were'
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
   # empty. Everything else is a program file, and must be gone.
   $keptPattern = '^(Bin\\hMailServer\.ini|Database\\.+|Data\\.+|Logs\\.+|Events\\.+|Addons\\(?!(DataDirectorySynchronizer|ImportTool)\\).+)$'
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
         $binaries = @(Get-ChildItem -LiteralPath $app -Recurse -File -Force -Include '*.exe', '*.dll', '*.tlb' -ErrorAction SilentlyContinue)
         if ($binaries.Count -gt 0) { throw "binaries are left: $(($binaries | ForEach-Object Name) -join ', ')" }
         "$($left.Count) files left under $app, every one of them kept on purpose"
      })

   Write-Info 'uninstall: what is left under the installation directory' (($keptFiles | ForEach-Object { ($_ -split '\\')[0] } | Group-Object | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ')

   [void] (Test-Step 'uninstall: hMailServer.INI is kept, with its database settings' {
         $ini = Join-Path $app 'Bin\hMailServer.INI'
         if (-not (Test-Path -LiteralPath $ini)) { throw "$ini is gone" }
         if ($State.IniDatabase -and (Read-IniSection $ini 'Database') -ne $State.IniDatabase) { throw '[Database] changed' }
         "Bin\hMailServer.INI, [Database] type $(Get-IniValue $ini 'Database' 'Type')"
      })

   [void] (Test-Step 'uninstall: the database is kept' {
         $files = @(Get-ChildItem -LiteralPath (Join-Path $app 'Database') -Filter '*.sdf' -File -ErrorAction SilentlyContinue)
         if ($files.Count -eq 0) { throw 'there is no .sdf left in Database' }
         ($files | ForEach-Object { "Database\$($_.Name)" }) -join ', '
      })

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

foreach ($path in @($Installer, $UpgradeInstaller)) {
   if ($path -and -not (Test-Path -LiteralPath $path)) {
      Write-Result 'FAIL' 'the installers are there' "$path is not"
      exit 1
   }
}

Write-Section 'The installers'
$firstVersion = Test-InstallerFile 'first' $Installer
$secondVersion = ''
if ($UpgradeInstaller) { $secondVersion = Test-InstallerFile 'upgrade' $UpgradeInstaller }

# ---- install
Write-Section "Install $firstVersion"
$installLog = Join-Path $LogDirectory 'install.log'
$installed = Test-Step "install: $(Split-Path -Leaf $Installer) ran silently and returned 0" {
   Invoke-Setup $Installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TYPE=full', "/adminpassword=$AdminPassword", "/LOG=$installLog") $installLog
}
if (-not $installed) { Write-DatabaseDiagnosis (Get-InstallDirectory) }
$State.Installed = ($null -ne (Get-UninstallEntry)) -or (Test-Path -LiteralPath (Join-Path (Get-InstallDirectory) 'Bin\hMailServer.exe'))

if ($State.Installed) {
   Test-Installed 'install' $firstVersion

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

   # ---- upgrade
   if ($UpgradeInstaller) {
      Write-Section "Upgrade $firstVersion to $secondVersion"
      $upgradeLog = Join-Path $LogDirectory 'upgrade.log'
      $upgraded = Test-Step "upgrade: $(Split-Path -Leaf $UpgradeInstaller) ran silently over $firstVersion and returned 0" {
         Invoke-Setup $UpgradeInstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/adminpassword=$AdminPassword", "/LOG=$upgradeLog") $upgradeLog
      }
      if (-not $upgraded) { Write-DatabaseDiagnosis (Get-InstallDirectory) }

      Test-Installed 'upgrade' $secondVersion

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
