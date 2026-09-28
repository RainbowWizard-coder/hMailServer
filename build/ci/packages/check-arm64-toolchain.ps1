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

   When something is missing it says what, and the command that mends it, run once
   from an elevated prompt by the owner. A component that is not installed: the
   Visual Studio Installer's modify of the instance that has MSBuild, adding the
   components it lacks, as WindowsOnArm.md gives it. A file that is not there when
   every component is: never a modify that adds those same components again, which
   changes nothing - that was what this check printed on bench150 in pipeline
   2887115157 (27 September 2026), where the ARM64 tools and ATL were there and it
   reported the ARM64 C++ runtime missing (a report of its own fault: see
   Find-Arm64Runtime). For the runtime it reads the instance's own catalog for the
   component that installs it (the package Microsoft.VisualCpp.CRT.Redist.ARM64)
   and adds that component when the instance lacks it; otherwise, and for any other
   file, the Installer's repair of the instance.

   Exit codes: 0 everything is there; 2 something is missing (each named); for
   -SelfTest, 0 every case passed and 1 one failed.

   Usage:
     pwsh -NoProfile -File build\ci\packages\check-arm64-toolchain.ps1
     pwsh -NoProfile -File build\ci\packages\check-arm64-toolchain.ps1 -SelfTest
#>

param(
   # The Visual Studio versions accepted: 2026 (18.x), whose default toolset is the
   # v145 the projects are pinned to, as libraries\build-common.ps1 asks for.
   [string] $VersionRange = '[18.0,19.0)',
   # The remedy's cases against catalogs made to order; reads no Visual Studio.
   [switch] $SelfTest
)

$ErrorActionPreference = 'Continue'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vs_installer.exe'
$base = @('Microsoft.Component.MSBuild', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
$arm64 = @('Microsoft.VisualStudio.Component.VC.Tools.ARM64', 'Microsoft.VisualStudio.Component.VC.ATL.ARM64')
# The Installer's package that puts VC\Redist\MSVC\<version>\arm64\Microsoft.VC*.CRT
# on the machine. Build Tools 18.9's catalog makes it a dependency of VC.Tools.ARM64
# itself; bench150's 18.10 had VC.Tools.ARM64 and not the folder.
$redistPackage = 'Microsoft.VisualCpp.CRT.Redist.ARM64'
$missing = New-Object System.Collections.Generic.List[string]

function Stop-Toolchain([string[]] $what, [string[]] $fix) {
   "ARM64 TOOLCHAIN MISSING:"
   $what | ForEach-Object { "  - $_" }
   if ($fix) { "The fix, once, from an elevated prompt on this machine (hmailserver/docs/WindowsOnArm.md, 'Visual Studio'):"; $fix | ForEach-Object { "  $_" } }
   "ARM64 TOOLCHAIN EXIT=2"
   exit 2
}

# The ARM64 C++ runtime folders of an instance: VC\Redist\MSVC\<version>\arm64\
# Microsoft.VC*.CRT. The wildcard is in the whole path. Until 28 September 2026 the
# check asked Get-ChildItem for 'VC\Redist\MSVC\*\arm64' with -Filter 'Microsoft.VC*.CRT':
# with a wildcard in -Path, Get-ChildItem returns the matching arm64 folders themselves
# and filters THEIR names, so it found no runtime on any machine - on the laptop, whose
# Build Tools 18.9 has 14.51.36231\arm64\Microsoft.VC145.CRT, and on bench150, which
# pipeline 2887115157 reported as lacking it.
function Find-Arm64Runtime([string] $instanceRoot) {
   return @(Get-ChildItem -Path (Join-Path $instanceRoot 'VC\Redist\MSVC\*\arm64\Microsoft.VC*.CRT') -Directory -ErrorAction SilentlyContinue)
}

# The components of a catalog (an instance's catalog.json) that install $package:
# those that name it among their dependencies, or name a package or group that does,
# the nearest first. Empty when the catalog cannot be read or names none.
function Get-ComponentsInstalling([string] $catalogPath, [string] $package) {
   try { $catalog = Get-Content -LiteralPath $catalogPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop }
   catch { return @() }
   $parents = @{}
   foreach ($p in @($catalog.packages)) {
      if (-not $p -or -not $p.id -or -not $p.dependencies) { continue }
      foreach ($name in $p.dependencies.PSObject.Properties.Name) {
         $key = $name.ToLowerInvariant()
         if (-not $parents.ContainsKey($key)) { $parents[$key] = New-Object System.Collections.Generic.List[string] }
         $parents[$key].Add([string] $p.id)
      }
   }
   $seen = @{}
   $level = @($package.ToLowerInvariant())
   while ($level.Count) {
      $next = New-Object System.Collections.Generic.List[string]
      $found = New-Object System.Collections.Generic.List[string]
      foreach ($id in $level) {
         if (-not $parents.ContainsKey($id)) { continue }
         foreach ($parent in $parents[$id]) {
            $key = $parent.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            if ($parent -like 'Microsoft.VisualStudio.Component.*') { $found.Add($parent) } else { $next.Add($key) }
         }
      }
      if ($found.Count) { return @($found | Sort-Object -Unique) }
      $level = @($next)
   }
   return @()
}

# The commands that mend what is missing once every component the build needs is
# installed: the runtime's component added when the catalog names one the instance
# lacks, and the instance repaired for anything else, the runtime included when its
# component is installed already or the catalog names none.
function Get-Remedy {
   param([string] $Installer, [string] $Root, [bool] $RuntimeMissing, [bool] $OtherMissing,
         [string[]] $RuntimeComponents, [string[]] $Installed)
   $fix = New-Object System.Collections.Generic.List[string]
   $repair = $OtherMissing
   if ($RuntimeMissing) {
      $absent = @($RuntimeComponents | Where-Object { $_ -and $Installed -notcontains $_ })
      $present = @($RuntimeComponents | Where-Object { $_ -and $Installed -contains $_ })
      if ($absent.Count -and -not $present.Count) {
         $fix.Add("`"$Installer`" modify --installPath `"$Root`" --add $($absent[0]) --passive --norestart")
      } else {
         if (-not $RuntimeComponents.Count) { $fix.Add("(this instance's catalog names no component that installs $redistPackage)") }
         $repair = $true
      }
   }
   if ($repair) { $fix.Add("`"$Installer`" repair --installPath `"$Root`" --passive --norestart") }
   return $fix.ToArray()
}

# Whether a command adds a component the instance has: a modify that changes nothing.
function Test-AddsInstalled([string] $command, [string[]] $installed) {
   foreach ($id in $installed) { if ($command -match ('--add ' + [regex]::Escape($id) + '(\s|$)')) { return $true } }
   return $false
}

function Invoke-SelfTest {
   $dir = Join-Path ([IO.Path]::GetTempPath()) ("arm64-toolchain-selftest-" + [guid]::NewGuid().ToString('N'))
   New-Item -ItemType Directory -Path $dir | Out-Null
   function Write-Catalog([string] $name, [string] $json) { $path = Join-Path $dir $name; [IO.File]::WriteAllText($path, $json); $path }
   function Check([bool] $condition, [string] $what) {
      if ($condition) { "  ok    $what" } else { "  FAIL  $what"; $script:selfTestFailed = $true }
   }
   $script:selfTestFailed = $false
   $tools = 'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
   $atl = 'Microsoft.VisualStudio.Component.VC.ATL.ARM64'
   $runtimes = 'Microsoft.VisualStudio.Component.VC.Runtimes.ARM64'
   $root = 'C:\VS\BuildTools'
   try {
      # As Build Tools 18.9's catalog has it: the runtime a dependency of VC.Tools.ARM64,
      # which a workload and another component name as well.
      $asToday = Write-Catalog 'today.json' (@{ packages = @(
         @{ id = $tools; type = 'Component'; dependencies = @{ $redistPackage = ''; 'Microsoft.VisualCpp.CRT.Headers' = '' } },
         @{ id = 'Microsoft.VisualStudio.Workload.NativeDesktop'; type = 'Workload'; dependencies = @{ $tools = '' } },
         @{ id = 'Microsoft.VisualStudio.Component.UWP.VC.ARM64'; type = 'Component'; dependencies = @{ $tools = '' } },
         @{ id = $redistPackage; type = 'Vsix'; dependencies = @{ 'Microsoft.VC.14.51.CRT.Redist.ARM64.base' = '' } }
      ) } | ConvertTo-Json -Depth 6)
      # A catalog that installs it through a group, from a component of its own.
      $throughGroup = Write-Catalog 'group.json' (@{ packages = @(
         @{ id = $tools; type = 'Component'; dependencies = @{ 'Microsoft.VisualCpp.CRT.Headers' = '' } },
         @{ id = $runtimes; type = 'Component'; dependencies = @{ 'Microsoft.VisualStudio.PackageGroup.VC.Runtimes.ARM64' = '' } },
         @{ id = 'Microsoft.VisualStudio.PackageGroup.VC.Runtimes.ARM64'; type = 'Group'; dependencies = @{ $redistPackage = @{ when = @('Microsoft.VisualStudio.Product.BuildTools') } } }
      ) } | ConvertTo-Json -Depth 6)
      $broken = Write-Catalog 'broken.json' '{ "packages": [ { "id": '

      "-- the ARM64 C++ runtime folder"
      # An instance as the laptop's Build Tools 18.9 lays it out: a versioned folder
      # with arm64\Microsoft.VC145.CRT, and v145 beside it with the redistributable
      # installers only.
      $withRuntime = Join-Path $dir 'with (x86)'
      New-Item -ItemType Directory -Path (Join-Path $withRuntime 'VC\Redist\MSVC\14.51.36231\arm64\Microsoft.VC145.CRT') -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $withRuntime 'VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT') -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $withRuntime 'VC\Redist\MSVC\v145') -Force | Out-Null
      [IO.File]::WriteAllText((Join-Path $withRuntime 'VC\Redist\MSVC\v145\vc_redist.arm64.exe'), 'not a program')
      $withoutRuntime = Join-Path $dir 'without'
      New-Item -ItemType Directory -Path (Join-Path $withoutRuntime 'VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT') -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $withoutRuntime 'VC\Redist\MSVC\14.51.36231\arm64\Microsoft.VC145.OPENMP') -Force | Out-Null
      $found = @(Find-Arm64Runtime $withRuntime)
      Check ($found.Count -eq 1 -and $found[0].FullName -like '*\14.51.36231\arm64\Microsoft.VC145.CRT') "an instance with arm64\Microsoft.VC145.CRT: found (got: $($found.FullName -join ', '))"
      Check (@(Find-Arm64Runtime $withoutRuntime).Count -eq 0) 'an instance with only the x64 runtime and an arm64 OPENMP: none'
      # The negative control: the expression this check used until 28 September 2026
      # finds nothing in the instance that has the runtime.
      $old = @(Get-ChildItem -Path (Join-Path $withRuntime 'VC\Redist\MSVC\*\arm64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue)
      Check ($old.Count -eq 0) "negative control: the old -Path '...\*\arm64' -Filter 'Microsoft.VC*.CRT' finds none there (got: $($old.Count))"

      "-- the components the catalog names"
      $c = @(Get-ComponentsInstalling $asToday $redistPackage)
      Check ($c.Count -eq 1 -and $c[0] -eq $tools) "18.9's catalog: VC.Tools.ARM64, and not the component or the workload that only name it (got: $($c -join ', '))"
      $c = @(Get-ComponentsInstalling $throughGroup $redistPackage)
      Check ($c.Count -eq 1 -and $c[0] -eq $runtimes) "through a group: the component above the group (got: $($c -join ', '))"
      Check (@(Get-ComponentsInstalling $broken $redistPackage).Count -eq 0) 'a catalog that is not JSON names none'
      Check (@(Get-ComponentsInstalling (Join-Path $dir 'absent.json') $redistPackage).Count -eq 0) 'a catalog that is not there names none'

      "-- the remedy"
      $installed = @('Microsoft.Component.MSBuild', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', $tools, $atl)
      # The negative control: the command the check printed on bench150 on 27 September
      # 2026, with both components installed, is one that adds what the instance has.
      $printed = "`"$installer`" modify --installPath `"$root`" --add $tools --add $atl --passive --norestart"
      Check (Test-AddsInstalled $printed $installed) 'negative control: the command of pipeline 2887115157 adds components the instance has'

      $fix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $true -OtherMissing $false -RuntimeComponents @(Get-ComponentsInstalling $asToday $redistPackage) -Installed $installed)
      Check ($fix.Count -eq 1 -and $fix[0] -match ' repair --installPath "C:\\VS\\BuildTools" --passive --norestart$') "the runtime missing under an installed VC.Tools.ARM64: repair (got: $($fix -join ' | '))"
      Check (-not ($fix | Where-Object { Test-AddsInstalled $_ $installed })) '...and nothing that adds an installed component'

      $fix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $true -OtherMissing $false -RuntimeComponents @(Get-ComponentsInstalling $throughGroup $redistPackage) -Installed $installed)
      Check ($fix.Count -eq 1 -and $fix[0] -match " modify --installPath `"C:\\VS\\BuildTools`" --add $([regex]::Escape($runtimes)) --passive --norestart$") "the runtime's own component not installed: modify --add it (got: $($fix -join ' | '))"
      Check (-not ($fix | Where-Object { Test-AddsInstalled $_ $installed })) '...and nothing that adds an installed component'

      $fix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $true -OtherMissing $false -RuntimeComponents @() -Installed $installed)
      Check ($fix.Count -eq 2 -and $fix[0] -match 'names no component' -and $fix[1] -match ' repair ') "no catalog: says so, and repair (got: $($fix -join ' | '))"

      $fix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $false -OtherMissing $true -RuntimeComponents @() -Installed $installed)
      Check ($fix.Count -eq 1 -and $fix[0] -match ' repair ') "a compiler file missing under installed components: repair (got: $($fix -join ' | '))"

      $fix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $true -OtherMissing $true -RuntimeComponents @($runtimes) -Installed $installed)
      Check ($fix.Count -eq 2 -and $fix[0] -match "--add $([regex]::Escape($runtimes))" -and $fix[1] -match ' repair ') "both: the runtime's component added, then repair (got: $($fix -join ' | '))"

      $fix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $false -OtherMissing $false -RuntimeComponents @() -Installed $installed)
      Check ($fix.Count -eq 0) 'nothing missing: no command'
   } finally {
      Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
   }
   if ($script:selfTestFailed) { 'SELF-TEST FAILED'; return 1 }
   'SELF-TEST PASSED'
   return 0
}

if ($SelfTest) {
   $out = @(Invoke-SelfTest)
   $out | Where-Object { $_ -is [string] }
   exit (@($out | Where-Object { $_ -is [int] })[-1])
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

# The default toolset's own files: a component can be registered while its files are
# not. Every component above is installed on this instance, so what is missing here is
# mended by the Installer's repair, or for the ARM64 C++ runtime by the component the
# catalog says installs it (Get-Remedy) - never by adding the same components again.
$versionFile = Join-Path $root 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
$tools = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim() } else { $null }
$vsFilesMissing = $false
$runtimeMissing = $false
if (-not $tools) { $missing.Add("the default MSVC toolset version ($versionFile)"); $vsFilesMissing = $true }
else {
   "MSVC toolset $tools"
   $msvc = Join-Path $root "VC\Tools\MSVC\$tools"
   foreach ($file in @('bin\Hostx64\arm64\cl.exe', 'bin\Hostx64\arm64\link.exe', 'atlmfc\lib\arm64\atls.lib', 'lib\arm64\msvcrt.lib')) {
      if (Test-Path -LiteralPath (Join-Path $msvc $file)) { "  ok  $file" } else { $missing.Add("$msvc\$file"); $vsFilesMissing = $true }
   }
   $mfc = @(Get-ChildItem -LiteralPath (Join-Path $msvc 'atlmfc\lib\arm64') -Filter 'mfc*.lib' -ErrorAction SilentlyContinue)
   "  MFC for ARM64: $(if ($mfc.Count) { 'present' } else { 'absent' }) (not needed: no server project uses MFC)"
}
$cross = Join-Path $root 'VC\Auxiliary\Build\vcvarsamd64_arm64.bat'
if (Test-Path -LiteralPath $cross) { "  ok  vcvarsamd64_arm64.bat" } else { $missing.Add($cross); $vsFilesMissing = $true }
$crt = @(Find-Arm64Runtime $root)
if ($crt.Count) { "  ok  the ARM64 C++ runtime: $($crt[-1].FullName)" }
else {
   $runtimeMissing = $true
   $missing.Add("$root\VC\Redist\MSVC\<version>\arm64\Microsoft.VC*.CRT (the ARM64 C++ runtime the installer carries)")
}

$vsFix = @()
if ($runtimeMissing -or $vsFilesMissing) {
   $runtimeComponents = @()
   $installedHere = @()
   if ($runtimeMissing) {
      $catalogPath = Join-Path $env:ProgramData "Microsoft\VisualStudio\Packages\_Instances\$($instance.instanceId)\catalog.json"
      $runtimeComponents = @(Get-ComponentsInstalling $catalogPath $redistPackage)
      foreach ($id in $runtimeComponents) {
         if (@(Get-Instances @($id) | Where-Object { $_.instanceId -eq $instance.instanceId }).Count) { $installedHere += $id }
      }
      "  the ARM64 C++ runtime ($redistPackage) is installed by, in $catalogPath" + $(if ($runtimeComponents.Count) { ": " + (($runtimeComponents | ForEach-Object { if ($installedHere -contains $_) { "$_ (installed)" } else { "$_ (not installed)" } }) -join ', ') } else { ": no component the catalog names" })
   }
   $vsFix = @(Get-Remedy -Installer $installer -Root $root -RuntimeMissing $runtimeMissing -OtherMissing $vsFilesMissing -RuntimeComponents $runtimeComponents -Installed $installedHere)
}

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
   if ($fixSdk) { $missing.Add($fixSdk) }
   Stop-Toolchain $missing $vsFix
}
"ARM64 TOOLCHAIN EXIT=0"
exit 0
