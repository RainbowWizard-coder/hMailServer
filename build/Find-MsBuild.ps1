# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

function Find-MsBuild {
    param(
        # Empty = latest installed Visual Studio / Build Tools.
        [string]$VsWhereMinVersion = '',
        # Further Visual Studio components the instance must have, by id - the
        # ARM64 build asks for the ARM64 compilers and ATL for ARM64, which a
        # machine may have in one of its instances and not another. Empty = none,
        # and the search is the one it always was. When components are named, an
        # msbuild.exe on PATH is not taken instead: nothing says it has them.
        [string[]]$Requires = @()
    )

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

    $msbuild = $null
    if (Test-Path $vswhere) {
        try {
            # One -requires with every id after it: vswhere takes the ids that follow
            # the switch, and an instance must have all of them.
            $vswhereArgs = @('-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild') + $Requires + @('-find', 'MSBuild\**\Bin\MSBuild.exe')
            if ($VsWhereMinVersion) { $vswhereArgs = @('-version', $VsWhereMinVersion) + $vswhereArgs }

            $msbuild = & $vswhere @vswhereArgs | Select-Object -First 1
        } catch {
            $msbuild = $null
        }
    } else {
        Write-Verbose "vswhere not found at $vswhere"
    }

    if (-not $msbuild -and -not $Requires.Count) {
        $msbuildCmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
        if ($msbuildCmd) { $msbuild = $msbuildCmd.Source }
    }

    return $msbuild
}
