# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The self-test of the Windows installer's decision on an installation of the
   original hMailServer's 5.7 (hmailserver\installation\OriginalHMailServer.iss,
   which hMailServerInnoExtension.iss includes and calls first in InitializeSetup).
   The decision is Inno Setup Pascal, which nothing but Inno Setup runs, so this
   compiles original-guard-test.iss - a setup that installs nothing, whose
   InitializeSetup runs the decision over its cases, writes a line each to the file
   /result= names and returns False - with the ISCC given, runs it with /VERYSILENT,
   and reads the file. Setup must end with exit code 1, Inno Setup's for a setup
   that did not initialise, and every case must pass.

   Then it proves that the cases catch a broken decision: two altered copies of
   OriginalHMailServer.iss - the refusal taking in 5708, the last version the two
   projects share, and the fall-back on the original's scripts left out - are each
   compiled and run the same way, and each must fail a case.

   Nothing is installed, and nothing outside -WorkDir (a new folder under the
   temporary folder by default, removed afterwards) is written. The
   windows-packages jobs run it after they find Inno Setup and before they build
   the installer (windows-packages.ps1, step 3).

   Exit codes: 0 every case passed and both altered copies were caught; 1 a case
   failed, or an altered copy was not caught; 2 it could not be compiled or run.

   Usage:
     pwsh -NoProfile -File build\ci\packages\original-guard-test.ps1 -Iscc 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
#>

param(
   [Parameter(Mandatory)][string] $Iscc,
   [string] $WorkDir = ''
)

# Continue, as windows-packages.ps1 has it: under Stop, Windows PowerShell 5.1 turns
# ISCC's own error output into an exception, and every outcome here is decided by
# what was made and what it wrote.
$ErrorActionPreference = 'Continue'
$root = (Get-Item (Split-Path -Parent $PSScriptRoot)).Parent.Parent.FullName
$decision = Join-Path $root 'hmailserver\installation\OriginalHMailServer.iss'
$harness = Join-Path $PSScriptRoot 'original-guard-test.iss'
$ownWorkDir = -not $WorkDir
if ($ownWorkDir) { $WorkDir = Join-Path ([IO.Path]::GetTempPath()) ('hm-original-guard-' + [Guid]::NewGuid().ToString('N').Substring(0, 12)) }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# Compiles a harness and runs it: the result file's lines, or $null when it could
# not be compiled or run as a setup that stops in InitializeSetup.
function Invoke-Harness([string] $name, [string] $script) {
   $out = Join-Path $WorkDir $name
   New-Item -ItemType Directory -Force -Path $out | Out-Null
   $compiled = & $Iscc /Q "/O$out" '/Foriginal-guard-test' $script 2>&1
   if ($LASTEXITCODE -ne 0) {
      $compiled | Select-Object -Last 20 | ForEach-Object { "  $_" } | Write-Host
      Write-Host "  ISCC could not compile $script (exit $LASTEXITCODE)"
      return $null
   }
   $result = Join-Path $out 'result.txt'
   $log = Join-Path $out 'setup.log'
   $setup = Start-Process -FilePath (Join-Path $out 'original-guard-test.exe') -Wait -PassThru `
      -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$log`"", "/result=`"$result`""
   if ($setup.ExitCode -ne 1) {
      Write-Host "  the self-test's setup ended with exit code $($setup.ExitCode), not 1: it did not stop in InitializeSetup"
      return $null
   }
   if (-not (Test-Path -LiteralPath $result)) {
      Write-Host "  the self-test's setup wrote no result ($result)"
      return $null
   }
   return @(Get-Content -LiteralPath $result | Where-Object { $_ })
}

function Get-Failed([string[]] $lines) {
   $last = $lines | Where-Object { $_ -match '^CASES (\d+) FAILED (\d+)$' } | Select-Object -Last 1
   if (-not $last) { return -1 }
   $null = $last -match '^CASES (\d+) FAILED (\d+)$'
   return [int] $Matches[2]
}

# The whole test: 0, 1 or 2, as the exit codes above.
function Test-Guard {
   Write-Host "--- the decision on the original hMailServer's 5.7"
   $lines = Invoke-Harness 'decision' $harness
   if ($null -eq $lines) { return 2 }
   $lines | ForEach-Object { Write-Host "  $_" }
   $failed = Get-Failed $lines
   if ($failed -ne 0) {
      Write-Host "  the self-test failed $failed case(s), or wrote no count"
      return 1
   }

   Write-Host "--- the cases catch a broken decision"
   $text = Get-Content -LiteralPath $decision -Raw
   $harnessText = Get-Content -LiteralPath $harness -Raw
   $include = '#include "..\..\..\hmailserver\installation\OriginalHMailServer.iss"'
   if (-not $harnessText.Contains($include)) {
      Write-Host "  original-guard-test.iss does not include the decision as the controls expect"
      return 2
   }
   $controls = [ordered]@{
      'the refusal taking in 5708' = @('(Version > ORIGINAL_LAST_SHARED_SCHEMA)', '(Version >= ORIGINAL_LAST_SHARED_SCHEMA)')
      'the fall-back on the original''s scripts left out' = @('else if OriginalScripts then', 'else if False then')
   }
   $status = 0
   $index = 0
   foreach ($control in $controls.GetEnumerator()) {
      $index++
      $old, $new = $control.Value
      if (-not $text.Contains($old)) {
         Write-Host "  $($control.Key): the text this control alters is not in OriginalHMailServer.iss; update the control"
         $status = 1
         continue
      }
      $folder = Join-Path $WorkDir "control$index"
      New-Item -ItemType Directory -Force -Path $folder | Out-Null
      Set-Content -LiteralPath (Join-Path $folder 'OriginalHMailServer.iss') -Value $text.Replace($old, $new) -NoNewline -Encoding utf8
      $alteredHarness = Join-Path $folder 'original-guard-test.iss'
      Set-Content -LiteralPath $alteredHarness -Value $harnessText.Replace($include, '#include "OriginalHMailServer.iss"') -NoNewline -Encoding utf8
      $lines = Invoke-Harness "control$index-out" $alteredHarness
      if ($null -eq $lines) { return 2 }
      $failed = Get-Failed $lines
      if ($failed -gt 0) {
         Write-Host "  ok    $($control.Key): $failed case(s) failed"
      } else {
         Write-Host "  FAIL  $($control.Key): no case failed"
         $status = 1
      }
   }
   return $status
}

$status = 2
try {
   $status = Test-Guard
} catch {
   Write-Host "  $_"
   $status = 2
} finally {
   if ($ownWorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($status -eq 0) { "ORIGINAL GUARD PASSED: every case, and both altered copies caught" }
elseif ($status -eq 1) { "ORIGINAL GUARD FAILED" }
else { "ORIGINAL GUARD NOT RUN: it could not be compiled or run" }
exit $status
