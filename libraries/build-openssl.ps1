# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Builds a specific OpenSSL 4.x version for hMailServer.

.DESCRIPTION
    Takes the OpenSSL source archive for the requested version from -Archive, extracts
    it into %hMailServerLibs%\openssl-<Version>, verifies it against the SHA-256 pinned below,
    then configures and builds it with the Visual Studio 2026 x64 toolchain into an
    "out64" install prefix, matching the layout hMailServer and libpq link against
    (out64\include, out64\lib, out64\bin with libcrypto-4-x64.dll / libssl-4-x64.dll).

    This is the README's recipe, unchanged: Configure for VC-WIN64A without assembler
    (so no NASM is needed), targeting Windows 10 (_WIN32_WINNT=0x0A00), then build and
    install only the libraries. The command-line openssl application is not built - it
    is not needed, and it has failed to compile in some 4.0.x source drops.

    Prerequisites (must be on PATH / installed):
      - The environment variable hMailServerLibs, pointing at your library folder.
      - Perl (e.g. Strawberry Perl) - required by OpenSSL's Configure.
      - Visual Studio 2026 with the x64 build tools (vcvars64.bat is located
        automatically via vswhere).

.PARAMETER Version
    The OpenSSL version to build, e.g. 4.0.3. Must match 4.x.y.

.PARAMETER Platform
    x64 (the default, and the build every x64 server links against) or ARM64, for
    Windows on ARM (hmailserver/docs/WindowsOnArm.md). An ARM64 build is
    cross-compiled on an x64 machine with Visual Studio's ARM64 build tools, into a
    tree of its own named with -arm64 (openssl-<Version>-arm64\out64, with
    libcrypto-4-arm64.dll and libssl-4-arm64.dll; target VC-WIN64-ARM), so it never touches the
    x64 tree.

.PARAMETER Archive
    A copy of openssl-<Version>.tar.gz that you obtained yourself, used instead of
    the download. Optional. Without it the script downloads the archive from
    Debian's archive: Debian ships OpenSSL's own release tarball unchanged as
    openssl_<Version>.orig.tar.gz, in the pool while the version is current
    (https://deb.debian.org/debian/pool/main/o/openssl/) and at
    https://snapshot.debian.org once it has left it. OpenSSL's own download address
    (openssl.org/source) redirects to its GitHub releases, and this project fetches
    nothing from GitHub since GitHub suspended its account on 18 September 2026.
    Checked on 26 September 2026: Debian's openssl_4.0.2.orig.tar.gz has exactly
    the SHA-256 pinned below. Checked on 30 September 2026: 4.0.3's SHA-256 below
    is the one Debian's signed openssl_4.0.3-1.dsc gives for
    openssl_4.0.3.orig.tar.gz, and Debian's copy of OpenSSL's signature of the
    archive (openssl_4.0.3.orig.tar.gz.asc) is good for OpenSSL's key
    B146 647E 45A7 B339 47AB 226B 2A2C 87D1 6169 2D40, the one
    openssl-library.org/source/ names for its current releases. Whatever its
    origin, the file must match the SHA-256 pinned below (or -Sha256), or it is
    refused.

.PARAMETER Sha256
    The SHA-256 of openssl-<Version>.tar.gz, for a version the digest table below does
    not know yet. Take it from the .sha256 file published beside the release.

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File libraries\build-openssl.ps1 -Version 4.0.3

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File libraries\build-openssl.ps1 -Version 4.0.3 -Archive C:\Downloads\openssl-4.0.3.tar.gz

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File libraries\build-openssl.ps1 -Version 4.0.3 -Platform ARM64 -Archive C:\Downloads\openssl-4.0.3.tar.gz
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^4\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory = $false)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$Sha256,

    [Parameter(Mandatory = $false)]
    [string]$Archive,

    [Parameter(Mandatory = $false)]
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = "Stop"

# Handle native-command exit codes explicitly (checked after each step) rather than
# letting a nonzero exit or stderr text abort the pipeline on its own.
$PSNativeCommandUseErrorActionPreference = $false

# The SHA-256 of each release archive, from the openssl-<version>.tar.gz.sha256 file
# OpenSSL published beside it (https://openssl-library.org/source/); 4.0.3's from
# Debian's signed openssl_4.0.3-1.dsc, with OpenSSL's signature of the archive
# checked (-Archive, above). See Get-PinnedDigest.
$Digests = @{
    '4.0.1' = '2db3f3a0d6ea4b59e1f094ace2c8cd536dffb87cdc39084c5afa1e6f7f37dd09'
    '4.0.2' = '736b467530f916737b7031310ccb21d8218c6229e61e8e160cd1d3458cd543a8'
    '4.0.3' = '325b5c806167c13b40b1ffeadfe0248197c00eccc4cf123ec1e28d2d2fd216d9'
}

# --- Set up a build log ---------------------------------------------------------

# OpenSSL's Configure/nmake output is verbose and the host console may buffer it, so
# mirror every step to build-openssl.log next to this script. This gives a file you can
# watch live from another shell to confirm the build is progressing:
#
#     Get-Content libraries\build-openssl.log -Wait
#
# and a full transcript to inspect if a step fails.
. (Join-Path -Path $PSScriptRoot -ChildPath "build-common.ps1")

# The ARM64 build has a log of its own, so that it does not overwrite the x64 one.
$logName = if ($Platform -eq 'ARM64') { "build-openssl-arm64.log" } else { "build-openssl.log" }
$logPath = Join-Path -Path $PSScriptRoot -ChildPath $logName
Start-BuildLog -LogPath $logPath -Title "OpenSSL $Version ($Platform) build log"

# --- Resolve the library folder -------------------------------------------------

$libsPath = Resolve-HMailServerLibs

$srcDir = Join-Path -Path $libsPath -ChildPath "openssl-$Version"
# ARM64: a tree of its own beside the x64 one, with the same out64 layout inside it.
$targetDir = $null
if ($Platform -eq 'ARM64')
{
    $targetDir = Join-Path -Path $libsPath -ChildPath "openssl-$Version-arm64"
    $srcDir = $targetDir
}
$outDir = Join-Path -Path $srcDir -ChildPath "out64"

# VC-WIN64A for x64, as always. VC-WIN64-ARM for ARM64 (Configurations/50-win-onecore.conf,
# whose multilib suffix names the DLLs libcrypto-4-arm64.dll and libssl-4-arm64.dll).
# no-asm for both: no NASM for x64, and no armasm64 step for ARM64.
$target = if ($Platform -eq 'ARM64') { 'VC-WIN64-ARM' } else { 'VC-WIN64A' }
$dllSuffix = if ($Platform -eq 'ARM64') { 'arm64' } else { 'x64' }

# --- Locate the Visual Studio build environment via vswhere --------------------

$vsInstall = Resolve-VcVars64 -Platform $Platform

# --- Verify a Windows-native Perl is available ----------------------------------

Resolve-NativePerl

# --- Download, verify and extract the source (always a clean tree) --------------

$digest = Get-PinnedDigest -Digests $Digests -Version $Version -Library 'OpenSSL' -Override $Sha256
# OpenSSL's release archives are GitHub release assets (openssl.org/source redirects
# there), and this project downloads nothing from GitHub since GitHub suspended its
# account on 18 September 2026. Debian ships the same release tarball unchanged as
# its .orig.tar.gz, so that is where it comes from, held to the pinned digest - which
# is OpenSSL's own - exactly as a download from OpenSSL was. A copy the caller has
# can be given with -Archive instead.
if ($Archive)
{
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf))
    {
        Throw "The archive given with -Archive was not found: $Archive"
    }
    if ((Split-Path -Leaf $Archive) -ne "openssl-$Version.tar.gz")
    {
        Throw "The archive given with -Archive must be named openssl-$Version.tar.gz, as OpenSSL publishes it: $Archive"
    }
    $tarUrl = (Resolve-Path -LiteralPath $Archive).ProviderPath
}
else
{
    # The pool holds the versions Debian ships now; snapshot.debian.org every one it
    # ever shipped. The digest check below is what makes either trustworthy.
    $orig = "openssl_$Version.orig.tar.gz"
    $poolUrl = "https://deb.debian.org/debian/pool/main/o/openssl/$orig"
    $download = Join-Path -Path $libsPath -ChildPath "openssl-$Version.tar.gz"
    Write-Host "Downloading OpenSSL $Version's release tarball from Debian's archive: $poolUrl"
    try
    {
        Invoke-WebRequest -Uri $poolUrl -OutFile $download -UseBasicParsing -MaximumRedirection 0
    }
    catch
    {
        Throw "OpenSSL $Version is not in Debian's pool ($poolUrl): it may have left it. Find $orig on https://snapshot.debian.org/package/openssl/ and pass it, renamed openssl-$Version.tar.gz, with -Archive. It is checked against the pinned SHA-256 $digest."
    }
    $tarUrl = $download
}
if ($targetDir)
{
    Get-SourceArchive -Url $tarUrl -SrcDir (Join-Path -Path $libsPath -ChildPath "openssl-$Version") -LibsPath $libsPath -Sha256 $digest -TargetDir $targetDir
}
else
{
    Get-SourceArchive -Url $tarUrl -SrcDir $srcDir -LibsPath $libsPath -Sha256 $digest
}

# --- Import the VS x64 build environment ---------------------------------------

Import-VsEnvironment -VsInstall $vsInstall -Platform $Platform

# --- Configure and build (each step checked individually) ----------------------

Write-Log "Building OpenSSL $Version (this takes ten to twenty minutes; nmake is single-threaded)"
Write-Log "Progress is being logged to $logPath (tail it with: Get-Content `"$logPath`" -Wait)"

Push-Location $srcDir
try
{
    Invoke-BuildStep "Configuring OpenSSL $Version for target $target" {
        perl Configure no-asm $target "--prefix=$outDir" "--openssldir=$outDir" '-D_WIN32_WINNT=0x0A00'
    }
    if ($LastExitCode -ne 0)
    {
        Throw "OpenSSL 'perl Configure' failed with exit code $LastExitCode. See $logPath for details."
    }

    Invoke-BuildStep "Compiling the libraries (nmake build_libs)" {
        nmake build_libs
    }
    if ($LastExitCode -ne 0)
    {
        Throw "OpenSSL 'nmake build_libs' failed with exit code $LastExitCode. See $logPath for details."
    }

    Invoke-BuildStep "Installing the headers, import libraries and DLLs (nmake install_dev install_runtime_libs)" {
        nmake install_dev install_runtime_libs
    }
    if ($LastExitCode -ne 0)
    {
        Throw "OpenSSL 'nmake install_dev install_runtime_libs' failed with exit code $LastExitCode. See $logPath for details."
    }
}
finally
{
    Pop-Location
}

# --- Verify the expected output -------------------------------------------------

# The DLL names carry the major version and the target: libcrypto-4-x64.dll for 4.x,
# libcrypto-4-arm64.dll for 4.x on ARM64.
$major = $Version.Split('.')[0]

$expected = @(
    "bin\libcrypto-$major-$dllSuffix.dll",
    "bin\libssl-$major-$dllSuffix.dll",
    "lib\libcrypto.lib",
    "lib\libssl.lib",
    "include\openssl\ssl.h"
) | ForEach-Object { Join-Path -Path $outDir -ChildPath $_ }

foreach ($item in $expected)
{
    if (!(Test-Path $item))
    {
        Throw "Build completed but expected output was missing: $item"
    }
}

Write-Log "OpenSSL $Version ($Platform) built successfully into $outDir"
