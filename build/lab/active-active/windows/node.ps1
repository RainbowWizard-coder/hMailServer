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

The folders are a real installation's: hMailServer.exe /ProtectFolders, which setup
runs at every installation and upgrade, hardens C:\hmlab-aa and Bin (SYSTEM and the
Administrators full control, Users Read & execute) and makes Data, Logs, Temp,
Database, Events and hMailServer.ini private to SYSTEM and the Administrators. Nothing
here that a LocalSystem service runs or this file trusts - Bin, lab\node.ps1,
lab\turn.json - can be changed by an account that is not an administrator.

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
INI. The lab's own service is Disabled outside a turn for the same reason the other
way round - started by hand, it would read the other installation's INI and run on its
database - and Manual only while a turn is open.

A turn ends by itself. It is given a time when it is opened (-TurnMinutes, 30 by
default; a start of the node while a turn is open renews it), and the scheduled task
'\hmlab-aa\hmlab-aa turn watchdog', which runs 'watchdog' as SYSTEM at the machine's
start and every five minutes, gives a turn back when its time has run out (the driver
cut off part of the way through: a usage limit, a laptop asleep) or when the machine
has started again since it was opened (a reboot for updates). The commands that open,
use or close a turn hold C:\hmlab-aa\lab\turn.lock while they run, so the watchdog
never gives a turn back in the middle of one.

Commands:
  install -Kit <zip> -Service <name> [-Register]
        lay the kit (aa-win.py's build kit: Bin, DBScripts, Languages, Events,
        WebAdmin) out under C:\hmlab-aa, write [Directories] into Bin\hMailServer.ini
        as the installer does, and make the service: with -Register the way a real
        installation does (hMailServer.exe /RegisterTypeLib and /Register - the
        service 'hMailServer' and the COM server - and InstallLocation, as the
        installer writes it), Manual; without it a second service of the given name,
        made as /Register makes one ("<exe>" RunAsService, own process, after RPCSS,
        LocalSystem), beside the installation the machine has (turns, above), Disabled.
        Then, as 'fit' does, the folders hardened and, beside another installation,
        the watchdog task registered.
  fit                      the node made what install makes now, changing only what
                           differs: the service's binary path quoted, its start type
                           (Manual, or Disabled outside a turn beside another
                           installation), the folders hardened (/ProtectFolders, in a
                           turn beside another installation), and beside another
                           installation the watchdog task
  release                  the lab's service stopped and a turn, if one is open,
                           given back
  watchdog                 a turn given back when its time has run out or the machine
                           has started again since it was opened (the scheduled task)
  deploy -Kit <zip>
        the service stopped, Bin's files (hMailServer.ini kept), DBScripts,
        Languages and WebAdmin replaced by the kit's; the service is left stopped
  msoledbsql -Msi <path>   the Microsoft OLE DB Driver 18, unless it is installed
  firewall                 inbound TCP for the lab's ports to hMailServer.exe, from the
                           local subnet, when a firewall profile is on (a rule already
                           as it should be is left as it is); nothing otherwise
  set-database -DbServer <host,port> -Database <name> -Login <name>
        [Database] as the server writes it: Type=MSSQL, the password sealed with DPAPI
        for this machine (CryptProtectData, CRYPTPROTECT_LOCAL_MACHINE, described
        "hMailServer secret", base64; PasswordEncryption=6), as
        IniFileSettings::SetPassword does; nothing is written when [Database] is
        already so and its password opens to the one given
  check-database           [Database] as the INI has it, the password left out, and
                           whether the sealed password opens to AA_NODE_PASSWORD
  set-admin-password       hMailServer.exe --set-admin-password, the password on its
                           input, unless the INI's hash already verifies against it
  upgrade-database         hMailServer.exe --upgrade-database (every node stopped first)
  create-database -DbServer <host,port> -Database <name> -Login <name>
        through COM, as DBSetup does: Database.CreateExternalDatabase (the server's own
        creation path: CREATE DATABASE and the create script), then Reinitialize
  configure -SinkHost <address> -SinkPort <port>
        through COM: the listeners moved to 2525, 2587, 1110 and 1143, auto-ban off, the
        domain example.test with account1..4, and the route sinking sink.test
  start [-TurnMinutes <n>]  the service started, in a turn beside another installation;
                           fails, with the service stopped and the turn given back,
                           unless it listens on all four lab ports within 90 seconds
  stop | status [-Service <name>]
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
    [string]$Msi,
    [string]$DbServer,
    [string]$Database,
    [string]$Login,
    [string]$SinkHost,
    [int]$SinkPort = 14325,
    [int]$TurnMinutes = 30,
    [string]$Root = 'C:\hmlab-aa'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$Bin = Join-Path $Root 'Bin'
$Exe = Join-Path $Bin 'hMailServer.exe'
$Ini = Join-Path $Bin 'hMailServer.ini'
$StateFile = Join-Path $Root 'lab\node.json'
$TurnFile = Join-Path $Root 'lab\turn.json'
$LockFile = Join-Path $Root 'lab\turn.lock'
$WatchdogLog = Join-Path $Root 'lab\watchdog.log'
$WatchdogTask = 'hmlab-aa turn watchdog'
$WatchdogPath = '\hmlab-aa\'
$AnchorKey = 'HKLM:\SOFTWARE\WOW6432Node\hMailServer'
$LabPorts = @(2525, 2587, 1110, 1143)
$PortMoves = @{ 25 = 2525; 587 = 2587; 110 = 1110; 143 = 1143 }

# The watchdog runs every five minutes: with no turn open it has nothing to do, and
# leaves before the native helpers below are compiled.
if ($Command -eq 'watchdog' -and -not (Test-Path $TurnFile)) { exit 0 }

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

    // HashCreator::ValidatePBKDF2: "$h1$<iterations>$<salt hex>$<key hex>", PBKDF2 with
    // HMAC-SHA256 over the password's UTF-8, compared in constant time.
    public static bool Pbkdf2Verifies(string stored, string password)
    {
        if (stored == null || !stored.StartsWith("$h1$")) return false;
        string[] parts = stored.Split('$');
        if (parts.Length != 5) return false;
        int iterations;
        if (!int.TryParse(parts[2], out iterations) || iterations <= 0) return false;
        byte[] salt = FromHex(parts[3]);
        byte[] expected = FromHex(parts[4]);
        if (salt == null || expected == null || salt.Length == 0 || expected.Length == 0) return false;
        byte[] plain = Encoding.UTF8.GetBytes(password);
        try
        {
            using (var kdf = new System.Security.Cryptography.Rfc2898DeriveBytes(plain, salt, iterations,
                       System.Security.Cryptography.HashAlgorithmName.SHA256))
            {
                byte[] actual = kdf.GetBytes(expected.Length);
                int difference = 0;
                for (int i = 0; i < expected.Length; i++) difference |= actual[i] ^ expected[i];
                Array.Clear(actual, 0, actual.Length);
                return difference == 0;
            }
        }
        finally
        {
            Array.Clear(plain, 0, plain.Length);
        }
    }

    private static byte[] FromHex(string text)
    {
        if (text.Length % 2 != 0) return null;
        byte[] bytes = new byte[text.Length / 2];
        for (int i = 0; i < bytes.Length; i++)
        {
            int high = Convert.ToInt32(text.Substring(2 * i, 1), 16);
            int low = Convert.ToInt32(text.Substring(2 * i + 1, 1), 16);
            bytes[i] = (byte) (high * 16 + low);
        }
        return bytes;
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

# The command's hold on the turn, for as long as it runs: the file is opened for this
# process alone, and Windows closes it when the process ends, however it ends.
$script:TurnLock = $null
function Lock-Turn([int]$seconds) {
    $folder = Split-Path $LockFile
    if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        try {
            $script:TurnLock = [System.IO.File]::Open($LockFile, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            return $true
        }
        catch {
            if ((Get-Date) -ge $deadline) { return $false }
            Start-Sleep -Milliseconds 500
        }
    }
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

# ---------------------------------------------------------------- the service

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

# The binary path /Register writes (ServiceManager::RegisterService), quoted. It is
# set through the service controller (Win32_Service.Change, ChangeServiceConfig), not
# sc.exe: Windows PowerShell 5.1 drops the quotes inside an argument it passes to a
# program, and an unquoted path with a space in it is one Windows reads as several.
function Get-WantedPath { return ('"{0}" RunAsService' -f $Exe) }

function Repair-ServicePath([string]$name) {
    $svc = Get-CimInstance Win32_Service -Filter "Name = '$name'"
    $want = Get-WantedPath
    if ($svc.PathName -eq $want) { return $false }
    $result = Invoke-CimMethod -InputObject $svc -MethodName Change -Arguments @{ PathName = $want }
    if ($result.ReturnValue -ne 0) { Fail "the binary path of $name could not be set (Win32_Service.Change returned $($result.ReturnValue))" }
    $now = (Get-CimInstance Win32_Service -Filter "Name = '$name'").PathName
    if ($now -ne $want) { Fail "the binary path of $name is $now, not $want" }
    Say "service ${name}: binary path $want (it was $($svc.PathName))"
    return $true
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

function Read-Turn {
    if (-not (Test-Path $TurnFile)) { return $null }
    return (Get-Content -Raw $TurnFile | ConvertFrom-Json)
}

function Get-TurnField($turn, [string]$name) {
    if ($turn -and ($turn.PSObject.Properties.Name -contains $name)) { return $turn.$name }
    return $null
}

# A time turn.json holds, as UTC: written as ISO 8601 ('o'); Windows PowerShell keeps it
# a string, PowerShell 7 makes it a date.
function ConvertTo-Utc($value) {
    if ($null -eq $value -or [string]$value -eq '') { return $null }
    if ($value -is [DateTime]) { return $value.ToUniversalTime() }
    return [DateTime]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
}

function Write-Turn($fields) {
    $fields | ConvertTo-Json | Set-Content -Path $TurnFile -Encoding ASCII
}

function Enter-Turn {
    $beside = Get-Beside
    if (-not $beside) { return $false }
    if (Test-Path $TurnFile) { return $false }
    $other = [string]$beside.service
    $lab = Get-ServiceName
    $svc = Get-Service -Name $other -ErrorAction SilentlyContinue
    $config = Get-StartConfig $other
    $now = Get-Date
    # Written before anything changes, so that release (or the watchdog) can put back
    # whatever a failure part of the way through left. The machine's start time is
    # kept so that the watchdog knows when the machine has started again since.
    Write-Turn ([ordered]@{ install_location = (Get-Anchor); service = $other; start = $config.start; delayed = $config.delayed
        was_running = [bool]($svc -and $svc.Status -eq 'Running'); since = $now.ToString('s')
        since_utc = $now.ToUniversalTime().ToString('o'); until_utc = $now.ToUniversalTime().AddMinutes($TurnMinutes).ToString('o')
        boot_utc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'); opened_by = $Command })
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
    # The lab's service may be started from now on, and only now: InstallLocation names
    # its own folder.
    Set-StartConfig $lab 3 0
    Say ("turn: the lab's until {0:HH:mm} - {1} stopped and disabled, InstallLocation {2}, {3} startable" -f $now.AddMinutes($TurnMinutes), $other, $Root, $lab)
    return $true
}

# A start of the node while its turn is open renews the turn's time.
function Set-TurnDeadline {
    $turn = Read-Turn
    if (-not $turn) { return }
    $fields = [ordered]@{}
    foreach ($p in $turn.PSObject.Properties) { $fields[$p.Name] = $p.Value }
    $fields['until_utc'] = (Get-Date).ToUniversalTime().AddMinutes($TurnMinutes).ToString('o')
    Write-Turn $fields
    Say ("turn: renewed until {0:HH:mm}" -f (Get-Date).AddMinutes($TurnMinutes))
}

function Exit-Turn {
    $turn = Read-Turn
    if (-not $turn) { return }
    $lab = Get-ServiceName
    Stop-Node $lab
    # Disabled before InstallLocation moves back, so that there is no moment in which
    # the lab's service could start and read the other installation's INI.
    Set-StartConfig $lab 4 0
    if ($turn.install_location) { Set-Anchor ([string]$turn.install_location) }
    Set-StartConfig ([string]$turn.service) ([int]$turn.start) ([int]$turn.delayed)
    if ($turn.was_running) {
        Start-Service -Name ([string]$turn.service)
        (Get-Service -Name ([string]$turn.service)).WaitForStatus('Running', [TimeSpan]::FromSeconds(120))
    }
    Remove-Item -Force $TurnFile
    Say ("turn: given back - InstallLocation {0}, {1} {2}, {3} disabled" -f $turn.install_location, $turn.service,
        $(if ($turn.was_running) { 'running again' } else { 'left stopped, as it was' }), $lab)
}

# The lab's service stopped, and the machine given back to the other installation;
# beside one, the lab's service is left Disabled whether a turn was open or not.
function Stop-LabNode([string]$name) {
    Stop-Node $name
    Exit-Turn
    if (Get-Beside) {
        if ((Get-StartConfig $name).start -ne 4) {
            Set-StartConfig $name 4 0
            Say "service ${name}: disabled (outside the lab's turn)"
        }
    }
}

function Write-WatchdogLog([string]$text) {
    Add-Content -Path $WatchdogLog -Value ('{0} {1}' -f (Get-Date).ToString('s'), $text) -Encoding ASCII
}

function Invoke-Watchdog {
    # Another command holds the turn: it is in the middle of opening, using or closing
    # it, and the next round looks again.
    if (-not (Lock-Turn 5)) { return }
    $turn = Read-Turn
    if (-not $turn) { return }
    $since = ConvertTo-Utc (Get-TurnField $turn 'since_utc')
    if (-not $since) { $since = ([DateTime]::Parse([string]$turn.since, [Globalization.CultureInfo]::InvariantCulture)).ToUniversalTime() }
    $until = ConvertTo-Utc (Get-TurnField $turn 'until_utc')
    if (-not $until) { $until = $since.AddMinutes(30) }
    # The machine's start time now against the one the turn was opened in: a reboot
    # moves it by the whole uptime there was. (Windows keeps it as the clock less the
    # time since the start, so a clock set far forward - a VM resumed after hours -
    # moves it too, and a turn whose driver slept that long is given back as well.)
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
    $bootThen = ConvertTo-Utc (Get-TurnField $turn 'boot_utc')
    $now = (Get-Date).ToUniversalTime()
    $why = $null
    if ($bootThen -and [Math]::Abs(($boot - $bootThen).TotalMinutes) -gt 2) {
        $why = 'the machine has started again since it was opened (its start {0:s} UTC, {1:s} UTC then)' -f $boot, $bootThen
    }
    elseif (-not $bootThen -and $boot -gt $since) { $why = 'the machine started again at {0:s} UTC, after it was opened' -f $boot }
    elseif ($now -gt $until) { $why = 'its time ran out at {0:s} UTC' -f $until }
    if (-not $why) { return }
    Write-WatchdogLog ('the turn opened at {0:s} UTC by {1} is given back: {2}' -f $since, (Get-TurnField $turn 'opened_by'), $why)
    try {
        Stop-LabNode (Get-ServiceName)
        Write-WatchdogLog ('given back: InstallLocation {0}, {1} {2}' -f (Get-Anchor), $turn.service, (Get-Service -Name ([string]$turn.service)).Status)
    }
    catch {
        Write-WatchdogLog ('giving it back failed: {0}' -f $_.Exception.Message)
        throw
    }
}

function Register-Watchdog {
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" watchdog' -f (Join-Path $Root 'lab\node.ps1')
    $task = Get-ScheduledTask -TaskName $WatchdogTask -TaskPath $WatchdogPath -ErrorAction SilentlyContinue
    if ($task) {
        $action = @($task.Actions)
        $triggers = @($task.Triggers)
        $startup = @($triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskBootTrigger' })
        $repeat = @($triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskTimeTrigger' -and $_.Repetition.Interval -eq 'PT5M' -and -not $_.Repetition.Duration })
        if ($action.Count -eq 1 -and $action[0].Execute -eq $powershell -and $action[0].Arguments -eq $arguments -and
            $triggers.Count -eq 2 -and $startup.Count -eq 1 -and $repeat.Count -eq 1 -and
            $task.Principal.UserId -eq 'SYSTEM' -and [string]$task.Principal.RunLevel -eq 'Highest' -and [string]$task.State -ne 'Disabled') {
            Say "watchdog: $WatchdogPath$WatchdogTask is registered as it should be"
            return
        }
    }
    $taskAction = New-ScheduledTaskAction -Execute $powershell -Argument $arguments
    $taskTriggers = @((New-ScheduledTaskTrigger -AtStartup),
        (New-ScheduledTaskTrigger -Once -At (Get-Date).Date -RepetitionInterval (New-TimeSpan -Minutes 5)))
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $WatchdogTask -TaskPath $WatchdogPath -Action $taskAction -Trigger $taskTriggers -Principal $principal `
        -Settings $settings -Force -Description ('Gives the active-active lab node''s turn back (build/lab/active-active/windows/node.ps1 watchdog): at the start ' +
        'of the machine, and every five minutes once the turn''s time has run out.') | Out-Null
    Say "watchdog: $WatchdogPath$WatchdogTask registered (SYSTEM, at start-up and every five minutes: node.ps1 watchdog)"
}

# ---------------------------------------------------------------- the folders

function Test-FoldersProtected {
    foreach ($path in @($Root, $Bin, $Ini, (Join-Path $Root 'Data'), (Join-Path $Root 'Logs'), (Join-Path $Root 'Temp'),
            (Join-Path $Root 'Database'), (Join-Path $Root 'Events'))) {
        if ((Test-Path $path) -and -not (Get-Acl $path).AreAccessRulesProtected) { return $false }
    }
    return $true
}

# hMailServer.exe /ProtectFolders, as setup runs it at every installation and upgrade.
# It finds the folders from where the program is, but reads hMailServer.INI and the
# database InstallLocation names, so beside another installation it runs in a turn.
function Protect-Folders {
    if (Test-FoldersProtected) { Say "folders: hardened already (/ProtectFolders)"; return }
    $entered = Enter-Turn
    $anchor = Get-Anchor
    if ($anchor -and ($anchor.TrimEnd('\') -ne $Root.TrimEnd('\'))) { Fail "InstallLocation is $anchor, not ${Root}: /ProtectFolders would read that installation's INI" }
    $out = Join-Path $Root 'lab\protect-folders.out'
    $p = Start-Process -FilePath $Exe -ArgumentList '/ProtectFolders' -Wait -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError ($out + '.err')
    Get-Content $out, ($out + '.err') -ErrorAction SilentlyContinue | Where-Object { $_ -match '^(  (made private|hardened|already|CHECK)|Done:)' } | ForEach-Object { Say "  $_" }
    if ($entered) { Exit-Turn }
    if ($p.ExitCode -ne 0) { Fail "/ProtectFolders returned $($p.ExitCode) (1 a folder failed, 2 not in a Bin folder, 3 not elevated): $out" }
    if (-not (Test-FoldersProtected)) { Fail "/ProtectFolders returned 0, and a folder still inherits its permissions: $out" }
    Say 'folders: hardened by /ProtectFolders'
}

# What install makes now, on a node installed before: only what differs is changed.
function Set-NodeFit {
    $name = Get-ServiceName
    if (-not (Get-Service -Name $name -ErrorAction SilentlyContinue)) { Fail "no service ${name}: run install" }
    $null = Repair-ServicePath $name
    $beside = Get-Beside
    $want = if ($beside -and -not (Test-Path $TurnFile)) { 4 } else { 3 }
    if ((Get-StartConfig $name).start -ne $want) {
        if ($want -eq 4) { Stop-Node $name }
        Set-StartConfig $name $want 0
        Say ("service {0}: {1}" -f $name, $(if ($want -eq 4) { 'Disabled (outside the lab''s turn)' } else { 'Manual' }))
    }
    Protect-Folders
    if ($beside) { Register-Watchdog }
    Say "fit: $name as install makes it"
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
        # process, "<exe>" RunAsService (quoted: New-Service passes the path to the
        # service controller as it is), after RPCSS, LocalSystem. Disabled: outside
        # the lab's turn it would read the other installation's INI.
        New-Service -Name $Service -BinaryPathName (Get-WantedPath) -DisplayName 'hMailServer (active-active lab node)' -StartupType Disabled `
            -DependsOn 'RPCSS' -Description ('hMailServer for the active-active lab (build/lab/active-active/windows), on the lab''s SQL Server. ' +
            'Started by node.ps1 alone, in the lab''s turn; Disabled outside it.') | Out-Null
    }
    $null = Repair-ServicePath $Service
    # Manual on a machine of its own: until the active-active units land,
    # WarmStandby.md holds - one service against a database at a time, and the lab
    # starts each node itself. Beside another installation: Disabled outside a turn.
    Set-StartConfig $Service $(if ($beside) { 4 } else { 3 }) 0
    @{ service = $Service; registered = [bool]$Register; root = $Root; beside = $beside } | ConvertTo-Json | Set-Content -Path $StateFile -Encoding ASCII
    Remove-Item -Recurse -Force $staging
    Say ("installed: {0} ({1}), service {2}{3}, {4}{5}" -f $Root, ((Get-Content (Join-Path $Root 'deployed.txt') | Select-Object -First 1)), $Service,
        $(if ($Register) { ', registered with COM, InstallLocation ' + $Root } else { ', not registered with COM' }),
        $(if ($beside) { 'Disabled outside the lab''s turn' } else { 'manual start' }),
        $(if ($beside) { '; beside the installation at ' + $beside.install_location + ', so it runs in turns' } else { '' }))
    Protect-Folders
    if ($beside) { Register-Watchdog }
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

# The lab's rule as it should be: one rule of that name, inbound TCP to the lab's four
# ports for hMailServer.exe from the local subnet, on every profile, enabled.
function Test-FirewallRule($rule) {
    if ([string]$rule.Direction -ne 'Inbound' -or [string]$rule.Action -ne 'Allow' -or [string]$rule.Enabled -ne 'True' -or
        [string]$rule.Profile -ne 'Any' -or [string]$rule.Group -ne 'hmlab-aa') { return $false }
    $port = $rule | Get-NetFirewallPortFilter
    $application = $rule | Get-NetFirewallApplicationFilter
    $address = $rule | Get-NetFirewallAddressFilter
    if ([string]$port.Protocol -ne 'TCP') { return $false }
    if (((@($port.LocalPort) | ForEach-Object { [int]$_ } | Sort-Object) -join ',') -ne ((@($LabPorts) | Sort-Object) -join ',')) { return $false }
    if ([string]$application.Program -ne $Exe) { return $false }
    if ((@($address.RemoteAddress) -join ',') -ne 'LocalSubnet') { return $false }
    return $true
}

function Open-Firewall {
    $on = @(Get-NetFirewallProfile | Where-Object { $_.Enabled })
    if (-not $on.Count) { Say 'firewall: every profile is off; nothing opened'; return }
    $rule = 'hmlab-aa: hMailServer lab ports'
    $have = @(Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue)
    if ($have.Count -eq 1 -and (Test-FirewallRule $have[0])) {
        Say ("firewall: the rule for TCP {0} to {1} from the local subnet is there as it should be ({2} profile(s) on)" -f ($LabPorts -join ','), $Exe, $on.Count)
        return
    }
    $have | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $rule -Group 'hmlab-aa' -Direction Inbound -Action Allow -Protocol TCP `
        -LocalPort $LabPorts -Program $Exe -RemoteAddress LocalSubnet -Profile Any | Out-Null
    Say ("firewall: inbound TCP {0} to {1} from the local subnet ({2} profile(s) on)" -f ($LabPorts -join ','), $Exe, $on.Count)
}

function Set-Database {
    $secrets = Read-Secrets
    Need $secrets @('AA_NODE_PASSWORD')
    if (-not ($DbServer -and $Database -and $Login)) { Fail 'set-database needs -DbServer, -Database and -Login' }
    $want = [ordered]@{ Type = 'MSSQL'; Username = $Login; PasswordEncryption = '6'; Port = '0'; Server = $DbServer; Database = $Database; Internal = '0' }
    $same = $true
    foreach ($key in $want.Keys) { if ([HmLabNative]::GetIni($Ini, 'Database', $key) -ne $want[$key]) { $same = $false } }
    if ($same -and [HmLabNative]::Opens([HmLabNative]::GetIni($Ini, 'Database', 'Password'), $secrets['AA_NODE_PASSWORD'])) {
        Say "[Database]: MSSQL $DbServer, database $Database, login $Login, the sealed password opens to the one given: unchanged"
        return
    }
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
    # The lab's own INI, read where it is: no turn is needed to read it.
    if ((Test-Path $Ini) -and [HmLabNative]::Pbkdf2Verifies([HmLabNative]::GetIni($Ini, 'Security', 'AdministratorPassword'), $secrets['AA_ADMIN_PASSWORD'])) {
        Say "administrator password: the hash in $Ini already verifies against it: unchanged"
        return
    }
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

function Get-ListeningLabPorts {
    return @(Get-NetTCPConnection -State Listen -LocalPort $LabPorts -ErrorAction SilentlyContinue |
            Where-Object { (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).Path -eq $Exe } |
            ForEach-Object { [int]$_.LocalPort } | Sort-Object -Unique)
}

function Start-Node {
    $name = Get-ServiceName
    $s = Get-Service -Name $name
    $opened = $false
    if ($s.Status -ne 'Running') {
        $opened = Enter-Turn
        $anchor = Get-Anchor
        if ($anchor -and ($anchor.TrimEnd('\') -ne $Root.TrimEnd('\'))) { Fail "InstallLocation is $anchor, not ${Root}: the service would read that installation's INI" }
        if ((Get-StartConfig $name).start -eq 4) { Fail "$name is Disabled: it is started in the lab's turn only (a turn left half made? run release)" }
        try { Start-Service -Name $name }
        catch {
            [Console]::Error.WriteLine("node.ps1: $name did not start: $($_.Exception.Message)")
            Stop-LabNode $name
            Fail "$name did not start; stopped, and the machine given back (see $Root\Logs)"
        }
    }
    if (-not $opened -and (Test-Path $TurnFile)) { Set-TurnDeadline }
    # Each of the four ports, whichever addresses it is bound on: a port on IPv4 and
    # IPv6 is still one port.
    $deadline = (Get-Date).AddSeconds(90)
    $up = @()
    while ((Get-Date) -lt $deadline) {
        $up = @(Get-ListeningLabPorts)
        if ($up.Count -ge $LabPorts.Count) { break }
        Start-Sleep -Seconds 1
    }
    Show-Status $name
    $missing = @($LabPorts | Where-Object { $up -notcontains $_ })
    if ($missing.Count) {
        # A node that is not listening is not left running: it would hold sessions on
        # the database and deliver beside the other, and beside another installation
        # it would keep that one's service down.
        [Console]::Error.WriteLine("node.ps1: $name is not listening on $($missing -join ', ') after 90 s")
        Stop-LabNode $name
        Show-Status $name
        Fail "$name did not come up: not listening on $($missing -join ', ') within 90 s; stopped (see $Root\Logs)"
    }
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
        $open = Read-Turn
        $until = ConvertTo-Utc (Get-TurnField $open 'until_utc')
        $turn = "; beside {0} ({1} {2}), {3}" -f $beside.install_location, $beside.service, $(if ($other) { $other.Status } else { 'absent' }),
            $(if ($open) { "the lab's turn (InstallLocation " + (Get-Anchor) + $(if ($until) { (', until {0:HH:mm})' -f $until.ToLocalTime()) } else { ')' }) } else { 'not the lab''s turn' })
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

# The commands that open, use or close a turn, or change the service, hold the turn's
# lock while they run; the watchdog takes it only when it is free.
if (@('install', 'fit', 'deploy', 'start', 'stop', 'release', 'set-admin-password', 'upgrade-database', 'clear-data') -contains $Command) {
    if (-not (Lock-Turn 900)) { Fail "another node.ps1 has held $LockFile for 15 minutes" }
}

switch ($Command) {
    'install' { Install-Node }
    'fit' { Set-NodeFit }
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
    'watchdog' { Invoke-Watchdog }
    'status' { Show-Status (Get-ServiceName) }
    'clear-data' { Clear-Data }
    'errors' { Show-Errors }
    'ado-probe' { Invoke-AdoProbe }
    default { Fail "no command '$Command' (install, fit, release, watchdog, deploy, msoledbsql, firewall, set-database, check-database, set-admin-password, upgrade-database, create-database, configure, start, stop, status, clear-data, errors, ado-probe)" }
}
exit 0
