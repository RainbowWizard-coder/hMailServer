# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The windows-installer job of .gitlab-ci.yml, on bench150, for a v* tag: the
   Windows installer of the tag's commit, built as RELEASE.md steps 8 and 11 build
   it, in the bench's own checkout - the one windows-gate has just built and tested
   the same commit in, which holds what a clean clone lacks (the libraries under
   libraries\ the server links, and the bench's hMailServerLibs tree).

   The steps:
     1. the checkout is the commit, with no change to a tracked file: a change git
        checkout kept - a bench fix made by hand, say - stops the job, because the
        installer is signed as the tag's. So does a file git neither tracks nor
        ignores anywhere under hmailserver\source, where a build would take it in
        (an SDK-style project compiles every .cs and .xaml below its folder and
        embeds every .resx, tracked or not), and an MSBuild or NuGet file every
        build would read (Directory.Build.props and its kin, NuGet.Config) that git
        does not track, in the checkout or in a folder above it;
     2. Inno Setup's compiler is found before anything is built, and its absence
        stops the job with what to install;
     3. every folder the installer script takes whole from the checkout, and every
        tool's publish folder, loses what git does not track (the .NET runtime,
        held to its pin, apart), so nothing from an earlier build or put there by
        hand is shipped from them;
     4. the server, Release (build\build.ps1), which after windows-gate is the same
        commit's build again;
     5. the tools (build\build-tools.ps1) and the Control Panel, published into
        their publish folders, emptied first (3) so that no file of an earlier
        build is shipped;
     6. the .NET Desktop Runtime the installer carries (build\get-dotnet-runtime.ps1)
        and the third-party files (build\get-installer-binaries.ps1, from this
        project's build-inputs-1 release on GitLab and from 7-zip.org), each checked
        against its pinned hash, then checked again with -Verify;
     7. no tracked file changed while all that ran: build-tools.ps1 publishes
        without --locked-mode, and a restore rewrites a packages.lock.json that
        disagrees with what it resolved, which would be tools built from a package
        graph the commit does not have;
     8. ISCC on hmailserver\installation\hMailServer64.iss;
     9. the installer is named as a Windows server looks for it,
        hMailServer-<the tag without its v>-x64.exe, and says in its version
        resource the version the tag names (UpdateFeed.md: a server refuses an
        installer whose version resource is another release's).
   It is copied into -OutDir for the job's artifacts, unsigned: release-authenticode
   signs it, and only once a maintainer has started the release.

   It does not fetch and does not take the gate lock: the job does both around it,
   as windows-gate does.

   What it does not check: OpenSSL's and libpq's DLLs come from the bench's library
   tree (-LibsDir), built by libraries\build-*.ps1 outside this job, and the MSVC
   runtime from the bench's Visual Studio (get-installer-binaries.ps1 holds it to a
   version, not a hash); the server's x64\Release output, and the build output the
   tools and the Control Panel are published from, are the checkout's incremental
   build of this commit, not a clean one; and what git ignores under
   hmailserver\source is build output that the builds write again when their inputs
   change.

   Exit codes:
     0  the installer is built           3  a build step failed
     2  Inno Setup is not installed      4  the installer is not what the tag names
     5  the checkout is not the commit, has a change of its own to a tracked file or
        an untracked file a build would take in (step 1), or a build changed a
        tracked file (step 7)

   Usage:
     pwsh -NoProfile -File build\ci\windows-installer.ps1 -Commit <sha> -Tag v6.3.5 -OutDir <dir>
#>

param(
   [Parameter(Mandatory)][string] $Commit,
   [Parameter(Mandatory)][string] $Tag,
   [Parameter(Mandatory)][string] $OutDir,
   [string] $BenchDir = 'C:\bench\hmailserver',
   [string] $LibsDir = 'C:\Dev\hMailLibs'
)

$ErrorActionPreference = 'Continue'
$shell = (Get-Process -Id $PID).Path

function Stop-Installer([int] $code, [string] $sentence) {
   "INSTALLER NOT BUILT: $sentence"
   "INSTALLER EXIT=$code"
   exit $code
}

if ($Tag -notmatch '^v[0-9][A-Za-z0-9._-]*$') { Stop-Installer 4 "$Tag is not a release tag" }
$version = $Tag.Substring(1)
$installerName = "hMailServer-$version-x64.exe"

if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath

Set-Location -LiteralPath $BenchDir
"== the installer of $Tag ($Commit) in $BenchDir as $([Security.Principal.WindowsIdentity]::GetCurrent().Name)"

"--- checkout"
git cat-file -e "$Commit^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) { Stop-Installer 5 "$Commit is not in $BenchDir - fetch it first" }
git checkout -q --detach $Commit 2>&1 | Select-Object -Last 1 | ForEach-Object { "$_" }
if ((git rev-parse HEAD) -notlike "$Commit*") { Stop-Installer 5 "HEAD is $(git rev-parse --short HEAD), not $Commit" }
# git checkout keeps an uncommitted change to a tracked file that the commit does not
# touch, and this checkout is the maintainer's split gate's too, where bench fixes have
# been made by hand before. The installer is signed as the tag's, so it is built from
# the tag's tracked files and nothing else.
$changed = @(git status --porcelain --untracked-files=no | Where-Object { $_ })
if ($LASTEXITCODE -ne 0) { Stop-Installer 5 "git status failed in $BenchDir (exit $LASTEXITCODE)" }
if ($changed.Count -gt 0) {
   $changed | Select-Object -First 20 | ForEach-Object { "  $_" }
   Stop-Installer 5 (("{0} tracked file(s) in {1} differ from {2}. Save any bench fix elsewhere, restore them " +
      "(git -C {1} checkout -- . as the runner's account), and retry the job") -f $changed.Count, $BenchDir, $Commit)
}
"the checkout is $Commit, with no change to a tracked file"

# Nor may a file git does not track sit where a build takes it in. Every project under
# hmailserver\source\Tools is SDK-style: it compiles every .cs and .xaml below its
# folder and embeds every .resx, whether git knows the file or not, so a stray source
# file there would be built into the Control Panel or a tool the installer carries.
# What git ignores there is build output, which the builds below write again; what it
# neither tracks nor ignores is refused, anywhere under hmailserver\source. And
# MSBuild imports a Directory.Build.props, .targets or .rsp, a Directory.Packages.props
# or a Directory.Solution file from any folder between a project and the drive's root
# - no tracked one stands between the server's projects and C:\ - and NuGet reads
# every NuGet.Config on that path: one git does not track, in the checkout or above
# it, would change every build here.
$stray = @(git status --porcelain --untracked-files=all -- hmailserver/source | Where-Object { $_.StartsWith('?? ') } | ForEach-Object { $_.Substring(3) })
if ($LASTEXITCODE -ne 0) { Stop-Installer 5 "git status failed in $BenchDir (exit $LASTEXITCODE)" }
$buildReads = '^(Directory\.Build\.(props|targets|rsp)|Directory\.Packages\.props|Directory\.Solution\.(props|targets)|nuget\.config)$'
$stray += @(git ls-files --others | Where-Object { ($_ -split '/')[-1] -match $buildReads })
if ($LASTEXITCODE -ne 0) { Stop-Installer 5 "git ls-files failed in $BenchDir (exit $LASTEXITCODE)" }
$above = Split-Path -Parent (Get-Location).ProviderPath
while ($above) {
   $stray += @(Get-ChildItem -LiteralPath $above -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $buildReads } | ForEach-Object { $_.FullName })
   $above = Split-Path -Parent $above
}
if ($stray.Count -gt 0) {
   $stray | Select-Object -First 20 | ForEach-Object { "  $_" }
   Stop-Installer 5 (("{0} file(s) git does not track would be built into the installer from {1} or read by every build there. " +
      "Move them out of the way (git -C {1} clean -n -- hmailserver/source lists the first kind), and retry the job") -f $stray.Count, $BenchDir)
}
"no file git does not track is where a build would take it in"

"--- Inno Setup"
# Installed for the machine: the runner is LocalSystem, which sees neither a per-user
# installation (%LOCALAPPDATA%\Programs\Inno Setup 6) nor the maintainer's PATH.
$candidates = @()
if ($env:HM_ISCC) { $candidates += $env:HM_ISCC }
$candidates += (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
$iscc = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $iscc) {
   Stop-Installer 2 (("Inno Setup 6 is not installed for the machine on this runner (looked for {0}). Install it once, as an administrator on bench150: " +
      "winget install --exact --id JRSoftware.InnoSetup --scope machine - or set the CI/CD variable HM_ISCC to ISCC.exe's path " +
      "(docs\ContinuousIntegration.md, 'The release').") -f ($candidates -join ', '))
}
"ISCC: $iscc"

"--- what the installer reads, emptied of what git does not track"
# Every folder hMailServer64.iss reads from inside the checkout loses what git does not
# track before anything is built: the folders it takes whole (Addons, Translations,
# DBScripts, WebAdmin\languages, Microsoft.VC145.CRT, Extras\plugin), the folders the
# two get- scripts place their files in, ISCC's Output, and every tool's publish
# folder. What the installer then takes from those folders is the commit's tracked
# files, what this run publishes, and the files get-installer-binaries.ps1 and
# get-dotnet-runtime.ps1 place and hold to their pins - nothing from an earlier build
# or put there by hand. The .NET runtime is kept: the installer names its one file
# exactly and get-dotnet-runtime.ps1 holds it to its SHA-512 pin, so it is not
# downloaded again. The server's own files in x64\Release and the COM wrapper in
# Tools\Interop are build.ps1's incremental build of this commit, and OpenSSL and
# libpq come from $LibsDir, outside the checkout.
git clean -fdxq -e DotNet/ -- hmailserver/installation hmailserver/source/Addons hmailserver/source/Translations hmailserver/source/DBScripts
if ($LASTEXITCODE -ne 0) { Stop-Installer 3 "git clean of the installer's inputs failed (exit $LASTEXITCODE)" }
foreach ($tool in Get-ChildItem -LiteralPath 'hmailserver\source\Tools' -Directory) {
   foreach ($folder in 'publish', 'publish-arm64') {
      $publish = Join-Path $tool.FullName $folder
      if (Test-Path -LiteralPath $publish) { Remove-Item -LiteralPath $publish -Recurse -Force }
   }
}
$left = @(git ls-files --others -- hmailserver/installation hmailserver/source/Addons hmailserver/source/Translations hmailserver/source/DBScripts |
   Where-Object { $_ -notlike 'hmailserver/installation/DotNet/*' -and $_ -notlike 'hmailserver/installation/ARM64/DotNet/*' })
if ($left.Count -gt 0) { Stop-Installer 3 "files git does not track are still among the installer's inputs: $(($left | Select-Object -First 10) -join ', ')" }

$env:hMailServerLibs = $LibsDir
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$env:MSBUILDDISABLENODEREUSE = '1'

function Invoke-Step([string] $what, [scriptblock] $step) {
   "--- $what"
   & $step
   if ($LASTEXITCODE -ne 0) { Stop-Installer 3 "$what failed (exit $LASTEXITCODE)" }
}

Invoke-Step 'the server (build.ps1 -Configuration Release)' {
   & powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build.ps1 -Configuration Release *>&1 |
      Select-String -Pattern "error C|error LNK|error MIDL|Build succeeded|Build FAILED" | Select-Object -Last 20 | ForEach-Object { "$_" }
}
"hMailServer.exe $((Get-FileHash 'hmailserver\source\Server\hMailServer\x64\Release\hMailServer.exe' -Algorithm SHA256).Hash)"
# build.ps1 turns the pre-build event off, so the build does not stop the service,
# which runs from x64\Release. After windows-gate of this commit nothing is linked
# again and that does not matter; when another commit has been built in this checkout
# since - the maintainer's split gate, or another pipeline's gate - the link fails on
# the running hMailServer.exe and the job stops above: retry windows-gate, then this
# job. The service is started in case something left it stopped: the bench runs the
# next gate against it.
Start-Service hMailServer -ErrorAction SilentlyContinue

Invoke-Step 'the tools (build-tools.ps1)' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\build-tools.ps1 -Configuration Release *>&1 | Select-Object -Last 5 | ForEach-Object { "$_" }
}

$controlPanel = 'hmailserver\source\Tools\ControlPanel\ControlPanel.csproj'
$controlPanelPublish = 'hmailserver\source\Tools\ControlPanel\publish'
if (Test-Path -LiteralPath $controlPanelPublish) { Remove-Item -LiteralPath $controlPanelPublish -Recurse -Force }
Invoke-Step 'the Control Panel: restore (locked)' { dotnet restore $controlPanel --locked-mode }
Invoke-Step 'the Control Panel: build (Release, warnings as errors)' { dotnet build $controlPanel -c Release --no-restore -warnaserror }
Invoke-Step 'the Control Panel: publish' { dotnet publish $controlPanel -c Release -o $controlPanelPublish --no-build }
"hMailCP.dll $((Get-Item (Join-Path $controlPanelPublish 'hMailCP.dll')).VersionInfo.FileVersion)"

Invoke-Step 'the .NET Desktop Runtime (get-dotnet-runtime.ps1)' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-dotnet-runtime.ps1 *>&1 | Select-Object -Last 3 | ForEach-Object { "$_" }
}
Invoke-Step 'the third-party files (get-installer-binaries.ps1)' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-installer-binaries.ps1 *>&1 | Select-Object -Last 5 | ForEach-Object { "$_" }
}
Invoke-Step 'the third-party files, checked (get-installer-binaries.ps1 -Verify)' {
   & $shell -NoProfile -ExecutionPolicy Bypass -File build\get-installer-binaries.ps1 -Verify *>&1 | Select-Object -Last 3 | ForEach-Object { "$_" }
}

"--- the checkout, after the builds"
# A build that rewrote a tracked file built from something the commit does not have:
# build-tools.ps1 publishes without --locked-mode, and a restore rewrites a
# packages.lock.json that disagrees with the packages it resolved
# (hmailserver\source\Tools\Directory.Build.props says when that happens).
$rewritten = @(git status --porcelain --untracked-files=no | Where-Object { $_ })
if ($LASTEXITCODE -ne 0) { Stop-Installer 5 "git status failed in $BenchDir (exit $LASTEXITCODE)" }
if ($rewritten.Count -gt 0) {
   $rewritten | Select-Object -First 20 | ForEach-Object { "  $_" }
   Stop-Installer 5 (("the builds changed {0} tracked file(s) in {1}, so what they built is not {2}'s. Find out why, restore them " +
      "(git -C {1} checkout -- . as the runner's account), and retry the job") -f $rewritten.Count, $BenchDir, $Commit)
}
"no tracked file changed while the installer's contents were built"

$output = 'hmailserver\installation\Output'
if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
Invoke-Step 'the installer (ISCC hMailServer64.iss)' {
   & $iscc /Q 'hmailserver\installation\hMailServer64.iss' 2>&1 | Select-Object -Last 20 | ForEach-Object { "$_" }
}

"--- what was built"
$built = @(Get-ChildItem -LiteralPath $output -Filter '*.exe' -File -ErrorAction SilentlyContinue)
if ($built.Count -ne 1 -or $built[0].Name -ne $installerName) {
   Stop-Installer 4 (("ISCC made {0}, not {1}: the name a Windows server looks for is made from the tag, and section_setup_64.iss's " +
      "OutputBaseFilename was not stamped for it (RELEASE.md step 7)") -f (($built | ForEach-Object Name) -join ', '), $installerName)
}
$installer = $built[0].FullName
# The version resource the server holds an installer to: <version>.0, the numbers of
# the tag without a suffix (a 6.3.5-rc1 installer says 6.3.5.0).
$numbers = ($version -replace '[^0-9.].*$', '')
$expected = "$numbers.0"
$stated = (Get-Item -LiteralPath $installer).VersionInfo.FileVersion
if ($stated -ne $expected) { Stop-Installer 4 "the installer's version resource says $stated, not $expected" }
$signature = Get-AuthenticodeSignature -LiteralPath $installer
Copy-Item -LiteralPath $installer -Destination (Join-Path $OutDir $installerName) -Force
"$installerName  $((Get-Item -LiteralPath $installer).Length) bytes  sha256 $((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash)"
"version resource $stated; Authenticode: $($signature.Status) (release-authenticode signs it)"
"INSTALLER EXIT=0"
exit 0
