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
        again, unless its feed lists it as Submitted - still waiting in moderation -
        when it is sent again and takes the waiting package's place.
     5. build/make-package-manifests.ps1 renders the package from those bytes, dated
        by the release itself - the release_date chocolatey-release read from the
        GitLab release's released_at and wrote into verification.json, never the day
        this job runs - and choco pack packs it.
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

# The release's day, YYYY-MM-DD, as verification.json gives it, or $null. Windows
# PowerShell's ConvertFrom-Json leaves it a string; PowerShell 7 may make a date of
# it, and either way the day is the one written.
function Get-ReleaseDay($Value) {
    if ($Value -is [datetime]) { return $Value.ToString('yyyy-MM-dd', [System.Globalization.CultureInfo]::InvariantCulture) }
    $text = [string] $Value
    $day = [datetime]::MinValue
    if ($text -notmatch '^\d{4}-\d{2}-\d{2}$' -or -not [datetime]::TryParseExact($text, 'yyyy-MM-dd',
            [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::None, [ref] $day)) {
        return $null
    }
    return $text
}

# The status the community repository's feed gives a version it holds: Submitted
# while it waits for moderation, Approved once a moderator passed it, and so on;
# 'unknown' where the answer names none.
function Get-HeldStatus([string] $Answer) {
    $found = [regex]::Match($Answer, '<d:PackageStatus>([^<]*)</d:PackageStatus>')
    if ($found.Success -and $found.Groups[1].Value.Trim()) { return $found.Groups[1].Value.Trim() }
    return 'unknown'
}

# The package's files, rendered by build/make-package-manifests.ps1 into
# $OutputDirectory: from the installer itself, held to its Authenticode signature,
# or (the self-test) from a hash alone. Both to the log: in a function anything
# left on the pipeline would be part of what it returns.
function New-PackageFiles([string] $Version, [string] $ReleaseDate, [string] $Id, [string] $OutputDirectory,
        [string] $InstallerPath, [string] $Sha256) {
    $render = @{ Version = $Version; ReleaseDate = $ReleaseDate; ChocolateyId = $Id; OutputDirectory = $OutputDirectory }
    if ($InstallerPath) { $render.InstallerPath = $InstallerPath; $render.RequireSignedInstaller = $true }
    else { $render.Sha256 = $Sha256 }
    & (Join-Path $root 'build\make-package-manifests.ps1') @render | Out-Host
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
    # The package is dated by its release, as GitLab records it, never by today.
    $releaseDate = if ($record.PSObject.Properties['release_date']) { Get-ReleaseDay $record.release_date } else { $null }
    if (-not $releaseDate) {
        Stop-Refused "$Verification gives no release_date: chocolatey-release read no date from the GitLab release v$version, and the package is never dated by the day this job runs."
    }
    $name = "hMailServer-$version-x64.exe"
    $entry = $record.files.$name
    if (-not $entry -or [string]$entry.sha256 -notmatch '^[0-9a-f]{64}$') {
        Stop-Refused "$Verification does not hold $name verified."
    }
    Write-Host "the release: $version, published on $releaseDate (UTC); $name verified as $($entry.identity), SHA-256 $($entry.sha256)"

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

    # A version the repository has, listed or unlisted, is never sent twice - but for
    # one still in moderation, which the feed lists as Submitted whatever its step
    # (as it listed hmailserver-progressiverobot 6.3.7 on 21 October 2026, the day
    # after it was pushed): Chocolatey's moderation asks for a package changed during
    # its review to be pushed again as "the exact same version"
    # (docs.chocolatey.org/en-us/community-repository/moderation), which takes the
    # waiting package's place. Approved, rejected or of any other status, it is not
    # sent again.
    try {
        $held = Invoke-WebRequest -UseBasicParsing -Uri "$feed/Packages(Id='$id',Version='$version')" -Method Get
        if ($held.StatusCode -eq 200) {
            $status = Get-HeldStatus $held.Content
            if ($status -ne 'Submitted') {
                Write-Host "The community repository has $id $version already ($status). Nothing to publish."
                return 0
            }
            Write-Host "The community repository has $id $version waiting in moderation (Submitted): it is sent again, in the waiting package's place."
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
    New-PackageFiles -Version $version -ReleaseDate $releaseDate -Id $id -OutputDirectory $manifests -InstallerPath $installer
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
        $other = '0' * 64
        $files = '"files": {"hMailServer-9.9.9-x64.exe": {"sha256": "' + $other + '", "identity": "fixture"}}'
        $undated = Join-Path $work 'undated.json'
        Set-Content -LiteralPath $undated -Value ('{"version": "9.9.9", ' + $files + '}')
        $answer = Run @{ CHOCO_API_KEY = 'fixture-key' } @('-Verification', $undated, '-OutDir', (Join-Path $work 'u'), '-InstallerFrom', $installer, '-NoPush')
        Expect 'a verification.json with no release_date: refused before anything is downloaded, never dated by today' (
            $answer[0] -eq 1 -and $answer[1] -match 'gives no release_date' -and -not (Test-Path (Join-Path $work 'u')))
        $misdated = Join-Path $work 'misdated.json'
        Set-Content -LiteralPath $misdated -Value ('{"version": "9.9.9", "release_date": "2026-13-45", ' + $files + '}')
        $answer = Run @{ CHOCO_API_KEY = 'fixture-key' } @('-Verification', $misdated, '-OutDir', (Join-Path $work 'v'), '-InstallerFrom', $installer, '-NoPush')
        Expect '  ... and one whose release_date is no day: refused' ($answer[0] -eq 1 -and $answer[1] -match 'gives no release_date')

        $json = Join-Path $work 'verification.json'
        Set-Content -LiteralPath $json -Value ('{"version": "9.9.9", "released_at": "2026-01-02T03:04:05.000Z", "release_date": "2026-01-02", ' + $files + '}')
        $day = Get-ReleaseDay (Get-Content -LiteralPath $json -Raw | ConvertFrom-Json).release_date
        Expect 'the release_date read back from verification.json as the day written' ($day -eq '2026-01-02')
        Expect '  ... and a date PowerShell made of it gives the same day' ((Get-ReleaseDay ([datetime]::new(2026, 1, 2))) -eq '2026-01-02')

        $answer = Run @{ CHOCO_API_KEY = 'fixture-key' } @('-Verification', $json, '-OutDir', (Join-Path $work 'd'), '-InstallerFrom', $installer, '-NoPush')
        Expect 'an installer that is not the bytes chocolatey-release verified: refused before anything is packed' (
            $answer[0] -eq 1 -and $answer[1] -match 'not \d*0{64}, the bytes chocolatey-release verified' -and -not (Test-Path (Join-Path $work 'd\manifests')))
        Expect '  ... and the key is not in what it said' ($answer[1] -notmatch 'fixture-key')

        $answer = Run @{ CHOCO_API_KEY = 'fixture-key'; CI = 'true' } @('-Verification', $json, '-OutDir', (Join-Path $work 'e'), '-InstallerFrom', $installer)
        Expect '-InstallerFrom in a CI job: refused' ($answer[0] -eq 1 -and $answer[1] -match 'outside CI')

        # The files the job packs, rendered as it renders them (New-PackageFiles), from a
        # hash in place of a signed installer: dated by the release, and saying the fork.
        $rendered = Join-Path $work 'f'
        New-PackageFiles -Version '9.9.9' -ReleaseDate $day -Id 'hmailserver-progressiverobot' -OutputDirectory $rendered -Sha256 ('A' * 64)
        $installerYaml = Get-Content -LiteralPath (Join-Path $rendered 'winget\manifests\p\ProgressiveRobot\hMailServer\9.9.9\ProgressiveRobot.hMailServer.installer.yaml') -Raw
        Expect "the installer manifest's ReleaseDate is the release's, 2026-01-02, not today" ($installerYaml -match '(?m)^ReleaseDate: "2026-01-02"\r?$')
        [xml] $nuspec = Get-Content -LiteralPath (Join-Path $rendered 'chocolatey\hmailserver.nuspec') -Raw
        Expect "the nuspec's id is the fork's, and its title is not the original's" (
            $nuspec.package.metadata.id -eq 'hmailserver-progressiverobot' -and $nuspec.package.metadata.title -eq "hMailServer (Progressive Robot's fork)")
        Expect "  ... and its summary says Progressive Robot's fork of hMailServer" ([string] $nuspec.package.metadata.summary -match "^Progressive Robot's fork of hMailServer, not the original")

        # What the feed says of a version it holds: only one waiting in moderation is sent again.
        $entry = '<entry><m:properties><d:Id>hmailserver-progressiverobot</d:Id><d:PackageStatus>{0}</d:PackageStatus></m:properties></entry>'
        Expect "a version the feed lists as Submitted, waiting in moderation, is read as such" ((Get-HeldStatus ($entry -f 'Submitted')) -eq 'Submitted')
        Expect "  ... an approved one as Approved, which is not sent again" ((Get-HeldStatus ($entry -f 'Approved')) -eq 'Approved')
        Expect "  ... and an answer that names no status as unknown, which is not sent again either" ((Get-HeldStatus '<entry></entry>') -eq 'unknown')
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
