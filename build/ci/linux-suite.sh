#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-suite job of .gitlab-ci.yml: the Linux server the linux-build job made,
# laid out as the lab VM's bench lays it out (PostgreSQL, the listeners off the
# privileged ports, auto-ban off, the test domain, the administrator password), then
# the WHOLE of hmailserver/test/LinuxRegressionTests against it. Run in the image
# of build/ci/linux.Dockerfile from the root of a checkout, beside a PostgreSQL the
# job starts as a service.
#
#   bash build/ci/linux-suite.sh           lay out, start, run, stop; exit 0 only for
#                                          "Failed 0" with at least one test passed
#   bash build/ci/linux-suite.sh collect   the job's after_script: the server's logs
#                                          into the artifacts, then the tree removed
#
# Skipped tests are expected (a fixture the REST API cannot drive says why and
# skips); what fails the job is a failed test, a run of no tests, or a server that
# did not start.
#
# Where things are. The tree is inside the checkout (HM_TREE, .hmtest-tree) and not
# in /tmp, because GitLab runs after_script in a fresh container from the same
# image with only the checkout carried over: the logs of a run that timed out or
# was cancelled are still there to collect. collect removes the tree; the results
# stay in linux-suite/ for the artifacts until the next job's checkout cleans them.
#
# How it differs from the VM's bench.sh, and why:
#   * the database is the job's postgres service (HM_PG_HOST, "postgres"), created
#     empty by the service, not a database dropped and made on the VM's own server;
#   * the submission listener keeps its stock 587, because the job runs as root in
#     its container and can bind it (the VM moves it to 5870; the one fixture that
#     judges that listener by number asks TestPorts.Actual(587), so either works);
#   * the suite serves its fake DNS zone (the VM sets HMTEST_NO_FAKE_DNS=1, as an
#     ordinary user cannot bind port 53); set HMTEST_NO_FAKE_DNS=1 in the job to
#     run without it, and the DNS fixtures skip.
#
# HM_SOURCE_TARBALL, as in build/ci/linux-build.sh: the suite, the database
# scripts and the data files are taken from the source tarball it names (or the
# one hmailserver-*.tar.gz in the directory it names), unpacked into
# HM_SOURCE_DIR (/tmp/hm-source) and checked first, and not from the checkout. The
# test tree, the results and the server binary are where they are without it,
# relative to where the script was started, and collect reads nothing of the
# tarball.
set -uo pipefail

T="${HM_TREE:-$PWD/.hmtest-tree}"
OUT="${HM_RESULTS_DIR:-$PWD/linux-suite}"
BINARY="${HM_SERVER_BINARY:-$PWD/linux-build/hmailserver}"
if [ -n "${HM_SOURCE_TARBALL:-}" ]; then
  # Where the script was started, not where the tarball is unpacked.
  case "$T" in /*) ;; *) T="$PWD/$T" ;; esac
  case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
  case "$BINARY" in /*) ;; *) BINARY="$PWD/$BINARY" ;; esac
fi
PGH="${HM_PG_HOST:-postgres}"
DB="${HM_PG_DATABASE:-hmtest}"
export PGUSER="${HM_PG_USER:-hmailserver}"
export PGPASSWORD="${HM_PG_PASSWORD:-hmtest}"
# The server judges free space on the volume holding its data directory: below
# 1024 MB it starts in its warning band, below 100 MB it refuses mail, and a whole
# run's log is written to the same volume (the VM's bench.sh refuses for the same
# reason).
MIN_FREE_MB="${HM_MIN_FREE_MB:-4096}"

export HMTEST_HOST=127.0.0.1
export HMTEST_SMTP_PORT=2525
export HMTEST_POP3_PORT=1110
export HMTEST_IMAP_PORT=1143
export HMTEST_SUBMISSION_PORT=587
export HMTEST_REST_PORT=8045
export HMTEST_ADMIN_PASSWORD=testar
export HMTEST_SERVER_INI="$T/program/hMailServer.ini"

stop_server() {
  if [ -f "$T/server.pid" ]; then
    local pid
    pid=$(cat "$T/server.pid")
    if kill -0 "$pid" 2> /dev/null; then
      kill "$pid" 2> /dev/null || true
      for _ in $(seq 1 20); do
        kill -0 "$pid" 2> /dev/null || break
        sleep 1
      done
      kill -9 "$pid" 2> /dev/null || true
    fi
    rm -f "$T/server.pid"
  fi
}

collect() {
  mkdir -p "$OUT"
  stop_server
  if [ -f "$OUT/linux-suite.trx" ] && [ ! -f "$OUT/junit.xml" ]; then
    python3 build/ci/trx-to-junit.py "$OUT/linux-suite.trx" "$OUT/junit.xml" || true
  fi
  if [ -d "$T" ]; then
    [ -f "$T/stdout.log" ] && cp "$T/stdout.log" "$OUT/server-stdout.log"
    [ -f "$T/program/hMailServer.ini" ] && cp "$T/program/hMailServer.ini" "$OUT/hMailServer.ini"
    if [ -d "$T/Logs" ]; then
      # The error logs always; every log, compressed, when the job did not pass.
      find "$T/Logs" -maxdepth 1 -type f -name 'ERROR_*' -exec cp {} "$OUT/" \;
      if [ "${CI_JOB_STATUS:-failed}" != "success" ]; then
        tar -C "$T" -czf "$OUT/server-logs.tar.gz" Logs && echo "the server's logs: linux-suite/server-logs.tar.gz"
      fi
    fi
    rm -rf "$T"
    echo "the test tree is removed"
  fi
  ls -la "$OUT" 2> /dev/null || true
}

if [ "${1:-}" = "collect" ]; then
  collect
  exit 0
fi

if [ -n "${HM_SOURCE_TARBALL:-}" ]; then
  source_root=$(bash "$(dirname "${BASH_SOURCE[0]}")/source-tarball.sh" unpack "$HM_SOURCE_TARBALL" "${HM_SOURCE_DIR:-/tmp/hm-source}") \
    || exit 2
  cd "$source_root" || exit 2
  echo "the suite and its data from the source tarball, unpacked at $source_root"
fi

if [ ! -f hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj ]; then
  echo "Run this from the root of a checkout (no hmailserver/test/LinuxRegressionTests here)."
  exit 2
fi
if [ ! -x "$BINARY" ] && [ ! -f "$BINARY" ]; then
  echo "REFUSED: no server binary at $BINARY - the linux-build job's artifact is missing."
  exit 2
fi

# HM_LINUX_SUITE_FILTER, when set, is a dotnet test filter: part of the suite, for
# build/ci/linux-coverage.sh measuring a part ("all", or unset, is the whole). A
# variable of its own and not build/lab/db-suite.sh's HM_SUITE_FILTER, whose unset
# value is "db": a project or group variable of that name, meant for the db-matrix
# job, would have made this a silent part of the suite. The linux-suite job is the
# pass or fail truth, so it refuses a filter whatever the project defines.
filter=()
if [ -n "${HM_LINUX_SUITE_FILTER:-}" ] && [ "$HM_LINUX_SUITE_FILTER" != "all" ]; then
  if [ "${CI_JOB_NAME:-}" = "linux-suite" ]; then
    echo "REFUSED: HM_LINUX_SUITE_FILTER is set ($HM_LINUX_SUITE_FILTER), and the linux-suite job runs the whole suite."
    exit 2
  fi
  filter=(--filter "$HM_LINUX_SUITE_FILTER")
fi

rm -rf "$T" "$OUT"
mkdir -p "$T/program" "$T/Data" "$T/Logs" "$T/Temp" "$T/Events" "$T/Database" "$OUT"

free_mb=$(df -Pm "$T" | awk 'NR==2 {print $4}')
if [ "${free_mb:-0}" -lt "$MIN_FREE_MB" ]; then
  echo "REFUSED: $free_mb MB free on the volume holding $T, and the run needs $MIN_FREE_MB MB."
  df -h "$T"
  exit 2
fi
echo "$free_mb MB free where the tree is"

echo "== the server tree"
cp "$BINARY" "$T/program/hmailserver"
chmod +x "$T/program/hmailserver"
cp -r hmailserver/source/DBScripts "$T/program/DBScripts"
cp hmailserver/installation/Extras/dh2048.pem "$T/program/"
sha256sum "$T/program/hmailserver"

echo "== the database ($PGUSER@$PGH/$DB)"
ready=""
for _ in $(seq 1 60); do
  if pg_isready -q -h "$PGH" -U "$PGUSER" -d "$DB"; then
    ready=yes
    break
  fi
  sleep 1
done
if [ -z "$ready" ]; then
  echo "REFUSED: PostgreSQL at $PGH did not answer in a minute."
  exit 2
fi
psql -h "$PGH" -d "$DB" -Atc "select version()"

cat > "$T/program/hMailServer.ini" <<INI
[Directories]
ProgramFolder=$T/program
DatabaseFolder=$T/Database
DataFolder=$T/Data
LogFolder=$T/Logs
TempFolder=$T/Temp
EventFolder=$T/Events
[Database]
Type=PostgreSQL
Server=$PGH
Port=5432
Username=$PGUSER
Password=$PGPASSWORD
Database=$DB
Internal=0
[Settings]
ProtectStoredSecretsWithDPAPI=0
TlsKeyExchangeGroups=X25519:secp384r1:secp256r1
RestApiPort=$HMTEST_REST_PORT
RestApiBindAddress=127.0.0.1
WindowsEventLogEnabled=0
INI

server=("$T/program/hmailserver" --config "$T/program/hMailServer.ini")
set -e
"${server[@]}" --version
"${server[@]}" --create-database
psql -h "$PGH" -d "$DB" -Atc "select 'schema ' || value from hm_dbversion"
# The listeners are rows. SMTP, POP3 and IMAP move to the ports the suite expects
# (HMTEST_* above); the submission listener stays on 587.
psql -h "$PGH" -d "$DB" -Atc "
  update hm_tcpipports set portnumber = 2525 where portnumber = 25;
  update hm_tcpipports set portnumber = 1110 where portnumber = 110;
  update hm_tcpipports set portnumber = 1143 where portnumber = 143;
  select portprotocol, portnumber from hm_tcpipports order by 1, 2"
# The suite fails logons on purpose, and three of them would ban 127.0.0.1 for an
# hour; the Windows setup switches this off over COM, which is not here.
psql -h "$PGH" -d "$DB" -Atc "
  update hm_settings set settinginteger = 0 where settingname = 'AutoBanOnLogonFailureEnabled';
  select settingname, settinginteger from hm_settings where settingname = 'AutoBanOnLogonFailureEnabled'"
# The test domain, as a row: every column declared NOT NULL without a default,
# with the value a new domain gets in the Control Panel.
psql -h "$PGH" -d "$DB" -Atc "
  insert into hm_domains (domainname, domainactive, domainpostmaster, domainmaxsize, domainaddomain,
    domainmaxmessagesize, domainuseplusaddressing, domainplusaddressingchar, domainantispamoptions,
    domainenablesignature, domainsignaturemethod, domainsignatureplaintext, domainsignaturehtml,
    domainaddsignaturestoreplies, domainaddsignaturestolocalemail, domainmaxnoofaccounts,
    domainmaxnoofaliases, domainmaxnoofdistributionlists, domainlimitationsenabled, domainmaxaccountsize,
    domaindkimselector, domaindkimprivatekeyfile, domaindkimsecondaryselector,
    domaindkimsecondaryprivatekeyfile, domainrelayhost, domainrelayport, domainrelayrequiresauth,
    domainrelayusername, domainrelaypassword, domainrelayconnectionsecurity)
  values ('example.test', 1, '', 0, '', 0, 0, '', 0, 0, 0, '', '', 0, 0, 0, 0, 0, 0, 0, '', '', '', '', '', 0, 0, '', '', 0);
  select domainid, domainname from hm_domains"
echo "$HMTEST_ADMIN_PASSWORD" | "${server[@]}" --set-admin-password
set +e

echo "== start"
# The cd is a command of its own: "cd && nohup ... &" would put the whole list in the
# background, and $! would be that subshell's and not the server's.
( cd "$T" || exit 1; nohup "${server[@]}" --foreground > "$T/stdout.log" 2>&1 & echo $! > "$T/server.pid" )
up=""
for _ in $(seq 1 60); do
  if curl -s -o /dev/null -u "Administrator:$HMTEST_ADMIN_PASSWORD" "http://127.0.0.1:$HMTEST_REST_PORT/api/v1/status"; then
    up=yes
    break
  fi
  sleep 1
done
if [ -z "$up" ]; then
  echo "REFUSED: the server did not answer on the REST port in a minute. What it said:"
  tail -n 40 "$T/stdout.log"
  ss -ltnp || true
  stop_server
  exit 2
fi
curl -s -u "Administrator:$HMTEST_ADMIN_PASSWORD" "http://127.0.0.1:$HMTEST_REST_PORT/api/v1/status"; echo
ss -ltn | grep -E ':(2525|587|1110|1143|8045)\b' || true

echo "== restore"
dotnet --list-sdks
if ! dotnet restore hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj --locked-mode > "$OUT/restore.log" 2>&1; then
  tail -n 30 "$OUT/restore.log"
  echo "RESTORE FAILED"
  stop_server
  exit 2
fi

if [ ${#filter[@]} -gt 0 ]; then
  echo "== LinuxRegressionTests, only the tests the filter selects, started $(date -u +%FT%TZ): $HM_LINUX_SUITE_FILTER"
else
  echo "== the whole of LinuxRegressionTests, started $(date -u +%FT%TZ)"
fi
started=$(date +%s)
dotnet test hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj -c Release --no-restore \
  "${filter[@]}" --logger "trx;LogFileName=linux-suite.trx" --logger "console;verbosity=normal" \
  --results-directory "$OUT" 2>&1 | tee "$OUT/console.log"
code=${PIPESTATUS[0]}
echo "dotnet test exit $code after $(( ($(date +%s) - started) / 60 )) min"

stop_server

summary=0
if [ -f "$OUT/linux-suite.trx" ]; then
  python3 build/ci/trx-to-junit.py "$OUT/linux-suite.trx" "$OUT/junit.xml"
  summary=$?
else
  echo "no TRX was written - the run did not finish"
  summary=2
fi

if [ "$code" -eq 0 ] && [ "$summary" -eq 0 ]; then
  echo "LINUX SUITE PASSED"
  exit 0
fi
echo "LINUX SUITE FAILED"
exit 1
