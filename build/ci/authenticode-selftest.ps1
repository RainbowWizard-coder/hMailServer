# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The project's two Authenticode checks, tested on the Windows runner that runs them
   (the build-inputs-authenticode job, bench150, .gitlab/ci/packages-windows.yml),
   with no secret, no Azure and no certificate store.

   build\ci\windows-authenticode.ps1, step 4 - its Get-PinProblems and
   Get-BlockProblems, read out of the script by PowerShell's parser so that what is
   tested is what release-authenticode runs:
     * the tree's module, HmailServer.psd1 and HmailServer.psm1, signed by SignTool
       through Windows' PowerShell SIP with a throwaway certificate (never in a store,
       never trusted), whose key this script creates by a name of its own in the
       running account's software key storage provider, hands SignTool by that name
       (/csp, /kc, the certificate as a .cer) and deletes again, and the test checks
       it is gone - a PFX would leave a key behind on every run, since SignTool
       imports it into a key container it never deletes (seen 2 October 2026): each
       signed file is its own bytes with one signature block appended, which passes;
       the same with a byte of the script changed, with a line after the block, with
       a statement inside the block, and unsigned, are each refused;
     * the pin refuses that signature (its root is trusted nowhere), and refuses a
       Valid, timestamped signature by another publisher (SignTool's own, Microsoft's)
       for its signer and its chain;
     * -Signed <file>...: files a release signed (an installer, a module file) must
       pass the whole pin - the positive control, run where such a file is at hand.
   build\ci\build-inputs-authenticode.ps1, with the build inputs downloaded once
   (-From): the manifest as it is passes; a manifest recording another signer for the
   file recorded Valid, a NotSigned file as Valid, the Valid file as NotSigned, a
   fetched file with no record of its signature ("NotRecorded") or one spelt
   otherwise ("notsigned"), another SHA-256 for a build input, or no Valid file at all
   is refused, each for its reason.
   -SkipBuildInputs leaves this half out.

   Exit: 0 every case passes; 1 a case failed, each named.

   Usage:
     pwsh -NoProfile -File build\ci\authenticode-selftest.ps1 [-Signed <file>...] [-SkipBuildInputs]
#>
param(
   [string[]] $Signed,
   [switch] $SkipBuildInputs
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).ProviderPath
$shell = (Get-Process -Id $PID).Path
$failures = New-Object System.Collections.Generic.List[string]
function Expect($condition, [string] $what) {
   if ($condition) { "ok    $what" } else { "FAIL  $what"; $failures.Add($what) }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('hm-authenticode-selftest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
   # windows-authenticode.ps1's step 4, as the script has it.
   $script = Join-Path $PSScriptRoot 'windows-authenticode.ps1'
   $parseErrors = $null
   $ast = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$parseErrors)
   Expect (-not $parseErrors.Count) "windows-authenticode.ps1 parses"
   foreach ($statement in $ast.EndBlock.Statements) {
      $isFunction = $statement -is [Management.Automation.Language.FunctionDefinitionAst] -and $statement.Name -in 'Get-PinProblems', 'Get-BlockProblems'
      $isSetting = $statement -is [Management.Automation.Language.AssignmentStatementAst] -and
         $statement.Left.Extent.Text -in '$timestampUrl', '$moduleFiles', '$blockBegin', '$blockEnd'
      if ($isFunction -or $isSetting) { . ([scriptblock]::Create($statement.Extent.Text)) }
   }
   Expect ((Get-Command Get-PinProblems -ErrorAction SilentlyContinue) -and (Get-Command Get-BlockProblems -ErrorAction SilentlyContinue) -and
      $moduleFiles.Count -eq 2) "Get-PinProblems, Get-BlockProblems and the module's two names are read out of windows-authenticode.ps1"

   $signtool = $env:HM_SIGNTOOL
   if (-not $signtool) {
      $signtool = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Directory -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending |
         ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
   }
   Expect ([bool]$signtool) "the Windows SDK's x64 signtool.exe is here ($signtool)"

   # A throwaway code-signing certificate. Its key is made by a name of this test's own
   # in the running account's software key storage provider, handed to SignTool by that
   # name with the certificate as a .cer, and deleted as soon as SignTool is done:
   # SignTool imports a PFX into a key container it never deletes, which would leave a
   # key behind on the runner at every run.
   $cli = Join-Path $repo 'hmailserver\source\Tools\Cli'
   $module = Join-Path $work 'module'
   New-Item -ItemType Directory -Path $module | Out-Null
   foreach ($name in $moduleFiles) { Copy-Item -LiteralPath (Join-Path $cli $name) -Destination (Join-Path $module $name) }
   $targets = @($moduleFiles | ForEach-Object { Join-Path $module $_ })
   $provider = [Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
   $keyName = 'hm-authenticode-selftest-' + [Guid]::NewGuid().ToString('N')
   $keyParameters = [Security.Cryptography.CngKeyCreationParameters]::new()
   $keyParameters.Provider = $provider
   $keyParameters.Parameters.Add([Security.Cryptography.CngProperty]::new('Length', [BitConverter]::GetBytes([int]3072),
         [Security.Cryptography.CngPropertyOptions]::None))
   $key = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::Rsa, $keyName, $keyParameters)
   $signedBy = -1
   try {
      $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=hMailServer self-test, O=Not a publisher',
         [Security.Cryptography.RSACng]::new($key), [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
      $usages = [Security.Cryptography.OidCollection]::new()
      [void] $usages.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
      $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usages, $false))
      $certificate = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddDays(1))
      $cer = Join-Path $work 'throwaway.cer'
      [IO.File]::WriteAllBytes($cer, $certificate.Export('Cert'))
      & $signtool sign /fd SHA256 /f $cer /csp $provider.Provider /kc $keyName @targets 2>&1 | Out-Null
      $signedBy = $LASTEXITCODE
   } finally {
      $key.Delete()
      $key.Dispose()
   }
   Expect ($signedBy -eq 0) "SignTool signs both module files with the throwaway certificate (exit $signedBy)"
   Expect (-not [Security.Cryptography.CngKey]::Exists($keyName, $provider)) "the throwaway key is deleted once SignTool is done: the runner keeps no key of this test"

   foreach ($name in $moduleFiles) {
      $unsigned = [IO.File]::ReadAllBytes((Join-Path $cli $name))
      $signedBytes = [IO.File]::ReadAllBytes((Join-Path $module $name))
      Expect (@(Get-BlockProblems $unsigned $signedBytes $name).Count -eq 0) "$name as SignTool signed it is its own bytes and one signature block"
      $changed = [byte[]]$signedBytes.Clone()
      $changed[[int]($unsigned.Length / 2)] = $changed[[int]($unsigned.Length / 2)] -bxor 1
      Expect ((Get-BlockProblems $unsigned $changed $name) -match 'does not begin with its own unsigned bytes') "$name with a byte of its script changed is refused"
      $after = [byte[]]($signedBytes + [Text.Encoding]::ASCII.GetBytes("Write-Host 1`r`n"))
      Expect ((Get-BlockProblems $unsigned $after $name) -match 'followed by one signature block') "$name with a line after its block is refused"
      $text = [Text.Encoding]::ASCII.GetString($signedBytes)
      $inside = $text.Substring(0, $text.Length - $blockEnd.Length) + "Write-Host 1`r`n" + $blockEnd
      Expect ((Get-BlockProblems $unsigned ([Text.Encoding]::ASCII.GetBytes($inside)) $name) -match 'not the signature''s base64') "$name with a statement inside its block is refused"
      Expect ((Get-BlockProblems $unsigned $unsigned $name) -match 'no longer than') "$name unsigned is refused"
      $pin = @(Get-PinProblems (Join-Path $module $name) 6>$null)
      Expect (($pin -join ' ') -match 'not Valid') "the pin refuses $name signed by a certificate no machine trusts: $($pin -join '; ')"
      # What the signed file does when PowerShell's own check reads it: the block is a
      # signature of the script, and a byte changed after signing breaks it.
      $tampered = Join-Path $work "tampered-$name"
      [IO.File]::WriteAllBytes($tampered, $changed)
      Expect ((Get-AuthenticodeSignature -LiteralPath $tampered).Status -eq 'HashMismatch') "$name changed after signing reads as HashMismatch"
   }

   $other = @(Get-PinProblems $signtool 6>$null)
   Expect ((Get-AuthenticodeSignature -LiteralPath $signtool).Status -eq 'Valid' -and ($other -join ' ') -match 'not the publisher' -and
      ($other -join ' ') -match 'does not end in Microsoft ID Verified') "the pin refuses a Valid, timestamped signature by another publisher (signtool.exe): $($other -join '; ')"

   # A release's file: the signer, the chain, the code-signing EKU and the timestamp.
   # An installer before 6.3.6 has no architecture in its file description (the marker
   # came with 6.3.6's ARM64 installer), so that part alone is left out for one.
   foreach ($file in $Signed) {
      $found = @(Get-PinProblems (Resolve-Path -LiteralPath $file).ProviderPath 6>$null | Where-Object { $_ -notmatch 'names no architecture' })
      Expect ($found.Count -eq 0) "$(Split-Path -Leaf $file), signed by a release, passes the pin's signer, chain, code-signing and timestamp checks: $($found -join '; ')"
   }

   if (-not $SkipBuildInputs) {
      # build-inputs-authenticode.ps1, the inputs downloaded once.
      $checker = Join-Path $PSScriptRoot 'build-inputs-authenticode.ps1'
      $manifestPath = Join-Path $repo 'hmailserver\docs\third-party-binaries.json'
      $inputs = Join-Path $work 'inputs'
      New-Item -ItemType Directory -Path $inputs | Out-Null
      $run = 0
      function Invoke-Checker($manifestObject, [string] $what) {
         $script:run++
         $path = Join-Path $work "manifest-$script:run.json"
         $manifestObject | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding utf8
         $said = & $shell -NoProfile -ExecutionPolicy Bypass -File $checker -Manifest $path -WorkDir (Join-Path $work "check-$script:run") -From $inputs 2>&1 | Out-String
         return @{ Code = $LASTEXITCODE; Said = $said }
      }
      $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
      foreach ($buildInput in $manifest.build_inputs) {
         $url = if ([string]$buildInput.release -match '^build-inputs-(\d+)$') { "https://gitlab.com/api/v4/projects/86730559/packages/generic/build-inputs/$($Matches[1])/$($buildInput.asset)" } else { "$($buildInput.release)/$($buildInput.asset)" }
         Invoke-WebRequest -Uri $url -OutFile (Join-Path $inputs $buildInput.asset) -UseBasicParsing -MaximumRedirection 0
      }
      $result = Invoke-Checker $manifest 'as it is'
      Expect ($result.Code -eq 0) "the manifest as it is passes, the inputs taken from -From: $(if ($result.Code) { $result.Said })"

      $valid = @($manifest.artifacts | Where-Object { $_.disposition -eq 'fetched' -and $_.authenticode -eq 'Valid' })[0]
      $unsignedOne = @($manifest.artifacts | Where-Object { $_.disposition -eq 'fetched' -and $_.authenticode -eq 'NotSigned' })[0]
      $cases = @(
         @{ What = "another signer recorded for $($valid.path)"; Needle = 'and the manifest records'; Change = { param($m) ($m.artifacts | Where-Object { $_.path -eq $valid.path }).authenticode_signer = 'CN=Somebody Else' } },
         @{ What = "$($unsignedOne.path), unsigned, recorded Valid"; Needle = 'signature NotSigned, and the manifest records Valid'; Change = { param($m) $a = $m.artifacts | Where-Object { $_.path -eq $unsignedOne.path }; $a.authenticode = 'Valid'; $a | Add-Member -Force NoteProperty authenticode_signer 'CN=Nobody' } },
         @{ What = "$($valid.path), signed, recorded NotSigned"; Needle = 'and the manifest records NotSigned'; Change = { param($m) ($m.artifacts | Where-Object { $_.path -eq $valid.path }).authenticode = 'NotSigned' } },
         @{ What = "$($unsignedOne.path), fetched, with no record of its signature"; Needle = "records its signature as 'NotRecorded', neither Valid nor NotSigned"; Change = { param($m) ($m.artifacts | Where-Object { $_.path -eq $unsignedOne.path }).authenticode = 'NotRecorded' } },
         @{ What = "$($unsignedOne.path), fetched, recorded 'notsigned'"; Needle = "records its signature as 'notsigned', neither Valid nor NotSigned"; Change = { param($m) ($m.artifacts | Where-Object { $_.path -eq $unsignedOne.path }).authenticode = 'notsigned' } },
         @{ What = 'another SHA-256 pinned for a build input'; Needle = 'and the manifest pins'; Change = { param($m) $m.build_inputs[1].sha256 = ('0' * 64) } },
         @{ What = 'no fetched file recorded Valid'; Needle = 'would check no signature'; Change = { param($m) $m.artifacts | Where-Object { $_.authenticode -eq 'Valid' -and $_.disposition -eq 'fetched' } | ForEach-Object { $_.authenticode = 'NotRecorded' } } }
      )
      foreach ($case in $cases) {
         $copy = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
         & $case.Change $copy
         $result = Invoke-Checker $copy $case.What
         Expect ($result.Code -eq 1 -and $result.Said -match [regex]::Escape($case.Needle)) "refused: $($case.What)$(if ($result.Code -ne 1) { " (exit $($result.Code))" })"
      }
   }
} finally {
   Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

""
if ($failures.Count) { "authenticode-selftest: $($failures.Count) case(s) failed"; exit 1 }
"authenticode-selftest: every case passed."
exit 0
