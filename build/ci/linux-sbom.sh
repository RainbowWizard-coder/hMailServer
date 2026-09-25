#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-sbom job of .gitlab-ci.yml: the Software Bill of Materials of the
# tree this pipeline is for, in SPDX and CycloneDX JSON, kept as the job's
# artifact. It is the old GitHub workflow sbom.yml less its last step: Syft
# inventories the tree (the .NET package references and the files), and
# build/merge-native-dependencies-into-sbom.ps1 adds what Syft cannot see -
# OpenSSL, Boost and libpq, which are built outside the tree - and the job fails
# when either document is missing one of them. Attaching the two files to a
# release is the release-chain row's (Roadmap2, "The release chain off a single
# forge"); on a v* tag the artifact is what it attaches.
#
#   bash build/ci/linux-sbom.sh
#
# Run in the image of build/ci/linux.Dockerfile (syft, pwsh), from the root of a
# checkout that holds nothing but the tree: the job needs no other job's
# artifacts, so there are none in it to be inventoried.
set -euo pipefail

out="${HM_OUT_DIR:-sbom}"
rm -rf "$out"
mkdir -p "$out"

echo "tree:   ${CI_COMMIT_SHA:-$(git rev-parse HEAD 2> /dev/null || echo unknown)}"
echo "ref:    ${CI_COMMIT_REF_NAME:-(not a pipeline)}"
echo "tag:    ${CI_COMMIT_TAG:-(none - this describes the branch, and claims nothing about a release)}"
syft version | head -n 2

# No update check (it would ask Anchore's server on every run) and nothing sent
# anywhere: the documents are written to files.
export SYFT_CHECK_FOR_APP_UPDATE=false
syft scan dir:. --quiet \
  --source-name hmailserver --source-version "${CI_COMMIT_TAG:-${CI_COMMIT_SHORT_SHA:-local}}" \
  --exclude "./$out" \
  -o "spdx-json=$out/hmailserver.spdx.json" \
  -o "cyclonedx-json=$out/hmailserver.cyclonedx.json"

for format in spdx cyclonedx; do
  pwsh -NoProfile -File build/merge-native-dependencies-into-sbom.ps1 -SbomFile "$out/hmailserver.$format.json"
done

# The absence this job exists to notice: every native dependency, by its purl,
# in both documents.
pwsh -NoProfile -Command '
  $ErrorActionPreference = "Stop"
  $expected = (./build/native-dependencies.ps1 | ConvertFrom-Json)
  $failed = $false
  foreach ($file in @("'"$out"'/hmailserver.spdx.json", "'"$out"'/hmailserver.cyclonedx.json")) {
    $content = Get-Content $file -Raw
    foreach ($dependency in $expected) {
      if ($content -notmatch [regex]::Escape($dependency.purl)) {
        Write-Host "$file does not list $($dependency.purl)"
        $failed = $true
      }
    }
  }
  if ($failed) { exit 1 }
  Write-Host "Both SBOMs list all $($expected.Count) native dependencies:"
  $expected | ForEach-Object { Write-Host ("  " + $_.purl) }
'
# And every input the build pins - by version and by hash - named in both, which the
# purl check above does not hold to (build/check-sbom-inputs.py).
python3 build/check-sbom-inputs.py --sbom "$out/hmailserver.spdx.json" --sbom "$out/hmailserver.cyclonedx.json"

ls -la "$out"
