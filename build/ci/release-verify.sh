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
# Then the rest of what the release carries beside its bundles: each compiled file's
# SBOM held to the file, each primary file's .sigstore.json held to its bundle, and
# the SLSA provenance held to the files (build/ci/release-chain.py check-provenance;
# release-sign found every file in it with cosign).
#
#   bash build/ci/release-verify.sh              a v* tag's release
#   bash build/ci/release-verify.sh --rehearse   the release-rehearsal-verify job of
#                                                .gitlab/ci/release-rehearsal.yml: signed as a
#                                                batch* branch, which the server's own
#                                                verifier must refuse - a rehearsal's bundles
#                                                install nowhere - after reading each bundle
#                                                as far as the identity
#
# In:   release-tools/hmailserver the server linux-build made from this commit, as the
#                                 release job kept it
#       release-signed/           the files and their bundles (release-sign's)
# Out:  release-verified/verification.md   the notes' verification section, which
#                                          release-publish puts in the release (not
#                                          in a rehearsal)
#       release-verified/verify.log        what the verifier said of each file
set -uo pipefail

server=release-tools/hmailserver
signed=release-signed
out=release-verified
tag="${CI_COMMIT_TAG:-}"
branch="${CI_COMMIT_BRANCH:-}"

fail() {
  echo "RELEASE-VERIFY FAILED: $*"
  exit 1
}

rehearse=""
case "${1:-}" in
  "") ;;
  --rehearse) rehearse=1 ;;
  *) fail "the one argument this takes is --rehearse, not '$1'" ;;
esac

if [ -z "$rehearse" ]; then
  case "$tag" in
    v[0-9]*) ;;
    *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
  esac
  version="${tag#v}"
  ref="refs/tags/${tag}"
else
  [ -z "$tag" ] || fail "a tag's pipeline is released, never rehearsed (CI_COMMIT_TAG is '$tag')"
  case "$branch" in
    batch*) ;;
    *) fail "a release is rehearsed on a protected batch* branch alone, not on '$branch'" ;;
  esac
  version=$(sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' \
    hmailserver/source/Server/Common/Application/Version.h | head -n 1 | tr -d '\r')
  [ -n "$version" ] || fail "Version.h's HMAILSERVER_VERSION could not be read"
  ref="refs/heads/${branch}"
fi
# The project's path, which the certificate names: the group's, where the project has
# been since 29 September 2026, and which a server trusts only with the project's id.
# The first path, Progressiverobot/hmailserver, signed 6.3.5; release-sign.sh signs
# nothing new there, and this verifies what it signs.
case "${CI_PROJECT_PATH:-}" in
  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;
  *) fail "the project's path is '${CI_PROJECT_PATH:-}', not one a server trusts a signature from" ;;
esac
identity="${repository}//.gitlab-ci.yml@${ref}"
[ -x "$server" ] || fail "$server is not here: the release job's artifact did not arrive"
[ -d "$signed" ] || fail "$signed is not here: release-sign's artifact did not arrive"

rm -rf "$out"
mkdir -p "$out"

# A configuration of its own, empty: the built-in trust and nothing an administrator
# could set (UpdateSigningIdentity and the rest would replace a signer).
scratch=$(mktemp -d)
printf '[Settings]\n' > "$scratch/hMailServer.ini"

"$server" --version 2>&1 | head -n 1
if [ -z "$rehearse" ]; then
  echo "the server verifies with its compiled-in trust; expecting $identity"
else
  echo "the server verifies with its compiled-in trust, which trusts no branch: expecting each file refused as signed by $identity"
fi

verified=0
rows=""
for file in "$signed"/*; do
  [ -f "$file" ] || continue
  case "$file" in *.cosign.bundle|*.sigstore.json) continue ;; esac
  name=$(basename "$file")
  [ -f "$file.cosign.bundle" ] || fail "$name has no bundle beside it"
  said=$("$server" --config "$scratch/hMailServer.ini" --verify-update "$file" 2>&1)
  code=$?
  printf '%s\n' "$said" | tee -a "$out/verify.log"
  if [ -n "$rehearse" ]; then
    # The verifier reads the bundle, the certificate's chain to Fulcio and the log's
    # entry and proof before it looks at the identity, and refuses at the identity
    # alone with these words (SigstoreVerifier.cpp): so the bundle is one a server
    # reads, and one no server installs.
    [ "$code" -ne 0 ] || fail "the server's verifier accepted $name, signed as the branch $branch: a rehearsal must install nowhere"
    case "$said" in
      *"The certificate names $identity, not this repository's release workflow."*) ;;
      *) fail "$name was refused before its identity was read, or for another reason: its bundle is not one a server reads" ;;
    esac
    verified=$((verified + 1))
    continue
  fi
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

# The sixty files a release carries (build/ci/release-chain.py, expected_assets).
[ "$verified" -eq 60 ] || fail "$verified file(s) verified; a release is 60"

# Each compiled file's own SBOM describes that very file (build/asset-sbom.py verify:
# well formed, and its subject's SHA-256 is the file's), as a user checks it
# (SECURITY.md, "Software bills of materials"); and the source tarball's. The source
# of the AppImages' runtime's libraries and of the libraries they carry in usr/lib has
# none: each is source, and its README.txt names each library or package, version,
# licence and where each file came from.
sbom_rows=""
described=0
for file in "$signed"/*.exe "$signed"/*.deb "$signed"/*.rpm "$signed"/*.AppImage "$signed"/*.tar.gz; do
  [ -f "$file" ] || continue
  name=$(basename "$file")
  case "$name" in
    *-appimage-runtime-sources.tar.gz | *-appimage-bundled-sources.tar.gz)
      [ ! -f "$file.cdx.json" ] || fail "$name is source and goes out without an SBOM, and $name.cdx.json is here"
      continue
      ;;
  esac
  [ -f "$file.cdx.json" ] || fail "$name goes out without its SBOM, $name.cdx.json"
  python3 build/asset-sbom.py verify "$file.cdx.json" "$file" > "$out/$name.sbom.txt" \
    || { cat "$out/$name.sbom.txt"; fail "$name.cdx.json does not describe $name"; }
  components=$(sed -n 's/^\([0-9][0-9]*\) components\.$/\1/p' "$out/$name.sbom.txt")
  echo "   $name.cdx.json describes $name ($components components)"
  sbom_rows="$sbom_rows| \`$name\` | \`$name.cdx.json\` | $components |
"
  described=$((described + 1))
done
[ "$described" -eq 27 ] || fail "$described file(s) held to their SBOM; a release has 27"

# Each primary file's .sigstore.json is its bundle, and the provenance names every
# file of the release by its SHA-256 and nothing else, built by this pipeline from
# this commit. Two calls, not one with an unquoted ${rehearse:+--rehearse}: the
# semgrep job counts that as a finding, and the release's gate refuses any it has
# not seen before.
if [ -n "$rehearse" ]; then
  python3 build/ci/release-chain.py check-provenance --assets "$signed" --rehearse \
    || fail "the .sigstore.json copies or the provenance are not what the rehearsal is"
else
  python3 build/ci/release-chain.py check-provenance --assets "$signed" \
    || fail "the .sigstore.json copies or the provenance are not what the release is"
fi

if [ -n "$rehearse" ]; then
  {
    echo "## Rehearsal of $version on $branch"
    echo ""
    echo "All $verified files were signed as \`$identity\`, which no server trusts: the server's own verifier read"
    echo "every bundle and refused each at the identity. Every SBOM describes its file, every .sigstore.json is its"
    echo "bundle, and the provenance names every file. Nothing was published."
  } > "$out/rehearsal.md"
  cat "$out/rehearsal.md"
  exit 0
fi

# The repository id (Fulcio's 1.3.6.1.4.1.57264.1.15), which a server requires at the
# group's path, and from 6.3.6 at the path the project began at too, and which the
# notes give from the installer's bundle. 6.3.5, the first release signed here, at the
# path the project began at, showed that Fulcio writes it for GitLab (86730559); the
# project moved into the group after it.
# Read from the certificate's extensions by build/ci/sigstore-certificate.py, never by
# a search of its bytes: its public key, bytes its holder chooses, could hold this
# project's id in front of the extension naming another's.
installer="hMailServer-${version}-x64.exe"
said=$(python3 build/ci/sigstore-certificate.py repository-id "$signed/$installer.cosign.bundle") \
  || fail "the certificate of $installer could not be read for the repository id: ${said#refused: }"
case "$said" in
  id=86730559) id_row="| Repository id | \`86730559\` |" ;;
  # The server refused such a file above already: at either path the id is required.
  absent) fail "the certificate records no repository id, which a server requires for $repository" ;;
  *) fail "the certificate names the repository id '${said#id=}', not 86730559" ;;
esac

provenance="hmailserver-${version}.intoto.sigstore.json"
{
  echo "## Verification"
  echo ""
  echo "Every file of this release is signed with Sigstore by this project's GitLab CI pipeline for the tag \`$tag\`,"
  echo "and was checked before publication with \`hmailserver --verify-update\`, built from the same commit"
  echo "(\`${CI_COMMIT_SHA:-unknown}\`). Each file's bundle is beside it, named \`<file>.cosign.bundle\`; each file's but"
  echo "an SBOM's is there a second time, byte for byte, as \`<file>.sigstore.json\`, the name Sigstore's own clients"
  echo "look for. The certificate:"
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
  echo "Every compiled file goes out with a software bill of materials of its own, in CycloneDX 1.6, named after it, which"
  echo "names that file by its SHA-256 and lists what it holds and needs - an AppImage's the libraries it carries, and the"
  echo "source tarball's the third-party code in it. The repository's \`build/asset-sbom.py verify\` checks that one"
  echo "describes the file beside it (SECURITY.md, \"Software bills of materials\"); each is signed like the rest."
  echo ""
  echo "| File | Its SBOM | Components |"
  echo "|---|---|---|"
  printf '%s' "$sbom_rows"
  echo "| the source tree | \`hmailserver.spdx.json\`, \`hmailserver.cyclonedx.json\` | |"
  echo ""
  echo "To check a download with cosign:"
  echo ""
  echo "    cosign verify-blob --bundle <file>.cosign.bundle \\"
  echo "      --certificate-identity '$identity' \\"
  echo "      --certificate-oidc-issuer https://gitlab.com <file>"
  echo ""
  echo "The release's SLSA v1 build provenance is \`$provenance\`: an in-toto statement that names every file above"
  echo "by its SHA-256, the commit it was built from and this pipeline as the builder, signed as the pipeline (a"
  echo "Sigstore bundle holding a DSSE envelope). To check that a download is one this pipeline built:"
  echo ""
  echo "    cosign verify-blob-attestation --new-bundle-format --bundle $provenance \\"
  echo "      --type slsaprovenance1 --certificate-identity '$identity' \\"
  echo "      --certificate-oidc-issuer https://gitlab.com <file>"
  echo ""
  echo "The two Windows installers, x64 and ARM64, are also signed with Authenticode through Azure Artifact Signing, with an"
  echo "RFC 3161 timestamp. The ARM64 one is for Windows on ARM; it is built to run there and has not yet been run on ARM64"
  echo "hardware (\`hmailserver/docs/WindowsOnArm.md\`)."
  echo ""
  echo "The Linux packages are the generic \`.deb\` and \`.rpm\` an installed server's update check asks for, for"
  echo "x86-64 and AArch64, and one \`.deb\` or \`.rpm\` for each supported distribution release on each: Debian 12"
  echo "and 13 and Ubuntu 24.04 (\`hmailserver_<version>-1~<release>_<arch>.deb\`), and Fedora 44 and 43, Enterprise"
  echo "Linux 10 and 9, openSUSE Leap 16.0 and Tumbleweed (\`hmailserver-<version>-1.<release>.<arch>.rpm\`). Each"
  echo "was built in that release's own container with its own compiler and libraries, the AArch64 ones cross-compiled"
  echo "there, and installed and run in a clean container of the release; the AArch64 ones under emulation"
  echo "(qemu-user), since no AArch64 machine has run them. The two AppImages, x86_64 and AArch64, carry the project's"
  echo "own AppImage runtime, linked statically from Alpine's packages; its libfuse3 is under the LGPL-2.1 and its lzo"
  echo "under the GPL-2.0-or-later. The runtime's source and build script are in the source tarball, and the source of"
  echo "every library linked into it, exactly as Alpine built it - each upstream archive from Alpine's distfiles mirror,"
  echo "and Alpine's build script and patches from aports at the commit its package records - is"
  echo "\`hmailserver-${version}-appimage-runtime-sources.tar.gz\`, which each AppImage names in"
  echo "\`usr/share/hmailserver/licenses/appimage-runtime.txt\` (\`hmailserver/docs/PackagesAppImage.md\`, \"Licences\")."
  echo "Each AppImage also carries in \`usr/lib\` the shared libraries the server needs that are not left to the host,"
  echo "libpq, libldap and what they need, among them GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils,"
  echo "which are under the LGPL: copied from Ubuntu 24.04's packages, with each package's copyright file and the"
  echo "licence texts those name, listed in \`usr/share/hmailserver/licenses/bundled.txt\`, beside hMailServer's own"
  echo "AGPL-3.0 text. The source of every one of them, exactly as Ubuntu built it - each source package at the version"
  echo "the image's package was built from, from Ubuntu's archive - is"
  echo "\`hmailserver-${version}-appimage-bundled-sources.tar.gz\`, which \`bundled.txt\` names."
  echo "The source tarball holds nothing that is not free"
  echo "(\`hmailserver/docs/SourceTarball.md\`), and the Helm chart names this release's image."
  echo ""
  echo "What a signature shows is which pipeline signed a file, not what the file was built from. Here, the x86-64"
  echo "\`.deb\` and \`.rpm\` and their SBOMs were made by this pipeline in the runner's own checkout of"
  echo "\`${CI_COMMIT_SHA:-unknown}\`, in the CI image, from the source tarball; every other Linux package and the"
  echo "AppImages in that checkout too, each in its distribution's own container, and their SBOMs from what those"
  echo "containers recorded the build was built against; the runtime's libraries' source fetched there, in an Alpine"
  echo "container, from Alpine's distfiles mirror and aports, each file held to the SHA-512 its APKBUILD gives it, and"
  echo "packed twice to the same bytes; the bundled libraries' source fetched there, in an Ubuntu container, from"
  echo "Ubuntu's archive (or its snapshot service), each file held to the checksums of the signed index and of its"
  echo "\`.dsc\`, and packed twice to the same bytes; each installer's SBOM from that installer as Authenticode signed it."
  echo "The ARM64 installer was made by this pipeline on the project's Windows runner, in the runner's own clean checkout"
  echo "of the commit, cross-compiled with Visual Studio's ARM64 tools. Its ARM64 OpenSSL, Boost and libpq come from a"
  echo "cache on that runner, built once from their pinned source archives by the same job's script and kept, and its"
  echo "C++ runtime from that runner's Visual Studio, both outside the pipeline; every program and library it packs was"
  echo "checked before it was packed to be ARM64 code or .NET that runs there, but Microsoft's ARM64EC"
  echo "\`vcruntime140_1.dll\`, which an ARM64 process does not load (\`hmailserver/docs/PackagesWindows.md\`); and nothing"
  echo "here proves that build reproducible."
  echo "The x64 Windows installer was made by this pipeline on the project's Windows runner, in that runner's long-lived"
  echo "checkout, which the job requires to be at that commit with no change to a tracked file and no file git does not"
  echo "track where a build would take it in, and from the folders the installer takes whole it removes every file git"
  echo "does not track before building. The server and the tools in it are that checkout's incremental build of the"
  echo "commit; its OpenSSL and libpq DLLs come from that runner's library tree and its C++ runtime from that runner's"
  echo "Visual Studio, both outside the pipeline; and nothing here proves that build reproducible. The x86-64 Linux one"
  echo "is checked: \`linux-repro\`, which the release waits for, built the server, the \`.deb\` and the \`.rpm\` a"
  echo "second time from the same commit in the same image, from a copy of the tree at another path, and both packages"
  echo "were byte for byte the files above (\`hmailserver/docs/ReproducibleBuilds.md\`). The other packages and the"
  echo "AppImages were built once, and no second build has been compared with them."
} > "$out/verification.md"

echo ""
echo "All $verified files verify with the server's own verifier, and a changed copy of each is refused."
