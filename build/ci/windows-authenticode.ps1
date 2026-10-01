# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The release-authenticode job of .gitlab-ci.yml, on bench150: the Windows
   installer Authenticode-signed through Azure Artifact Signing, with no secret
   anywhere. The job's GitLab ID token (id_tokens AZURE_ID_TOKEN, audience
   api://AzureADTokenExchange) is what Microsoft Entra ID accepts, through a
   federated credential on the app registration the owner made, in place of a
   client secret: the token names this project and this tag, and only a
   protected v* tag's pipeline can have one. RELEASE.md, "Authenticode", says
   what the owner creates on the Azure side.

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
     4. the signature is Valid, carries the timestamp, and the version resource is
        the one the unsigned installer had; and it passes the pin every
        installation's Control Panel holds it to before its Install update runs
        it (Common/Util/AuthenticodePublisher.h): the signer's subject exactly
        CN=Progressive Robot Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire
        West and Chester, C=GB, a chain of four ending in Microsoft ID Verified
        Code Signing PCA 2021 and Microsoft Identity Verification Root
        Certificate Authority 2020 by their SHA-256, the code-signing EKU, and
        the file description naming its architecture, "(x64)" or "(ARM64)" - a
        release the Control Panel would refuse is never published. Then the
        file goes to -OutDir.
   The token file is deleted whatever happens.

   Exit codes: 0 signed; 2 a setting or a tool is missing; 3 Entra refused the
   token; 4 SignTool failed; 5 the signed file is not what was asked for.

   Usage:
     pwsh -NoProfile -File build\ci\windows-authenticode.ps1 -Installer <exe> -OutDir <dir>
#>

param(
   [Parameter(Mandatory)][string] $Installer,
   [Parameter(Mandatory)][string] $OutDir
)

$ErrorActionPreference = 'Continue'
$timestampUrl = 'http://timestamp.acs.microsoft.com'

function Stop-Signing([int] $code, [string] $sentence) {
   "NOT SIGNED: $sentence"
   "AUTHENTICODE EXIT=$code"
   exit $code
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
if (-not (Test-Path -LiteralPath $Installer)) { Stop-Signing 5 "$Installer is not here: the release job's artifact, which carries both installers, did not arrive" }
$Installer = (Resolve-Path -LiteralPath $Installer).ProviderPath
$name = Split-Path -Leaf $Installer
$versionBefore = (Get-Item -LiteralPath $Installer).VersionInfo.FileVersion
$hashBefore = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash

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
"signing $name ($hashBefore, version resource $versionBefore)"

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

   # 3. Sign, with the workload identity credential alone.
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

   $signing = Join-Path $work $name
   Copy-Item -LiteralPath $Installer -Destination $signing
   $env:AZURE_FEDERATED_TOKEN_FILE = $tokenFile
   $env:AZURE_AUTHORITY_HOST = 'https://login.microsoftonline.com/'
   & $signtool sign /v /fd SHA256 /tr $timestampUrl /td SHA256 /dlib $dlib /dmdf $metadata $signing 2>&1 | ForEach-Object { "  $_" }
   $code = $LASTEXITCODE
   Remove-Item Env:\AZURE_FEDERATED_TOKEN_FILE -ErrorAction SilentlyContinue
   if ($code -ne 0) { Stop-Signing 4 "SignTool failed (exit $code); the lines above say why" }

   # 4. What was made.
   $signature = Get-AuthenticodeSignature -LiteralPath $signing
   "status:      $($signature.Status)"
   "signer:      $($signature.SignerCertificate.Subject)"
   if ($signature.Status -ne 'Valid') { Stop-Signing 5 "the signed installer's signature is $($signature.Status), not Valid" }
   # Valid alone cannot see a missing timestamp: the certificate is minutes old. In
   # three days it expires, and a signature without a timestamp expires with it.
   if ($null -eq $signature.TimeStamperCertificate) { Stop-Signing 5 "the signature carries no RFC 3161 timestamp from $timestampUrl" }
   "timestamper: $($signature.TimeStamperCertificate.Subject)"
   $versionAfter = (Get-Item -LiteralPath $signing).VersionInfo.FileVersion
   if ($versionAfter -ne $versionBefore) { Stop-Signing 5 "the version resource changed in signing ($versionBefore, now $versionAfter)" }

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
   if (-not $sameSubject) { Stop-Signing 5 "the signer is $($certificate.Subject), not the publisher every installation's Install update is pinned to" }
   $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
   $chain.ChainPolicy.RevocationMode = 'NoCheck'
   [void] $chain.Build($certificate)
   $digests = @($chain.ChainElements | ForEach-Object { [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($_.Certificate.RawData)).Replace('-', '') })
   if ($digests.Count -ne 4 -or $digests[2] -ne '3D29798CC5D3F0644A7E0DC9CB1CADE523EA5EC83B335109B605BFEAA7D5F5C1' -or $digests[3] -ne '5367F20C7ADE0E2BCA790915056D086B720C33C1FA2A2661ACF787E3292E1270') {
      Stop-Signing 5 "the certificate chain ($($digests -join ' <- ')) does not end in Microsoft ID Verified Code Signing PCA 2021 and Microsoft Identity Verification Root Certificate Authority 2020"
   }
   if (-not ($certificate.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq '1.3.6.1.5.5.7.3.3' })) { Stop-Signing 5 "the certificate is not issued for code signing" }
   $description = (Get-Item -LiteralPath $signing).VersionInfo.FileDescription
   if ($description -notmatch '\((x64|ARM64)\)\s*$') { Stop-Signing 5 "the file description '$description' names no architecture, (x64) or (ARM64)" }
   "pin:         subject, PCA 2021 and Root 2020, code signing, '$description'"

   if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
   New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
   Copy-Item -LiteralPath $signing -Destination (Join-Path $OutDir $name)
   "$name  sha256 $((Get-FileHash -LiteralPath (Join-Path $OutDir $name) -Algorithm SHA256).Hash), signed and timestamped"
   "AUTHENTICODE EXIT=0"
   exit 0
} finally {
   Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
