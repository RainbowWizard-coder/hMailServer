#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image-sign job of .gitlab/ci/container-image.yml: the release's
# container image, which container-image pushed - one index naming a linux/amd64 and a
# linux/arm64 image - signed with Sigstore, keylessly, as this project's pipeline for
# its v* tag, the same identity every file of the release is signed with
# (build/ci/release-sign.sh): the index by its digest, and each platform's image by its
# own, with that platform's SBOM attested to it. Run in cosign's own image
# (HM_COSIGN_IMAGE, gcr.io/projectsigstore/cosign, the -dev tag, whose busybox is the
# shell), from the root of the checkout:
#
#   sh build/ci/container-image-sign.sh
#
# In:   HM_IMAGE, HM_IMAGE_AMD64 and HM_IMAGE_ARM64, from container-image's dotenv
#       report: the index and the two platforms' images, each
#       <registry image>@sha256:<digest>. Every tag that job pushed (:<version>, and
#       :<major>.<minor> and :latest for a release without a suffix) names the index,
#       so its one signature covers them.
#       container-image-sbom/hmailserver-image-<arch>.cdx.json, each platform's image's
#       SBOM, which container-image-sbom made (build/ci/container-image-sbom.sh).
# Out:  container-image-signed/verification.txt, what cosign verify and cosign
#       verify-attestation answered, and the commands that check each.
#
# A client that pulls a tag gets the index, and then its own platform's image by that
# image's digest; so the index and each image are signed, each by its own digest. Each
# platform's SBOM goes with that platform's image as a signed attestation of the
# predicate type cyclonedx, stored beside it in the registry (sha256-<digest>.att), by
# the same identity and in the same log as the signature (OpenSSF Baseline
# OSPS-QA-02.02):
#
#   cosign verify-attestation --type cyclonedx --certificate-identity <identity> \
#     --certificate-oidc-issuer https://gitlab.com <image>@sha256:<the platform's digest>
#
# prints it, and SECURITY.md, "Software bills of materials", says how to read it.
#
# The signatures are stored in the registry beside what they sign, in cosign's tag form
# (sha256-<digest>.sig), which every cosign and the admission controllers that use it
# read; the signing configuration and the new bundle format are turned off wherever the
# version has them, and the log is Rekor v1 at rekor.sigstore.dev, as for the files.
#
# The identity token is the job's SIGSTORE_ID_TOKEN (id_tokens, aud sigstore), which
# cosign reads from the environment itself. The registry password is the job's own
# CI_REGISTRY_PASSWORD, handed to cosign login on its standard input into a
# configuration folder of the job's, removed at the end. Neither is ever on a command
# line or printed.
set -eu

out=container-image-signed
issuer=https://gitlab.com
# The platforms the image is built for; container-image pushed one image of each.
platforms="amd64 arm64"

fail() {
  echo "CONTAINER-IMAGE-SIGN FAILED: $*"
  exit 1
}

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
# The project's path, as release-sign.sh has it: the group's, where the project has
# been since 29 September 2026. The image carries the same identity as the release's
# files, and is pushed only at the group's path (build/ci/container-image-push.sh).
case "${CI_PROJECT_PATH:-}" in
  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;
  *) fail "the project's path is '${CI_PROJECT_PATH:-}', not one a release is signed from" ;;
esac
identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"

registry_image="${CI_REGISTRY_IMAGE:-}"
[ -n "$registry_image" ] || fail "the job has no CI_REGISTRY_IMAGE: the project's container registry is off"
index="${HM_IMAGE:-}"
printf '%s\n' "$index" | grep -Eqx "${registry_image}@sha256:[0-9a-f]{64}" \
  || fail "HM_IMAGE is '$index', not $registry_image by digest: container-image's dotenv report did not arrive"
# Each platform's image, by the variable container-image named it in.
platform_image() {
  case "$1" in
    amd64) printf '%s\n' "${HM_IMAGE_AMD64:-}" ;;
    arm64) printf '%s\n' "${HM_IMAGE_ARM64:-}" ;;
  esac
}
for arch in $platforms; do
  image=$(platform_image "$arch")
  printf '%s\n' "$image" | grep -Eqx "${registry_image}@sha256:[0-9a-f]{64}" \
    || fail "the linux/$arch image is '$image', not $registry_image by digest: container-image's dotenv report did not arrive"
  [ "$image" != "$index" ] || fail "the linux/$arch image is the index itself"
  sbom="container-image-sbom/hmailserver-image-$arch.cdx.json"
  [ -s "$sbom" ] || fail "$sbom is not here: container-image-sbom did not hand back the linux/$arch image's SBOM"
  grep -q "\"content\": \"${image#*@sha256:}\"" "$sbom" || fail "$sbom does not describe ${image#*@}, the linux/$arch image"
done

[ -n "${SIGSTORE_ID_TOKEN:-}" ] || fail "the job has no SIGSTORE_ID_TOKEN: id_tokens with aud sigstore is missing from it"
[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD"
if ! command -v cosign > /dev/null 2>&1; then
  [ -x /ko-app/cosign ] && PATH="/ko-app:$PATH" || fail "there is no cosign in this image: HM_COSIGN_IMAGE must be cosign's own -dev image"
fi
cosign version 2>&1 | grep -E 'GitVersion|GitCommit' || true

rm -rf "$out"
mkdir -p "$out"
DOCKER_CONFIG="$(mktemp -d)"
export DOCKER_CONFIG
trap 'rm -rf "$DOCKER_CONFIG"' EXIT
printf '%s' "$CI_REGISTRY_PASSWORD" | cosign login "$CI_REGISTRY" --username "$CI_REGISTRY_USER" --password-stdin > /dev/null \
  || fail "cosign could not log in to $CI_REGISTRY with the job's credentials"

sign_flags="--yes --rekor-url https://rekor.sigstore.dev --fulcio-url https://fulcio.sigstore.dev"
help=$(cosign sign --help 2>&1 || true)
# Each case over lines of its own: Semgrep's bash parser cannot read a case on one
# line, and a case it cannot read leaves the rest of the script unscanned.
case "$help" in
  *--use-signing-config*) sign_flags="$sign_flags --use-signing-config=false" ;;
esac
case "$help" in
  *--new-bundle-format*) sign_flags="$sign_flags --new-bundle-format=false" ;;
esac
verify_flags=""
case "$(cosign verify --help 2>&1 || true)" in
  *--new-bundle-format*) verify_flags="--new-bundle-format=false" ;;
esac
attest_flags="--yes --rekor-url https://rekor.sigstore.dev --fulcio-url https://fulcio.sigstore.dev"
attest_help=$(cosign attest --help 2>&1 || true)
case "$attest_help" in
  *--use-signing-config*) attest_flags="$attest_flags --use-signing-config=false" ;;
esac
case "$attest_help" in
  *--new-bundle-format*) attest_flags="$attest_flags --new-bundle-format=false" ;;
esac

# sign NAME IMAGE: IMAGE (<registry image>@sha256:<digest>) signed, and the signature
# verified as this pipeline's, over that very digest.
sign() {
  echo ""
  echo "== $1: $2"
  # The flags are split into words on purpose, here and below: busybox sh has no arrays.
  # shellcheck disable=SC2086
  # nosemgrep
  cosign sign $sign_flags "$2" || fail "cosign did not sign $2 ($1)"
  # shellcheck disable=SC2086
  # nosemgrep
  cosign verify $verify_flags --certificate-identity "$identity" --certificate-oidc-issuer "$issuer" \
    "$2" > "$out/verify-$1.json" 2> "$out/verify-$1.log" \
    || { cat "$out/verify-$1.log"; fail "$2 ($1) does not verify as signed by $identity"; }
  grep -q '"critical"' "$out/verify-$1.json" || fail "cosign verify answered nothing for $2 ($1)"
  grep -q "${2#*@}" "$out/verify-$1.json" || fail "the signature cosign verify found is not over ${2#*@} ($1)"
}

# The index, which every tag names.
sign index "$index"
# Each platform's image, and its SBOM attested to it by its digest.
for arch in $platforms; do
  image=$(platform_image "$arch")
  sbom="container-image-sbom/hmailserver-image-$arch.cdx.json"
  sign "$arch" "$image"
  # shellcheck disable=SC2086
  # nosemgrep
  cosign attest $attest_flags --type cyclonedx --predicate "$sbom" "$image" || fail "cosign did not attest the SBOM to $image"
  # shellcheck disable=SC2086
  # nosemgrep
  cosign verify-attestation $verify_flags --type cyclonedx --certificate-identity "$identity" --certificate-oidc-issuer "$issuer" \
    "$image" > "$out/verify-attestation-$arch.json" 2> "$out/verify-attestation-$arch.log" \
    || { cat "$out/verify-attestation-$arch.log"; fail "the SBOM attested to $image does not verify as $identity's"; }
  grep -q '"payloadType"' "$out/verify-attestation-$arch.json" || fail "cosign verify-attestation answered nothing for $image"
done

{
  echo "index:        $index"
  echo "tags:         ${HM_IMAGE_TAGS:-}"
  for arch in $platforms; do
    printf 'linux/%-6s %s\n' "$arch:" "$(platform_image "$arch")"
  done
  echo "identity:     $identity"
  echo "issuer:       $issuer"
  echo ""
  echo "The index, which every tag names, and each platform's image are signed:"
  echo "cosign verify --certificate-identity $identity \\"
  echo "  --certificate-oidc-issuer $issuer $index"
  for arch in $platforms; do
    echo "cosign verify --certificate-identity $identity \\"
    echo "  --certificate-oidc-issuer $issuer $(platform_image "$arch")"
  done
  echo ""
  echo "Each platform's SBOM (CycloneDX), attested to that platform's image:"
  for arch in $platforms; do
    echo "cosign verify-attestation --type cyclonedx --certificate-identity $identity \\"
    echo "  --certificate-oidc-issuer $issuer $(platform_image "$arch")"
  done
} > "$out/verification.txt"
cat "$out/verification.txt"
echo ""
echo "SIGNED: $index and its linux/amd64 and linux/arm64 images, as $identity, each image with its SBOM attested."
