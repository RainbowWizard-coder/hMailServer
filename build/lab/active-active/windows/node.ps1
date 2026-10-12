# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
One Windows node of the active-active lab: the hMailServer Windows server, installed
as an installation lays itself out, under C:\hmlab-aa, against the lab's SQL Server.
aa-win.py copies this file to C:\hmlab-aa\lab\node.ps1 on the node and runs it there
over SSH, in Windows PowerShell 5.1, as an administrator. The lab's README
(build/lab/active-active/README.md, "The Windows lab") says what it is for.

Secrets are never arguments. A command that needs one reads KEY=VALUE lines from
standard input, up to a line '%%' or the end of the input:
  AA_NODE_PASSWORD     the database login's password (set-database, create-database, ado-probe)
  AA_ADMIN_PASSWORD    the lab server's administrator password (set-admin-password,
                       create-database, configure)
  AA_ACCOUNT_PASSWORD  the lab accounts' password (configure)

One installation per machine. On Windows the server finds hMailServer.INI through
HKLM\SOFTWARE\hMailServer\InstallLocation (the 32-bit view, which the installer
writes) before its own folder (Utilities::GetBinDirectory), so a second installation
on a machine that has one reads the first's INI and runs on the first's database.
A node installed beside another installation (the AD lab's DC has its own) therefore
runs in TURNS: for as long as the lab's service runs, and for the lab's own verbs, the
other installation's service is stopped and Disabled and InstallLocation names
C:\hmlab-aa; when the lab's service stops, both are put back and the other service
is started again if it was running. What a turn changed is kept in
C:\hmlab-aa\lab\turn.json first, so that 'release' puts it back after any failure.
Disabled and not Manual: during a turn a COM activation of hMailServer.Application
would otherwise start the other installation's service, which would read the lab's
INI.

Commands:
  install -Kit <zip> -Service <name> [-Register]
        lay the kit (aa-win.py's build kit: Bin, DBScripts, Languages, Events,
        WebAdmin) out under C:\hmlab-aa, write [Directories] into Bin\hMailServer.ini
        as the installer does, and make the service: with -Register the way a real
        installation does (hMailServer.exe /RegisterTypeLib and /Register - the
        service 'hMailServer' and the COM server - and InstallLocation, as the
        installer writes it); without it a second service of the given name, made as
        /Register makes one, beside the installation the machine has (turns, above).
        Either way the service starts manually.
  release                  the lab's service stopped and a turn, if one is open,
                           given back
  deploy -Kit <zip>
        the service stopped, Bin's files (hMailServer.ini kept), DBScripts,
        Languages and WebAdmin replaced by the kit's; the service is left stopped
  msoledbsql -Msi <path>   the Microsoft OLE DB Driver 18, unless it is installed
  firewall                 inbound TCP for the lab's ports to hMailServer.exe, from the
                           local subnet, when a firewall profile is on; nothing otherwise
  set-database -DbServer <host,port> -Database <name> -Login <name>
        [Database] as the server writes it: Type=MSSQL, the password sealed with DPAPI
        for this machine (CryptProtectData, CRYPTPROTECT_LOCAL_MACHINE, described
        "hMailServer secret", base64; PasswordEncryption=6), as
        IniFileSettings::SetPassword does
  check-database           [Database] as the INI has it, the password left out, and
                           whether the sealed password opens to AA_NODE_PASSWORD
  set-admin-password       hMailServer.exe --set-admin-password, the password on its input
  upgrade-database         hMailServer.exe --upgrade-database (every node stopped first)
  create-database -DbServer <host,port> -Database <name> -Login <name>
        through COM, as DBSetup does: Database.CreateExternalDatabase (the server's own
        creation path: CREATE DATABASE and the create script), then Reinitialize
  configure -SinkHost <address> -SinkPort <port>
        through COM: the listeners moved to 2525, 2587, 1110 and 1143, auto-ban off, the
        domain example.test with account1..4, and the route sinking sink.test
  start | stop | status [-Service <name>]
  clear-data               the service stopped: the data and log folders emptied
  errors                   the error log's codes and how often each appears
  ado-probe -DbServer <host,port> -Database <name> -Login <name>
        ADO's Command.Execute RecordsAffected through each OLE DB provider installed
        (MSOLEDBSQL, SQLOLEDB), in a session set up as ADOConnection::Connect sets one
        (DATEFORMAT ymd, READ UNCOMMITTED); the clock functions; a temporary table only
#>
param(
    [Parameter(Position = 0, Mandatory = $true)][string]$Command,
    [string]$Kit,
    [string]$Service,
    [switch]$Register,
    [switch]$UpgradeDatabase,
    [string]$Msi,
    [string]$DbServer,
    [string]$Database,
    [string]$Login,
    [string]$SinkHost,
    [int]$SinkPort = 14325,
    [string]$Root = 'C:\hmlab-aa'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$Bin = Join-Path $Root 'Bin'
$Exe = Join-Path $Bin 'hMailServer.exe'
$Ini = Join-Path $Bin 'hMailServer.ini'
$StateFile = Join-Path $Root 'lab\node.json'
$TurnFile = Join-Path $Root 'lab\turn.json'
$AnchorKey = 'HKLM:\SOFTWARE\WOW6432Node\hMailServer'
$LabPorts = @(2525, 2587, 1110, 1143)
$PortMoves = @{ 25 = 2525; 587 = 2587; 110 = 1110; 143 = 1143 }

function Say([string]$text) { [Console]::Out.WriteLine($text) }
function Fail([string]$text) { [Console]::Error.WriteLine("node.ps1: $text"); exit 1 }

function Read-Secrets {
    $values = @{}
    if (-not [Console]::IsInputRedirected) { return $values }
    while ($true) {
        $line = [Console]::In.ReadLine()
        if ($null -eq $line -or $line -eq '%%') { break }
        $line = $line.TrimEnd("`r")
        if ($line -eq '') { continue }
        $i = $line.IndexOf('=')
        if ($i -lt 1) { Fail 'standard input: expected KEY=VALUE lines and %%' }
        $values[$line.Substring(0, $i)] = $line.Substring($i + 1)
    }
    return $values
}

function Need($secrets, [string[]]$keys) {
    foreach ($k in $keys) {
        if (-not $secrets.ContainsKey($k) -or [string]::IsNullOrEmpty($secrets[$k])) { Fail "$k was not given on standard input" }
    }
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class HmLabNative
{
    [StructLayout(LayoutKind.Sequential)]
    private struct DATA_BLOB { public int cbData; public IntPtr pbData; }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptProtectData(ref DATA_BLOB dataIn, string description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, int flags, ref DATA_BLOB dataOut);

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptUnprotectData(ref DATA_BLOB dataIn, IntPtr description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, int flags, ref DATA_BLOB dataOut);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WritePrivateProfileString(string section, string key, string value, string file);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetPrivateProfileString(string section, string key, string defaultValue,
        StringBuilder value, int size, string file);

    private const int CRYPTPROTECT_LOCAL_MACHINE = 0x4;

    // DataProtector::Protect on Windows: the UTF-8 bytes, sealed for this machine,
    // described "hMailServer secret", with no entropy, written as base64.
    public static string Protect(string secret)
    {
        byte[] plain = Encoding.UTF8.GetBytes(secret);
        DATA_BLOB input = new DATA_BLOB();
        DATA_BLOB output = new DATA_BLOB();
        input.cbData = plain.Length;
        input.pbData = Marshal.AllocHGlobal(plain.Length);
        try
        {
            Marshal.Copy(plain, 0, input.pbData, plain.Length);
            if (!CryptProtectData(ref input, "hMailServer secret", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                                  CRYPTPROTECT_LOCAL_MACHINE, ref output))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            byte[] sealedBytes = new byte[output.cbData];
            Marshal.Copy(output.pbData, sealedBytes, 0, output.cbData);
            return Convert.ToBase64String(sealedBytes);
        }
        finally
        {
            for (int i = 0; i < plain.Length; i++) { plain[i] = 0; Marshal.WriteByte(input.pbData, i, 0); }
            Marshal.FreeHGlobal(input.pbData);
            if (output.pbData != IntPtr.Zero) LocalFree(output.pbData);
        }
    }

    // True when the value opens to the secret given: the check that what is in the
    // file is the secret meant, without the secret being written anywhere.
    public static bool Opens(string sealedBase64, string secret)
    {
        byte[] sealedBytes;
        try { sealedBytes = Convert.FromBase64String(sealedBase64); } catch (FormatException) { return false; }
        DATA_BLOB input = new DATA_BLOB();
        DATA_BLOB output = new DATA_BLOB();
        input.cbData = sealedBytes.Length;
        input.pbData = Marshal.AllocHGlobal(sealedBytes.Length);
        try
        {
            Marshal.Copy(sealedBytes, 0, input.pbData, sealedBytes.Length);
            if (!CryptUnprotectData(ref input, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0, ref output))
                return false;
            byte[] plain = new byte[output.cbData];
            Marshal.Copy(output.pbData, plain, 0, output.cbData);
            bool same = Encoding.UTF8.GetString(plain) == secret;
            for (int i = 0; i < plain.Length; i++) { plain[i] = 0; Marshal.WriteByte(output.pbData, i, 0); }
            return same;
        }
        finally
        {
            Marshal.FreeHGlobal(input.pbData);
            if (output.pbData != IntPtr.Zero) LocalFree(output.pbData);
        }
    }

    // What the server writes its INI with (IniFileSettings::WriteIniSetting_).
    public static void SetIni(string file, string section, string key, string value)
    {
        if (!WritePrivateProfileString(section, key, value, file))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        WritePrivateProfileString(null, null, null, file);
    }

    public static string GetIni(string file, string section, string key)
    {
        StringBuilder value = new StringBuilder(8192);
        GetPrivateProfileString(section, key, "", value, value.Capacity, file);
        return value.ToString();
    }
}
'@

function Get-MsoledbsqlVersion {
    $key = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\MSOLEDBSQL' -ErrorAction SilentlyContinue
    if ($key -and ($key.PSObject.Properties.Name -contains 'InstalledVersion')) { return [string]$key.InstalledVersion }
    return $null
}

function Get-State {
    if (Test-Path $StateFile) { return (Get-Content -Raw $StateFile | ConvertFrom-Json) }
    return $null
}

function Get-ServiceName {
    if ($Service) { return $Service }
    $state = Get-State
    if ($state -and $state.service) { return [string]$state.service }
    Fail "no service is known for this node: run install first, or give -Service"
}

function Wait-Stopped([string]$name, [int]$seconds = 180) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $s = Get-Service -Name $name -ErrorAction SilentlyContinue
        $p = Get-CimInstance Win32_Process -Filter ("ExecutablePath = '" + ($Exe -replace '\\', '\\') + "'") -ErrorAction SilentlyContinue
        if ((-not $s -or $s.Status -eq 'Stopped') -and -not $p) { return }
        Start-Sleep -Milliseconds 500
    }
    Fail "$name did not stop within $seconds s"
}

function Stop-Node([string]$name) {
    $s = Get-Service -Name $name -ErrorAction SilentlyContinue
    if (-not $s) { return }
    if ($s.Status -ne 'Stopped') {
        & sc.exe stop $name | Out-Null
    }
    # The process ends after the service says stopped (the uninstaller's lesson of
    # 25 September 2026): wait for both.
    Wait-Stopped $name
}

# ---------------------------------------------------------------- turns (one installation per machine)

function Get-Anchor {
    $key = Get-ItemProperty $AnchorKey -ErrorAction SilentlyContinue
    if ($key -and ($key.PSObject.Properties.Name -contains 'InstallLocation')) { return [string]$key.InstallLocation }
    return $null
}

function Set-Anchor([string]$value) {
    if (-not (Test-Path $AnchorKey)) { New-Item -Path $AnchorKey -Force | Out-Null }
    Set-ItemProperty -Path $AnchorKey -Name InstallLocation -Value $value
}

function Get-Beside {
    $state = Get-State
    if ($state -and ($state.PSObject.Properties.Name -contains 'beside') -and $state.beside) { return $state.beside }
    return $null
}

function Get-StartConfig([string]$name) {
    $key = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$name"
    $delayed = 0
    if ($key.PSObject.Properties.Name -contains 'DelayedAutostart') { $delayed = [int]$key.DelayedAutostart }
    return @{ start = [int]$key.Start; delayed = $delayed }
}

function Set-StartConfig([string]$name, [int]$start, [int]$delayed) {
    $word = switch ($start) { 2 { if ($delayed) { 'delayed-auto' } else { 'auto' } } 3 { 'demand' } 4 { 'disabled' } default { 'demand' } }
    & sc.exe config $name start= $word | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "sc config $name start= $word failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

function Enter-Turn {
    $beside = Get-Beside
    if (-not $beside) { return $false }
    if (Test-Path $TurnFile) { return $false }
    $other = [string]$beside.service
    $svc = Get-Service -Name $other -ErrorAction SilentlyContinue
    $config = Get-StartConfig $other
    # Written before anything changes, so that release can put back whatever a
    # failure part of the way through left.
    @{ install_location = (Get-Anchor); service = $other; start = $config.start; delayed = $config.delayed
        was_running = [bool]($svc -and $svc.Status -eq 'Running'); since = (Get-Date).ToString('s') } |
        ConvertTo-Json | Set-Content -Path $TurnFile -Encoding ASCII
    Set-StartConfig $other 4 0
    if ($svc -and $svc.Status -ne 'Stopped') {
        & sc.exe stop $other | Out-Null
        $otherExe = ((Get-CimInstance Win32_Service -Filter "Name = '$other'").PathName -replace '^"([^"]+)".*$', '$1')
        $deadline = (Get-Date).AddSeconds(180)
        while ((Get-Date) -lt $deadline) {
            $s = Get-Service -Name $other
            $p = Get-CimInstance Win32_Process -Filter ("ExecutablePath = '" + ($otherExe -replace '\\', '\\') + "'") -ErrorAction SilentlyContinue
            if ($s.Status -eq 'Stopped' -and -not $p) { break }
            Start-Sleep -Milliseconds 500
        }
        if ((Get-Service -Name $other).Status -ne 'Stopped') { Fail "$other did not stop; run release" }
    }
    Set-Anchor $Root
    Say "turn: the lab's - $other stopped and disabled, InstallLocation $Root"
    return $true
}

function Exit-Turn {
    if (-not (Test-Path $TurnFile)) { return }
    $turn = Get-Content -Raw $TurnFile | ConvertFrom-Json
    Stop-Node (Get-ServiceName)
    if ($turn.install_location) { Set-Anchor ([string]$turn.install_location) }
    Set-StartConfig ([string]$turn.service) ([int]$turn.start) ([int]$turn.delayed)
    if ($turn.was_running) {
        Start-Service -Name ([string]$turn.service)
        (Get-Service -Name ([string]$turn.service)).WaitForStatus('Running', [TimeSpan]::FromSeconds(120))
    }
    Remove-Item -Force $TurnFile
    Say ("turn: given back - InstallLocation {0}, {1} {2}" -f $turn.install_location, $turn.service, $(if ($turn.was_running) { 'running again' } else { 'left stopped, as it was' }))
}

# The lab's service stopped, and the machine given back to the other installation.
function Stop-LabNode([string]$name) {
    Stop-Node $name
    Exit-Turn
}

function Expand-Kit([string]$zip, [string]$into) {
    if (-not (Test-Path $zip)) { Fail "no kit at $zip" }
    if (Test-Path $into) { Remove-Item -Recurse -Force $into }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $into)
    $sums = Join-Path $into 'SHA256SUMS.txt'
    if (-not (Test-Path $sums)) { Fail "the kit has no SHA256SUMS.txt" }
    $bad = 0
    foreach ($line in Get-Content $sums) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { continue }
        $file = Join-Path $into $Matches[2]
        if (-not (Test-Path $file) -or (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower() -ne $Matches[1]) {
            $bad++
            [Console]::Error.WriteLine("checksum: $($Matches[2])")
        }
    }
    if ($bad) { Fail "$bad files of the kit do not match SHA256SUMS.txt" }
}

function Copy-Tree([string]$from, [string]$to, [string[]]$exclude = @()) {
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    $robocopyArgs = @($from, $to, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:2', '/W:2')
    if ($exclude.Count) { $robocopyArgs += '/XF'; $robocopyArgs += $exclude }
    & robocopy.exe @robocopyArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "robocopy $from -> $to failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

function Install-Node {
    if (-not $Kit) { Fail 'install needs -Kit' }
    if (-not $Service) { Fail 'install needs -Service' }
    if ($Register -and $Service -ne 'hMailServer') { Fail '/Register makes the service hMailServer and no other' }
    if (Test-Path $TurnFile) { Fail "a turn is open ($TurnFile): run release first" }
    $anchor = Get-Anchor
    $beside = $null
    if ($anchor -and ($anchor.TrimEnd('\') -ne $Root.TrimEnd('\'))) {
        if ($Register) { Fail "this machine has an installation of its own at ${anchor}: install beside it, without -Register" }
        $beside = @{ service = 'hMailServer'; install_location = $anchor }
    }
    $other = Get-Service -Name $Service -ErrorAction SilentlyContinue
    if ($other) {
        $path = (Get-CimInstance Win32_Service -Filter "Name = '$Service'").PathName
        if ($path -notlike "*$Exe*") { Fail "a service named $Service exists already and is not this lab's ($path)" }
        Stop-Node $Service
    }
    $staging = Join-Path $Root 'lab\kit'
    Expand-Kit $Kit $staging
    foreach ($dir in 'Bin', 'DBScripts', 'Languages', 'WebAdmin', 'Data', 'Logs', 'Temp', 'Database', 'Events', 'lab') {
        New-Item -ItemType Directory -Force -Path (Join-Path $Root $dir) | Out-Null
    }
    Copy-Tree (Join-Path $staging 'Bin') $Bin @('hMailServer.ini')
    foreach ($dir in 'DBScripts', 'Languages', 'WebAdmin') { Copy-Tree (Join-Path $staging $dir) (Join-Path $Root $dir) }
    # The event scripts are the administrator's: put there once, never overwritten.
    if (-not (Get-ChildItem (Join-Path $Root 'Events') -File -ErrorAction SilentlyContinue)) {
        Copy-Tree (Join-Path $staging 'Events') (Join-Path $Root 'Events')
    }
    Copy-Item (Join-Path $staging 'build-info.txt') (Join-Path $Root 'deployed.txt') -Force

    # [Directories] and [GUILanguages], as the installer writes them (section_ini.iss).
    foreach ($pair in @(
            @('Directories', 'ProgramFolder', $Root),
            @('Directories', 'DatabaseFolder', (Join-Path $Root 'Database')),
            @('Directories', 'DataFolder', (Join-Path $Root 'Data')),
            @('Directories', 'LogFolder', (Join-Path $Root 'Logs')),
            @('Directories', 'TempFolder', (Join-Path $Root 'Temp')),
            @('Directories', 'EventFolder', (Join-Path $Root 'Events')),
            @('GUILanguages', 'ValidLanguages', 'english,swedish'))) {
        [HmLabNative]::SetIni($Ini, $pair[0], $pair[1], $pair[2])
    }

    if ($Register) {
        # The anchor the installer writes (section_registry.iss: HKLM32
        # Software\hMailServer InstallLocation), before the first verb reads it.
        Set-Anchor $Root
        foreach ($verb in '/RegisterTypeLib', '/Register') {
            $p = Start-Process -FilePath $Exe -ArgumentList $verb -Wait -PassThru -NoNewWindow
            if ($p.ExitCode -ne 0) { Fail "hMailServer.exe $verb returned $($p.ExitCode)" }
        }
    }
    elseif (-not (Get-Service -Name $Service -ErrorAction SilentlyContinue)) {
        # What ServiceManager::RegisterService makes, under another name: its own
        # process, "<exe>" RunAsService, after RPCSS, LocalSystem.
        & sc.exe create $Service binPath= "`"$Exe`" RunAsService" type= own start= demand error= normal depend= RPCSS DisplayName= "hMailServer (active-active lab node)" | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "sc create $Service failed ($LASTEXITCODE)" }
        & sc.exe description $Service "hMailServer for the active-active lab (build/lab/active-active/windows), on the lab's SQL Server. Started by hand only." | Out-Null
    }
    # Manual: until the active-active units land, WarmStandby.md holds - one service
    # against a database at a time, and the lab starts each node itself.
    & sc.exe config $Service start= demand | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "sc config $Service failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    @{ service = $Service; registered = [bool]$Register; root = $Root; beside = $beside } | ConvertTo-Json | Set-Content -Path $StateFile -Encoding ASCII
    Remove-Item -Recurse -Force $staging
    Say ("installed: {0} ({1}), service {2}{3}, manual start{4}" -f $Root, ((Get-Content (Join-Path $Root 'deployed.txt') | Select-Object -First 1)), $Service,
        $(if ($Register) { ', registered with COM, InstallLocation ' + $Root } else { ', not registered with COM' }),
        $(if ($beside) { '; beside the installation at ' + $beside.install_location + ', so it runs in turns' } else { '' }))
}

function Publish-Node {
    if (-not $Kit) { Fail 'deploy needs -Kit' }
    $name = Get-ServiceName
    if (-not (Test-Path $Ini)) { Fail "no ${Ini}; install first" }
    $staging = Join-Path $Root 'lab\kit'
    Expand-Kit $Kit $staging
    Stop-LabNode $name
    Copy-Tree (Join-Path $staging 'Bin') $Bin @('hMailServer.ini')
    foreach ($dir in 'DBScripts', 'Languages', 'WebAdmin') { Copy-Tree (Join-Path $staging $dir) (Join-Path $Root $dir) }
    Copy-Item (Join-Path $staging 'build-info.txt') (Join-Path $Root 'deployed.txt') -Force
    Remove-Item -Recurse -Force $staging
    Say ("deployed: {0} into {1}; {2} stays stopped" -f ((Get-Content (Join-Path $Root 'deployed.txt') | Select-Object -First 1)), $Root, $name)
}

function Update-Database {
    # The server's own upgrade path (SchemaUpgrader), as `hmailserver
    # --upgrade-database` on Linux. Every node must be stopped first (D16); the verb
    # itself refuses while a service named hMailServer runs on this machine (exit 3).
    # Beside another installation it runs in a turn, which stops that one's service.
    $name = Get-ServiceName
    Stop-Node $name
    $entered = Enter-Turn
    $p = Start-Process -FilePath $Exe -ArgumentList '--upgrade-database' -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $Root 'lab\upgrade-database.out') -RedirectStandardError (Join-Path $Root 'lab\upgrade-database.err')
    Get-Content (Join-Path $Root 'lab\upgrade-database.out'), (Join-Path $Root 'lab\upgrade-database.err') -ErrorAction SilentlyContinue | ForEach-Object { Say "  $_" }
    if ($entered) { Exit-Turn }
    if ($p.ExitCode -ne 0) { Fail "--upgrade-database returned $($p.ExitCode) (1 failed, 2 not configured, 3 a service named hMailServer runs)" }
    Say 'database: upgraded by --upgrade-database (or already current)'
}

function Test-Database {
    $secrets = Read-Secrets
    $values = [ordered]@{}
    foreach ($key in 'Type', 'Server', 'Database', 'Username', 'Port', 'Internal', 'Provider', 'TlsMode', 'PasswordEncryption') {
        $values[$key] = [HmLabNative]::GetIni($Ini, 'Database', $key)
    }
    $sealed = [HmLabNative]::GetIni($Ini, 'Database', 'Password')
    $opens = 'not checked (no AA_NODE_PASSWORD given)'
    if ($secrets.ContainsKey('AA_NODE_PASSWORD')) {
        $opens = if ($values['PasswordEncryption'] -eq '6' -and [HmLabNative]::Opens($sealed, $secrets['AA_NODE_PASSWORD'])) { 'yes' } else { 'NO' }
    }
    Say ("[Database] {0}; password {1} characters, opens to the lab's: {2}" -f (($values.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '), $sealed.Length, $opens)
    if ($opens -eq 'NO') { exit 1 }
}

function Install-Msoledbsql {
    $have = Get-MsoledbsqlVersion
    if ($have -and [int]($have.Split('.')[0]) -ge 18) { Say "MSOLEDBSQL $have is installed"; return }
    if (-not $Msi -or -not (Test-Path $Msi)) { Fail 'msoledbsql needs -Msi <path to msoledbsql.msi>' }
    $sig = Get-AuthenticodeSignature $Msi
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notlike 'CN=Microsoft Corporation,*') { Fail "the MSI is not signed by Microsoft ($($sig.Status))" }
    $log = Join-Path $Root 'lab\msoledbsql-install.log'
    $p = Start-Process -FilePath msiexec.exe -ArgumentList @('/i', "`"$Msi`"", '/qn', '/norestart', 'IACCEPTMSOLEDBSQLLICENSETERMS=YES', '/l*v', "`"$log`"") -Wait -PassThru
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Fail "msiexec returned $($p.ExitCode) (log $log)" }
    $have = Get-MsoledbsqlVersion
    if (-not $have) { Fail "msiexec returned $($p.ExitCode) and no MSOLEDBSQL is registered (log $log)" }
    Say "MSOLEDBSQL $have installed (msiexec $($p.ExitCode))"
}

function Open-Firewall {
    $on = @(Get-NetFirewallProfile | Where-Object { $_.Enabled })
    if (-not $on.Count) { Say 'firewall: every profile is off; nothing opened'; return }
    $rule = 'hmlab-aa: hMailServer lab ports'
    Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $rule -Group 'hmlab-aa' -Direction Inbound -Action Allow -Protocol TCP `
        -LocalPort $LabPorts -Program $Exe -RemoteAddress LocalSubnet -Profile Any | Out-Null
    Say ("firewall: inbound TCP {0} to {1} from the local subnet ({2} profile(s) on)" -f ($LabPorts -join ','), $Exe, $on.Count)
}

function Set-Database {
    $secrets = Read-Secrets
    Need $secrets @('AA_NODE_PASSWORD')
    if (-not ($DbServer -and $Database -and $Login)) { Fail 'set-database needs -DbServer, -Database and -Login' }
    $sealed = [HmLabNative]::Protect($secrets['AA_NODE_PASSWORD'])
    foreach ($pair in @(@('Type', 'MSSQL'), @('Username', $Login), @('Password', $sealed), @('PasswordEncryption', '6'),
            @('Port', '0'), @('Server', $DbServer), @('Database', $Database), @('Internal', '0'))) {
        [HmLabNative]::SetIni($Ini, 'Database', $pair[0], $pair[1])
    }
    if (-not [HmLabNative]::Opens([HmLabNative]::GetIni($Ini, 'Database', 'Password'), $secrets['AA_NODE_PASSWORD'])) {
        Fail 'the sealed password in the INI does not open to the one given'
    }
    Say "[Database]: MSSQL $DbServer, database $Database, login $Login, password sealed for this machine (DPAPI)"
}

function Set-AdminPassword {
    $secrets = Read-Secrets
    Need $secrets @('AA_ADMIN_PASSWORD')
    # The verb writes the INI it finds through InstallLocation: beside another
    # installation, that is the other's unless this is the lab's turn.
    $entered = Enter-Turn
    $anchor = Get-Anchor
    if ($anchor -and ($anchor.TrimEnd('\') -ne $Root.TrimEnd('\'))) { Fail "InstallLocation is $anchor, not ${Root}: the verb would write that installation's INI" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = '--set-admin-password'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.StandardInput.WriteLine($secrets['AA_ADMIN_PASSWORD'])
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($entered) { Exit-Turn }
    if ($p.ExitCode -ne 0) { Fail "--set-admin-password returned $($p.ExitCode): $out" }
    Say "administrator password: written ([Security] AdministratorPassword, PBKDF2) into $Ini"
}

function Connect-Com($secrets) {
    $state = Get-State
    if (-not $state -or -not $state.registered) { Fail 'COM is this node''s only when it was installed with -Register' }
    $app = New-Object -ComObject hMailServer.Application
    if (-not $app.Authenticate('Administrator', $secrets['AA_ADMIN_PASSWORD'])) { Fail 'the administrator password was refused over COM' }
    return $app
}

function New-Database {
    $secrets = Read-Secrets
    Need $secrets @('AA_ADMIN_PASSWORD', 'AA_NODE_PASSWORD')
    if (-not ($DbServer -and $Database -and $Login)) { Fail 'create-database needs -DbServer, -Database and -Login' }
    $app = Connect-Com $secrets
    # DBSetup's way (Unattended.cs): CreateExternalDatabase, then Reinitialize. The
    # port is in the server's name, "host,port", which is how SQL Server's providers
    # read one; the port argument is not used for SQL Server.
    $app.Database.CreateExternalDatabase(2, $DbServer, 0, $Database, $Login, $secrets['AA_NODE_PASSWORD'])
    $app.Reinitialize()
    $deadline = (Get-Date).AddSeconds(60)
    while (-not $app.Database.IsConnected -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    if (-not $app.Database.IsConnected) { Fail 'the server did not connect to the new database' }
    Say ("database: {0} made on {1} by the server (CreateExternalDatabase), schema {2}, type {3}" -f $Database, $DbServer, $app.Database.CurrentVersion, $app.Database.DatabaseType)
}

function Set-LabConfiguration {
    $secrets = Read-Secrets
    Need $secrets @('AA_ADMIN_PASSWORD', 'AA_ACCOUNT_PASSWORD')
    if (-not $SinkHost) { Fail 'configure needs -SinkHost' }
    $app = Connect-Com $secrets
    if (-not $app.Database.IsConnected) { Fail 'the server is not connected to its database' }
    $settings = $app.Settings
    $ports = $settings.TCPIPPorts
    for ($i = 0; $i -lt $ports.Count; $i++) {
        $port = $ports.Item($i)
        if ($PortMoves.ContainsKey([int]$port.PortNumber)) {
            $port.PortNumber = $PortMoves[[int]$port.PortNumber]
            $port.Save()
        }
    }
    $settings.AutoBanOnLogonFailure = $false
    $domain = $null
    try { $domain = $app.Domains.ItemByName('example.test') } catch { $domain = $null }
    if (-not $domain) {
        $domain = $app.Domains.Add()
        $domain.Name = 'example.test'
    }
    $domain.Active = $true
    $domain.Save()
    foreach ($n in 1..4) {
        $address = "account$n@example.test"
        $account = $null
        try { $account = $domain.Accounts.ItemByAddress($address) } catch { $account = $null }
        if (-not $account) {
            $account = $domain.Accounts.Add()
            $account.Address = $address
        }
        $account.Password = $secrets['AA_ACCOUNT_PASSWORD']
        $account.Active = $true
        $account.Save()
    }
    $route = $null
    try { $route = $settings.Routes.ItemByName('sink.test') } catch { $route = $null }
    if (-not $route) {
        $route = $settings.Routes.Add()
        $route.DomainName = 'sink.test'
    }
    $route.TargetSMTPHost = $SinkHost
    $route.TargetSMTPPort = $SinkPort
    $route.NumberOfTries = 5
    $route.MinutesBetweenTry = 1
    $route.TreatRecipientAsLocalDomain = $true
    $route.Save()
    $list = @()
    for ($i = 0; $i -lt $ports.Count; $i++) { $list += [string]$ports.Item($i).PortNumber }
    Say ("configured: listeners {0}; auto-ban off; example.test with account1..4; route sink.test -> {1}:{2}" -f ($list -join ','), $SinkHost, $SinkPort)
}

function Start-Node {
    $name = Get-ServiceName
    $s = Get-Service -Name $name
    if ($s.Status -ne 'Running') {
        $null = Enter-Turn
        $anchor = Get-Anchor
        if ($anchor -and ($anchor.TrimEnd('\') -ne $Root.TrimEnd('\'))) { Fail "InstallLocation is $anchor, not ${Root}: the service would read that installation's INI" }
        Start-Service -Name $name
    }
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline) {
        $listening = @(Get-NetTCPConnection -State Listen -LocalPort $LabPorts -ErrorAction SilentlyContinue |
                Where-Object { (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).Path -eq $Exe })
        if ($listening.Count -ge $LabPorts.Count) { break }
        Start-Sleep -Seconds 1
    }
    Show-Status $name
}

function Show-Status([string]$name) {
    if (-not $name) { $name = Get-ServiceName }
    $s = Get-Service -Name $name -ErrorAction SilentlyContinue
    $proc = Get-CimInstance Win32_Process -Filter ("ExecutablePath = '" + ($Exe -replace '\\', '\\') + "'") -ErrorAction SilentlyContinue | Select-Object -First 1
    $ports = @()
    if ($proc) {
        $ports = @(Get-NetTCPConnection -State Listen -OwningProcess $proc.ProcessId -ErrorAction SilentlyContinue | Sort-Object LocalPort -Unique | ForEach-Object { $_.LocalPort })
    }
    $startType = '-'
    $svc = Get-CimInstance Win32_Service -Filter "Name = '$name'" -ErrorAction SilentlyContinue
    if ($svc) { $startType = $svc.StartMode }
    $deployed = ''
    $info = Join-Path $Root 'deployed.txt'
    if (Test-Path $info) { $deployed = (Get-Content $info | Select-Object -First 1) -replace '^commit=', '' }
    $turn = ''
    $beside = Get-Beside
    if ($beside) {
        $other = Get-Service -Name ([string]$beside.service) -ErrorAction SilentlyContinue
        $turn = "; beside {0} ({1} {2}), {3}" -f $beside.install_location, $beside.service, $(if ($other) { $other.Status } else { 'absent' }),
            $(if (Test-Path $TurnFile) { "the lab's turn (InstallLocation " + (Get-Anchor) + ')' } else { 'not the lab''s turn' })
    }
    Say ("{0}: service {1} {2} ({3}), pid {4}, listening {5}; build {6}{7}" -f $env:COMPUTERNAME, $name,
        $(if ($s) { $s.Status } else { 'absent' }), $startType, $(if ($proc) { $proc.ProcessId } else { '-' }),
        $(if ($ports.Count) { $ports -join ',' } else { 'none' }), $deployed, $turn)
}

function Clear-Data {
    $name = Get-ServiceName
    Stop-LabNode $name
    foreach ($dir in 'Data', 'Logs', 'Temp') {
        $path = Join-Path $Root $dir
        if (Test-Path $path) { Get-ChildItem -Force $path | Remove-Item -Recurse -Force }
    }
    Say "cleared: $Root\Data, Logs and Temp; $name stopped"
}

function Show-Errors {
    $codes = @{}
    Get-ChildItem (Join-Path $Root 'Logs') -Filter 'ERROR_hmailserver_*.log' -ErrorAction SilentlyContinue | ForEach-Object {
        Select-String -Path $_.FullName -Pattern 'HM[0-9]{4}' -AllMatches | ForEach-Object {
            foreach ($m in $_.Matches) { $codes[$m.Value] = 1 + [int]$codes[$m.Value] }
        }
    }
    if (-not $codes.Count) { Say "$($env:COMPUTERNAME): no error codes logged"; return }
    $codes.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Say ("{0,7} {1}" -f $_.Value, $_.Key) }
}

function Invoke-AdoProbe {
    $secrets = Read-Secrets
    Need $secrets @('AA_NODE_PASSWORD')
    if (-not ($DbServer -and $Database -and $Login)) { Fail 'ado-probe needs -DbServer, -Database and -Login' }
    $providers = @()
    if (Get-MsoledbsqlVersion) { $providers += 'MSOLEDBSQL' }
    $providers += 'SQLOLEDB'
    foreach ($provider in $providers) {
        $connection = New-Object -ComObject ADODB.Connection
        $tls = if ($provider -eq 'MSOLEDBSQL') { 'Use Encryption for Data=True;Trust Server Certificate=True;' } else { '' }
        $connection.Open("Provider=$provider;Data Source=`"$DbServer`";Initial Catalog=`"$Database`";$tls", $Login, $secrets['AA_NODE_PASSWORD'])
        $run = {
            param($sql)
            $command = New-Object -ComObject ADODB.Command
            $command.ActiveConnection = $connection
            $command.CommandText = $sql
            # Command.Execute(RecordsAffected, Parameters, adCmdText), the call
            # ADOConnection::TryExecute makes with NULL where AA-01 passes a VARIANT
            # for the count: the first argument by reference, through IDispatch.
            $callArgs = [object[]]@($null, [Type]::Missing, 1)
            $byRef = New-Object System.Reflection.ParameterModifier 3
            $byRef[0] = $true
            $null = [System.__ComObject].InvokeMember('Execute', [Reflection.BindingFlags]::InvokeMethod, $null, $command,
                $callArgs, @($byRef), $null, $null)
            return $callArgs[0]
        }
        $null = $connection.Execute('SET DATEFORMAT ymd')
        $null = $connection.Execute('SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED')
        $null = $connection.Execute('create table #aa_probe (id int not null primary key, owner int not null, counter int not null)')
        $null = $connection.Execute('insert into #aa_probe values (1, 0, 0)')
        $null = $connection.Execute('insert into #aa_probe values (2, 0, 0)')
        $got = [ordered]@{
            match_none          = & $run 'update #aa_probe set counter = counter + 1 where id = 99'
            match_one_changed   = & $run 'update #aa_probe set counter = counter + 1 where id = 1'
            match_one_unchanged = & $run 'update #aa_probe set owner = 0 where id = 1'
            match_two           = & $run 'update #aa_probe set counter = counter + 1 where id in (1, 2)'
            insert              = & $run 'insert into #aa_probe values (3, 0, 0)'
            delete              = & $run 'delete from #aa_probe where id = 3'
            claim_epoch_first   = & $run 'update #aa_probe set owner = 7, counter = 4242 where id = 1 and (owner = 0 or counter = 4242)'
            claim_epoch_again   = & $run 'update #aa_probe set owner = 7, counter = 4242 where id = 1 and (owner = 0 or counter = 4242)'
            claim_plain_again   = & $run 'update #aa_probe set owner = 7 where id = 1 and owner = 0'
        }
        $rs = $connection.Execute("select convert(varchar(33), sysdatetimeoffset(), 126) as offsetnow, convert(varchar(23), getdate(), 126) as localnow, convert(varchar(27), sysutcdatetime(), 126) as utcnow")
        $clock = [ordered]@{ sysdatetimeoffset = [string]$rs.Fields.Item('offsetnow').Value; getdate = [string]$rs.Fields.Item('localnow').Value; sysutcdatetime = [string]$rs.Fields.Item('utcnow').Value; isolation_level = '' }
        $rs.Close()
        # The session's own options without VIEW SERVER STATE, which the nodes'
        # login does not hold (sys.dm_exec_sessions needs it).
        $rs = $connection.Execute('DBCC USEROPTIONS WITH NO_INFOMSGS')
        while (-not $rs.EOF) {
            if ([string]$rs.Fields.Item(0).Value -eq 'isolation level') { $clock['isolation_level'] = [string]$rs.Fields.Item(1).Value }
            $rs.MoveNext()
        }
        $rs.Close()
        $connection.Close()
        $want = [ordered]@{ match_none = 0; match_one_changed = 1; match_one_unchanged = 1; match_two = 2; insert = 1; delete = 1; claim_epoch_first = 1; claim_epoch_again = 1; claim_plain_again = 0 }
        $ok = $true
        foreach ($k in $want.Keys) { if ([int]$got[$k] -ne $want[$k]) { $ok = $false } }
        [ordered]@{ check = 'ado_records_affected'; ok = $ok; node = $env:COMPUTERNAME; provider = $provider; got = $got; want = $want; session = $clock } | ConvertTo-Json -Compress -Depth 4 | ForEach-Object { Say $_ }
    }
}

switch ($Command) {
    'install' { Install-Node }
    'deploy' { Publish-Node }
    'msoledbsql' { Install-Msoledbsql }
    'firewall' { Open-Firewall }
    'set-database' { Set-Database }
    'check-database' { Test-Database }
    'set-admin-password' { Set-AdminPassword }
    'upgrade-database' { Update-Database }
    'create-database' { New-Database }
    'configure' { Set-LabConfiguration }
    'start' { Start-Node }
    'stop' { $n = Get-ServiceName; Stop-LabNode $n; Show-Status $n }
    'release' { $n = Get-ServiceName; Stop-LabNode $n; Show-Status $n }
    'status' { Show-Status (Get-ServiceName) }
    'clear-data' { Clear-Data }
    'errors' { Show-Errors }
    'ado-probe' { Invoke-AdoProbe }
    default { Fail "no command '$Command' (install, release, deploy, msoledbsql, firewall, set-database, check-database, set-admin-password, upgrade-database, create-database, configure, start, stop, status, clear-data, errors, ado-probe)" }
}
exit 0
