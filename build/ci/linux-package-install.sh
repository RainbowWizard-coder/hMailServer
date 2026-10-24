#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-package-deb and linux-package-rpm jobs of .gitlab-ci.yml, and the
# deb-ubuntu26.04-arm64-start and rpm-fedora44-aarch64-start jobs of
# .gitlab/ci/packages-deb.yml and packages-rpm.yml: a package installed in a clean
# distribution container - the job's own image, which is a stock distribution image
# and not the CI image - and held to what a working installation needs. It is the
# old GitHub workflow linux-build.yml's package-install job, moved from a hosted
# runner with systemd into a container without it.
#
#   bash build/ci/linux-package-install.sh deb          in ubuntu:26.04 of the
#                                                       package's architecture, beside
#                                                       a PostgreSQL service ("postgres")
#   bash build/ci/linux-package-install.sh rpm          linux-build's x86_64 .rpm, in
#                                                       Fedora 44
#   bash build/ci/linux-package-install.sh rpm-fedora   the generic .rpm that
#                                                       rpm-fedora44-<arch> builds in
#                                                       Fedora 44, in Fedora 44 of its
#                                                       architecture, beside "postgres"
#
# HM_PACKAGES_DIR is where the package is: linux-build/packages by default, the .deb
# and the .rpm linux-build made; packages-deb/ubuntu26.04-arm64 for the arm64 .deb
# and packages-rpm/fedora44-aarch64/generic for the aarch64 .rpm, the generic files a
# release carries beside linux-build's. The package taken is the one there of this
# container's own architecture, as dpkg or rpm names it, and there must be exactly
# one: an arm64 package is held in an arm64 container - under qemu-user on the
# project's runners, natively on an arm64 machine. HM_START_WAIT is how many seconds
# the started server is given to answer (60; the arm64 jobs give more, since under
# qemu-user every instruction of it is emulated), each request of the wait 30 of them
# at most, so that a REST port that takes the connection and never answers fails
# within it.
#
# The .deb, and the generic .rpm built in Fedora 44: the distribution's package
# manager installs it with its dependencies from that release's own repositories -
# apt from Ubuntu's archive, dnf from Fedora's, never --nodeps - and the installed
# server answers its version; every file a working installation needs is where the
# package says, and its licences with it (build/ci/packages/check-licenses.sh, which
# the linux-build .rpm's half runs too); the packaged configuration is edited the
# way packaging/README.md tells an administrator to; the server checks it, creates
# the schema and stores its settings as the service user; then it is started the way
# its unit starts it - as hmailserver, in /var/lib/hmailserver, with the one
# capability the unit grants and no new privileges - and asked whether port 25
# answers SMTP with its own banner, whether the REST API answers and whether GET / is
# the Control Deck. Last, it is removed.
#
# What a container cannot prove, and the hosted runner did: the start under
# systemd itself, with the unit's whole sandbox (ProtectSystem=strict, the
# syscall filter) in force. The package-systemd jobs prove that, with systemd as
# PID 1 on linix's privileged runner entry (build/ci/package-systemd-test.sh).
# The unit's syntax is verified by the update-helper
# job's systemd-analyze, and the unit's user, directory and capability are
# reproduced here with setpriv. The privileged listeners bind in any case in a
# container, because docker sets net.ipv4.ip_unprivileged_port_start to 0.
#
# linux-build's .rpm: the payload and the scriptlets, as before - installed with
# --nodeps. That package is built on Ubuntu, so its automatic requires are that
# build's sonames and symbol versions; dnf resolved them on Fedora 43 and 44 on
# 26 September 2026 (docs/PackageManagers.md, "apt and dnf"), but this job holds the
# package, not a distribution's libraries, which are a question for a build made on
# it - the generic .rpm built in Fedora 44, which rpm-fedora installs with dnf and
# starts.
set -uo pipefail

kind="${1:-}"
packages="${HM_PACKAGES_DIR:-linux-build/packages}"
REST_PORT="${HM_REST_PORT:-8046}"
ADMIN_PASSWORD="${HM_ADMIN_PASSWORD:-testar}"
PGH="${HM_PG_HOST:-postgres}"
export PGPASSWORD="${HM_PG_PASSWORD:-hmtest}"
START_WAIT="${HM_START_WAIT:-60}"

case "$kind" in
  deb|rpm|rpm-fedora) ;;
  *) echo "usage: $0 deb|rpm|rpm-fedora"; exit 64 ;;
esac
case "$START_WAIT" in
  ''|*[!0-9]*) echo "REFUSED: HM_START_WAIT is '$START_WAIT', not a number of seconds"; exit 2 ;;
esac

if [ ! -d "$packages" ] || [ -z "$(ls "$packages" 2> /dev/null)" ]; then
  echo "REFUSED: no packages in $packages. The job that builds them made none;"
  echo "its artifacts (linux-build/package.log for linux-build's) say why."
  exit 2
fi

# Every file the server opens, and why each one is here (the list the old
# workflow asserted on both package formats):
#   hmailserver                the service
#   dh2048.pem                 the Diffie-Hellman group; without it every TLS
#                              context reports a critical 5603
#   CreateTables*.sql          the schema --create-database runs
#   WebAdmin/index.html        the Control Deck the REST listener serves at GET /
#   WebAdmin/languages/        the Deck's catalogues, at /deck-lang/<code>.json
#   the units, the configuration, the logrotate rule, the update helper
REQUIRED_FILES="
/usr/bin/hmailserver
/usr/bin/dh2048.pem
/usr/share/hmailserver/DBScripts/CreateTablesPGSQL.sql
/usr/share/hmailserver/DBScripts/CreateTablesMySQL.sql
/usr/share/hmailserver/DBScripts/ScriptPrerequisitesPGSQL.sql
/usr/share/hmailserver/WebAdmin/index.html
/usr/share/hmailserver/WebAdmin/languages/de.json
/usr/share/hmailserver/WebAdmin/languages/zh-Hans.json
/usr/lib/systemd/system/hmailserver.service
/usr/lib/systemd/system/hmailserver-update.path
/usr/lib/systemd/system/hmailserver-update.service
/usr/lib/hmailserver/hmailserver-update
/etc/hmailserver/hMailServer.ini
/etc/logrotate.d/hmailserver
"

files_are_there() {
  local missing=0 f
  for f in $REQUIRED_FILES; do
    if [ -f "$f" ]; then
      echo "  present  $f"
    else
      echo "  MISSING  $f"
      missing=$((missing + 1))
    fi
  done
  if [ "$missing" -ne 0 ]; then
    echo "FAILED: $missing file(s) a working installation needs are not in the package."
    return 1
  fi
}

# ---------------------------------------------------------------- linux-build's .rpm
if [ "$kind" = rpm ]; then
  set -e
  . /etc/os-release
  echo "== $PRETTY_NAME"
  rpm=$(ls "$packages"/hmailserver-*.x86_64.rpm | head -n 1)
  echo "== $(basename "$rpm")"
  rpm -qip "$rpm"
  # The one dependency every RPM distribution meets with its own package
  # (CMakeLists.txt): this .rpm is the one the live update offers each of them,
  # which installs it with dnf where there is a dnf command and with rpm -Uvh where
  # there is not - so required where dnf is installed on Fedora or EL, and
  # recommended. Read, not resolved: --nodeps below leaves it out, and
  # rpm-install-test.sh installs the per-distribution packages with it, and with
  # rpm -Uvh and openSUSE's dnf without it.
  echo "== it requires glibc's character-set converters where dnf is on Fedora or EL, and recommends them"
  rpm -qp --requires "$rpm" | grep -xF '((glibc-gconv-extra(x86-64) or glibc-gconv-modules-extra(x86-64)) if (dnf and system-release))'
  rpm -qp --recommends "$rpm" | grep -xF '(glibc-gconv-extra(x86-64) or glibc-gconv-modules-extra(x86-64))'
  echo "== install (--nodeps: built on Ubuntu, so its requires name Ubuntu sonames)"
  rpm -Uvh --nodeps "$rpm"
  echo "== every file a working installation needs"
  files_are_there
  echo "== the licences it goes out under, as installed"
  bash build/ci/packages/check-licenses.sh --installed rpm
  echo "== the scriptlet made the identity and the directories"
  getent passwd hmailserver
  ls -ld /var/lib/hmailserver /var/log/hmailserver /etc/hmailserver
  echo "== the configuration is a %config(noreplace) file owned by root"
  rpm -qc hmailserver
  stat -c "%a %U:%G %n" /etc/hmailserver/hMailServer.ini
  # By hash: Fedora's container image has no cmp.
  installed=$(sha256sum < /usr/share/hmailserver/WebAdmin/index.html)
  repository=$(sha256sum < hmailserver/installation/WebAdmin/index.html)
  [ "$installed" = "$repository" ] || { echo "FAILED: the installed Control Deck is not the repository's"; exit 1; }
  echo "the installed Control Deck is the repository's, byte for byte"
  echo "== and it comes off again"
  rpm -e hmailserver
  test ! -f /usr/bin/hmailserver
  echo "PACKAGE PASSED: the .rpm installs, carries every file, and removes cleanly."
  exit 0
fi

# ---------------------------------------------------------------- the .deb, and the .rpm built in Fedora
. /etc/os-release

step() {
  echo ""
  echo "== $*"
}
fail() {
  echo "FAILED: $*"
  show_logs
  exit 1
}
show_logs() {
  [ -f /tmp/hm-stdout.log ] && { echo "-- the server's output"; tail -n 60 /tmp/hm-stdout.log; }
  ls -la /var/log/hmailserver 2> /dev/null || true
  tail -n 80 /var/log/hmailserver/*.log 2> /dev/null || true
}
stop_server() {
  if [ -f /tmp/hm.pid ] && kill -0 "$(cat /tmp/hm.pid)" 2> /dev/null; then
    kill "$(cat /tmp/hm.pid)" 2> /dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "$(cat /tmp/hm.pid)" 2> /dev/null || break
      sleep 1
    done
    kill -9 "$(cat /tmp/hm.pid)" 2> /dev/null || true
  fi
  rm -f /tmp/hm.pid
}
trap stop_server EXIT

# The package of this container's architecture: exactly one in $packages.
shopt -s nullglob
if [ "$kind" = deb ]; then
  arch=$(dpkg --print-architecture)
  pattern="hmailserver_*_$arch.deb"
  candidates=("$packages"/hmailserver_*_"$arch".deb)
else
  arch=$(rpm --eval '%{_arch}')
  pattern="hmailserver-*.$arch.rpm"
  candidates=("$packages"/hmailserver-*."$arch".rpm)
fi
shopt -u nullglob
echo "== $PRETTY_NAME, $(uname -m) ($arch)"
if [ "${#candidates[@]}" != 1 ]; then
  echo "REFUSED: not exactly one $pattern in $packages (found: ${candidates[*]:-none}). This container is $arch:"
  echo "a package is held in a container of its own architecture, and its build job made it or did not (its artifacts say)."
  exit 2
fi
package=${candidates[0]}
found=$(basename "$package")
case "$package" in /*) ;; *) package="./$package" ;; esac
echo "== $found"
version=$(sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' \
  hmailserver/source/Server/Common/Application/Version.h)

# The repositories, tried again when they could not be reached, and failed at once
# otherwise: a dependency the release cannot meet is what this is for. apt over IPv4
# only, as deb-install.sh says why: under qemu-user apt's fall-back from a mirror's
# IPv6 address to its IPv4 one gave up in 5 of 21 arm64 containers on linix. And
# apt-get update exits 0 when it could not fetch an index, so such a warning fails it.
apt_opts=(-o Acquire::ForceIPv4=true -o Acquire::Retries=3)
network_failure='Failed to fetch|Unable to connect|Temporary failure resolving|Could not resolve|Curl error|Cannot download|Failed to download|Download \(curl\) error|Timeout exceeded|Couldn.t resolve|Couldn.t connect|Connection (refused|timed out|reset)'
fetch() {
  local log=$1 try
  shift
  for try in 1 2 3; do
    if "$@" > "$log" 2>&1 && ! grep -q -E '^(W: Failed to fetch|W: Some index files failed|E: )' "$log"; then
      return 0
    fi
    grep -q -E "$network_failure|^W: Some index files failed" "$log" || return 1
    echo "the repositories could not be reached, try $try of 3: $(grep -m 1 -E "$network_failure|^W: Some index files failed" "$log")"
    [ "$try" = 3 ] || sleep 15
  done
  return 1
}

if [ "$kind" = deb ]; then
  step "install it, with what apt resolves for it from $PRETTY_NAME"
  export DEBIAN_FRONTEND=noninteractive
  fetch /tmp/apt.log apt-get update -qq "${apt_opts[@]}" || { tail -n 20 /tmp/apt.log; fail "apt-get update: $PRETTY_NAME's archive could not be read"; }
  echo "Depends: $(dpkg-deb -f "$package" Depends)"
  fetch /tmp/install.log apt-get install -y -qq "${apt_opts[@]}" --no-install-recommends "$package" postgresql-client curl iproute2 \
    || { tail -n 30 /tmp/install.log; fail "apt-get install of $found"; }
  installed=$(dpkg-query -W -f '${Version} ${Architecture} ${Status}' hmailserver)
  echo "installed: $installed"
  [ "$installed" = "$(dpkg-deb -f "$package" Version) $arch install ok installed" ] \
    || fail "dpkg says '$installed', not the file's Version and Architecture, installed"
else
  step "install it, with what dnf resolves for it from $PRETTY_NAME's repositories"
  requires=$(rpm -qp --requires "$package" | grep -v '^rpmlib(' | sort -u)
  grep -v -E '^lib[^ ]*\.so' <<< "$requires" | sed 's/^/  requires /'
  echo "  requires $(grep -c -E '^lib[^ ]*\.so' <<< "$requires") library sonames and symbol versions besides"
  # Without weak dependencies, as rpm-install-test.sh installs: what the package
  # requires comes, glibc's character-set converters among them where dnf is on
  # Fedora. Beside it what this test runs and the image lacks: setpriv and runuser
  # (util-linux; the image has util-linux-core), the PostgreSQL client, ss and cmp.
  fetch /tmp/install.log dnf -y -q --setopt=install_weak_deps=False install "$package" postgresql util-linux iproute diffutils \
    || { tail -n 40 /tmp/install.log; fail "dnf could not install $found with its dependencies"; }
  installed=$(rpm -q --qf '%{VERSION}-%{RELEASE} %{ARCH}' hmailserver)
  echo "installed: $installed"
  [ "$installed" = "$(rpm -qp --qf '%{VERSION}-%{RELEASE} %{ARCH}' "$package")" ] \
    || fail "rpm says '$installed' is installed, not the file's version and architecture"
  echo "glibc's converters, which it requires here: $(rpm -q glibc-gconv-extra)"
fi
answer=$(/usr/bin/hmailserver --version 2>&1) || fail "the installed binary does not start: $answer"
echo "$answer"
case "$answer" in
  "hMailServer $version build "*) ;;
  *) fail "--version answered '$answer', not hMailServer $version, the version of this tree" ;;
esac

step "every file a working installation needs"
files_are_there || fail "files missing"
cmp /usr/share/hmailserver/WebAdmin/index.html hmailserver/installation/WebAdmin/index.html \
  || fail "the installed Control Deck is not the repository's"
for catalogue in hmailserver/installation/WebAdmin/languages/*.json; do
  cmp "$catalogue" "/usr/share/hmailserver/WebAdmin/languages/$(basename "$catalogue")" \
    || fail "the installed catalogue $(basename "$catalogue") is not the repository's"
done
echo "the Deck and its $(ls hmailserver/installation/WebAdmin/languages/*.json | wc -l) catalogues are the repository's, byte for byte"

step "the licences it goes out under, as installed"
bash build/ci/packages/check-licenses.sh --installed "${kind%%-*}" || fail "the installed licence files (above)"

if [ "$kind" = deb ]; then
  step "what postinst did with no systemd running"
  getent passwd hmailserver || fail "postinst made no hmailserver user"
  stat -c "%a %U:%G %n" /etc/hmailserver /etc/hmailserver/hMailServer.ini /var/lib/hmailserver /var/log/hmailserver
  # Enabled, not started: deb-systemd-helper links both units into
  # multi-user.target.wants, which their [Install] sections name, and it does so
  # with no systemd running. (The old job asked systemctl is-enabled; there is no
  # systemd to ask here.)
  if command -v deb-systemd-helper > /dev/null 2>&1; then
    for unit in hmailserver.service hmailserver-update.path; do
      link="/etc/systemd/system/multi-user.target.wants/$unit"
      if [ -L "$link" ]; then
        echo "$unit: enabled ($link -> $(readlink "$link"))"
      else
        fail "postinst did not enable $unit: no $link"
      fi
    done
  fi
else
  step "what %post did with no systemd running"
  getent passwd hmailserver || fail "%post made no hmailserver user"
  stat -c "%a %U:%G %n" /etc/hmailserver /etc/hmailserver/hMailServer.ini /var/lib/hmailserver /var/log/hmailserver
  # grep reads the whole list: under pipefail an early exit could leave rpm writing
  # into a closed pipe (rpm-install-test.sh).
  rpm -qc hmailserver | grep -x /etc/hmailserver/hMailServer.ini > /dev/null || fail "hMailServer.ini is not a %config file"
fi
owner=$(stat -c '%U:%G %a' /etc/hmailserver/hMailServer.ini)
[ "$owner" = "root:hmailserver 640" ] || fail "the packaged configuration is '$owner', not 'root:hmailserver 640'"
/usr/lib/hmailserver/hmailserver-update check || true

step "configure the packaged INI against the PostgreSQL service"
# Edited, not replaced, as the README tells an administrator. ProtectStoredSecretsWithDPAPI
# and WindowsEventLogEnabled take effect because the settings store has no row
# for them before the first start; RestApiPort is stored with --set-setting below.
INI=/etc/hmailserver/hMailServer.ini
sed -i \
  -e 's|^Type=.*|Type=PostgreSQL|' \
  -e "s|^Server=.*|Server=$PGH|" \
  -e 's|^Database=.*|Database=hmpackage|' \
  -e 's|^Username=.*|Username=hmailserver|' \
  -e "s|^Password=.*|Password=$PGPASSWORD|" \
  -e 's|^Port=.*|Port=5432|' \
  -e 's|^ProtectStoredSecretsWithDPAPI=.*|ProtectStoredSecretsWithDPAPI=0|' \
  -e 's|^WindowsEventLogEnabled=.*|WindowsEventLogEnabled=0|' \
  "$INI"
grep -nE '^(Type|Server|Database|Username|Port|Internal|RestApiPort|RestApiBindAddress)=' "$INI"
for _ in $(seq 1 60); do
  pg_isready -q -h "$PGH" -U hmailserver -d hmpackage && break
  sleep 1
done
pg_isready -h "$PGH" -U hmailserver -d hmpackage || fail "PostgreSQL at $PGH did not answer in a minute"

step "check the configuration, create the schema, set the administrator password - as the service would"
runuser -u hmailserver -- /usr/bin/hmailserver --check-config || fail "--check-config as hmailserver"
runuser -u hmailserver -- /usr/bin/hmailserver --create-database || fail "--create-database as hmailserver"
psql -h "$PGH" -U hmailserver -d hmpackage -Atc "select 'schema ' || value from hm_dbversion"
psql -h "$PGH" -U hmailserver -d hmpackage -Atc "select portprotocol, portnumber from hm_tcpipports order by 1"
echo "$ADMIN_PASSWORD" | /usr/bin/hmailserver --set-admin-password || fail "--set-admin-password"
owner=$(stat -c '%U:%G %a' "$INI")
echo "the configuration after --set-admin-password: $owner"
[ "$owner" = "root:hmailserver 640" ] || fail "--set-admin-password left $INI as '$owner', not 'root:hmailserver 640': the service could no longer read it"

step "store the REST listener with the server's own verb"
out=$(/usr/bin/hmailserver --set-setting "RestApiPort=$REST_PORT" 2>&1); rc=$?
echo "$out"
[ "$rc" = 0 ] && grep -qF 'stored in the database' <<< "$out" || fail "--set-setting RestApiPort=$REST_PORT exited $rc and did not say it stored the setting"
stored=$(psql -h "$PGH" -U hmailserver -d hmpackage -Atc "select inisettingvalue from hm_inisettings where inisettingname = 'RestApiPort'")
[ "$stored" = "$REST_PORT" ] || fail "hm_inisettings holds RestApiPort='$stored' after --set-setting, not $REST_PORT"
grep -qx "RestApiPort=$REST_PORT" "$INI" || fail "--set-setting did not write RestApiPort=$REST_PORT into the file's copy"
owner=$(stat -c '%U:%G %a' "$INI")
[ "$owner" = "root:hmailserver 640" ] || fail "--set-setting left $INI as '$owner'"
rootowned=$(find /var/log/hmailserver -user root -type f)
[ -z "$rootowned" ] || fail "files in /var/log/hmailserver are root's after the verbs run as root, so the service cannot write them: $rootowned"
for bad in NoSuchKey=1 RestApiPort=99999 RestApiBindAddress=not-an-address; do
  out=$(/usr/bin/hmailserver --set-setting "$bad" 2>&1); rc=$?
  echo "--set-setting $bad: exit $rc: $out"
  [ "$rc" = 2 ] || fail "--set-setting $bad exited $rc, not 2"
done
out=$(/usr/bin/hmailserver --set-setting SRSSecret=ci-argv-secret 2>&1); rc=$?
{ [ "$rc" = 2 ] && ! grep -qF ci-argv-secret <<< "$out"; } || fail "--set-setting SRSSecret=<value> exited $rc (not 2), or printed the value"
out=$(printf 'ci-stdin-secret\n' | /usr/bin/hmailserver --set-setting SRSSecret 2>&1); rc=$?
{ [ "$rc" = 0 ] && ! grep -qF ci-stdin-secret <<< "$out" && grep -qF 'SRSSecret=(set) stored in the database' <<< "$out"; } \
  || fail "--set-setting SRSSecret from standard input exited $rc, printed the value, or did not say it stored it"
echo "a secret on the command line is refused, and one on standard input is stored and printed as (set)"

step "start it as its unit does: hmailserver, in /var/lib/hmailserver, CAP_NET_BIND_SERVICE and no new privileges"
install -d -o hmailserver -g hmailserver -m 0750 /run/hmailserver
( cd /var/lib/hmailserver && umask 0027 && exec setpriv --reuid=hmailserver --regid=hmailserver --init-groups \
    --inh-caps=+net_bind_service --ambient-caps=+net_bind_service --bounding-set=-all,+net_bind_service \
    --no-new-privs /usr/bin/hmailserver --foreground ) > /tmp/hm-stdout.log 2>&1 &
pid=$!
echo "$pid" > /tmp/hm.pid
started=$SECONDS
up=""
# Each request is given what is left of the wait, 30 s at most, so that a server that
# takes the connection on its REST port and never answers fails within HM_START_WAIT,
# not at the job's timeout.
while [ $((SECONDS - started)) -lt "$START_WAIT" ]; do
  left=$((START_WAIT - (SECONDS - started)))
  if curl -fs -o /dev/null --max-time "$((left < 30 ? left : 30))" -u "Administrator:$ADMIN_PASSWORD" \
      "http://127.0.0.1:$REST_PORT/api/v1/status"; then
    up=yes
    break
  fi
  if ! kill -0 "$pid" 2> /dev/null; then
    wait "$pid"
    fail "the server exited, with status $?, $((SECONDS - started)) s after it was started and before its REST API answered on $REST_PORT"
  fi
  sleep 1
done
[ -n "$up" ] || fail "the server did not answer on $REST_PORT within $START_WAIT s (HM_START_WAIT)"
echo "the REST API answered $((SECONDS - started)) s after the start"
# Under qemu-user ss has no netlink, and lists the sockets without their process.
ss -lntp 2> /dev/null | grep -E ":(25|110|143|587|$REST_PORT)\b" || true

step "port 25 answers SMTP with the server's own banner"
# The banner of a server whose welcome message is not set, as SMTPConnection's
# GetBannerText_ writes it: 220, the machine's name as gethostname gives it, ESMTP.
# A banner of another program listening on 25 is not it. Then EHLO and its whole
# answer, and QUIT.
banner="220 $(cat /proc/sys/kernel/hostname) ESMTP"
smtp_conversation() {
  local line
  exec 3<> /dev/tcp/127.0.0.1/25 || return 1
  IFS= read -r -t 30 line <&3 || { exec 3>&-; echo "no banner within 30 s"; return 2; }
  line="${line%$'\r'}"
  echo "S: $line"
  [ "$line" = "$banner" ] || { exec 3>&-; echo "the banner is not '$banner'"; return 2; }
  printf 'EHLO package-test.invalid\r\n' >&3
  echo "C: EHLO package-test.invalid"
  while :; do
    IFS= read -r -t 30 line <&3 || { exec 3>&-; echo "EHLO had no whole answer within 30 s"; return 2; }
    line="${line%$'\r'}"
    echo "S: $line"
    case "$line" in
      250-*) ;;
      250\ *) break ;;
      *) exec 3>&-; echo "EHLO was answered with something else than 250"; return 2 ;;
    esac
  done
  printf 'QUIT\r\n' >&3
  echo "C: QUIT"
  IFS= read -r -t 30 line <&3 || line="(nothing)"
  line="${line%$'\r'}"
  echo "S: $line"
  exec 3>&-
  case "$line" in 221*) return 0 ;; *) echo "QUIT was not answered 221"; return 2 ;; esac
}
said=""
while :; do
  said=$(smtp_conversation 2> /dev/null); rc=$?
  [ "$rc" = 1 ] || break
  kill -0 "$pid" 2> /dev/null || fail "the server stopped before port 25 answered"
  [ $((SECONDS - started)) -lt "$START_WAIT" ] || fail "nothing answered on port 25 within $START_WAIT s of the start (HM_START_WAIT)"
  sleep 1
done
echo "$said"
[ "$rc" = 0 ] || fail "port 25 did not hold an SMTP conversation with the server: $(echo "$said" | tail -n 1)"
echo "port 25 answered with the server's banner, EHLO and QUIT"

step "the REST API answers"
curl -fsS -u "Administrator:$ADMIN_PASSWORD" "http://127.0.0.1:$REST_PORT/api/v1/status" || fail "GET /api/v1/status"
echo ""

step "GET / returns the Control Deck and not the stub"
curl -fsS -D /tmp/headers.txt -o /tmp/deck.html "http://127.0.0.1:$REST_PORT/" || fail "GET /"
head -n 1 /tmp/headers.txt
grep -i '^content-type' /tmp/headers.txt
grep -m 1 -o '<title>[^<]*</title>' /tmp/deck.html || true
grep -q 'Web administration page not installed' /tmp/deck.html && fail "GET / returned the built-in stub"
grep -q 'Control Deck' /tmp/deck.html || fail "GET / returned something that is not the Control Deck"
cmp /tmp/deck.html /usr/share/hmailserver/WebAdmin/index.html || fail "GET / is not the installed page"
echo "GET / served the installed Control Deck, byte for byte."

step "GET /deck-lang/de.json returns the installed catalogue, and nothing else is served"
curl -fsS -D /tmp/lang-headers.txt -o /tmp/deck-de.json "http://127.0.0.1:$REST_PORT/deck-lang/de.json" || fail "GET /deck-lang/de.json"
grep -qi '^content-type: application/json' /tmp/lang-headers.txt || fail "de.json is not served as JSON"
cmp /tmp/deck-de.json /usr/share/hmailserver/WebAdmin/languages/de.json || fail "de.json is not the installed catalogue"
for wrong in xx ../index ..%2Findex de.json; do
  code=$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "http://127.0.0.1:$REST_PORT/deck-lang/${wrong}.json")
  [ "$code" = 404 ] || fail "/deck-lang/${wrong}.json answered $code; a code that is not an installed language is 404"
done
echo "the German catalogue, byte for byte, and a 404 for what is not a language"

step "stop it, and remove the package"
stop_server
tail -n 5 /tmp/hm-stdout.log
if [ "$kind" = deb ]; then
  apt-get remove -y -qq hmailserver > /tmp/remove.log 2>&1 || { tail -n 20 /tmp/remove.log; fail "apt-get remove"; }
else
  rpm -e hmailserver > /tmp/remove.log 2>&1 || { tail -n 20 /tmp/remove.log; fail "rpm -e"; }
fi
[ ! -f /usr/bin/hmailserver ] || fail "/usr/bin/hmailserver is still there after the package was removed"
echo ""
echo "PACKAGE PASSED: $found installs on $PRETTY_NAME $arch with its dependencies, the packaged server starts as its unit starts it, answers SMTP on 25 and its REST API, serves the Deck, and comes off again."
