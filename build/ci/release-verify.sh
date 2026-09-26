#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The release-verify job of .gitlab-ci.yml: every file release-sign signed, checked
# with the server's own verifier before anything is published - the hmailserver
# binary linux-build made from this same commit, `--verify-update <file>`, which is
# the check a Linux server's update helper runs as root and which shares its bundle
# reader, its trust list and its Rekor and Fulcio keys with the Windows server's.
# cosign's verify-blob in release-sign proves the bundle to cosign; this proves it to
# every server built after 6.3.3 (docs/UpdateFeed.md, "What the signing job produces").
#
#   bash build/ci/release-verify.sh
#
# In:   release-tools/hmailserver the server linux-build made from this commit, as the
#                                 release job kept it
#       release-signed/           the files and their bundles (release-sign's)
# Out:  release-verified/verification.md   the notes' verification section, which
#                                          release-publish puts in the release
#       release-verified/verify.log        what the verifier said of each file
set -uo pipefail

server=release-tools/hmailserver
signed=release-signed
out=release-verified
tag="${CI_COMMIT_TAG:-}"

fail() {
  echo "RELEASE-VERIFY FAILED: $*"
  exit 1
}

case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
# The project's path, which the certificate names: the personal namespace the project
# began in, or the group it is to move to - the two GitLab paths the server trusts, the
# group's only with the project's id, which a transfer keeps.
case "${CI_PROJECT_PATH:-}" in
  Progressiverobot/hmailserver) repository=https://gitlab.com/Progressiverobot/hmailserver ;;
  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;
  *) fail "the project's path is '${CI_PROJECT_PATH:-}', not one a server trusts a signature from" ;;
esac
identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"
[ -x "$server" ] || fail "$server is not here: the release job's artifact did not arrive"
[ -d "$signed" ] || fail "$signed is not here: release-sign's artifact did not arrive"

rm -rf "$out"
mkdir -p "$out"

# A configuration of its own, empty: the built-in trust and nothing an administrator
# could set (UpdateSigningIdentity and the rest would replace a signer).
scratch=$(mktemp -d)
printf '[Settings]\n' > "$scratch/hMailServer.ini"

"$server" --version 2>&1 | head -n 1
echo "the server verifies with its compiled-in trust; expecting $identity"

verified=0
rows=""
for file in "$signed"/*; do
  [ -f "$file" ] || continue
  case "$file" in *.cosign.bundle) continue ;; esac
  name=$(basename "$file")
  [ -f "$file.cosign.bundle" ] || fail "$name has no bundle beside it"
  said=$("$server" --config "$scratch/hMailServer.ini" --verify-update "$file" 2>&1)
  code=$?
  printf '%s\n' "$said" | tee -a "$out/verify.log"
  [ "$code" -eq 0 ] || fail "$name does not verify with the server's own verifier (exit $code)"
  # "<file> verifies: signed by <identity>, recorded by the transparency log at <time>."
  case "$said" in
    *"verifies: signed by $identity, recorded by the transparency log at "*) ;;
    *) fail "$name verifies, but not as signed by $identity" ;;
  esac
  # The time alone: the rest of that line, not whatever the verifier printed after it.
  when=${said##*recorded by the transparency log at }
  when=${when%%$'\n'*}
  when=${when%$'\r'}
  when=${when%.}
  rows="$rows| \`$name\` | \`$(sha256sum "$file" | cut -d' ' -f1)\` | $when |
"
  verified=$((verified + 1))
  # The same file with one byte more must be refused, or the check above proves nothing.
  cp "$file" "$scratch/$name"
  cp "$file.cosign.bundle" "$scratch/$name.cosign.bundle"
  printf 'x' >> "$scratch/$name"
  if "$server" --config "$scratch/hMailServer.ini" --verify-update "$scratch/$name" > /dev/null 2>&1; then
    fail "the verifier accepted $name with a byte appended: it is not checking"
  fi
  rm -f "$scratch/$name" "$scratch/$name.cosign.bundle"
done
rm -rf "$scratch"

[ "$verified" -eq 6 ] || fail "$verified file(s) verified; a release is 6"

# The repository id (Fulcio's 1.3.6.1.4.1.57264.1.15): for the path the project began
# at the server requires it when the certificate carries it, for the group's always, and
# the notes say which it was, from the installer's bundle. The first release says here
# whether Fulcio writes it for GitLab at all, which the move to the group waits for.
installer="hMailServer-${tag#v}-x64.exe"
repository_id=$(python3 - "$signed/$installer.cosign.bundle" <<'PY'
import base64, json, sys
bundle = json.load(open(sys.argv[1], encoding="utf-8"))
material = bundle["verificationMaterial"]
raw = (material.get("certificate") or material["x509CertificateChain"]["certificates"][0])["rawBytes"]
der = base64.b64decode(raw)
oid = bytes([0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x83, 0xBF, 0x30, 0x01, 0x0F])
at = der.find(oid)
if at < 0:
    print("absent")
else:
    # The extension: the OID, a critical flag when there is one (01 01 ff), then an
    # OCTET STRING whose content is a DER UTF8String.
    rest = der[at + len(oid):]
    if rest[0] == 0x01:
        rest = rest[3:]
    if rest[0] != 0x04 or rest[2] != 0x0C:
        print("unreadable")
    else:
        print(rest[4:4 + rest[3]].decode("utf-8", "replace"))
PY
)
case "$repository_id" in
  86730559) id_row="| Repository id | \`86730559\` |" ;;
  absent)
    # The server refused such a file above already where the path is the group's.
    [ "$repository" = "https://gitlab.com/Progressiverobot/hmailserver" ] \
      || fail "the certificate records no repository id, which a server requires for $repository"
    id_row="| Repository id | not in the certificate; a server checks it only when it is there |" ;;
  *) fail "the certificate names the repository id '$repository_id', not 86730559" ;;
esac

{
  echo "## Verification"
  echo ""
  echo "Every file of this release is signed with Sigstore by this project's GitLab CI pipeline for the tag \`$tag\`,"
  echo "and was checked before publication with \`hmailserver --verify-update\`, built from the same commit"
  echo "(\`${CI_COMMIT_SHA:-unknown}\`). Each file's bundle is beside it, named \`<file>.cosign.bundle\`. The certificate:"
  echo ""
  echo "| | |"
  echo "|---|---|"
  echo "| Identity | \`$identity\` |"
  echo "| Issuer | \`https://gitlab.com\` |"
  echo "| Repository | \`$repository\` |"
  echo "$id_row"
  echo ""
  echo "| File | SHA-256 | Recorded by the transparency log |"
  echo "|---|---|---|"
  printf '%s' "$rows"
  echo ""
  echo "To check a download with cosign:"
  echo ""
  echo "    cosign verify-blob --bundle <file>.cosign.bundle \\"
  echo "      --certificate-identity '$identity' \\"
  echo "      --certificate-oidc-issuer https://gitlab.com <file>"
  echo ""
  echo "The Windows installer is also signed with Authenticode through Azure Artifact Signing, with an RFC 3161 timestamp."
  echo ""
  echo "Unlike the releases up to 6.3.3, this one carries no AArch64 packages, no AppImages and no SLSA provenance:"
  echo "the pipeline builds on x86-64 only, the AppImage tool is published only on GitHub, and the provenance generator"
  echo "is GitHub's."
  echo ""
  echo "What a signature shows is which pipeline signed a file, not what the file was built from. Here, the \`.deb\`,"
  echo "the \`.rpm\` and the SBOMs were made by this pipeline in the runner's own checkout of \`${CI_COMMIT_SHA:-unknown}\`."
  echo "The Windows installer was made by this pipeline on the project's Windows runner, in that runner's long-lived"
  echo "checkout, which the job requires to be at that commit with no change to a tracked file and no file git does not"
  echo "track where a build would take it in, and from the folders the installer takes whole it removes every file git"
  echo "does not track before building. The server and the tools in it are that checkout's incremental build of the"
  echo "commit; its OpenSSL and libpq DLLs come from that runner's library tree and its C++ runtime from that runner's"
  echo "Visual Studio, both outside the pipeline; and nothing here proves the build reproducible."
} > "$out/verification.md"

echo ""
echo "All $verified files verify with the server's own verifier, and a changed copy of each is refused."
