# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The TCP ports something listens on, on this machine, and where they were read
   from: dot-sourced by build\preflight-tests.ps1, for its clamd and protocol
   listener checks, and by build\ci\windows-gate.ps1, which starts clamd when
   nothing listens on 3310. It is here, under build\ci, because the windows-gate
   job runs the commit's build\ci alone, taken out of the object store before
   the bench's checkout moves; the pre-flight runs from the checkout, which has
   it too.

   Get-NetTCPConnection is asked first. When it returns no TCP endpoint at all,
   in any state - which no running Windows has, its RPC endpoint mapper listening
   on 135 if nothing else - the CIM provider behind it (MSFT_NetTCPConnection)
   is not answering, and every port would read as closed. That is server1 since
   9 October 2026: Get-NetUDPEndpoint, netstat and .NET see every listener,
   Get-NetTCPConnection nothing, and a restart of WMI did not bring it back, so
   the pre-flight failed clamd and the three protocol listeners on a bench that
   was listening on all of them, and the gate ran clamd's task and waited for a
   clamd that was already listening. The listeners are then read from .NET's
   table (IPGlobalProperties, which asks the IP Helper API, as netstat does),
   and from netstat -ano itself when that has nothing either.

   Get-NetTCPConnection is asked as a job, waited for $TcpListenersCimSeconds
   (15) at most: a provider that does not answer at all, rather than answering
   nothing, is passed over the same way, and holds no caller - the gate's minute
   for clamd among them - for longer than that.

   Get-TcpListeners returns an object with three properties:
     Ports    a HashSet[int] of the listening ports (empty when no source listed
              any, so that every port then reads as closed)
     Source   the source they were read from, for the output
     Why      '' when Get-NetTCPConnection answered, else why each source before
              the one used was passed over
   Windows PowerShell 5.1 and PowerShell 7 alike.
#>

$TcpListenersCimSeconds = 15

function Read-TcpListenersFromCim {
    $all = @()
    $job = $null
    $script:TcpListenersCimTimedOut = $false
    try {
        $job = Get-NetTCPConnection -AsJob -ErrorAction Stop
        if (Wait-Job $job -Timeout $TcpListenersCimSeconds) {
            $all = @(Receive-Job $job -ErrorAction SilentlyContinue)
        } else {
            $script:TcpListenersCimTimedOut = $true
        }
    } catch {
        $all = @()
    } finally {
        if ($null -ne $job) {
            Stop-Job $job -ErrorAction SilentlyContinue
            Remove-Job $job -Force -ErrorAction SilentlyContinue
        }
    }
    if ($all.Count -eq 0) { return $null }
    $ports = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($connection in $all) {
        if ("$($connection.State)" -eq 'Listen') { [void]$ports.Add([int]$connection.LocalPort) }
    }
    return ,$ports
}

function Read-TcpListenersFromDotNet {
    try {
        $listeners = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
    } catch {
        return $null
    }
    if ($null -eq $listeners -or $listeners.Count -eq 0) { return $null }
    $ports = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($endpoint in $listeners) { [void]$ports.Add([int]$endpoint.Port) }
    return ,$ports
}

function Read-TcpListenersFromNetstat {
    $lines = @()
    try { $lines = @(& netstat.exe -ano -p TCP 2>$null) + @(& netstat.exe -ano -p TCPv6 2>$null) } catch { return $null }
    # A listening row is read by its foreign address, all zeros with port 0,
    # rather than by its state, which netstat writes in the system's language.
    $ports = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($line in $lines) {
        if ("$line" -match '^\s*TCP\s+\S+:(\d+)\s+(?:0\.0\.0\.0:0|\[::\]:0)\s') { [void]$ports.Add([int]$Matches[1]) }
    }
    if ($ports.Count -eq 0) { return $null }
    return ,$ports
}

function Get-TcpListeners {
    $ports = Read-TcpListenersFromCim
    $source = 'Get-NetTCPConnection'
    $why = ''
    if ($null -eq $ports) {
        if ($script:TcpListenersCimTimedOut) {
            $why = "Get-NetTCPConnection did not answer within $TcpListenersCimSeconds s - its CIM provider, MSFT_NetTCPConnection, is not answering"
        } else {
            $why = 'Get-NetTCPConnection returned no TCP endpoint at all - its CIM provider, MSFT_NetTCPConnection, is not answering'
        }
        $ports = Read-TcpListenersFromDotNet
        $source = '.NET (IPGlobalProperties.GetActiveTcpListeners)'
    }
    if ($null -eq $ports) {
        $why += ', and .NET listed no TCP listener'
        $ports = Read-TcpListenersFromNetstat
        $source = 'netstat -ano'
    }
    if ($null -eq $ports) {
        $why += ', and netstat -ano listed none'
        $ports = [System.Collections.Generic.HashSet[int]]::new()
        $source = 'nowhere: no source listed a TCP listener, so every port reads as closed'
    }
    return [pscustomobject]@{ Ports = $ports; Source = $source; Why = $why }
}
