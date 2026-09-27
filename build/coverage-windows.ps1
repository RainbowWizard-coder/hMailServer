# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

#Requires -RunAsAdministrator
# The native server's line coverage under the Windows regression suite: the
# server built in this tree, run as a console server (hMailServer.exe /Debug)
# under OpenCppCoverage 0.9.9 (GPL-3.0, a FLOSS tool), and the suite
# told so with HM_CONSOLE_SERVER=1, which skips the fixtures that restart the
# service. It is the script that measured 73.48% on 15 September 2026, then run
# by hand from outside the repository, moved into the tree with its paths made
# parameters and the vendored QuickJS left out, so that the measure can be
# repeated from the tree. It has not been run from the tree yet.
#
# OpenCppCoverage is looked for in C:\Program Files\OpenCppCoverage, or where
# -OpenCppCoverage says. Its installer is published on github.com, so a bench
# gets a copy of a trusted installation, as it gets Inno Setup
# (docs/ContinuousIntegration.md).
#
#   pwsh -NoProfile -File build\coverage-windows.ps1 -Smoke    about ten minutes: proves the chain
#   pwsh -NoProfile -File build\coverage-windows.ps1           the whole suite: 1 h 46 min on 15 September 2026
#   pwsh -NoProfile -File build\coverage-windows.ps1 -Where "class =~ /RegressionTests.IMAP/"
#
# Elevated, because the console server takes the LocalService value off its COM
# AppID for its lifetime and the service is disabled meanwhile, so that a COM
# client cannot start it. Whatever happens, the service is put back.
#
# What it measures: lines, not branches (OpenCppCoverage has no branch mode; the
# branch figure comes from the Linux build, build/ci/linux-coverage.sh). The
# instrumented server is roughly twenty times slower, so a few throughput
# assertions do not hold under it: the pass or fail truth is the ordinary gate on
# the service, and the figure here is which lines ran. Never run it while a gate
# is running on the same machine - it stops the service the gate is using. On
# bench150 take the gate lock first (build\ci\gate-lock.ps1 -Take -Holder
# coverage-<commit>) and release it after, as the gates do.
#
# Writes the transcript, coverage.xml (Cobertura), html\ and the suite's log
# under -OutDir (coverage-windows under the repository root by default).
param(
   [switch]$Smoke,
   [string]$Where = '',
   [string]$OutDir = '',
   [string]$OpenCppCoverage = 'C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe'
)
$ErrorActionPreference = 'Continue'
$repo = (Get-Item $PSScriptRoot).Parent.FullName
if (-not $OutDir) { $OutDir = Join-Path $repo 'coverage-windows' }
$exe = Join-Path $repo 'hmailserver\source\Server\hMailServer\x64\Release\hMailServer.exe'
foreach ($needed in @($OpenCppCoverage, $exe)) {
   if (-not (Test-Path $needed)) { throw "not found: $needed" }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
Remove-Item (Join-Path $OutDir '*') -Recurse -Force -ErrorAction SilentlyContinue
Start-Transcript -Path (Join-Path $OutDir 'coverage-windows.log') -Append | Out-Null
Set-Location $repo
Add-Type -Namespace Win32 -Name Msg -MemberDefinition '[DllImport("user32.dll", SetLastError=true)] public static extern bool PostThreadMessage(uint threadId, uint msg, IntPtr wParam, IntPtr lParam);'
"== coverage (console server) on $((Get-FileHash $exe -Algorithm SHA256).Hash), started $(Get-Date -Format s)"

# The console server with an exact command line. OpenCppCoverage appends a space
# to the arguments it hands its child; a batch file launched by cmd.exe carries
# no such space, and --cover_children reaches the server through it. (The server
# reads its mode from the last non-empty token since 6.3.4, so this is for the
# older binaries too.)
$runDebug = Join-Path $OutDir 'run-debug.cmd'
Set-Content -Path $runDebug -Encoding ascii -Value @(
   '@echo off',
   ('cd /d "{0}"' -f (Split-Path $exe)),
   ('"{0}" /Debug' -f $exe))

$p = $null
# Put back as found at the end - except Disabled, which is what this script leaves
# while it runs: found so, it is an earlier run's that was cut off, and the service
# must be startable now for COM to reach it.
$startType = (Get-Service hMailServer).StartType
if ($startType -eq 'Disabled') {
   "the service was Disabled - left so by an earlier run that was cut off? - and is set to Automatic"
   $startType = 'Automatic'
   Set-Service hMailServer -StartupType Automatic
}
$dnsStored = $false
$dnsHad = $false
$dnsBefore = $null
function Get-AdminApp {
   $app = New-Object -ComObject hMailServer.Application
   if (-not $app.Authenticate('Administrator', 'testar')) { throw "COM refused Administrator/testar" }
   return $app
}
try {
   # A WSL leftover holding a port voids the run, as it voids a gate.
   wsl --shutdown 2>&1 | Out-Null

   # The suite's own resolver. SuiteDns points DNSServer at its zone and restarts
   # the service to make it read it; a console server cannot be restarted, so the
   # value is stored before it starts and SuiteDns checks for it
   # (HM_CONSOLE_SERVER=1). Stored over COM (Settings.SetIniSetting), as SuiteDns
   # says, and not written into hMailServer.ini: since schema 6042 the database is
   # the settings store, a line edited into the file loses to a stored row (HM5804)
   # and a line with no row is adopted as one, which would outlive the run. The
   # service answers COM for this, before it is stopped; what was stored before is
   # put back in the finally block.
   $app = Get-AdminApp
   $dnsHad = @($app.Settings.IniSettingNames -split "`r?`n") -contains 'DNSServer'
   if ($dnsHad) { $dnsBefore = $app.Settings.GetIniSetting('DNSServer') }
   # 127.0.0.1 already stored is the suite's own resolver, left by a run that was cut
   # off before its end - this script's, like the Disabled start type above, or a
   # gate's, whose SuiteDns takes it out at the end. Put back, it would leave the
   # service asking a resolver nobody serves; so it is taken out at the end, as the
   # suite's teardown takes it out.
   if ($dnsHad -and $dnsBefore -eq '127.0.0.1') {
      "DNSServer is already 127.0.0.1 - the suite's resolver, left by a run that was cut off? - and is taken out at the end"
      $dnsHad = $false
      $dnsBefore = $null
   }
   $app.Settings.SetIniSetting('DNSServer', '127.0.0.1')
   $dnsStored = $true
   "DNSServer=127.0.0.1 stored for the run (before: $(if ($dnsHad) { "'$dnsBefore'" } else { 'not set' }))"
   $app = $null
   [GC]::Collect()

   Stop-Service hMailServer -ErrorAction SilentlyContinue
   (Get-Service hMailServer).WaitForStatus('Stopped', '00:01:00')
   Set-Service hMailServer -StartupType Disabled
   "service $((Get-Service hMailServer).Status), start type $((Get-Service hMailServer).StartType)"
   # The service's process outlives the Stopped status by a moment; it gets thirty
   # seconds to close the database and its ports itself before it is ended.
   for ($w = 0; $w -lt 30 -and (Get-Process hMailServer -ErrorAction SilentlyContinue); $w++) { Start-Sleep 1 }
   $stray = Get-Process hMailServer -ErrorAction SilentlyContinue
   if ($stray) { "a stray hMailServer.exe ($($stray.Id -join ', ')) is still running after 30 s; ending it"; $stray | Stop-Process -Force; Start-Sleep 3 }

   $occArgs = @(
      '--sources', (Join-Path $repo 'hmailserver\source\Server'),
      '--excluded_sources', (Join-Path $repo 'libraries'),
      '--excluded_sources', (Join-Path $repo 'hmailserver\source\Server\zlib'),
      '--excluded_sources', (Join-Path $repo 'hmailserver\source\Server\quickjs'),
      '--export_type', ('cobertura:' + (Join-Path $OutDir 'coverage.xml')),
      '--export_type', ('html:' + (Join-Path $OutDir 'html')),
      '--working_dir', (Split-Path $exe),
      '--continue_after_cpp_exception',
      '--optimized_build',
      '--cover_children',
      '--', 'cmd.exe', '/c', $runDebug
   )
   $p = Start-Process -FilePath $OpenCppCoverage -ArgumentList $occArgs -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $OutDir 'occ-stdout.txt') -RedirectStandardError (Join-Path $OutDir 'occ-stderr.txt')
   "OpenCppCoverage pid $($p.Id)"
   $up = $false
   for ($i = 0; $i -lt 90 -and -not $up; $i++) {
      Start-Sleep 2
      if ($p.HasExited) { break }
      try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 25); $c.Close(); $up = $true } catch { }
   }
   "server up on port 25: $up after $($i * 2) s (coverage tool exited: $($p.HasExited))"
   if (-not $up) {
      "SERVER DID NOT COME UP. OpenCppCoverage's last lines:"
      Get-Content (Join-Path $OutDir 'occ-stderr.txt') -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch 'Module:' } | Select-Object -Last 12
      throw "no console server"
   }
   $server = Get-Process hMailServer -ErrorAction SilentlyContinue | Select-Object -First 1
   "console server pid $($server.Id)"

   # COM must reach the console server, not the (disabled) service: the suite lives on it.
   $answered = $false
   for ($i = 0; $i -lt 30 -and -not $answered; $i++) {
      try {
         $app = New-Object -ComObject hMailServer.Application
         $null = $app.Authenticate('Administrator', 'testar')
         "COM answered: version $($app.Version), database $($app.Database.CurrentVersion)"
         $answered = $true
      } catch { Start-Sleep 2 }
   }
   if (-not $answered) { throw "COM did not reach the console server within 60 s: $($Error[0].Exception.Message)" }
   if ((Get-Process hMailServer -ErrorAction SilentlyContinue | Measure-Object).Count -ne 1) { throw "more than one hMailServer.exe: COM started the service?" }

   $env:HM_CONSOLE_SERVER = '1'
   $runArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'build\run-tests.ps1'))
   if ($Smoke) { $Where = 'class =~ /RegressionTests.API.ComPropertyValidation|RegressionTests.AntiSpam.Basics|RegressionTests.AntiSpam.Whitelisting/' }
   if ($Where) { $runArgs += @('-Where', $Where) }
   "-- suite: $(if ($Where) { $Where } else { 'everything' }), started $(Get-Date -Format s)"
   & powershell.exe @runArgs *>&1 | Tee-Object -FilePath (Join-Path $OutDir 'suite.log') | Select-String -Pattern 'Test Count|Overall result|Failed :|^\s+\d+\) ' | Select-Object -First 60 | ForEach-Object { Write-Host $_ }
   "-- suite finished $(Get-Date -Format s)"
   $summary = Select-String -Path (Join-Path $OutDir 'suite.log') -Pattern 'Test Count:' | Select-Object -Last 1
   if ($summary) { "SUMMARY " + $summary.Line.Trim() }
}
catch {
   "FAILED: $($_.Exception.Message)"
}
finally {
   # WM_QUIT to the console server's first thread: it stops as it would on a
   # service stop, and OpenCppCoverage writes its report when its child ends.
   $server = Get-Process hMailServer -ErrorAction SilentlyContinue | Select-Object -First 1
   if ($server) {
      $main = $server.Threads | Sort-Object StartTime | Select-Object -First 1
      "posting WM_QUIT to pid $($server.Id) thread $($main.Id): " + [Win32.Msg]::PostThreadMessage([uint32]$main.Id, 0x12, [IntPtr]::Zero, [IntPtr]::Zero)
   }
   if ($p -and -not $p.WaitForExit(600000)) { "the coverage tool is still up after ten minutes; ending the server"; Get-Process hMailServer -ErrorAction SilentlyContinue | Stop-Process -Force; $p.WaitForExit(60000) | Out-Null }
   $xml = Join-Path $OutDir 'coverage.xml'
   "report written: $(Test-Path $xml)"
   if (Test-Path $xml) {
      [xml]$x = Get-Content $xml
      "LINE COVERAGE: {0:P2} ({1} of {2} lines)" -f ([double]$x.coverage.'line-rate'), $x.coverage.'lines-covered', $x.coverage.'lines-valid'
   }
   # The service back as it was found, then the resolver: what was stored before
   # the run, or nothing (DeleteIniSetting drops the row and the line), as
   # SuiteDns's own teardown does on the service - and a restart, since the server
   # reads DNSServer at its start.
   Set-Service hMailServer -StartupType $startType
   Start-Service hMailServer -ErrorAction SilentlyContinue
   if ($dnsStored) {
      try {
         $app = Get-AdminApp
         if ($dnsHad) { $app.Settings.SetIniSetting('DNSServer', $dnsBefore); "DNSServer put back to '$dnsBefore'" }
         else { $app.Settings.DeleteIniSetting('DNSServer'); "DNSServer taken out of the store" }
         $app = $null
         [GC]::Collect()
         Restart-Service hMailServer -ErrorAction SilentlyContinue
      }
      catch {
         "DNSServer NOT PUT BACK:$($_.Exception.Message) - over COM as Administrator, " +
            $(if ($dnsHad) { "Settings.SetIniSetting('DNSServer', '$dnsBefore')" } else { "Settings.DeleteIniSetting('DNSServer')" }) +
            ", then restart the service"
      }
   }
   Start-Sleep 5
   "service $((Get-Service hMailServer).Status), start type $((Get-Service hMailServer).StartType), $((Get-NetTCPConnection -State Listen -LocalPort 25,143 -ErrorAction SilentlyContinue | Measure-Object).Count) of ports 25/143 listening"
   "== finished $(Get-Date -Format s)"
   Stop-Transcript | Out-Null
}
