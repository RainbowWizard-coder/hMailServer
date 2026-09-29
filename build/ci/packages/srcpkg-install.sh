#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The srcpkg-<release>-install jobs of .gitlab/ci/source-packages.yml: what
# srcpkg-deb.sh or srcpkg-rpm.sh built, installed in a CLEAN container of the
# same release - the job's own image, which has never had a build in it - and
# started (hmailserver/docs/SourcePackages.md).
#
#   HM_SRCPKG_TARGET=debian13 bash build/ci/packages/srcpkg-install.sh
#
# from the root of a checkout, as root, with the build job's artifact in
# HM_PACKAGES_DIR (source-packages/<release>).
#
# Debian and Ubuntu: the package's own autopkgtest (debian/tests), run by
# autopkgtest from the .dsc against the two .deb files with its null runner - in
# this container, which is the isolation the test's isolation-container asks for,
# hence --ignore-restrictions for that one restriction. autopkgtest installs the
# packages and the test's Depends with apt from the release's archive, and the
# test starts the server against a PostgreSQL cluster pg_virtualenv makes for it
# and asks SMTP and IMAP for their greetings.
#
# Fedora, EL and openSUSE: dnf or zypper installs the two packages with what
# they require from the release's repositories; the account and the directories
# are held to what the package says; then the same test, debian/tests/server-starts
# from this checkout, against a UTF8 PostgreSQL cluster made here with the
# release's own initdb and pg_ctl; and the packages are removed again.
#
# Leaves install.txt (and for Debian autopkgtest/, autopkgtest's output) in
# HM_PACKAGES_DIR.
set -uo pipefail

target="${HM_SRCPKG_TARGET:-}"
dir="${HM_PACKAGES_DIR:-source-packages/$target}"
TEST=hmailserver/source/Server/platform/packaging/debian-src/debian/tests/server-starts

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

[ -f "$TEST" ] || refuse "run this from the root of a checkout (no $TEST here)."
[ "$(id -u)" = 0 ] || refuse "run this as root in a clean container: it installs the packages."
[ -d "$dir" ] || refuse "no $dir: the build job made nothing; its artifacts say why."
. /etc/os-release
case "$target" in
  debian13 | ubuntu24.04 | ubuntu26.04)
    family=deb
    [ "$ID$VERSION_ID" = "$target" ] ;;
  fedora44) family=dnf; [ "$ID" = fedora ] && [ "$VERSION_ID" = 44 ] ;;
  el10) family=dnf; [[ " ${ID_LIKE:-} $ID " == *" rhel "* ]] && [ "${VERSION_ID%%.*}" = 10 ] ;;
  leap16) family=zypper; [ "$ID" = opensuse-leap ] && [ "${VERSION_ID%%.*}" = 16 ] ;;
  tumbleweed) family=zypper; [ "$ID" = opensuse-tumbleweed ] ;;
  *) refuse "HM_SRCPKG_TARGET='$target' is not one of the seven source-package releases." ;;
esac
[ $? -eq 0 ] || refuse "the packages are for $target, but this container is $PRETTY_NAME."
dir=$(cd "$dir" && pwd)
report="$dir/install.txt"
started=$EPOCHSECONDS
echo "$PRETTY_NAME: $(cd "$dir" && ls -- *.deb *.rpm 2> /dev/null | tr '\n' ' ')" | tee "$report"

retry() {
  local try code
  for try in 1 2 3; do
    "$@" > /tmp/hm-install.log 2>&1
    code=$?
    [ "$code" -eq 0 ] && return 0
    grep -q -i -E 'Failed to fetch|Unable to connect|Temporary failure|Could not resolve|curl error|Timeout was reached|Failed to download' /tmp/hm-install.log || break
    echo "try $try of 3: $(grep -i -E 'Failed to fetch|Unable to connect|curl error|Could not resolve' /tmp/hm-install.log | head -n 1)"
    [ "$try" = 3 ] || sleep 15
  done
  tail -n 30 /tmp/hm-install.log
  return 1
}

# ------------------------------------------------------------------ Debian and Ubuntu
if [ "$family" = deb ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt_opts=(-o Acquire::Retries=3 -o Acquire::ForceIPv4=true)
  step "autopkgtest, from $PRETTY_NAME's archive"
  retry apt-get update -qq "${apt_opts[@]}" || fail "apt-get update"
  retry apt-get install -y -qq "${apt_opts[@]}" --no-install-recommends autopkgtest dpkg-dev \
    || fail "autopkgtest did not install"
  echo "autopkgtest $(dpkg-query -W -f '${Version}' autopkgtest)"
  dsc=$(ls "$dir"/hmailserver_*.dsc)
  mapfile -t debs < <(ls "$dir"/*.deb)
  rm -rf "$dir/autopkgtest"
  step "the package's autopkgtest: $(basename "$dsc") with ${debs[*]##*/}"
  autopkgtest --no-built-binaries --ignore-restrictions=isolation-container \
    --output-dir "$dir/autopkgtest" "${debs[@]}" "$dsc" -- null
  code=$?
  echo "autopkgtest exit $code"
  if [ -f "$dir/autopkgtest/summary" ]; then tee -a "$report" < "$dir/autopkgtest/summary"; fi
  # 0: every test passed; 2: at least one was skipped; anything else failed.
  [ "$code" -eq 0 ] || fail "autopkgtest exited $code (the summary above; the log is autopkgtest/log)"
  # debian/tests/control's one test is a Test-Command, which autopkgtest names command1.
  grep -q ' PASS' "$dir/autopkgtest/summary" 2> /dev/null || fail "autopkgtest's summary has no PASS"
  echo "installed: $(dpkg-query -W -f '${Package} ${Version} ${Status}\n' hmailserver hmailserver-cli | tr '\n' ';')" | tee -a "$report"
  echo "seconds $(( EPOCHSECONDS - started ))" >> "$report"
  echo ""
  echo "PACKAGE PASSED: the Debian source package's .deb files install on $PRETTY_NAME and its autopkgtest starts the server ($(( EPOCHSECONDS - started )) s)."
  exit 0
fi

# ------------------------------------------------------------------ Fedora, EL, openSUSE
repos=()
[ "$target" = el10 ] && repos=(--enablerepo=crb)
install_packages() {
  case "$family" in
    dnf) retry dnf -y "${repos[@]}" --setopt=install_weak_deps=False install "$@" ;;
    zypper) retry zypper -n install --no-recommends --allow-unsigned-rpm "$@" ;;
  esac
}
server_rpm=$(ls "$dir"/hmailserver-[0-9]*.x86_64.rpm)
cli_rpm=$(ls "$dir"/hmailserver-cli-*.noarch.rpm)

step "install the two packages, with what they require from $PRETTY_NAME"
rpm -qp --requires "$server_rpm" | grep -E '^lib' | tr '\n' ' '
echo ""
install_packages "$server_rpm" "$cli_rpm" || fail "the packages did not install"
rpm -q hmailserver hmailserver-cli | tee -a "$report"
missing=$(ldd /usr/bin/hmailserver 2>&1 | grep -F 'not found' || true)
[ -z "$missing" ] || fail "libraries not found: $missing"
# Not through a pipe, whose status would be tee's: a server that cannot even say
# its version must fail the job here.
answer=$(/usr/bin/hmailserver --version 2>&1)
code=$?
[ "$code" -eq 0 ] || fail "/usr/bin/hmailserver --version exited $code: $answer"
echo "$answer" | tee -a "$report"

step "what the package made: the account, the directories, the configuration's owner"
getent passwd hmailserver || fail "no hmailserver account after the install"
stat -c '%a %U:%G %n' /etc/hmailserver /etc/hmailserver/hMailServer.ini /var/lib/hmailserver /var/log/hmailserver | tee -a "$report"
[ "$(stat -c '%a %U:%G' /var/lib/hmailserver)" = "750 hmailserver:hmailserver" ] || fail "/var/lib/hmailserver is not 0750 hmailserver:hmailserver"
[ "$(stat -c '%a %U:%G' /etc/hmailserver/hMailServer.ini)" = "640 root:hmailserver" ] || fail "the configuration is not 0640 root:hmailserver"
[ ! -e /usr/lib/hmailserver/hmailserver-update ] || fail "the live update's helper is installed"
head -n 1 /usr/bin/hmctl | grep -qx '#!/usr/bin/python3' || fail "hmctl's first line is $(head -n 1 /usr/bin/hmctl)"

step "a PostgreSQL cluster for the test, with $PRETTY_NAME's own server"
case "$family" in
  dnf) install_packages postgresql-server util-linux procps-ng || fail "PostgreSQL did not install" ;;
  zypper) install_packages postgresql-server postgresql util-linux procps || fail "PostgreSQL did not install" ;;
esac
initdb=$(command -v initdb || ls -d /usr/lib/postgresql*/bin/initdb /usr/pgsql-*/bin/initdb 2> /dev/null | sort -V | tail -n 1)
[ -x "$initdb" ] || fail "no initdb after installing postgresql-server"
pgbin=$(dirname "$initdb")
pgdir=/tmp/hm-install-pg
password=$(od -An -N 12 -tx1 /dev/urandom | tr -d ' \n')
rm -rf "$pgdir"
install -d -o postgres -g postgres -m 0700 "$pgdir"
printf '%s\n' "$password" > "$pgdir/password"
chown postgres:postgres "$pgdir/password"
# UTF8, as the server creates its database: initdb takes the encoding from the
# locale, and a container has none, so without these the cluster is SQL_ASCII and
# the server's CREATE DATABASE ... ENCODING 'UTF8' is refused (Fedora 44, 29
# September 2026). --no-locale is C collation, which every release has.
runuser -u postgres -- "$pgbin/initdb" -D "$pgdir/data" -U postgres -A scram-sha-256 --pwfile="$pgdir/password" \
    --encoding=UTF8 --no-locale > "$pgdir/initdb.log" 2>&1 \
  || { tail -n 20 "$pgdir/initdb.log"; fail "initdb"; }
runuser -u postgres -- "$pgbin/pg_ctl" -D "$pgdir/data" -l "$pgdir/server.log" -w \
  -o "-p 55432 -k $pgdir -c listen_addresses=127.0.0.1" start > /dev/null \
  || { tail -n 20 "$pgdir/server.log"; fail "pg_ctl start"; }
echo "PostgreSQL $("$pgbin/postgres" --version | sed 's/.* //') on 127.0.0.1:55432"

step "the test the Debian package's autopkgtest runs: $TEST"
PGHOST=127.0.0.1 PGPORT=55432 PGUSER=postgres PGPASSWORD=$password bash "$TEST"
code=$?
runuser -u postgres -- "$pgbin/pg_ctl" -D "$pgdir/data" -m fast stop > /dev/null 2>&1 || true
[ "$code" -eq 0 ] || fail "the server did not start and answer (above)"
echo "server-starts PASS" >> "$report"

step "and the packages come off again"
case "$family" in
  dnf) retry dnf -y remove hmailserver hmailserver-cli || fail "dnf remove" ;;
  zypper) retry zypper -n remove hmailserver hmailserver-cli || fail "zypper remove" ;;
esac
[ ! -e /usr/bin/hmailserver ] || fail "/usr/bin/hmailserver is still there after the removal"
echo "seconds $(( EPOCHSECONDS - started ))" >> "$report"
echo ""
echo "PACKAGE PASSED: $(basename "$server_rpm") installs on $PRETTY_NAME, the server starts against PostgreSQL and answers SMTP and IMAP, and it comes off again ($(( EPOCHSECONDS - started )) s)."
