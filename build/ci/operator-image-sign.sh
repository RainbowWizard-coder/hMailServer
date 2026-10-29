#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The operator-image-sign job of .gitlab/ci/operator.yml: the operator's image, which
# operator-image-push pushed - one index naming a linux/amd64 and a linux/arm64 image -
# signed with Sigstore, keylessly, as this project's pipeline for its v* tag, the
# identity the release's files and the server's image are signed with
# (build/ci/container-image-sign.sh): the index by its digest and each platform's
# image by its own. Then the release's OLM bundle is written for that index
# (build/ci/operator-bundle.sh), for the pull request to OperatorHub.io's repository,
# which a maintainer makes by hand (platform/packaging/helm/operator/README.md). Run
# in cosign's own image (HM_COSIGN_IMAGE, the -dev tag, whose busybox is the shell),
# from the root of the checkout:
#
#   sh build/ci/operator-image-sign.sh
#
# In:   HM_OPERATOR_IMAGE, HM_OPERATOR_IMAGE_AMD64 and HM_OPERATOR_IMAGE_ARM64, from
#       operator-image-push's dotenv report.
# Out:  operator-image-signed/verification.txt and certificate-<index|amd64|arm64>.pem;
#       operator-bundle/hmailserver-operator/<version>/, the bundle, and its
#       SHA256SUMS.txt.
#
# Each signature's certificate is held to the project's id, 86730559, as
# container-image-sign.sh holds the server image's (build/ci/sigstore-certificate.sh
# reads it from the certificate's extensions). The identity token is the job's
# SIGSTORE_ID_TOKEN (id_tokens, aud sigstore); the registry password the job's own
# CI_REGISTRY_PASSWORD, on cosign login's standard input, into a configuration folder
# of the job's removed at the end. Neither is on a command line or printed.
set -eu

out=operator-image-signed
issuer=https://gitlab.com
project_id=86730559
platforms="amd64 arm64"

fail() {
  echo "OPERATOR-IMAGE-SIGN FAILED: $*"
  exit 1
}

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
case "${CI_PROJECT_PATH:-}" in
  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;
  *) fail "the project's path is '${CI_PROJECT_PATH:-}', not one a release is signed from" ;;
esac
identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"
operator_repository="${CI_REGISTRY_IMAGE:-}/operator"
[ "$operator_repository" = registry.gitlab.com/hmailserver/hmailserver/operator ] \
  || fail "the operator's repository would be '$operator_repository', not registry.gitlab.com/hmailserver/hmailserver/operator"

index="${HM_OPERATOR_IMAGE:-}"
printf '%s\n' "$index" | grep -Eqx "${operator_repository}@sha256:[0-9a-f]{64}" \
  || fail "HM_OPERATOR_IMAGE is '$index', not $operator_repository by digest: operator-image-push's dotenv report did not arrive"
platform_image() {
  case "$1" in
    amd64) printf '%s\n' "${HM_OPERATOR_IMAGE_AMD64:-}" ;;
    arm64) printf '%s\n' "${HM_OPERATOR_IMAGE_ARM64:-}" ;;
  esac
}
for arch in $platforms; do
  image=$(platform_image "$arch")
  printf '%s\n' "$image" | grep -Eqx "${operator_repository}@sha256:[0-9a-f]{64}" \
    || fail "the linux/$arch operator image is '$image', not $operator_repository by digest"
  [ "$image" != "$index" ] || fail "the linux/$arch operator image is the index itself"
done

[ -n "${SIGSTORE_ID_TOKEN:-}" ] || fail "the job has no SIGSTORE_ID_TOKEN: id_tokens with aud sigstore is missing from it"
[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD"
if ! command -v cosign > /dev/null 2>&1; then
  [ -x /ko-app/cosign ] || fail "there is no cosign in this image: HM_COSIGN_IMAGE must be cosign's own -dev image"
  PATH="/ko-app:$PATH"
fi

rm -rf "$out" operator-bundle
mkdir -p "$out"
DOCKER_CONFIG="$(mktemp -d)"
export DOCKER_CONFIG
trap 'rm -rf "$DOCKER_CONFIG"' EXIT
printf '%s' "$CI_REGISTRY_PASSWORD" | cosign login "$CI_REGISTRY" --username "$CI_REGISTRY_USER" --password-stdin > /dev/null \
  || fail "cosign could not log in to $CI_REGISTRY with the job's credentials"

# Which of two flags this cosign's sign and verify know, read from their help: each
# is given, set false, only where it is known (cosign_sign and cosign_verify, below).
help=$(cosign sign --help 2>&1 || true)
sign_has_signing_config=""
case "$help" in
  *--use-signing-config*) sign_has_signing_config=yes ;;
esac
sign_has_bundle_format=""
case "$help" in
  *--new-bundle-format*) sign_has_bundle_format=yes ;;
esac
verify_has_bundle_format=""
case "$(cosign verify --help 2>&1 || true)" in
  *--new-bundle-format*) verify_has_bundle_format=yes ;;
esac

# cosign_sign CERTIFICATE IMAGE and cosign_verify IMAGE: cosign sign and cosign verify
# with the flags above, each flag a word of its own. cosign's image's shell is
# busybox's sh, which has no arrays, so each function builds its flags as its own
# arguments with set --, and no word is split out of a string.
cosign_sign() {
  certificate=$1 target=$2
  set -- --yes --rekor-url https://rekor.sigstore.dev --fulcio-url https://fulcio.sigstore.dev
  [ -z "$sign_has_signing_config" ] || set -- "$@" --use-signing-config=false
  [ -z "$sign_has_bundle_format" ] || set -- "$@" --new-bundle-format=false
  cosign sign "$@" --output-certificate "$certificate" "$target"
}
cosign_verify() {
  target=$1
  set --
  [ -z "$verify_has_bundle_format" ] || set -- --new-bundle-format=false
  cosign verify "$@" --certificate-identity "$identity" --certificate-oidc-issuer "$issuer" "$target"
}

# certificate_id NAME: the project's id in certificate-NAME.pem, read as
# container-image-sign.sh reads it.
certificate_id() {
  pem="$out/certificate-$1.pem"
  [ -s "$pem" ] || fail "cosign sign wrote no certificate for the $1 signature"
  raw=$(tr -d '\r' < "$pem" | awk '
    /^-----BEGIN CERTIFICATE-----$/ { if (seen) exit; seen = 1; on = 1; next }
    /^-----END CERTIFICATE-----$/ { if (on) exit }
    on { printf "%s", $0 }')
  case "$raw" in
    "" | *[!A-Za-z0-9+/=]*) fail "certificate-$1.pem holds no PEM certificate" ;;
  esac
  printf '{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json", "verificationMaterial": {"certificate": {"rawBytes": "%s"}}}\n' \
    "$raw" > "$out/certificate-$1.json"
  said=$(sh build/ci/sigstore-certificate.sh repository-id "$out/certificate-$1.json") || said="refused: ${said#refused: }"
  rm -f "$out/certificate-$1.json"
  case "$said" in
    "id=$project_id") ;;
    *) fail "the certificate of the $1 signature does not carry the project's id $project_id: $said" ;;
  esac
  echo "the $1 signature's certificate carries the project's id, $project_id"
}

sign() {
  echo ""
  echo "== $1: $2"
  cosign_sign "$out/certificate-$1.pem" "$2" || fail "cosign did not sign $2 ($1)"
  cosign_verify "$2" > "$out/verify-$1.json" 2> "$out/verify-$1.log" \
    || { cat "$out/verify-$1.log"; fail "$2 ($1) does not verify as signed by $identity"; }
  grep -q "${2#*@}" "$out/verify-$1.json" || fail "the signature cosign verify found is not over ${2#*@} ($1)"
  certificate_id "$1"
}

sign index "$index"
for arch in $platforms; do
  sign "$arch" "$(platform_image "$arch")"
done

{
  echo "index:        $index ($operator_repository:$version)"
  for arch in $platforms; do
    printf 'linux/%-6s %s\n' "$arch:" "$(platform_image "$arch")"
  done
  echo "identity:     $identity"
  echo "issuer:       $issuer"
  echo ""
  echo "cosign verify --certificate-identity $identity \\"
  echo "  --certificate-oidc-issuer $issuer $index"
} > "$out/verification.txt"
cat "$out/verification.txt"

echo ""
sh build/ci/operator-bundle.sh "$version" "$index" operator-bundle
echo "SIGNED: the operator $index and its two images, as $identity; its bundle is in operator-bundle/."
