#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# WHAT THIS PROVES
#
# The update check knows where the server runs (docs/LiveUpdateKinds.md), and a
# build without the live update has none of it:
#
#   * started in a snap (SNAP, SNAP_NAME and SNAP_INSTANCE_NAME in its
#     environment, as snapd sets them, SNAP naming the directory the server's
#     own file is in, as a snap's mounted revision is), the built server's
#     update check reports installKind "snap" and liveUpdate false, names "snap
#     refresh hmailserver", and its download and install refuse with that
#     sentence - nothing is fetched from the release and nothing is written to
#     its Updates directory; a snap name snapd never gives (a leading hyphen, a
#     shell's characters) is not named, and the snap's own name is;
#   * a SNAP the server merely inherited - a snap's terminal passes its own to
#     whatever is started from it - names a directory the server is not in, and
#     is not taken for a snap: an AppImage started so keeps its live update;
#   * started with HM_INSTALL_KIND=container (what the image's Dockerfile sets),
#     in any case, the same with installKind "container" and "docker pull
#     registry.gitlab.com/hmailserver/hmailserver:<the release>"; any other
#     value of HM_INSTALL_KIND is not a kind, and changes nothing;
#   * with --off-server, a server built with -DHM_LIVE_UPDATE=OFF reports
#     installKind "distribution" and names the command it was built with
#     (--off-command), and in a snap still says snap refresh;
#   * with --install-off, `cmake --install` of that build into an empty DESTDIR
#     leaves no hmailserver-update and neither of its units, while it does leave
#     the server and its own unit; --install-on shows a default build's install
#     leaves all three, so the check is looking in the right place.
#
# The server is the real one, started under each environment with nothing else
# of the caller's environment (env -i): a REST fixture cannot set a variable in
# the server's own environment, which is why this is a script and not a fixture.
# It reads a fake release feed on the loopback (UpdateFeedUrl), which offers a
# release far newer than any build, with every platform's file and bundle, and
# records every request it answers. Two controls keep the "nothing fetched"
# checks honest: the same server as an AppImage (APPIMAGE set) DOES fetch its
# file's bundle from the feed, so a fetch would be seen; and a server run with
# no marker at all says what a build tree always said (installKind "none", no
# command), so the kinds come from the markers and not from this machine.
#
# It needs PostgreSQL (the linux-suite job's service, or any the caller names),
# psql, curl and python3. The server's listeners are removed from the database,
# so nothing binds a mail port; the REST listener is on the loopback only.
#
# USAGE
#   build/test-install-kind.sh --server BINARY [--off-server BINARY
#        --off-command "apt upgrade hmailserver"] [--install-off BUILD_DIR]
#        [--install-on BUILD_DIR] [--rest-port N] [--keep]
#
#   The database: HM_PG_HOST (127.0.0.1), HM_PG_PORT (5432), HM_PG_USER
#   (hmailserver), HM_PG_PASSWORD (hmtest), HM_PG_DATABASE (hminstallkind) - an
#   empty database or one this script used before; nothing else of it is read.
#   The server's own copies of DBScripts and dh2048.pem come from this
#   checkout (--repo-root, the checkout this script is in by default).
#
# Exit status: 0 when every check passes, 1 when any does not, 2 when it could
# not run.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"
SERVER=""
OFF_SERVER=""
OFF_COMMAND=""
INSTALL_OFF=""
INSTALL_ON=""
REST_PORT="${HM_INSTALL_KIND_REST_PORT:-18947}"
KEEP=0

while [ "$#" -gt 0 ]; do
   case "$1" in
      --server) SERVER="${2:-}"; shift 2 ;;
      --off-server) OFF_SERVER="${2:-}"; shift 2 ;;
      --off-command) OFF_COMMAND="${2:-}"; shift 2 ;;
      --install-off) INSTALL_OFF="${2:-}"; shift 2 ;;
      --install-on) INSTALL_ON="${2:-}"; shift 2 ;;
      --rest-port) REST_PORT="${2:-}"; shift 2 ;;
      --repo-root) REPO_ROOT="$(cd "${2:-.}" && pwd)"; shift 2 ;;
      --keep) KEEP=1; shift ;;
      -h|--help) sed -n '5,61p' "${BASH_SOURCE[0]}"; exit 0 ;;
      *) echo "Unknown option: $1" >&2; exit 2 ;;
   esac
done

if [ -z "${SERVER}" ] && [ -z "${INSTALL_OFF}" ]; then
   echo "Give --server (a default build's hmailserver), --install-off (an OFF build directory), or both." >&2
   exit 2
fi
for binary in "${SERVER}" "${OFF_SERVER}"; do
   if [ -n "${binary}" ] && [ ! -x "${binary}" ]; then
      echo "No server binary at ${binary}." >&2
      exit 2
   fi
done
if [ -n "${OFF_SERVER}" ] && [ -z "${OFF_COMMAND}" ]; then
   echo "--off-server needs --off-command: the HM_UPDATE_COMMAND it was built with, which its check must name." >&2
   exit 2
fi

failures=0
passes=0
fail() { echo "  FAIL: $*"; failures=$((failures + 1)); }
pass() { echo "  ok    $*"; passes=$((passes + 1)); }

WORK="$(mktemp -d)"
FEED_PID=""
SERVER_PID=""
cleanup() {
   stop_server
   if [ -n "${FEED_PID}" ]; then
      kill "${FEED_PID}" 2> /dev/null || true
      wait "${FEED_PID}" 2> /dev/null || true
   fi
   if [ "${KEEP}" = 0 ]; then
      rm -rf "${WORK}"
   else
      echo "Kept ${WORK}"
   fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- the install check

# DESTDIR installs of a build directory: what the OFF build must not leave, and
# what a default build does leave, by the same three names.
check_install() {
   local label="$1" dir="$2" expect="$3" dest
   dest="${WORK}/install-${label}"
   echo "== cmake --install of ${dir} (${label})"
   if [ ! -f "${dir}/CMakeCache.txt" ]; then
      fail "${label}: ${dir} is not a configured build directory"
      return
   fi
   if ! DESTDIR="${dest}" cmake --install "${dir}" > "${WORK}/install-${label}.log" 2>&1; then
      tail -n 20 "${WORK}/install-${label}.log"
      fail "${label}: cmake --install failed"
      return
   fi
   local option
   option="$(sed -n 's/^HM_LIVE_UPDATE:BOOL=//p' "${dir}/CMakeCache.txt")"
   echo "  the build's HM_LIVE_UPDATE: ${option:-unset}"
   if [ -x "${dest}/usr/bin/hmailserver" ] && [ -f "${dest}/usr/lib/systemd/system/hmailserver.service" ]; then
      pass "${label}: the server and hmailserver.service are installed"
   else
      fail "${label}: the install has no usr/bin/hmailserver or no hmailserver.service, so it is not an install to judge"
   fi
   local found
   found="$(cd "${dest}" && find . -name 'hmailserver-update*' | sort)"
   if [ "${expect}" = none ]; then
      if [ -z "${found}" ]; then
         pass "${label}: no hmailserver-update, no hmailserver-update.path, no hmailserver-update.service anywhere in the install"
      else
         fail "${label}: the install carries the live update's helper or units:"
         printf '%s\n' "${found}" | sed 's/^/         /'
      fi
   else
      local want
      for want in ./usr/lib/hmailserver/hmailserver-update ./usr/lib/systemd/system/hmailserver-update.path ./usr/lib/systemd/system/hmailserver-update.service; do
         # A here-string: printf into grep -q, which stops at the match, can die of
         # SIGPIPE writing the rest, and pipefail then reads the file as missing.
         if grep -qxF "${want}" <<< "${found}"; then
            pass "${label}: ${want#./} is installed"
         else
            fail "${label}: ${want#./} is not installed"
         fi
      done
   fi
}

if [ -n "${INSTALL_OFF}" ]; then
   check_install off "${INSTALL_OFF}" none
fi
if [ -n "${INSTALL_ON}" ]; then
   check_install on "${INSTALL_ON}" all
fi

if [ -z "${SERVER}" ]; then
   echo ""
   echo "${passes} passed, ${failures} failed"
   [ "${failures}" -eq 0 ] || exit 1
   exit 0
fi

for tool in curl python3 psql; do
   command -v "${tool}" > /dev/null 2>&1 || { echo "${tool} is needed and is not on PATH." >&2; exit 2; }
done

# ---------------------------------------------------------------- the fake feed

# Far newer than any build, so every server here has a release on offer; every
# platform's file with its bundle, so a server that would fetch one finds it.
# The machine names the AppImage the control's server asks for.
NEWER="99.1.0"
case "$(uname -m)" in
   aarch64|arm64) MACHINE=aarch64 ;;
   *) MACHINE=x86_64 ;;
esac

cat > "${WORK}/feed.py" <<'PY'
import http.server, json, os, sys

work, version = sys.argv[1], sys.argv[2]
names = [
    "hMailServer-%s-x64.exe" % version,
    "hmailserver_%s_amd64.deb" % version, "hmailserver_%s_arm64.deb" % version,
    "hmailserver-%s-1.x86_64.rpm" % version, "hmailserver-%s-1.aarch64.rpm" % version,
    "hMailServer-%s-x86_64.AppImage" % version, "hMailServer-%s-aarch64.AppImage" % version,
]

class Feed(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        with open(os.path.join(work, "requests.log"), "a") as log:
            log.write(self.path + "\n")
        base = "http://127.0.0.1:%d" % self.server.server_address[1]
        if self.path == "/releases/latest":
            assets = []
            for name in names:
                for file in (name, name + ".cosign.bundle"):
                    assets.append({"name": file, "size": 7, "browser_download_url": base + "/download/" + file})
            body = json.dumps({"tag_name": "v" + version, "name": "hMailServer " + version, "draft": False,
                               "prerelease": False, "published_at": "2026-09-28T00:00:00Z",
                               "html_url": base + "/releases/v" + version, "assets": assets}).encode()
        elif self.path.startswith("/download/"):
            body = b"{}" if self.path.endswith(".cosign.bundle") else b"payload"
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = http.server.HTTPServer(("127.0.0.1", 0), Feed)
with open(os.path.join(work, "feed.port.tmp"), "w") as port:
    port.write(str(server.server_address[1]))
os.rename(os.path.join(work, "feed.port.tmp"), os.path.join(work, "feed.port"))
server.serve_forever()
PY

python3 "${WORK}/feed.py" "${WORK}" "${NEWER}" > "${WORK}/feed.log" 2>&1 &
FEED_PID=$!
for _ in $(seq 1 50); do
   [ -f "${WORK}/feed.port" ] && break
   sleep 0.1
done
if [ ! -f "${WORK}/feed.port" ]; then
   cat "${WORK}/feed.log"
   echo "The fake feed did not start." >&2
   exit 2
fi
FEED_URL="http://127.0.0.1:$(cat "${WORK}/feed.port")/releases/latest"
echo "the fake feed: ${FEED_URL}, offering ${NEWER}"

# ---------------------------------------------------------------- the server tree

PGH="${HM_PG_HOST:-127.0.0.1}"
PGP="${HM_PG_PORT:-5432}"
DB="${HM_PG_DATABASE:-hminstallkind}"
export PGUSER="${HM_PG_USER:-hmailserver}"
export PGPASSWORD="${HM_PG_PASSWORD:-hmtest}"

T="${WORK}/tree"
mkdir -p "${T}/program" "${T}/Data" "${T}/Logs" "${T}/Temp" "${T}/Events" "${T}/Database"
cp -r "${REPO_ROOT}/hmailserver/source/DBScripts" "${T}/program/DBScripts"
cp "${REPO_ROOT}/hmailserver/installation/Extras/dh2048.pem" "${T}/program/"
INI="${T}/program/hMailServer.ini"
cat > "${INI}" <<INI
[Directories]
ProgramFolder=${T}/program
DatabaseFolder=${T}/Database
DataFolder=${T}/Data
LogFolder=${T}/Logs
TempFolder=${T}/Temp
EventFolder=${T}/Events
[Database]
Type=PostgreSQL
Server=${PGH}
Port=${PGP}
Username=${PGUSER}
Password=${PGPASSWORD}
Database=${DB}
Internal=0
[Settings]
ProtectStoredSecretsWithDPAPI=0
WindowsEventLogEnabled=0
INI
chmod 0600 "${INI}"

# A password made for this run, given on standard input and to curl in a file
# of its own, never on a command line.
ADMIN_PASSWORD="$(python3 -c 'import secrets; print(secrets.token_hex(16))')"
CURL_CONFIG="${WORK}/curl.config"
( umask 077; printf 'user = "Administrator:%s"\n' "${ADMIN_PASSWORD}" > "${CURL_CONFIG}" )

if ! pg_isready -q -h "${PGH}" -p "${PGP}" -U "${PGUSER}" -d "${DB}"; then
   echo "PostgreSQL at ${PGH}:${PGP} does not answer for ${PGUSER}@${DB}." >&2
   exit 2
fi

prepare() {
   local binary="$1"
   local run=("${binary}" --config "${INI}")
   "${run[@]}" --create-database > "${WORK}/prepare.log" 2>&1 || { cat "${WORK}/prepare.log"; return 1; }
   # No listener: nothing here is about mail, and on a machine whose ports are
   # shared (WSL's mirrored network) a mail port may be somebody else's.
   psql -h "${PGH}" -p "${PGP}" -d "${DB}" -qAtc "delete from hm_tcpipports" >> "${WORK}/prepare.log" 2>&1 || { cat "${WORK}/prepare.log"; return 1; }
   local setting
   for setting in "RestApiPort=${REST_PORT}" "RestApiBindAddress=127.0.0.1" "UpdateFeedUrl=${FEED_URL}" \
                  "UpdateCheckEnabled=0" "UpdateAutoDownload=0" "UpdateChannel=stable"; do
      "${run[@]}" --set-setting "${setting}" >> "${WORK}/prepare.log" 2>&1 || { cat "${WORK}/prepare.log"; return 1; }
   done
   printf '%s\n' "${ADMIN_PASSWORD}" | "${run[@]}" --set-admin-password >> "${WORK}/prepare.log" 2>&1 || { cat "${WORK}/prepare.log"; return 1; }
}

stop_server() {
   if [ -n "${SERVER_PID}" ]; then
      kill "${SERVER_PID}" 2> /dev/null || true
      for _ in $(seq 1 30); do
         kill -0 "${SERVER_PID}" 2> /dev/null || break
         sleep 0.5
      done
      kill -9 "${SERVER_PID}" 2> /dev/null || true
      wait "${SERVER_PID}" 2> /dev/null || true
      SERVER_PID=""
   fi
}

# The server under exactly the environment given, and nothing of this shell's.
start_server() {
   local binary="$1"; shift
   : > "${WORK}/stdout.log"
   ( cd "${T}" && exec env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="${T}" LANG=C.UTF-8 "$@" \
        "${binary}" --config "${INI}" --foreground ) > "${WORK}/stdout.log" 2>&1 &
   SERVER_PID=$!
   for _ in $(seq 1 120); do
      if curl -s -o /dev/null -K "${CURL_CONFIG}" "http://127.0.0.1:${REST_PORT}/api/v1/status"; then
         return 0
      fi
      kill -0 "${SERVER_PID}" 2> /dev/null || break
      sleep 0.5
   done
   echo "  the server did not answer on the REST port; what it said:"
   tail -n 30 "${WORK}/stdout.log"
   return 1
}

rest() {
   local method="$1" path="$2"
   if [ "${method}" = POST ]; then
      curl -s -K "${CURL_CONFIG}" -X POST -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:${REST_PORT}${path}"
   else
      curl -s -K "${CURL_CONFIG}" "http://127.0.0.1:${REST_PORT}${path}"
   fi
}

# One key of a JSON document, as text: true/false for a boolean.
field() {
   python3 -c 'import json, sys
value = json.loads(sys.stdin.read())
for part in sys.argv[1].split("."):
    value = value.get(part, "") if isinstance(value, dict) else ""
print(("true" if value else "false") if isinstance(value, bool) else value)' "$1"
}

expect() {
   local label="$1" document="$2" key="$3" want="$4" got
   got="$(printf '%s' "${document}" | field "${key}")"
   if [ "${got}" = "${want}" ]; then
      pass "${label}: ${key} is ${want:-empty}"
   else
      fail "${label}: ${key} is '${got}', not '${want}'"
   fi
}

expect_contains() {
   local label="$1" document="$2" key="$3" want="$4" got
   got="$(printf '%s' "${document}" | field "${key}")"
   case "${got}" in
      *"${want}"*) pass "${label}: ${key} names '${want}'" ;;
      *) fail "${label}: ${key} does not name '${want}': '${got}'" ;;
   esac
}

# Paths the feed answered since the case began, other than the feed itself.
fetched_since() {
   local from="$1"
   tail -n +"$((from + 1))" "${WORK}/requests.log" 2> /dev/null | grep -v '^/releases/latest$' || true
}

requests_so_far() {
   if [ -f "${WORK}/requests.log" ]; then wc -l < "${WORK}/requests.log"; else echo 0; fi
}

# ---------------------------------------------------------------- the cases

# run_case LABEL BINARY KIND LIVE COMMAND [VAR=VALUE ...]
#   KIND and LIVE are what installKind and liveUpdate must say, COMMAND what
#   updateCommand must be. A server something else updates (LIVE false with a
#   COMMAND, or the distribution kind) must fetch nothing and write nothing.
run_case() {
   local label="$1" binary="$2" kind="$3" live="$4" command="$5"; shift 5
   echo ""
   echo "== ${label}: ${*:-no marker in the environment}"
   rm -rf "${T}/Data/Updates"
   if ! start_server "${binary}" "$@"; then
      fail "${label}: the server did not start"
      stop_server
      return
   fi
   local before
   before="$(requests_so_far)"

   local checked
   checked="$(rest POST /api/v1/update/check)"
   expect "${label}" "${checked}" state 2
   expect "${label}" "${checked}" availableVersion "${NEWER}"
   expect "${label}" "${checked}" installKind "${kind}"
   expect "${label}" "${checked}" liveUpdate "${live}"
   expect "${label}" "${checked}" updateCommand "${command}"

   local advice
   advice="$(printf '%s' "${checked}" | field updateAdvice)"
   local elsewhere=no
   case "${kind}" in snap|container|distribution) elsewhere=yes ;; esac

   if [ "${elsewhere}" = yes ]; then
      # The refusals below are held to this sentence, so it must be there: an
      # empty one would let an empty lastError pass for a refusal.
      if [ -n "${advice}" ]; then
         pass "${label}: updateAdvice says what updates this server"
      else
         fail "${label}: updateAdvice is empty"
      fi
      [ -n "${command}" ] && expect_contains "${label}" "${checked}" updateAdvice "${command}"
      expect "${label}" "${checked}" installer.name ""

      local downloaded installed
      downloaded="$(rest POST /api/v1/update/download)"
      expect "${label} (download)" "${downloaded}" state 2
      expect "${label} (download)" "${downloaded}" lastError "${advice}"
      installed="$(rest POST /api/v1/update/install)"
      expect "${label} (install)" "${installed}" lastError "${advice}"
      local stateName
      stateName="$(printf '%s' "${installed}" | field stateName)"
      if [ "${stateName}" != installing ]; then
         pass "${label} (install): nothing was handed over (state ${stateName})"
      else
         fail "${label} (install): the server says it is installing"
      fi

      local fetched
      fetched="$(fetched_since "${before}")"
      if [ -z "${fetched}" ]; then
         pass "${label}: the feed was asked for the release and for nothing else"
      else
         fail "${label}: the server fetched from the release: $(printf '%s' "${fetched}" | tr '\n' ' ')"
      fi
      if [ -z "$(find "${T}/Data" -path "${T}/Data/Updates/*" -type f 2> /dev/null)" ]; then
         pass "${label}: nothing was written to the Updates directory"
      else
         fail "${label}: the Updates directory has files: $(find "${T}/Data/Updates" -type f | tr '\n' ' ')"
      fi
   else
      expect "${label}" "${checked}" updateAdvice ""
      if [ "${kind}" = appimage ]; then
         # The control: this kind fetches, and the feed sees it.
         expect "${label}" "${checked}" installer.name "hMailServer-${NEWER}-${MACHINE}.AppImage"
         rest POST /api/v1/update/download > /dev/null
         local fetched
         fetched="$(fetched_since "${before}")"
         if grep -qxF "/download/hMailServer-${NEWER}-${MACHINE}.AppImage.cosign.bundle" <<< "${fetched}"; then
            pass "${label}: the download asked the feed for the AppImage's bundle, so a fetch is seen when there is one"
         else
            fail "${label}: the download fetched nothing from the feed ($(printf '%s' "${fetched}" | tr '\n' ' ')), so the checks that nothing is fetched prove nothing"
         fi
      fi
   fi
   stop_server
}

echo ""
echo "== the server: ${SERVER}"
"${SERVER}" --version | head -n 1
if ! prepare "${SERVER}"; then
   echo "The database could not be prepared." >&2
   exit 2
fi

# A snap's mounted revision is the directory its programs are in: SNAP here is
# the one the server's file is in, by its physical path, as /proc/self/exe
# names it.
snap_of() {
   (cd "$(dirname "$1")" && pwd -P)
}

run_case snap "${SERVER}" snap false "snap refresh hmailserver" \
   SNAP="$(snap_of "${SERVER}")" SNAP_NAME=hmailserver SNAP_INSTANCE_NAME=hmailserver SNAP_REVISION=x1
run_case snap-parallel-instance "${SERVER}" snap false "snap refresh hmailserver_edge" \
   SNAP="$(snap_of "${SERVER}")/" SNAP_NAME=hmailserver SNAP_INSTANCE_NAME=hmailserver_edge SNAP_REVISION=x1
run_case snap-names-snapd-never-gives "${SERVER}" snap false "snap refresh hmailserver" \
   SNAP="$(snap_of "${SERVER}")" SNAP_NAME='hm;id' SNAP_INSTANCE_NAME='--devmode' SNAP_REVISION=x1
run_case container "${SERVER}" container false "docker pull registry.gitlab.com/hmailserver/hmailserver:${NEWER}" \
   HM_INSTALL_KIND=container
run_case container-in-capitals "${SERVER}" container false "docker pull registry.gitlab.com/hmailserver/hmailserver:${NEWER}" \
   HM_INSTALL_KIND=Container
run_case appimage-control "${SERVER}" appimage true "" \
   APPIMAGE="${WORK}/hMailServer.AppImage" APPDIR="${WORK}/appdir"
run_case appimage-under-an-inherited-snap "${SERVER}" appimage true "" \
   SNAP=/snap/code/211 SNAP_NAME=code SNAP_INSTANCE_NAME=code SNAP_REVISION=211 \
   APPIMAGE="${WORK}/hMailServer.AppImage" APPDIR="${WORK}/appdir"
run_case no-marker "${SERVER}" none false ""
run_case unknown-kind "${SERVER}" none false "" HM_INSTALL_KIND=appliance

if [ -n "${OFF_SERVER}" ]; then
   echo ""
   echo "== the server built without the live update: ${OFF_SERVER}"
   "${OFF_SERVER}" --version | head -n 1
   run_case off-build "${OFF_SERVER}" distribution false "${OFF_COMMAND}"
   run_case off-build-in-a-snap "${OFF_SERVER}" snap false "snap refresh hmailserver" \
      SNAP="$(snap_of "${OFF_SERVER}")" SNAP_NAME=hmailserver SNAP_INSTANCE_NAME=hmailserver SNAP_REVISION=x1
   run_case off-build-in-a-container "${OFF_SERVER}" container false "docker pull registry.gitlab.com/hmailserver/hmailserver:${NEWER}" \
      HM_INSTALL_KIND=container
   run_case off-build-as-an-appimage "${OFF_SERVER}" distribution false "${OFF_COMMAND}" \
      APPIMAGE="${WORK}/hMailServer.AppImage" APPDIR="${WORK}/appdir"
fi

echo ""
echo "${passes} passed, ${failures} failed"
[ "${failures}" -eq 0 ] || exit 1
exit 0
