#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The Linux server on a database of the test lab's matrix - MySQL 8.4, MariaDB 11.4 or
# PostgreSQL 17 on linix (build/lab/docker-compose.yml) - laid out as the linux-suite
# job lays it out (build/ci/linux-suite.sh: the listeners off the privileged ports,
# auto-ban off, the test domain, the administrator password), then the Linux
# regression suite against it. Run as root from the root of a tree, in the CI image
# (build/ci/linux.Dockerfile) or the lab's bench image made from it:
#
#   bash build/lab/db-suite.sh run      lay out, start, run the suite, stop; exit 0
#                                       only for "Failed 0" with a test passed
#   bash build/lab/db-suite.sh serve    lay out, start, and keep the server running
#                                       until the container is stopped (lab.sh serve)
#   bash build/lab/db-suite.sh create   an empty hMailServer database, by the
#                                       server's own --create-database, and nothing else
#
# Two callers: build/lab/lab.sh on linix, inside the lab's network, where a database
# is its service name and its own port; and the manual db-matrix job of
# .gitlab-ci.yml, whose container reaches the lab on linix's address and the ports
# the lab publishes - the defaults below.
#
# What it reads:
#   HM_LAB_DB            mysql, mariadb or postgresql (the Linux server has no SQL
#                        Server backend: HM6390)
#   HM_LAB_DB_HOST       192.168.11.154 by default
#   HM_LAB_DB_PORT       10310 for mysql, 10311 for mariadb, 10312 for postgresql
#   HM_LAB_DB_USER       hmlab, the application user lab.sh up makes
#   HM_LAB_DB_PASSWORD   its password (HMLAB_DB_PASSWORD in ~/hmlab/secrets.env on
#                        linix; the protected CI variable of the same value)
#   HM_LAB_DB_NAME       hmlab_ci by default; DROPPED and made again at every start
#   HM_SUITE_FILTER      db (the default): the database-sensitive fixtures below;
#                        all: the whole suite; anything else: a dotnet test --filter
#   HM_SERVER_BINARY     linux-build/hmailserver (the linux-build job's artifact)
#   HM_RESULTS_DIR       db-matrix/ under the tree
#   HM_RESULTS_OWNER     uid:gid to give the results to at the end (the lab's caller)
set -uo pipefail

MODE="${1:-run}"
KIND="${HM_LAB_DB:-}"
case "$KIND" in
  mysql) DEFAULT_PORT=10310; TYPE=MySQL ;;
  mariadb) DEFAULT_PORT=10311; TYPE=MySQL ;;
  postgresql) DEFAULT_PORT=10312; TYPE=PostgreSQL ;;
  mssql) echo "REFUSED: the Linux server cannot use SQL Server (HM6390: it is reached through ADO, which exists only on Windows)."; exit 2 ;;
  *) echo "REFUSED: HM_LAB_DB is '$KIND'; it must be mysql, mariadb or postgresql."; exit 2 ;;
esac
DBHOST="${HM_LAB_DB_HOST:-192.168.11.154}"
DBPORT="${HM_LAB_DB_PORT:-$DEFAULT_PORT}"
DBUSER="${HM_LAB_DB_USER:-hmlab}"
DBPASS="${HM_LAB_DB_PASSWORD:-}"
DB="${HM_LAB_DB_NAME:-hmlab_ci}"
FILTER="${HM_SUITE_FILTER:-db}"
T="${HM_TREE:-$PWD/.hmtest-tree}"
OUT="${HM_RESULTS_DIR:-$PWD/db-matrix}"
BINARY="${HM_SERVER_BINARY:-$PWD/linux-build/hmailserver}"
MIN_FREE_MB="${HM_MIN_FREE_MB:-4096}"

# The database-sensitive fixtures: those whose SQL differs by dialect or leans on the
# database's own behaviour - name validation and uniqueness (collation and case),
# search, sort and threading over stored headers, full-text search's index tables,
# UTF-8 in every column, the domain cascades, identifiers and UIDs, the folder tree
# and its ACLs, the retention and tombstone sweeps (date arithmetic), greylisting and
# auto-ban (timestamps and expiry), IP ranges (addresses stored as numbers), backup and
# restore, the SQL layer's own error handling, the audit trail and password history.
# 315 tests on 25 September 2026; the whole suite is HM_SUITE_FILTER=all.
DB_FIXTURES="RegressionTests.Infrastructure.Persistence.
RegressionTests.Infrastructure.SqlLayerErrorHandling.
RegressionTests.Infrastructure.DatabaseFailureHandling.
RegressionTests.Infrastructure.DatabaseMetrics.
RegressionTests.Infrastructure.BackupVerifiedRestore.
RegressionTests.Infrastructure.ScheduledBackup.
RegressionTests.Infrastructure.MessageRetentionTests.
RegressionTests.Infrastructure.ArchiveRetentionTests.
RegressionTests.Infrastructure.IPRanges.
RegressionTests.Infrastructure.AccountProperties.
RegressionTests.IMAP.Search.
RegressionTests.IMAP.Sort.
RegressionTests.IMAP.ThreadCommand.
RegressionTests.IMAP.FullTextSearch.
RegressionTests.IMAP.AttachmentTextSearch.
RegressionTests.IMAP.SearchLimits.
RegressionTests.IMAP.Utf8Append.
RegressionTests.IMAP.TombstoneRetention.
RegressionTests.IMAP.MessageUids.
RegressionTests.IMAP.Folders.
RegressionTests.IMAP.ACL.
RegressionTests.IMAP.SharedMailboxes.
RegressionTests.IMAP.Metadata.
RegressionTests.API.Unicode.
RegressionTests.API.RestApiDomains.
RegressionTests.API.RestApiAuditRevert.
RegressionTests.Domains.
RegressionTests.Security.PasswordHashUpgrade.
RegressionTests.Security.PasswordPolicyTests.
RegressionTests.Security.AutoBan.
RegressionTests.AntiSpam.GreyListing.
RegressionTests.AntiSpam.GreyListingHardened.
RegressionTests.ExternalAccounts.ImapFetch."

export HMTEST_HOST=127.0.0.1
export HMTEST_SMTP_PORT=2525
export HMTEST_POP3_PORT=1110
export HMTEST_IMAP_PORT=1143
export HMTEST_SUBMISSION_PORT=587
export HMTEST_REST_PORT=8045
export HMTEST_ADMIN_PASSWORD=testar
export HMTEST_SERVER_INI="$T/program/hMailServer.ini"

server=("$T/program/hmailserver" --config "$T/program/hMailServer.ini")

hand_back() {
  if [ -n "${HM_RESULTS_OWNER:-}" ] && [ -d "$OUT" ]; then
    chown -R "$HM_RESULTS_OWNER" "$OUT" 2> /dev/null || true
  fi
}

stop_server() {
  if [ -f "$T/server.pid" ]; then
    local pid
    pid=$(cat "$T/server.pid")
    if kill -0 "$pid" 2> /dev/null; then
      kill "$pid" 2> /dev/null || true
      for _ in $(seq 1 30); do
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
  [ -f "$T/stdout.log" ] && cp "$T/stdout.log" "$OUT/server-stdout.log"
  if [ -d "$T/Logs" ]; then
    find "$T/Logs" -maxdepth 1 -type f -name 'ERROR_*' -exec cp {} "$OUT/" \;
    tar -C "$T" -czf "$OUT/server-logs.tar.gz" Logs 2> /dev/null || true
  fi
  hand_back
}

# What a run needs beyond the CI image: for MySQL and MariaDB, the client library the
# server loads with dlopen (libmariadb.so.3 first, MySQLInterface.cpp) and the mariadb
# command-line client for the layout below; for every database, 7-Zip, which the
# server's backup runs as the program 7z (ScheduledBackup and BackupVerifiedRestore
# fail without it; the CI image has none, the lab VM's host has Ubuntu's 7zip). The
# lab's bench image has them all; the CI job's container installs them here, as root,
# from Ubuntu's archive.
ensure_clients() {
  local wanted=""
  command -v 7z > /dev/null || wanted="$wanted 7zip"
  case "$KIND" in
    mysql|mariadb)
      ldconfig -p | grep -q 'libmariadb\.so\.3' || wanted="$wanted libmariadb3"
      command -v mariadb > /dev/null || wanted="$wanted mariadb-client"
      ;;
    postgresql)
      command -v psql > /dev/null || { echo "REFUSED: no psql in this image."; exit 2; }
      ;;
  esac
  if [ -n "$wanted" ]; then
    echo "== installing$wanted from Ubuntu's archive"
    export DEBIAN_FRONTEND=noninteractive
    # $wanted is the package names chosen above, one word each, split on purpose.
    # shellcheck disable=SC2086
    # nosemgrep
    apt-get update -qq > /dev/null && apt-get install -y -qq --no-install-recommends $wanted > /dev/null \
      || { echo "REFUSED:$wanted could not be installed."; exit 2; }
  fi
}

# SQL on standard input, against the database named in $1 (empty: none, for the
# server-level statements). The password is in the environment, never an argument.
sql() {
  local database="$1"
  case "$KIND" in
    postgresql)
      PGPASSWORD="$DBPASS" psql -h "$DBHOST" -p "$DBPORT" -U "$DBUSER" -d "${database:-postgres}" -v ON_ERROR_STOP=1 -At -q
      ;;
    *)
      # --skip-ssl-verify-server-cert: the lab's database servers have self-made
      # certificates; this is the layout's client, not the server's connection.
      # ${database:+"$database"}: the database's name as one word, or no word at all.
      MYSQL_PWD="$DBPASS" mariadb -h "$DBHOST" -P "$DBPORT" -u "$DBUSER" --skip-ssl-verify-server-cert -N -B ${database:+"$database"} # nosemgrep
      ;;
  esac
}

case "$MODE" in
  run|serve|create) ;;
  *) echo "usage: db-suite.sh run|serve|create"; exit 2 ;;
esac
if [ -z "$DBPASS" ]; then
  echo "REFUSED: HM_LAB_DB_PASSWORD is empty. On linix it is HMLAB_DB_PASSWORD in ~/hmlab/secrets.env; in GitLab, the protected variable HM_LAB_DB_PASSWORD (docs/TestLab.md)."
  exit 2
fi
if [ ! -f "$BINARY" ]; then
  echo "REFUSED: no server binary at $BINARY."
  exit 2
fi
if [ "$MODE" = run ] && [ ! -f hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj ]; then
  echo "REFUSED: run this from the root of a tree (no hmailserver/test/LinuxRegressionTests here)."
  exit 2
fi

# The results directory may be a mount point (the lab's), so it is emptied, not removed.
rm -rf "$T"
mkdir -p "$T/program" "$T/Data" "$T/Logs" "$T/Temp" "$T/Events" "$T/Database" "$OUT"
find "$OUT" -mindepth 1 -delete
free_mb=$(df -Pm "$T" | awk 'NR==2 {print $4}')
if [ "${free_mb:-0}" -lt "$MIN_FREE_MB" ]; then
  echo "REFUSED: $free_mb MB free where the tree is, and a run needs $MIN_FREE_MB MB."
  exit 2
fi

ensure_clients

echo "== $KIND at $DBHOST:$DBPORT as $DBUSER, database $DB"
reached=""
for _ in $(seq 1 30); do
  if version=$(echo "select version();" | sql "" 2> "$OUT/connect.err"); then
    reached=yes
    break
  fi
  sleep 2
done
if [ -z "$reached" ]; then
  cat "$OUT/connect.err"
  echo "REFUSED: $KIND at $DBHOST:$DBPORT did not answer in a minute. Is it up? On linix: bash build/lab/lab.sh up $KIND"
  hand_back
  exit 2
fi
echo "server version: $version"
case "$KIND" in
  postgresql) echo "drop database if exists \"$DB\" with (force);" | sql "" ;;
  *) echo "drop database if exists \`$DB\`;" | sql "" ;;
esac || { echo "REFUSED: $DB could not be dropped."; hand_back; exit 2; }

echo "== the server tree"
cp "$BINARY" "$T/program/hmailserver"
chmod +x "$T/program/hmailserver"
cp -r hmailserver/source/DBScripts "$T/program/DBScripts"
cp hmailserver/installation/Extras/dh2048.pem "$T/program/"
sha256sum "$T/program/hmailserver"

# The password goes into the ini, which is the server's; the tree is the container's own.
umask 077
cat > "$T/program/hMailServer.ini" <<INI
[Directories]
ProgramFolder=$T/program
DatabaseFolder=$T/Database
DataFolder=$T/Data
LogFolder=$T/Logs
TempFolder=$T/Temp
EventFolder=$T/Events
[Database]
Type=$TYPE
Server=$DBHOST
Port=$DBPORT
Username=$DBUSER
Password=$DBPASS
Database=$DB
Internal=0
[Settings]
ProtectStoredSecretsWithDPAPI=0
TlsKeyExchangeGroups=X25519:secp384r1:secp256r1
RestApiPort=$HMTEST_REST_PORT
RestApiBindAddress=127.0.0.1
WindowsEventLogEnabled=0
INI
umask 022

"${server[@]}" --version || { echo "REFUSED: the server binary does not run here."; hand_back; exit 2; }
echo "== --create-database"
"${server[@]}" --create-database > "$OUT/create-database.log" 2>&1
created=$?
cat "$OUT/create-database.log"
if [ "$created" -ne 0 ]; then
  echo "CREATE FAILED (exit $created)"
  hand_back
  exit 3
fi
schema=$(echo "select value from hm_dbversion;" | sql "$DB")
case "$KIND" in
  postgresql) tables=$(echo "select count(*) from information_schema.tables where table_schema = 'public' and table_type = 'BASE TABLE';" | sql "$DB") ;;
  *) tables=$(echo "select count(*) from information_schema.tables where table_schema = '$DB' and table_type = 'BASE TABLE';" | sql "$DB") ;;
esac
echo "schema $schema, $tables tables" | tee "$OUT/schema.txt"

if [ "$MODE" = create ]; then
  echo "$DB on $KIND ($version) at $DBHOST:$DBPORT: an empty hMailServer database, schema $schema, $tables tables."
  hand_back
  exit 0
fi

echo "== the layout"
# The listeners are rows. SMTP, POP3 and IMAP move to the ports the suite expects;
# submission keeps 587, which root can bind in a container. The test domain is made
# after the start, through the REST API, and not by an INSERT as linux-suite.sh makes
# it: that INSERT names the columns PostgreSQL has no default for, and MySQL has more
# (a TEXT column cannot have a default there), so one statement cannot serve both.
sql "$DB" <<'SQL' || { echo "LAYOUT FAILED"; hand_back; exit 3; }
update hm_tcpipports set portnumber = 2525 where portnumber = 25;
update hm_tcpipports set portnumber = 1110 where portnumber = 110;
update hm_tcpipports set portnumber = 1143 where portnumber = 143;
update hm_settings set settinginteger = 0 where settingname = 'AutoBanOnLogonFailureEnabled';
select portprotocol, portnumber from hm_tcpipports order by 1, 2;
SQL
echo "$HMTEST_ADMIN_PASSWORD" | "${server[@]}" --set-admin-password

echo "== start"
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
  stop_server
  collect
  exit 2
fi
status=$(curl -s -u "Administrator:$HMTEST_ADMIN_PASSWORD" "http://127.0.0.1:$HMTEST_REST_PORT/api/v1/status")
echo "$status"
made=$(printf '{"name":"example.test","active":true}' | curl -s -o "$OUT/domain.json" -w '%{http_code}' \
  -u "Administrator:$HMTEST_ADMIN_PASSWORD" -H 'Content-Type: application/json' --data-binary @- \
  "http://127.0.0.1:$HMTEST_REST_PORT/api/v1/domains")
echo "the test domain example.test: HTTP $made $(head -c 60 "$OUT/domain.json")..."
case "$made" in
  200|201) ;;
  *) echo "REFUSED: the test domain could not be made."; stop_server; collect; exit 3 ;;
esac

if [ "$MODE" = serve ]; then
  trap 'stop_server; collect; exit 0' TERM INT
  {
    echo "hMailServer $(echo "$status" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p') on $KIND ($version), database $DB, schema $schema"
    echo "  in container $(hostname) on network hmlab as 'hmailserver': SMTP 2525, POP3 1110, IMAP 1143, submission 587, REST 127.0.0.1:8045"
    echo "  from linix and the LAN: SMTP 10350, POP3 10351, IMAP 10352, submission 10353"
  } > "$OUT/serving"
  hand_back
  while kill -0 "$(cat "$T/server.pid")" 2> /dev/null; do
    sleep 5 &
    wait $!
  done
  echo "the server stopped by itself"
  collect
  exit 1
fi

echo "== restore"
if ! dotnet restore hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj --locked-mode > "$OUT/restore.log" 2>&1; then
  tail -n 30 "$OUT/restore.log"
  echo "RESTORE FAILED"
  stop_server
  collect
  exit 2
fi

filter_args=()
case "$FILTER" in
  all) described="the whole suite" ;;
  db)
    described="the database-sensitive fixtures"
    expression=""
    for prefix in $DB_FIXTURES; do
      expression="${expression:+$expression|}FullyQualifiedName~$prefix"
    done
    filter_args=(--filter "$expression")
    ;;
  *) described="--filter $FILTER"; filter_args=(--filter "$FILTER") ;;
esac

echo "== $described on $KIND, started $(date -u +%FT%TZ)"
started=$(date +%s)
dotnet test hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj -c Release --no-restore \
  "${filter_args[@]}" --logger "trx;LogFileName=db-matrix.trx" --logger "console;verbosity=normal" \
  --results-directory "$OUT" > "$OUT/console.log" 2>&1
code=$?
ended=$(date +%s)
grep -E '^\s+Failed |^(Passed|Failed)!|error CS' "$OUT/console.log" | head -n 80
echo "dotnet test exit $code after $(( (ended - started) / 60 )) min"

stop_server

summary=0
if [ -f "$OUT/db-matrix.trx" ]; then
  python3 build/ci/trx-to-junit.py "$OUT/db-matrix.trx" "$OUT/junit.xml"
  summary=$?
else
  echo "no TRX was written - the run did not finish"
  summary=2
fi
collect

echo "$KIND ($version), schema $schema, $described: dotnet test exit $code" > "$OUT/summary.txt"
if [ "$code" -eq 0 ] && [ "$summary" -eq 0 ]; then
  echo "DB MATRIX PASSED on $KIND"
  exit 0
fi
echo "DB MATRIX FAILED on $KIND"
exit 1
