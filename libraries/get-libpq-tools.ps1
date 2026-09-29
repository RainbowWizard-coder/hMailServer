# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Fetches the tools build-pgsql.ps1 needs besides Perl and Visual Studio - Python,
    Meson, Ninja and winflexbison - each held to the SHA-256 pinned below, into one
    folder, without installing anything.

.DESCRIPTION
    PostgreSQL's Meson build, which build-pgsql.ps1 runs to make libpq, needs Python
    with Meson and Ninja, and flex and bison, on PATH. A developer installs them once
    (README.md, "Building the libraries"). A CI runner that has none of them - bench150,
    whose runner is LocalSystem and sees only the PATH its service was given - gets
    them from this script instead, which windows-packages.ps1 runs for the
    windows-packages-arm64 job when they are not on PATH:

      Python 3.14.7     the Python Software Foundation's own package on nuget.org
                        (python.3.14.7.nupkg: the whole interpreter in tools\, made for
                        build machines, installed nowhere); its SHA-512 is nuget.org's
                        packageHash for the package, and the SHA-256 below is of the
                        same bytes;
      Meson 1.11.1      the wheel on PyPI, the version libpq's ARM64 build was first
                        made with on 26 September 2026;
      Ninja 1.13.2      the win_amd64 wheel on PyPI, which carries ninja.exe;
      winflexbison 2.5.24
                        win_flex 2.6.4 and win_bison 3.7.4, from the project's
                        SourceForge files (its MD5 there, 6b549d43e34ece0e8ed05af92daa31c4,
                        is the file's). Its later 2.5.25 is published on GitHub only,
                        and this project fetches nothing from GitHub.

    Every file is downloaded, checked against its pin and refused - deleted, and the
    script stops - when it differs. Nothing comes from GitHub. Pip installs the two
    wheels from the files on disk (--no-index, --no-deps): it reaches no index.

    The layout under -Into:

      python\             python.exe, and Scripts\meson.exe and Scripts\ninja.exe
      winflexbison\       win_flex.exe, win_bison.exe and bison's data\
      tools.txt           the pins this folder was made from

    A folder whose tools.txt names the same pins and whose programs are there is kept;
    any other is made again from scratch. The last line printed is
    "LIBPQ TOOLS PATH=<folders>", the folders to put ahead on PATH, in that order.

    Exit codes: 0 the tools are in -Into; 1 a download, a check or an install failed
    (said above the exit).

.PARAMETER Into
    The folder to put the tools in (windows-packages.ps1 uses <LibsDir>\tools).

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File libraries\get-libpq-tools.ps1 -Into C:\bench\hMailLibs-arm64\tools
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Into
)

# Continue, with each step checked: under Windows PowerShell 5.1 and 'Stop', a line a
# native program writes to stderr would stop the script before its exit code is read.
# A cmdlet whose failure must stop it says -ErrorAction Stop, inside the try below.
$ErrorActionPreference = "Continue"
$PSNativeCommandUseErrorActionPreference = $false

# Moving a pin: take the new file's SHA-256 from where its publisher states it (PyPI's
# JSON API, nuget.org's catalog, which states SHA-512, or SourceForge's file list,
# which states MD5 and SHA-1, checked against the download), never from the download
# alone.
$Pins = @(
    [pscustomobject]@{
        Name   = 'Python 3.14.7'
        File   = 'python.3.14.7.nupkg'
        Url    = 'https://api.nuget.org/v3-flatcontainer/python/3.14.7/python.3.14.7.nupkg'
        Sha256 = '46a4da5529a92d18ff894911f6e6033a8253198d705b8161bf28c9123c87d46b'
    }
    [pscustomobject]@{
        Name   = 'Meson 1.11.1'
        File   = 'meson-1.11.1-py3-none-any.whl'
        Url    = 'https://files.pythonhosted.org/packages/5e/cd/f3a881ff5e601d6bbeff63b38ee2362e1167c47d9cde03eddf8d71a4ffb0/meson-1.11.1-py3-none-any.whl'
        Sha256 = '9b3a023657e393dbc5335b95c561337d49b7a458f5541e47ec44f2cc566e0d80'
    }
    [pscustomobject]@{
        Name   = 'Ninja 1.13.2'
        File   = 'ninja-1.13.2-py3-none-win_amd64.whl'
        Url    = 'https://files.pythonhosted.org/packages/3f/dd/3766b5f4d32e8a9b97d195496b0b01fbbe2e1a41669dab0cd6492a6ce199/ninja-1.13.2-py3-none-win_amd64.whl'
        Sha256 = '1293f4078278b70d0ee4b6cc8f3a9e030656c9b2f59909970343c4fe76070118'
    }
    [pscustomobject]@{
        Name   = 'winflexbison 2.5.24'
        File   = 'win_flex_bison-2.5.24.zip'
        Url    = 'https://downloads.sourceforge.net/project/winflexbison/win_flex_bison-2.5.24.zip'
        Sha256 = '39c6086ce211d5415500acc5ed2d8939861ca1696aee48909c7f6daf5122b505'
    }
)

$pythonDir = Join-Path -Path $Into -ChildPath 'python'
$flexDir = Join-Path -Path $Into -ChildPath 'winflexbison'
$record = Join-Path -Path $Into -ChildPath 'tools.txt'
$pathDirs = @($pythonDir, (Join-Path -Path $pythonDir -ChildPath 'Scripts'), $flexDir)
$programs = @(
    (Join-Path -Path $pythonDir -ChildPath 'python.exe'),
    (Join-Path -Path $pythonDir -ChildPath 'Scripts\meson.exe'),
    (Join-Path -Path $pythonDir -ChildPath 'Scripts\ninja.exe'),
    (Join-Path -Path $flexDir -ChildPath 'win_flex.exe'),
    (Join-Path -Path $flexDir -ChildPath 'win_bison.exe')
)
$wanted = @($Pins | ForEach-Object { "$($_.Name) $($_.File) sha256:$($_.Sha256)" })

function Stop-Tools([string]$sentence)
{
    Write-Output "LIBPQ TOOLS NOT FETCHED: $sentence"
    exit 1
}

$kept = $false
if ((Test-Path -LiteralPath $record) -and -not @($programs | Where-Object { -not (Test-Path -LiteralPath $_) }).Count)
{
    $have = @(Get-Content -LiteralPath $record)
    $kept = (($have -join "`n") -eq ($wanted -join "`n"))
}

# Makes -Into afresh: every file downloaded and held to its pin, then unpacked.
function New-ToolsFolder
{
    if (Test-Path -LiteralPath $Into)
    {
        # Only a folder this script made, or began to make, is removed.
        $foreign = @(Get-ChildItem -LiteralPath $Into -Force | Where-Object { @('python', 'winflexbison', 'tools.txt', '.downloads') -notcontains $_.Name })
        if ($foreign.Count)
        {
            Stop-Tools "$Into holds $($foreign[0].Name), which this script did not put there: give -Into a folder of its own"
        }
        Write-Output "Removing $Into to make it again"
        Remove-Item -LiteralPath $Into -Recurse -Force -ErrorAction Stop
    }
    New-Item -ItemType Directory -Force -Path $Into -ErrorAction Stop | Out-Null
    $downloads = Join-Path -Path $Into -ChildPath '.downloads'
    New-Item -ItemType Directory -Force -Path $downloads -ErrorAction Stop | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $previousProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try
    {
        foreach ($pin in $Pins)
        {
            $file = Join-Path -Path $downloads -ChildPath $pin.File
            Write-Output "Downloading $($pin.Name): $($pin.Url)"
            try
            {
                # A user agent that is not a browser's: SourceForge answers a browser
                # with a page that starts the download, and anything else with the file.
                Invoke-WebRequest -Uri $pin.Url -OutFile $file -UseBasicParsing -UserAgent 'hmailserver-build' -ErrorAction Stop
            }
            catch
            {
                Stop-Tools "the download of $($pin.Url) failed: $($_.Exception.Message)"
            }
            $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $pin.Sha256)
            {
                Remove-Item -LiteralPath $file -Force
                Stop-Tools "$($pin.File) does not match its pinned SHA-256 and has been deleted: expected $($pin.Sha256), got $actual"
            }
            Write-Output "  SHA-256 verified: $actual"
        }
    }
    finally
    {
        $ProgressPreference = $previousProgress
    }

    # Python: the package's tools\ folder is the interpreter.
    $unpacked = Join-Path -Path $downloads -ChildPath 'python-package'
    [System.IO.Compression.ZipFile]::ExtractToDirectory((Join-Path -Path $downloads -ChildPath $Pins[0].File), $unpacked)
    Move-Item -LiteralPath (Join-Path -Path $unpacked -ChildPath 'tools') -Destination $pythonDir -ErrorAction Stop
    $python = Join-Path -Path $pythonDir -ChildPath 'python.exe'

    # pip from the wheel the interpreter carries (ensurepip reaches no network), then
    # the two wheels from disk. No user site, no cache, no index.
    $env:PYTHONNOUSERSITE = '1'
    $env:PIP_DISABLE_PIP_VERSION_CHECK = '1'
    & $python -m ensurepip --default-pip 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Output "  $_" }
    if ($LastExitCode -ne 0) { Stop-Tools "python -m ensurepip failed with exit code $LastExitCode" }
    & $python -m pip install --no-index --no-deps --no-cache-dir --no-warn-script-location (Join-Path -Path $downloads -ChildPath $Pins[1].File) (Join-Path -Path $downloads -ChildPath $Pins[2].File) 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Output "  $_" }
    if ($LastExitCode -ne 0) { Stop-Tools "pip install of Meson and Ninja failed with exit code $LastExitCode" }

    # winflexbison: the archive holds the two programs and bison's data\ at its top.
    [System.IO.Compression.ZipFile]::ExtractToDirectory((Join-Path -Path $downloads -ChildPath $Pins[3].File), $flexDir)

    Remove-Item -LiteralPath $downloads -Recurse -Force -ErrorAction Stop
    $absent = @($programs | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($absent.Count) { Stop-Tools "made without $($absent -join ', ')" }
    Set-Content -LiteralPath $record -Value $wanted -Encoding ASCII -ErrorAction Stop
}

if ($kept)
{
    Write-Output "libpq's build tools are in $Into already ($($Pins.Name -join ', '))"
}
else
{
    try
    {
        New-ToolsFolder
    }
    catch
    {
        Stop-Tools "$($_.Exception.Message)"
    }
}

# What each program says it is, from the folder made.
$python = Join-Path -Path $pythonDir -ChildPath 'python.exe'
Write-Output "  $(& $python --version 2>&1)"
Write-Output "  meson $(& (Join-Path -Path $pythonDir -ChildPath 'Scripts\meson.exe') --version 2>&1)"
Write-Output "  ninja $(& (Join-Path -Path $pythonDir -ChildPath 'Scripts\ninja.exe') --version 2>&1)"
Write-Output "  $(& (Join-Path -Path $flexDir -ChildPath 'win_flex.exe') --version 2>&1)"
Write-Output "  $(@(& (Join-Path -Path $flexDir -ChildPath 'win_bison.exe') --version 2>&1)[0])"
Write-Output "LIBPQ TOOLS PATH=$($pathDirs -join ';')"
exit 0
