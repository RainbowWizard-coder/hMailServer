#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-image job of .gitlab-ci.yml: the container image of
# hmailserver/source/Server/platform/packaging/docker/Dockerfile, built from the
# .deb the linux-build job made, and not published. It is the build half of the
# old GitHub workflow linux-build.yml's image job.
#
#   sh build/ci/linux-image.sh
#
# And the container-image job of .gitlab/ci/container-image.yml, with
# HM_IMAGE_PUSH=1: the same image built from the .deb a v* tag's release carries,
# as container-image-deb took it from the published release and held it to its
# Sigstore bundle (HM_IMAGE_DEB_DIR), and pushed to the project's container
# registry as :<version>, and for a release without a suffix as :<major>.<minor>
# and :latest too - only while the project's CI/CD variable HM_PUBLISH_IMAGE is 1,
# which is checked here as the job runs, not when the pipeline is made, and only at
# the group's path, registry.gitlab.com/hmailserver/hmailserver
# (hmailserver/docs/ContainerImage.md). The job's own registry
# credentials (CI_REGISTRY_USER and CI_REGISTRY_PASSWORD, which the job token has
# for its own project) go into kaniko's configuration and nowhere else. It leaves
# <out>/image.env, a dotenv report naming the image by digest, for the jobs that
# sign and scan it.
#
# Built with kaniko, in kaniko's own image (gcr.io/kaniko-project/executor, the
# debug variant, whose busybox runs this script): a job on linix has no Docker
# daemon and no socket, and the runner is not privileged, which is what kaniko
# is for - it builds the image's filesystem inside its own container and writes
# the result to a tarball, and pushes it only when told to. POSIX sh for the same
# reason: there is no bash in that image.
#
# kaniko cannot push a tarball it wrote before, so in push mode one run both pushes
# the image and writes the tarball of the very image it pushed, and the checks below
# read that. A check that fails then fails the job after the push: nothing signs or
# scans the image, and the job says which tags to delete. The same checks passed on
# the same Dockerfile and .deb in linux-image earlier in the tag's pipeline.
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
# linux-image/SHA256SUMS.txt (HM_OUT_DIR names another folder), and in push mode
# digest.txt and image.env beside them; the tarball itself is removed at the end
# unless HM_KEEP_IMAGE=1.
set -eu

out="${HM_OUT_DIR:-linux-image}"
context=hmailserver/source/Server/platform/packaging/docker
executor="${HM_KANIKO:-/kaniko/executor}"
push="${HM_IMAGE_PUSH:-0}"
pushed=""

fail() {
  echo "FAILED: $*"
  if [ -n "$pushed" ]; then
    echo "The image was pushed before this check failed, and nothing will sign or scan it. These"
    echo "tags now name it; delete them from the container registry (Deploy > Container registry):"
    echo "$pushed" | tr ' ' '\n' | sed '/^$/d; s/^/  /'
  fi
  exit 1
}

if [ "$push" = 1 ]; then
  # The switch, read as the job runs: the jobs exist in every v* tag's pipeline, so
  # that a release published before the project's move into the group (6.3.5) can
  # still have its image pushed from its own pipeline afterwards, and nothing is
  # pushed until the project's variable says so.
  if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
    echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so no image is pushed."
    echo "It is set by the maintainers for the image at registry.gitlab.com/hmailserver/hmailserver"
    echo "(hmailserver/docs/ContainerImage.md, 'Publishing'); then run the image jobs again."
    exit 3
  fi
  # A release's image: the .deb of the tag's version, from the verified release files
  # and nowhere else. The name is linux-build's, never a distribution-suffixed one.
  tag="${CI_COMMIT_TAG:-}"
  case "$tag" in
    v[0-9]*) ;;
    *) fail "HM_IMAGE_PUSH=1 is for a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
  esac
  deb="${HM_IMAGE_DEB_DIR:-container-image-deb}/hmailserver_${tag#v}_amd64.deb"
  if [ ! -f "$deb" ]; then
    echo "REFUSED: no $deb. container-image-deb's artifact holds the release's .deb, taken from"
    echo "the published release and held to its Sigstore bundle; it did not arrive."
    exit 2
  fi
else
  deb=$(ls linux-build/packages/hmailserver_*_amd64.deb 2> /dev/null | head -n 1 || true)
  if [ -z "$deb" ]; then
    echo "REFUSED: no linux-build/packages/hmailserver_*_amd64.deb. The packaging step of the"
    echo "linux-build job made none; its artifact linux-build/package.log says why."
    exit 2
  fi
fi
version=$(basename "$deb" | sed 's/^hmailserver_\(.*\)_amd64\.deb$/\1/')
echo "package: $deb (version $version)"
# On a release tag the package must be the tag's version, as the old job checked.
case "${CI_COMMIT_TAG:-}" in
  v*) [ "${CI_COMMIT_TAG#v}" = "$version" ] || fail "the .deb is $version, and the tag is $CI_COMMIT_TAG" ;;
esac

# Where the image goes. Not pushed: a local name kaniko needs for the tarball. Pushed:
# the project's own repository at the group's path, the image's lasting address, which
# the Compose file and the chart name, with the version's tag always, and the minor
# line's and latest only for a release without a suffix - a pre-release never moves
# them. Never anywhere else: a container repository's path is always its project's,
# and the maintainers decided on 27 September 2026 to push the first image only once
# the project was in the group, so that its address would not change with the move.
# The project moved on 29 September 2026; a pipeline whose registry is the first
# path, registry.gitlab.com/progressiverobot/hmailserver, is another project's now.
if [ "$push" = 1 ]; then
  repository="${CI_REGISTRY_IMAGE:-}"
  case "$repository" in
    registry.gitlab.com/hmailserver/hmailserver) ;;
    *) fail "the project's registry image is '$repository', not registry.gitlab.com/hmailserver/hmailserver, which the Compose file and the chart name" ;;
  esac
  printf '%s\n' "$version" | grep -Eqx '[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}' || fail "$version is not a container image tag"
  tags="$version"
  if printf '%s\n' "$version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+'; then
    tags="$version ${version%.*} latest"
  fi
  [ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
    || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD: the project's container registry is off"
  # kaniko reads its credentials from DOCKER_CONFIG (/kaniko/.docker/ in its image).
  # The password goes into that file and is never echoed; the file is removed once
  # kaniko has pushed, and on any exit before that. It has a name of its own,
  # registry_config: the checks below reuse "config" for the image's configuration,
  # and the trap reads the variable when it runs, not when it is set.
  registry_config="${DOCKER_CONFIG:-/kaniko/.docker}"
  mkdir -p "$registry_config"
  auth=$(printf '%s:%s' "$CI_REGISTRY_USER" "$CI_REGISTRY_PASSWORD" | base64 | tr -d '\n')
  ( umask 077 && printf '{"auths":{"%s":{"auth":"%s"}}}\n' "$CI_REGISTRY" "$auth" > "$registry_config/config.json" )
  auth=""
  trap 'rm -f "$registry_config/config.json"' EXIT
  destinations=""
  for t in $tags; do
    destinations="$destinations --destination $repository:$t"
  done
  echo "pushing to: $(echo "$destinations" | sed 's/ --destination / /g')"
  # $destinations is "--destination <image>:<tag>" per tag, each tag held to
  # [A-Za-z0-9_.-] above, split on purpose: kaniko's busybox sh has no arrays.
  # shellcheck disable=SC2086
  set -- $destinations --digest-file "$out/digest.txt" --push-retry 3 # nosemgrep
else
  set -- --no-push --destination "hmailserver:$version"
fi

# Only that one file goes into the context: the .rpm is not for the image.
rm -rf "$context/packages" "$out"
mkdir -p "$context/packages" "$out"
cp "$deb" "$context/packages/"

# The labels that change with the build; the Dockerfile has the rest. created is the
# commit's time, so that two builds of one commit say the same (a build outside CI has
# no commit time, and says when it was made). Artifact Hub reads created, and
# prerelease for a version with a suffix.
created="${CI_COMMIT_TIMESTAMP:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
case "$version" in
  *[!0-9.]*) prerelease=true ;;
  *) prerelease=false ;;
esac

"$executor" version 2> /dev/null || true
# HM_KANIKO_FLAGS is for a test against a registry of one's own (--insecure); the jobs
# never set it. It is split into words on purpose: busybox sh has no arrays.
# shellcheck disable=SC2086
# nosemgrep
"$executor" \
  --context "dir://$PWD/$context" \
  --dockerfile "$PWD/$context/Dockerfile" \
  "$@" ${HM_KANIKO_FLAGS:-} \
  --tar-path "$out/hmailserver-image.tar" \
  --label "org.opencontainers.image.version=$version" \
  --label "org.opencontainers.image.revision=${CI_COMMIT_SHA:-unknown}" \
  --label "org.opencontainers.image.created=$created" \
  --label "io.artifacthub.package.prerelease=$prerelease" \
  --cleanup
rm -rf "$context/packages"
if [ "$push" = 1 ]; then
  rm -f "$registry_config/config.json"
  for t in $tags; do
    pushed="$pushed $repository:$t"
  done
fi

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
# The install kind the server's update check reads, and the three labels Artifact Hub
# will not list an image without.
grep -q '"HM_INSTALL_KIND=container"' "$out/image.json" || fail "the image does not set HM_INSTALL_KIND=container for the update check"
for label in io.artifacthub.package.readme-url org.opencontainers.image.created org.opencontainers.image.description \
             org.opencontainers.image.source org.opencontainers.image.licenses; do
  grep -q "\"$label\":\"[^\"]" "$out/image.json" || fail "the image carries no $label label"
done
grep -q "\"org.opencontainers.image.version\":\"$version\"" "$out/image.json" || fail "the image's version label is not $version"
echo "HM_INSTALL_KIND=container, and the labels a registry and Artifact Hub read, version $version"

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
if [ "$push" != 1 ]; then
  echo "IMAGE BUILT: hmailserver:$version, not pushed."
  exit 0
fi

# What the jobs after this one read: the image by its digest, which every tag above
# names. A dotenv report (artifacts:reports:dotenv), one KEY=value a line; CS_IMAGE is
# the name GitLab's container scanning reads the image to scan from.
digest=$(cat "$out/digest.txt" 2> /dev/null || true)
printf '%s\n' "$digest" | grep -Eqx 'sha256:[0-9a-f]{64}' || fail "kaniko wrote no digest of what it pushed ('$digest')"
{
  echo "HM_IMAGE=$repository@$digest"
  echo "HM_IMAGE_TAGS=$(printf '%s' "$tags" | tr ' ' ',')"
  echo "CS_IMAGE=$repository@$digest"
} > "$out/image.env"
cat "$out/image.env"
echo "IMAGE PUSHED: $repository@$digest, as$pushed."
