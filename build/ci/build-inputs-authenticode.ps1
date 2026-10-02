# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The build-inputs-authenticode job (.gitlab/ci/packages-windows.yml), on bench150:
   the Authenticode record of every third-party file the Windows installers carry
   from a build input, checked on Windows - the one kind of machine that can - before
   any build of the pipeline uses them. windows-packages-x64, windows-packages-arm64
   and windows-installer need this job.

   hmailserver/docs/third-party-binaries.json pins each build input by its SHA-256
   (build_inputs: the release build-inputs-<n>, whose files are this project's
   generic package build-inputs/<n> on GitLab, or a publisher's https directory) and
   records, for each file a build places from one (an artifact whose disposition is
   "fetched": the input itself, or a member of it, "<release>/<asset>#<member>"), its
   own SHA-256 and the Authenticode status it had when it was recorded, "Valid" with
   its signer's subject, or "NotSigned". build/get-installer-binaries.ps1 holds every
   download to the hashes and places the files; this holds the hashes to what the
   files are:

     1. every build input is downloaded afresh into an empty folder - from the
        package registry, or from the publisher's address with no redirect followed,
        as get-installer-binaries.ps1 downloads it - and must have its pinned
        SHA-256;
     2. every fetched artifact, for x64 and ARM64 alike, is taken from it - the input
        itself, or the member, extracted as get-installer-binaries.ps1 extracts it
        (Expand-Archive for a .zip, Windows' tar.exe for a .7z) - and must have its
        own pinned SHA-256;
     3. a file recorded "Valid" must have a Valid signature, by the signer recorded,
        exactly, with an RFC 3161 timestamp (its certificate has long expired: a
        signature without one would not be Valid either); a file recorded
        "NotSigned" must carry no signature. Any other record - none, "NotRecorded",
        or either word spelt otherwise - is refused: a file this job fetches and
        cannot hold to a record would pass unchecked.

   A manifest that records no Valid file would make this check nothing, and is
   refused. Nothing is placed in the tree and no cache is read or written: what is
   checked is what the registry and the publisher serve now.

   -From <dir> takes each build input from that folder, by its asset name, in place
   of downloading it - for build\ci\authenticode-selftest.ps1, which downloads them
   once for all its cases; the job never passes it. Every hash is checked all the
   same.

   Exit: 0 every record holds; 1 a problem, each named.

   Usage:
     pwsh -NoProfile -File build\ci\build-inputs-authenticode.ps1 [-Manifest <json>] [-WorkDir <empty dir>]
#>
param(
   [string] $Manifest,
   [string] $WorkDir,
   [string] $From
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).ProviderPath
if (-not $Manifest) { $Manifest = Join-Path $repo 'hmailserver\docs\third-party-binaries.json' }
# The registry get-installer-binaries.ps1 downloads a build-inputs-<n> release's files from.
$registryBase = 'https://gitlab.com/api/v4/projects/86730559/packages/generic'
if (-not $WorkDir) { $WorkDir = Join-Path ([IO.Path]::GetTempPath()) ('hm-build-inputs-' + [Guid]::NewGuid().ToString('N')) }
if ((Test-Path -LiteralPath $WorkDir) -and @(Get-ChildItem -LiteralPath $WorkDir -Force).Count) {
   throw "$WorkDir is not empty: the build inputs are checked as downloaded now, never from what a folder holds already"
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

function Get-Sha256([string] $path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower() }

$problems = New-Object System.Collections.Generic.List[string]
$data = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
"manifest: $Manifest"

try {
   # 1. Every build input, afresh.
   $inputs = @{}
   foreach ($buildInput in $data.build_inputs) {
      $release = [string]$buildInput.release
      $asset = [string]$buildInput.asset
      $key = "$release/$asset"
      $local = Join-Path $WorkDir $asset
      try {
         if ($From) {
            Copy-Item -LiteralPath (Join-Path $From $asset) -Destination $local
         } elseif ($release -match '^https://') {
            Invoke-WebRequest -Uri "$release/$asset" -OutFile $local -UseBasicParsing -MaximumRedirection 0
         } elseif ($release -match '^build-inputs-(\d+)$') {
            Invoke-WebRequest -Uri "$registryBase/build-inputs/$($Matches[1])/$asset" -OutFile $local -UseBasicParsing
         } else {
            $problems.Add("$key - the release '$release' is neither a build-inputs-<n> release nor an https address"); continue
         }
      } catch {
         $problems.Add("$key could not be downloaded: $($_.Exception.Message)"); continue
      }
      $actual = Get-Sha256 $local
      if ($actual -ne ([string]$buildInput.sha256).ToLower()) {
         $problems.Add("$key has the SHA-256 $actual, and the manifest pins $($buildInput.sha256)"); continue
      }
      "input    $key  sha256 $actual"
      $inputs[$key] = $local
   }

   # 2 and 3. Every fetched artifact, by its record.
   $checked = @{ Valid = 0; NotSigned = 0; Other = 0 }
   foreach ($artifact in @($data.artifacts | Where-Object { $_.disposition -eq 'fetched' })) {
      $parts = ([string]$artifact.source) -split '#', 2
      $key = $parts[0]
      $member = if ($parts.Count -gt 1) { $parts[1] } else { $null }
      if (-not $inputs.ContainsKey($key)) { $problems.Add("$($artifact.path) - its build input $key was not downloaded and checked"); continue }
      $file = $inputs[$key]
      if ($member) {
         $extract = $inputs[$key] + '.extracted'
         if (-not (Test-Path -LiteralPath $extract)) {
            New-Item -ItemType Directory -Path $extract | Out-Null
            if ($key -like '*.7z') {
               & (Join-Path $env:SystemRoot 'System32\tar.exe') -x -f $inputs[$key] -C $extract
               if ($LASTEXITCODE -ne 0) { $problems.Add("$key could not be extracted with tar.exe (exit $LASTEXITCODE)"); continue }
            } else {
               Expand-Archive -LiteralPath $inputs[$key] -DestinationPath $extract -Force
            }
         }
         $file = Join-Path $extract $member
         if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $problems.Add("$($artifact.path) - $key has no member $member"); continue }
      }
      $actual = Get-Sha256 $file
      if ($actual -ne ([string]$artifact.sha256).ToLower()) {
         $problems.Add("$($artifact.path) - $($artifact.source) has the SHA-256 $actual, and the manifest pins $($artifact.sha256)"); continue
      }
      $recorded = [string]$artifact.authenticode
      $signature = Get-AuthenticodeSignature -LiteralPath $file
      switch -CaseSensitive ($recorded) {
         'Valid' {
            $checked.Valid++
            $want = [string]$artifact.authenticode_signer
            if ($signature.Status -ne 'Valid') {
               $problems.Add("$($artifact.path) - $($artifact.source): signature $($signature.Status), and the manifest records Valid")
            } elseif ($signature.SignerCertificate.Subject -cne $want) {
               $problems.Add("$($artifact.path) - $($artifact.source): signed by '$($signature.SignerCertificate.Subject)', and the manifest records '$want'")
            } elseif ($null -eq $signature.TimeStamperCertificate) {
               $problems.Add("$($artifact.path) - $($artifact.source): Valid, by '$want', without a timestamp")
            } else {
               "Valid    $($artifact.source)  $want, timestamped by $($signature.TimeStamperCertificate.Subject)"
            }
         }
         'NotSigned' {
            $checked.NotSigned++
            if ($signature.Status -ne 'NotSigned') {
               $problems.Add("$($artifact.path) - $($artifact.source): signature $($signature.Status) by '$($signature.SignerCertificate.Subject)', and the manifest records NotSigned")
            } else {
               "unsigned $($artifact.source)"
            }
         }
         default {
            $checked.Other++
            $problems.Add("$($artifact.path) - $($artifact.source): the manifest records its signature as '$recorded', neither Valid nor NotSigned, so it cannot be checked (it is $($signature.Status))")
         }
      }
   }
   if ($checked.Valid -eq 0) { $problems.Add("the manifest records no fetched file as Valid: this job would check no signature") }
   ""
   "{0} fetched file(s) recorded Valid, {1} NotSigned, {2} with no such record (refused)." -f $checked.Valid, $checked.NotSigned, $checked.Other
} finally {
   Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($problems.Count) {
   foreach ($problem in $problems) { "PROBLEM: $problem" }
   "build-inputs-authenticode: $($problems.Count) problem(s); no build of this pipeline uses these inputs."
   exit 1
}
"build-inputs-authenticode: every build input has its pinned SHA-256, and every fetched file the signature the manifest records."
exit 0
