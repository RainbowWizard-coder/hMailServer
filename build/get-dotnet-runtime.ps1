# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
   Puts the .NET Desktop Runtime installer that the hMailServer installer bundles
   where the installer script expects it: exactly the version pinned in
   hmailserver/docs/third-party-binaries.json, checked against the SHA-512
   Microsoft publishes for it, and nothing else.

.DESCRIPTION
   The installer carries the runtime because the Control Panel and the database
   tools are .NET 10 applications, and DBSetup, DBSetupQuick and DBUpdater run
   during installation. The file is not in git (60 MB, and .gitignore excludes
   the DotNet folder); run this once before building the installer.

   Until 25 September 2026 this script downloaded whatever
   https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe pointed at on
   the day and checked only that the file was larger than 40 MB. So the runtime
   an installer carried depended on the day it was built, and nothing could tell
   a genuine file from any other of the same size. Now the manifest entry for
   installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe pins one version,
   the address Microsoft publishes it at, and the SHA-512 Microsoft's release
   metadata gives for it. A download is checked before it is put in place, and a
   file already there is checked too: one that is not the pinned runtime - an
   older download, a newer one, anything else - is removed and the pinned one
   downloaded, so that if the download then fails or does not match there is no
   runtime for the installer to package rather than the wrong one.

   Moving the pin is a deliberate step: build/show-dotnet-runtime-pin.ps1 prints
   the fields for a chosen version from Microsoft's metadata, they are copied into
   the manifest entry, and hmailserver/docs/ThirdPartyBinaries.md ("Moving the
   .NET runtime pin") has the rest. The file keeps the channel's name
   (windowsdesktop-runtime-10.0-win-x64.exe), because that is the name the
   installer builds from DOTNET_CHANNEL in hMailServer64.iss; keep the channel in
   step with three other places, or the installer will either ship the wrong
   runtime or decide it does not need to install one:
     * <TargetFramework> in the .csproj files under source/Tools
     * global.json (the SDK pin)
     * DOTNET_MAJOR and DOTNET_CHANNEL in hMailServer64.iss, which
       DotNetDesktopMissing() in hMailServerInnoExtension.iss and the file
       references in section_files_64.iss and section_run.iss are built from

.PARAMETER Verify
   Only check that the file in place is the pinned runtime; exit 1 otherwise.
   Downloads nothing.

.PARAMETER Path
   The file to place or check. Default: the manifest entry's path under the
   repository.

.PARAMETER Manifest
   The manifest the pin is read from. Default:
   hmailserver/docs/third-party-binaries.json. build/test-get-dotnet-runtime.ps1
   passes one of its own.
#>
[CmdletBinding()]
param(
   [switch] $Verify,
   [string] $Path,
   [string] $Manifest
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Manifest) {
   $Manifest = Join-Path (Join-Path (Join-Path $repo 'hmailserver') 'docs') 'third-party-binaries.json'
}

# The one manifest entry this script places, by its path. build/check-sbom-inputs.py
# reads this line to find the pin, and holds the SBOM to naming it.
$pinnedPath = 'hmailserver/installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe'

function Get-Sha512([string] $file) {
   (Get-FileHash -LiteralPath $file -Algorithm SHA512).Hash.ToLowerInvariant()
}

# The pin, read from the manifest and refused unless it is a pin: one version, a
# 128-digit SHA-512, and an address that names that version. The last is what
# refuses a moving link such as aka.ms/dotnet/10.0/..., whose target changes with
# every Microsoft release; the hash would refuse it too, but only on the day the
# link moved, and in the middle of somebody's release.
function Get-RuntimePin([string] $manifestFile) {
   if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) { throw "The manifest is not there: $manifestFile" }
   $entries = @((Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json).artifacts | Where-Object { $_.path -eq $pinnedPath })
   if ($entries.Count -ne 1) { throw "$manifestFile has $($entries.Count) entries for $pinnedPath; it must have exactly one, which pins the runtime this script places." }
   $entry = $entries[0]

   $version = [string] $entry.version
   $sha512 = ([string] $entry.sha512).ToLowerInvariant()
   $source = [string] $entry.source
   $channel = [regex]::Match($pinnedPath, 'windowsdesktop-runtime-(\d+\.\d+)-').Groups[1].Value

   $problems = @()
   if ($entry.disposition -ne 'downloaded') { $problems += "its disposition is '$($entry.disposition)', not 'downloaded'" }
   if ($version -notmatch '^\d+\.\d+\.\d+$') { $problems += "its version '$version' is not one release (major.minor.patch)" }
   elseif (-not $version.StartsWith($channel + '.')) { $problems += "its version $version is not in the $channel channel the file is named for and the installer probes" }
   if ($sha512 -notmatch '^[0-9a-f]{128}$') { $problems += "its sha512 '$sha512' is not a SHA-512 (128 hexadecimal digits)" }
   if ($source -notmatch '^https?://') { $problems += "its source '$source' is not an http or https address" }
   elseif ($version -and -not $source.EndsWith("/windowsdesktop-runtime-$version-win-x64.exe")) {
      $problems += "its source '$source' does not name windowsdesktop-runtime-$version-win-x64.exe, so it is not an address for that one version (a moving link such as aka.ms is not a pin)"
   }
   if ($problems.Count) {
      throw ("The .NET Desktop Runtime pin in {0} is not a pin: {1}. build/show-dotnet-runtime-pin.ps1 prints the fields for a version." -f $manifestFile, ($problems -join '; '))
   }

   return [pscustomobject]@{ Version = $version; Sha512 = $sha512; Source = $source }
}

$pin = Get-RuntimePin $Manifest

if (-not $Path) {
   $Path = $repo
   foreach ($segment in ($pinnedPath -split '/')) { $Path = Join-Path $Path $segment }
}

if ($Verify) {
   if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
      Write-Host "  BAD $Path is not there. Run build\get-dotnet-runtime.ps1 to download the .NET Desktop Runtime $($pin.Version)."
      exit 1
   }
   $actual = Get-Sha512 $Path
   if ($actual -ne $pin.Sha512) {
      Write-Host "  BAD $Path is not the pinned .NET Desktop Runtime $($pin.Version).`n      expected sha512 $($pin.Sha512)`n      actual   sha512 $actual`n      Run build\get-dotnet-runtime.ps1, which replaces it with the pinned file."
      exit 1
   }
   Write-Host "  OK  $Path is the .NET Desktop Runtime $($pin.Version) (sha512 matches the pin)."
   exit 0
}

if (Test-Path -LiteralPath $Path -PathType Leaf) {
   $actual = Get-Sha512 $Path
   if ($actual -eq $pin.Sha512) {
      Write-Host "already present: $Path (.NET Desktop Runtime $($pin.Version), sha512 matches the pin)"
      return
   }
   # Removed before the download rather than after it: left in place, a stale file
   # is what the installer script would package if this run then failed and the
   # failure went unread. With nothing there, ISCC stops at the missing file.
   Write-Host "$Path is not the pinned .NET Desktop Runtime $($pin.Version) (its sha512 is $actual); removing it and downloading the pinned one."
   Remove-Item -LiteralPath $Path -Force
}

$directory = Split-Path -Parent $Path
New-Item -ItemType Directory -Force $directory | Out-Null
$partial = "$Path.download"
if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }

Write-Host "downloading the .NET Desktop Runtime $($pin.Version) (x64) from $($pin.Source)"
# Windows PowerShell 5.1 redraws a progress bar for every chunk it receives,
# which turns a 60 MB download from seconds into minutes. Off for the download.
$previousProgress = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'
try {
   Invoke-WebRequest -Uri $pin.Source -OutFile $partial -UseBasicParsing
}
catch {
   if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
   throw "The download of $($pin.Source) failed: $($_.Exception.Message)"
}
finally {
   $ProgressPreference = $previousProgress
}

$actual = Get-Sha512 $partial
if ($actual -ne $pin.Sha512) {
   Remove-Item -LiteralPath $partial -Force
   throw ("The download of {0} does not match the pinned SHA-512 and has been deleted; nothing was put in place.`n  expected {1}`n  actual   {2}`nIf Microsoft has re-issued the file, take the new hash from its release metadata with build/show-dotnet-runtime-pin.ps1 and move the pin deliberately." -f $pin.Source, $pin.Sha512, $actual)
}

Move-Item -LiteralPath $partial -Destination $Path -Force
Write-Host ("downloaded {0} ({1:N1} MB): the .NET Desktop Runtime {2}, sha512 matches the pin" -f $Path, ((Get-Item -LiteralPath $Path).Length / 1MB), $pin.Version)
