#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image-sign job of .gitlab/ci/container-image.yml: the release's
# container image, which container-image pushed, signed by its digest with Sigstore,
# keylessly, as this project's pipeline for its v* tag - the same identity every file
# of the release is signed with (build/ci/release-sign.sh). Run in cosign's own image
# (HM_COSIGN_IMAGE, gcr.io/projectsigstore/cosign, the -dev tag, whose busybox is the
# shell), from the root of the checkout:
#
#   sh build/ci/container-image-sign.sh
#
# In:   HM_IMAGE, from container-image's dotenv report: <registry image>@sha256:<digest>.
#       Every tag that job pushed (:<version>, and :<major>.<minor> and :latest for a
#       release without a suffix) names that one digest, so one signature covers them.
# Out:  container-image-signed/verification.txt, what cosign verify answered.
#
# The signature is stored in the registry beside the image, in cosign's tag form
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

fail() {
  echo "CONTAINER-IMAGE-SIGN FAILED: $*"
  exit 1
}

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
# The two paths a server trusts a signature from, as release-sign.sh has them; the
# image carries the same identity as the release's files.
case "${CI_PROJECT_PATH:-}" in
  Progressiverobot/hmailserver) repository=https://gitlab.com/Progressiverobot/hmailserver ;;
  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;
  *) fail "the project's path is '${CI_PROJECT_PATH:-}', not one a release is signed from" ;;
esac
identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"

image="${HM_IMAGE:-}"
registry_image="${CI_REGISTRY_IMAGE:-}"
[ -n "$registry_image" ] || fail "the job has no CI_REGISTRY_IMAGE: the project's container registry is off"
printf '%s\n' "$image" | grep -Eqx "${registry_image}@sha256:[0-9a-f]{64}" \
  || fail "HM_IMAGE is '$image', not $registry_image by digest: container-image's dotenv report did not arrive"

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
case "$help" in *--use-signing-config*) sign_flags="$sign_flags --use-signing-config=false" ;; esac
case "$help" in *--new-bundle-format*) sign_flags="$sign_flags --new-bundle-format=false" ;; esac
verify_flags=""
case "$(cosign verify --help 2>&1 || true)" in *--new-bundle-format*) verify_flags="--new-bundle-format=false" ;; esac

echo ""
echo "== $image"
# shellcheck disable=SC2086
cosign sign $sign_flags "$image" || fail "cosign did not sign $image"

# shellcheck disable=SC2086
cosign verify $verify_flags --certificate-identity "$identity" --certificate-oidc-issuer "$issuer" \
  "$image" > "$out/verify.json" 2> "$out/verify.log" \
  || { cat "$out/verify.log"; fail "$image does not verify as signed by $identity"; }
grep -q '"critical"' "$out/verify.json" || fail "cosign verify answered nothing for $image"
grep -q "${image#*@}" "$out/verify.json" || fail "the signature cosign verify found is not over ${image#*@}"

{
  echo "image:    $image"
  echo "tags:     ${HM_IMAGE_TAGS:-}"
  echo "identity: $identity"
  echo "issuer:   $issuer"
  echo ""
  echo "cosign verify --certificate-identity $identity \\"
  echo "  --certificate-oidc-issuer $issuer $image"
} > "$out/verification.txt"
cat "$out/verification.txt"
echo ""
echo "SIGNED: $image, as $identity."
