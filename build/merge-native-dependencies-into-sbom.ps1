# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Adds the components Syft cannot see to a Syft-produced SBOM, in place.
#
# Syft scans the repository and reports what it finds there. Boost, OpenSSL and libpq are
# built outside the tree, so it finds nothing of them, and the shipped SBOM described a
# mail server with no TLS library. This adds them - and, since 25 September 2026, every
# other component build/native-dependencies.ps1 lists: the vendored zlib and QuickJS, what
# the Windows installer carries with the hash each is pinned by, and the Linux packages'
# system libraries - in whichever of the two formats the file is. The linux-sbom job
# (build/ci/linux-sbom.sh) produces SPDX 2.3 and CycloneDX, and both go out with the release.
#
# Each component is added under an identifier made from its id - SPDXRef-Package-native-<id>
# in SPDX, hmailserver-native-<id> as the CycloneDX bom-ref - which is what
# build/check-sbom-inputs.py --sbom finds it by. Its hashes go in the package's checksums
# (SPDX) or the component's hashes (CycloneDX); a component with no version, such as a
# Linux distribution's library, is added without one rather than with a guess.
#
# Written to be safe to run twice: a package already present by PURL is left alone rather
# than duplicated, so a re-run of the job cannot produce an SBOM claiming OpenSSL
# twice. Written to fail loudly on an unrecognised file, because an SBOM this step silently
# declined to touch is exactly the failure it exists to fix.

[CmdletBinding()]
param(
   [Parameter(Mandatory = $true)] [string] $SbomFile,
   [string] $DependencyFile
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $SbomFile)) { throw "SBOM not found: $SbomFile" }

$dependencies =
   if ($DependencyFile) { Get-Content $DependencyFile -Raw | ConvertFrom-Json }
   else { & (Join-Path $PSScriptRoot 'native-dependencies.ps1') | ConvertFrom-Json }
$dependencies = @($dependencies)

if ($dependencies.Count -eq 0) {
   throw "No native dependencies were produced; refusing to leave the SBOM as it is."
}

$sbom = Get-Content $SbomFile -Raw | ConvertFrom-Json

$isSpdx      = $null -ne $sbom.spdxVersion
$isCycloneDx = $null -ne $sbom.bomFormat -and $sbom.bomFormat -eq 'CycloneDX'

if (-not $isSpdx -and -not $isCycloneDx) {
   throw "$SbomFile is neither SPDX (no spdxVersion) nor CycloneDX (no bomFormat). Refusing to guess."
}

# A component list written before ids and kinds existed still merges: its name is its id,
# and a vendored path is what made it vendored.
function Get-Id($dependency) { if ($dependency.id) { [string] $dependency.id } else { [string] $dependency.name } }

function Get-Kind($dependency) {
   if ($dependency.kind) { return [string] $dependency.kind }
   if ($dependency.vendored) { return 'vendored' }
   return 'linked'
}

function Get-Comment($dependency) {
   $text = switch (Get-Kind $dependency) {
      'vendored' { 'Native dependency vendored as source in the repository at {0}: {1}. Version taken from {2}.' -f $dependency.vendored, $dependency.why, $dependency.source }
      'shipped'  { 'Carried by the Windows installer, not present in the repository: {0}. Pinned in {1}.' -f $dependency.why, $dependency.source }
      'system'   { 'The Linux packages link the distribution''s library, not one of the repository''s: {0}. Not pinned, so no version is given: the one a package was built against is in its build log. Named because {1}.' -f $dependency.why, $dependency.source }
      default    { 'Linked native dependency, not present in the repository: {0}. Version taken from {1}.' -f $dependency.why, $dependency.source }
   }
   if ($dependency.licenseName) { $text += ' Licence: {0}.' -f $dependency.licenseName }
   return $text
}

$added = 0

if ($isSpdx) {
   $packages = [System.Collections.ArrayList]::new()
   if ($sbom.packages) { foreach ($package in $sbom.packages) { [void]$packages.Add($package) } }

   $relationships = [System.Collections.ArrayList]::new()
   if ($sbom.relationships) { foreach ($relationship in $sbom.relationships) { [void]$relationships.Add($relationship) } }

   # The document's own root, so the added packages hang off the same node Syft's do
   # rather than floating unattached - an SPDX package with no relationship is valid and
   # invisible to plenty of consumers.
   $root = $sbom.documentDescribes | Select-Object -First 1
   if (-not $root) { $root = 'SPDXRef-DOCUMENT' }

   foreach ($dependency in $dependencies) {
      $existing = $packages | Where-Object {
         $_.externalRefs | Where-Object { $_.referenceLocator -eq $dependency.purl }
      }
      if ($existing) { continue }

      $id = 'SPDXRef-Package-native-' + (Get-Id $dependency)

      $package = [ordered]@{
         SPDXID = $id
         name   = $dependency.name
      }
      if ($dependency.version) { $package.versionInfo = [string] $dependency.version }
      $package.downloadLocation = $dependency.website
      $package.homepage         = $dependency.website
      $package.licenseConcluded = $dependency.license
      $package.licenseDeclared  = $dependency.license
      $package.copyrightText    = 'NOASSERTION'
      $package.filesAnalyzed    = $false
      $package.supplier         = 'NOASSERTION'
      $checksums = @(foreach ($hash in @($dependency.hashes)) {
         if ($hash) { [pscustomobject]@{ algorithm = ([string] $hash.algorithm -replace '-', ''); checksumValue = [string] $hash.value } }
      })
      if ($checksums.Count) { $package.checksums = $checksums }
      $package.comment = Get-Comment $dependency
      $references = @([pscustomobject]@{ referenceCategory = 'PACKAGE-MANAGER'; referenceType = 'purl'; referenceLocator = $dependency.purl })
      if ($dependency.cpe) { $references += [pscustomobject]@{ referenceCategory = 'SECURITY'; referenceType = 'cpe23Type'; referenceLocator = $dependency.cpe } }
      $package.externalRefs = $references

      [void]$packages.Add([pscustomobject]$package)

      [void]$relationships.Add([pscustomobject]@{
         spdxElementId      = $root
         relationshipType   = 'DEPENDS_ON'
         relatedSpdxElement = $id
      })

      $added++
   }

   $sbom.packages = $packages.ToArray()
   if ($sbom.PSObject.Properties.Name -contains 'relationships') { $sbom.relationships = $relationships.ToArray() }
   else { $sbom | Add-Member -NotePropertyName relationships -NotePropertyValue $relationships.ToArray() }
}
else {
   $components = [System.Collections.ArrayList]::new()
   if ($sbom.components) { foreach ($component in $sbom.components) { [void]$components.Add($component) } }

   foreach ($dependency in $dependencies) {
      if ($components | Where-Object { $_.purl -eq $dependency.purl }) { continue }

      $component = [ordered]@{
         'bom-ref' = 'hmailserver-native-' + (Get-Id $dependency)
         type      = 'library'
         name      = $dependency.name
      }
      if ($dependency.version) { $component.version = [string] $dependency.version }
      $component.purl = $dependency.purl
      if ($dependency.cpe) { $component.cpe = $dependency.cpe }
      $component.scope = 'required'
      $component.description = Get-Comment $dependency
      $hashes = @(foreach ($hash in @($dependency.hashes)) {
         if ($hash) { [pscustomobject]@{ alg = [string] $hash.algorithm; content = [string] $hash.value } }
      })
      if ($hashes.Count) { $component.hashes = $hashes }
      # An SPDX expression (it has an operator) as an expression, an identifier as an id,
      # and a licence SPDX has no identifier for by its name.
      $license = [string] $dependency.license
      if ($license -match ' ') { $component.licenses = @([pscustomobject]@{ expression = $license }) }
      elseif ($license -and $license -ne 'NOASSERTION') { $component.licenses = @([pscustomobject]@{ license = [pscustomobject]@{ id = $license } }) }
      elseif ($dependency.licenseName) { $component.licenses = @([pscustomobject]@{ license = [pscustomobject]@{ name = [string] $dependency.licenseName } }) }
      $component.externalReferences = @([pscustomobject]@{ type = 'website'; url = $dependency.website })

      [void]$components.Add([pscustomobject]$component)

      $added++
   }

   $sbom.components = $components.ToArray()
}

# Depth matters: the default of 2 flattens externalRefs into type names and produces a
# corrupt SBOM that still looks plausible.
$sbom | ConvertTo-Json -Depth 32 | Set-Content -Path $SbomFile -Encoding UTF8

$format = if ($isSpdx) { 'SPDX' } else { 'CycloneDX' }
Write-Host ("{0}: added {1} component(s) to {2}" -f $format, $added, (Split-Path -Leaf $SbomFile))

foreach ($dependency in $dependencies) {
   Write-Host ("  {0} {1} ({2})" -f $dependency.name, $(if ($dependency.version) { $dependency.version } else { 'the distribution''s' }), $dependency.license)
}
