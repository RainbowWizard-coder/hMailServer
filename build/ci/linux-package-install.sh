#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-package-deb and linux-package-rpm jobs of .gitlab-ci.yml: the
# packages the linux-build job made (linux-build/packages), installed in a clean
# distribution container - the job's own image, which is a stock distribution
# image and not the CI image - and held to what a working installation needs.
# It is the old GitHub workflow linux-build.yml's package-install job, moved from
# a hosted runner with systemd into a container without it.
#
#   bash build/ci/linux-package-install.sh deb   in ubuntu:26.04, beside a
#                                                PostgreSQL service ("postgres")
#   bash build/ci/linux-package-install.sh rpm   in Fedora
#
# The .deb: apt installs it with its dependencies from Ubuntu's archive; every
# file a working installation needs is where the package says, and its licences
# with it (build/ci/packages/check-licenses.sh, which the .rpm half runs too);
# the packaged configuration is edited the way packaging/README.md tells an
# administrator to; the server checks it, creates the schema and stores its
# settings as the service user; then it is started the way its unit starts it -
# as hmailserver, in /var/lib/hmailserver, with the one capability the unit
# grants and no new privileges - and asked whether the REST API answers and
# whether GET / is the Control Deck. Last, it is removed.
#
# What a container cannot prove, and the hosted runner did: the start under
# systemd itself, with the unit's whole sandbox (ProtectSystem=strict, the
# syscall filter) in force. The unit's syntax is verified by the update-helper
# job's systemd-analyze, and the unit's user, directory and capability are
# reproduced here with setpriv. The privileged listeners bind in any case in a
# container, because docker sets net.ipv4.ip_unprivileged_port_start to 0.
#
# The .rpm: the payload and the scriptlets, as before - installed with --nodeps.
# The package is built on Ubuntu, so its automatic requires are that build's
# sonames and symbol versions; dnf resolved them on Fedora 43 and 44 on
# 26 September 2026 (docs/PackageManagers.md, "apt and dnf"), but this job holds the
# package, not a distribution's libraries, which are a question for a build made on it.
set -uo pipefail

kind="${1:-}"
packages="${HM_PACKAGES_DIR:-linux-build/packages}"
REST_PORT="${HM_REST_PORT:-8046}"
ADMIN_PASSWORD="${HM_ADMIN_PASSWORD:-testar}"
PGH="${HM_PG_HOST:-postgres}"
export PGPASSWORD="${HM_PG_PASSWORD:-hmtest}"

if [ ! -d "$packages" ] || [ -z "$(ls "$packages" 2> /dev/null)" ]; then
  echo "REFUSED: no packages in $packages. The packaging step of the linux-build job made none;"
  echo "its artifact linux-build/package.log says why."
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

# ---------------------------------------------------------------- the .rpm
if [ "$kind" = rpm ]; then
  set -e
  . /etc/os-release
  echo "== $PRETTY_NAME"
  rpm=$(ls "$packages"/hmailserver-*.x86_64.rpm | head -n 1)
  echo "== $(basename "$rpm")"
  rpm -qip "$rpm"
  # The one dependency every RPM distribution meets with its own package
  # (CMakeLists.txt): this .rpm is the one the live update offers each of them.
  # Read, not resolved: --nodeps below leaves it out, and rpm-install-test.sh
  # installs the per-distribution packages with it.
  echo "== it requires glibc's character-set converters"
  rpm -qp --requires "$rpm" | grep -xF '(glibc-gconv-extra(x86-64) or glibc-gconv-modules-extra(x86-64))'
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

if [ "$kind" != deb ]; then
  echo "usage: $0 deb|rpm"
  exit 64
fi

# ---------------------------------------------------------------- the .deb
. /etc/os-release
echo "== $PRETTY_NAME"
deb=$(ls "$packages"/hmailserver_*_amd64.deb | head -n 1)
echo "== $(basename "$deb")"

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

step "install it, with what apt resolves for it from $PRETTY_NAME"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq > /dev/null || fail "apt-get update"
echo "Depends: $(dpkg-deb -f "$deb" Depends)"
apt-get install -y -qq --no-install-recommends "./$deb" postgresql-client curl iproute2 > /tmp/install.log 2>&1 \
  || { tail -n 30 /tmp/install.log; fail "apt-get install of $(basename "$deb")"; }
/usr/bin/hmailserver --version || fail "the installed binary does not start"

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
bash build/ci/packages/check-licenses.sh --installed deb || fail "the installed licence files (above)"

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
echo $! > /tmp/hm.pid
up=""
for _ in $(seq 1 60); do
  if curl -fs -o /dev/null -u "Administrator:$ADMIN_PASSWORD" "http://127.0.0.1:$REST_PORT/api/v1/status"; then
    up=yes
    break
  fi
  kill -0 "$(cat /tmp/hm.pid)" 2> /dev/null || break
  sleep 1
done
[ -n "$up" ] || fail "the server did not answer on $REST_PORT in a minute"
ss -lntp | grep -E ":(25|110|143|587|$REST_PORT)\b" || true

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
apt-get remove -y -qq hmailserver > /tmp/remove.log 2>&1 || { tail -n 20 /tmp/remove.log; fail "apt-get remove"; }
[ ! -f /usr/bin/hmailserver ] || fail "/usr/bin/hmailserver is still there after apt-get remove"
echo ""
echo "PACKAGE PASSED: the .deb installs on $PRETTY_NAME, the packaged server starts as its unit starts it, serves the Deck, and comes off again."
