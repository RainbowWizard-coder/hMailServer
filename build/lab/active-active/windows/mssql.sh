#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The active-active Windows lab's side on server2 (10.0.0.19): the SQL Server 2022
# (Developer edition, the free edition for development and testing) the two Windows
# nodes share, in the long-lived container hmlab-aa-mssql, and the sink a baseline
# run relays mail to. aa-win.py copies this file to ~/hmlab/aa-win/lab/windows/ and
# runs it there over SSH; the lab's README (build/lab/active-active/README.md, "The
# Windows lab") says what it is for.
#
# No secret is stored on server2 and none is an argument. They come on standard
# input, as KEY=VALUE lines ended by a line '%%' (or the end of the input), and
# whatever follows that line is the command's own input (SQL, for 'sql'):
#
#   AA_MSSQL_SA_PASSWORD   sa's password
#   AA_NODE_LOGIN          the login the nodes use (hmlab_aa_node)
#   AA_NODE_PASSWORD       its password
#   AA_DB_NAME             the lab database (hmlab_aa)
#   AA_CHECK_LOGIN         the login the engine checks use (hmlab_aa_check)
#   AA_CHECK_PASSWORD      its password
#
#   up                  create (once) or start the container; on its first start sa
#                       gets a throwaway password, replaced at once by AA_MSSQL_SA_PASSWORD
#   logins              the nodes' login, with dbcreator while the lab database does
#                       not exist (so that the server's own CreateExternalDatabase can
#                       make it), and the checks' login with its database; a login
#                       whose password is already the one given is left as it is
#   finish              once the lab database exists: the nodes' login loses dbcreator
#                       and keeps only the database it owns; a golden backup is taken
#   reset [--recreate]  restore the golden backup (the lab database as provisioning left
#                       it); --recreate drops it and gives dbcreator back instead, for
#                       the server to create it again
#   sql                 run the SQL that follows the secrets as sa
#   status              the container, the server's version and clock, the lab
#                       database, and which hosts hold sessions of the nodes' login
#   sink-start <run>    the sink (../sink.py) on <address>:14325, logging to
#                       ~/hmlab/aa-win/runs/<run>/sink/sink.jsonl
#   sink-stop           remove the sink
#   sink-log <run>      print that run's sink log
#   sqlcheck <file>     run ../sqlcheck.py's checks through windows/<file> (default
#                       sqlcheck-mssql.py) in a container, as the checks' login
#   stop | start        stop or start the container (it restarts by itself otherwise)
#
# The container is never removed by this script. To remove the lab for good:
# docker rm -f hmlab-aa-mssql && docker volume rm hmlab-aa-mssql-data.
set -uo pipefail

LAB_DIR="$(cd "$(dirname "$0")" && pwd)"
AA_WIN_HOME="${AA_WIN_HOME:-$HOME/hmlab/aa-win}"
CONTAINER=hmlab-aa-mssql
VOLUME=hmlab-aa-mssql-data
SINK=hmlab-aa-sink
CHECK=hmlab-aa-sqlcheck
# build/lab/docker-compose.yml pins the same image: SQL Server 2022 CU27 (16.0.4295.3).
IMAGE=mcr.microsoft.com/mssql/server:2022-latest@sha256:4402d880dd4c34bfa7d8705e56a86cd6c88da80a1f6bbbe741f999e76264a090
PYTHON_IMAGE=python:3.12-slim@sha256:46cb7cc2877e60fbd5e21a9ae6115c30ace7a077b9f8772da879e4590c18c2e3
PYTHON_TDS_VERSION=1.16.0
ADDRESS="${AA_WIN_ADDRESS:-10.0.0.19}"
MSSQL_PORT="${AA_WIN_MSSQL_PORT:-14330}"
SINK_PORT="${AA_WIN_SINK_PORT:-14325}"
SQLCMD=/opt/mssql-tools18/bin/sqlcmd

say() { printf '%s\n' "$*"; }
die() { printf 'mssql.sh: %s\n' "$*" >&2; exit 1; }

read_secrets() {
  local line
  while IFS= read -r line; do
    line="${line%$'\r'}"
    [ "$line" = "%%" ] && break
    case "$line" in
      AA_MSSQL_SA_PASSWORD=*|AA_NODE_LOGIN=*|AA_NODE_PASSWORD=*|AA_DB_NAME=*|AA_CHECK_LOGIN=*|AA_CHECK_PASSWORD=*)
        export "${line%%=*}=${line#*=}" ;;
      '') ;;
      *) die "standard input: expected KEY=VALUE lines and '%%'" ;;
    esac
  done
}

need() {
  local key
  for key in "$@"; do
    [ -n "${!key:-}" ] || die "$key was not given on standard input"
  done
}

# A value inside '...' in T-SQL: quotes doubled.
q() { printf '%s' "${1//\'/\'\'}"; }

running() { docker ps -q --filter "name=^$CONTAINER\$" | grep -q .; }

# sa's SQL on standard input. The password reaches the container's sqlcmd through
# the environment, by name: never an argument, which the process list would show.
sa_sql() {
  SQLCMDPASSWORD="$AA_MSSQL_SA_PASSWORD" docker exec -i -e SQLCMDPASSWORD "$CONTAINER" \
    "$SQLCMD" -C -S localhost -U sa -b -h -1 -W "$@"
}

sa_works() {
  local password="$1"
  SQLCMDPASSWORD="$password" docker exec -e SQLCMDPASSWORD "$CONTAINER" \
    "$SQLCMD" -C -S localhost -U sa -b -Q 'set nocount on; select 1' -o /dev/null > /dev/null 2>&1
}

generate() {
  python3 -c 'import secrets, string; a = string.ascii_letters + string.digits; print("Bt-" + "".join(secrets.choice(a) for _ in range(26)) + "-7q")'
}

cmd_up() {
  read_secrets
  need AA_MSSQL_SA_PASSWORD
  mkdir -p "$AA_WIN_HOME/runs" && chmod 700 "$AA_WIN_HOME"
  local boot=""
  if running; then
    say "$CONTAINER: running"
  elif docker ps -aq --filter "name=^$CONTAINER\$" | grep -q .; then
    docker start "$CONTAINER" > /dev/null || die "starting $CONTAINER failed"
    say "$CONTAINER: started"
  else
    # The first start sets sa's password from MSSQL_SA_PASSWORD, which docker inspect
    # shows for the life of the container: so it is a throwaway, replaced below. A
    # later start of the same data volume does not read it again.
    boot=$(generate)
    MSSQL_SA_PASSWORD="$boot" docker run -d --name "$CONTAINER" --hostname "$CONTAINER" \
      --restart unless-stopped --memory 4g --cpus 4 \
      -e ACCEPT_EULA=Y -e MSSQL_PID=Developer -e MSSQL_SA_PASSWORD -e MSSQL_MEMORY_LIMIT_MB=3072 \
      -e TZ=Europe/London \
      -p "$ADDRESS:$MSSQL_PORT:1433" -v "$VOLUME:/var/opt/mssql" "$IMAGE" > /dev/null \
      || die "creating $CONTAINER failed"
    say "$CONTAINER: created from $IMAGE"
  fi
  local ok=""
  for _ in $(seq 1 90); do
    if sa_works "$AA_MSSQL_SA_PASSWORD"; then ok=real; break; fi
    if [ -n "$boot" ] && sa_works "$boot"; then ok=boot; break; fi
    sleep 2
  done
  [ -n "$ok" ] || die "$CONTAINER does not take sa's password (docker logs $CONTAINER)"
  if [ "$ok" = boot ]; then
    printf "ALTER LOGIN sa WITH PASSWORD = '%s';\nGO\n" "$(q "$AA_MSSQL_SA_PASSWORD")" \
      | SQLCMDPASSWORD="$boot" docker exec -i -e SQLCMDPASSWORD "$CONTAINER" "$SQLCMD" -C -S localhost -U sa -b > /dev/null \
      || die "replacing sa's first password failed"
    sa_works "$AA_MSSQL_SA_PASSWORD" || die "sa's password did not take"
    sa_works "$boot" && die "sa still takes the throwaway password"
    say "sa: the throwaway first password replaced"
  fi
  printf 'set nocount on;\nselect concat(@@version, char(10), %s);\nGO\n' \
    "'clock: ', convert(varchar(33), sysdatetimeoffset(), 126), '  getdate(): ', convert(varchar(23), getdate(), 126), '  utc: ', convert(varchar(23), getutcdate(), 126), '  collation: ', convert(varchar(64), serverproperty('Collation'))" \
    | sa_sql | sed -n '1p;$p'
  say "listening on $ADDRESS:$MSSQL_PORT"
}

cmd_logins() {
  read_secrets
  need AA_MSSQL_SA_PASSWORD AA_NODE_LOGIN AA_NODE_PASSWORD AA_DB_NAME AA_CHECK_LOGIN AA_CHECK_PASSWORD
  running || die "$CONTAINER is not running - run: up"
  sa_sql > /dev/null <<SQL || die "making the logins failed"
set nocount on;
-- A login is made once, and its password set again only when it is not the one given
-- (PWDCOMPARE): run again, this changes nothing.
IF NOT EXISTS (SELECT * FROM sys.server_principals WHERE name = N'$(q "$AA_NODE_LOGIN")')
  CREATE LOGIN [$AA_NODE_LOGIN] WITH PASSWORD = N'$(q "$AA_NODE_PASSWORD")', CHECK_POLICY = ON, CHECK_EXPIRATION = OFF;
ELSE IF NOT EXISTS (SELECT * FROM sys.sql_logins WHERE name = N'$(q "$AA_NODE_LOGIN")' AND PWDCOMPARE(N'$(q "$AA_NODE_PASSWORD")', password_hash) = 1)
  ALTER LOGIN [$AA_NODE_LOGIN] WITH PASSWORD = N'$(q "$AA_NODE_PASSWORD")';
-- The server's CreateExternalDatabase runs CREATE DATABASE as the login it is given,
-- which then owns the database; it needs dbcreator for that and for nothing after.
IF DB_ID(N'$(q "$AA_DB_NAME")') IS NULL
  ALTER SERVER ROLE dbcreator ADD MEMBER [$AA_NODE_LOGIN];
IF NOT EXISTS (SELECT * FROM sys.server_principals WHERE name = N'$(q "$AA_CHECK_LOGIN")')
  CREATE LOGIN [$AA_CHECK_LOGIN] WITH PASSWORD = N'$(q "$AA_CHECK_PASSWORD")', CHECK_POLICY = ON, CHECK_EXPIRATION = OFF;
ELSE IF NOT EXISTS (SELECT * FROM sys.sql_logins WHERE name = N'$(q "$AA_CHECK_LOGIN")' AND PWDCOMPARE(N'$(q "$AA_CHECK_PASSWORD")', password_hash) = 1)
  ALTER LOGIN [$AA_CHECK_LOGIN] WITH PASSWORD = N'$(q "$AA_CHECK_PASSWORD")';
IF DB_ID(N'hmlab_aa_sqlcheck') IS NULL CREATE DATABASE hmlab_aa_sqlcheck;
GO
IF SUSER_SNAME((SELECT owner_sid FROM sys.databases WHERE name = N'hmlab_aa_sqlcheck')) IS NULL
   OR SUSER_SNAME((SELECT owner_sid FROM sys.databases WHERE name = N'hmlab_aa_sqlcheck')) <> N'$(q "$AA_CHECK_LOGIN")'
  ALTER AUTHORIZATION ON DATABASE::hmlab_aa_sqlcheck TO [$AA_CHECK_LOGIN];
GO
SQL
  say "logins: $AA_NODE_LOGIN ($( [ "$(db_exists)" = 1 ] && echo 'owns its database' || echo 'dbcreator until its database exists')), $AA_CHECK_LOGIN (owns hmlab_aa_sqlcheck)"
}

db_exists() {
  printf "set nocount on; select case when db_id(N'%s') is null then 0 else 1 end;\nGO\n" "$(q "$AA_DB_NAME")" | sa_sql | tr -d '\r '
}

cmd_finish() {
  read_secrets
  need AA_MSSQL_SA_PASSWORD AA_NODE_LOGIN AA_DB_NAME
  running || die "$CONTAINER is not running - run: up"
  [ "$(db_exists)" = 1 ] || die "$AA_DB_NAME does not exist yet: the server makes it (aa-win.py provision)"
  local owner
  owner=$(printf "set nocount on; select suser_sname(owner_sid) from sys.databases where name = N'%s';\nGO\n" "$(q "$AA_DB_NAME")" | sa_sql | tr -d '\r ')
  [ "$owner" = "$AA_NODE_LOGIN" ] || die "$AA_DB_NAME is owned by '$owner', not $AA_NODE_LOGIN"
  sa_sql > /dev/null <<SQL || die "finishing the logins failed"
set nocount on;
IF IS_SRVROLEMEMBER(N'dbcreator', N'$(q "$AA_NODE_LOGIN")') = 1
  ALTER SERVER ROLE dbcreator DROP MEMBER [$AA_NODE_LOGIN];
GO
BACKUP DATABASE [$AA_DB_NAME] TO DISK = N'/var/opt/mssql/data/$AA_DB_NAME-golden.bak' WITH INIT, FORMAT, COPY_ONLY, CHECKSUM;
GO
SQL
  say "$AA_DB_NAME: owned by $AA_NODE_LOGIN, which holds no server role; golden backup /var/opt/mssql/data/$AA_DB_NAME-golden.bak"
}

cmd_reset() {
  read_secrets
  need AA_MSSQL_SA_PASSWORD AA_NODE_LOGIN AA_DB_NAME
  running || die "$CONTAINER is not running - run: up"
  local sessions
  sessions=$(printf "set nocount on; select count(*) from sys.dm_exec_sessions where database_id = db_id(N'%s') and session_id <> @@spid;\nGO\n" "$(q "$AA_DB_NAME")" | sa_sql | tr -d '\r ')
  [ "${sessions:-0}" = 0 ] || die "$sessions sessions are open on $AA_DB_NAME: stop the nodes first (aa-win.py stop all)"
  if [ "${1:-}" = --recreate ]; then
    sa_sql > /dev/null <<SQL || die "dropping $AA_DB_NAME failed"
set nocount on;
IF DB_ID(N'$(q "$AA_DB_NAME")') IS NOT NULL
BEGIN
  ALTER DATABASE [$AA_DB_NAME] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;
  DROP DATABASE [$AA_DB_NAME];
END
-- CreateExternalDatabase's first connection names no database, so the login's
-- default one must exist: a login left defaulting to the dropped database is refused
-- ("Cannot open user default database").
ALTER LOGIN [$AA_NODE_LOGIN] WITH DEFAULT_DATABASE = [master];
ALTER SERVER ROLE dbcreator ADD MEMBER [$AA_NODE_LOGIN];
GO
SQL
    say "$AA_DB_NAME: dropped; $AA_NODE_LOGIN may create it again (dbcreator)"
    return 0
  fi
  sa_sql > /dev/null <<SQL || die "restoring $AA_DB_NAME failed (is there a golden backup? run: finish)"
set nocount on;
IF DB_ID(N'$(q "$AA_DB_NAME")') IS NOT NULL
  ALTER DATABASE [$AA_DB_NAME] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;
GO
RESTORE DATABASE [$AA_DB_NAME] FROM DISK = N'/var/opt/mssql/data/$AA_DB_NAME-golden.bak' WITH REPLACE, CHECKSUM;
GO
ALTER DATABASE [$AA_DB_NAME] SET MULTI_USER;
GO
SQL
  say "$AA_DB_NAME: restored from its golden backup"
}

cmd_sql() {
  read_secrets
  need AA_MSSQL_SA_PASSWORD
  running || die "$CONTAINER is not running - run: up"
  sa_sql -s $'\t'
}

cmd_status() {
  read_secrets
  docker ps -a --filter "name=^($CONTAINER|$SINK)\$" --format '{{.Names}}\t{{.Status}}\t{{.Ports}}'
  running || return 0
  docker stats --no-stream --format '{{.Name}}\t{{.MemUsage}}\t{{.CPUPerc}}' "$CONTAINER" 2> /dev/null
  [ -n "${AA_MSSQL_SA_PASSWORD:-}" ] || return 0
  sa_sql -s $'\t' <<SQL
set nocount on;
select 'version', cast(serverproperty('ProductVersion') as varchar(32)), cast(serverproperty('ProductUpdateLevel') as varchar(16)), cast(serverproperty('Edition') as varchar(64));
select 'clock', convert(varchar(33), sysdatetimeoffset(), 126), convert(varchar(23), getutcdate(), 126), '';
select 'database', name, suser_sname(owner_sid), concat(collation_name, ' rcsi=', is_read_committed_snapshot_on) from sys.databases where name in (N'$(q "${AA_DB_NAME:-hmlab_aa}")', N'hmlab_aa_sqlcheck');
select 'sessions', host_name, program_name, count(*) from sys.dm_exec_sessions where login_name = N'$(q "${AA_NODE_LOGIN:-hmlab_aa_node}")' group by host_name, program_name;
GO
SQL
}

python_lib() {
  local lib="$AA_WIN_HOME/pylib"
  if [ ! -f "$lib/.python-tds-$PYTHON_TDS_VERSION" ]; then
    rm -rf "$lib" && mkdir -p "$lib"
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$lib:/pylib" "$PYTHON_IMAGE" \
      pip install -q --no-cache-dir --target /pylib "python-tds==$PYTHON_TDS_VERSION" > /dev/null \
      || die "installing python-tds $PYTHON_TDS_VERSION failed"
    touch "$lib/.python-tds-$PYTHON_TDS_VERSION"
  fi
  printf '%s' "$lib"
}

cmd_sqlcheck() {
  read_secrets
  need AA_CHECK_LOGIN AA_CHECK_PASSWORD
  local file="${1:-sqlcheck-mssql.py}" lib results
  shift || true
  [ -f "$LAB_DIR/$file" ] || die "no $LAB_DIR/$file"
  lib=$(python_lib)
  mkdir -p "$AA_WIN_HOME/runs"
  results="$AA_WIN_HOME/runs/sqlcheck-$(date -u +%Y%m%dT%H%M%SZ).jsonl"
  AA_DB_PASSWORD="$AA_CHECK_PASSWORD" docker run --rm --name "$CHECK" -e AA_DB_PASSWORD \
    -e AA_DB_HOST="$ADDRESS" -e AA_DB_PORT="$MSSQL_PORT" -e AA_DB_USER="$AA_CHECK_LOGIN" -e AA_DB_NAME=hmlab_aa_sqlcheck \
    -e PYTHONPATH=/pylib -v "$lib:/pylib:ro" -v "$LAB_DIR/..:/aa:ro" "$PYTHON_IMAGE" \
    python3 "/aa/windows/$file" "$@" | tee "$results"
  local code=${PIPESTATUS[0]}
  say "results: $results"
  return "$code"
}

cmd_sink_start() {
  local run="${1:-}"
  [ -n "$run" ] || die "usage: sink-start <run>"
  local dir="$AA_WIN_HOME/runs/$run/sink"
  mkdir -p "$dir" && chmod 777 "$dir"
  docker rm -f "$SINK" > /dev/null 2>&1 || true
  docker run -d --init --name "$SINK" --memory 256m -p "$ADDRESS:$SINK_PORT:2525" \
    -e AA_SINK_DATA_DELAY_MS="${AA_SINK_DATA_DELAY_MS:-0}" -e AA_SINK_TEMPFAIL_EVERY="${AA_SINK_TEMPFAIL_EVERY:-0}" \
    -v "$LAB_DIR/../sink.py:/aa/sink.py:ro" -v "$dir:/data" "$PYTHON_IMAGE" python3 /aa/sink.py > /dev/null \
    || die "starting the sink failed"
  for _ in $(seq 1 30); do
    docker logs "$SINK" 2>&1 | grep -q 'sink listening' && { say "sink: $ADDRESS:$SINK_PORT, log $dir/sink.jsonl"; return 0; }
    sleep 1
  done
  die "the sink did not start: docker logs $SINK"
}

cmd_sink_stop() {
  docker rm -f "$SINK" > /dev/null 2>&1 || true
  say "sink: removed"
}

cmd_sink_log() {
  local run="${1:-}"
  [ -n "$run" ] || die "usage: sink-log <run>"
  cat "$AA_WIN_HOME/runs/$run/sink/sink.jsonl" 2> /dev/null || true
}

command="${1:-}"
shift || true
case "$command" in
  up) cmd_up "$@" ;;
  logins) cmd_logins "$@" ;;
  finish) cmd_finish "$@" ;;
  reset) cmd_reset "$@" ;;
  sql) cmd_sql "$@" ;;
  status) cmd_status "$@" ;;
  sink-start) cmd_sink_start "$@" ;;
  sink-stop) cmd_sink_stop "$@" ;;
  sink-log) cmd_sink_log "$@" ;;
  sqlcheck) cmd_sqlcheck "$@" ;;
  stop) docker stop -t 60 "$CONTAINER" > /dev/null && say "$CONTAINER: stopped" ;;
  start) docker start "$CONTAINER" > /dev/null && say "$CONTAINER: started" ;;
  *) sed -n '6,47p' "$0"; exit 2 ;;
esac
