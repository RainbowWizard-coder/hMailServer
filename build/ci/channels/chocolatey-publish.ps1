# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The chocolatey-publish job of .gitlab/ci/channels.yml, on bench150: the release's
   Chocolatey package, packed from the release's own signed installer and pushed to
   the community repository (hmailserver/docs/Channels.md, "Chocolatey").

   It runs after chocolatey-release, which on a Linux runner downloaded the release's
   installer and verified it against its Sigstore bundle as this project's pipeline
   for the tag (build/ci/channels/release-files.py verify) - cosign has no Windows
   build outside GitHub - and handed over verification.json. Here:

     1. Dormant: without the project's CI/CD variable CHOCO_API_KEY (the
        community.chocolatey.org account's API key; protected, masked) it says "not
        configured" and exits 0.
     2. The package's id: HM_CHOCOLATEY_ID, hmailserver (upstream's unlisted package,
        once its maintainers add the project's account) or
        hmailserver-progressiverobot; hmailserver when unset.
     3. The installer downloaded from the release's address and held to the SHA-256
        chocolatey-release verified, and to a valid Authenticode signature.
     4. A version the community repository has already, listed or not, is not sent
        again.
     5. build/make-package-manifests.ps1 renders the package from those bytes, and
        choco pack packs it.
     6. The .nupkg is pushed with an HTTP PUT to https://push.chocolatey.org/, the key
        in the request's X-NuGet-ApiKey header - never on a command line, which is why
        this does not call choco push. A new package, and every new version, then
        goes through Chocolatey's moderation before it is listed.

   choco pack needs Chocolatey's CLI on bench150 (https://community.chocolatey.org/
   install.ps1, not GitHub); the job stops, saying so, where it is not.

   Usage, from the root of a checkout (Windows PowerShell 5.1 or PowerShell 7):
     build\ci\channels\chocolatey-publish.ps1 [-Verification chocolatey-release\verification.json] [-OutDir chocolatey-publish]
     build\ci\channels\chocolatey-publish.ps1 -SelfTest

   -InstallerFrom (outside CI only) takes the installer from a local file instead of
   the release's address, for a run against a fixture; -NoPush stops after the pack.
#>

[CmdletBinding()]
param(
    [string] $Verification = 'chocolatey-release\verification.json',
    [string] $OutDir = 'chocolatey-publish',
    [string] $InstallerFrom,
    [switch] $NoPush,
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Windows PowerShell 5.1 draws Invoke-WebRequest's progress for every block it reads,
# which makes the 80 MB installer's download many times slower.
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$registry = 'https://gitlab.com/api/v4/projects/86730559/packages/generic/hmailserver'
$feed = 'https://community.chocolatey.org/api/v2'
$push = 'https://push.chocolatey.org/api/v2/package/'
$allowedIds = @('hmailserver', 'hmailserver-progressiverobot')

function Stop-Refused([string] $Why) {
    Write-Host "REFUSED: $Why"
    exit 1
}

function Test-InCi {
    return [bool]($env:CI -or $env:GITLAB_CI)
}

function Get-FileSha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-Publish {
    if (-not $env:CHOCO_API_KEY) {
        Write-Host 'not configured: CHOCO_API_KEY is not set, so nothing was published.'
        Write-Host 'The owner makes the community.chocolatey.org account and chooses the id; a maintainer stores the'
        Write-Host "account's API key as the protected, masked CI/CD variable CHOCO_API_KEY"
        Write-Host "(hmailserver/docs/Channels.md, 'Chocolatey')."
        return 0
    }
    if ($InstallerFrom -and (Test-InCi)) {
        Stop-Refused '-InstallerFrom is for a run outside CI: the job takes the release''s own installer.'
    }
    $id = if ($env:HM_CHOCOLATEY_ID) { $env:HM_CHOCOLATEY_ID.Trim() } else { 'hmailserver' }
    if ($allowedIds -notcontains $id) {
        Stop-Refused "HM_CHOCOLATEY_ID is '$id': the package is hmailserver or hmailserver-progressiverobot."
    }

    if (-not (Test-Path -LiteralPath $Verification)) {
        Stop-Refused "$Verification is not here: chocolatey-release did not verify the release's installer."
    }
    $record = Get-Content -LiteralPath $Verification -Raw | ConvertFrom-Json
    $version = [string] $record.version
    if ($version -notmatch '^\d+\.\d+\.\d+$') { Stop-Refused "$Verification names the version '$version'." }
    $name = "hMailServer-$version-x64.exe"
    $entry = $record.files.$name
    if (-not $entry -or [string]$entry.sha256 -notmatch '^[0-9a-f]{64}$') {
        Stop-Refused "$Verification does not hold $name verified."
    }
    Write-Host "the release: $version; $name verified as $($entry.identity), SHA-256 $($entry.sha256)"

    if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $installer = Join-Path (Resolve-Path $OutDir) $name
    if ($InstallerFrom) {
        Copy-Item -LiteralPath $InstallerFrom -Destination $installer
    }
    else {
        Invoke-WebRequest -UseBasicParsing -Uri "$registry/$version/$name" -OutFile $installer
    }
    $sha = Get-FileSha256 $installer
    if ($sha -ne [string] $entry.sha256) {
        Stop-Refused "$name downloaded here is SHA-256 $sha, not $($entry.sha256), the bytes chocolatey-release verified. Nothing was packed."
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid') {
        Stop-Refused "$name's Authenticode signature is $($signature.Status), not Valid."
    }
    Write-Host "Authenticode: Valid, $($signature.SignerCertificate.Subject)"

    # A version the repository has, listed or unlisted, is never sent twice.
    try {
        $held = Invoke-WebRequest -UseBasicParsing -Uri "$feed/Packages(Id='$id',Version='$version')" -Method Get
        if ($held.StatusCode -eq 200) {
            Write-Host "The community repository has $id $version already. Nothing to publish."
            return 0
        }
    }
    catch {
        $status = $null
        if ($_.Exception.PSObject.Properties['Response'] -and $_.Exception.Response) {
            $status = [int] $_.Exception.Response.StatusCode
        }
        if ($status -ne 404) { Stop-Refused "the community repository answered $status for $id $version." }
    }

    if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
        Stop-Refused 'there is no choco on this machine: install Chocolatey''s CLI on bench150 (https://community.chocolatey.org/install.ps1).'
    }

    # Both to the log: in this function anything left on the pipeline would be part of
    # what it returns, and exit takes that, not the log.
    $manifests = Join-Path $OutDir 'manifests'
    & (Join-Path $root 'build\make-package-manifests.ps1') -Version $version -InstallerPath $installer `
        -RequireSignedInstaller -ChocolateyId $id -OutputDirectory $manifests | Out-Host
    $nuspec = Join-Path $manifests 'chocolatey\hmailserver.nuspec'
    & choco pack $nuspec --outputdirectory $OutDir --limit-output | Out-Host
    if ($LASTEXITCODE -ne 0) { Stop-Refused "choco pack failed ($LASTEXITCODE)." }
    $package = Join-Path $OutDir "$id.$version.nupkg"
    if (-not (Test-Path -LiteralPath $package)) { Stop-Refused "choco pack made no $id.$version.nupkg." }
    Write-Host "packed: $package, SHA-256 $(Get-FileSha256 $package)"
    if ($NoPush) {
        Write-Host 'NOT PUSHED (-NoPush).'
        return 0
    }

    # NuGet's v2 push: the package as a multipart form, the key in a header.
    $bytes = [System.IO.File]::ReadAllBytes($package)
    $boundary = [System.Guid]::NewGuid().ToString('N')
    $head = [System.Text.Encoding]::ASCII.GetBytes("--$boundary`r`nContent-Type: application/octet-stream`r`nContent-Disposition: form-data; name=`"package`"; filename=`"package.nupkg`"`r`n`r`n")
    $tail = [System.Text.Encoding]::ASCII.GetBytes("`r`n--$boundary--`r`n")
    $body = New-Object byte[] ($head.Length + $bytes.Length + $tail.Length)
    [System.Buffer]::BlockCopy($head, 0, $body, 0, $head.Length)
    [System.Buffer]::BlockCopy($bytes, 0, $body, $head.Length, $bytes.Length)
    [System.Buffer]::BlockCopy($tail, 0, $body, $head.Length + $bytes.Length, $tail.Length)
    try {
        $answer = Invoke-WebRequest -UseBasicParsing -Uri $push -Method Put -Body $body `
            -ContentType "multipart/form-data; boundary=$boundary" -Headers @{ 'X-NuGet-ApiKey' = $env:CHOCO_API_KEY }
    }
    catch {
        $status = if ($_.Exception.PSObject.Properties['Response'] -and $_.Exception.Response) { [int] $_.Exception.Response.StatusCode } else { 'no answer' }
        Stop-Refused "the community repository refused the push ($status): $($_.Exception.Message)"
    }
    Write-Host "PUBLISHED: $id $version pushed (HTTP $($answer.StatusCode)); it is listed once Chocolatey's moderation approves it: https://community.chocolatey.org/packages/$id/$version"
    return 0
}

function Invoke-SelfTest {
    $failures = New-Object System.Collections.ArrayList
    function Expect([string] $What, [bool] $Condition) {
        Write-Host ("{0}  {1}" -f $(if ($Condition) { 'ok  ' } else { 'FAIL' }), $What)
        if (-not $Condition) { [void] $failures.Add($What) }
    }
    $shell = (Get-Process -Id $PID).Path
    $self = $PSCommandPath
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ("hm-choco-test-" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    $saved = @{ CHOCO_API_KEY = $env:CHOCO_API_KEY; HM_CHOCOLATEY_ID = $env:HM_CHOCOLATEY_ID; CI = $env:CI; GITLAB_CI = $env:GITLAB_CI }
    try {
        function Run([hashtable] $Environment, [string[]] $Arguments) {
            foreach ($k in @('CHOCO_API_KEY', 'HM_CHOCOLATEY_ID', 'CI', 'GITLAB_CI')) {
                Set-Item -Path "env:$k" -Value $(if ($Environment.ContainsKey($k)) { $Environment[$k] } else { $null })
            }
            $said = & $shell -NoProfile -ExecutionPolicy Bypass -File $self @Arguments 2>&1 | Out-String
            return @($LASTEXITCODE, $said)
        }
        $answer = Run @{} @('-Verification', (Join-Path $work 'none.json'), '-OutDir', (Join-Path $work 'a'))
        Expect "without CHOCO_API_KEY: 'not configured', exit 0, before anything is read" ($answer[0] -eq 0 -and $answer[1] -match 'not configured')

        $answer = Run @{ CHOCO_API_KEY = 'fixture-key'; HM_CHOCOLATEY_ID = 'someone-else' } @('-OutDir', (Join-Path $work 'b'))
        Expect 'an id other than the two refused' ($answer[0] -eq 1 -and $answer[1] -match "HM_CHOCOLATEY_ID is 'someone-else'")

        $answer = Run @{ CHOCO_API_KEY = 'fixture-key' } @('-Verification', (Join-Path $work 'none.json'), '-OutDir', (Join-Path $work 'c'))
        Expect 'no verification from chocolatey-release: refused' ($answer[0] -eq 1 -and $answer[1] -match 'did not verify')

        $installer = Join-Path $work 'hMailServer-9.9.9-x64.exe'
        [System.IO.File]::WriteAllBytes($installer, [byte[]] (1, 2, 3))
        $json = Join-Path $work 'verification.json'
        $other = '0' * 64
        Set-Content -LiteralPath $json -Value ('{"version": "9.9.9", "files": {"hMailServer-9.9.9-x64.exe": {"sha256": "' + $other + '", "identity": "fixture"}}}')
        $answer = Run @{ CHOCO_API_KEY = 'fixture-key' } @('-Verification', $json, '-OutDir', (Join-Path $work 'd'), '-InstallerFrom', $installer, '-NoPush')
        Expect 'an installer that is not the bytes chocolatey-release verified: refused before anything is packed' (
            $answer[0] -eq 1 -and $answer[1] -match 'not \d*0{64}, the bytes chocolatey-release verified' -and -not (Test-Path (Join-Path $work 'd\manifests')))
        Expect '  ... and the key is not in what it said' ($answer[1] -notmatch 'fixture-key')

        $answer = Run @{ CHOCO_API_KEY = 'fixture-key'; CI = 'true' } @('-Verification', $json, '-OutDir', (Join-Path $work 'e'), '-InstallerFrom', $installer)
        Expect '-InstallerFrom in a CI job: refused' ($answer[0] -eq 1 -and $answer[1] -match 'outside CI')
    }
    finally {
        foreach ($k in $saved.Keys) { Set-Item -Path "env:$k" -Value $saved[$k] }
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host ''
    if ($failures.Count -gt 0) {
        Write-Host "$($failures.Count) self-test case(s) failed"
        return 1
    }
    Write-Host 'Every self-test case passed.'
    return 0
}

if ($SelfTest) { exit (Invoke-SelfTest) }
exit (Invoke-Publish)
