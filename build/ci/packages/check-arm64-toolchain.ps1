# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   Can this machine build hMailServer for Windows on ARM? The first step of the
   windows-packages-arm64 job (.gitlab/ci/packages-windows.yml), before anything is
   built or the ARM64 libraries are touched, and a check anyone can run on a bench
   by hand: it reads, and changes nothing.

   What the ARM64 build needs (hmailserver/docs/WindowsOnArm.md), all in ONE Visual
   Studio 2026 instance, because build\build.ps1 -Platform ARM64 takes MSBuild from
   the instance that has the ARM64 compilers and ATL for ARM64 together:

     Microsoft.Component.MSBuild                        MSBuild
     Microsoft.VisualStudio.Component.VC.Tools.x86.x64  the x64 compilers (MIDL, b2's bootstrap)
     Microsoft.VisualStudio.Component.VC.Tools.ARM64    "MSVC v145 - ARM64/ARM64EC build tools"
     Microsoft.VisualStudio.Component.VC.ATL.ARM64      "C++ ATL for v145 build tools (ARM64/ARM64EC)"

   and, looked for as files in that instance's default toolset (so that a component
   id that is present but a toolset that is not is caught too): the x64-hosted ARM64
   compiler (bin\Hostx64\arm64\cl.exe), ATL for ARM64 (atlmfc\lib\arm64\atls.lib),
   the cross environment's vcvarsamd64_arm64.bat, and the ARM64 C++ runtime the
   installer carries (Redist\MSVC\<version>\arm64\Microsoft.VC*.CRT). And from the
   Windows SDK: the newest Windows 10/11 SDK, the one WindowsTargetPlatformVersion
   10.0 selects, has its ARM64 import libraries (Lib\<version>\um\arm64\kernel32.lib
   and ucrt\arm64\ucrt.lib).

   MFC for ARM64 is reported and not required: no project of the server sets
   UseOfMfc, and the server's ATL is static (UseOfAtl Static in hMailServer.vcxproj).

   When something is missing it says what, and the one command that adds it: the
   Visual Studio Installer's modify of the instance that has MSBuild, as
   WindowsOnArm.md gives it, run once from an elevated prompt by the owner.

   Exit codes: 0 everything is there; 2 something is missing (each named).

   Usage:
     pwsh -NoProfile -File build\ci\packages\check-arm64-toolchain.ps1
#>

param(
   # The Visual Studio versions accepted: 2026 (18.x), whose default toolset is the
   # v145 the projects are pinned to, as libraries\build-common.ps1 asks for.
   [string] $VersionRange = '[18.0,19.0)'
)

$ErrorActionPreference = 'Continue'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vs_installer.exe'
$base = @('Microsoft.Component.MSBuild', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
$arm64 = @('Microsoft.VisualStudio.Component.VC.Tools.ARM64', 'Microsoft.VisualStudio.Component.VC.ATL.ARM64')
$missing = New-Object System.Collections.Generic.List[string]

function Stop-Toolchain([string[]] $what, [string] $fix) {
   "ARM64 TOOLCHAIN MISSING:"
   $what | ForEach-Object { "  - $_" }
   if ($fix) { "The fix, once, from an elevated prompt on this machine (hmailserver/docs/WindowsOnArm.md, 'Visual Studio'):"; "  $fix" }
   "ARM64 TOOLCHAIN EXIT=2"
   exit 2
}

if (-not (Test-Path -LiteralPath $vswhere)) {
   Stop-Toolchain @("vswhere.exe is not at ${vswhere}: no Visual Studio Installer on this machine") 'install the Visual Studio 2026 Build Tools with the C++ workload (https://aka.ms/vs/18/stable/vs_BuildTools.exe), then run this again'
}

# The instances vswhere knows, with or without what is asked, for the report.
function Get-Instances([string[]] $requires) {
   $arguments = @('-products', '*', '-version', $VersionRange, '-format', 'json', '-utf8')
   if ($requires.Count) { $arguments += @('-requires') + $requires }
   $json = (& $vswhere @arguments | Out-String)
   if ([string]::IsNullOrWhiteSpace($json)) { return @() }
   return @($json | ConvertFrom-Json)
}

$all = Get-Instances @()
"== Visual Studio $VersionRange instances on $env:COMPUTERNAME"
if ($all.Count -eq 0) {
   Stop-Toolchain @("no Visual Studio $VersionRange instance at all") 'install the Visual Studio 2026 Build Tools with the C++ workload (https://aka.ms/vs/18/stable/vs_BuildTools.exe), then run this again'
}
foreach ($i in $all) {
   $has = foreach ($id in ($base + $arm64 + 'Microsoft.VisualStudio.Component.VC.MFC.ARM64')) {
      $hit = @(Get-Instances @($id) | Where-Object { $_.instanceId -eq $i.instanceId })
      "{0}={1}" -f ($id -replace '^Microsoft\.(VisualStudio\.)?Component\.', ''), $(if ($hit.Count) { 'yes' } else { 'no' })
   }
   "  {0} {1} at {2}" -f $i.displayName, $i.installationVersion, $i.installationPath
   "     $($has -join '  ')"
}

# A prerelease instance (Visual Studio Insiders or Preview) is not one the build
# uses: build\Find-MsBuild.ps1 and libraries\build-common.ps1 ask vswhere without
# -prerelease. Named, so that its ARM64 tools are not mistaken for the bench's.
$prerelease = @(& $vswhere -products * -prerelease -version $VersionRange -format json -utf8 | Out-String | ConvertFrom-Json |
   Where-Object { $_.isPrerelease })
foreach ($p in $prerelease) { "  (not used by the build, a prerelease: $($p.displayName) $($p.installationVersion) at $($p.installationPath))" }

$withMsBuild = @(Get-Instances $base)
$complete = @(Get-Instances ($base + $arm64))
if ($complete.Count -eq 0) {
   $target = if ($withMsBuild.Count) { $withMsBuild[0].installationPath } else { $all[0].installationPath }
   $absent = foreach ($id in $arm64) { if (-not @(Get-Instances @($id) | Where-Object { $_.installationPath -eq $target }).Count) { $id } }
   $what = @("no instance has MSBuild, the x64 and ARM64 compilers and ATL for ARM64 together; $target lacks: $($absent -join ', ')")
   $adds = ($absent | ForEach-Object { "--add $_" }) -join ' '
   Stop-Toolchain $what ("`"$installer`" modify --installPath `"$target`" $adds --passive --norestart")
}
$instance = $complete[0]
$root = $instance.installationPath
"the instance the ARM64 build uses: $($instance.displayName) $($instance.installationVersion) at $root"

# The default toolset's own files: a component can be registered while its files are not.
$versionFile = Join-Path $root 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
$tools = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim() } else { $null }
$fixArm = "`"$installer`" modify --installPath `"$root`" --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.ATL.ARM64 --passive --norestart"
if (-not $tools) { $missing.Add("the default MSVC toolset version ($versionFile)") }
else {
   "MSVC toolset $tools"
   $msvc = Join-Path $root "VC\Tools\MSVC\$tools"
   foreach ($file in @('bin\Hostx64\arm64\cl.exe', 'bin\Hostx64\arm64\link.exe', 'atlmfc\lib\arm64\atls.lib', 'lib\arm64\msvcrt.lib')) {
      if (Test-Path -LiteralPath (Join-Path $msvc $file)) { "  ok  $file" } else { $missing.Add("$msvc\$file") }
   }
   $mfc = @(Get-ChildItem -LiteralPath (Join-Path $msvc 'atlmfc\lib\arm64') -Filter 'mfc*.lib' -ErrorAction SilentlyContinue)
   "  MFC for ARM64: $(if ($mfc.Count) { 'present' } else { 'absent' }) (not needed: no server project uses MFC)"
}
$cross = Join-Path $root 'VC\Auxiliary\Build\vcvarsamd64_arm64.bat'
if (Test-Path -LiteralPath $cross) { "  ok  vcvarsamd64_arm64.bat" } else { $missing.Add($cross) }
$crt = @(Get-ChildItem -Path (Join-Path $root 'VC\Redist\MSVC\*\arm64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue)
if ($crt.Count) { "  ok  the ARM64 C++ runtime: $($crt[-1].FullName)" } else { $missing.Add("$root\VC\Redist\MSVC\<version>\arm64\Microsoft.VC*.CRT (the ARM64 C++ runtime the installer carries)") }

# The Windows SDK MSBuild selects for WindowsTargetPlatformVersion 10.0: the newest
# one installed, which is the newest folder under Lib with an um\x64 in it.
$kitsRoot = $null
foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots', 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots') {
   $value = (Get-ItemProperty -Path $key -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
   if ($value) { $kitsRoot = $value; break }
}
if (-not $kitsRoot) { $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$sdk = Get-ChildItem -LiteralPath (Join-Path $kitsRoot 'Lib') -Directory -ErrorAction SilentlyContinue |
   Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'um\x64\kernel32.lib')) } |
   Sort-Object { [version]$_.Name } | Select-Object -Last 1
$fixSdk = $null
if (-not $sdk) { $missing.Add("no Windows 10/11 SDK with x64 libraries under $kitsRoot\Lib") }
else {
   "Windows SDK $($sdk.Name) at $kitsRoot"
   foreach ($file in @('um\arm64\kernel32.lib', 'ucrt\arm64\ucrt.lib')) {
      if (Test-Path -LiteralPath (Join-Path $sdk.FullName $file)) { "  ok  $file" }
      else {
         $missing.Add("$($sdk.FullName)\$file")
         $fixSdk = "the Windows SDK $($sdk.Name) needs its ARM64 libraries: in Settings, Apps, 'Windows Software Development Kit - $($sdk.Name)', Modify, add 'Windows SDK for Desktop C++ ARM64 Apps' (the SDK installer's feature OptionId.DesktopCPParm64)"
      }
   }
}

if ($missing.Count) {
   $fix = $fixArm
   if ($fixSdk) { $missing.Add($fixSdk) }
   Stop-Toolchain $missing $fix
}
"ARM64 TOOLCHAIN EXIT=0"
exit 0
