#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The rpm-<distribution>-<arch>-install jobs of .gitlab/ci/packages-rpm.yml: the
# package rpm-build.sh made for one distribution and architecture, installed in
# a clean container of that same distribution and architecture - the job's own
# image, a stock distribution image, run as aarch64 under qemu on linix for the
# aarch64 packages - by the distribution's own package manager, which has to
# resolve every Requires from the distribution's own repositories. Then:
#
#   hmailserver --version answers, from the installed binary;
#   every file a working installation needs is where the package put it;
#   the licences are there as the tree has them - the AGPL-3.0 text, QuickJS's
#   and /usr/share/licenses/hmailserver/copyright - installed under the image's
#   tsflags=nodocs, and each one rpm -qL names (check-licenses.sh);
#   %post made the service account and the directories, and left the
#   configuration root-owned and readable by the service only;
#   the Control Deck is the repository's, byte for byte;
#   and the package comes off again.
#
#   bash build/ci/packages/rpm-install-test.sh <target> <arch>
#
# Unlike build/ci/linux-package-install.sh's rpm half, nothing is installed with
# --nodeps: the whole point of a per-distribution build is that it installs
# with its dependencies on that distribution. What a container cannot prove is
# the start under systemd (linux-package-install.sh says what that leaves out).
set -uo pipefail

target="${1:-}"
arch="${2:-}"
packages="${HM_OUT_ROOT:-packages-rpm}/$target-$arch"

case "$target" in
  fedora44|fedora43|el10|el9) family=dnf ;;
  leap16|tumbleweed) family=zypper ;;
  *) echo "usage: $0 <fedora44|fedora43|el10|el9|leap16|tumbleweed> <x86_64|aarch64>"; exit 64 ;;
esac
case "$arch" in x86_64|aarch64) ;; *) echo "arch is x86_64 or aarch64"; exit 64 ;; esac

. /etc/os-release
echo "== $PRETTY_NAME on $(uname -m)"
if [ "$(uname -m)" != "$arch" ]; then
  echo "REFUSED: an $arch package is tested in an $arch container; this one is $(uname -m)."
  exit 2
fi

rpm_file=$(ls "$packages"/hmailserver-*."$arch".rpm 2> /dev/null | head -n 1)
if [ -z "$rpm_file" ]; then
  echo "REFUSED: no hmailserver-*.$arch.rpm in $packages; the rpm-$target-$arch job made none (its log says why)."
  exit 2
fi
echo "== $(basename "$rpm_file")"
rpm -qp --requires "$rpm_file" | grep -v '^rpmlib(' | sed 's/^/  requires /'

REQUIRED_FILES="
/usr/bin/hmailserver
/usr/bin/hmctl
/usr/bin/dh2048.pem
/usr/share/hmailserver/DBScripts/CreateTablesPGSQL.sql
/usr/share/hmailserver/DBScripts/CreateTablesMySQL.sql
/usr/share/hmailserver/WebAdmin/index.html
/usr/share/hmailserver/WebAdmin/languages/de.json
/usr/lib/systemd/system/hmailserver.service
/usr/lib/systemd/system/hmailserver-update.path
/usr/lib/systemd/system/hmailserver-update.service
/usr/lib/hmailserver/hmailserver-update
/usr/lib/hmailserver/autoban-hook
/etc/hmailserver/hMailServer.ini
/etc/logrotate.d/hmailserver
"

# The package manager's install, tried up to three times when it failed only to
# reach the repository, and failed at once otherwise: a Requires the release
# cannot meet is what this test is for (rpm-build.sh says more).
network_failure='Curl error|Cannot download|Failed to download|Download \(curl\) error|Timeout exceeded|Problem retrieving|Could not resolve|Couldn.t resolve|Couldn.t connect|Connection (refused|timed out|reset)'
retry_fetch() {
  local log=$1 try
  shift
  for try in 1 2 3; do
    "$@" > "$log" 2>&1 && return 0
    grep -q -E "$network_failure" "$log" || return 1
    echo "the repository could not be reached, try $try of 3: $(grep -m 1 -E "$network_failure" "$log")"
    [ "$try" = 3 ] || sleep 15
  done
  return 1
}
# zypper refreshes its repositories by itself before it installs, and trusts
# only the keys the image already holds: nothing is auto-imported.
install_rpm() {
  case "$family" in
    dnf) retry_fetch "$2" dnf -y -q --setopt=install_weak_deps=False install "./$1" ;;
    zypper) retry_fetch "$2" zypper -n install --no-recommends --allow-unsigned-rpm "./$1" ;;
  esac
}

set -e
started=$(date +%s)
echo ""
echo "== install it, with what $family resolves for it from $PRETTY_NAME's repositories"
install_rpm "$rpm_file" /tmp/install.log \
  || { cat /tmp/install.log; echo "FAILED: $family could not install $(basename "$rpm_file")"; exit 1; }
grep -iE 'installing|installed|hMailServer is installed' /tmp/install.log | head -n 5 || true
echo "installed in $(( $(date +%s) - started )) s"

echo ""
echo "== hmailserver --version"
/usr/bin/hmailserver --version

echo ""
echo "== every file a working installation needs"
missing=0
for f in $REQUIRED_FILES; do
  if [ -f "$f" ]; then echo "  present  $f"; else echo "  MISSING  $f"; missing=$((missing + 1)); fi
done
[ "$missing" -eq 0 ] || { echo "FAILED: $missing file(s) missing"; exit 1; }

echo ""
echo "== the licences it goes out under, as installed"
bash build/ci/packages/check-licenses.sh --installed rpm || { echo "FAILED: the installed licence files (above)"; exit 1; }

echo ""
echo "== what %post did"
getent passwd hmailserver
ls -ld /var/lib/hmailserver /var/log/hmailserver /etc/hmailserver
owner=$(stat -c '%U:%G %a' /etc/hmailserver/hMailServer.ini)
echo "hMailServer.ini: $owner"
[ "$owner" = "root:hmailserver 640" ] || { echo "FAILED: the configuration is $owner, not root:hmailserver 640"; exit 1; }
# grep reads the whole list rather than -q stopping at the first match: under
# pipefail an early exit can leave rpm writing into a closed pipe and fail a
# package that is right.
rpm -qc hmailserver | grep -x /etc/hmailserver/hMailServer.ini > /dev/null || { echo "FAILED: hMailServer.ini is not a %config file"; exit 1; }
installed=$(sha256sum < /usr/share/hmailserver/WebAdmin/index.html)
repository=$(sha256sum < hmailserver/installation/WebAdmin/index.html)
[ "$installed" = "$repository" ] || { echo "FAILED: the installed Control Deck is not the repository's"; exit 1; }
echo "the installed Control Deck is the repository's, byte for byte"
# As the service account where the image has runuser. Fedora's images carry
# util-linux-core, which has no runuser (nor setpriv or su), so there it ran as
# root only, and this says so. Where runuser is there, a failure fails the test.
if command -v runuser > /dev/null 2>&1; then
  runuser -u hmailserver -- /usr/bin/hmailserver --version > /dev/null 2>&1 \
    || { echo "FAILED: the service account cannot run /usr/bin/hmailserver --version"; exit 1; }
  echo "and the service account can run it"
else
  echo "(this image has no runuser, so --version ran as root only, above)"
fi

echo ""
echo "== and it comes off again"
rpm -e hmailserver > /tmp/remove.log 2>&1 || { cat /tmp/remove.log; echo "FAILED: rpm -e"; exit 1; }
test ! -f /usr/bin/hmailserver || { echo "FAILED: /usr/bin/hmailserver is still there after rpm -e"; exit 1; }
echo "removed"

# Fedora 44's aarch64 build also leaves the same server under the generic name
# the update checker looks for (rpm-build.sh). It is the same binary with
# release 1, so it is installed the same way, answers, and comes off again.
generic=$(ls "$packages"/generic/hmailserver-*."$arch".rpm 2> /dev/null | head -n 1 || true)
if [ -n "$generic" ]; then
  echo ""
  echo "== and the generic copy, $(basename "$generic")"
  install_rpm "$generic" /tmp/install-generic.log \
    || { cat /tmp/install-generic.log; echo "FAILED: the generic copy did not install"; exit 1; }
  release=$(rpm -q --qf '%{RELEASE}' hmailserver)
  [ "$release" = 1 ] || { echo "FAILED: the generic copy's release is $release, not 1"; exit 1; }
  /usr/bin/hmailserver --version
  rpm -e hmailserver > /tmp/remove.log 2>&1 || { cat /tmp/remove.log; echo "FAILED: rpm -e of the generic copy"; exit 1; }
  echo "installed, answered and removed"
fi
echo ""
echo "PACKAGE PASSED: $(basename "$rpm_file") installs on $PRETTY_NAME ($arch) with its dependencies, runs, carries every file, and removes cleanly, in $(( $(date +%s) - started )) s."
