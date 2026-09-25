# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The second Windows bench's gate lock: one file, C:\bench\gate.lock, that whoever
   is about to change the bench's checkout, its build or its service creates, and
   removes when done. Two things run gates there - GitLab's windows-gate job
   (.gitlab-ci.yml) and the maintainer's split gate over SSH - and either one
   checking out a commit under the other's running suite is a gate that means
   nothing.

   The protocol, which is all a third user needs to follow:
     * take:    create the file EXCLUSIVELY (it must not exist), holding one line,
                "<holder> <UTC time, yyyy-MM-ddTHH:mm:ssZ>";
     * release: remove it, but only when its line starts with your holder;
     * stale:   a lock older than -StaleHours (5: an hour more than the longest a
                job may run) is taken over, and the takeover is said.

   A holder is one word with no spaces. The CI job's is
   gitlab-pipeline-<pipeline id>-job-<job id>; the split gate's names itself and
   the commit.

   -Take    exit 0 when the lock is this holder's (taken now, or already), 1 when
            another holder kept it for -WaitMinutes, 3 when it cannot be made at all
   -Release exit 0 when this holder's lock was removed, 4 when the lock is another
            holder's (left alone), 5 when there was none

   Examples:
     pwsh -NoProfile -File C:\bench\gate-lock.ps1 -Take -Holder split-gate-b35-3a6a93c00
     pwsh -NoProfile -File C:\bench\gate-lock.ps1 -Release -Holder split-gate-b35-3a6a93c00
#>

[CmdletBinding(DefaultParameterSetName = 'Take')]
param(
   [Parameter(Mandatory, ParameterSetName = 'Take')][switch] $Take,
   [Parameter(Mandatory, ParameterSetName = 'Release')][switch] $Release,
   [Parameter(Mandatory)][string] $Holder,
   [string] $Path = 'C:\bench\gate.lock',
   [int] $WaitMinutes = 0,
   [double] $StaleHours = 5
)

$ErrorActionPreference = 'Stop'

if ($Holder -notmatch '^\S+$') {
   Write-Output "A holder is one word with no spaces; '$Holder' is not."
   exit 3
}

function Read-Lock {
   try { return ((Get-Content -LiteralPath $Path -TotalCount 1 -ErrorAction Stop) -as [string]).Trim() }
   catch { return $null }
}

if ($Release) {
   $line = Read-Lock
   if ($null -eq $line) {
      Write-Output "gate lock: none at $Path"
      exit 5
   }
   if ($line.StartsWith("$Holder ") -or $line -eq $Holder) {
      Remove-Item -LiteralPath $Path -Force
      Write-Output "gate lock released: $line"
      exit 0
   }
   Write-Output "gate lock is not ours ($Holder), left alone: $line"
   exit 4
}

$parent = Split-Path -Parent $Path
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
   Write-Output "gate lock: $parent does not exist, so $Path cannot be made."
   exit 3
}

$deadline = (Get-Date).AddMinutes($WaitMinutes)
$said = $null
while ($true) {
   try {
      $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
      try {
         $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
         $writer.WriteLine("$Holder " + [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
         $writer.Dispose()
      } finally {
         $stream.Dispose()
      }
      Write-Output "gate lock taken: $(Read-Lock)"
      exit 0
   } catch [UnauthorizedAccessException] {
      Write-Output "gate lock: not allowed to create $Path ($($_.Exception.Message))"
      exit 3
   } catch [IO.IOException] {
      # It exists (or is being written this instant): whose, and how old?
      $line = Read-Lock
      if ($null -eq $line) { Start-Sleep -Seconds 2; continue }
      if ($line.StartsWith("$Holder ")) {
         Write-Output "gate lock is already ours: $line"
         exit 0
      }
      $age = (Get-Date) - (Get-Item -LiteralPath $Path).LastWriteTime
      if ($age.TotalHours -ge $StaleHours) {
         Write-Output ("gate lock is stale ({0:N1} h old, older than {1} h), taken over: {2}" -f $age.TotalHours, $StaleHours, $line)
         Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
         continue
      }
      if ((Get-Date) -ge $deadline) {
         Write-Output "the bench is busy - gate lock held by: $line"
         exit 1
      }
      if ($said -ne $line) {
         Write-Output ("waiting up to {0} min for the bench - gate lock held by: {1}" -f $WaitMinutes, $line)
         $said = $line
      }
      Start-Sleep -Seconds 30
   }
}
