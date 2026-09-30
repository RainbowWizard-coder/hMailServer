#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The jobs of .gitlab/ci/alpine.yml: the server built with musl on Alpine Linux, as
# aports would build it, and tested there (hmailserver/docs/Alpine.md). Run as root
# in Docker Hub's alpine:3.24, from the root of a checkout, after
# `apk add --no-cache bash`:
#
#   bash build/ci/alpine.sh package   the APKBUILD run by abuild on the source
#                                      tarball: the build, its check() - the
#                                      server's --version and every tester program -
#                                      the packages, and apkbuild-lint
#   bash build/ci/alpine.sh install   in a clean container, the packages installed
#                                      beside Alpine's PostgreSQL, the database made
#                                      as an administrator makes it, the server
#                                      started with rc-service, and SMTP (with
#                                      STARTTLS) and IMAP asked for their greetings
#   bash build/ci/alpine.sh suite     the whole of LinuxRegressionTests against the
#                                      installed package's server, through
#                                      build/ci/linux-suite.sh, beside the job's
#                                      PostgreSQL service
#   bash build/ci/alpine.sh collect   suite's after_script: linux-suite.sh collect
#
# The environment it reads:
#   HM_SOURCE_TARBALL  the source tarball, or a directory holding exactly one - the
#                      source-tarball job's artifact, source-tarball/ (package and
#                      suite). The APKBUILD, its install scripts, the OpenRC script
#                      and the suite are all taken from it, unpacked into
#                      HM_SOURCE_DIR (/tmp/hm-source), so that what aports would be
#                      given is what is built; this checkout gives only the scripts.
#   HM_BUILD_JOBS      the compilers abuild runs at once (4)
#   HM_OUT_DIR         where package leaves everything (alpine/): packages/, the
#                      signing key's public half, abuild.log, lint.txt; install and
#                      suite read the packages from there
#
# The packages are signed with a key made for the job and thrown away with it: its
# public half goes with the packages so the next jobs can trust them, and nothing
# else ever does. aports signs with Alpine's own keys.
set -euo pipefail

# package empties it: never the root, and never the checkout or a directory holding
# it, compared as the file system resolves them.
mkdir -p "${HM_OUT_DIR:-alpine}"
OUT="$(cd "${HM_OUT_DIR:-alpine}" && pwd -P)"
holds="${OUT%/}/"
case "$(pwd -P)/" in
  "$holds"*) echo "REFUSED: HM_OUT_DIR (${OUT}) holds the checkout, and package empties it." >&2; exit 2 ;;
esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

fail() {
  echo "FAILED: $*" >&2
  exit 1
}

# The source tarball unpacked and checked (source-tarball.sh unpack), its root on
# stdout.
unpack_tarball() {
  [ -n "${HM_SOURCE_TARBALL:-}" ] || fail "HM_SOURCE_TARBALL names no source tarball"
  bash "$HERE/source-tarball.sh" unpack "$HM_SOURCE_TARBALL" "${HM_SOURCE_DIR:-/tmp/hm-source}"
}

tarball_file() {
  local found
  if [ -d "$HM_SOURCE_TARBALL" ]; then
    found=$(find "$HM_SOURCE_TARBALL" -maxdepth 1 -name 'hmailserver-*.tar.gz' | head -n 1)
  else
    found="$HM_SOURCE_TARBALL"
  fi
  [ -f "$found" ] || fail "no hmailserver-*.tar.gz at $HM_SOURCE_TARBALL"
  printf '%s\n' "$found"
}

# The packages the package step made, and the key they are signed with trusted.
trust_packages() {
  [ -d "$OUT/packages" ] || fail "no $OUT/packages: the alpine-package job's artifact is missing"
  cp "$OUT"/keys/*.rsa.pub /etc/apk/keys/
}

package_files() {
  find "$OUT/packages" -name '*.apk' | sort
}

package() {
  # tar: GNU tar, which source-tarball.sh unpack runs with its long options.
  apk add --no-cache abuild atools python3 tar > /dev/null
  local root tarball version pkgver aport
  root=$(unpack_tarball)
  tarball=$(tarball_file)
  version=$(sed -n 's/^#define[[:space:]][[:space:]]*HMAILSERVER_VERSION[[:space:]][[:space:]]*"\([0-9.]*\)".*/\1/p' \
    "$root/hmailserver/source/Server/Common/Application/Version.h" | tr -d '\r')
  pkgver=$(sed -n 's/^pkgver=//p' "$root/hmailserver/source/Server/platform/packaging/alpine/APKBUILD")
  [ "$pkgver" = "$version" ] || fail "the APKBUILD says pkgver=$pkgver and Version.h $version"
  [ "$(basename "$tarball")" = "hmailserver-$version.tar.gz" ] || fail "the tarball is $(basename "$tarball"), not hmailserver-$version.tar.gz"
  echo "hmailserver $version from $(basename "$tarball")"

  rm -rf "$OUT"
  mkdir -p "$OUT/packages" "$OUT/keys"

  # aports' layout: testing/hmailserver/, the APKBUILD and its install scripts as
  # the tarball carries them, the tarball where abuild looks before it downloads.
  aport=/tmp/aports/testing/hmailserver
  rm -rf /tmp/aports
  mkdir -p "$aport" /var/cache/distfiles
  cp "$root"/hmailserver/source/Server/platform/packaging/alpine/APKBUILD \
     "$root"/hmailserver/source/Server/platform/packaging/alpine/hmailserver.pre-install \
     "$root"/hmailserver/source/Server/platform/packaging/alpine/hmailserver.post-upgrade "$aport/"
  cp "$tarball" /var/cache/distfiles/

  echo "== apkbuild-lint"
  ( cd "$aport" && apkbuild-lint APKBUILD ) > "$OUT/lint.txt" 2>&1 || true
  cat "$OUT/lint.txt"
  if [ -s "$OUT/lint.txt" ]; then
    fail "apkbuild-lint has something to say about the APKBUILD (above)"
  fi
  echo "apkbuild-lint: nothing to say"

  # A key for this job alone, named for the packager abuild records, and trusted so
  # that abuild -r can install what it builds. By hand: abuild-keygen -i would ask
  # doas, which the image has not, even of root.
  export PACKAGER="hMailServer CI <ci@hmailserver.invalid>"
  abuild-keygen -a -n > /dev/null 2>&1
  cp "$HOME"/.abuild/*.rsa.pub /etc/apk/keys/
  cp "$HOME"/.abuild/*.rsa.pub "$OUT/keys/"

  echo "== abuild checksum, then abuild -r (the build, check() and the packages)"
  # abuild -r installs makedepends with apk, which needs the index that the
  # apk add --no-cache calls above never kept.
  apk update > /dev/null
  cd "$aport"
  abuild -F checksum
  grep -A2 '^sha512sums=' APKBUILD
  local code=0
  # JOBS is what abuild gives make, samurai and CMake; its default is every processor
  # the machine has, which a job's memory cannot hold.
  JOBS="${HM_BUILD_JOBS:-4}" REPODEST="$OUT/packages" abuild -F -r > "$OUT/abuild.log" 2>&1 || code=$?
  grep -E '^>>> |hmailserver --version|-tests|PASSED|FAILED|passed|failed' "$OUT/abuild.log" | tail -n 80
  if [ "$code" -ne 0 ]; then
    tail -n 60 "$OUT/abuild.log"
    fail "abuild exited $code: the whole log is $OUT/abuild.log"
  fi
  echo ""
  package_files | while read -r apk; do
    echo "$(basename "$apk")  $(stat -c %s "$apk") bytes"
  done
  ( cd "$OUT/packages" && find . -name '*.apk' -exec sha256sum {} + ) > "$OUT/SHA256SUMS.txt"
  echo "PASSED: the APKBUILD built hmailserver $version on $(cat /etc/alpine-release) with musl, and its check() ran the testers."
}

# bash's /dev/tcp: SMTP greets with 220 and offers STARTTLS, IMAP greets with * OK,
# as the Debian package's autopkgtest and the NixOS test ask (nix/probe.sh).
reply() {
  local line
  IFS= read -r -t 15 line <&3 || return 1
  printf '%s\n' "${line%$'\r'}"
}

probe() {
  local smtp=25 imap=143 greeting line extensions="" bye answered=""
  for _ in $(seq 1 60); do
    if (exec 3<> "/dev/tcp/127.0.0.1/$smtp") 2> /dev/null; then
      answered=yes
      break
    fi
    sleep 1
  done
  [ -n "$answered" ] || return 1
  exec 3<> "/dev/tcp/127.0.0.1/$smtp"
  greeting=$(reply) || { echo "no greeting on $smtp"; return 1; }
  echo "S: $greeting"
  [[ $greeting == 220\ * ]] || { echo "the greeting is not 220"; return 1; }
  printf 'EHLO alpine.invalid\r\n' >&3
  while line=$(reply); do
    echo "S: $line"
    extensions="$extensions $line"
    [[ $line == 250\ * ]] && break
    [[ $line == 250-* ]] || { echo "EHLO answered: $line"; return 1; }
  done
  [[ $extensions == *STARTTLS* ]] || { echo "EHLO offers no STARTTLS, which the first start's defaults turn on"; return 1; }
  printf 'QUIT\r\n' >&3
  bye=$(reply) || return 1
  echo "S: $bye"
  exec 3>&-
  exec 3<> "/dev/tcp/127.0.0.1/$imap"
  greeting=$(reply) || { echo "no IMAP greeting"; return 1; }
  echo "S: $greeting"
  [[ $greeting == "* OK"* ]] || { echo "the IMAP greeting is not * OK"; return 1; }
  printf 'a1 LOGOUT\r\n' >&3
  exec 3>&-
}

install_test() {
  trust_packages
  echo "== apk add the packages, OpenRC and PostgreSQL"
  # shellcheck disable=SC2046 # one word per package file
  apk add --no-cache openrc postgresql18 postgresql18-client $(package_files) > /dev/null
  apk info -e hmailserver hmailserver-openrc hmailserver-cli
  hmailserver --version
  getent passwd hmailserver > /dev/null || fail "the package made no hmailserver account"
  [ -x /etc/init.d/hmailserver ] || fail "hmailserver-openrc installed no /etc/init.d/hmailserver"
  [ ! -e /usr/lib/systemd ] || fail "the package installed /usr/lib/systemd"
  stat -c '%a %U:%G %n' /etc/hmailserver /etc/hmailserver/hMailServer.ini /var/lib/hmailserver /var/log/hmailserver

  echo "== PostgreSQL, a cluster of the job's own"
  mkdir -p /run/postgresql /tmp/pg
  chown postgres:postgres /run/postgresql /tmp/pg
  su -s /bin/sh -c 'initdb -D /tmp/pg -A trust > /dev/null && pg_ctl -D /tmp/pg -o "-c listen_addresses=127.0.0.1" -w start > /dev/null' postgres
  psql -h 127.0.0.1 -U postgres -Atc "create role hmailserver login createdb password 'alpine-test'"

  echo "== the configuration, as Alpine.md tells an administrator to write it"
  local ini=/etc/hmailserver/hMailServer.ini
  sed -i -e 's|^Type=.*|Type=PostgreSQL|' -e 's|^Server=.*|Server=127.0.0.1|' -e 's|^Port=.*|Port=5432|' \
    -e 's|^Database=.*|Database=hmailserver|' -e 's|^Username=.*|Username=hmailserver|' \
    -e 's|^Password=.*|Password=alpine-test|' "$ini"
  su -s /bin/sh -c 'hmailserver --check-config' hmailserver
  local created
  created=$(su -s /bin/sh -c 'hmailserver --create-database' hmailserver)
  printf '%s\n' "$created"
  # It names the OpenRC service's next start as what secures the new database, not
  # the systemd unit this machine has not got (main.cpp, OpenRcServiceFindsMarker).
  [[ $created == *"The OpenRC service's next start"* ]] \
    || fail "--create-database did not say that the OpenRC service's next start secures the new database"
  echo "alpine-test-admin" | hmailserver --set-admin-password > /dev/null
  hmailserver --create-stored-secrets-key > /dev/null
  grep -q '^StoredSecretsKey=.' "$ini" || fail "--create-stored-secrets-key wrote no key into $ini"
  stat -c '%a %U:%G %n' "$ini"

  echo "== rc-service hmailserver start"
  # OpenRC in a container: no init ran, so it is told it is in one, and that the
  # network it would need is there (the container's own).
  sed -i -e 's|^#\?rc_sys=.*|rc_sys="docker"|' -e 's|^#\?rc_provide=.*|rc_provide="loopback net"|' /etc/rc.conf
  mkdir -p /run/openrc
  touch /run/openrc/softlevel
  rc-service hmailserver checkconfig
  rc-service hmailserver start || { tail -n 40 /var/log/hmailserver/*.log 2> /dev/null; fail "rc-service hmailserver start"; }
  local pid
  pid=$(cat /run/hmailserver.pid)
  echo "running as $(stat -c %U "/proc/$pid"), $(grep -E '^Cap(Amb|Eff):' "/proc/$pid/status" | tr '\n' ' ')"
  [ "$(stat -c %U "/proc/$pid")" = hmailserver ] || fail "the server does not run as hmailserver"
  if ! probe; then
    tail -n 40 /var/log/hmailserver/*.log 2> /dev/null || true
    fail "SMTP or IMAP did not answer as they should"
  fi
  rc-service hmailserver reload
  sleep 3
  probe > /dev/null || fail "the server did not answer after a reload"
  rc-service hmailserver stop
  kill -0 "$pid" 2> /dev/null && fail "the server is still running after rc-service hmailserver stop"
  su -s /bin/sh -c 'pg_ctl -D /tmp/pg -w stop > /dev/null' postgres
  echo "PASSED: the packages installed on $(cat /etc/alpine-release), rc-service started the server as hmailserver, SMTP (with STARTTLS) and IMAP answered, and it reloaded and stopped."
}

suite() {
  trust_packages
  echo "== apk add the packages, the .NET SDK and what linux-suite.sh runs"
  # As the CI image has them (build/ci/linux.Dockerfile): 7-Zip, which the server
  # writes and reads a backup with, and the MariaDB connector it opens for MySQL.
  # shellcheck disable=SC2046 # one word per package file
  apk add --no-cache dotnet10-sdk postgresql18-client curl iproute2 procps python3 tar 7zip mariadb-connector-c \
    $(package_files) > /dev/null
  hmailserver --version
  dotnet --list-sdks
  # The whole suite, whatever the project defines: linux-suite.sh refuses a filter in
  # the job named linux-suite alone, and a project variable outranks the job's own.
  unset HM_LINUX_SUITE_FILTER
  HM_SERVER_BINARY=/usr/bin/hmailserver bash "$HERE/linux-suite.sh"
}

case "${1:-}" in
  package) package ;;
  install) install_test ;;
  suite) suite ;;
  collect) HM_SERVER_BINARY=/usr/bin/hmailserver bash "$HERE/linux-suite.sh" collect ;;
  *) echo "usage: $0 package|install|suite|collect" >&2; exit 2 ;;
esac
