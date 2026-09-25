#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-image job of .gitlab-ci.yml: the container image of
# hmailserver/source/Server/platform/packaging/docker/Dockerfile, built from the
# .deb the linux-build job made, and NOT published - publishing it is the
# release-chain row's (Roadmap2, "The release chain off a single forge"). It is
# the build half of the old GitHub workflow linux-build.yml's image job.
#
#   sh build/ci/linux-image.sh
#
# Built with kaniko, in kaniko's own image (gcr.io/kaniko-project/executor, the
# debug variant, whose busybox runs this script): a job on linix has no Docker
# daemon and no socket, and the runner is not privileged, which is what kaniko
# is for - it builds the image's filesystem inside its own container and writes
# the result to a tarball, with nothing pushed anywhere. POSIX sh for the same
# reason: there is no bash in that image.
#
# What it checks in the tarball, because nothing can run it here (that needs a
# daemon): the image runs as hmailserver, starts through the entrypoint, exposes
# the mail ports and the REST port, carries the healthcheck, and /usr/bin/hmailserver
# keeps the one capability the Dockerfile gives it with setcap - a builder that
# dropped the file's extended attributes would ship a server that cannot bind
# port 25 as the user it runs as. Running the image - the first start against a
# database, the restart, the REST answers - stays with the release chain, which
# will have a daemon to run it in.
#
# Leaves linux-image/image.json (the image's configuration) and
# linux-image/SHA256SUMS.txt; the tarball itself is removed at the end unless
# HM_KEEP_IMAGE=1.
set -eu

out="${HM_OUT_DIR:-linux-image}"
context=hmailserver/source/Server/platform/packaging/docker
executor="${HM_KANIKO:-/kaniko/executor}"

fail() {
  echo "FAILED: $*"
  exit 1
}

deb=$(ls linux-build/packages/hmailserver_*_amd64.deb 2> /dev/null | head -n 1 || true)
if [ -z "$deb" ]; then
  echo "REFUSED: no linux-build/packages/hmailserver_*_amd64.deb. The packaging step of the"
  echo "linux-build job made none; its artifact linux-build/package.log says why."
  exit 2
fi
version=$(basename "$deb" | sed 's/^hmailserver_\(.*\)_amd64\.deb$/\1/')
echo "package: $deb (version $version)"
# On a release tag the package must be the tag's version, as the old job checked.
case "${CI_COMMIT_TAG:-}" in
  v*) [ "${CI_COMMIT_TAG#v}" = "$version" ] || fail "the .deb is $version, and the tag is $CI_COMMIT_TAG" ;;
esac

# Only that one file goes into the context: the .rpm is not for the image.
rm -rf "$context/packages" "$out"
mkdir -p "$context/packages" "$out"
cp "$deb" "$context/packages/"

"$executor" version 2> /dev/null || true
"$executor" \
  --context "dir://$PWD/$context" \
  --dockerfile "$PWD/$context/Dockerfile" \
  --no-push \
  --destination "hmailserver:$version" \
  --tar-path "$out/hmailserver-image.tar" \
  --label "org.opencontainers.image.title=hMailServer" \
  --label "org.opencontainers.image.description=hMailServer, a mail server for SMTP, IMAP and POP3, on Linux" \
  --label "org.opencontainers.image.licenses=AGPL-3.0-or-later" \
  --label "org.opencontainers.image.vendor=Progressive Robot Ltd" \
  --label "org.opencontainers.image.version=$version" \
  --label "org.opencontainers.image.revision=${CI_COMMIT_SHA:-unknown}" \
  --cleanup
rm -rf "$context/packages"

echo ""
echo "== what was built"
ls -la "$out"
work="$out/unpacked"
mkdir -p "$work"
tar -xf "$out/hmailserver-image.tar" -C "$work"
config=$(sed -n 's/.*"Config":"\([^"]*\)".*/\1/p' "$work/manifest.json")
[ -n "$config" ] && [ -f "$work/$config" ] || fail "the tarball has no image configuration named in manifest.json"
cp "$work/$config" "$out/image.json"
tr ',' '\n' < "$out/image.json" | grep -E '"(User|Entrypoint|WorkingDir|Healthcheck|Test)"|/tcp' | head -n 20

grep -q '"User":"hmailserver"' "$out/image.json" || fail "the image does not run as hmailserver"
grep -q 'docker-entrypoint.sh' "$out/image.json" || fail "the image does not start through docker-entrypoint.sh"
grep -q '"Healthcheck"' "$out/image.json" || fail "the image carries no healthcheck"
for port in 25 110 143 465 587 993 995 4190 8045; do
  grep -q "\"$port/tcp\"" "$out/image.json" || fail "the image does not expose $port/tcp"
done
echo "runs as hmailserver, through the entrypoint, with the healthcheck and the nine ports"

# The layer that holds the binary, and the capability on it: tar writes a file's
# extended attributes as a SCHILY.xattr PAX record in front of it.
found=""
for layer in $(sed -n 's/.*"Layers":\[\([^]]*\)\].*/\1/p' "$work/manifest.json" | tr -d '"' | tr ',' ' '); do
  if zcat "$work/$layer" 2> /dev/null | tar -tf - 2> /dev/null | grep -qx 'usr/bin/hmailserver'; then
    found="$layer"
  fi
done
[ -n "$found" ] || fail "no layer holds usr/bin/hmailserver"
if zcat "$work/$found" | grep -c 'SCHILY.xattr.security.capability' > /dev/null; then
  echo "/usr/bin/hmailserver carries its file capability (security.capability) in layer $found"
else
  fail "/usr/bin/hmailserver lost the capability setcap gave it: the builder dropped the file's extended attributes"
fi

( cd "$out" && sha256sum hmailserver-image.tar image.json > SHA256SUMS.txt && cat SHA256SUMS.txt )
rm -rf "$work"
if [ "${HM_KEEP_IMAGE:-0}" != 1 ]; then
  rm -f "$out/hmailserver-image.tar"
fi
echo "IMAGE BUILT: hmailserver:$version, not pushed."
