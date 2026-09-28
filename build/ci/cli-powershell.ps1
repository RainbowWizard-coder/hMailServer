# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    The HmailServer PowerShell module as the PowerShell Gallery will take it: its
    manifest passes Test-ModuleManifest, the module imports through it with the
    functions the manifest lists, and PSScriptAnalyzer finds nothing in either file.

.DESCRIPTION
    The cli-powershell job of .gitlab/ci/cli-packages.yml runs this under pwsh; it runs
    as well under Windows PowerShell 5.1 (hmailserver/docs/CommandLinePackages.md).

    PSScriptAnalyzer comes from the PowerShell Gallery - never from GitHub - at the
    version pinned below, and the package is held to the SHA-512 the Gallery
    published for it (its PackageHash) before anything in it is loaded: the Gallery
    is where the module itself will be published, and a pinned hash is what makes the
    analyzer the one that was reviewed. To move to a newer PSScriptAnalyzer, read
    PackageHash from
    https://www.powershellgallery.com/api/v2/Packages(Id='PSScriptAnalyzer',Version='<v>')
    and change both lines.

    Every rule PSScriptAnalyzer runs by default is run, with no settings file: a
    finding is fixed in the module or justified where it stands, with a
    SuppressMessageAttribute on the one function and its reason.

.PARAMETER ModuleDirectory
    The folder holding HmailServer.psd1 and HmailServer.psm1.

.PARAMETER WorkDirectory
    Where the analyzer is downloaded and unpacked.
#>
param(
    [string] $ModuleDirectory = (Join-Path $PSScriptRoot '../../hmailserver/source/Tools/Cli'),
    [string] $WorkDirectory = (Join-Path ([System.IO.Path]::GetTempPath()) 'hm-cli-powershell')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$AnalyzerVersion = '1.25.0'
$AnalyzerSha512 = '5/tXMmLmBLqymRSuYpIdJLl8L4i1GbQSd7QD72zhY/FLfRs23HNEmmjlrlqNlQKJGxDCZBMf0YE63ym/ExwWYw=='

$failed = 0
$ModuleDirectory = (Resolve-Path $ModuleDirectory).Path
$manifestPath = Join-Path $ModuleDirectory 'HmailServer.psd1'
"PowerShell $($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition)) on $([System.Environment]::OSVersion.VersionString)"

# ------------------------------------------------------------ the manifest
$manifest = Test-ModuleManifest -Path $manifestPath
"Test-ModuleManifest: $($manifest.Name) $($manifest.Version), $($manifest.ExportedFunctions.Count) functions, " +
    "editions $($manifest.CompatiblePSEditions -join ' and '), PowerShell $($manifest.PowerShellVersion) or later"

Import-Module $manifestPath -Force
$module = Get-Module HmailServer
$listed = @($manifest.ExportedFunctions.Keys | Sort-Object)
$loaded = @($module.ExportedFunctions.Keys | Sort-Object)
if (Compare-Object $listed $loaded) {
    "FAILED: the module exports other functions than its manifest lists:"
    Compare-Object $listed $loaded | Format-Table -AutoSize | Out-String
    $failed++
}
else {
    "Import-Module: $($module.Name) $($module.Version), the $($loaded.Count) functions the manifest lists"
}
if (@($module.ExportedCmdlets.Keys).Count -or @($module.ExportedAliases.Keys).Count -or @($module.ExportedVariables.Keys).Count) {
    "FAILED: the module exports cmdlets, aliases or variables, which its manifest says it does not"
    $failed++
}
Remove-Module HmailServer

# ------------------------------------------------------------ the publisher's cmdlet
# psgallery-publish runs Publish-PSResource under the pwsh of the image this job runs
# in, and nothing else runs it before a real publish: its absence is found here, not
# on the day the Gallery account exists. Windows PowerShell 5.1 has no PSResourceGet
# of its own, and publishes nothing.
if ($PSVersionTable.PSEdition -eq 'Core') {
    $publisher = Get-Command Publish-PSResource -ErrorAction SilentlyContinue
    if ($publisher) {
        "Publish-PSResource: $($publisher.Source) $($publisher.Version), which psgallery-publish runs"
    }
    else {
        "FAILED: this pwsh has no Publish-PSResource (Microsoft.PowerShell.PSResourceGet), which psgallery-publish runs in the same image"
        $failed++
    }
}

# ------------------------------------------------------------ the analyzer, from the Gallery
New-Item -ItemType Directory -Force $WorkDirectory | Out-Null
$package = Join-Path $WorkDirectory "PSScriptAnalyzer.$AnalyzerVersion.nupkg"
$unpacked = Join-Path $WorkDirectory "PSScriptAnalyzer/$AnalyzerVersion"
$source = "https://www.powershellgallery.com/api/v2/package/PSScriptAnalyzer/$AnalyzerVersion"
"Downloading $source"
for ($attempt = 1; ; $attempt++) {
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $source -OutFile $package
        break
    }
    catch {
        if ($attempt -ge 3) { throw }
        "  attempt $attempt failed ($($_.Exception.Message)); again in $(10 * $attempt) s"
        Start-Sleep -Seconds (10 * $attempt)
    }
}
$sha = [System.Security.Cryptography.SHA512]::Create()
$stream = [System.IO.File]::OpenRead($package)
try { $actual = [System.Convert]::ToBase64String($sha.ComputeHash($stream)) } finally { $stream.Dispose(); $sha.Dispose() }
if ($actual -ne $AnalyzerSha512) {
    throw "PSScriptAnalyzer $AnalyzerVersion's package has SHA-512 $actual, and the Gallery published $AnalyzerSha512 for it: not loaded."
}
"PSScriptAnalyzer $AnalyzerVersion, SHA-512 as the Gallery published it"
if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
New-Item -ItemType Directory -Force $unpacked | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($package, $unpacked)
Import-Module (Join-Path $unpacked 'PSScriptAnalyzer.psd1') -Force

# ------------------------------------------------------------ the analysis
$findings = $null
for ($attempt = 1; ; $attempt++) {
    try {
        $findings = @(Invoke-ScriptAnalyzer -Path $ModuleDirectory -ErrorAction Stop)
        break
    }
    catch {
        # PSScriptAnalyzer 1.25.0 once threw a NullReferenceException from its first
        # run in a fresh session on the laptop (28 September 2026) and ran cleanly
        # every time after; a crash of the analyzer is not a finding, and is run again
        # once before it fails the job.
        if ($attempt -ge 2) { throw }
        "  Invoke-ScriptAnalyzer threw ($($_.Exception.Message)); running it again"
    }
}
$analysed = @(Get-ChildItem -Path $ModuleDirectory -File | Where-Object { $_.Extension -in '.psd1', '.psm1', '.ps1' } | ForEach-Object Name)
if ($findings.Count) {
    "FAILED: PSScriptAnalyzer found $($findings.Count) thing(s) in $($analysed -join ', '):"
    $findings | Sort-Object ScriptName, Line | Format-Table ScriptName, Line, Severity, RuleName, Message -AutoSize -Wrap | Out-String -Width 220
    $failed++
}
else {
    "PSScriptAnalyzer $AnalyzerVersion, every default rule: nothing in $($analysed -join ', ')"
}

if ($failed) {
    "$failed check(s) failed."
    exit 1
}
"The module is what the Gallery takes."
exit 0
