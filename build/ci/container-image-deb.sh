#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image-deb job of .gitlab/ci/container-image.yml, the first of the
# image jobs, which a maintainer starts by hand in a v* tag's pipeline once the
# release is published: the .deb of each architecture the image is built for - amd64
# and arm64 - and each .deb's own SBOM, taken from the PUBLISHED release and held to
# their Sigstore bundles, for container-image-amd64, container-image-arm64 and
# container-image-sbom. Run in cosign's own image
# (HM_COSIGN_IMAGE, gcr.io/projectsigstore/cosign, the -dev tag, whose busybox is the
# shell), from the root of the checkout of the tag:
#
#   sh build/ci/container-image-deb.sh
#
# Out:  container-image-deb/hmailserver_<v>_<arch>.deb            the release's .deb
#       container-image-deb/hmailserver_<v>_<arch>.deb.cdx.json   its SBOM
#       for amd64 and arm64, each with <file>.cosign.bundle beside it, and
#       verification.txt: what was checked, the SHA-256 of each file and the identity
#       that signed it. A release without either architecture's .deb, or its SBOM,
#       stops the job: the image is built for both platforms or not at all.
#
# Why from the release and not from release-sign's artifact: the image jobs may run
# weeks after the tag - the project's CI/CD variable HM_PUBLISH_IMAGE, which this
# checks first, is set only with the project at its lasting path, the group's since
# 29 September 2026 (hmailserver/docs/ContainerImage.md, "Publishing") - and by then
# the jobs' artifacts may have expired. What the release publishes does not expire, and it
# is what users download, so the image is built from exactly those bytes.
#
# How the files are trusted: each is verified with cosign verify-blob against its
# bundle, as signed by this project's pipeline for this tag - at either of the two
# paths a server trusts, as a release signed before the project's move into the
# group on 29 September 2026 was (though 6.3.5, the one such release signed by a
# pipeline, has no image jobs in its own: they read the switch as its pipeline was
# made) - with the project's id in the certificate required
# for the group's path, as the server requires it (SigstoreVerifier.cpp,
# build/ci/release-verify.sh), read from the certificate's extensions
# (build/ci/sigstore-certificate.sh). The transport is not what is trusted: busybox's wget,
# the one this image has, does not check a server's TLS certificate, so nothing is
# sent that a listener could use - no token: the project is public, and its release
# and package registry answer anonymously - and a file altered on the way fails its
# signature and stops the job.
set -eu

out=container-image-deb
issuer=https://gitlab.com
# The project whose releases a server takes, by its id, which the move kept.
project_id=86730559

fail() {
  echo "CONTAINER-IMAGE-DEB FAILED: $*"
  exit 1
}

# The switch, read as the job runs (build/ci/linux-image.sh reads it again before it
# pushes): the jobs exist in every v* tag's pipeline, and do nothing until it is 1.
if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
  echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so no image is built or pushed."
  echo "It is set by the maintainers for the image at registry.gitlab.com/hmailserver/hmailserver"
  echo "(hmailserver/docs/ContainerImage.md, 'Publishing'); then run this job again."
  exit 3
fi

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
# A version is what a container tag and a package name may hold, and nothing a URL
# would have to escape.
printf '%s\n' "$version" | grep -Eqx '[0-9][A-Za-z0-9._-]{0,62}' || fail "$version is not a release version"
[ "${CI_PROJECT_ID:-}" = "$project_id" ] || fail "the project's id is '${CI_PROJECT_ID:-}', not $project_id, whose releases a server takes"
api="https://gitlab.com/api/v4/projects/$project_id"

if ! command -v cosign > /dev/null 2>&1; then
  [ -x /ko-app/cosign ] && PATH="/ko-app:$PATH" || fail "there is no cosign in this image: HM_COSIGN_IMAGE must be cosign's own -dev image"
fi
cosign version 2>&1 | grep -E 'GitVersion|GitCommit' || true
# The bundle read as the Sigstore bundle it is, strictly - no field twice, none cosign
# does not know - which build/ci/sigstore-certificate.sh's reading of the one
# certificate cosign verified rests on. A cosign without --new-bundle-format reads the
# old format, its "cert" field, and passes over every field it does not know, so the
# certificate the reader found would be one cosign never looked at.
cosign verify-blob --help 2>&1 | grep -q -- '--new-bundle-format' \
  || fail "this cosign has no verify-blob --new-bundle-format: HM_COSIGN_IMAGE must be cosign 2.4 or later"
verify_flags="--new-bundle-format"

rm -rf "$out"
mkdir -p "$out"

fetch() {
  # fetch URL FILE: three tries, anonymously.
  attempt=1
  while :; do
    rm -f "$2"
    if wget -q -U hmailserver-container-image -O "$2" "$1" 2> "$out/wget.log"; then
      return 0
    fi
    if [ "$attempt" -ge 3 ]; then
      cat "$out/wget.log"
      return 1
    fi
    attempt=$((attempt + 1))
    sleep 5
  done
}

# The release, which must be published and link each file where the release chain
# put it (build/ci/release-chain.py, registry_url).
fetch "$api/releases/$tag" "$out/release.json" || fail "the release $tag could not be read from $api/releases/$tag: publish it first"
grep -qF "\"tag_name\":\"$tag\"" "$out/release.json" || fail "$api/releases/$tag is not the release $tag"

rows=""
signed_as=""
# The two architectures the image is built for, each a .deb the release carries
# (linux-build's amd64 one and deb-ubuntu26.04-arm64's), and each .deb's SBOM.
for arch in amd64 arm64; do
  deb="hmailserver_${version}_${arch}.deb"
  for name in "$deb" "$deb.cdx.json"; do
    url="$api/packages/generic/hmailserver/$version/$name"
    grep -qF "\"url\":\"$url\"" "$out/release.json" \
      || fail "the release $tag has no link to $url: it does not carry $name"
    fetch "$url" "$out/$name" || fail "$name could not be downloaded from $url"
    fetch "$url.cosign.bundle" "$out/$name.cosign.bundle" || fail "$name.cosign.bundle could not be downloaded: the release carries $name unsigned"
    [ -s "$out/$name" ] || fail "$name is empty"

    # Signed by this project's pipeline for this tag, at either path.
    identity=""
    for repository in https://gitlab.com/Progressiverobot/hmailserver https://gitlab.com/hmailserver/hmailserver; do
      candidate="$repository//.gitlab-ci.yml@refs/tags/$tag"
      # $verify_flags is the one flag set above, split on purpose: busybox sh has no arrays.
      # shellcheck disable=SC2086
      # nosemgrep
      if cosign verify-blob $verify_flags --bundle "$out/$name.cosign.bundle" \
           --certificate-identity "$candidate" --certificate-oidc-issuer "$issuer" "$out/$name" > "$out/verify.log" 2>&1; then
        identity="$candidate"
        break
      fi
    done
    [ -n "$identity" ] || { cat "$out/verify.log"; fail "$name does not verify as signed by this project's pipeline for $tag"; }

    # The project's id in the certificate (Fulcio's 1.3.6.1.4.1.57264.1.15, a UTF8String
    # in an OCTET STRING): required at the group's path, and where it is there at all,
    # this project's. Read from the certificate's extensions by
    # build/ci/sigstore-certificate.sh (this image has no Python, and cosign checks no
    # CI's extension but GitHub's), which build/ci/sigstore-certificate.py holds to its
    # own reading - never by a search of the certificate's bytes, whose public key, bytes
    # its holder chooses, could hold this project's id in front of the extension naming
    # another's.
    said=$(sh build/ci/sigstore-certificate.sh repository-id "$out/$name.cosign.bundle") \
      || fail "the certificate of $name could not be read for the project's id: ${said#refused: }"
    case "$said" in
      "id=$project_id") id_said="$project_id" ;;
      absent)
        case "$identity" in
          https://gitlab.com/hmailserver/hmailserver//*) fail "the certificate of $name, at the group's path, records no repository id" ;;
        esac
        id_said="none recorded" ;;
      *) fail "the certificate of $name names the repository id '${said#id=}', not $project_id" ;;
    esac

    if [ -n "$signed_as" ] && [ "$signed_as" != "$identity" ]; then
      fail "$name is signed as $identity, and the file before it as $signed_as"
    fi
    signed_as="$identity"
    sum=$(sha256sum "$out/$name" | cut -d' ' -f1)
    echo "$name  $sum  verifies as $identity (repository id: $id_said)"
    rows="$rows$name  $sum
  "
  done
done
rm -f "$out/release.json" "$out/verify.log" "$out/wget.log"

{
  echo "release:  $tag, $api/releases/$tag"
  echo "identity: $signed_as"
  echo "issuer:   $issuer"
  echo ""
  printf '%s' "$rows"
} > "$out/verification.txt"
cat "$out/verification.txt"
echo "TAKEN: hmailserver_${version}_amd64.deb and hmailserver_${version}_arm64.deb and their SBOMs from the published release $tag, all verified as $signed_as."
