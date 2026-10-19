#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# zypper refresh for the openSUSE jobs, made to survive a mirror that serves wrong
# files. rpm-build.sh (for the build container and the aarch64 sysroot),
# rpm-install-test.sh, srcpkg-rpm.sh and srcpkg-install.sh run it, as root in the
# job's own Leap or Tumbleweed container, before their first zypper install:
#
#   bash build/ci/packages/zypper-refresh.sh <log> [--root <dir>]
#
# zypper's output goes to <log>, emptied first; this prints nothing of its own
# unless it retries, and exits 0 when every repository refreshed, 1 otherwise.
# ZYPP_CONF in the environment reaches zypper (rpm-build.sh's aarch64 sysroot).
#
# Why. A repository whose baseurl is on download.opensuse.org or cdn.opensuse.org
# is one libzypp (17.38) asks there for the directory's ?mirrorlist; it then takes
# the repository's files from the mirrors listed, the first ones first, and when a
# file from a mirror fails its checksum it does not fetch it from another: zypper
# says "Digest verification failed", discards the file and reports "Repository
# '...' is invalid". It falls back from a mirror that refuses the connection, not
# from one that answers wrongly. On 19 October 2026 one listed mirror,
# opensuse.mirror-services.net, answered every path with the same unrelated page,
# and every openSUSE job that drew it failed, for hours. For the origin,
# downloadcontent.opensuse.org, libzypp asks no mirror list and takes every file
# from the origin itself (0 mirror-list requests that day, every repository
# refreshed).
#
# So the refresh is tried up to three times when it failed only to reach a
# repository, as the scripts' installs are; and when it failed on a digest or an
# invalid repository, every repository on download.opensuse.org or
# cdn.opensuse.org is pointed at downloadcontent.opensuse.org - the same paths,
# the same metadata signed by the same key, which the image already trusts and
# zypper verifies as before - one line says so and why, and the refresh is tried
# once more. Leap 16's repositories are made by a service (openSUSE, a repository
# index in the image itself, refreshed by every zypper command) that would write
# the old addresses back, so a service's automatic refresh is turned off first.
# zypper reports a repository invalid too when download.opensuse.org or
# cdn.opensuse.org itself could not be reached three times running, and the origin
# is then tried as well. Without such a failure nothing is changed.
set -uo pipefail

usage() {
  echo "usage: $0 <log> [--root <dir>]"
  exit 64
}
log=${1:-}
[ -n "$log" ] || usage
shift
root=""
global=()
case "$#" in
  0) ;;
  2) [ "$1" = --root ] && [ -n "$2" ] || usage; root=$2; global=(--root "$root") ;;
  *) usage ;;
esac
origin=downloadcontent.opensuse.org
network_failure='Curl error|Cannot download|Failed to download|Download \(curl\) error|Timeout exceeded|Timeout was reached|Problem retrieving|Could not resolve|Couldn.t resolve|Couldn.t connect|Connection (refused|timed out|reset)|Temporary failure'
mirror_failure="Digest verification failed|Repository '[^']*' is invalid"
attempt=$(mktemp)
trap 'rm -f "$attempt"' EXIT
: > "$log"

refresh() {
  local try
  for try in 1 2 3; do
    echo "== zypper -n${global[*]:+ ${global[*]}} refresh, try $try" >> "$log"
    zypper -n "${global[@]}" refresh > "$attempt" 2>&1
    local code=$?
    cat "$attempt" >> "$log"
    [ "$code" -eq 0 ] && return 0
    grep -q -E "$network_failure" "$attempt" || return 1
    echo "zypper refresh could not reach a repository, try $try of 3: $(grep -m 1 -E "$network_failure" "$attempt")"
    [ "$try" = 3 ] || sleep 15
  done
  return 1
}

refresh && exit 0
grep -q -E "$mirror_failure" "$attempt" || exit 1

shopt -s nullglob
repos=("$root"/etc/zypp/repos.d/*.repo)
services=("$root"/etc/zypp/services.d/*.service)
shopt -u nullglob
on_mirrors=()
for repo in "${repos[@]}"; do
  grep -q -E '^baseurl=https?://(download|cdn)\.opensuse\.org/' "$repo" && on_mirrors+=("$repo")
done
reason=$(grep -m 1 -o -E "Repository '[^']*' is invalid" "$attempt" || true)
[ -n "$reason" ] || reason="a repository was refused"
if grep -q 'Digest verification failed' "$attempt"; then
  reason="$reason (Digest verification failed: a mirror served a file that is not the one its metadata names)"
fi
if [ "${#on_mirrors[@]}" -eq 0 ]; then
  echo "zypper refresh: $reason, and no repository is on download.opensuse.org or cdn.opensuse.org, so there is no origin to turn to."
  exit 1
fi
if [ "${#services[@]}" -gt 0 ]; then
  echo "== zypper -n${global[*]:+ ${global[*]}} modifyservice --no-refresh --all" >> "$log"
  zypper -n "${global[@]}" modifyservice --no-refresh --all >> "$log" 2>&1 || exit 1
fi
sed -i -E "s#^(baseurl=https?://)(download|cdn)\.opensuse\.org/#\1$origin/#" "${on_mirrors[@]}" || exit 1
moved="the ${#on_mirrors[@]} repositories on download.opensuse.org or cdn.opensuse.org now take"
[ "${#on_mirrors[@]}" -eq 1 ] && moved="the repository on download.opensuse.org or cdn.opensuse.org now takes"
echo "zypper refresh: $reason; $moved every file from the origin, $origin, for which libzypp asks no mirror list, and the refresh is tried once more."
refresh
