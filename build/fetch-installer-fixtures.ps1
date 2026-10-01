# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
   Puts the real release installers the internal suite checks an administrator's
   install of an update against where AdminUpdateTester looks for them: the
   folder HMAILSERVER_INSTALLER_FIXTURES names, or C:\HMTest\Installers.

.DESCRIPTION
   AdminUpdateTester and AdminUpdateStageTester (Common/Util/AdminUpdateTester.h,
   AdminUpdate.h) hold the Authenticode publisher pin and the Sigstore check of the
   Control Panel's Install update to the installers the project actually released:
   6.3.1, 6.3.3, 6.3.4 and 6.3.5 must pass the pin, 6.3.0 is not signed, Microsoft's
   .NET Desktop Runtime installer is another publisher, and 6.3.5's bundle must
   verify. They are 80 MB each and are not in git. This fetches each from the
   project's GitLab releases (the .NET runtime from Microsoft) and keeps it only when
   its SHA-256 - SHA-512 for the runtime - is the one written here, which is the
   file that was released. A file already there is checked the same way and
   replaced when it is not that file.

   A missing fixture FAILS the testers rather than skipping them, and names this
   script: a check whose test never runs looks exactly like one that does not
   work. Run it once on a bench, and again when this list changes.

.PARAMETER Destination
   Where to put them; by default HMAILSERVER_INSTALLER_FIXTURES, or C:\HMTest\Installers.
#>
Param(
   [string]$Destination = $(if ($env:HMAILSERVER_INSTALLER_FIXTURES) { $env:HMAILSERVER_INSTALLER_FIXTURES } else { 'C:\HMTest\Installers' })
)

$ErrorActionPreference = 'Stop'

$project = 'https://gitlab.com/api/v4/projects/86730559/packages/generic/hmailserver'
$fixtures = @(
   @{ Name = 'hMailServer-6.3.0-x64.exe'; Url = "$project/6.3.0/hMailServer-6.3.0-x64.exe"; Algorithm = 'SHA256'; Hash = 'd9f1217375dd70d523ffb27dbdcd668dd525edc98edf64f00b4f69d070434fcb' },
   @{ Name = 'hMailServer-6.3.1-x64.exe'; Url = "$project/6.3.1/hMailServer-6.3.1-x64.exe"; Algorithm = 'SHA256'; Hash = '68c72a60e7a8530bdd32d28b60eaadab5b9c90505e174c03196e5dfa58ac9b70' },
   @{ Name = 'hMailServer-6.3.3-x64.exe'; Url = "$project/6.3.3/hMailServer-6.3.3-x64.exe"; Algorithm = 'SHA256'; Hash = '3188be2fa16a4c43d3b43ddbd7274e4cfe878c082343ef25ffbd00489610a057' },
   @{ Name = 'hMailServer-6.3.4-x64.exe'; Url = "$project/6.3.4/hMailServer-6.3.4-x64.exe"; Algorithm = 'SHA256'; Hash = '843cb07b753f8d3d764b3120029e47ea407aa6874468cb3351a06777e3f2310f' },
   @{ Name = 'hMailServer-6.3.5-x64.exe'; Url = "$project/6.3.5/hMailServer-6.3.5-x64.exe"; Algorithm = 'SHA256'; Hash = 'ebb708e056dfd1ca3bcb56b6356e819511b496965e0a82834bdf6987fec79e97' },
   @{ Name = 'hMailServer-6.3.5-x64.exe.cosign.bundle'; Url = "$project/6.3.5/hMailServer-6.3.5-x64.exe.cosign.bundle"; Algorithm = 'SHA256'; Hash = $null },
   # Microsoft's, validly signed and not by the project's publisher: the pin must
   # refuse it. The version and hash the installer build pins
   # (hmailserver/docs/third-party-binaries.json).
   @{ Name = 'windowsdesktop-runtime-10.0.12-win-x64.exe'; Url = 'https://builds.dotnet.microsoft.com/dotnet/WindowsDesktop/10.0.12/windowsdesktop-runtime-10.0.12-win-x64.exe'; Algorithm = 'SHA512'; Hash = '0b907e9312867172a4eb82f4b5ab3f7c2d25e27d8349546d77eeb5d5b8cbecab9ebebfed189b13e9669dc578d892756c548366da539d47b3ddac5efcb7ae72fe' }
)

if (-not (Test-Path $Destination)) {
   New-Item -ItemType Directory -Path $Destination -Force | Out-Null
}

$failed = 0
foreach ($fixture in $fixtures) {
   $path = Join-Path $Destination $fixture.Name
   $good = $false
   if (Test-Path $path) {
      $good = ($null -eq $fixture.Hash) -or ((Get-FileHash -Algorithm $fixture.Algorithm -LiteralPath $path).Hash -ieq $fixture.Hash)
      if (-not $good) {
         Write-Host "Not the released file, replaced: $path"
         Remove-Item -LiteralPath $path -Force
      }
   }
   if (-not $good) {
      $partial = "$path.partial"
      Write-Host "Fetching $($fixture.Url)"
      Invoke-WebRequest -Uri $fixture.Url -OutFile $partial -UseBasicParsing
      # The bundle is checked by what it verifies: 6.3.5's SHA-256 above, through
      # AdminUpdateStageTester.
      if (($null -ne $fixture.Hash) -and ((Get-FileHash -Algorithm $fixture.Algorithm -LiteralPath $partial).Hash -ine $fixture.Hash)) {
         Remove-Item -LiteralPath $partial -Force
         Write-Host "FAILED: $($fixture.Name) is not the released file ($($fixture.Algorithm) differs); nothing was kept."
         $failed++
         continue
      }
      Move-Item -LiteralPath $partial -Destination $path -Force
   }
   Write-Host "OK  $path"
}

if ($failed -gt 0) {
   exit 1
}
Write-Host "The installer fixtures are in $Destination."
exit 0
