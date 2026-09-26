#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The release-sign job of .gitlab-ci.yml: every file of the release signed with
# Sigstore, keylessly, as this project's pipeline for its v* tag. Run in cosign's own
# image (gcr.io/projectsigstore/cosign, the -dev tag, whose busybox is the shell),
# pinned by digest in HM_COSIGN_IMAGE, from the root of the checkout:
#
#   sh build/ci/release-sign.sh
#
# In:   release-assets/        what the release job collected (build/ci/release-chain.py collect)
#       release-authenticode/  the Windows installer as release-authenticode signed it
# Out:  release-signed/        every file, with <file>.cosign.bundle beside it
#
# The identity token is the job's SIGSTORE_ID_TOKEN (id_tokens, aud sigstore), which
# cosign reads from the environment itself: it is never on a command line and never
# printed. Fulcio names the certificate after the token's ci_config_ref_uri,
#   https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/<tag>
# which is the GitLab signer SigstoreVerifier.cpp trusts, and only a top-level pipeline
# of a tag has it - so this runs in the tag's own pipeline and never in a child one.
#
# What every server built after 6.3.3 reads, and nothing else (docs/UpdateFeed.md,
# "What the signing job produces"): a Sigstore bundle, not cosign's older format and
# not an attestation; a messageSignature over the file's SHA-256; the certificate; and
# a Rekor v1 entry - hashedrekord 0.0.1 with an integratedTime - from the public log,
# whose key the server has compiled in. cosign 3 follows Sigstore's signing
# configuration, which can send a signature to Rekor v2 with another key and no
# integratedTime, so the log and the bundle format are named here, and each bundle is
# looked at before it is kept. The release-verify job then checks every one with the
# server's own verifier.
set -eu

in=release-assets
authenticode=release-authenticode
out=release-signed
issuer=https://gitlab.com

fail() {
  echo "RELEASE-SIGN FAILED: $*"
  exit 1
}

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
installer="hMailServer-${version}-x64.exe"
identity="https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/${tag}"

[ -n "${SIGSTORE_ID_TOKEN:-}" ] || fail "the job has no SIGSTORE_ID_TOKEN: id_tokens with aud sigstore is missing from it"
if ! command -v cosign > /dev/null 2>&1; then
  [ -x /ko-app/cosign ] && PATH="/ko-app:$PATH" || fail "there is no cosign in this image: HM_COSIGN_IMAGE must be cosign's own -dev image"
fi
cosign version 2>&1 | grep -E 'GitVersion|GitCommit' || true

[ -d "$in" ] || fail "$in is not here: the release job's artifact did not arrive"
[ -f "$authenticode/$installer" ] || fail "$authenticode/$installer is not here: release-authenticode did not hand back a signed installer"
[ -f "$in/$installer" ] || fail "$in/$installer is not here"
# The signed installer is not the unsigned one: Authenticode adds a signature, so the
# bytes change. The same bytes would mean nothing was signed.
if cmp -s "$in/$installer" "$authenticode/$installer"; then
  fail "the installer release-authenticode handed back is byte for byte the unsigned one"
fi

rm -rf "$out"
mkdir -p "$out"
cp "$in"/* "$out"/
cp "$authenticode/$installer" "$out/$installer"

# The flags every cosign from 2.4 on reads the same way; --use-signing-config exists
# from 2.6 and is true by default from 3.0, and is turned off where it exists.
sign_flags="--yes --new-bundle-format --rekor-url https://rekor.sigstore.dev --fulcio-url https://fulcio.sigstore.dev"
if cosign sign-blob --help 2>&1 | grep -q -- '--use-signing-config'; then
  sign_flags="$sign_flags --use-signing-config=false"
fi
verify_flags=""
if cosign verify-blob --help 2>&1 | grep -q -- '--new-bundle-format'; then
  verify_flags="--new-bundle-format"
fi

signed=0
for file in "$out"/*; do
  [ -f "$file" ] || continue
  case "$file" in *.cosign.bundle) continue ;; esac
  name=$(basename "$file")
  echo ""
  echo "== $name  $(sha256sum "$file" | cut -d' ' -f1)"
  # shellcheck disable=SC2086
  cosign sign-blob $sign_flags --bundle "$file.cosign.bundle" "$file" > /dev/null
  bundle="$file.cosign.bundle"
  [ -s "$bundle" ] || fail "cosign wrote no bundle for $name"

  # What the server's parser reads, looked for before the bundle is kept.
  grep -Eq '"mediaType" *: *"application/vnd\.dev\.sigstore\.bundle' "$bundle" || fail "$name.cosign.bundle is not a Sigstore bundle"
  grep -q '"messageSignature"' "$bundle" || fail "$name.cosign.bundle carries no messageSignature"
  if grep -q '"dsseEnvelope"' "$bundle"; then fail "$name.cosign.bundle is an attestation, not a blob signature"; fi
  grep -Eq '"kind" *: *"hashedrekord"' "$bundle" || fail "$name.cosign.bundle's log entry is not a hashedrekord"
  grep -Eq '"version" *: *"0\.0\.1"' "$bundle" || fail "$name.cosign.bundle's log entry is not hashedrekord 0.0.1 (Rekor v1)"
  grep -q '"integratedTime"' "$bundle" || fail "$name.cosign.bundle's log entry has no integratedTime (Rekor v2?)"
  grep -Eq '"inclusionPromise"|"inclusionProof"' "$bundle" || fail "$name.cosign.bundle carries neither a signed entry timestamp nor an inclusion proof"

  # shellcheck disable=SC2086
  cosign verify-blob $verify_flags --bundle "$bundle" \
    --certificate-identity "$identity" --certificate-oidc-issuer "$issuer" "$file" > /dev/null 2>&1 \
    || fail "$name does not verify as signed by $identity"
  echo "   signed, recorded in Rekor v1, and verified as $identity"
  signed=$((signed + 1))
done

# Counted, because a loop over a glob that matched nothing passes having signed nothing.
expected=6
[ "$signed" -eq "$expected" ] || fail "$signed file(s) signed; a release is $expected"
echo ""
echo "All $signed files of $tag are signed as $identity."
ls -l "$out"
