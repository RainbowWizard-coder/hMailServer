# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The windows-dotnet job of .gitlab-ci.yml, on bench150: the .NET half of the
   repository - the Control Panel and the setup, migration and import tools -
   built with warnings as errors, published, and tested, and held to the
   formatting dotnet format writes. It is the old GitHub workflow ci.yml's
   control-panel and tools jobs and style.yml's dotnet-format job, in one job,
   in the runner's own checkout of the commit: nothing here touches the bench's
   checkout (C:\bench\hmailserver), its build or its service.

   The steps, each as the old jobs ran it:
     1. the Control Panel's settings search index is what
        build\generate-settings-index.ps1 writes now (ci.yml, first step);
     2. the COM wrapper the tools compile against, made from hMailServer.idl
        alone with MIDL and TlbImp (build\generate-com-wrapper.ps1 -FromIdl);
     3. the Control Panel: restore held to packages.lock.json, Release build
        with -warnaserror, publish as a smoke check;
     4. its tests (ControlPanel.Tests);
     5. the tools solution: restore held to the lock files, Release build with
        -warnaserror, the Import Tool's tests, DBSetup's tests, and
        build\build-tools.ps1's publish of every tool the installer ships;
     6. dotnet format --verify-no-changes over the tools solution and the
        Control Panel.

   Not ported: the coverage upload. ci.yml collected Cobertura coverage and
   sent it to GitHub's code-quality service; there is no such service here, and
   the tests run without the collector.

   Every step runs even when an earlier one failed, unless it needs what that
   one made, and the job fails when any did; the summary at the end names them.
   The test results are TRX files in -ResultsDirectory, for the job's artifacts.

   Usage:
     pwsh -NoProfile -File build\ci\windows-dotnet.ps1 [-ResultsDirectory dotnet-results]
#>

param(
   [string] $ResultsDirectory = 'dotnet-results'
)

$ErrorActionPreference = 'Continue'
$started = Get-Date
$root = (Get-Item (Split-Path -Parent $PSScriptRoot)).Parent.FullName
Set-Location -LiteralPath $root
$shell = (Get-Process -Id $PID).Path

if (Test-Path -LiteralPath $ResultsDirectory) { Remove-Item -LiteralPath $ResultsDirectory -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $ResultsDirectory | Out-Null
$ResultsDirectory = (Resolve-Path -LiteralPath $ResultsDirectory).ProviderPath

$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
# No MSBuild or compiler server left running as LocalSystem after the job.
$env:MSBUILDDISABLENODEREUSE = '1'
$env:UseSharedCompilation = 'false'

$controlPanel = 'hmailserver\source\Tools\ControlPanel\ControlPanel.csproj'
$controlPanelTests = 'hmailserver\source\Tools\ControlPanel.Tests\ControlPanel.Tests.csproj'
$tools = 'hmailserver\source\Tools\hMailServer Tools.sln'
$importToolTests = 'hmailserver\source\Tools\ImportTool.Tests\ImportTool.Tests.csproj'
$dbSetupTests = 'hmailserver\source\Tools\DBSetup.Tests\DBSetup.Tests.csproj'

$failed = [System.Collections.Generic.List[string]]::new()

# Runs one step, says how long it took, and records it when it failed. A step
# is a script block whose last native command's exit code is the verdict. What
# the step prints goes to the log, not into the caller's variable: the function
# answers only whether the step passed.
function Invoke-Step([string] $name, [scriptblock] $body) {
   Write-Host ''
   Write-Host "== $name"
   $clock = [Diagnostics.Stopwatch]::StartNew()
   $global:LASTEXITCODE = 0
   & $body *>&1 | Out-Host
   $code = $LASTEXITCODE
   Write-Host "   (${name}: exit $code after $([int]$clock.Elapsed.TotalSeconds) s)"
   if ($code -ne 0) { $failed.Add($name) }
   return ($code -eq 0)
}

"== windows-dotnet of $(git rev-parse HEAD 2>$null) as $([Security.Principal.WindowsIdentity]::GetCurrent().Name), under $shell"
dotnet --list-sdks

$null = Invoke-Step 'the settings search index is current' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\generate-settings-index.ps1
   if ($LASTEXITCODE -ne 0) { return }
   $index = 'hmailserver/source/Tools/ControlPanel/Services/SettingsSearchIndex.g.cs'
   if (git status --porcelain -- $index) {
      git --no-pager diff -- $index | Select-Object -First 80
      "$index is out of date: run build\generate-settings-index.ps1 and commit what it writes."
      $global:LASTEXITCODE = 1
   } else {
      "$index matches the settings page definitions."
      $global:LASTEXITCODE = 0
   }
}

$wrapper = Invoke-Step 'the COM wrapper, from the IDL' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\generate-com-wrapper.ps1 -FromIdl
}

if ($wrapper) {
   $built = (Invoke-Step 'the Control Panel: restore (locked)' { dotnet restore $controlPanel --locked-mode }) -and
            (Invoke-Step 'the Control Panel: build (Release, warnings as errors)' { dotnet build $controlPanel -c Release --no-restore -warnaserror })
   if ($built) {
      $null = Invoke-Step 'the Control Panel: publish (smoke)' { dotnet publish $controlPanel -c Release -o publish --no-build }
   }
   $null = (Invoke-Step 'the Control Panel tests: restore (locked)' { dotnet restore $controlPanelTests --locked-mode }) -and
           (Invoke-Step 'the Control Panel tests' {
              dotnet test $controlPanelTests --configuration Release --no-restore `
                 --logger 'trx;LogFileName=control-panel-tests.trx' --logger 'console;verbosity=minimal' --results-directory $ResultsDirectory
           })

   $toolsBuilt = (Invoke-Step 'the tools: restore (locked)' { dotnet restore $tools --locked-mode }) -and
                 (Invoke-Step 'the tools: build (Release, warnings as errors)' { dotnet build $tools -c Release --no-restore -warnaserror })
   if ($toolsBuilt) {
      $null = Invoke-Step 'the Import Tool tests' {
         dotnet test $importToolTests -c Release --no-build `
            --logger 'trx;LogFileName=import-tool-tests.trx' --logger 'console;verbosity=minimal' --results-directory $ResultsDirectory
      }
      $null = Invoke-Step 'the DBSetup tests' {
         dotnet test $dbSetupTests -c Release --no-build `
            --logger 'trx;LogFileName=dbsetup-tests.trx' --logger 'console;verbosity=minimal' --results-directory $ResultsDirectory
      }
      $null = Invoke-Step 'the tools: publish (build-tools.ps1)' {
         & $shell -NoProfile -ExecutionPolicy Bypass -File build\build-tools.ps1
      }
   }
} else {
   'The tools and the Control Panel compile against the COM wrapper; without it nothing below step 2 can build.'
}

$null = Invoke-Step 'dotnet format: the tools solution' { dotnet format $tools --verify-no-changes }
$null = Invoke-Step 'dotnet format: the Control Panel' { dotnet format $controlPanel --verify-no-changes }

dotnet build-server shutdown *> $null

''
"windows-dotnet finished after $([int]((Get-Date) - $started).TotalMinutes) min"
Get-ChildItem -LiteralPath $ResultsDirectory -Recurse -File | ForEach-Object { "  result: $($_.Name)" }
if ($failed.Count) {
   "FAILED: $($failed -join '; ')"
   exit 1
}
'Every step passed.'
exit 0
