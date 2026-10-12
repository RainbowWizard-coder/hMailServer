# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Builds the MariaDB Connector/C client the x64 installer ships - libmariadb.dll as
    Bin\libmysql.dll and its authentication plugins in Bin\plugin - and packs it as the
    build-inputs archive build/get-installer-binaries.ps1 fetches.

.DESCRIPTION
    Until 6.3.7 these files were MariaDB's own build: the client files of MariaDB Server
    12.3.2's mariadb-12.3.2-winx64.zip, which bundles Connector/C 3.4.9
    (hmailserver/docs/ThirdPartyBinaries.md, "Provenance established for the Connector/C
    files"). 3.4.1 to 3.4.9 have CVE-2026-107819, fixed in 3.4.10, a version MariaDB
    tagged and never released, and MariaDB publishes 3.4.11, the first release with
    the fix, as source only: no Windows package, and no MariaDB Server release bundles
    it yet. So this script builds it, the way MariaDB's own build of the server builds
    the copy it bundles - which is how the 3.4.9 files were built (their PE debug
    records name MariaDB's buildbot, libmariadb inside the server tree, RelWithDebInfo):

      - The source archive from MariaDB's own download archive (archive.mariadb.org),
        held to the SHA-256 pinned below - the value MariaDB publishes in the release's
        sha256sums.txt, which MariaDB signs with its release key, as it signs the
        archive (MariaDB Signing Key, fingerprint
        177F 4010 FE56 CA33 3630 0305 F165 6F24 C74C D1D8).
      - Connector/C added as a subproject, with the options MariaDB Server's
        cmake/mariadb_connector_c.cmake gives it: Schannel for TLS (what a server built
        with WolfSSL, as MariaDB's Windows packages are, gives the client), an external
        zlib, no curl, the MySQL compatibility names, the named pipe, shared memory and
        socket transports linked in. Built standalone, Connector/C would link the C
        runtime statically (/MT); as a subproject it takes its parent's.
      - The compiler and linker settings of the server's cmake/os/Windows.cmake: the
        C runtime DLLs (/MD), program-database debug information, RelWithDebInfo with
        its default inlining restored, /Gw, /utf-8, the same warnings as errors, and
        /INCREMENTAL:NO /RELEASE /OPT:REF,ICF.
      - zlib: the tree's own, hmailserver/source/Server/zlib (1.3.2, the version MariaDB
        Server 12.3.2 bundles), given to Connector/C as the MariaDB::zlib target it
        looks for when the server tree provides one. Connector/C's own copy is 1.3.1.
        The client calls compress2, uncompress and crc32 only.

    What MariaDB's build does not do and this one does, so that the result is
    reproducible: /Brepro for the compiler and the linker (no time stamps; the debug
    record's identifier derived from the content), /pathmap for the three source and
    build folders, with /experimental:deterministic, which /pathmap needs, and
    /PDBALTPATH so the DLL names its PDB without a path. The archive
    is written with its entries in a fixed order, a fixed time stamp and no compression,
    so it does not depend on the deflate implementation of the .NET that writes it. Two
    builds with the same Visual Studio, Windows SDK and CMake give the same bytes
    wherever the library folder is.

    What is checked after the build, rather than assumed: each file is an x64 image;
    libmariadb.dll imports nothing but Windows, the C runtime DLLs and Schannel's
    libraries; each plugin exports _mysql_client_plugin_declaration_ and imports no
    client library (hMailServer renames libmariadb.dll to libmysql.dll, so a plugin that
    imported libmariadb.dll would not load); every file says it is Connector/C <Version>.

    The archive holds libmysql.dll and plugin\ with the seven authentication plugins
    Connector/C builds as DLLs: auth_gssapi_client, caching_sha2_password,
    client_ed25519, dialog, mysql_clear_password, parsec, sha256_password. The 3.4.9
    archive held two more files from MariaDB Server's own tree, which this one does not:
    auth_named_pipe.dll, the server's named_pipe authentication plugin, which no client
    loads, and authentication_windows_client.dll, the client of MySQL Enterprise
    Edition's Windows authentication; both are GPL-2.0 code, and neither is Connector/C.

    Output, in -OutDir:
      mariadb-connector-c-<Version>-win64-client.zip                the build input
      mariadb-connector-c-<Version>-win64-client-debugsymbols.zip   the PDBs, for a crash dump
      SHA256SUMS                                                    every file above and in the archive
      build-environment.txt                                         the tools that built it

    Both text files have LF line ends and no byte order mark: the first two lines of
    SHA256SUMS, the two archives', are in sha256sum's own format, and sha256sum -c would
    take a CR at the end of a line for part of the file's name.

    Prerequisites:
      - PowerShell 7 (pwsh), as the example below runs it. Windows PowerShell 5.1 makes
        the first line a native tool writes to stderr a terminating error under this
        script's error preference, and cl writes its banner there.
      - The environment variable hMailServerLibs, pointing at your library folder.
      - Visual Studio 2026 with the x64 C++ tools and the "C++ CMake tools for Windows"
        component (CMake and Ninja; vcvars64 puts them on PATH).

.PARAMETER Version
    The Connector/C version to build, e.g. 3.4.11. Must be in the digest table below,
    or given with -Sha256.

.PARAMETER Sha256
    The SHA-256 of mariadb-connector-c-<Version>-src.tar.gz, for a version the digest
    table does not know yet. Take it from MariaDB's sha256sums.txt beside the archive,
    after checking that file's signature.

.PARAMETER Archive
    A copy of mariadb-connector-c-<Version>-src.tar.gz you already have, used instead
    of a download; it is held to the same digest.

.PARAMETER OutDir
    Where the archives and SHA256SUMS are written. Default:
    %hMailServerLibs%\mariadb-connector-c-<Version>-out. The script empties it of its
    own earlier outputs first - the two archives of any version, SHA256SUMS and
    build-environment.txt - and refuses a folder that holds anything else, before it
    downloads or builds anything, so that -OutDir never costs a file the script did
    not write. A new folder, an empty one or an earlier -OutDir of this script will do.

.EXAMPLE
    pwsh -NoProfile -File libraries\build-mariadb-connector-c.ps1 -Version 3.4.11
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [ValidatePattern('^3\.4\.\d+$')]
    [string]$Version = '3.4.11',

    [Parameter(Mandatory = $false)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$Sha256,

    [Parameter(Mandatory = $false)]
    [string]$Archive,

    [Parameter(Mandatory = $false)]
    [string]$OutDir
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

# The SHA-256 of each source archive, as MariaDB publishes it in
# https://archive.mariadb.org/connector-c-<version>/sha256sums.txt (and in
# https://downloads.mariadb.org/rest-api/connector-c/<major.minor>/).
$Digests = @{
    '3.4.11' = '521c0712e9291fa96558df9e2ff431376a3a79329f13751896b694cae12765b4'
}

# The authentication plugins Connector/C builds as DLLs (plugins/auth/CMakeLists.txt,
# DEFAULT DYNAMIC). mysql_native_password and mysql_old_password are linked into
# libmariadb.dll, as are the three transports.
$Plugins = @('auth_gssapi_client', 'caching_sha2_password', 'client_ed25519', 'dialog', 'mysql_clear_password', 'parsec', 'sha256_password')

# What libmariadb.dll may import: Windows, the C runtime DLLs, Schannel's libraries.
# The 3.4.9 file MariaDB built imported exactly these.
$AllowedImports = @('ADVAPI32.dll', 'bcrypt.dll', 'CRYPT32.dll', 'KERNEL32.dll', 'Secur32.dll', 'SHLWAPI.dll', 'VCRUNTIME140.dll', 'WS2_32.dll')

# The time stamp every entry of the archives carries: the earliest a ZIP can say.
$EntryTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)

. (Join-Path -Path $PSScriptRoot -ChildPath "build-common.ps1")

$logPath = Join-Path -Path $PSScriptRoot -ChildPath "build-mariadb-connector-c.log"
Start-BuildLog -LogPath $logPath -Title "MariaDB Connector/C $Version (x64) build log"

$libsPath = Resolve-HMailServerLibs
$srcDir = Join-Path -Path $libsPath -ChildPath "mariadb-connector-c-$Version-src"
$workDir = Join-Path -Path $libsPath -ChildPath "mariadb-connector-c-$Version-build"
$wrapperDir = Join-Path -Path $workDir -ChildPath "wrapper"
$buildDir = Join-Path -Path $workDir -ChildPath "out"
$stageDir = Join-Path -Path $workDir -ChildPath "stage"
$symbolsDir = Join-Path -Path $workDir -ChildPath "symbols"
if (-not $OutDir)
{
    $OutDir = Join-Path -Path $libsPath -ChildPath "mariadb-connector-c-$Version-out"
}
# -OutDir may hold nothing but what this script writes there: it is emptied of those
# files before the build, and a folder with anything else in it - -OutDir ., or one's
# Downloads - is a refusal now, before anything is downloaded or built, rather than a
# folder deleted.
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
$ownOutputs = '^(mariadb-connector-c-\d+\.\d+\.\d+-win64-client(-debugsymbols)?\.zip|SHA256SUMS|build-environment\.txt)$'
if (Test-Path -LiteralPath $OutDir -PathType Leaf)
{
    Throw "-OutDir $OutDir is a file, not a folder."
}
if (Test-Path -LiteralPath $OutDir)
{
    $foreign = @(Get-ChildItem -LiteralPath $OutDir -Force | Where-Object { $_.PSIsContainer -or $_.Name -notmatch $ownOutputs })
    if ($foreign.Count)
    {
        Throw ("-OutDir {0} holds {1} item(s) this script did not write ({2}). It empties -OutDir before the build, so it takes only a new folder, an empty one, or one that holds nothing but its own earlier outputs." -f $OutDir, $foreign.Count, (($foreign | Select-Object -First 5 | ForEach-Object { $_.Name }) -join ', '))
    }
}
$zlibDir = [System.IO.Path]::GetFullPath((Join-Path -Path $PSScriptRoot -ChildPath "..\hmailserver\source\Server\zlib"))
if (-not (Test-Path (Join-Path -Path $zlibDir -ChildPath "zlib.h")))
{
    Throw "The tree's zlib was not found at $zlibDir."
}
$zlibVersion = (Select-String -Path (Join-Path $zlibDir "zlib.h") -Pattern '#define ZLIB_VERSION "([0-9.]+)"' | Select-Object -First 1).Matches[0].Groups[1].Value

# --- The source --------------------------------------------------------------------

$digest = Get-PinnedDigest -Digests $Digests -Version $Version -Library 'MariaDB Connector/C' -Override $Sha256
$url = if ($Archive) { $Archive } else { "https://archive.mariadb.org/connector-c-$Version/mariadb-connector-c-$Version-src.tar.gz" }
Get-SourceArchive -Url $url -SrcDir $srcDir -LibsPath $libsPath -Sha256 $digest

$cmakeLists = Get-Content -LiteralPath (Join-Path $srcDir "CMakeLists.txt") -Raw
$parts = foreach ($name in 'MAJOR', 'MINOR', 'PATCH') { if ($cmakeLists -match "SET\(CPACK_PACKAGE_VERSION_$name (\d+)\)") { $Matches[1] } }
if (($parts -join '.') -ne $Version)
{
    Throw "The archive's CMakeLists.txt says it is Connector/C $($parts -join '.'), not $Version."
}

# --- The wrapper: what MariaDB Server's build gives the Connector/C it bundles -------

# The work folder is this script's own, under the library folder. -OutDir loses its
# earlier outputs alone, file by file: it was checked above to hold nothing else.
if (Test-Path -LiteralPath $workDir) { Remove-Item -LiteralPath $workDir -Recurse -Force }
New-Item -ItemType Directory -Path $workDir | Out-Null
if (Test-Path -LiteralPath $OutDir)
{
    Get-ChildItem -LiteralPath $OutDir -Force -File | Where-Object { $_.Name -match $ownOutputs } | Remove-Item -Force
}
else
{
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}
New-Item -ItemType Directory -Path $wrapperDir, $stageDir, (Join-Path $stageDir "plugin"), $symbolsDir | Out-Null

$cmake = @'
# Written by libraries/build-mariadb-connector-c.ps1. MariaDB Connector/C configured as
# MariaDB Server's build configures the copy it bundles: cmake/os/Windows.cmake for the
# compiler and linker, cmake/mariadb_connector_c.cmake for Connector/C's options.
cmake_minimum_required(VERSION 3.25)
project(hmailserver_mariadb_connector_c C)

# cmake/os/Windows.cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
add_link_options(/DEBUG)
add_compile_definitions(_CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_WARNINGS _WIN32_WINNT=0x0A00 NOMINMAX NOSERVICE WIN32_LEAN_AND_MEAN)
foreach(suffix _RELWITHDEBINFO _RELWITHDEBINFO_INIT)
  string(REGEX REPLACE "/Ob[0-1]" "" CMAKE_C_FLAGS${suffix} "${CMAKE_C_FLAGS${suffix}}")
endforeach()
add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:/INCREMENTAL:NO;/RELEASE;/OPT:REF,ICF>")
add_compile_options($<$<COMPILE_LANGUAGE:C>:$<$<CONFIG:Release,RelWithDebInfo>:/Gw>>)
add_compile_options("$<$<COMPILE_LANGUAGE:C>:/we4099;/we4700;/we4311;/we4477;/we4302;/we4090>")
add_compile_options($<$<COMPILE_LANGUAGE:C>:/utf-8>)
add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:$<$<COMPILE_LANGUAGE:C>:/d2OptimizeHugeFunctions>>)

# Not MariaDB's: what makes the result the same wherever it is built. The compiler is
# given native paths, so the maps are written in that form.
file(TO_NATIVE_PATH "${HM_SOURCE}" hm_source_native)
file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" hm_build_native)
file(TO_NATIVE_PATH "${HM_ZLIB}" hm_zlib_native)
add_compile_options("$<$<COMPILE_LANGUAGE:C>:/Brepro;/experimental:deterministic;/pathmap:${hm_source_native}=mariadb-connector-c-src;/pathmap:${hm_build_native}=build;/pathmap:${hm_zlib_native}=zlib>")
add_link_options(/Brepro "/PDBALTPATH:%_PDB%")
set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} /Brepro")

# The server tree's zlib: here the hMailServer tree's, the same version.
add_library(hm_zlib STATIC
  ${HM_ZLIB}/adler32.c ${HM_ZLIB}/compress.c ${HM_ZLIB}/crc32.c ${HM_ZLIB}/deflate.c
  ${HM_ZLIB}/inffast.c ${HM_ZLIB}/inflate.c ${HM_ZLIB}/inftrees.c ${HM_ZLIB}/trees.c
  ${HM_ZLIB}/uncompr.c ${HM_ZLIB}/zutil.c)
target_include_directories(hm_zlib PUBLIC ${HM_ZLIB})
add_library(MariaDB::zlib ALIAS hm_zlib)

# cmake/mariadb_connector_c.cmake
set(OPT CONC_)
set(CONC_WITH_SIGNCODE OFF)
set(CONC_WITH_EXTERNAL_ZLIB ON)
set(CONC_WITH_SSL SCHANNEL)
set(CONC_WITH_CURL OFF)
set(CONC_WITH_MYSQLCOMPAT ON)
set(CONC_INSTALL_LAYOUT DEFAULT)
set(CONC_WITH_UNIT_TESTS OFF)
# The server's install layout names the plugin folder; Connector/C as a subproject
# takes it from there (cmake/install.cmake).
set(INSTALL_PLUGINDIR lib/plugin)
set(PLUGIN_INSTALL_DIR ${INSTALL_PLUGINDIR})
set(CLIENT_PLUGIN_PVIO_NPIPE STATIC)
set(CLIENT_PLUGIN_PVIO_SHMEM STATIC)
set(CLIENT_PLUGIN_PVIO_SOCKET STATIC)
add_subdirectory(${HM_SOURCE} libmariadb)
'@
Set-Content -LiteralPath (Join-Path $wrapperDir "CMakeLists.txt") -Value $cmake -Encoding ASCII

# --- Build -------------------------------------------------------------------------

$vsInstall = Resolve-VcVars64
Import-VsEnvironment -VsInstall $vsInstall
foreach ($tool in 'cmake', 'ninja', 'dumpbin')
{
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue))
    {
        Throw "$tool is not on PATH after vcvars64. Add the 'C++ CMake tools for Windows' component in the Visual Studio Installer."
    }
}

$cmakeVersion = ((& cmake --version) | Select-Object -First 1)
# cl writes its banner to stderr and its usage to stdout, and 2>&1 passes each line on as it
# arrives, in either order: the banner is found by its words, not by its place.
$clLines = @((& cl.exe 2>&1) | ForEach-Object { if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { [string]$_ } })
$clBanner = $clLines | Where-Object { $_ -match 'Compiler Version' } | Select-Object -First 1
if (-not $clBanner) { $clBanner = $clLines | Select-Object -First 1 }
$environment = @(
    "MariaDB Connector/C $Version from $url, SHA-256 $digest",
    "zlib $zlibVersion from hmailserver/source/Server/zlib",
    "Visual Studio $($vsInstall.Version) at $($vsInstall.InstallPath)",
    "Compiler: $clBanner",
    # vcvars ends WindowsSDKVersion with a backslash (10.0.26100.0\).
    "VCToolsVersion $env:VCToolsVersion, Windows SDK $(([string]$env:WindowsSDKVersion).TrimEnd('\'))",
    $cmakeVersion,
    "Ninja $((& ninja --version))"
)
$environment | ForEach-Object { Write-Log $_ }

$targets = @('libmariadb') + $Plugins
Invoke-BuildStep "Configuring Connector/C $Version (RelWithDebInfo, Schannel, the server's settings)" {
    cmake -G Ninja -S $wrapperDir -B $buildDir -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DHM_SOURCE=$($srcDir -replace '\\', '/')" "-DHM_ZLIB=$($zlibDir -replace '\\', '/')" "-DCMAKE_INSTALL_PREFIX=C:/Program Files/MariaDB/MariaDB Connector C 64-bit"
}
if ($LastExitCode -ne 0) { Throw "CMake configuration failed with exit code $LastExitCode. See $logPath." }

Invoke-BuildStep "Building $($targets -join ', ')" {
    cmake --build $buildDir --target @targets
}
if ($LastExitCode -ne 0) { Throw "The build failed with exit code $LastExitCode. See $logPath." }

# --- Check what was built ----------------------------------------------------------

function Get-Built([string]$name)
{
    $found = @(Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter "$name.dll")
    if ($found.Count -ne 1) { Throw "Expected one $name.dll under $buildDir, found $($found.Count)." }
    return $found[0]
}

function Get-Dumpbin([string]$what, [string]$path)
{
    $text = & dumpbin /nologo $what $path | Out-String
    if ($LastExitCode -ne 0) { Throw "dumpbin $what failed on $path." }
    return $text
}

function Get-Imports([string]$path)
{
    $text = Get-Dumpbin '/dependents' $path
    return @([regex]::Matches($text, '(?m)^\s+(\S+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value })
}

function Assert-Built([System.IO.FileInfo]$file, [string]$product)
{
    $headers = Get-Dumpbin '/headers' $file.FullName
    if ($headers -notmatch '8664 machine \(x64\)') { Throw "$($file.Name) is not an x64 image." }
    $info = $file.VersionInfo
    if ($info.ProductName -ne 'MariaDB Connector/C' -or $info.ProductVersion -ne $product)
    {
        Throw "$($file.Name) says it is '$($info.ProductName)' '$($info.ProductVersion)', not MariaDB Connector/C $product."
    }
    # No folder of this machine may be in the file, or another machine's build differs.
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($file.FullName))
    foreach ($folder in $libsPath, $zlibDir)
    {
        if ($text.IndexOf($folder, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $text.IndexOf(($folder -replace '\\', '/'), [StringComparison]::OrdinalIgnoreCase) -ge 0)
        {
            Throw "$($file.Name) names the folder ${folder}: /pathmap did not cover it, and the build is not reproducible."
        }
    }
}

$library = Get-Built 'libmariadb'
Assert-Built $library $Version
$imports = Get-Imports $library.FullName
$unexpected = @($imports | Where-Object { $AllowedImports -notcontains $_ -and $_ -notlike 'api-ms-win-crt-*' })
if ($unexpected.Count) { Throw "libmariadb.dll imports $($unexpected -join ', '), which nothing ships beside it. Its imports: $($imports -join ', ')." }
if ($imports -notcontains 'VCRUNTIME140.dll') { Throw "libmariadb.dll does not import VCRUNTIME140.dll: it was not linked against the C runtime DLLs (/MD). Its imports: $($imports -join ', ')." }
if ($imports -notcontains 'Secur32.dll') { Throw "libmariadb.dll does not import Secur32.dll: it was built without Schannel. Its imports: $($imports -join ', ')." }
Write-Log "libmariadb.dll imports $($imports -join ', ')"
Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $stageDir "libmysql.dll")
Copy-Item -LiteralPath ([System.IO.Path]::ChangeExtension($library.FullName, '.pdb')) -Destination (Join-Path $symbolsDir "libmariadb.pdb")

foreach ($plugin in $Plugins)
{
    $file = Get-Built $plugin
    Assert-Built $file $Version
    $exports = Get-Dumpbin '/exports' $file.FullName
    if ($exports -notmatch '_mysql_client_plugin_declaration_') { Throw "$plugin.dll does not export _mysql_client_plugin_declaration_: the client cannot load it." }
    $pluginImports = Get-Imports $file.FullName
    $client = @($pluginImports | Where-Object { $_ -match '^lib(mariadb|mysql)' })
    if ($client.Count) { Throw "$plugin.dll imports $($client -join ', '); beside hMailServer the client is libmysql.dll, so it would not load." }
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $stageDir "plugin\$plugin.dll")
    Copy-Item -LiteralPath ([System.IO.Path]::ChangeExtension($file.FullName, '.pdb')) -Destination (Join-Path $symbolsDir "$plugin.pdb")
}

# --- Pack --------------------------------------------------------------------------

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# LF after every line and no byte order mark, where Set-Content ends each line with CR LF
# on Windows: sha256sum -c reads the first two lines of SHA256SUMS, and a CR would be part
# of the file name it looks for.
function Write-TextLines([string]$path, [string[]]$lines)
{
    [System.IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
}

# Stored entries, in the order given, each stamped $EntryTime: the same files give the
# same archive.
function New-StoredZip([string]$zipPath, [string]$root, [string[]]$entries)
{
    Add-Type -AssemblyName System.IO.Compression
    $stream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
    try
    {
        $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
        try
        {
            foreach ($entry in $entries)
            {
                $item = $zip.CreateEntry($entry, [System.IO.Compression.CompressionLevel]::NoCompression)
                $item.LastWriteTime = $EntryTime
                $out = $item.Open()
                try
                {
                    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $root ($entry -replace '/', '\')))
                    $out.Write($bytes, 0, $bytes.Length)
                }
                finally { $out.Dispose() }
            }
        }
        finally { $zip.Dispose() }
    }
    finally { $stream.Dispose() }
}

$clientEntries = @('libmysql.dll') + @($Plugins | ForEach-Object { "plugin/$_.dll" })
$clientZip = Join-Path $OutDir "mariadb-connector-c-$Version-win64-client.zip"
New-StoredZip $clientZip $stageDir $clientEntries

$symbolEntries = @('libmariadb.pdb') + @($Plugins | ForEach-Object { "$_.pdb" })
$symbolsZip = Join-Path $OutDir "mariadb-connector-c-$Version-win64-client-debugsymbols.zip"
New-StoredZip $symbolsZip $symbolsDir $symbolEntries

$sums = @()
$sums += "{0}  {1}" -f (Get-Sha256 $clientZip), (Split-Path -Leaf $clientZip)
$sums += "{0}  {1}" -f (Get-Sha256 $symbolsZip), (Split-Path -Leaf $symbolsZip)
foreach ($entry in $clientEntries)
{
    $file = Join-Path $stageDir ($entry -replace '/', '\')
    $sums += "{0}  {1}#{2}  {3} bytes" -f (Get-Sha256 $file), (Split-Path -Leaf $clientZip), $entry, (Get-Item -LiteralPath $file).Length
}
Write-TextLines (Join-Path $OutDir "SHA256SUMS") $sums
Write-TextLines (Join-Path $OutDir "build-environment.txt") $environment
$sums | ForEach-Object { Write-Log $_ }
Write-Log ("{0} ({1} bytes) and its symbols written to {2}" -f (Split-Path -Leaf $clientZip), (Get-Item -LiteralPath $clientZip).Length, $OutDir)
