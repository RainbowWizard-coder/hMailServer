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
#   glibc's character-set converters came with it, which the package requires
#   where dnf is installed on Fedora or EL and recommends everywhere, and the
#   image lacks: the server finds every one it names (--check-config), BIG5 and
#   ISO-2022-JP text converts - and with the converters taken away again,
#   neither does, which is the check seeing what it checks;
#   the live update's own install, without them: on openSUSE rpm -Uvh installs
#   the package, and so does dnf where it has been installed beside zypper; on
#   Fedora and EL rpm -Uvh refuses it, and dnf, which the live update installs
#   with there, brings them;
#   every file a working installation needs is where the package put it;
#   the licences are there as the tree has them - the AGPL-3.0 text, the notices
#   of QuickJS, Unicode's data and HTML's named character references, and
#   /usr/share/licenses/hmailserver/copyright - installed under the image's
#   tsflags=nodocs, and each one rpm -qL names (check-licenses.sh);
#   %post made the service account and the directories, and left the
#   configuration root-owned and readable by the service only;
#   the Control Deck is the repository's, byte for byte;
#   and the package comes off again.
#
#   bash build/ci/packages/rpm-install-test.sh <target> <arch>
#
# Unlike build/ci/linux-package-install.sh's half for linux-build's .rpm, nothing is
# installed with --nodeps: the whole point of a per-distribution build is that it
# installs with its dependencies on that distribution. It does not start the server
# against a database: linux-package-install.sh rpm-fedora does that for the generic
# aarch64 copy, the file a release carries, in rpm-fedora44-aarch64-start. What a
# container cannot prove is the start under systemd (linux-package-install.sh says
# what that leaves out).
set -uo pipefail

target="${1:-}"
arch="${2:-}"
packages="${HM_OUT_ROOT:-packages-rpm}/$target-$arch"

# converters: the package of glibc's character-set converters beyond its base
# package's, which the .rpm requires where dnf is installed on Fedora or EL and
# recommends everywhere (CMakeLists.txt, CPACK_RPM_PACKAGE_REQUIRES and
# _RECOMMENDS).
case "$target" in
  fedora44|fedora43|el10|el9) family=dnf; converters=glibc-gconv-extra ;;
  leap16|tumbleweed) family=zypper; converters=glibc-gconv-modules-extra ;;
  *) echo "usage: $0 <fedora44|fedora43|el10|el9|leap16|tumbleweed> <x86_64|aarch64>"; exit 64 ;;
esac
# rpm's name for the architecture in a dependency (%{_isa}).
case "$arch" in
  x86_64) isa=x86-64 ;;
  aarch64) isa=aarch-64 ;;
  *) echo "arch is x86_64 or aarch64"; exit 64 ;;
esac

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
# zypper's repositories are refreshed before the first install by
# zypper-refresh.sh, which points those on download.opensuse.org or
# cdn.opensuse.org at openSUSE's origin when a mirror serves a wrong file and
# says so; zypper trusts only the keys the image already holds: nothing is
# auto-imported. dnf without weak dependencies: the converters are
# required on Fedora and EL. zypper with them, as a system installs
# (solver.onlyRequires off, its default; the images set it on): on openSUSE the
# converters are a recommendation, rpm -Uvh being the live update's install there.
install_rpm() {
  case "$family" in
    dnf) retry_fetch "$2" dnf -y -q --setopt=install_weak_deps=False install "./$1" ;;
    zypper) retry_fetch "$2" zypper -n install --recommends --allow-unsigned-rpm "./$1" ;;
  esac
}

# The converters, as the server and the C library answer for them. present: the
# server names no character set it cannot convert, and a BIG5 and an ISO-2022-JP
# sample - U+4E2D U+6587 and U+65E5 U+672C, which glibc's base package cannot
# read - come out as their UTF-8. absent: the server names BIG5 and ISO-2022-JP-2
# among those it cannot, and neither sample converts. Both through the installed
# server's --check-config, which opens each character set it converts text in
# with the C library's iconv, both ways, and through that iconv itself.
check_charsets() {
  local expect=$1 answer line big5 jis
  answer=$(/usr/bin/hmailserver --check-config 2>&1) \
    || { echo "$answer"; echo "FAILED: hmailserver --check-config"; exit 1; }
  line=$(grep '^Character sets:' <<< "$answer" || true)
  echo "  $line"
  big5=$(printf '\244\244\244\345' | iconv -f BIG5 -t UTF-8 2> /dev/null | od -An -tx1 | tr -d ' \n' || true)
  jis=$(printf '\033$BF|K\\\033(B' | iconv -f ISO-2022-JP-2 -t UTF-8 2> /dev/null | od -An -tx1 | tr -d ' \n' || true)
  echo "  BIG5 a4a4a4e5 -> UTF-8 '${big5:-nothing}'; ISO-2022-JP-2 1b244246...1b2842 -> UTF-8 '${jis:-nothing}'"
  if [ "$expect" = present ]; then
    [ "$line" = "Character sets:     every one the server converts text in converts here" ] \
      || { echo "FAILED: with $converters installed, the server still names character sets it cannot convert (above)"; exit 1; }
    [ "$big5" = e4b8ade69687 ] || { echo "FAILED: BIG5 text did not convert to UTF-8"; exit 1; }
    [ "$jis" = e697a5e69cac ] || { echo "FAILED: ISO-2022-JP text did not convert to UTF-8"; exit 1; }
  else
    case "$line" in
      *'do not convert here'*BIG5*ISO-2022-JP-2*) ;;
      *) echo "FAILED: without $converters the server did not name BIG5 and ISO-2022-JP-2 as character sets it cannot convert"; exit 1 ;;
    esac
    [ -z "$big5" ] || { echo "FAILED: BIG5 text converted without $converters, so this test cannot tell the package from the image"; exit 1; }
    [ -z "$jis" ] || { echo "FAILED: ISO-2022-JP text converted without $converters, so this test cannot tell the package from the image"; exit 1; }
  fi
}

set -e
started=$(date +%s)
echo ""
echo "== the character-set converters: required where dnf is on Fedora or EL, recommended, and not in this image"
# The rich dependency each distribution meets with its own package
# (CMakeLists.txt); grep reads the whole list (see below, at rpm -qc).
either="(glibc-gconv-extra($isa) or glibc-gconv-modules-extra($isa))"
condition="(dnf and system-release)"
rpm -qp --requires "$rpm_file" | grep -xF "($either if $condition)" > /dev/null \
  || { echo "FAILED: $(basename "$rpm_file") does not require glibc's converters where dnf is on Fedora or EL, ($either if $condition)"; exit 1; }
rpm -qp --recommends "$rpm_file" | grep -xF "$either" > /dev/null \
  || { echo "FAILED: $(basename "$rpm_file") does not recommend glibc's converters, $either"; exit 1; }
echo "$(basename "$rpm_file") requires $either if $condition, and recommends it"
if rpm -q "$converters" > /dev/null 2>&1; then
  echo "($converters is in the image already, so the install below cannot show it comes with the package)"
else
  echo "$converters is not installed: this image, as every container image of $NAME, goes without"
fi

echo ""
echo "== install it, with what $family resolves for it from $PRETTY_NAME's repositories"
if [ "$family" = zypper ]; then
  bash "$(dirname "$0")/zypper-refresh.sh" /tmp/refresh.log \
    || { tail -n 40 /tmp/refresh.log; echo "FAILED: zypper could not refresh $PRETTY_NAME's repositories"; exit 1; }
fi
install_rpm "$rpm_file" /tmp/install.log \
  || { cat /tmp/install.log; echo "FAILED: $family could not install $(basename "$rpm_file")"; exit 1; }
grep -iE 'installing|installed|hMailServer is installed' /tmp/install.log | head -n 5 || true
echo "installed in $(( $(date +%s) - started )) s"

echo ""
echo "== hmailserver --version"
/usr/bin/hmailserver --version

echo ""
echo "== the character sets the server converts text in, with the converters the install brought"
rpm -q --whatprovides "$converters($isa)" > /dev/null \
  || { echo "FAILED: $family installed the package without $converters($isa)"; exit 1; }
echo "$(rpm -q --whatprovides "$converters($isa)") came with it"
check_charsets present
# And the check, seeing what it checks: with the converters taken away again,
# behind the package's back, the server names what it has lost and BIG5 text no
# longer converts. Put back by the generic copy's install below, where there is
# one, and gone with the container otherwise.
rpm -e --nodeps "$converters" > /tmp/remove-converters.log 2>&1 \
  || { cat /tmp/remove-converters.log; echo "FAILED: rpm -e --nodeps $converters"; exit 1; }
echo "without $converters:"
check_charsets absent

echo ""
echo "== the live update's install, without the converters"
# The live update's helper (hmailserver-update, 6.3.5's as this release's)
# installs an .rpm with dnf install -y where there is a dnf command, and with
# rpm -Uvh where there is not, which resolves nothing and refuses a package whose
# requirements are not met. With the converters required on every distribution,
# 6.3.5's helper refused the update to 6.3.6 on every openSUSE server without
# them; the requirement holds only where dnf is installed on Fedora or EL.
rpm -e hmailserver > /tmp/remove.log 2>&1 || { cat /tmp/remove.log; echo "FAILED: rpm -e before rpm -Uvh"; exit 1; }
if rpm -Uvh "$rpm_file" > /tmp/uvh.log 2>&1; then uvh=installed; else uvh=refused; fi
case "$family:$uvh" in
  zypper:installed)
    echo "rpm -Uvh installed it without $converters, as the live update does here"
    check_charsets absent
    # openSUSE packages dnf too, and where it has been installed beside zypper the
    # live update installs with it - a dnf with no repositories here, which
    # refused the package while the requirement held wherever dnf was (on Leap
    # 16.0 and Tumbleweed, 3 October 2026). The helper's own command.
    rpm -e hmailserver > /tmp/remove.log 2>&1 || { cat /tmp/remove.log; echo "FAILED: rpm -e before dnf"; exit 1; }
    retry_fetch /tmp/install-dnf.log zypper -n install --no-recommends dnf \
      || { cat /tmp/install-dnf.log; echo "FAILED: zypper could not install dnf"; exit 1; }
    if ! dnf install -y "./$rpm_file" > /tmp/dnf-install.log 2>&1; then
      cat /tmp/dnf-install.log
      echo "FAILED: dnf, installed beside zypper, refused $(basename "$rpm_file"), so the live update could not install it here"
      exit 1
    fi
    echo "and dnf install -y, with $(rpm -q --whatprovides dnf) installed beside zypper, installed it too"
    rpm -q --whatprovides "$converters($isa)" > /dev/null 2>&1 || check_charsets absent ;;
  dnf:refused)
    grep -q 'is needed by' /tmp/uvh.log || { cat /tmp/uvh.log; echo "FAILED: rpm -Uvh refused it for a reason that is not a dependency"; exit 1; }
    echo "rpm -Uvh refused it without $converters, which it requires here: $(grep -m 1 'is needed by' /tmp/uvh.log | sed 's/^[[:space:]]*//')"
    install_rpm "$rpm_file" /tmp/install-again.log \
      || { cat /tmp/install-again.log; echo "FAILED: $family could not install $(basename "$rpm_file") again"; exit 1; }
    rpm -q --whatprovides "$converters($isa)" > /dev/null \
      || { echo "FAILED: $family installed the package again without $converters($isa)"; exit 1; }
    echo "and $family installs it with them, as the live update does here" ;;
  zypper:refused)
    cat /tmp/uvh.log
    echo "FAILED: rpm -Uvh refused $(basename "$rpm_file") without $converters, so the live update could not install it here"
    exit 1 ;;
  *)
    echo "FAILED: rpm -Uvh installed $(basename "$rpm_file") without $converters on $NAME, where it requires them"
    exit 1 ;;
esac

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
  rpm -e --nodeps "$converters" > /dev/null 2>&1 || true
  install_rpm "$generic" /tmp/install-generic.log \
    || { cat /tmp/install-generic.log; echo "FAILED: the generic copy did not install"; exit 1; }
  release=$(rpm -q --qf '%{RELEASE}' hmailserver)
  [ "$release" = 1 ] || { echo "FAILED: the generic copy's release is $release, not 1"; exit 1; }
  /usr/bin/hmailserver --version
  # The converters taken away above come back with it: it requires them too.
  check_charsets present
  rpm -e hmailserver > /tmp/remove.log 2>&1 || { cat /tmp/remove.log; echo "FAILED: rpm -e of the generic copy"; exit 1; }
  echo "installed, answered, converted and removed"
fi
echo ""
# The line semgrep's baseline knows by its text (build/ci/semgrep-baseline.txt):
# the converters are said above, where they were checked.
echo "PACKAGE PASSED: $(basename "$rpm_file") installs on $PRETTY_NAME ($arch) with its dependencies, runs, carries every file, and removes cleanly, in $(( $(date +%s) - started )) s."
