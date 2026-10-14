# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
The build kit of the active-active Windows lab: the Windows server of one commit,
built with build\build.ps1 -Configuration Release (post-build events off, so nothing
is registered, stopped or started on the build machine), and put together as the
installer lays an installation out (installation\section_files_64.iss and
section_files_common.iss): Bin (hMailServer.exe and .tlb, the C++ runtime the
installer ships app-local, OpenSSL, libpq, the MariaDB client and its plugins,
dh2048.pem), DBScripts, Languages, Events and WebAdmin, with build-info.txt and
SHA256SUMS.txt. aa-win.py deploys it (node.ps1 install and deploy check the sums).

Run on the build machine (server1), in PowerShell 7, from anywhere:
  pwsh -File make-kit.ps1 -Commit <sha> [-Work C:\t1\aa-lab] [-From C:\bench\hmailserver]

-From is a clone that has the commit (on server1, C:\bench\hmailserver, which
push-to-server1.sh delivers branches to); -Work holds the tree (src), the build's
log and the kit, kit-<sha12>.zip. The ignored inputs the build reads and the
installer ships are copied from -From's tree: libraries\msado28, and the MariaDB
client's folder that the commit's binary manifest
(hmailserver\docs\third-party-binaries.json) names,
libraries\mariadb-connector-c-<version>, which build\get-installer-binaries.ps1
puts in a checkout at a commit with that manifest. The client goes into the kit
file by file as the manifest lists it, each file held to the manifest's SHA-256,
so that the kit carries the client the commit's installer ships and nothing else
of that folder; a -From without the folder, or in which one of those files is
missing or not the manifest's, is refused before the build. hMailServerLibs (the
machine's environment) names the libraries, as build.ps1 expects.
#>
param(
    [Parameter(Mandatory = $true)][string]$Commit,
    [string]$Work = 'C:\t1\aa-lab',
    [string]$From = 'C:\bench\hmailserver'
)
$ErrorActionPreference = 'Stop'
$src = Join-Path $Work 'src'
New-Item -ItemType Directory -Force -Path $Work | Out-Null

if (-not (Test-Path (Join-Path $src '.git'))) {
    git clone -q --no-checkout $From $src
    if ($LASTEXITCODE -ne 0) { throw "cloning $From failed" }
}
git -C $src cat-file -t $Commit 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    git -C $src fetch -q $From '+refs/heads/*:refs/remotes/from/*' '+refs/remotes/*:refs/remotes/from-remotes/*'
    git -C $src cat-file -t $Commit 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "commit $Commit is not in $From" }
}
git -C $src checkout -q --detach --force $Commit
if ($LASTEXITCODE -ne 0) { throw "checking out $Commit failed" }
$sha = (git -C $src rev-parse HEAD).Trim()
$sha12 = $sha.Substring(0, 12)
Write-Host "source: $src at $sha"

$to = Join-Path $src 'libraries\msado28'
if (-not (Test-Path $to)) {
    robocopy (Join-Path $From 'libraries\msado28') $to /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copying libraries\msado28 failed ($LASTEXITCODE)" }
}

# The MariaDB client the installer ships - libmysql.dll and the plugins
# section_files_common.iss names one by one - as this commit's binary manifest has it:
# the fetched x64 files under one libraries\mariadb-connector-c-<version> folder, the
# archive members the installer's Extras copies are made from too, and what
# post-build.bat puts beside the server the suite runs. Read from the manifest rather
# than written here, so that the kit follows the client the commit ships: until 14
# October 2026 this script named the 3.4.9 folder, which the move of the installer to
# 3.4.11 left on the build machines beside the new one, and so made kits with the old
# client.
$manifest = Get-Content -Raw -LiteralPath (Join-Path $src 'hmailserver\docs\third-party-binaries.json') | ConvertFrom-Json
$client = @($manifest.artifacts | Where-Object {
        $_.disposition -eq 'fetched' -and -not $_.platform -and ([string]$_.path) -match '^libraries/mariadb-connector-c-[^/]+/'
    })
$clientDirs = @($client | ForEach-Object { (([string]$_.path) -split '/')[0..1] -join '\' } | Sort-Object -Unique)
if ($clientDirs.Count -ne 1 -or -not ($client | Where-Object { ([string]$_.path) -match '/libmysql\.dll$' })) {
    throw "the binary manifest at $sha12 names $($clientDirs.Count) MariaDB client folder(s) under libraries ($($clientDirs -join ', ')), not one with libmysql.dll in it"
}
$clientDir = $clientDirs[0]
$fromClient = Join-Path $From $clientDir
if (-not (Test-Path -LiteralPath $fromClient -PathType Container)) {
    throw "$From has no $clientDir, the MariaDB client the binary manifest at $sha12 names: run build\get-installer-binaries.ps1 in $From at a commit with that manifest"
}
# Mirrored each time, so that src holds what -From holds now and nothing an earlier
# run left; then every file held to the manifest.
robocopy $fromClient (Join-Path $src $clientDir) /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "copying $clientDir failed ($LASTEXITCODE)" }
foreach ($a in $client) {
    $name = ([string]$a.path) -replace '/', '\'
    $file = Join-Path $src $name
    $hash = if (Test-Path -LiteralPath $file -PathType Leaf) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLower() } else { 'none, the file is missing' }
    if ($hash -ne ([string]$a.sha256).ToLower()) {
        throw "$From's $name is not the file the binary manifest at $sha12 names (sha256 $hash, the manifest's $($a.sha256)): run build\get-installer-binaries.ps1 in $From at a commit with that manifest"
    }
}
Write-Host ("MariaDB client: {0}, {1} file(s), each as the binary manifest has it" -f $clientDir, $client.Count)

$out = Join-Path $src 'hmailserver\source\Server\hMailServer\x64\Release'
$exe = Join-Path $out 'hMailServer.exe'
$log = Join-Path $Work "build-$sha12.log"
$built = Join-Path $Work 'built-commit.txt'
if (-not (Test-Path $exe) -or ((Get-Content $built -ErrorAction SilentlyContinue) -ne $sha)) {
    & pwsh -NoProfile -File (Join-Path $src 'build\build.ps1') -Configuration Release *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 40; throw "build.ps1 failed ($LASTEXITCODE): $log" }
    Set-Content -Path $built -Value $sha -NoNewline
}
if (Select-String -Path $log -Pattern 'error C\d+|error LNK\d+' -Quiet -ErrorAction SilentlyContinue) {
    throw "compile or link errors in $log"
}
Write-Host ("built: {0} {1:yyyy-MM-dd HH:mm:ss}" -f $exe, (Get-Item $exe).LastWriteTime)

$kit = Join-Path $Work "kit-$sha12"
if (Test-Path $kit) { Remove-Item -Recurse -Force $kit }
$bin = Join-Path $kit 'Bin'
New-Item -ItemType Directory -Force -Path $bin, (Join-Path $bin 'plugin') | Out-Null
$libs = $env:hMailServerLibs
if (-not $libs) { $libs = [Environment]::GetEnvironmentVariable('hMailServerLibs', 'Machine') }
Copy-Item -Path $exe, (Join-Path $out 'hMailServer.tlb') -Destination $bin
$minidump = Join-Path $out 'hMailServer.Minidump.exe'
if (Test-Path $minidump) { Copy-Item $minidump $bin }
Copy-Item -Path "$libs\openssl-4.0.3\out64\bin\libcrypto-4-x64.dll", "$libs\openssl-4.0.3\out64\bin\libssl-4-x64.dll" -Destination $bin
Copy-Item -Path "$libs\postgresql-18.6\builddir\src\interfaces\libpq\*.dll" -Destination $bin
# The client file by file, as the manifest lists it (libmysql.dll in Bin, the plugins
# in Bin\plugin, where the installer puts them): a wildcard would take whatever else
# the folder holds, as section_files_common.iss says of its own list.
foreach ($a in $client) {
    $inKit = Join-Path $bin (([string]$a.path).Substring($clientDir.Length + 1) -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path $inKit) | Out-Null
    Copy-Item -LiteralPath (Join-Path $src (([string]$a.path) -replace '/', '\')) -Destination $inKit
}
Copy-Item -Path (Join-Path $src 'hmailserver\installation\Extras\dh2048.pem') -Destination $bin
Copy-Item -Path (Join-Path $src 'hmailserver\installation\License.rtf') -Destination $bin -ErrorAction SilentlyContinue

# The C++ runtime the installer ships app-local (its Microsoft.VC145.CRT folder),
# from the Build Tools the binaries were built with.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem -Path (Join-Path $vs 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC145.CRT' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $crt) { throw "no Microsoft.VC145.CRT under $vs\VC\Redist\MSVC" }
Copy-Item -Path "$crt\*.dll" -Destination $bin

robocopy (Join-Path $src 'hmailserver\source\DBScripts') (Join-Path $kit 'DBScripts') *.sql /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $src 'hmailserver\source\Translations') (Join-Path $kit 'Languages') /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $src 'hmailserver\source\Addons\Events') (Join-Path $kit 'Events') /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $out 'WebAdmin') (Join-Path $kit 'WebAdmin') /E /NFL /NDL /NJH /NJS /NP | Out-Null
$global:LASTEXITCODE = 0

$version = (Get-Item $exe).VersionInfo
@(
    "commit=$sha",
    "built=$((Get-Item $exe).LastWriteTime.ToString('s'))",
    "file_version=$($version.FileVersion)",
    "crt=$crt",
    "mariadb_client=$clientDir"
) | Set-Content -Path (Join-Path $kit 'build-info.txt')
Get-ChildItem -Recurse -File $kit | Get-FileHash -Algorithm SHA256 | ForEach-Object {
    '{0}  {1}' -f $_.Hash.ToLower(), $_.Path.Substring($kit.Length + 1)
} | Set-Content -Path (Join-Path $kit 'SHA256SUMS.txt')
$zip = Join-Path $Work "kit-$sha12.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$kit\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host ("KIT {0} ({1:N1} MB), file version {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $version.FileVersion)
