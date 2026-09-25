# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
   The .NET Desktop Runtime pin, held at the script: build/get-dotnet-runtime.ps1
   refuses a file whose SHA-512 is not the pinned one, whether the file is already
   in place or has just been downloaded, and refuses a pin that is not a pin.

.DESCRIPTION
   Needs no network and changes nothing in the tree. Every case runs the script
   as a separate process, against a file in a temporary directory and, except
   the first two, a manifest of its own; the downloads are served by a loopback
   listener this script starts, which hands out whatever bytes the case chooses.
   The committed pin is exercised too, by -Verify, which downloads nothing.

   Windows PowerShell 5.1 and PowerShell 7 both run it, and it runs the script
   under the same one it was started with:

     powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\test-get-dotnet-runtime.ps1
     pwsh -NoProfile -File build/test-get-dotnet-runtime.ps1

   Exit 0 when every case holds, 1 otherwise.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$getRuntime = Join-Path $PSScriptRoot 'get-dotnet-runtime.ps1'
$shell = (Get-Process -Id $PID).Path
$pinnedPath = 'hmailserver/installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('hm-dotnet-pin-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null

function Get-Sha512OfBytes([byte[]] $bytes) {
   $algorithm = [System.Security.Cryptography.SHA512]::Create()
   try { return (($algorithm.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '') }
   finally { $algorithm.Dispose() }
}

function New-Bytes([string] $text) { return [System.Text.Encoding]::ASCII.GetBytes($text) }

# A manifest holding one entry, the runtime's, with the fields a case chooses.
function New-Manifest([string] $name, [string] $version, [string] $sha512, [string] $source, [string] $disposition = 'downloaded') {
   $file = Join-Path $work $name
   $entry = [ordered]@{ path = $pinnedPath; sha256 = $null; sha512 = $sha512; version = $version; disposition = $disposition; source = $source }
   $document = [ordered]@{ artifacts = @([pscustomobject]$entry) }
   Set-Content -LiteralPath $file -Value ($document | ConvertTo-Json -Depth 4) -Encoding ASCII
   return $file
}

function Invoke-GetRuntime([string[]] $arguments) {
   # Windows PowerShell 5.1 turns a native command's first line on stderr into a
   # terminating error when $ErrorActionPreference is Stop; the refusals this
   # test looks for arrive on stderr.
   $previous = $ErrorActionPreference
   $ErrorActionPreference = 'Continue'
   try {
      $output = & $shell -NoProfile -ExecutionPolicy Bypass -File $getRuntime @arguments 2>&1 | Out-String
      $code = $LASTEXITCODE
   }
   finally { $ErrorActionPreference = $previous }
   return [pscustomobject]@{ Exit = $code; Output = $output }
}

# ------------------------------------------------------------ the loopback server

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$state = [hashtable]::Synchronized(@{ Body = [byte[]]@(); Requests = 0 })
$server = [powershell]::Create()
[void] $server.AddScript({
   param($listener, $state)
   while ($true) {
      try { $client = $listener.AcceptTcpClient() } catch { break }
      try {
         $stream = $client.GetStream()
         $buffer = New-Object byte[] 4096
         $seen = ''
         while ($seen -notmatch "`r`n`r`n") {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { break }
            $seen += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
         }
         $body = [byte[]] $state.Body
         $head = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: application/octet-stream`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n")
         $stream.Write($head, 0, $head.Length)
         $stream.Write($body, 0, $body.Length)
         $stream.Flush()
         $state.Requests++
      }
      catch { }
      finally { $client.Close() }
   }
}).AddArgument($listener).AddArgument($state)
$serving = $server.BeginInvoke()

# ------------------------------------------------------------------- the cases

$pinned = New-Bytes 'the pinned runtime, as far as this test is concerned'
$wrong = New-Bytes 'a different file of the same name'
$old = New-Bytes 'an older runtime left by an earlier download'
$pinnedHash = Get-Sha512OfBytes $pinned
$version = '10.0.99'
$source = "http://127.0.0.1:$port/dotnet/WindowsDesktop/$version/windowsdesktop-runtime-$version-win-x64.exe"
$failures = 0

function Assert-Case([string] $name, [bool] $held, $result) {
   if ($held) {
      Write-Host ('  OK    {0}' -f $name)
   }
   else {
      Write-Host ('  FAIL  {0}' -f $name)
      if ($result) { Write-Host ('        exit {0}; output:{1}{2}' -f $result.Exit, [Environment]::NewLine, $result.Output.TrimEnd()) }
      $script:failures++
   }
}

Write-Host "get-dotnet-runtime.ps1 under $shell"

try {
   # 1. The committed pin is a pin, and -Verify reports a missing file as missing.
   $target = Join-Path $work 'missing.exe'
   $result = Invoke-GetRuntime @('-Verify', '-Path', $target)
   Assert-Case 'the committed pin is read and a missing file is reported' `
      ($result.Exit -eq 1 -and $result.Output -match 'is not there' -and $result.Output -notmatch 'is not a pin') $result

   # 2. The committed pin refuses a file with other bytes.
   $target = Join-Path $work 'wrong-committed.exe'
   [System.IO.File]::WriteAllBytes($target, $wrong)
   $result = Invoke-GetRuntime @('-Verify', '-Path', $target)
   Assert-Case 'the committed pin refuses a file whose SHA-512 is not the pinned one' `
      ($result.Exit -eq 1 -and $result.Output -match 'is not the pinned \.NET Desktop Runtime') $result

   $manifest = New-Manifest 'pin.json' $version $pinnedHash $source

   # 3. The negative control of 2: the same check accepts the pinned bytes.
   $target = Join-Path $work 'pinned-verify.exe'
   [System.IO.File]::WriteAllBytes($target, $pinned)
   $result = Invoke-GetRuntime @('-Verify', '-Path', $target, '-Manifest', $manifest)
   Assert-Case 'a file whose SHA-512 is the pinned one is accepted' ($result.Exit -eq 0 -and $result.Output -match '  OK  ') $result

   # 4. A download whose bytes do not match is refused, deleted and not placed.
   $target = Join-Path $work 'download-wrong.exe'
   $state.Body = $wrong
   $result = Invoke-GetRuntime @('-Path', $target, '-Manifest', $manifest)
   Assert-Case 'a download whose SHA-512 is not the pinned one is refused and nothing is placed' `
      ($result.Exit -ne 0 -and $result.Output -match 'does not match the pinned SHA-512' -and
       -not (Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath "$target.download")) $result

   # 5. A file already in place that is not the pinned one is not accepted: it is
   #    removed, so that when its replacement does not match either there is no
   #    runtime for the installer to package rather than the wrong one.
   $target = Join-Path $work 'stale.exe'
   [System.IO.File]::WriteAllBytes($target, $old)
   $state.Body = $wrong
   $result = Invoke-GetRuntime @('-Path', $target, '-Manifest', $manifest)
   Assert-Case 'a file in place that is not the pinned one is refused and removed, and nothing is placed when the download does not match' `
      ($result.Exit -ne 0 -and $result.Output -match 'is not the pinned \.NET Desktop Runtime' -and
       $result.Output -match 'does not match the pinned SHA-512' -and -not (Test-Path -LiteralPath $target)) $result

   # 6. The negative control of 4 and 5: the pinned bytes are downloaded and
   #    replace a stale file.
   [System.IO.File]::WriteAllBytes($target, $old)
   $state.Body = $pinned
   $result = Invoke-GetRuntime @('-Path', $target, '-Manifest', $manifest)
   $placed = (Test-Path -LiteralPath $target) -and ((Get-FileHash -LiteralPath $target -Algorithm SHA512).Hash.ToLowerInvariant() -eq $pinnedHash)
   Assert-Case 'a download whose SHA-512 is the pinned one replaces the stale file' ($result.Exit -eq 0 -and $placed) $result

   # 7. Pins that are not pins, refused before anything is downloaded.
   $requestsBefore = $state.Requests
   $cases = @(
      @{ Name = 'a moving link (aka.ms) is not a pin'; Manifest = (New-Manifest 'moving.json' $version $pinnedHash 'https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe') },
      @{ Name = 'a SHA-256 in place of the SHA-512 is not a pin'; Manifest = (New-Manifest 'sha256.json' $version (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant() $source) },
      @{ Name = 'a version outside the 10.0 channel is not a pin'; Manifest = (New-Manifest 'channel.json' '11.0.1' $pinnedHash "http://127.0.0.1:$port/11.0.1/windowsdesktop-runtime-11.0.1-win-x64.exe") },
      @{ Name = 'an entry of another kind is not a pin'; Manifest = (New-Manifest 'kind.json' $version $pinnedHash $source 'fetched') }
   )
   foreach ($case in $cases) {
      $target = Join-Path $work ('refused-' + [guid]::NewGuid().ToString('N') + '.exe')
      $result = Invoke-GetRuntime @('-Path', $target, '-Manifest', $case.Manifest)
      Assert-Case $case.Name ($result.Exit -ne 0 -and $result.Output -match 'is not a pin' -and -not (Test-Path -LiteralPath $target)) $result
   }
   Assert-Case 'nothing was downloaded for a pin that is not one' ($state.Requests -eq $requestsBefore) $null
}
finally {
   $listener.Stop()
   try { [void] $server.EndInvoke($serving) } catch { }
   $server.Dispose()
   Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures -eq 0) {
   Write-Host 'Every case held: the script places the pinned runtime and nothing else.'
   exit 0
}
Write-Host ("{0} case(s) failed." -f $failures)
exit 1
