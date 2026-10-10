# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The Windows regression gate on a bench whose hMailServer service is registered
   from the bench's own checkout: check the exact commit out, move the bench
   database forward through the running service, build the server and the tests,
   pre-flight, watch the idle server, run the suite. It is the second bench's
   remote-gate.ps1 (the split gate's remote half, run over SSH since 16 September
   2026) versioned in the tree, and what GitLab's windows-gate job runs on bench150
   (.gitlab-ci.yml; docs\ContinuousIntegration.md).

   It does not fetch: the commit must already be in the checkout. The windows-gate
   job fetches it from GitLab before this runs; the split gate brings it by bundle.
   It does not take the bench's gate lock either (build\ci\gate-lock.ps1): whoever
   runs it holds the lock around it.

   -Where   an NUnit --where expression, for a subset: a half of the split gate, or
            one fixture. Empty - the default, and what the CI job passes unless
            HM_GATE_WHERE says otherwise - runs the whole suite in one run.

   Exit codes, the first seven as remote-gate.ps1 had them:
     0  the suite ran and nothing failed          5  the checkout is not the commit
     1  the suite ran and tests failed            6  the tests did not build
     2  pre-flight failed                         7  the test assembly is stale
     3  the server did not build                  8  the bench database upgrade failed
     4  the suite did not run, or ran no test     9  the idle server logs errors on its own

   Results, for the job's artifacts, in -ResultsDirectory (emptied first):
     gate.log              everything the run printed
     TestResult.xml        NUnit's own result file
     junit.xml             the same as JUnit XML (build\ci\nunit-to-junit.ps1)
     build-*.log           the MSBuild logs of the server and the tests
     ERROR_*.log           the server's error logs written during the gate
     server-logs.zip       the server's logs written during the gate, when it failed
     crash-oracle-before-this-gate.log
                           the crash oracle's records the binary before this
                           gate's left behind, moved aside before this gate's
                           binary starts (Move-EarlierCrashRecords)
     server-modules.txt    when it failed and the crash oracle recorded a fault:
                           the running service's modules, and every address in
                           the records placed in them (Save-CrashEvidence)
     crash-events.txt      when it failed: the Application log's crash events of
                           the gate - 1000, 1001, 1026 - of any process, the test
                           agent's included

   Usage:
     pwsh -NoProfile -File build\ci\windows-gate.ps1 -Commit <sha> [-Where <expr>]
#>

param(
   [Parameter(Mandatory)][string] $Commit,
   [string] $Where = $env:HM_GATE_WHERE,
   [string] $BenchDir = 'C:\bench\hmailserver',
   [string] $ResultsDirectory = 'C:\bench\gate-results',
   [string] $LogFolder = 'C:\HMTest\Logs',
   [string] $LibsDir = 'C:\Dev\hMailLibs'
)

$ErrorActionPreference = 'Continue'
$started = Get-Date
$here = $PSScriptRoot
# The PowerShell running this, for the scripts it starts: by its path, because the
# runner's LocalSystem has only the machine PATH, and a PowerShell 7 installed from
# the Store is on no PATH but its installing user's.
$shell = (Get-Process -Id $PID).Path

if (Test-Path -LiteralPath $ResultsDirectory) { Remove-Item -LiteralPath $ResultsDirectory -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $ResultsDirectory | Out-Null
$ResultsDirectory = (Resolve-Path -LiteralPath $ResultsDirectory).ProviderPath

Set-Location -LiteralPath $BenchDir
$whom = [Security.Principal.WindowsIdentity]::GetCurrent().Name
"== bench gate of $Commit ($(if ($Where) { 'subset: ' + $Where } else { 'everything' })) in $BenchDir as $whom, started $(Get-Date -Format s)"

function Start-TheService {
   Start-Service hMailServer -ErrorAction SilentlyContinue
}

# What the job keeps whatever happens: the build logs, the error logs written since
# the gate began, and on a failure every server log written since then.
function Save-Results([bool] $failed) {
   foreach ($log in 'build-Release.log', 'build-tests.log') {
      $file = Join-Path $BenchDir "logs\$log"
      if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).LastWriteTime -ge $started) {
         Copy-Item -LiteralPath $file -Destination $ResultsDirectory -Force
      }
   }
   $written = @(Get-ChildItem -LiteralPath $LogFolder -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started })
   $written | Where-Object { $_.Name -like 'ERROR_*' } | Copy-Item -Destination $ResultsDirectory -Force
   if ($failed -and $written.Count) {
      # 300 MB of text is a zip GitLab would still take; a larger one is named, not sent.
      $small = @($written | Where-Object { $_.Length -le 300MB })
      $written | Where-Object { $_.Length -gt 300MB } | ForEach-Object { "  not kept, too large: $($_.Name) ($([int]($_.Length / 1MB)) MB)" }
      if ($small.Count) {
         Compress-Archive -LiteralPath $small.FullName -DestinationPath (Join-Path $ResultsDirectory 'server-logs.zip') -Force
         "the server's logs of this gate: server-logs.zip ($($small.Count) file(s))"
      }
   }
   if ($failed) {
      Save-CrashEvidence -Written $written
   }
}

# The crash oracle's records (C:\HMTest\Logs\crash-oracle.log) are the server's
# own, and the suite fails the first test that finds one there
# (CrashOracleAsserts.AssertNoMemorySafetyEvents, in every SetUp). A record the
# binary before this gate's wrote - an earlier gate that was killed, the service
# idling since, the database upgrade above, which runs through that binary - is
# not this gate's to fail on: on 28 September 2026 pipeline 2890818010's gate
# failed exactly one test, the first SetUp, on the record a killed gate had left
# at 21:15:40, and the other 3,752 passed. So once that binary has been stopped
# and before this gate's starts, the file is moved aside, printed, and kept with
# the results. A record written after this is this gate's binary's, and fails.
function Move-EarlierCrashRecords([string] $Folder, [string] $Results) {
   $file = Join-Path $Folder 'crash-oracle.log'
   if (-not (Test-Path -LiteralPath $file)) {
      "  no crash-oracle.log left behind"
      return
   }
   $item = Get-Item -LiteralPath $file
   $records = @(Get-Content -LiteralPath $file -ErrorAction SilentlyContinue | Where-Object { $_.Trim() })
   $archived = Join-Path $Folder ('crash-oracle.before-gate.{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))
   try {
      Move-Item -LiteralPath $file -Destination $archived -Force -ErrorAction Stop
   } catch {
      "  crash-oracle.log was left behind and could not be moved aside ($($_.Exception.Message)): the first test will fail on it"
      return
   }
   Copy-Item -LiteralPath $archived -Destination (Join-Path $Results 'crash-oracle-before-this-gate.log') -Force -ErrorAction SilentlyContinue
   "  crash-oracle.log was left behind by the binary before this gate's (last written $($item.LastWriteTime.ToString('s'))): $($records.Count) record(s), moved to $archived and kept as crash-oracle-before-this-gate.log, so that they do not fail this gate's first test:"
   $records | Select-Object -First 40 | ForEach-Object { '    ' + $_.Substring(0, [Math]::Min(600, $_.Length)) }
   if ($records.Count -gt 40) { "    ... and $($records.Count - 40) more" }
}

# What names a fault after the fact, saved when the gate fails. The crash oracle
# records the faulting address, the image base and offset, and the faulting
# thread's frames, but no module names (it may not take the loader lock). A
# module's base is the same in every process of a boot for a system library, and
# for hMailServer.exe in the process that recorded it, so the modules of the
# service still running place the addresses - read now, because a restart moves
# the executable's base and a reboot moves them all. On 28 September 2026 a
# swallowed access violation on a REST worker, and the test agent's own death a
# second later, ended a gate with neither placed.
#   server-modules.txt  the running service's modules, and each record with every
#                       address in it placed as module+offset
#   crash-events.txt    the Application log's crash events since the gate began:
#                       Application Error 1000 and .NET Runtime 1026, which name
#                       the faulting module and offset of any process, the test
#                       agent's included, and Windows Error Reporting 1001
function Save-CrashEvidence($Written, [string] $ProcessName = 'hMailServer', [datetime] $Since = $started, [string] $Results = $ResultsDirectory) {
   $oracle = @($Written | Where-Object { $_.Name -like 'crash-oracle*.log' -and $_.Name -notlike 'crash-oracle.before-gate.*' })
   if ($oracle.Count) {
      $out = Join-Path $Results 'server-modules.txt'
      $lines = @()
      $service = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
      $modules = @()
      if (-not $service) {
         $lines += "no $ProcessName process to read modules from: the addresses stay unplaced"
      } else {
         try {
            $modules = @($service.Modules | ForEach-Object {
               [pscustomobject]@{ Base = [UInt64]$_.BaseAddress.ToInt64(); Size = [UInt64]$_.ModuleMemorySize; Name = $_.ModuleName; Path = $_.FileName; Version = $_.FileVersion }
            } | Sort-Object Base)
         } catch {
            $lines += "the modules of $ProcessName (pid $($service.Id)) could not be read: $($_.Exception.Message)"
         }
         $boot = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).LastBootUpTime
         $lines += "$ProcessName pid $($service.Id), started $($service.StartTime.ToString('s')); the machine booted $(if ($boot) { $boot.ToString('s') } else { '(unknown)' })"
         $lines += "An address in a system library is placed right for any process of this boot; one in hMailServer.exe only for a record whose pid is the one above."
      }
      $lines += ''
      foreach ($file in $oracle) {
         $lines += "== $($file.Name)"
         foreach ($record in @(Get-Content -LiteralPath $file.FullName -ErrorAction SilentlyContinue | Where-Object { $_.Trim() })) {
            $lines += $record
            if (-not $modules.Count) { continue }
            $seen = @{}
            foreach ($match in [regex]::Matches($record, '0x([0-9A-F]{16})')) {
               $hex = $match.Groups[1].Value
               if ($seen.ContainsKey($hex)) { continue }
               $seen[$hex] = $true
               $address = [Convert]::ToUInt64($hex, 16)
               $in = $modules | Where-Object { $address -ge $_.Base -and $address -lt ($_.Base + $_.Size) } | Select-Object -First 1
               if ($in) { $lines += ('   0x{0} = {1}+0x{2:X} ({3})' -f $hex, $in.Name, ($address - $in.Base), $in.Version) }
            }
         }
         $lines += ''
      }
      if ($modules.Count) {
         $lines += 'modules:'
         $lines += $modules | ForEach-Object { '0x{0:X16} {1,10} {2} {3}' -f $_.Base, $_.Size, $_.Path, $_.Version }
      }
      $lines | Set-Content -LiteralPath $out -Encoding utf8
      "the crash oracle recorded a fault: its records, the service's modules and every address placed in them are in server-modules.txt"
   }

   $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001, 1026; StartTime = $Since } -ErrorAction SilentlyContinue |
      Sort-Object TimeCreated | Select-Object -First 200)
   if ($events.Count) {
      $events | ForEach-Object { "== $($_.TimeCreated.ToString('o')) $($_.ProviderName) $($_.Id)"; $_.Message; '' } |
         Set-Content -LiteralPath (Join-Path $Results 'crash-events.txt') -Encoding utf8
      "the Application log's crash events of this gate: crash-events.txt ($($events.Count))"
   }
}

function Stop-Gate([int] $code, [string] $sentence, [switch] $StartService) {
   if ($StartService) { Start-TheService }
   $sentence
   Save-Results $true
   "GATE EXIT=$code  finished $(Get-Date -Format s)"
   exit $code
}

"--- checkout"
git cat-file -e "$Commit^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) { Stop-Gate 5 "WRONG TREE: $Commit is not in $BenchDir - fetch it first" }
git checkout -q --detach $Commit 2>&1 | Select-Object -Last 1 | ForEach-Object { "$_" }
if ((git rev-parse HEAD) -notlike "$Commit*") { Stop-Gate 5 "WRONG TREE: HEAD is $(git rev-parse --short HEAD), not $Commit" }
git log --oneline -1 | ForEach-Object { $_.Substring(0, [Math]::Min(100, $_.Length)) }

"--- bench database"
# It is upgraded THROUGH the running service (upgrade-test-database.ps1 says why),
# so a service an interrupted gate left stopped is started first. The binary that
# starts is the previous gate's, which matches the database it left.
if ((Get-Service hMailServer -ErrorAction SilentlyContinue).Status -ne 'Running') {
   "the service was not running; starting it"
   Start-TheService
   Start-Sleep 5
}
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\upgrade-test-database.ps1 *>&1 | Select-Object -Last 3 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { Stop-Gate 8 "BENCH DATABASE UPGRADE FAILED - no gate" }
Stop-Service hMailServer -ErrorAction SilentlyContinue
Get-Process hMailServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

"--- crash records the binary before this gate's left"
Move-EarlierCrashRecords -Folder $LogFolder -Results $ResultsDirectory

"--- build"
$env:hMailServerLibs = $LibsDir
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build.ps1 -Configuration Release *>&1 | Select-String -Pattern "error C|error LNK|error MIDL|Build succeeded|Build FAILED" | Select-Object -Last 20 | ForEach-Object { "$_" }
$build = $LASTEXITCODE
"BUILD EXIT=$build"
if ($build -ne 0) { Stop-Gate 3 "BUILD FAILED - no gate" -StartService }
$hash = (Get-FileHash 'hmailserver\source\Server\hMailServer\x64\Release\hMailServer.exe' -Algorithm SHA256).Hash
"binary $hash"
# The DLLs, the type library and the WebAdmin files beside the exe, as post-build
# puts them (the service registration it also does is idempotent).
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\post-build.ps1 -Configuration Release *>&1 | Select-Object -Last 1 | ForEach-Object { "$_" }

"--- build-tests"
# Removed first, so that a tree whose test sources are unchanged since the last
# build still gets a fresh assembly: MSBuild would otherwise leave the old one,
# and the staleness check below refuses it.
Remove-Item 'hmailserver\test\RegressionTests\bin\x64\Debug\RegressionTests.dll', 'hmailserver\test\RegressionTests\obj\x64\Debug\RegressionTests.dll' -Force -ErrorAction SilentlyContinue
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build-tests.ps1 *>&1 | Select-String -Pattern "error CS|Build succeeded|Build FAILED|Build failed" | Select-Object -Last 20 | ForEach-Object { "$_" }
$tests = $LASTEXITCODE
"BUILD-TESTS EXIT=$tests"
if ($tests -ne 0) { Stop-Gate 6 "BUILD-TESTS FAILED - no gate" -StartService }
$testDll = Get-Item 'hmailserver\test\RegressionTests\bin\x64\Debug\RegressionTests.dll'
"test assembly $($testDll.LastWriteTime.ToString('s'))"
if ($testDll.LastWriteTime -lt (Get-Date).AddMinutes(-30)) { Stop-Gate 7 "STALE TEST ASSEMBLY - no gate" -StartService }

"--- clamd"
# The listening ports as the pre-flight reads them (tcp-listeners.ps1, beside this
# script): Get-NetTCPConnection, or .NET or netstat -ano where its CIM provider
# answers nothing, as on server1 since 9 October 2026 - there the gate ran clamd's
# task and waited for a clamd that was listening all along. Started when nothing
# listens on 3310, and waited for, a minute at most; the pre-flight says whether it
# came up.
. (Join-Path $here 'tcp-listeners.ps1')
$listeners = Get-TcpListeners
"  listening TCP ports read from $($listeners.Source)$(if ($listeners.Why) { ': ' + $listeners.Why })"
if ($listeners.Ports.Contains(3310)) {
   "  clamd is listening on 3310"
} else {
   "  nothing listens on 3310: starting the clamd task"
   schtasks /run /tn clamd | Out-Null
   # The minute is the clock's, not a count of the sleeps: each look also reads the ports,
   # which can take seconds (Get-TcpListeners bounds the CIM read itself).
   $clock = [System.Diagnostics.Stopwatch]::StartNew()
   do {
      Start-Sleep 5
      $listening = (Get-TcpListeners).Ports.Contains(3310)
   } until ($listening -or $clock.Elapsed.TotalSeconds -ge 60)
   $waited = [int]$clock.Elapsed.TotalSeconds
   if ($listening) { "  clamd is listening on 3310 after $waited s" } else { "  clamd is not listening on 3310 after $waited s" }
}

"--- pre-flight"
& $shell -NoProfile -ExecutionPolicy Bypass -File build\preflight-tests.ps1 -Clean *>&1 | ForEach-Object { "$_" }
$pre = $LASTEXITCODE
"PREFLIGHT EXIT=$pre"
if ($pre -ne 0) { Stop-Gate 2 "PREFLIGHT FAILED - no gate" -StartService }
Restart-Service hMailServer
Start-Sleep 3
"service " + (Get-Service hMailServer).Status

"--- idle server, two minutes"
Start-Sleep 70
$errorLogs = @(Get-ChildItem -Path $LogFolder -Filter 'ERROR_*.log' -ErrorAction SilentlyContinue)
if ($errorLogs.Count) {
   "the server logged an error at start (cleared, and watched again):"
   $errorLogs | ForEach-Object { Get-Content $_.FullName -TotalCount 3 | ForEach-Object { '  ' + $_.Substring(0, [Math]::Min(300, $_.Length)) }; Remove-Item $_.FullName -Force }
   Start-Sleep 70
   $again = @(Get-ChildItem -Path $LogFolder -Filter 'ERROR_*.log' -ErrorAction SilentlyContinue)
   if ($again.Count) {
      $again | ForEach-Object { Get-Content $_.FullName -TotalCount 5 | ForEach-Object { '  ' + $_.Substring(0, [Math]::Min(400, $_.Length)) } }
      Stop-Gate 9 "SERVER LOGS ERRORS ON ITS OWN - no gate"
   }
   "  it did not come back"
} else { "  no error logged" }

"--- the suite, started $(Get-Date -Format s)"
# nunit3-console writes TestResult.xml into the directory it runs in, which is this
# one; a file left by an earlier run must not be read as this run's.
Remove-Item -LiteralPath (Join-Path $BenchDir 'TestResult.xml') -Force -ErrorAction SilentlyContinue
$gateLog = Join-Path $ResultsDirectory 'gate.log'
$runArgs = @(); if ($Where) { $runArgs = @('-Where', $Where) }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\run-tests.ps1 @runArgs *>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $gateLog
$code = $LASTEXITCODE
$summary = Select-String -Path $gateLog -Pattern "Test Count:" | Select-Object -Last 1
if ($summary) { "SUMMARY " + $summary.Line.Trim() }
"NUNIT EXIT=$code after $([int]((Get-Date) - $started).TotalMinutes) min of gate"

$converted = 2
$resultFile = Join-Path $BenchDir 'TestResult.xml'
if (Test-Path -LiteralPath $resultFile) {
   Copy-Item -LiteralPath $resultFile -Destination $ResultsDirectory -Force
   & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'nunit-to-junit.ps1') -Path $resultFile -Out (Join-Path $ResultsDirectory 'junit.xml') | ForEach-Object { "$_" }
   $converted = $LASTEXITCODE
} else {
   "no TestResult.xml: the runner did not finish"
}

if ($code -eq 0 -and $converted -eq 0) {
   Save-Results $false
   "GATE EXIT=0  finished $(Get-Date -Format s)"
   "GATE PASSED"
   exit 0
}
# nunit3-console exits with the number of failed tests, or a negative code when it
# could not run them; the job reports a failed suite as 1 either way it ran.
if ($code -gt 0 -or $converted -eq 1) { Stop-Gate 1 "GATE FAILED: the suite ran and tests failed" }
Stop-Gate 4 "GATE FAILED: the suite did not run to a result (nunit exit $code)"
