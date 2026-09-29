#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image-sbom job of .gitlab/ci/container-image.yml: the software bills
# of materials of the release's container image, one for each platform's image that
# container-image pushed - linux/amd64 and linux/arm64 - for container-image-sign to
# attest to that platform's image with cosign (OpenSSF Baseline OSPS-QA-02.02: every
# compiled release asset is delivered with an SBOM; for the image, the SBOM travels
# with it in the registry as a signed attestation).
#
#   bash build/ci/container-image-sbom.sh
#
# Run in the image of build/ci/linux.Dockerfile (syft, python3), from the root of the
# checkout of the tag.
#
# In:   HM_IMAGE_AMD64 and HM_IMAGE_ARM64, from container-image's dotenv report: each
#       <registry image>@sha256:<digest> of that platform's image, as the index HM_IMAGE
#       names it.
#       container-image-deb/, container-image-deb's artifact: the .deb of each
#       architecture the images are built from, and each .deb's own SBOM
#       (build/asset-sbom.py), all taken from the published release and held to their
#       Sigstore bundles (build/ci/container-image-deb.sh).
# Out:  container-image-sbom/hmailserver-image-<arch>.cdx.json   the linux/<arch> image's SBOM
#       container-image-sbom/image-<arch>.syft.cdx.json          what Syft found in it
#
# Syft reads each image from the registry by its digest, as the platform it is, with
# the job's own registry credentials (CI_REGISTRY_USER and CI_REGISTRY_PASSWORD, which
# the job token has for this project alone), handed to it in the environment and never
# printed. It lists every distribution package in the image, the hmailserver .deb
# among them; build/asset-sbom.py adds what that architecture's .deb's own SBOM names
# and Syft cannot see inside the binary (Boost, linked in, and QuickJS, compiled in),
# and names the image by its digest and its platform as the document's subject.
set -euo pipefail

out=container-image-sbom
signed="${HM_IMAGE_DEB_DIR:-container-image-deb}"
# The platforms the image is built for; container-image pushes one image of each.
platforms=(amd64 arm64)

fail() {
  echo "CONTAINER-IMAGE-SBOM FAILED: $*"
  exit 1
}

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
registry_image="${CI_REGISTRY_IMAGE:-}"
[ -n "$registry_image" ] || fail "the job has no CI_REGISTRY_IMAGE: the project's container registry is off"
[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD"

rm -rf "$out"
mkdir -p "$out"
syft version | head -n 2

# No update check, nothing sent anywhere but the registry read.
export SYFT_CHECK_FOR_APP_UPDATE=false
for arch in "${platforms[@]}"; do
  variable="HM_IMAGE_${arch^^}"
  image="${!variable:-}"
  printf '%s\n' "$image" | grep -Eqx "${registry_image}@sha256:[0-9a-f]{64}" \
    || fail "$variable is '$image', not $registry_image by digest: container-image's dotenv report did not arrive"
  deb="$signed/hmailserver_${version}_${arch}.deb"
  [ -f "$deb" ] || fail "no $deb: container-image-deb's artifact did not arrive"
  [ -f "$deb.cdx.json" ] || fail "no $deb.cdx.json: the release carries no SBOM of the .deb the linux/$arch image is built from"
  python3 build/asset-sbom.py verify "$deb.cdx.json" "$deb" > /dev/null \
    || fail "$deb.cdx.json does not describe $deb"

  echo ""
  echo "== linux/$arch: $image"
  SYFT_REGISTRY_AUTH_AUTHORITY="$CI_REGISTRY" \
  SYFT_REGISTRY_AUTH_USERNAME="$CI_REGISTRY_USER" \
  SYFT_REGISTRY_AUTH_PASSWORD="$CI_REGISTRY_PASSWORD" \
    syft scan "registry:$image" --platform "linux/$arch" --quiet \
      --source-name "$registry_image" --source-version "$version" \
      -o "cyclonedx-json=$out/image-$arch.syft.cdx.json" \
    || fail "syft could not read $image, the linux/$arch image, from the registry"

  python3 build/asset-sbom.py image --syft "$out/image-$arch.syft.cdx.json" --deb-sbom "$deb.cdx.json" \
    --image "$image" --arch "$arch" --version "$version" --out "$out/hmailserver-image-$arch.cdx.json" \
    || fail "the linux/$arch image's SBOM could not be made"

  # Syft must have found the server's own package, of this architecture, in the image,
  # or it did not read the image the .deb went into.
  python3 - "$out/image-$arch.syft.cdx.json" "$arch" << 'EOF' || fail "Syft found no hmailserver package for $arch in $image: it did not read the image the .deb went into"
import json, sys
components = json.load(open(sys.argv[1])).get("components") or []
sys.exit(0 if any(c.get("name") == "hmailserver" and "arch=" + sys.argv[2] in (c.get("purl") or "") for c in components) else 1)
EOF
done
ls -la "$out"
echo "SBOMS MADE: $out/hmailserver-image-amd64.cdx.json and $out/hmailserver-image-arm64.cdx.json describe the two platforms' images."
