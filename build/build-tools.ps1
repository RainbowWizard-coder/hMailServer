# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Builds and publishes the .NET tools the installer ships: the database setup
# tools into their per-project publish folders (merged into {app}\Bin by the
# installer) plus DataDirectorySynchronizer and ImportTool. Run before
# compiling the installer. ControlPanel has its own publish step - a plain
# dotnet publish of ControlPanel.csproj into its publish\ folder, spelled out in
# RELEASE.md step 8. This script covers "hMailServer Tools.sln" only, and a
# release that skips that separate step ships whatever hMailCP.dll the previous
# build left behind, because nothing deletes it first.
#
# -Platform ARM64 publishes the same tools, and the Control Panel with them, for
# Windows on ARM (hmailserver/docs/WindowsOnArm.md): win-arm64, into publish-arm64
# beside each project's publish folder, which hMailServerARM64.iss ships. Their
# NuGet lock files are packages.arm64.lock.json, so that an ARM64 restore never
# rewrites the win-x64 lock files CI restores in locked mode. The default, x64,
# does exactly what it always did.
Param(
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$toolsDir = Resolve-Path (Join-Path $scriptRoot '..\hmailserver\source\Tools')

# The COM wrapper every tool project references by HintPath is generated, not
# committed: from the server build's type library when there is a current one,
# otherwise from the IDL (MIDL needs the Visual C++ tools for that). A no-op
# when the wrapper is already made from the same type library.
& (Join-Path $scriptRoot 'generate-com-wrapper.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$publishProjects = 'DBSetup', 'DBSetupQuick', 'DBUpdater', 'DataDirectorySynchronizer', 'ImportTool'

if ($Platform -eq 'ARM64') {
    # The Control Panel too: for x64 it is RELEASE.md step 8's own publish.
    $publishProjects += 'ControlPanel'
    foreach ($project in $publishProjects) {
        $csproj = Join-Path $toolsDir "$project\$project.csproj"
        $output = Join-Path $toolsDir "$project\publish-arm64"
        if (Test-Path $output) { Remove-Item -Recurse -Force $output }

        Write-Host "Publishing $project (win-arm64) -> $output"
        # The projects name x64 as their platform and win-x64 as their runtime;
        # both are overridden here, for this publish and the projects it references.
        # Its own bin and obj as well: the compiler's incremental check does not look
        # at the platform, and an x64 publish after this one must not find ARM64
        # assemblies in obj and ship them.
        dotnet publish $csproj -c $Configuration -o $output -r win-arm64 --self-contained false -p:PlatformTarget=ARM64 -p:NuGetLockFilePath=packages.arm64.lock.json "-p:OutputPath=bin\ARM64\$Configuration\\" "-p:IntermediateOutputPath=obj\ARM64\$Configuration\\"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    Write-Host "All tools published for ARM64."
    exit 0
}

foreach ($project in $publishProjects) {
    $csproj = Join-Path $toolsDir "$project\$project.csproj"
    $output = Join-Path $toolsDir "$project\publish"

    # Start from a clean folder: the installer wildcards its entire contents,
    # and dotnet publish never removes files a previous publish left behind.
    if (Test-Path $output) { Remove-Item -Recurse -Force $output }

    Write-Host "Publishing $project -> $output"
    dotnet publish $csproj -c $Configuration -o $output
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "All tools published."
