# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The release-authenticode job of .gitlab-ci.yml, on bench150: the Windows
   installers and the PowerShell module Authenticode-signed through Azure Artifact
   Signing, with no secret anywhere. The job's GitLab ID token (id_tokens
   AZURE_ID_TOKEN, audience api://AzureADTokenExchange) is what Microsoft Entra ID
   accepts, through a federated credential on the app registration the owner made,
   in place of a client secret: the token names this project and this tag, and only
   a protected v* tag's pipeline can have one. RELEASE.md, "Authenticode", says what
   the owner creates on the Azure side.

   What it reads, as CI/CD variables of the project (names only are ever printed):
     AZURE_TENANT_ID, AZURE_CLIENT_ID    the app registration holding the Artifact
                                         Signing Certificate Profile Signer role
     ARTIFACT_SIGNING_ENDPOINT           https://neu.codesigning.azure.net
     ARTIFACT_SIGNING_ACCOUNT, ARTIFACT_SIGNING_PROFILE
   and on the machine, installed once: Microsoft's Artifact Signing Client Tools,
   installed for the machine with msiexec and ALLUSERS=1 into
   C:\Program Files\Microsoft\ArtifactSigningClientTools (winget's --scope machine
   finds no installer, and without it the MSI goes into the administrator's
   profile, which LocalSystem does not see; docs\ContinuousIntegration.md,
   "bench150", has the command), which bring SignTool's Artifact Signing plugin
   (Azure.CodeSigning.Dlib.dll), run on the machine's .NET 10 through
   rollForward Major; and SignTool from the Windows SDK the bench's Build Tools
   installed. HM_SIGNTOOL and HM_SIGNING_DLIB name either where it is somewhere
   else.

   What it signs, one call each:
     -Installer <exe>   a Windows installer, x64 or ARM64;
     -Module <dir>      the PowerShell module's manifest and script,
                        HmailServer.psd1 and HmailServer.psm1, which <dir> holds as
                        the release job collected them from the tagged tree,
                        unsigned (build/ci/release-chain.py, MODULE_FILES). Both are
                        signed by one SignTool call, through Windows' PowerShell SIP,
                        which appends a signature block to each script and changes
                        nothing of it; the job holds each to that.
   And -Check <file> signs nothing and needs no setting: it holds a file already
   signed - an installer or a module file of a release - to step 4 below, as a
   person checks a download (exit 0, or 5 naming what fails).

   The steps:
     1. every setting is there, or the job stops naming the missing ones, before
        anything is sent anywhere;
     2. the token is written to a file only SYSTEM and the Administrators can
        read, and exchanged once with Entra ID for an Artifact Signing token, only
        to see that the exchange works: when the federated credential does not
        match, Entra's own answer (AADSTS...) is printed, which names the subject
        it was shown, and nothing is signed;
     3. SignTool signs with the plugin, whose DefaultAzureCredential takes the
        same token file through its workload identity credential
        (AZURE_FEDERATED_TOKEN_FILE), every other credential excluded, with the
        service's RFC 3161 timestamp - not optional: the certificate lives about
        three days, and a signature without a timestamp stops verifying with it;
     4. each signature is Valid and carries the timestamp, and it passes the pin
        every installation's Control Panel holds an installer to before its
        Install update runs it (Common/Util/AuthenticodePublisher.h): the signer's
        subject exactly CN=Progressive Robot Ltd, O=Progressive Robot Ltd,
        L=Chester, S=Cheshire West and Chester, C=GB, a chain of four ending in
        Microsoft ID Verified Code Signing PCA 2021 and Microsoft Identity
        Verification Root Certificate Authority 2020 by their SHA-256, and the
        code-signing EKU. An installer's version resource is the one the unsigned
        installer had, and its file description names its architecture, "(x64)"
        or "(ARM64)" - a release the Control Panel would refuse is never
        published. A module file is its unsigned bytes, unchanged, followed by one
        signature block. Then the files go to -OutDir.
   The token file is deleted whatever happens.

   Exit codes: 0 signed (or, with -Check, every file passes); 2 a setting or a tool
   is missing; 3 Entra refused the token; 4 SignTool failed; 5 a signed file is not
   what was asked for; 6 an error stopped the signing part-way (a file that could
   not be written or read), named - never 0 with -OutDir unwritten.

   Usage:
     pwsh -NoProfile -File build\ci\windows-authenticode.ps1 -Installer <exe> -OutDir <dir>
     pwsh -NoProfile -File build\ci\windows-authenticode.ps1 -Module <dir> -OutDir <dir>
     pwsh -NoProfile -File build\ci\windows-authenticode.ps1 -Check <file>
#>

[CmdletBinding(DefaultParameterSetName = 'Installer')]
param(
   [Parameter(Mandatory, ParameterSetName = 'Installer')][string] $Installer,
   [Parameter(Mandatory, ParameterSetName = 'Module')][string] $Module,
   [Parameter(Mandatory, ParameterSetName = 'Installer')]
   [Parameter(Mandatory, ParameterSetName = 'Module')][string] $OutDir,
   [Parameter(Mandatory, ParameterSetName = 'Check')][string[]] $Check
)

$ErrorActionPreference = 'Continue'
$timestampUrl = 'http://timestamp.acs.microsoft.com'
# The module's two files (build/ci/release-chain.py, MODULE_FILES), and what SignTool's
# PowerShell SIP writes around the signature it appends to each.
$moduleFiles = 'HmailServer.psd1', 'HmailServer.psm1'
$blockBegin = "`r`n# SIG # Begin signature block`r`n"
$blockEnd = "# SIG # End signature block`r`n"

function Stop-Signing([int] $code, [string] $sentence) {
   "NOT SIGNED: $sentence"
   "AUTHENTICODE EXIT=$code"
   exit $code
}

# Step 4, for one signed file: what is wrong with it, as sentences; none when it passes.
# An installer (.exe) is held to its architecture's file description too.
function Get-PinProblems([string] $path) {
   $problems = @()
   $signature = Get-AuthenticodeSignature -LiteralPath $path
   $leaf = Split-Path -Leaf $path
   "  ${leaf}: $($signature.Status), signer $($signature.SignerCertificate.Subject)" | Write-Host
   if ($signature.Status -ne 'Valid') { return @("$leaf's signature is $($signature.Status), not Valid") }
   # Valid alone cannot see a missing timestamp: the certificate is minutes old. In
   # three days it expires, and a signature without a timestamp expires with it.
   if ($null -eq $signature.TimeStamperCertificate) { $problems += "$leaf's signature carries no RFC 3161 timestamp from $timestampUrl" }
   else { "  timestamper: $($signature.TimeStamperCertificate.Subject)" | Write-Host }

   # The pin of an administrator's install (Common/Util/AuthenticodePublisher.h),
   # the same five attributes, the same two certificates and the same marker, so
   # that no release is published that every 6.3.6 and later Control Panel would
   # refuse to install.
   $certificate = $signature.SignerCertificate
   $subject = @{}
   foreach ($part in ($certificate.SubjectName.Format($true) -split "`r?`n")) {
      if ($part -match '^\s*([A-Za-z]+)=(.*)$') { $subject[$Matches[1]] = $Matches[2].Trim() }
   }
   $expected = @{ 'CN' = 'Progressive Robot Ltd'; 'O' = 'Progressive Robot Ltd'; 'L' = 'Chester'; 'S' = 'Cheshire West and Chester'; 'C' = 'GB' }
   $sameSubject = $subject.Count -eq $expected.Count
   foreach ($key in $expected.Keys) { if ($subject[$key] -ne $expected[$key]) { $sameSubject = $false } }
   if (-not $sameSubject) { $problems += "$leaf's signer is $($certificate.Subject), not the publisher every installation's Install update is pinned to" }
   $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
   $chain.ChainPolicy.RevocationMode = 'NoCheck'
   [void] $chain.Build($certificate)
   $digests = @($chain.ChainElements | ForEach-Object { [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($_.Certificate.RawData)).Replace('-', '') })
   if ($digests.Count -ne 4 -or $digests[2] -ne '3D29798CC5D3F0644A7E0DC9CB1CADE523EA5EC83B335109B605BFEAA7D5F5C1' -or $digests[3] -ne '5367F20C7ADE0E2BCA790915056D086B720C33C1FA2A2661ACF787E3292E1270') {
      $problems += "$leaf's certificate chain ($($digests -join ' <- ')) does not end in Microsoft ID Verified Code Signing PCA 2021 and Microsoft Identity Verification Root Certificate Authority 2020"
   }
   if (-not ($certificate.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq '1.3.6.1.5.5.7.3.3' })) { $problems += "$leaf's certificate is not issued for code signing" }
   if ($leaf -like '*.exe') {
      $description = (Get-Item -LiteralPath $path).VersionInfo.FileDescription
      if ($description -notmatch '\((x64|ARM64)\)\s*$') { $problems += "$leaf's file description '$description' names no architecture, (x64) or (ARM64)" }
      else { "  pin: subject, PCA 2021 and Root 2020, code signing, '$description'" | Write-Host }
   } elseif (-not $problems.Count) {
      "  pin: subject, PCA 2021 and Root 2020, code signing" | Write-Host
   }
   return $problems
}

# A module file as signed: its unsigned bytes, unchanged, then one signature block -
# '# SIG # Begin signature block', lines of base64 behind '# ', '# SIG # End signature
# block' - and nothing else (build/ci/release-chain.py, module_signature_problems, holds
# the files to the same rule again in release-sbom).
function Get-BlockProblems([byte[]] $unsigned, [byte[]] $signed, [string] $leaf) {
   $ascii = [Text.Encoding]::ASCII
   if ($signed.Length -le $unsigned.Length) { return @("$leaf as signed is no longer than it was unsigned") }
   for ($i = 0; $i -lt $unsigned.Length; $i++) {
      if ($signed[$i] -ne $unsigned[$i]) { return @("$leaf as signed does not begin with its own unsigned bytes: signing changed the script at byte $i") }
   }
   $block = $ascii.GetString($signed, $unsigned.Length, $signed.Length - $unsigned.Length)
   if (-not $block.StartsWith($blockBegin, [StringComparison]::Ordinal) -or -not $block.EndsWith($blockEnd, [StringComparison]::Ordinal)) {
      return @("$leaf as signed is not its own bytes followed by one signature block")
   }
   $lines = $block.Substring($blockBegin.Length, $block.Length - $blockBegin.Length - $blockEnd.Length) -split "`r`n"
   if ($lines.Count -lt 2 -or $lines[-1] -ne '' -or @($lines[0..($lines.Count - 2)] | Where-Object { $_ -cnotmatch '^# [A-Za-z0-9+/=]+$' }).Count) {
      return @("$leaf as signed has something in its signature block that is not the signature's base64")
   }
   return @()
}

# -Check: files signed already, held to step 4 alone. Nothing is sent anywhere.
if ($PSCmdlet.ParameterSetName -eq 'Check') {
   $failed = 0
   foreach ($file in $Check) {
      if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { "CHECK FAILED: $file is not here"; $failed++; continue }
      $found = @(Get-PinProblems (Resolve-Path -LiteralPath $file).ProviderPath)
      foreach ($problem in $found) { "CHECK FAILED: $problem" }
      if ($found.Count) { $failed++ } else { "CHECKED: $(Split-Path -Leaf $file) is signed as a release is" }
   }
   "AUTHENTICODE CHECK EXIT=$(if ($failed) { 5 } else { 0 })"
   exit $(if ($failed) { 5 } else { 0 })
}

# 1. The settings, all or nothing.
$settings = 'AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'ARTIFACT_SIGNING_ENDPOINT', 'ARTIFACT_SIGNING_ACCOUNT', 'ARTIFACT_SIGNING_PROFILE'
$missing = @($settings | Where-Object { -not ([Environment]::GetEnvironmentVariable($_)) -or -not ([Environment]::GetEnvironmentVariable($_)).Trim() })
if ($missing.Count) {
   Stop-Signing 2 (("the CI/CD variables {0} are not set for this pipeline. They are protected variables of the project, which a protected v* tag's " +
      "pipeline reads (RELEASE.md, 'Authenticode'). Nothing was sent to Azure.") -f ($missing -join ', '))
}
if (-not $env:AZURE_ID_TOKEN) { Stop-Signing 2 'the job has no AZURE_ID_TOKEN: its id_tokens with the audience api://AzureADTokenExchange is missing' }
if ($env:ARTIFACT_SIGNING_ENDPOINT -notmatch '^https://[a-z0-9]+\.codesigning\.azure\.net/?$') {
   Stop-Signing 2 "ARTIFACT_SIGNING_ENDPOINT is not a region's Artifact Signing endpoint (https://<region>.codesigning.azure.net)"
}

# What is signed: the installer, or the module's two files, each by its full path.
if ($PSCmdlet.ParameterSetName -eq 'Installer') {
   if (-not (Test-Path -LiteralPath $Installer)) { Stop-Signing 5 "$Installer is not here: the release job's artifact, which carries both installers, did not arrive" }
   $sources = @((Resolve-Path -LiteralPath $Installer).ProviderPath)
   $versionBefore = (Get-Item -LiteralPath $sources[0]).VersionInfo.FileVersion
   "signing $(Split-Path -Leaf $sources[0]) ($((Get-FileHash -LiteralPath $sources[0] -Algorithm SHA256).Hash), version resource $versionBefore)"
} else {
   $sources = @()
   foreach ($file in $moduleFiles) {
      $path = Join-Path $Module $file
      if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Stop-Signing 5 "$path is not here: the release job collects the module's two files from the tagged tree" }
      $path = (Resolve-Path -LiteralPath $path).ProviderPath
      if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)).Contains($blockBegin.Trim())) {
         Stop-Signing 5 "$file carries a signature block already: the tree holds the module unsigned, and the release signs it"
      }
      $sources += $path
      "signing $file ($((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash))"
   }
}

# The tools.
$signtool = $env:HM_SIGNTOOL
if (-not $signtool) {
   $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
   $signtool = Get-ChildItem -LiteralPath $kits -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending |
      ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $signtool -or -not (Test-Path -LiteralPath $signtool)) {
   Stop-Signing 2 'there is no x64 signtool.exe from the Windows SDK on this machine (or set HM_SIGNTOOL)'
}
$dlib = $env:HM_SIGNING_DLIB
if (-not $dlib) {
   $roots = @(${env:ProgramFiles(x86)}, $env:ProgramFiles) | ForEach-Object {
      Join-Path $_ 'Microsoft\ArtifactSigningClientTools'
      Join-Path $_ 'Microsoft\Trusted Signing Client Tools'
   }
   $dlib = $roots | Where-Object { Test-Path -LiteralPath $_ } |
      ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' -File -ErrorAction SilentlyContinue } |
      Sort-Object { if ($_.FullName -match '\\x64\\') { 0 } else { 1 } } | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $dlib -or -not (Test-Path -LiteralPath $dlib)) {
   Stop-Signing 2 ('the Artifact Signing plugin is not installed on this runner. Install it once, as an administrator on bench150, for the machine: ' +
      'msiexec /i ArtifactSigningClientTools.msi TARGETDIR="C:\Program Files\Microsoft\ArtifactSigningClientTools\" ALLUSERS=1 /qn ' +
      '(winget --scope machine finds no installer, and without it the MSI goes into the administrator''s profile, which LocalSystem does not see; ' +
      'docs\ContinuousIntegration.md, ''bench150'') - or set HM_SIGNING_DLIB to Azure.CodeSigning.Dlib.dll')
}
"signtool: $signtool"
"plugin:   $dlib"

$work = Join-Path ([IO.Path]::GetTempPath()) ("hm-authenticode-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
   # 2. The token in a file only SYSTEM and the Administrators can read.
   $acl = New-Object Security.AccessControl.DirectorySecurity
   $acl.SetAccessRuleProtection($true, $false)
   foreach ($sid in 'S-1-5-18', 'S-1-5-32-544') {
      $who = (New-Object Security.Principal.SecurityIdentifier $sid)
      $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule $who, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
   }
   Set-Acl -LiteralPath $work -AclObject $acl
   $tokenFile = Join-Path $work 'gitlab-id-token'
   [IO.File]::WriteAllText($tokenFile, $env:AZURE_ID_TOKEN)

   $form = @{
      client_id             = $env:AZURE_CLIENT_ID
      scope                 = 'https://codesigning.azure.net/.default'
      grant_type            = 'client_credentials'
      client_assertion_type = 'urn:ietf:params:oauth:client-assertion-type:jwt-bearer'
      client_assertion      = $env:AZURE_ID_TOKEN
   }
   try {
      $answer = Invoke-RestMethod -Method Post -Uri "https://login.microsoftonline.com/$($env:AZURE_TENANT_ID)/oauth2/v2.0/token" -Body $form -ContentType 'application/x-www-form-urlencoded'
      if (-not $answer.access_token) { Stop-Signing 3 'Entra ID answered without a token' }
      "Entra ID takes this pipeline's token for the app registration (the answer is not kept)"
      $answer = $null
   } catch {
      $detail = $_.ErrorDetails.Message
      try { $parsed = $detail | ConvertFrom-Json; $detail = "$($parsed.error): $($parsed.error_description)" } catch { }
      Stop-Signing 3 (("Entra ID refused this pipeline's token: {0}`nThe federated credential of the app registration must match the token: issuer https://gitlab.com, " +
         "audience api://AzureADTokenExchange, and the subject project_path:hmailserver/hmailserver:ref_type:tag:ref:{1} (RELEASE.md, 'Authenticode').") -f $detail, $env:CI_COMMIT_TAG)
   }

   # 3. Sign, with the workload identity credential alone: every file of this call in
   # one SignTool run.
   $metadata = Join-Path $work 'metadata.json'
   @{
      Endpoint               = $env:ARTIFACT_SIGNING_ENDPOINT.TrimEnd('/')
      CodeSigningAccountName = $env:ARTIFACT_SIGNING_ACCOUNT
      CertificateProfileName = $env:ARTIFACT_SIGNING_PROFILE
      CorrelationId          = "gitlab-pipeline-$($env:CI_PIPELINE_ID)-job-$($env:CI_JOB_ID)"
      ExcludeCredentials     = @('EnvironmentCredential', 'ManagedIdentityCredential', 'SharedTokenCacheCredential', 'VisualStudioCredential',
                                 'VisualStudioCodeCredential', 'AzureCliCredential', 'AzurePowerShellCredential', 'AzureDeveloperCliCredential',
                                 'InteractiveBrowserCredential')
   } | ConvertTo-Json | Set-Content -LiteralPath $metadata -Encoding utf8

   $signing = @()
   foreach ($source in $sources) {
      $copy = Join-Path $work (Split-Path -Leaf $source)
      Copy-Item -LiteralPath $source -Destination $copy -ErrorAction Stop
      $signing += $copy
   }
   $env:AZURE_FEDERATED_TOKEN_FILE = $tokenFile
   $env:AZURE_AUTHORITY_HOST = 'https://login.microsoftonline.com/'
   & $signtool sign /v /fd SHA256 /tr $timestampUrl /td SHA256 /dlib $dlib /dmdf $metadata @signing 2>&1 | ForEach-Object { "  $_" }
   $code = $LASTEXITCODE
   Remove-Item Env:\AZURE_FEDERATED_TOKEN_FILE -ErrorAction SilentlyContinue
   if ($code -ne 0) { Stop-Signing 4 "SignTool failed (exit $code); the lines above say why" }

   # 4. What was made: each file held to the pin, an installer to its version resource
   # and a module file to its unsigned bytes.
   $problems = @()
   for ($i = 0; $i -lt $sources.Count; $i++) {
      $problems += @(Get-PinProblems $signing[$i])
      $leaf = Split-Path -Leaf $sources[$i]
      if ($PSCmdlet.ParameterSetName -eq 'Installer') {
         $versionAfter = (Get-Item -LiteralPath $signing[$i]).VersionInfo.FileVersion
         if ($versionAfter -ne $versionBefore) { $problems += "the version resource changed in signing ($versionBefore, now $versionAfter)" }
      } else {
         $problems += @(Get-BlockProblems ([IO.File]::ReadAllBytes($sources[$i])) ([IO.File]::ReadAllBytes($signing[$i])) $leaf)
      }
   }
   if ($problems.Count) { Stop-Signing 5 ($problems -join '; ') }

   if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force -ErrorAction Stop }
   New-Item -ItemType Directory -Force -Path $OutDir -ErrorAction Stop | Out-Null
   foreach ($file in $signing) {
      $name = Split-Path -Leaf $file
      Copy-Item -LiteralPath $file -Destination (Join-Path $OutDir $name) -ErrorAction Stop
      "$name  sha256 $((Get-FileHash -LiteralPath (Join-Path $OutDir $name) -Algorithm SHA256).Hash), signed and timestamped"
   }
   "AUTHENTICODE EXIT=0"
   exit 0
} catch {
   # An exception here - a .NET call that throws, a copy that fails - would otherwise
   # end the script after the finally below with exit code 0 and -OutDir unwritten
   # (seen with pwsh 7 on 2 October 2026).
   Stop-Signing 6 "the signing stopped on an error, and nothing -OutDir holds is to be used (a copy into it may have stopped part-way): $($_.Exception.Message)"
} finally {
   Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
