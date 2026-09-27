# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The windows-packages-x64 and windows-packages-arm64 jobs of
   .gitlab/ci/packages-windows.yml, on bench150: the Windows installer of one
   processor built from the commit, unsigned, for every protected pipeline -
   master, a batch* branch and a v* tag - in the runner's own checkout of the
   commit, never the bench's (C:\bench\hmailserver), whose hMailServer service the
   gate runs against. Nothing here stops, starts or registers that service, and no
   file it runs from is written.

   The steps, for -Platform x64 or ARM64:
     1. the checkout is the commit (CI_COMMIT_SHA), and no file git does not track
        is where a build would take it in: an untracked, unignored file under
        hmailserver\source, or an MSBuild or NuGet file (Directory.Build.props and
        its kin, NuGet.Config) in the checkout or a folder above it, as
        build\ci\windows-installer.ps1 refuses them;
     2. the version is section_setup_64.iss's AppVersion, which both installer
        scripts take; for a v* tag it must be the tag's;
     3. Inno Setup's compiler is found (HM_ISCC, or Inno Setup 6 installed for the
        machine), or the job stops saying what to install;
     4. ARM64 only: the toolchain, checked by check-arm64-toolchain.ps1 with
        vswhere before anything is built - MSVC's ARM64 tools, ATL for ARM64 and
        the Windows SDK's ARM64 libraries in the instance that has MSBuild; a
        missing one stops the job naming the vs_installer command of
        hmailserver/docs/WindowsOnArm.md;
     5. the installer's inputs lose what git does not track (the .NET runtimes,
        held to their pins, apart), and every tool's publish folders go;
     6. ARM64 only: the ARM64 OpenSSL, Boost and libpq, in -LibsDir, a cache on the
        bench beside and apart from the x64 libraries: each is built with
        libraries\build-*.ps1 -Platform ARM64 when its tree is not complete there,
        and its DLLs are checked to be ARM64. OpenSSL is not built while
        libraries\build-openssl.ps1 fetches its source from github.com (a string
        in its code names github.com), which this project's CI does not
        download from: the job stops, saying so. The GitLab sweep's version
        takes the same archive from Debian's, and with it the job builds OpenSSL;
     7. the third-party files (build\get-installer-binaries.ps1, which also places
        the ADO type libraries the server #imports) and then the server, Release
        (build\build.ps1), with hMailServerLibs pointing at -LibsDir;
     8. the tools (build\build-tools.ps1) and, for x64, the Control Panel, restored
        in locked mode, built with warnings as errors and published, as
        RELEASE.md step 8 and windows-installer.ps1 do it (for ARM64 build-tools.ps1
        publishes the Control Panel itself);
     9. the .NET Desktop Runtime (build\get-dotnet-runtime.ps1) and both pinned
        placements checked again with -Verify;
    10. no tracked file changed while that ran;
    11. every program and library the installer script packs is built for the
        processor (check-pe-machine.ps1, which reads the script as ISCC does);
    12. ISCC on hMailServer64.iss or hMailServerARM64.iss;
    13. the installer is the one the script names, hMailServer-<version>-x64.exe or
        -arm64.exe, and its version resource is section_setup_64.iss's
        VersionInfoVersion.
   The installer goes to -OutDir as hMailServer-<version>-<x64|arm64>.exe for a v*
   tag - the name a server looks for - and as
   hMailServer-<version>-<first 8 of the commit>-<x64|arm64>.exe for a branch, so
   that no branch build is mistaken for the release; beside it SHA256SUMS and
   build-info.txt, and for ARM64 any NuGet lock file packages.arm64.lock.json the
   first ARM64 publish created, to be committed (WindowsOnArm.md).

   It is unsigned. The release chain's release-authenticode job signs a release's
   installers (docs/Packages-windows.md says where the ARM64 one goes).

   It does not take the gate lock: the job does, around it.

   Exit codes:
     0  the installer is built              2  Inno Setup or the ARM64 toolchain is missing
     3  a build step failed                 4  the installer is not what the tag or the script names
     5  the checkout is not the commit, holds a file a build would take in, or a
        build changed a tracked file
     6  a file the installer packs is built for another processor
     7  the ARM64 OpenSSL cannot be built here: build-openssl.ps1 downloads from GitHub

   Usage:
     pwsh -NoProfile -File build\ci\packages\windows-packages.ps1 -Platform x64 -OutDir packages-windows\x64
     pwsh -NoProfile -File build\ci\packages\windows-packages.ps1 -Platform ARM64 -OutDir packages-windows\arm64 -LibsDir C:\bench\hMailLibs-arm64
#>

param(
   [Parameter(Mandatory)][ValidateSet('x64', 'ARM64')][string] $Platform,
   [Parameter(Mandatory)][string] $OutDir,
   # x64: the bench's prebuilt libraries, as windows-gate and windows-installer use.
   # ARM64: the cache the ARM64 libraries are built into once and kept.
   [string] $LibsDir = '',
   [string] $Commit = $env:CI_COMMIT_SHA,
   [string] $Tag = $env:CI_COMMIT_TAG
)

$ErrorActionPreference = 'Continue'
$shell = (Get-Process -Id $PID).Path
$started = Get-Date
$arch = if ($Platform -eq 'ARM64') { 'arm64' } else { 'x64' }
if (-not $LibsDir) { $LibsDir = if ($Platform -eq 'ARM64') { 'C:\bench\hMailLibs-arm64' } else { 'C:\Dev\hMailLibs' } }

function Stop-Packages([int] $code, [string] $sentence) {
   "PACKAGES NOT BUILT ($Platform): $sentence"
   "PACKAGES EXIT=$code"
   exit $code
}

function Invoke-Step([string] $what, [scriptblock] $step) {
   "--- $what"
   $global:LASTEXITCODE = 0
   & $step
   if ($LASTEXITCODE -ne 0) { Stop-Packages 3 "$what failed (exit $LASTEXITCODE)" }
}

$root = (Get-Item (Split-Path -Parent $PSScriptRoot)).Parent.Parent.FullName
Set-Location -LiteralPath $root
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath
"== the Windows installer for $Platform in $root as $([Security.Principal.WindowsIdentity]::GetCurrent().Name)"

# ---------------------------------------------------------------- 1. the checkout
"--- checkout"
$head = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { Stop-Packages 5 "$root is not a git checkout" }
if ($Commit -and $head -notlike "$Commit*") { Stop-Packages 5 "HEAD is $head, not $Commit" }
$changed = @(git status --porcelain --untracked-files=no | Where-Object { $_ })
if ($changed.Count -gt 0) {
   $changed | Select-Object -First 20 | ForEach-Object { "  $_" }
   Stop-Packages 5 "$($changed.Count) tracked file(s) differ from $head before anything was built"
}
$stray = @(git status --porcelain --untracked-files=all -- hmailserver/source | Where-Object { $_.StartsWith('?? ') } | ForEach-Object { $_.Substring(3) })
$buildReads = '^(Directory\.Build\.(props|targets|rsp)|Directory\.Packages\.props|Directory\.Solution\.(props|targets)|nuget\.config)$'
$stray += @(git ls-files --others | Where-Object { ($_ -split '/')[-1] -match $buildReads })
$above = Split-Path -Parent $root
while ($above) {
   $stray += @(Get-ChildItem -LiteralPath $above -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $buildReads } | ForEach-Object { $_.FullName })
   $above = Split-Path -Parent $above
}
if ($stray.Count -gt 0) {
   $stray | Select-Object -First 20 | ForEach-Object { "  $_" }
   Stop-Packages 5 "$($stray.Count) file(s) git does not track would be built in, or read by every build, from $root or a folder above it"
}
"the checkout is $head, with nothing untracked where a build would take it in"

# ---------------------------------------------------------------- 2. the version
function Read-SetupValue([string] $key) {
   $line = Get-Content -LiteralPath 'hmailserver\installation\section_setup_64.iss' | Where-Object { $_ -match "^\s*$key\s*=" } | Select-Object -First 1
   if (-not $line) { return '' }
   return ($line -split '=', 2)[1].Trim()
}
$version = Read-SetupValue 'AppVersion'
$versionInfo = Read-SetupValue 'VersionInfoVersion'
if (-not $version -or -not $versionInfo) { Stop-Packages 4 'section_setup_64.iss names no AppVersion or VersionInfoVersion' }
if ($Tag) {
   if ($Tag -notmatch '^v[0-9][A-Za-z0-9._-]*$') { Stop-Packages 4 "$Tag is not a release tag" }
   if ($Tag.Substring(1) -ne $version) {
      Stop-Packages 4 "the tag is $Tag and section_setup_64.iss says ${version}: the version stamp (RELEASE.md step 7) was not made for this tag"
   }
   $artifact = "hMailServer-$version-$arch.exe"
} else {
   $artifact = "hMailServer-$version-$($head.Substring(0, 8))-$arch.exe"
}
$scriptName = if ($Platform -eq 'ARM64') { 'hMailServerARM64.iss' } else { 'hMailServer64.iss' }
$iss = "hmailserver\installation\$scriptName"
$expectedName = "hMailServer-$version-$arch.exe"
"version $version (resource $versionInfo); $scriptName makes $expectedName; the artifact is $artifact"

# ---------------------------------------------------------------- 3. Inno Setup
"--- Inno Setup"
$candidates = @()
if ($env:HM_ISCC) { $candidates += $env:HM_ISCC }
$candidates += (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
$iscc = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $iscc) {
   Stop-Packages 2 (("Inno Setup 6 is not installed for the machine on this runner (looked for {0}). Install it once, as an administrator on bench150: " +
      "winget install --exact --id JRSoftware.InnoSetup --scope machine - or set the CI/CD variable HM_ISCC to ISCC.exe's path.") -f ($candidates -join ', '))
}
# ISCC.exe carries no version resource (0.0.0.0) and prints none, so it is named by
# its file's date and hash in build-info.txt.
$isccItem = Get-Item -LiteralPath $iscc
$isccId = "{0}, {1:yyyy-MM-dd}, sha256 {2}" -f $iscc, $isccItem.LastWriteTimeUtc, (Get-FileHash -LiteralPath $iscc -Algorithm SHA256).Hash.Substring(0, 16).ToLowerInvariant()
"ISCC: $isccId"

# ---------------------------------------------------------------- 4. the ARM64 toolchain
if ($Platform -eq 'ARM64') {
   "--- the ARM64 toolchain"
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\ci\packages\check-arm64-toolchain.ps1
   if ($LASTEXITCODE -ne 0) { Stop-Packages 2 'the ARM64 toolchain is not on this machine (above: what is missing and the command that adds it)' }
}

# ---------------------------------------------------------------- 5. the installer's inputs, emptied
"--- what the installer reads, emptied of what git does not track"
# As windows-installer.ps1 does, for the same reason: what the installer takes from
# these folders is the commit's tracked files, what this run builds and publishes,
# and the files the two get- scripts place and hold to their pins. The .NET runtimes
# (DotNet\ and ARM64\DotNet\) are kept - get-dotnet-runtime.ps1 holds each to its
# SHA-512 pin and replaces one that is not - so they are not downloaded every time.
git clean -fdxq -e DotNet/ -- hmailserver/installation hmailserver/source/Addons hmailserver/source/Translations hmailserver/source/DBScripts
if ($LASTEXITCODE -ne 0) { Stop-Packages 3 "git clean of the installer's inputs failed (exit $LASTEXITCODE)" }
foreach ($tool in Get-ChildItem -LiteralPath 'hmailserver\source\Tools' -Directory) {
   foreach ($folder in 'publish', 'publish-arm64') {
      $publish = Join-Path $tool.FullName $folder
      if (Test-Path -LiteralPath $publish) { Remove-Item -LiteralPath $publish -Recurse -Force }
   }
}

$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:MSBUILDDISABLENODEREUSE = '1'
$env:UseSharedCompilation = 'false'

# ---------------------------------------------------------------- 6. the ARM64 libraries
$libraryVersions = [ordered]@{}
foreach ($line in (& $shell -NoProfile -ExecutionPolicy Bypass -File build\Get-LibraryVersions.ps1)) {
   if ("$line" -match '^(\w+)=(.+)$') { $libraryVersions[$Matches[1]] = $Matches[2] }
}
if (-not $libraryVersions['openssl'] -or -not $libraryVersions['boost'] -or -not $libraryVersions['postgresql']) {
   Stop-Packages 3 'build\Get-LibraryVersions.ps1 did not name the OpenSSL, Boost and PostgreSQL versions'
}
"libraries: OpenSSL $($libraryVersions['openssl']), Boost $($libraryVersions['boost']), PostgreSQL $($libraryVersions['postgresql']) in $LibsDir"

if ($Platform -eq 'ARM64') {
   "--- the ARM64 libraries in $LibsDir"
   New-Item -ItemType Directory -Force -Path $LibsDir | Out-Null
   $openssl = $libraryVersions['openssl']
   $opensslMajor = $openssl.Split('.')[0]
   $opensslOut = Join-Path $LibsDir "openssl-$openssl-arm64\out64"
   $boostDir = Join-Path $LibsDir "$($libraryVersions['boost_dir'])-arm64"
   $libpqDir = Join-Path $LibsDir "$($libraryVersions['postgresql_dir'])-arm64\builddir\src\interfaces\libpq"
   $opensslDlls = @("$opensslOut\bin\libcrypto-$opensslMajor-arm64.dll", "$opensslOut\bin\libssl-$opensslMajor-arm64.dll")
   $libraries = @(
      [pscustomobject]@{ Name = 'OpenSSL'; Script = 'libraries\build-openssl.ps1'; Arguments = @('-Version', $openssl)
         Complete = @($opensslDlls + "$opensslOut\lib\libssl.lib", "$opensslOut\lib\libcrypto.lib", "$opensslOut\include\openssl\ssl.h"); Dlls = $opensslDlls }
      [pscustomobject]@{ Name = 'Boost'; Script = 'libraries\build-boost.ps1'; Arguments = @('-Version', $libraryVersions['boost'])
         Complete = @("$boostDir\boost\version.hpp"); Dlls = @() }
      [pscustomobject]@{ Name = 'libpq'; Script = 'libraries\build-pgsql.ps1'; Arguments = @('-Version', $libraryVersions['postgresql'], '-OpenSSLVersion', $openssl)
         Complete = @("$libpqDir\libpq.dll", "$libpqDir\libpq.lib"); Dlls = @("$libpqDir\libpq.dll") }
   )
   $env:hMailServerLibs = $LibsDir
   foreach ($library in $libraries) {
      $absent = @($library.Complete | Where-Object { -not (Test-Path -LiteralPath $_) })
      if ($library.Name -eq 'Boost') {
         # Every library the server auto-links, release and debug, tagged -a64.
         $tag = "vc145"
         foreach ($lib in 'thread', 'filesystem', 'regex', 'chrono', 'atomic') {
            foreach ($variant in 'mt', 'mt-gd') {
               if (-not (Get-ChildItem -Path (Join-Path $boostDir 'stage\lib') -Filter "libboost_$lib-$tag-$variant-a64-*.lib" -ErrorAction SilentlyContinue)) { $absent += "$boostDir\stage\lib\libboost_$lib-$tag-$variant-a64-*.lib" }
            }
         }
      }
      if ($absent.Count -eq 0) { "  $($library.Name): in the cache"; continue }
      "  $($library.Name): not complete in the cache ($($absent[0])$(if ($absent.Count -gt 1) { " and $($absent.Count - 1) more" })) - building it"
      if ($library.Name -eq 'OpenSSL') {
         # This project's CI downloads nothing from GitHub. At 6399e08a5
         # build-openssl.ps1 takes OpenSSL's release archive from
         # github.com/openssl/openssl/releases, the only place OpenSSL publishes it;
         # the GitLab sweep takes the same bytes from Debian's archive instead
         # (openssl_<version>.orig.tar.gz, held to the same pinned SHA-256). Every
         # string in the script's code - not its comments - is read with PowerShell's
         # own parser: one that names github.com or githubusercontent.com means a
         # download from GitHub, and the job stops; none means it builds OpenSSL.
         $tokens = $null; $parseErrors = $null
         [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $library.Script), [ref]$tokens, [ref]$parseErrors)
         if ($parseErrors.Count) { Stop-Packages 3 "$($library.Script) does not parse: $($parseErrors[0].Message)" }
         $gitHub = @($tokens | Where-Object { $_ -is [System.Management.Automation.Language.StringToken] -and $_.Text -match 'github\.com|githubusercontent\.com' } | ForEach-Object { "line $($_.Extent.StartLineNumber): $($_.Text)" })
         if ($gitHub.Count) {
            Stop-Packages 7 (("the ARM64 OpenSSL {0} is not in {1}, and {2} downloads its source from GitHub ({3}), which this project's CI does not do. " +
               "Build it once on bench150 into that folder, or bring in the GitLab sweep's build-openssl.ps1, which takes the same archive from Debian's (docs/Packages-windows.md, 'The ARM64 libraries'), " +
               "and retry the job") -f $openssl, $LibsDir, $library.Script, ($gitHub -join '; '))
         }
      }
      Invoke-Step "the ARM64 $($library.Name) ($($library.Script) -Platform ARM64)" {
         & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $library.Script @($library.Arguments) -Platform ARM64 *>&1 | Select-Object -Last 15 | ForEach-Object { "    $_" }
      }
      $still = @($library.Complete | Where-Object { -not (Test-Path -LiteralPath $_) })
      if ($still.Count) { Stop-Packages 3 "$($library.Script) ended without $($still -join ', ')" }
   }
   $dlls = @($libraries | ForEach-Object { $_.Dlls })
   "  the cached DLLs are ARM64:"
   & (Join-Path $root 'build\ci\packages\check-pe-machine.ps1') -Path $dlls -Expect ARM64
   if ($LASTEXITCODE -ne 0) { Stop-Packages 6 "a DLL in the ARM64 library cache $LibsDir is not ARM64: delete that library's tree there and retry, so that it is built again" }
} else {
   foreach ($dir in 'openssl_dir', 'boost_dir', 'postgresql_dir') {
      if (-not (Test-Path -LiteralPath (Join-Path $LibsDir $libraryVersions[$dir]))) { Stop-Packages 3 "the x64 library tree $LibsDir\$($libraryVersions[$dir]) is not on this runner" }
   }
}
$env:hMailServerLibs = $LibsDir

# ---------------------------------------------------------------- 7. the third-party files, then the server
$platformArguments = if ($Platform -eq 'ARM64') { @('-Platform', 'ARM64') } else { @() }
Invoke-Step "the third-party files and the ADO type libraries (get-installer-binaries.ps1 $platformArguments)" {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-installer-binaries.ps1 @platformArguments *>&1 | Select-Object -Last 6 | ForEach-Object { "  $_" }
}
Invoke-Step "the server (build.ps1 -Configuration Release $platformArguments)" {
   & powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build.ps1 -Configuration Release @platformArguments *>&1 |
      Select-String -Pattern 'error C|error LNK|error MIDL|error MSB|warning MSB3270|Build succeeded|Build FAILED|Using MSBuild|is not there' | Select-Object -Last 25 | ForEach-Object { "  $_" }
}
$serverOut = "hmailserver\source\Server\hMailServer\$Platform\Release"
foreach ($file in 'hMailServer.exe', 'hMailServer.Minidump.exe', 'hMailServer.Updater.exe', 'hMailServer.tlb') {
   if (-not (Test-Path -LiteralPath (Join-Path $serverOut $file))) { Stop-Packages 3 "build.ps1 succeeded without $serverOut\$file" }
}
"hMailServer.exe $((Get-FileHash (Join-Path $serverOut 'hMailServer.exe') -Algorithm SHA256).Hash)"

# ---------------------------------------------------------------- 8. the tools and the Control Panel
Invoke-Step "the tools (build-tools.ps1 -Configuration Release $platformArguments)" {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\build-tools.ps1 -Configuration Release @platformArguments *>&1 | Select-Object -Last 8 | ForEach-Object { "  $_" }
}
if ($Platform -eq 'x64') {
   $controlPanel = 'hmailserver\source\Tools\ControlPanel\ControlPanel.csproj'
   $controlPanelPublish = 'hmailserver\source\Tools\ControlPanel\publish'
   Invoke-Step 'the Control Panel: restore (locked)' { dotnet restore $controlPanel --locked-mode }
   Invoke-Step 'the Control Panel: build (Release, warnings as errors)' { dotnet build $controlPanel -c Release --no-restore -warnaserror }
   Invoke-Step 'the Control Panel: publish' { dotnet publish $controlPanel -c Release -o $controlPanelPublish --no-build }
   "hMailCP.dll $((Get-Item (Join-Path $controlPanelPublish 'hMailCP.dll')).VersionInfo.FileVersion)"
}

# ---------------------------------------------------------------- 9. the .NET runtime, and both placements checked
Invoke-Step "the .NET Desktop Runtime (get-dotnet-runtime.ps1 $platformArguments)" {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-dotnet-runtime.ps1 @platformArguments *>&1 | Select-Object -Last 3 | ForEach-Object { "  $_" }
}
Invoke-Step 'the .NET Desktop Runtime, checked (-Verify)' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-dotnet-runtime.ps1 @platformArguments -Verify *>&1 | Select-Object -Last 2 | ForEach-Object { "  $_" }
}
Invoke-Step 'the third-party files, checked (get-installer-binaries.ps1 -Verify)' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-installer-binaries.ps1 @platformArguments -Verify *>&1 | Select-Object -Last 3 | ForEach-Object { "  $_" }
}

# ---------------------------------------------------------------- 10. nothing tracked changed
"--- the checkout, after the builds"
$rewritten = @(git status --porcelain --untracked-files=no | Where-Object { $_ })
if ($rewritten.Count -gt 0) {
   $rewritten | Select-Object -First 20 | ForEach-Object { "  $_" }
   Stop-Packages 5 "the builds changed $($rewritten.Count) tracked file(s), so what they built is not $($head.Substring(0, 12))'s (a restore rewriting a packages.lock.json that disagrees with what it resolved is the usual one)"
}
"no tracked file changed"
# The ARM64 publish's own NuGet lock files are created by its first run
# (WindowsOnArm.md): kept with the artifact, to be committed.
$newLocks = @(git ls-files --others --exclude-standard -- '*packages.arm64.lock.json')
if ($newLocks.Count) {
   "$($newLocks.Count) ARM64 lock file(s) the repository does not have yet, kept in the artifact under lockfiles\ to be committed:"
   foreach ($lock in $newLocks) {
      "  $lock"
      $target = Join-Path (Join-Path $OutDir 'lockfiles') $lock
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
      Copy-Item -LiteralPath $lock -Destination $target -Force
   }
}

# ---------------------------------------------------------------- 11. every PE image for the processor
"--- every program and library $scriptName packs is $Platform"
# vcruntime140_1.dll is allowed on ARM64 and named in the output: the arm64 folder of
# Microsoft's own C++ redistributable carries it as ARM64EC code (checked on
# 26 September 2026 in MSVC 14.51.36231's redist), for x64 and ARM64EC processes;
# an ARM64 process never imports it, and third-party-binaries.json gathers it.
$allow = if ($Platform -eq 'ARM64') { @('vcruntime140_1.dll') } else { @() }
& (Join-Path $root 'build\ci\packages\check-pe-machine.ps1') -Iss $iss -Expect $Platform -Allow $allow
$pe = $LASTEXITCODE
if ($pe -eq 1) { Stop-Packages 6 "a file $scriptName packs is not built for $Platform (the WRONG lines above)" }
if ($pe -ne 0) { Stop-Packages 3 "check-pe-machine.ps1 could not check what $scriptName packs (exit $pe)" }

# ---------------------------------------------------------------- 12. ISCC
$output = 'hmailserver\installation\Output'
if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
Invoke-Step "the installer (ISCC $scriptName)" {
   & $iscc /Q $iss 2>&1 | Select-Object -Last 20 | ForEach-Object { "  $_" }
}

# ---------------------------------------------------------------- 13. what was built
"--- what was built"
$built = @(Get-ChildItem -LiteralPath $output -Filter '*.exe' -File -ErrorAction SilentlyContinue)
if ($built.Count -ne 1 -or $built[0].Name -ne $expectedName) {
   Stop-Packages 4 "ISCC made $(($built | ForEach-Object Name) -join ', '), not $expectedName (for x64 section_setup_64.iss's OutputBaseFilename is stamped apart from AppVersion, RELEASE.md step 7)"
}
$installer = $built[0].FullName
$stated = (Get-Item -LiteralPath $installer).VersionInfo.FileVersion
if ($stated -ne $versionInfo) { Stop-Packages 4 "the installer's version resource says $stated, not section_setup_64.iss's $versionInfo" }
if ($Tag) {
   $numbers = ($version -replace '[^0-9.].*$', '')
   if ($stated -ne "$numbers.0") { Stop-Packages 4 "the installer's version resource says $stated, not $numbers.0, which a server holds $Tag's installer to (UpdateFeed.md)" }
}
$target = Join-Path $OutDir $artifact
Copy-Item -LiteralPath $installer -Destination $target -Force
$hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $artifact" | Set-Content -LiteralPath (Join-Path $OutDir 'SHA256SUMS') -Encoding ascii

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path -LiteralPath $vswhere) { (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -format value -property installationVersion) -join ' ' } else { 'unknown' }
@(
   "installer      $artifact"
   "sha256         $hash"
   "size           $((Get-Item -LiteralPath $target).Length) bytes"
   "platform       $Platform"
   "version        $version (version resource $stated)"
   "commit         $head"
   "ref            $(if ($Tag) { "tag $Tag" } else { "branch $($env:CI_COMMIT_BRANCH)" })"
   "pipeline       $($env:CI_PIPELINE_URL)"
   "job            $($env:CI_JOB_URL)"
   "runner         $env:COMPUTERNAME ($($env:CI_RUNNER_DESCRIPTION))"
   "visual studio  $vs"
   "iscc           $isccId"
   "dotnet sdk     $((dotnet --version) -join ' ')"
   "libraries      OpenSSL $($libraryVersions['openssl']), Boost $($libraryVersions['boost']), PostgreSQL $($libraryVersions['postgresql']) from $LibsDir"
   "authenticode   $((Get-AuthenticodeSignature -LiteralPath $target).Status) - unsigned by design; release-authenticode signs a release's installers"
   "built          $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')) in $([int]((Get-Date) - $started).TotalMinutes) min"
) | Set-Content -LiteralPath (Join-Path $OutDir 'build-info.txt') -Encoding utf8
Get-Content -LiteralPath (Join-Path $OutDir 'build-info.txt') | ForEach-Object { "  $_" }
"PACKAGES EXIT=0"
exit 0
