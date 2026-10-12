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
installer ships - libraries\msado28 and libraries\mariadb-connector-c-3.4.9 - are
copied from -From's tree. hMailServerLibs (the machine's environment) names the
libraries, as build.ps1 expects.
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

foreach ($dir in @('libraries\msado28', 'libraries\mariadb-connector-c-3.4.9')) {
    $to = Join-Path $src $dir
    if (-not (Test-Path $to)) {
        robocopy (Join-Path $From $dir) $to /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "copying $dir failed ($LASTEXITCODE)" }
    }
}

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
Copy-Item -Path (Join-Path $src 'libraries\mariadb-connector-c-3.4.9\libmysql.dll') -Destination $bin
Copy-Item -Path (Join-Path $src 'libraries\mariadb-connector-c-3.4.9\plugin\*.dll') -Destination (Join-Path $bin 'plugin')
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
    "crt=$crt"
) | Set-Content -Path (Join-Path $kit 'build-info.txt')
Get-ChildItem -Recurse -File $kit | Get-FileHash -Algorithm SHA256 | ForEach-Object {
    '{0}  {1}' -f $_.Hash.ToLower(), $_.Path.Substring($kit.Length + 1)
} | Set-Content -Path (Join-Path $kit 'SHA256SUMS.txt')
$zip = Join-Path $Work "kit-$sha12.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$kit\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host ("KIT {0} ({1:N1} MB), file version {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $version.FileVersion)
