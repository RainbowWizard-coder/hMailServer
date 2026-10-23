#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The deb-<distro>-<arch>-install jobs of .gitlab/ci/packages-deb.yml: one
# package deb-build.sh made, installed in a CLEAN container of the release it
# was built for and of its own architecture - an arm64 container runs under
# qemu-user, through the binfmt handler registered on the host - and held to
# what an installation needs:
#
#   * apt installs it, resolving its Depends from that release's archive and
#     nothing else, and the installed Version is the file's;
#   * no library the binary needs is missing (ldd), which is what a Depends line
#     written on another release gets wrong;
#   * hmailserver --version runs on that architecture and answers the version;
#   * every file an installation needs is where the package says, the three
#     systemd units among them, and postinst made the service's user and
#     enabled its units the way deb-systemd-helper does without systemd running;
#   * the licences are there as the tree has them: the AGPL-3.0 text, the notices
#     of QuickJS, Unicode's data and HTML's named character references, and
#     /usr/share/doc/hmailserver/copyright, which the image's dpkg keeps
#     where it drops the rest of /usr/share/doc (check-licenses.sh);
#   * apt removes it again.
#
# What it does NOT do: start the server against a database and ask the REST API.
# build/ci/linux-package-install.sh does that for the two generic packages named
# for Ubuntu 26.04 alone: the amd64 one in the linux-package-deb job, and the arm64
# one this file's jobs make, the file a release carries, in
# deb-ubuntu26.04-arm64-start; a container has no systemd, so none of these jobs
# proves the unit's own start (that script says what a container cannot prove).
#
#   HM_DEB_DISTRO=debian13 HM_DEB_ARCH=arm64 bash build/ci/packages/deb-install.sh
#
# from the root of a checkout, as root, in the release's own container of the
# package's architecture, with deb-build.sh's output in HM_PACKAGES_DIR
# (packages-deb/<distro>-<arch> by default). Leaves install-<distro>-<arch>.txt
# there with what it found.
set -uo pipefail

distro="${HM_DEB_DISTRO:-}"
arch="${HM_DEB_ARCH:-}"
dir="${HM_PACKAGES_DIR:-packages-deb/$distro-$arch}"

refuse() {
  echo "REFUSED: $*"
  exit 2
}
fail() {
  echo "FAILED: $*"
  exit 1
}
step() {
  echo ""
  echo "== $*"
}

[ -n "$distro" ] && [ -n "$arch" ] || refuse "set HM_DEB_DISTRO and HM_DEB_ARCH."
[ "$(id -u)" = 0 ] || refuse "run this as root in a clean container: it installs the package."
. /etc/os-release
[ "$ID$VERSION_ID" = "$distro" ] || refuse "the package is for $distro, but this container is $PRETTY_NAME."
here=$(dpkg --print-architecture)
[ "$here" = "$arch" ] || refuse "the package is for $arch, but this container is $here: an arm64 package is tested in an arm64 container, under qemu."

debs=$(ls "$dir"/hmailserver_*_"$arch".deb 2> /dev/null)
[ "$(echo "$debs" | grep -c .)" = 1 ] \
  || refuse "not exactly one hmailserver_*_$arch.deb in $dir (found: ${debs:-none}). The build job made none; its artifacts say why."
deb=$debs
version=$(sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' \
  hmailserver/source/Server/Common/Application/Version.h)
report="$dir/install-$distro-$arch.txt"
started=$(date +%s)
echo "$PRETTY_NAME, $(uname -m) ($here), $(basename "$deb")" | tee "$report"

step "install it, with what apt resolves from $PRETTY_NAME"
export DEBIAN_FRONTEND=noninteractive
echo "Depends: $(dpkg-deb -f "$deb" Depends)"
# apt over IPv4 only. The job containers on linix have no IPv6 route, and under
# qemu-user (the arm64 jobs) apt's fall-back from the mirror's IPv6 address to its
# IPv4 one often gives up with "Unable to connect to deb.debian.org:80:". In clean
# debian:13 arm64 containers on linix on 26 September 2026 (Ubuntu's qemu-user
# 10.2.1) that happened in 5 of 21, Acquire::Retries=3 or not, and in none of 24
# with IPv4 forced. In debian:13 amd64 containers, without emulation, it happened
# in 1 of 40, and in 1 of 90 with IPv4 forced: that rest is what the retries below
# are for.
apt_opts=(-o Acquire::ForceIPv4=true -o Acquire::Retries=3)
# And apt-get update exits 0 when it could not fetch an index ("W: Failed to
# fetch", "W: Some index files failed to download"), leaving the install to fail
# on a Depends it has no candidate for, as if the package were wrong. Such a
# warning fails the update here, and the update is tried three times.
updated=""
for try in 1 2 3; do
  if apt-get update -qq "${apt_opts[@]}" > /tmp/apt.log 2>&1 \
      && ! grep -q -E '^(W: Failed to fetch|W: Some index files failed|E: )' /tmp/apt.log; then
    updated=yes
    break
  fi
  echo "apt-get update, try $try of 3: $(grep -E '^(W|E): ' /tmp/apt.log | head -n 1)"
  [ "$try" = 3 ] || sleep 15
done
[ -n "$updated" ] || { tail -n 20 /tmp/apt.log; fail "apt-get update: $PRETTY_NAME's archive could not be read, so nothing is known about the package"; }
# The same connect failure can meet the install's downloads: an install that
# failed only to download is tried again, up to three times. One that failed
# otherwise - a Depends the release cannot meet - fails at once.
installed_ok=""
for try in 1 2 3; do
  if apt-get install -y -qq "${apt_opts[@]}" --no-install-recommends "./$deb" > /tmp/install.log 2>&1; then
    installed_ok=yes
    break
  fi
  grep -q -E 'Failed to fetch|Unable to connect|Temporary failure resolving|Could not resolve' /tmp/install.log || break
  echo "apt-get install, try $try of 3: $(grep -E 'Failed to fetch|Unable to connect' /tmp/install.log | head -n 1)"
  [ "$try" = 3 ] || sleep 15
done
[ -n "$installed_ok" ] || { tail -n 30 /tmp/install.log; fail "apt-get install of $(basename "$deb") on $PRETTY_NAME"; }
installed=$(dpkg-query -W -f '${Version} ${Architecture} ${Status}' hmailserver)
echo "installed: $installed"
[ "$installed" = "$(dpkg-deb -f "$deb" Version) $arch install ok installed" ] \
  || fail "dpkg says '$installed', not the file's Version and Architecture, installed"

step "every library the binary needs is there"
missing=$(ldd /usr/bin/hmailserver 2>&1 | grep -F 'not found' || true)
ldd /usr/bin/hmailserver | sed 's/ (0x[0-9a-f]*)$//'
[ -z "$missing" ] || fail "libraries not found: $missing"

step "it runs on $arch and answers the version"
answer=$(/usr/bin/hmailserver --version 2>&1) || fail "/usr/bin/hmailserver --version exited $?: $answer"
echo "$answer"
case "$answer" in
  "hMailServer $version build "*) ;;
  *) fail "--version answered '$answer', not hMailServer $version" ;;
esac
echo "version $answer" >> "$report"

step "every file an installation needs"
count=0
for f in \
    /usr/bin/hmailserver /usr/bin/hmctl /usr/bin/dh2048.pem \
    /usr/share/hmailserver/DBScripts/CreateTablesPGSQL.sql \
    /usr/share/hmailserver/DBScripts/CreateTablesMySQL.sql \
    /usr/share/hmailserver/WebAdmin/index.html \
    /usr/lib/systemd/system/hmailserver.service \
    /usr/lib/systemd/system/hmailserver-update.path \
    /usr/lib/systemd/system/hmailserver-update.service \
    /usr/lib/hmailserver/hmailserver-update /usr/lib/hmailserver/autoban-hook \
    /etc/hmailserver/hMailServer.ini /etc/logrotate.d/hmailserver; do
  [ -f "$f" ] || fail "$f is not installed"
  count=$((count + 1))
done
echo "all $count present, the three systemd units among them"
cmp /usr/share/hmailserver/WebAdmin/index.html hmailserver/installation/WebAdmin/index.html \
  || fail "the installed Control Deck is not the repository's"

step "the licences it goes out under, as installed"
bash build/ci/packages/check-licenses.sh --installed deb || fail "the installed licence files (above)"

step "what postinst did with no systemd running"
getent passwd hmailserver || fail "postinst made no hmailserver user"
stat -c "%a %U:%G %n" /etc/hmailserver/hMailServer.ini /var/lib/hmailserver /var/log/hmailserver
# The mail, the logs and the configuration - the database password and the
# administrator password's hash - are the service's and root's alone, as the .rpm's
# test holds its own to: nobody else may read any of them.
[ "$(stat -c '%a %U:%G' /etc/hmailserver)" = "750 root:hmailserver" ] || fail "/etc/hmailserver is $(stat -c '%a %U:%G' /etc/hmailserver), not 750 root:hmailserver"
[ "$(stat -c '%a %U:%G' /etc/hmailserver/hMailServer.ini)" = "640 root:hmailserver" ] || fail "the configuration is $(stat -c '%a %U:%G' /etc/hmailserver/hMailServer.ini), not 640 root:hmailserver"
for d in /var/lib/hmailserver /var/log/hmailserver; do
  [ "$(stat -c '%a %U:%G' "$d")" = "750 hmailserver:hmailserver" ] || fail "$d is $(stat -c '%a %U:%G' "$d"), not 750 hmailserver:hmailserver"
done
# And as packed, before postinst: never readable by others.
[ "$(dpkg-deb -c "$deb" | awk '$NF == "./etc/hmailserver/hMailServer.ini" { print $1 }')" = "-rw-r-----" ] || fail "the .deb packs hMailServer.ini readable by others: $(dpkg-deb -c "$deb" | grep hMailServer.ini)"
if command -v deb-systemd-helper > /dev/null 2>&1; then
  for unit in hmailserver.service hmailserver-update.path; do
    [ -L "/etc/systemd/system/multi-user.target.wants/$unit" ] || fail "postinst did not enable $unit"
    echo "$unit: enabled"
  done
else
  echo "no deb-systemd-helper in this image: the units are installed and not enabled, as postinst does then"
fi

step "and it comes off again"
apt-get remove -y -qq hmailserver > /tmp/remove.log 2>&1 || { tail -n 20 /tmp/remove.log; fail "apt-get remove"; }
[ ! -e /usr/bin/hmailserver ] || fail "/usr/bin/hmailserver is still there after apt-get remove"

echo "seconds $(( $(date +%s) - started ))" >> "$report"
echo ""
echo "PACKAGE PASSED: $(basename "$deb") installs on $PRETTY_NAME $arch, runs, carries its units, and comes off again ($(( $(date +%s) - started )) s)."
