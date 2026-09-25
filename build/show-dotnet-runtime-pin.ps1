# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
   Prints the pin for one release of the .NET Desktop Runtime the installer
   bundles - its version, the address Microsoft publishes it at and its SHA-512 -
   read from Microsoft's own release metadata, to be copied into
   hmailserver/docs/third-party-binaries.json by hand.

.DESCRIPTION
   Moving the pin is meant to be a deliberate, reviewed step, so this changes
   nothing: it reads, compares and prints. The metadata is the releases.json of
   the channel, fetched from both of the hosts Microsoft serves it from,
   builds.dotnet.microsoft.com and dotnetcli.blob.core.windows.net. When both
   answer they must agree about the file, or nothing is printed; when one does
   not answer the other is used, and the script says so.

   With no -Version it shows the channel's latest release. It also says whether
   a release is a security release and which CVEs it names, which is what
   decides when the pin should move, and what the manifest pins now.

   hmailserver/docs/ThirdPartyBinaries.md, "Moving the .NET runtime pin", has
   the whole step.

.PARAMETER Version
   The release to show, such as 10.0.13. Default: the channel's latest.

.PARAMETER Channel
   The channel to read. Default: the channel of the version given, or 10.0.
#>
[CmdletBinding()]
param(
   [string] $Version,
   [string] $Channel
)

$ErrorActionPreference = 'Stop'

if (-not $Channel) {
   $Channel = if ($Version -match '^(\d+\.\d+)\.') { $Matches[1] } else { '10.0' }
}
if ($Channel -notmatch '^\d+\.\d+$') { throw "'$Channel' is not a channel such as 10.0." }
if ($Version -and -not $Version.StartsWith($Channel + '.')) { throw "$Version is not a release of the $Channel channel." }

$hosts = @(
   "https://builds.dotnet.microsoft.com/dotnet/release-metadata/$Channel/releases.json",
   "https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/$Channel/releases.json"
)
$wanted = 'windowsdesktop-runtime-win-x64.exe'

$answers = @()
foreach ($url in $hosts) {
   try {
      $metadata = Invoke-RestMethod -Uri $url -UseBasicParsing
      $answers += [pscustomobject]@{ Url = $url; Metadata = $metadata }
   }
   catch {
      Write-Warning "$url did not answer: $($_.Exception.Message)"
   }
}
if ($answers.Count -eq 0) { throw "Neither host answered, so there is nothing to read the pin from." }

$found = @()
foreach ($answer in $answers) {
   $releaseVersion = if ($Version) { $Version } else { [string] $answer.Metadata.'latest-release' }
   $release = @($answer.Metadata.releases | Where-Object { $_.'release-version' -eq $releaseVersion }) | Select-Object -First 1
   if (-not $release) { throw "$($answer.Url) has no release $releaseVersion in the $Channel channel." }
   $file = @($release.windowsdesktop.files | Where-Object { $_.name -eq $wanted }) | Select-Object -First 1
   if (-not $file) { throw "$($answer.Url) lists no $wanted for $releaseVersion." }
   $found += [pscustomobject]@{
      Host     = $answer.Url
      Version  = [string] $release.windowsdesktop.version
      Date     = [string] $release.'release-date'
      Security = [bool] $release.security
      Cves     = @($release.'cve-list' | ForEach-Object { $_.'cve-id' })
      Url      = [string] $file.url
      Sha512   = ([string] $file.hash).ToLowerInvariant()
   }
}

$first = $found[0]
foreach ($other in $found | Select-Object -Skip 1) {
   if ($other.Version -ne $first.Version -or $other.Url -ne $first.Url -or $other.Sha512 -ne $first.Sha512) {
      throw ("The two hosts disagree about the {0} file of {1}:`n  {2}`n    {3} {4}`n  {5}`n    {6} {7}`nNothing is printed; read them again later." -f $wanted, $first.Version, $first.Host, $first.Url, $first.Sha512, $other.Host, $other.Url, $other.Sha512)
   }
}
if ($first.Sha512 -notmatch '^[0-9a-f]{128}$') { throw "The hash published for $($first.Version) is not a SHA-512: $($first.Sha512)" }
if (-not $first.Url.EndsWith("/windowsdesktop-runtime-$($first.Version)-win-x64.exe")) { throw "The address published for $($first.Version) does not name that version: $($first.Url)" }

$current = $null
$manifest = Join-Path (Join-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'hmailserver') 'docs') 'third-party-binaries.json'
if (Test-Path -LiteralPath $manifest) {
   $current = @((Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).artifacts |
      Where-Object { $_.path -eq 'hmailserver/installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe' }) | Select-Object -First 1
}

Write-Host ("The .NET Desktop Runtime {0} (x64 installer), released {1}{2}" -f $first.Version, $first.Date, $(if ($first.Security) { ', a security release' } else { '' }))
if ($first.Cves.Count) { Write-Host ("  CVEs it names: {0}" -f ($first.Cves -join ', ')) }
Write-Host ("  read from: {0}" -f (($found | ForEach-Object { $_.Host }) -join ' and '))
if ($current) {
   if ($current.version -eq $first.Version -and ([string] $current.sha512).ToLowerInvariant() -eq $first.Sha512) {
      Write-Host "  The manifest pins this release already."
   }
   else {
      Write-Host ("  The manifest pins {0} now." -f $current.version)
   }
}
Write-Host ''
Write-Host 'Its fields in hmailserver/docs/third-party-binaries.json (the entry for'
Write-Host 'hmailserver/installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe):'
Write-Host ''
Write-Host ('      "sha512": "{0}",' -f $first.Sha512)
Write-Host ('      "version": "{0}",' -f $first.Version)
Write-Host ('      "source": "{0}"' -f $first.Url)
Write-Host ''
Write-Host "and the release named in its 'upstream' sentence. Then run build\get-dotnet-runtime.ps1 and"
Write-Host 'python build\check-sbom-inputs.py, and commit the entry by itself.'
