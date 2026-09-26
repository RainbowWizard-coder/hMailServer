#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The test lab on linix: one script for everything build/lab/docker-compose.yml runs
# and everything run against it. Run it ON linix, as a member of the docker group,
# from a copy of the tree (build/lab/push-source.sh puts one in ~/hmlab/src/<commit>):
#
#   bash build/lab/lab.sh <command> [arguments]
#
#   init                     the lab's home (~/hmlab, mode 700), the credentials file
#                            (~/hmlab/secrets.env, mode 600, generated once and never
#                            printed) and the lab CA with Keycloak's certificate
#   up <profile>...          start services: core, mysql, mariadb, postgresql, mssql,
#                            idp, peers, numbers. A database also gets its application user
#                            (hmlab); idp gets its test users' passwords
#   down <profile>...        stop and remove those services' containers; their volumes
#                            stay (the core is never taken down by this)
#   status                   what runs, and the memory each container uses now
#   kuma-setup               Uptime Kuma's administrator and monitors (kuma/setup.js)
#   build                    the Linux server of this tree, once, with clang, in the CI
#                            image (build/ci/linux-build.sh); into ~/hmlab/build/<commit>
#   createdb <db> [name]     an empty hMailServer database, hmlab_<name> (default
#                            hmlab_empty), on mysql, mariadb, postgresql or mssql
#   suite <db> [filter]      the Linux regression suite against that database: filter
#                            "db" (the default: the database-sensitive fixtures), "all"
#                            (the whole suite) or a dotnet test --filter expression
#   serve <db>               lay the server out on that database and leave it running
#                            as container hmlab-bench (SMTP 10350, POP3 10351, IMAP
#                            10352, submission 10353), for the three below
#   unserve                  stop hmlab-bench
#   interop                  Postfix and Dovecot against the served server (profile peers)
#   imaptest [secs]          Dovecot's imaptest compliance scripts against its IMAP
#   idp-check                Keycloak's realm, and single sign-on into the served server
#   numbers <target> <mode> [arguments]
#                            the published numbers (hmailserver/docs/PublishedNumbers.md):
#                            build/perf/load/loadgen.py, run on linix, against the served
#                            server (target hmailserver; serve it with HMLAB_SERVE_STORE set)
#                            or profile numbers' Postfix and Dovecot (target postfix-dovecot);
#                            modes prepare, smtp-in, smtp-out, imap-sessions, imaptest; the
#                            arguments go to loadgen.py; results in ~/hmlab/runs/numbers-*
#
# Nothing here reads or changes the host's own PostgreSQL, MariaDB or Postfix, ~/hmbench,
# the GitLab runner or its images, or Forgejo. Every port is in 10300-10399.
set -uo pipefail

LAB_DIR="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$LAB_DIR/../.." && pwd)"
HMLAB_HOME="${HMLAB_HOME:-$HOME/hmlab}"
SECRETS="$HMLAB_HOME/secrets.env"
LAB_IP="${HMLAB_HOST_IP:-192.168.11.154}"
BENCH=hmlab-bench
NUGET_VOLUME=hmlab-nuget
IMAPTEST_COMMIT=dae2f0d16d6484d7ed601cce0c5d3d9036a41b7a
IMAPTEST_IMAGE=dovecot/imaptest:latest@sha256:720111fb1be0cec892f3aa028031a0a7dbcc8fee3dfa804fe0f3296d3c713bd7
export HMLAB_HOME

say() { printf '%s\n' "$*"; }
die() { printf 'lab.sh: %s\n' "$*" >&2; exit 1; }

# The commit this tree is: the .commit file push-source.sh writes, or git's answer.
commit_of_tree() {
  if [ -f "$TREE/.commit" ]; then
    cut -c1-12 "$TREE/.commit"
  elif git -C "$TREE" rev-parse --short=12 HEAD > /dev/null 2>&1; then
    git -C "$TREE" rev-parse --short=12 HEAD
  else
    die "cannot tell which commit $TREE is (no .commit file, no git)"
  fi
}

# The CI image's tag, made from build/ci/linux.Dockerfile the way
# build/ci/build-linux-image.sh and build/check-ci-image.py make it.
ci_image() {
  local dockerfile="$TREE/build/ci/linux.Dockerfile" release digest
  release=$(sed -n 's/^FROM ubuntu:\([0-9][0-9]*\.[0-9][0-9]*\)@.*$/\1/p' "$dockerfile" | head -n 1)
  digest=$(sed 's/\r$//' "$dockerfile" | sha256sum | cut -c1-12)
  printf 'hmailserver-ci/linux:%s-%s' "$release" "$digest"
}

# The bench image: the CI image plus the MariaDB client library the server dlopens
# for MySQL and MariaDB (MySQLInterface.cpp), the mariadb command-line client for the
# layout, 7-Zip, which the server's backup runs as the program 7z, and isync (mbsync)
# for the interop check. Built here, once per CI image and package list (both are in
# the tag), never pushed. db-suite.sh installs all but isync when they are missing,
# which is how the CI job, on the bare CI image, gets them.
BENCH_PACKAGES="libmariadb3 mariadb-client 7zip isync"
bench_image() {
  local base tag dockerfile
  base=$(ci_image)
  dockerfile=$(printf 'FROM %s\nRUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends %s > /dev/null && rm -rf /var/lib/apt/lists/*\n' "$base" "$BENCH_PACKAGES")
  tag="hmlab/bench:${base##*:}-$(printf '%s' "$dockerfile" | sha256sum | cut -c1-8)"
  if ! docker image inspect "$tag" > /dev/null 2>&1; then
    docker image inspect "$base" > /dev/null 2>&1 \
      || die "the CI image $base is not on this host: build it with bash build/ci/build-linux-image.sh (docs/ContinuousIntegration.md, \"The image\")"
    say "== building $tag from $base" >&2
    printf '%s\n' "$dockerfile" | docker build -q -t "$tag" - >&2 || die "building $tag failed"
  fi
  printf '%s' "$tag"
}

load_secrets() {
  [ -f "$SECRETS" ] || die "no $SECRETS - run: bash build/lab/lab.sh init"
  set -a
  # shellcheck disable=SC1090
  . "$SECRETS"
  set +a
}

compose() {
  docker compose -p hmlab --env-file "$SECRETS" -f "$LAB_DIR/docker-compose.yml" "$@"
}

services_of() {
  case "$1" in
    core) echo kuma ;;
    mysql|mariadb|postgresql|mssql) echo "$1" ;;
    idp) echo keycloak ;;
    peers) echo "postfix dovecot" ;;
    numbers) echo "numbers-postfix numbers-dovecot" ;;
    *) die "no profile '$1' (core, mysql, mariadb, postgresql, mssql, idp, peers, numbers)" ;;
  esac
}

# 24 letters and digits from the kernel's generator.
generate() {
  python3 -c 'import secrets, string; a = string.ascii_letters + string.digits; print("".join(secrets.choice(a) for _ in range(24)))'
}

# ---------------------------------------------------------------- init

cmd_init() {
  umask 077
  mkdir -p "$HMLAB_HOME" "$HMLAB_HOME/ca" "$HMLAB_HOME/src" "$HMLAB_HOME/build" "$HMLAB_HOME/runs"
  chmod 700 "$HMLAB_HOME" "$HMLAB_HOME/ca"
  touch "$SECRETS"
  chmod 600 "$SECRETS"
  local key added=""
  # Each is added once, when it is missing, and never changed after: a database
  # volume keeps the password it was first started with.
  for key in HMLAB_MYSQL_ROOT_PASSWORD HMLAB_MARIADB_ROOT_PASSWORD HMLAB_POSTGRES_PASSWORD \
             HMLAB_MSSQL_SA_PASSWORD HMLAB_DB_PASSWORD HMLAB_KEYCLOAK_ADMIN_PASSWORD \
             HMLAB_IDP_USER_PASSWORD HMLAB_IDP_CLIENT_SECRET HMLAB_KUMA_ADMIN_PASSWORD \
             HMLAB_PEER_PASSWORD HMLAB_BENCH_ACCOUNT_PASSWORD; do
    if ! grep -q "^$key=" "$SECRETS"; then
      if [ "$key" = HMLAB_MSSQL_SA_PASSWORD ]; then
        # SQL Server's policy wants three kinds of character of the four.
        printf '%s=Sa-%s-9z\n' "$key" "$(generate)" >> "$SECRETS"
      else
        printf '%s=%s\n' "$key" "$(generate)" >> "$SECRETS"
      fi
      added="$added $key"
    fi
  done
  say "credentials: $SECRETS (mode $(stat -c %a "$SECRETS"))${added:+; generated:$added}"

  local ca="$HMLAB_HOME/ca"
  if [ ! -f "$ca/lab-ca.pem" ]; then
    openssl req -x509 -newkey rsa:3072 -nodes -days 3650 -sha256 \
      -keyout "$ca/lab-ca.key" -out "$ca/lab-ca.pem" -subj "/CN=hMailServer test lab CA/O=hmlab" \
      -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2> /dev/null \
      || die "making the lab CA failed"
    say "lab CA: $ca/lab-ca.pem"
  fi
  if [ ! -f "$ca/keycloak.pem" ]; then
    # The address is a dNSName as well as an iPAddress: the server checks a host
    # name with X509_check_host alone (CertificateVerifier.cpp), which reads the
    # dNSName entries and never the iPAddress ones.
    local ext
    ext=$(mktemp)
    printf 'subjectAltName=DNS:%s,IP:%s,DNS:keycloak,DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature,keyEncipherment\nbasicConstraints=CA:FALSE\n' "$LAB_IP" "$LAB_IP" > "$ext"
    openssl req -newkey rsa:2048 -nodes -keyout "$ca/keycloak.key" -out "$ca/keycloak.csr" -subj "/CN=$LAB_IP/O=hmlab" 2> /dev/null \
      && openssl x509 -req -in "$ca/keycloak.csr" -CA "$ca/lab-ca.pem" -CAkey "$ca/lab-ca.key" -CAcreateserial \
           -days 825 -sha256 -extfile "$ext" -out "$ca/keycloak.pem" 2> /dev/null \
      || die "making Keycloak's certificate failed"
    rm -f "$ext" "$ca/keycloak.csr"
    say "Keycloak's certificate: $ca/keycloak.pem (for $LAB_IP, keycloak, localhost)"
  fi
  chmod 600 "$ca/lab-ca.key" "$ca/keycloak.key"
  chmod 644 "$ca/lab-ca.pem" "$ca/keycloak.pem"
}

# ---------------------------------------------------------------- services

# The application user every run connects as: hmlab, with the password in
# HMLAB_DB_PASSWORD, allowed to make and drop databases named hmlab_*. The SQL goes
# to the client on standard input, so no password is on a command line.
ensure_db_user() {
  local db="$1"
  case "$db" in
    mysql|mariadb)
      local client=mariadb root_password="$HMLAB_MARIADB_ROOT_PASSWORD"
      if [ "$db" = mysql ]; then client=mysql; root_password="$HMLAB_MYSQL_ROOT_PASSWORD"; fi
      MYSQL_PWD="$root_password" docker exec -i -e MYSQL_PWD "hmlab-$db-1" "$client" -uroot -h 127.0.0.1 > /dev/null <<SQL
CREATE USER IF NOT EXISTS 'hmlab'@'%' IDENTIFIED BY '$HMLAB_DB_PASSWORD';
ALTER USER 'hmlab'@'%' IDENTIFIED BY '$HMLAB_DB_PASSWORD';
GRANT ALL PRIVILEGES ON \`hmlab\\_%\`.* TO 'hmlab'@'%';
FLUSH PRIVILEGES;
SQL
      ;;
    postgresql)
      docker exec -i -u postgres hmlab-postgresql-1 psql -q -v ON_ERROR_STOP=1 > /dev/null <<SQL
DO \$\$ BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hmlab') THEN CREATE ROLE hmlab; END IF;
END \$\$;
ALTER ROLE hmlab WITH LOGIN CREATEDB PASSWORD '$HMLAB_DB_PASSWORD';
SQL
      ;;
    mssql)
      SQLCMDPASSWORD="$HMLAB_MSSQL_SA_PASSWORD" docker exec -i -e SQLCMDPASSWORD hmlab-mssql-1 \
        /opt/mssql-tools18/bin/sqlcmd -C -S localhost -U sa -b > /dev/null <<SQL
IF NOT EXISTS (SELECT * FROM sys.server_principals WHERE name = 'hmlab')
  CREATE LOGIN hmlab WITH PASSWORD = '$HMLAB_DB_PASSWORD', CHECK_POLICY = OFF;
ALTER LOGIN hmlab WITH PASSWORD = '$HMLAB_DB_PASSWORD';
ALTER SERVER ROLE dbcreator ADD MEMBER hmlab;
GO
SQL
      ;;
  esac
  local code=$?
  [ "$code" -eq 0 ] || die "making the application user hmlab on $db failed (exit $code)"
  say "$db: application user hmlab ready (password: HMLAB_DB_PASSWORD in $SECRETS)"
}

cmd_up() {
  [ $# -gt 0 ] || die "up what? core, mysql, mariadb, postgresql, mssql, idp, peers"
  load_secrets
  local profile services
  for profile in "$@"; do
    services=$(services_of "$profile")
    say "== up $profile ($services)"
    # $services is the profile's service names from the compose file, split on purpose.
    # shellcheck disable=SC2086
    compose --profile "$profile" up -d --wait $services || die "$profile did not come up healthy" # nosemgrep
    case "$profile" in
      mysql|mariadb|postgresql|mssql) ensure_db_user "$profile" ;;
      idp) python3 "$LAB_DIR/keycloak-setup.py" setup || die "setting up Keycloak's realm failed" ;;
    esac
  done
}

cmd_down() {
  [ $# -gt 0 ] || die "down what? mysql, mariadb, postgresql, mssql, idp, peers (core is left alone)"
  load_secrets
  local profile services
  for profile in "$@"; do
    [ "$profile" != core ] || { say "core stays up: stop it with docker compose -p hmlab stop kuma, deliberately"; continue; }
    services=$(services_of "$profile")
    say "== down $profile ($services); volumes kept"
    # $services is the profile's service names from the compose file, split on purpose.
    # shellcheck disable=SC2086
    compose --profile "$profile" stop $services && compose --profile "$profile" rm -f $services > /dev/null # nosemgrep
  done
}

cmd_status() {
  docker ps -a --filter "label=com.docker.compose.project=hmlab" --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'
  docker ps -a --filter "name=^$BENCH\$" --format '{{.Names}}\t{{.Status}}\t{{.Ports}}'
  say ""
  docker stats --no-stream --format 'table {{.Name}}\t{{.MemUsage}}\t{{.CPUPerc}}' \
    $(docker ps -q --filter "label=com.docker.compose.project=hmlab") $(docker ps -q --filter "name=^$BENCH\$") 2> /dev/null
  say ""
  docker volume ls --filter "label=com.docker.compose.project=hmlab" --format '{{.Name}}'
}

cmd_kuma_setup() {
  load_secrets
  local token_file="$HMLAB_HOME/gitlab-token"
  HMLAB_GITLAB_TOKEN=""
  if [ -f "$token_file" ]; then
    HMLAB_GITLAB_TOKEN=$(tr -d ' \r\n' < "$token_file")
  fi
  HMLAB_GITLAB_TOKEN="$HMLAB_GITLAB_TOKEN" docker exec -e HMLAB_KUMA_ADMIN_PASSWORD -e HMLAB_GITLAB_TOKEN hmlab-kuma-1 node /hmlab/setup.js
}

# ---------------------------------------------------------------- the build

cmd_build() {
  local commit out log
  commit=$(commit_of_tree)
  out="$HMLAB_HOME/build/$commit"
  log="$HMLAB_HOME/build/$commit.log"
  if [ -x "$out/hmailserver" ] && [ "${1:-}" != "--again" ]; then
    say "already built: $out/hmailserver ($(head -n 2 "$out/build-info.txt" | tr '\n' ' ')); --again rebuilds"
    return 0
  fi
  mkdir -p "$out"
  say "== building $commit with clang in $(ci_image); log $log"
  # As root, as the linux-build job runs, and the output handed back to the calling
  # user at the end. The tree is read-only; the build directory is the container's
  # own /tmp and goes with it. Five jobs by default, not the job's six: linix is
  # shared.
  docker run --rm --name "hmlab-build-$commit" \
    -e HM_BUILD_JOBS="${HM_BUILD_JOBS:-5}" -e HM_OUT_DIR=/out -e CI_COMMIT_SHA="$commit" -e OWNER="$(id -u):$(id -g)" \
    -v "$TREE:/src:ro" -v "$out:/out" -w /src "$(ci_image)" \
    bash -c 'bash build/ci/linux-build.sh; code=$?; chown -R "$OWNER" /out; exit $code' > "$log" 2>&1
  local code=$?
  tail -n 6 "$log"
  [ "$code" -eq 0 ] || die "the build failed (exit $code): $log"
}

# ---------------------------------------------------------------- the bench

# The address a database has from inside the lab network, and its application
# database name.
db_host() { case "$1" in mysql|mariadb|postgresql) echo "$1" ;; *) die "no Linux server backend '$1' (mysql, mariadb, postgresql)" ;; esac; }
db_port() { case "$1" in postgresql) echo 5432 ;; *) echo 3306 ;; esac; }

# Runs build/lab/db-suite.sh <mode> in the bench image, on the lab network, as the
# hMailServer under test: the server built from this tree, the tests from this tree.
run_bench() {
  local mode="$1" db="$2" name="$3" results="$4"; shift 4
  local commit binary
  commit=$(commit_of_tree)
  binary="$HMLAB_HOME/build/$commit/hmailserver"
  [ -x "$binary" ] || die "no server built from $commit at $binary - run: bash build/lab/lab.sh build"
  docker ps -q --filter "name=^hmlab-$db-1\$" --filter health=healthy | grep -q . \
    || die "$db is not up - run: bash build/lab/lab.sh up $db"
  mkdir -p "$results"
  local image
  image=$(bench_image) || exit 1
  # The tree is copied inside the container, so that what dotnet and the server write
  # stays there and nothing root's is left in this user's tree.
  HM_LAB_DB_PASSWORD="$HMLAB_DB_PASSWORD" HMLAB_BENCH_ACCOUNT_PASSWORD="$HMLAB_BENCH_ACCOUNT_PASSWORD" \
  docker run --rm --init "$@" \
    --network hmlab --hostname mail.example.test \
    -v "$TREE:/src:ro" -v "$HMLAB_HOME/build/$commit:/build:ro" -v "$results:/results" \
    -v "$NUGET_VOLUME:/root/.nuget/packages" \
    -v "$HMLAB_HOME/ca/lab-ca.pem:/usr/local/share/ca-certificates/hmlab-ca.crt:ro" \
    -e HM_LAB_DB="$db" -e HM_LAB_DB_HOST="$(db_host "$db")" -e HM_LAB_DB_PORT="$(db_port "$db")" \
    -e HM_LAB_DB_USER=hmlab -e HM_LAB_DB_PASSWORD -e HM_LAB_DB_NAME="hmlab_$name" \
    -e HM_SUITE_FILTER="${HM_SUITE_FILTER:-db}" -e HM_SERVER_BINARY=/build/hmailserver \
    -e HM_RESULTS_DIR=/results -e HM_RESULTS_OWNER="$(id -u):$(id -g)" \
    -e HMLAB_BENCH_ACCOUNT_PASSWORD \
    "$image" bash -c 'mkdir -p /work && cd /src && cp -a --parents global.json hmailserver/test hmailserver/source/DBScripts hmailserver/installation/Extras build /work/ && cd /work && update-ca-certificates > /dev/null 2>&1; exec bash build/lab/db-suite.sh "$0"' "$mode"
}

cmd_createdb() {
  local db="${1:-}" name="${2:-empty}"
  [ -n "$db" ] || die "createdb <mysql|mariadb|postgresql|mssql> [name]"
  load_secrets
  if [ "$db" = mssql ]; then
    create_mssql "$name"
    return
  fi
  run_bench create "$db" "$name" "$HMLAB_HOME/runs/createdb-$db-$name" --name "hmlab-createdb-$db"
}

# SQL Server: the Linux server cannot use it (DALConnectionFactory refuses it,
# HM6390), so the create script is run the way the Windows server runs it -
# SQLScriptParser's rules, in build/lab/mssql-script.py - through sqlcmd inside the
# SQL Server container. hmlab owns the result, for a Windows server to be pointed at.
create_mssql() {
  local name="hmlab_$1"
  docker ps -q --filter "name=^hmlab-mssql-1\$" --filter health=healthy | grep -q . || die "mssql is not up - run: bash build/lab/lab.sh up mssql"
  local sqlcmd=(/opt/mssql-tools18/bin/sqlcmd -C -S localhost -U sa -b)
  SQLCMDPASSWORD="$HMLAB_MSSQL_SA_PASSWORD" docker exec -i -e SQLCMDPASSWORD hmlab-mssql-1 "${sqlcmd[@]}" > /dev/null <<SQL || die "dropping or creating $name failed"
IF DB_ID('$name') IS NOT NULL BEGIN ALTER DATABASE [$name] SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE [$name]; END
CREATE DATABASE [$name];
GO
SQL
  local batches
  batches=$(python3 "$LAB_DIR/mssql-script.py" "$TREE/hmailserver/source/DBScripts/CreateTablesMSSQL.sql" --count) || die "splitting the create script failed"
  say "== $name on SQL Server: $batches statements from CreateTablesMSSQL.sql, split as SQLScriptParser splits them"
  python3 "$LAB_DIR/mssql-script.py" "$TREE/hmailserver/source/DBScripts/CreateTablesMSSQL.sql" \
    | SQLCMDPASSWORD="$HMLAB_MSSQL_SA_PASSWORD" docker exec -i -e SQLCMDPASSWORD hmlab-mssql-1 "${sqlcmd[@]}" -d "$name" -I > "$HMLAB_HOME/runs/createdb-mssql.log" 2>&1 \
    || { tail -n 20 "$HMLAB_HOME/runs/createdb-mssql.log"; die "the create script failed on SQL Server"; }
  SQLCMDPASSWORD="$HMLAB_MSSQL_SA_PASSWORD" docker exec -i -e SQLCMDPASSWORD hmlab-mssql-1 "${sqlcmd[@]}" -d "$name" -h -1 -W <<SQL
SET NOCOUNT ON;
ALTER AUTHORIZATION ON DATABASE::[$name] TO hmlab;
SELECT 'schema ' + CAST(value AS varchar(12)) FROM hm_dbversion;
SELECT CAST(COUNT(*) AS varchar(12)) + ' tables' FROM sys.tables;
GO
SQL
  say "$name on SQL Server 2022 at $LAB_IP,10313, owned by hmlab"
}

cmd_suite() {
  local db="${1:-}"
  [ -n "$db" ] || die "suite <mysql|mariadb|postgresql> [db|all|<filter>]"
  [ "$db" != mssql ] || die "the Linux server cannot use SQL Server (HM6390: it is reached through ADO, which exists only on Windows); createdb mssql makes a database for a Windows server"
  load_secrets
  local stamp results
  stamp=$(date -u +%Y%m%d-%H%M%S)
  results="$HMLAB_HOME/runs/$stamp-$db-$(commit_of_tree)"
  say "== suite on $db, filter ${2:-db}; results in $results"
  HM_SUITE_FILTER="${2:-db}" run_bench run "$db" suite "$results" --name "hmlab-suite-$db"
}

cmd_serve() {
  local db="${1:-mariadb}"
  load_secrets
  docker rm -f "$BENCH" > /dev/null 2>&1 || true
  local results="$HMLAB_HOME/runs/serve-$db"
  rm -rf "$results"
  # HMLAB_SERVE_STORE names a docker volume for the server's tree - its data, its
  # logs, its temp - in place of the container's own overlay filesystem: what the
  # published numbers need, so that the message store is on a volume as Postfix's
  # queue and Dovecot's Maildir are (lab.sh numbers).
  local store=()
  if [ -n "${HMLAB_SERVE_STORE:-}" ]; then
    store=(-v "$HMLAB_SERVE_STORE:/store" -e HM_TREE=/store/tree)
    say "== the server's tree on volume $HMLAB_SERVE_STORE"
  fi
  # The served server alone answers to hmailserver on the lab network, the name
  # Postfix's transport and imaptest use; a suite run beside it must not.
  run_bench serve "$db" serve "$results" --name "$BENCH" --detach --network-alias hmailserver "${store[@]}" \
    -p 10350:2525 -p 10351:1110 -p 10352:1143 -p 10353:587 > /dev/null || die "the bench did not start"
  local up=""
  for _ in $(seq 1 180); do
    if [ -f "$results/serving" ]; then up=yes; break; fi
    if ! docker ps -q --filter "name=^$BENCH\$" | grep -q .; then break; fi
    sleep 1
  done
  [ -n "$up" ] || { docker logs --tail 40 "$BENCH" 2>&1; die "the server did not come up"; }
  cat "$results/serving"
}

cmd_unserve() {
  docker stop -t 30 "$BENCH" > /dev/null 2>&1 && say "$BENCH stopped" || say "$BENCH was not running"
}

need_bench() {
  docker ps -q --filter "name=^$BENCH\$" | grep -q . || die "nothing served - run: bash build/lab/lab.sh serve mariadb"
}

cmd_interop() {
  load_secrets
  need_bench
  docker ps -q --filter "name=^hmlab-postfix-1\$" | grep -q . || die "the peers are not up - run: bash build/lab/lab.sh up peers"
  HMLAB_PEER_PASSWORD="$HMLAB_PEER_PASSWORD" docker exec -e HMLAB_PEER_PASSWORD -e HMLAB_BENCH_ACCOUNT_PASSWORD "$BENCH" bash /work/build/lab/interop.sh
}

cmd_imaptest() {
  local secs="${1:-}"
  load_secrets
  need_bench
  local tests="$HMLAB_HOME/imaptest/$IMAPTEST_COMMIT"
  if [ ! -d "$tests/src/tests" ]; then
    # From Codeberg, where Dovecot keeps imaptest (codeberg.org/dovecot/imaptest): the
    # image on Docker Hub carries the program and not its scripted tests.
    mkdir -p "$tests"
    curl -fsSL "https://codeberg.org/dovecot/imaptest/archive/$IMAPTEST_COMMIT.tar.gz" | tar -xzf - -C "$tests" --strip-components=1 \
      || die "fetching imaptest's tests from codeberg.org failed"
  fi
  # The two accounts imaptest logs in as, on the served server: made fresh for each
  # run, so that it starts from empty mailboxes, with a password made for the run
  # alone and deleted with them at the end. In its scripted mode imaptest takes the
  # password only as pass= on its command line, which is why it is not one from
  # ~/hmlab/secrets.env.
  local run_password
  run_password=$(generate)
  HMLAB_BENCH_ACCOUNT_PASSWORD="$run_password" docker exec -e HMLAB_BENCH_ACCOUNT_PASSWORD "$BENCH" \
    bash /work/build/lab/interop.sh accounts imaptest1 imaptest2 > /dev/null || die "making the imaptest accounts failed"
  local users
  users=$(mktemp -d)
  chmod 755 "$users"
  printf 'imaptest1@example.test:%s\nimaptest2@example.test:%s\n' "$run_password" "$run_password" > "$users/users"
  chmod 644 "$users/users"
  local run="$HMLAB_HOME/runs/imaptest-$(date -u +%Y%m%d-%H%M%S)"
  local log="$run.log"
  if [ -z "$secs" ]; then
    # One script at a time (imaptest-each.sh says why), as this user, a log per
    # script in $run/.
    mkdir -p "$run"
    say "== imaptest's scripted tests (codeberg.org/dovecot/imaptest $IMAPTEST_COMMIT) against hmailserver:1143; logs $run/"
    IMAPTEST_PASS="$run_password" docker run --rm --network hmlab --user "$(id -u):$(id -g)" \
      -e IMAPTEST_PASS -e HOST=hmailserver -e PORT=1143 -e USER1=imaptest1@example.test -e USER2=imaptest2@example.test \
      -v "$tests/src/tests:/tests:ro" -v "$run:/out" -v "$LAB_DIR/imaptest-each.sh:/each.sh:ro" \
      --entrypoint sh "$IMAPTEST_IMAGE" /each.sh > "$log" 2>&1
    local code=$?
    rm -rf "$users"
    docker exec "$BENCH" bash /work/build/lab/interop.sh drop imaptest1 imaptest2 > /dev/null
    cat "$log"
    say ""
    awk '{print $2}' "$log" | sort | uniq -c | sed 's/^/   /'
    say "imaptest exit $code; the summary: $log"
    return
  else
    say "== imaptest's checking load, $secs s, against hmailserver:1143; log $log"
    docker run --rm --network hmlab -v "$tests/src/tests:/tests:ro" -v "$users:/u:ro" "$IMAPTEST_IMAGE" \
      host=hmailserver port=1143 user=imaptest1@example.test userfile=/u/users mbox=/tests/default.mbox clients=4 "secs=$secs" > "$log" 2>&1
  fi
  local code=$?
  rm -rf "$users"
  docker exec "$BENCH" bash /work/build/lab/interop.sh drop imaptest1 imaptest2 > /dev/null
  tail -n 40 "$log"
  say "imaptest exit $code; the whole output: $log"
}

# ---------------------------------------------------------------- the published numbers

# A container's address on the lab network.
lab_address() {
  docker inspect -f '{{range $name, $net := .NetworkSettings.Networks}}{{if eq $name "hmlab"}}{{$net.IPAddress}}{{end}}{{end}}' "$1" 2> /dev/null
}

# The load generator on linix against one side of the comparison. The generator
# reaches each container at its lab address, not through a published port, and
# samples the side's containers' cgroups: the served server and its database, or
# Postfix and Dovecot - each side the whole mail service. Beside each result it
# records how that side was configured (the fsync setting; postconf -n and
# doveconf -n), so that the fairness rules can be checked after the fact.
cmd_numbers() {
  local target="${1:-}" mode="${2:-}"
  [ -n "$target" ] && [ -n "$mode" ] || die "numbers <hmailserver|postfix-dovecot> <prepare|smtp-in|smtp-out|imap-sessions|imaptest> [loadgen.py arguments]"
  shift 2
  load_secrets
  command -v python3 > /dev/null || die "no python3 on this host"
  local loadgen="$TREE/build/perf/load/loadgen.py"
  local gateway
  gateway=$(docker network inspect hmlab -f '{{(index .IPAM.Config 0).Gateway}}' 2> /dev/null)
  [ -n "$gateway" ] || die "no hmlab network: bash build/lab/lab.sh up core"
  local mailboxes="${HMLAB_NUMBERS_MAILBOXES:-50}"
  local smtp imap smtp_port imap_port domain containers server alias db
  case "$target" in
    hmailserver)
      need_bench
      db=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$BENCH" | sed -n 's/^HM_LAB_DB=//p')
      smtp=$(lab_address "$BENCH"); imap="$smtp"; smtp_port=2525; imap_port=1143; domain=example.test; alias=hmailserver
      containers="$BENCH,hmlab-$db-1"
      server="hMailServer $(commit_of_tree) (Linux build, lab.sh serve $db)"
      export HMLOAD_PASSWORD="$HMLAB_BENCH_ACCOUNT_PASSWORD"
      ;;
    postfix-dovecot)
      docker ps -q --filter "name=^hmlab-numbers-dovecot-1\$" | grep -q . || die "profile numbers is not up - run: bash build/lab/lab.sh up numbers"
      smtp=$(lab_address hmlab-numbers-postfix-1); imap=$(lab_address hmlab-numbers-dovecot-1)
      smtp_port=587; imap_port=31143; domain=numbers.test; alias=numbers-dovecot
      containers="hmlab-numbers-postfix-1,hmlab-numbers-dovecot-1"
      server="Postfix 3.10 (boky/postfix 5.1.0) + Dovecot 2.4.5"
      export HMLOAD_PASSWORD="$HMLAB_PEER_PASSWORD"
      ;;
    *) die "no target '$target' (hmailserver, postfix-dovecot)" ;;
  esac
  [ -n "$smtp" ] && [ -n "$imap" ] || die "could not find the $target containers' addresses on hmlab"
  local run
  run="$HMLAB_HOME/runs/numbers-$(date -u +%Y%m%d-%H%M%S)-$target-$mode"
  mkdir -p "$HMLAB_HOME/runs"

  # What the side ran with, beside the result.
  {
    echo "== $server, $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    if [ "$target" = hmailserver ]; then
      docker exec "$BENCH" sh -c 'curl -s -u "Administrator:${HMTEST_ADMIN_PASSWORD:-testar}" http://127.0.0.1:8045/api/v1/settings/ini/MessageStoreFsync; echo'
      if [ "$db" = postgresql ]; then
        docker exec "hmlab-$db-1" psql -U postgres -tA -c "select 'fsync ' || current_setting('fsync') || ', synchronous_commit ' || current_setting('synchronous_commit')" || true
      else
        docker exec "hmlab-$db-1" sh -c 'echo "innodb_flush_log_at_trx_commit, sync_binlog, log_bin:"; (MYSQL_PWD="${MARIADB_ROOT_PASSWORD:-$MYSQL_ROOT_PASSWORD}"; export MYSQL_PWD; mariadb -N -u root -e "select @@innodb_flush_log_at_trx_commit, @@sync_binlog, @@log_bin" 2> /dev/null || mysql -N -u root -e "select @@innodb_flush_log_at_trx_commit, @@sync_binlog, @@log_bin")' || true
      fi
    else
      docker exec hmlab-numbers-postfix-1 postconf -n
      docker exec hmlab-numbers-dovecot-1 doveconf -n
    fi
  } > "$run.config.txt" 2>&1

  local common=(--server "$server" --server-container "$containers" --data-path /var/lib/docker
                --json "$run.json" --markdown "$run.md")
  case "$mode" in
    prepare)
      if [ "$target" = postfix-dovecot ]; then
        say "Postfix and Dovecot need nothing made: any load{n}@numbers.test is a mailbox. Their configuration: $run.config.txt"
        return 0
      fi
      # Inside the bench, where the REST listener is (127.0.0.1:8045), with the tree
      # the bench copied. The generator connects from the lab network's gateway.
      docker exec -e HMLOAD_PASSWORD "$BENCH" sh -c 'HMLOAD_ADMIN_PASSWORD="${HMTEST_ADMIN_PASSWORD:-testar}" exec python3 /work/build/perf/load/loadgen.py prepare-hmailserver "$@"' \
        prepare --accounts "$mailboxes" --route "sink.test=$gateway:10365" --client-range "$gateway-$gateway" \
        --fsync "${HMLAB_NUMBERS_FSYNC:-on}" "$@"
      return
      ;;
    smtp-in)
      python3 "$loadgen" smtp-in --host "$smtp" --port "$smtp_port" --rcpt-template "load{n}@$domain" \
        --mailboxes "$mailboxes" --verify-imap-host "$imap" --verify-imap-port "$imap_port" "${common[@]}" "$@"
      ;;
    smtp-out)
      python3 "$loadgen" smtp-out --host "$smtp" --port "$smtp_port" --mail-from "load1@$domain" \
        --rcpt-template "sink{n}@sink.test" --sink-listen 0.0.0.0:10365 "${common[@]}" "$@"
      ;;
    imap-sessions)
      python3 "$loadgen" imap-sessions --host "$imap" --port "$imap_port" --user-template "load{n}@$domain" \
        --users "$mailboxes" "${common[@]}" "$@"
      ;;
    imaptest)
      local tests="$HMLAB_HOME/imaptest/$IMAPTEST_COMMIT"
      if [ ! -f "$tests/src/tests/default.mbox" ]; then
        mkdir -p "$tests"
        curl -fsSL "https://codeberg.org/dovecot/imaptest/archive/$IMAPTEST_COMMIT.tar.gz" | tar -xzf - -C "$tests" --strip-components=1 \
          || die "fetching imaptest's tests from codeberg.org failed"
      fi
      python3 "$loadgen" imaptest --docker --image "$IMAPTEST_IMAGE" --network hmlab --host "$alias" --port "$imap_port" \
        --user-template "load{n}@$domain" --users "$mailboxes" --mbox "$tests/src/tests/default.mbox" --log "$run.log" \
        "${common[@]}" "$@"
      ;;
    *) die "no mode '$mode' (prepare, smtp-in, smtp-out, imap-sessions, imaptest)" ;;
  esac
  local code=$?
  say "loadgen exit $code; $run.md, $run.json, $run.config.txt"
  return "$code"
}

cmd_idp_check() {
  load_secrets
  python3 "$LAB_DIR/keycloak-setup.py" check || die "the realm check failed"
  if docker ps -q --filter "name=^$BENCH\$" | grep -q .; then
    say ""
    say "== single sign-on into the served server"
    HMLAB_IDP_USER_PASSWORD="$HMLAB_IDP_USER_PASSWORD" docker exec -e HMLAB_IDP_USER_PASSWORD -e HMLAB_BENCH_ACCOUNT_PASSWORD \
      -e HMLAB_IDP_ISSUER="https://$LAB_IP:10321/realms/hmailserver" "$BENCH" python3 /work/build/lab/idp-signin.py
  else
    say "(no server served: lab.sh serve mariadb, then idp-check again, to sign in to it through Keycloak)"
  fi
}

command="${1:-}"
[ -n "$command" ] || { sed -n '6,42p' "$0"; exit 2; }
shift
case "$command" in
  init) cmd_init "$@" ;;
  up) cmd_up "$@" ;;
  down) cmd_down "$@" ;;
  status) cmd_status "$@" ;;
  kuma-setup) cmd_kuma_setup "$@" ;;
  build) cmd_build "$@" ;;
  createdb) cmd_createdb "$@" ;;
  suite) cmd_suite "$@" ;;
  serve) cmd_serve "$@" ;;
  unserve) cmd_unserve "$@" ;;
  interop) cmd_interop "$@" ;;
  imaptest) cmd_imaptest "$@" ;;
  idp-check) cmd_idp_check "$@" ;;
  numbers) cmd_numbers "$@" ;;
  *) die "no command '$command' (bash build/lab/lab.sh for the list)" ;;
esac
