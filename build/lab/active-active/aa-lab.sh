#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The active-active lab: two or more hMailServer nodes on one database, with the
# databases, the object store, a balancer, a remote mail server that remembers what
# it is given, and a load generator - for docs/ActiveActive.md and the units of its
# build plan. Run it ON server2 (10.0.0.19), as a member of the docker group, from a
# copy of the tree. Nothing needs sudo, nothing listens on the host, and everything
# it makes is named hmlab-p-aa-*. README.md beside this file says what each command
# is for and what was measured with it.
#
#   bash build/lab/active-active/aa-lab.sh <command> [arguments]
#
#   init                    ~/hmlab/aa (mode 700), its credentials file (mode 600,
#                           generated once, never printed) and the lab CA
#   image                   the node image: the CI image of this tree, plus the
#                           MariaDB client library and client, libfaketime and
#                           python-tds (for sqlcheck's SQL Server leg)
#   build [--again]         the Linux server of this tree, with clang, in the CI
#                           image (build/ci/linux-build.sh), into ~/hmlab/aa/build/<commit>
#   up <db>...              start databases: postgresql, mariadb, mysql, mssql (the
#                           images build/lab/docker-compose.yml pins), and the
#                           application user hmlab on each
#   store                   MinIO behind the lab CA, with the bucket hmlab-aa
#   sqlcheck <db> [check]   sqlcheck.py against that database: row counts, claims,
#                           a lease on the database's clock and on its UTC clock,
#                           named leases with no row yet, UIDs with and without a
#                           transaction and with rollbacks (SQL Server also at
#                           READ UNCOMMITTED, as the server's sessions run)
#   up-dst <db>             postgresql, mariadb or mysql in a zone with daylight
#                           saving (Europe/London), whose clock libfaketime moves:
#                           container <prefix>-<db>-dst, no volume
#   dst <db> autumn|spring  that database's clock put a minute before the change,
#                           then sqlcheck's dst check: a lease on its local clock
#                           and one on its UTC clock across the change
#   nodes <db> [n] [--shared-data]
#                           the sink, then n nodes (2 by default) of the server built
#                           from this tree on database hmlab_aa of <db>; node1 lays it
#                           out. --shared-data gives every node one data directory on
#                           a shared volume (what docs/WarmStandby.md says not to do)
#   balancer                HAProxy in front of the nodes (haproxy.cfg)
#   load <tag> <count> <to> [hosts]
#                           aa-load.py send: <count> messages to <to> (a recipient or
#                           a comma-separated list) spread over the nodes, or over
#                           the hosts given (balancer:25 for the balancer)
#   check <tag>             aa-load.py check-sink for that load
#   imap <account> [expect] aa-load.py imap: the account's INBOX on every node
#   queue                   the queue and the mailboxes as the database has them:
#                           queued rows, locked rows, and UIDs given twice in a folder
#   errors                  each node's error log: the codes and how often
#   fault <kind> <node>     kill | freeze | thaw | cutdb | heal | restart
#   baseline <db> [--shared-data]
#                           scenario S0: two nodes, 2,000 messages relayed to the
#                           sink and 1,000 into one mailbox, then check, queue,
#                           imap and errors (README.md has what it measured)
#   status                  what runs, and its memory
#   down [all]              stop the nodes, the sink, the balancer and the load
#                           (all: the databases and the store too; the volumes stay)
#
# The databases are reached only on the lab's back network, the nodes, the sink and
# the balancer on the front one; a node is on both. No port is published on the host.
# AA_PREFIX (hmlab-p-aa) names everything it makes, AA_HOME (~/hmlab/aa) is where it
# keeps its credentials and results: a unit runs an instance of its own with both.
set -uo pipefail

AA_DIR="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$AA_DIR/../../.." && pwd)"
AA_HOME="${AA_HOME:-$HOME/hmlab/aa}"
SECRETS="$AA_HOME/secrets.env"
# Everything the lab makes is named $P-*: hmlab-p-aa by default, and a unit of the
# build plan runs an instance of its own beside the others with AA_PREFIX (which
# must begin hmlab-) and an AA_HOME of its own.
P="${AA_PREFIX:-hmlab-p-aa}"
case "$P" in
  *[!a-z0-9-]*) printf 'aa-lab.sh: AA_PREFIX may hold only a-z, 0-9 and - (it is %s)\n' "$P" >&2; exit 2 ;;
  hmlab-[a-z0-9]*) ;;
  *) printf 'aa-lab.sh: AA_PREFIX must begin hmlab- (it is %s)\n' "$P" >&2; exit 2 ;;
esac
FRONT="$P-front"
BACK="$P-back"
DB_NAME=hmlab_aa
PYTHON_TDS_VERSION=1.16.0

say() { printf '%s\n' "$*"; }
die() { printf 'aa-lab.sh: %s\n' "$*" >&2; exit 1; }

commit_of_tree() {
  if [ -f "$TREE/.commit" ]; then
    cut -c1-12 "$TREE/.commit"
  elif git -C "$TREE" rev-parse --short=12 HEAD > /dev/null 2>&1; then
    git -C "$TREE" rev-parse --short=12 HEAD
  else
    die "cannot tell which commit $TREE is (no .commit file, no git)"
  fi
}

ci_image() {
  local dockerfile="$TREE/build/ci/linux.Dockerfile" release digest
  release=$(sed -n 's/^FROM ubuntu:\([0-9][0-9]*\.[0-9][0-9]*\)@.*$/\1/p' "$dockerfile" | head -n 1)
  digest=$(sed 's/\r$//' "$dockerfile" | sha256sum | cut -c1-12)
  printf 'hmailserver-ci/linux:%s-%s' "$release" "$digest"
}

NODE_PACKAGES="libmariadb3 mariadb-client faketime 7zip python3-pip"
node_image() {
  local base tag dockerfile
  base=$(ci_image)
  dockerfile=$(printf 'FROM %s\nRUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends %s > /dev/null && rm -rf /var/lib/apt/lists/*\nRUN pip3 install -q --break-system-packages --no-cache-dir python-tds==%s\n' "$base" "$NODE_PACKAGES" "$PYTHON_TDS_VERSION")
  tag="$P/node:${base##*:}-$(printf '%s' "$dockerfile" | sha256sum | cut -c1-8)"
  if ! docker image inspect "$tag" > /dev/null 2>&1; then
    docker image inspect "$base" > /dev/null 2>&1 || die "the CI image $base is not on this host"
    say "== building $tag from $base" >&2
    printf '%s\n' "$dockerfile" | docker build -q -t "$tag" - >&2 || die "building $tag failed"
  fi
  printf '%s' "$tag"
}

load_secrets() {
  [ -f "$SECRETS" ] || die "no $SECRETS - run: bash $0 init"
  set -a
  # shellcheck disable=SC1090
  . "$SECRETS"
  set +a
}

generate() {
  python3 -c 'import secrets, string; a = string.ascii_letters + string.digits; print("".join(secrets.choice(a) for _ in range(24)))'
}

# The image build/lab/docker-compose.yml pins for a service, so that this lab and
# linix's run the same database builds.
pinned_image() {
  awk -v svc="  $1:" '$0 == svc {found = 1; next} found && $1 == "image:" {print $2; exit}' "$TREE/build/lab/docker-compose.yml"
}

networks() {
  docker network inspect "$FRONT" > /dev/null 2>&1 || docker network create "$FRONT" > /dev/null || die "making $FRONT failed"
  docker network inspect "$BACK" > /dev/null 2>&1 || docker network create "$BACK" > /dev/null || die "making $BACK failed"
}

front_subnet() {
  docker network inspect -f '{{range .IPAM.Config}}{{.Subnet}}{{end}}' "$FRONT"
}

# ---------------------------------------------------------------- init, image, build

cmd_init() {
  umask 077
  mkdir -p "$AA_HOME" "$AA_HOME/ca" "$AA_HOME/build" "$AA_HOME/runs"
  chmod 700 "$AA_HOME" "$AA_HOME/ca"
  touch "$SECRETS"
  chmod 600 "$SECRETS"
  local key added=""
  for key in AA_POSTGRES_PASSWORD AA_MARIADB_ROOT_PASSWORD AA_MYSQL_ROOT_PASSWORD AA_MSSQL_SA_PASSWORD \
             AA_DB_PASSWORD AA_ADMIN_PASSWORD AA_ACCOUNT_PASSWORD AA_MINIO_ACCESS AA_MINIO_SECRET; do
    if ! grep -q "^$key=" "$SECRETS"; then
      if [ "$key" = AA_MSSQL_SA_PASSWORD ]; then
        printf '%s=Sa-%s-9z\n' "$key" "$(generate)" >> "$SECRETS"
      else
        printf '%s=%s\n' "$key" "$(generate)" >> "$SECRETS"
      fi
      added="$added $key"
    fi
  done
  say "credentials: $SECRETS (mode $(stat -c %a "$SECRETS"))${added:+; generated:$added}"
  local ca="$AA_HOME/ca"
  if [ ! -f "$ca/lab-ca.pem" ]; then
    openssl req -x509 -newkey rsa:3072 -nodes -days 3650 -sha256 -keyout "$ca/lab-ca.key" -out "$ca/lab-ca.pem" \
      -subj "/CN=hMailServer active-active lab CA/O=hmlab" \
      -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2> /dev/null \
      || die "making the lab CA failed"
  fi
  if [ ! -f "$ca/minio.pem" ]; then
    local ext
    ext=$(mktemp)
    printf 'subjectAltName=DNS:%s-minio,DNS:minio,DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature,keyEncipherment\nbasicConstraints=CA:FALSE\n' "$P" > "$ext"
    openssl req -newkey rsa:2048 -nodes -keyout "$ca/minio.key" -out "$ca/minio.csr" -subj "/CN=$P-minio/O=hmlab" 2> /dev/null \
      && openssl x509 -req -in "$ca/minio.csr" -CA "$ca/lab-ca.pem" -CAkey "$ca/lab-ca.key" -CAcreateserial \
           -days 825 -sha256 -extfile "$ext" -out "$ca/minio.pem" 2> /dev/null \
      || die "making MinIO's certificate failed"
    rm -f "$ext" "$ca/minio.csr"
  fi
  chmod 600 "$ca/lab-ca.key" "$ca/minio.key"
  chmod 644 "$ca/lab-ca.pem" "$ca/minio.pem"
  say "lab CA: $ca/lab-ca.pem; MinIO's certificate: $ca/minio.pem"
}

cmd_image() {
  node_image > /dev/null
  say "node image: $(node_image)"
}

cmd_build() {
  local commit out log
  commit=$(commit_of_tree)
  out="$AA_HOME/build/$commit"
  log="$AA_HOME/build/$commit.log"
  if [ -x "$out/hmailserver" ] && [ "${1:-}" != "--again" ]; then
    say "already built: $out/hmailserver ($(head -n 2 "$out/build-info.txt" | tr '\n' ' ')); --again rebuilds"
    return 0
  fi
  mkdir -p "$out"
  say "== building $commit with clang in $(ci_image); log $log"
  docker run --rm --name "$P-build-$commit" --cpus "${AA_BUILD_CPUS:-20}" \
    -e HM_BUILD_JOBS="${HM_BUILD_JOBS:-20}" -e HM_OUT_DIR=/out -e CI_COMMIT_SHA="$commit" -e OWNER="$(id -u):$(id -g)" \
    -v "$TREE:/src:ro" -v "$out:/out" -w /src "$(ci_image)" \
    bash -c 'bash build/ci/linux-build.sh; code=$?; chown -R "$OWNER" /out; exit $code' > "$log" 2>&1
  local code=$?
  tail -n 6 "$log"
  [ "$code" -eq 0 ] || die "the build failed (exit $code): $log"
}

binary_dir() {
  local dir
  dir="$AA_HOME/build/$(commit_of_tree)"
  [ -x "$dir/hmailserver" ] || die "no server built from $(commit_of_tree) - run: bash $0 build"
  printf '%s' "$dir"
}

# ---------------------------------------------------------------- the databases and the store

db_port() { case "$1" in postgresql) echo 5432 ;; mssql) echo 1433 ;; *) echo 3306 ;; esac; }

cmd_up() {
  load_secrets
  networks
  local db image
  for db in "$@"; do
    image=$(pinned_image "$db")
    [ -n "$image" ] || die "no service '$db' in build/lab/docker-compose.yml (postgresql, mariadb, mysql, mssql)"
    if ! docker ps -q --filter "name=^$P-$db\$" | grep -q .; then
      docker rm -f "$P-$db" > /dev/null 2>&1 || true
      case "$db" in
        postgresql)
          POSTGRES_PASSWORD="$AA_POSTGRES_PASSWORD" docker run -d --name "$P-$db" --network "$BACK" --memory 2g --cpus 4 \
            -e POSTGRES_USER=postgres -e POSTGRES_PASSWORD -v "$P-$db-data:/var/lib/postgresql/data" "$image" \
            -c max_connections=300 > /dev/null ;;
        mariadb)
          MARIADB_ROOT_PASSWORD="$AA_MARIADB_ROOT_PASSWORD" docker run -d --name "$P-$db" --network "$BACK" --memory 2g --cpus 4 \
            -e MARIADB_ROOT_PASSWORD -e MARIADB_AUTO_UPGRADE=1 -v "$P-$db-data:/var/lib/mysql" "$image" \
            --innodb-buffer-pool-size=256M --max-connections=300 > /dev/null ;;
        mysql)
          MYSQL_ROOT_PASSWORD="$AA_MYSQL_ROOT_PASSWORD" docker run -d --name "$P-$db" --network "$BACK" --memory 2g --cpus 4 \
            -e MYSQL_ROOT_PASSWORD -v "$P-$db-data:/var/lib/mysql" "$image" \
            --innodb-buffer-pool-size=256M --performance-schema=OFF --max-connections=300 > /dev/null ;;
        mssql)
          MSSQL_SA_PASSWORD="$AA_MSSQL_SA_PASSWORD" docker run -d --name "$P-$db" --network "$BACK" --memory 3g --cpus 4 \
            -e ACCEPT_EULA=Y -e MSSQL_PID=Developer -e MSSQL_SA_PASSWORD -e MSSQL_MEMORY_LIMIT_MB=2048 \
            -v "$P-$db-data:/var/opt/mssql" "$image" > /dev/null ;;
        *) die "no database '$db'" ;;
      esac || die "starting $db failed"
    fi
    wait_db "$db"
    ensure_user "$db"
  done
}

# $1 the database, $2 its container ($P-<db> by default).
wait_db() {
  local db="$1" container="${2:-$P-$1}" ok=""
  for _ in $(seq 1 120); do
    case "$db" in
      postgresql) docker exec "$container" pg_isready -U postgres -h 127.0.0.1 > /dev/null 2>&1 && ok=yes ;;
      mariadb) docker exec "$container" healthcheck.sh --connect --innodb_initialized > /dev/null 2>&1 && ok=yes ;;
      mysql) docker exec -e MYSQL_PWD="$AA_MYSQL_ROOT_PASSWORD" "$container" mysqladmin ping -uroot -h 127.0.0.1 --silent > /dev/null 2>&1 && ok=yes ;;
      mssql) SQLCMDPASSWORD="$AA_MSSQL_SA_PASSWORD" docker exec -e SQLCMDPASSWORD "$container" /opt/mssql-tools18/bin/sqlcmd -C -S localhost -U sa -Q 'select 1' -b -o /dev/null > /dev/null 2>&1 && ok=yes ;;
    esac
    [ -n "$ok" ] && break
    sleep 2
  done
  [ -n "$ok" ] || die "$db did not come up in four minutes: docker logs $container"
  say "$db: up ($container)"
}

# The application user hmlab, allowed to make and drop the lab's databases, and the
# database sqlcheck uses. The SQL goes to the client on standard input.
ensure_user() {
  local db="$1" container="${2:-$P-$1}"
  case "$db" in
    mysql|mariadb)
      local client=mariadb root="$AA_MARIADB_ROOT_PASSWORD"
      if [ "$db" = mysql ]; then client=mysql; root="$AA_MYSQL_ROOT_PASSWORD"; fi
      MYSQL_PWD="$root" docker exec -i -e MYSQL_PWD "$container" "$client" -uroot -h 127.0.0.1 > /dev/null <<SQL
CREATE USER IF NOT EXISTS 'hmlab'@'%' IDENTIFIED BY '$AA_DB_PASSWORD';
ALTER USER 'hmlab'@'%' IDENTIFIED BY '$AA_DB_PASSWORD';
GRANT ALL PRIVILEGES ON \`hmlab\\_%\`.* TO 'hmlab'@'%';
FLUSH PRIVILEGES;
CREATE DATABASE IF NOT EXISTS hmlab_aa_sqlcheck;
SQL
      ;;
    postgresql)
      docker exec -i -u postgres "$container" psql -q -v ON_ERROR_STOP=1 > /dev/null <<SQL
DO \$\$ BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hmlab') THEN CREATE ROLE hmlab; END IF;
END \$\$;
ALTER ROLE hmlab WITH LOGIN CREATEDB PASSWORD '$AA_DB_PASSWORD';
SQL
      docker exec -u postgres "$container" psql -tAq -c "select 1 from pg_database where datname = 'hmlab_aa_sqlcheck'" | grep -q 1 \
        || docker exec -u postgres "$container" psql -q -c "create database hmlab_aa_sqlcheck owner hmlab" > /dev/null
      ;;
    mssql)
      SQLCMDPASSWORD="$AA_MSSQL_SA_PASSWORD" docker exec -i -e SQLCMDPASSWORD "$container" \
        /opt/mssql-tools18/bin/sqlcmd -C -S localhost -U sa -b > /dev/null <<SQL
IF NOT EXISTS (SELECT * FROM sys.server_principals WHERE name = 'hmlab')
  CREATE LOGIN hmlab WITH PASSWORD = '$AA_DB_PASSWORD', CHECK_POLICY = OFF;
ALTER LOGIN hmlab WITH PASSWORD = '$AA_DB_PASSWORD';
ALTER SERVER ROLE dbcreator ADD MEMBER hmlab;
GO
IF DB_ID('hmlab_aa_sqlcheck') IS NULL CREATE DATABASE hmlab_aa_sqlcheck;
GO
ALTER AUTHORIZATION ON DATABASE::hmlab_aa_sqlcheck TO hmlab;
GO
SQL
      ;;
  esac
  local code=$?
  [ "$code" -eq 0 ] || die "making the application user on $db failed (exit $code)"
}

cmd_store() {
  load_secrets
  networks
  local image="${AA_MINIO_IMAGE:-pgsty/minio:RELEASE.2026-08-04T00-00-00Z}"
  docker rm -f "$P-minio" > /dev/null 2>&1 || true
  mkdir -p "$AA_HOME/minio-certs/CAs"
  cp "$AA_HOME/ca/minio.pem" "$AA_HOME/minio-certs/public.crt"
  cp "$AA_HOME/ca/minio.key" "$AA_HOME/minio-certs/private.key"
  cp "$AA_HOME/ca/lab-ca.pem" "$AA_HOME/minio-certs/CAs/lab-ca.crt"
  MINIO_ROOT_USER="$AA_MINIO_ACCESS" MINIO_ROOT_PASSWORD="$AA_MINIO_SECRET" docker run -d --name "$P-minio" \
    --network "$BACK" --network-alias minio --memory 1g -e MINIO_ROOT_USER -e MINIO_ROOT_PASSWORD \
    -v "$AA_HOME/minio-certs:/certs:ro" -v "$P-minio-data:/data" "$image" server /data --certs-dir /certs > /dev/null \
    || die "starting MinIO ($image) failed"
  local image_node
  image_node=$(node_image)
  docker run --rm --network "$BACK" -e AA_MINIO_ACCESS -e AA_MINIO_SECRET -v "$AA_HOME/ca/lab-ca.pem:/ca.pem:ro" \
    -v "$AA_DIR:/aa:ro" "$image_node" python3 /aa/s3bucket.py "https://$P-minio:9000" hmlab-aa /ca.pem \
    || die "making the bucket failed"
}

# ---------------------------------------------------------------- the engine checks

cmd_sqlcheck() {
  load_secrets
  local db="${1:-}" which="${2:-all}"
  [ -n "$db" ] || die "usage: sqlcheck <db> [counts|claims|lease|named|uids|rollback|all]"
  docker ps -q --filter "name=^$P-$db\$" | grep -q . || die "$db is not up - run: bash $0 up $db"
  local results="$AA_HOME/runs/sqlcheck-$db-$(date -u +%Y%m%dT%H%M%SZ).jsonl"
  docker run --rm --network "$BACK" -e AA_DB_PASSWORD -e AA_DB_HOST="$P-$db" -e AA_DB_PORT="$(db_port "$db")" \
    -e AA_DB_USER=hmlab -e AA_DB_NAME=hmlab_aa_sqlcheck -v "$AA_DIR:/aa:ro" "$(node_image)" \
    python3 /aa/sqlcheck.py "$db" "$which" | tee "$results"
  local code=${PIPESTATUS[0]}
  say "results: $results"
  return "$code"
}

# libfaketime for the databases' own containers: upstream 0.9.10, checked against its
# SHA-256, built in Red Hat's UBI 9 (glibc 2.34) so that it loads in every database
# image the lab pins - MySQL's is Oracle Linux 9, where the node image's Ubuntu build,
# which asks for glibc 2.38, does not load. Its threaded form (libfaketimeMT), which
# reads the offset under a lock: the other reads it in each thread at once, and
# MySQL's threads then read half a spec and end the server.
FAKETIME_SOURCE=https://github.com/wolfcw/libfaketime/archive/refs/tags/v0.9.10.tar.gz
FAKETIME_SHA256=729ad33b9c750a50d9c68e97b90499680a74afd1568d859c574c0fe56fe7947f
FAKETIME_BUILDER=registry.access.redhat.com/ubi9/ubi@sha256:abdef1603c7dd3281d9223b0419e3cc59446e4a8137f2a7ed5cc5bd7da981f8b

faketime_library() {
  local dir="$1"
  [ -s "$dir/libfaketimeMT.so.1" ] && return 0
  say "== building libfaketime 0.9.10 in UBI 9" >&2
  docker run --rm --platform linux/amd64 --name "$P-faketime" -e FAKETIME_SOURCE="$FAKETIME_SOURCE" \
    -e FAKETIME_SHA256="$FAKETIME_SHA256" -e OWNER="$(id -u):$(id -g)" \
    -v "$dir:/out" "$FAKETIME_BUILDER" bash -c '
      set -e
      dnf -q -y install gcc make tar gzip > /dev/null
      cd /tmp
      curl -fsSL -o faketime.tar.gz "$FAKETIME_SOURCE"
      echo "$FAKETIME_SHA256  faketime.tar.gz" | sha256sum -c - > /dev/null
      tar -xzf faketime.tar.gz
      make -s -C libfaketime-0.9.10/src libfaketimeMT.so.1 > /dev/null 2>&1
      install -m 644 libfaketime-0.9.10/src/libfaketimeMT.so.1 /out/libfaketimeMT.so.1
      chown "$OWNER" /out/libfaketimeMT.so.1' || die "building libfaketime failed"
}

# A database whose zone has daylight saving and whose clock can be moved: the image
# build/lab/docker-compose.yml pins, with TZ=Europe/London (PostgreSQL's timezone
# too), and libfaketime preloaded, reading its offset from a file under
# $AA_HOME/dst that `dst` writes - each process of the database reads it again once
# a second, so the whole server moves at once. The monotonic clock and the files'
# times are left alone. Run as the database's own user (999 in all three images)
# from the first process: libfaketime's shared memory, which the first process
# makes, could not otherwise be opened by the user the entrypoints change to, and
# every server process would end at its first look at the clock. No volume: it is
# made for the check and removed by down all.
cmd_up_dst() {
  load_secrets
  networks
  local db="${1:-}" image dir="$AA_HOME/dst" container
  case "$db" in postgresql|mariadb|mysql) ;; *) die "usage: up-dst postgresql|mariadb|mysql" ;; esac
  image=$(pinned_image "$db")
  container="$P-$db-dst"
  mkdir -p "$dir"
  faketime_library "$dir"
  printf '+0\n' > "$dir/$db.rc"
  chmod 755 "$dir"
  chmod 644 "$dir/libfaketimeMT.so.1" "$dir/$db.rc"
  docker rm -f "$container" > /dev/null 2>&1 || true
  local fake=(--user 999:999 -e TZ=Europe/London -e LD_PRELOAD=/faketime/libfaketimeMT.so.1
              -e "FAKETIME_TIMESTAMP_FILE=/faketime/$db.rc" -e FAKETIME_CACHE_DURATION=1 -e DONT_FAKE_MONOTONIC=1
              -e NO_FAKE_STAT=1 -v "$dir:/faketime:ro")
  case "$db" in
    postgresql)
      POSTGRES_PASSWORD="$AA_POSTGRES_PASSWORD" docker run -d --rm --name "$container" --network "$BACK" --memory 1g --cpus 2 \
        -e POSTGRES_USER=postgres -e POSTGRES_PASSWORD "${fake[@]}" "$image" -c timezone=Europe/London > /dev/null ;;
    mariadb)
      MARIADB_ROOT_PASSWORD="$AA_MARIADB_ROOT_PASSWORD" docker run -d --rm --name "$container" --network "$BACK" --memory 1g --cpus 2 \
        -e MARIADB_ROOT_PASSWORD "${fake[@]}" "$image" --innodb-buffer-pool-size=128M > /dev/null ;;
    mysql)
      MYSQL_ROOT_PASSWORD="$AA_MYSQL_ROOT_PASSWORD" docker run -d --rm --name "$container" --network "$BACK" --memory 1g --cpus 2 \
        -e MYSQL_ROOT_PASSWORD "${fake[@]}" "$image" --innodb-buffer-pool-size=128M --performance-schema=OFF > /dev/null ;;
  esac || die "starting $container failed"
  wait_db "$db" "$container"
  ensure_user "$db" "$container"
}

# The daylight-saving check (E0 of the build plan): the database of up-dst put a
# minute before the change of the season (01:00 UTC on 25 October 2026, or on 28
# March 2027 - these two, forward to them before they come and back to them after),
# then sqlcheck's dst check, which watches a lease on its local clock and one on its
# UTC clock across it; the clock is put back afterwards.
cmd_dst() {
  load_secrets
  local db="${1:-}" season="${2:-}" change now offset container
  case "$db" in postgresql|mariadb|mysql) ;; *) die "usage: dst postgresql|mariadb|mysql autumn|spring" ;; esac
  case "$season" in
    autumn) change=$(date -u -d '2026-10-25 01:00:00' +%s) ;;
    spring) change=$(date -u -d '2027-03-28 01:00:00' +%s) ;;
    *) die "usage: dst <db> autumn|spring" ;;
  esac
  container="$P-$db-dst"
  docker ps -q --filter "name=^$container\$" | grep -q . || die "$container is not up - run: bash $0 up-dst $db"
  now=$(date +%s)
  # Signed either way: after the change has passed the clock goes back to it, and
  # libfaketime reads "-N" as N seconds back - where "+-N", which a plain "+%d"
  # wrote, it reads as no offset at all, and the check then found the clock where
  # it was.
  offset=$(( change - 60 - now ))
  printf '%+d\n' "$offset" > "$AA_HOME/dst/$db.rc"
  # Read again by the database within a second (FAKETIME_CACHE_DURATION).
  sleep 3
  local results
  results="$AA_HOME/runs/sqlcheck-$db-dst-$season-$(date -u +%Y%m%dT%H%M%SZ).jsonl"
  docker run --rm --network "$BACK" -e AA_DB_PASSWORD -e AA_DB_HOST="$container" -e AA_DB_PORT="$(db_port "$db")" \
    -e AA_DB_USER=hmlab -e AA_DB_NAME=hmlab_aa_sqlcheck -e AA_DST_SEASON="$season" -v "$AA_DIR:/aa:ro" "$(node_image)" \
    python3 /aa/sqlcheck.py "$db" dst | tee "$results"
  local code=${PIPESTATUS[0]}
  printf '+0\n' > "$AA_HOME/dst/$db.rc"
  say "results: $results"
  return "$code"
}

# ---------------------------------------------------------------- the nodes

cmd_nodes() {
  load_secrets
  networks
  local db="${1:-}" count=2 shared=""
  shift || true
  [ -n "$db" ] || die "usage: nodes <db> [n] [--shared-data]"
  case "$db" in postgresql|mysql|mariadb) ;; *) die "the Linux server cannot use '$db' (postgresql, mysql, mariadb)";; esac
  for arg in "$@"; do
    case "$arg" in
      --shared-data) shared=yes ;;
      [0-9]*) count="$arg" ;;
      *) die "unknown argument $arg" ;;
    esac
  done
  docker ps -q --filter "name=^$P-$db\$" | grep -q . || die "$db is not up - run: bash $0 up $db"
  local bin image run
  bin=$(binary_dir)
  image=$(node_image)
  cmd_down_nodes
  run="$AA_HOME/runs/current"
  rm -rf "$run"
  mkdir -p "$run/run" "$run/sink"
  printf '%s %s %s nodes=%s shared=%s\n' "$(date -u +%FT%TZ)" "$(commit_of_tree)" "$db" "$count" "${shared:-no}" > "$run/about.txt"
  chmod 777 "$run/run" "$run/sink"
  docker run -d --init --name "$P-sink" --network "$FRONT" --network-alias sink --memory 256m \
    -e AA_SINK_DATA_DELAY_MS="${AA_SINK_DATA_DELAY_MS:-0}" -e AA_SINK_TEMPFAIL_EVERY="${AA_SINK_TEMPFAIL_EVERY:-0}" \
    -v "$AA_DIR:/aa:ro" -v "$run/sink:/data" "$image" python3 /aa/sink.py > /dev/null || die "starting the sink failed"
  local data_args=() i
  if [ -n "$shared" ]; then
    docker volume rm "$P-shared-data" > /dev/null 2>&1 || true
    data_args=(-v "$P-shared-data:/aa/shared" -e AA_DATA=/aa/shared)
  fi
  for i in $(seq 1 "$count"); do
    local layout=0
    [ "$i" = 1 ] && layout=1
    docker run -d --init --name "$P-node$i" --hostname "node$i" --network "$BACK" --memory 2g --cpus 4 \
      -e AA_NODE="node$i" -e AA_DB="$db" -e AA_DB_HOST="$P-$db" -e AA_DB_PORT="$(db_port "$db")" -e AA_DB_USER=hmlab \
      -e AA_DB_NAME="$DB_NAME" -e AA_DB_PASSWORD -e AA_ADMIN_PASSWORD -e AA_ACCOUNT_PASSWORD -e AA_LAYOUT="$layout" \
      -e AA_FRONT_SUBNET="$(front_subnet)" -e AA_SINK_HOST=sink -e FAKETIME="${AA_FAKETIME:-}" \
      "${data_args[@]}" -v "$TREE:/src:ro" -v "$bin:/build:ro" -v "$run/run:/aa/run" -v "$AA_DIR:/aa/lab:ro" \
      "$image" bash /aa/lab/node.sh > /dev/null || die "starting node$i failed"
    docker network connect --alias "node$i" "$FRONT" "$P-node$i" || die "connecting node$i to the front network failed"
    if [ "$i" = 1 ]; then
      wait_serving node1
    fi
  done
  for i in $(seq 2 "$count"); do wait_serving "node$i"; done
  say "nodes: $count on $db ($DB_NAME), data ${shared:+shared }per node; sink log $run/sink/sink.jsonl"
}

wait_serving() {
  local node="$1"
  for _ in $(seq 1 240); do
    docker logs "$P-$node" 2>&1 | grep -q '^SERVING:' && { docker logs "$P-$node" 2>&1 | grep '^SERVING:'; return 0; }
    docker ps -q --filter "name=^$P-$node\$" | grep -q . || break
    sleep 1
  done
  docker logs --tail 40 "$P-$node" 2>&1
  die "$node did not come up"
}

running_nodes() {
  docker ps --format '{{.Names}}' --filter "name=^$P-node" | sed "s/^$P-//" | sort
}

cmd_balancer() {
  local cfg nodes node
  nodes=$(running_nodes)
  [ -n "$nodes" ] || die "no nodes - run: bash $0 nodes <db>"
  cfg="$AA_HOME/runs/current/haproxy.cfg"
  cp "$AA_DIR/haproxy.cfg" "$cfg"
  for node in $nodes; do
    sed -i "s|^    # NODES smtp 2525 send-proxy-v2 check|&\n    server $node $node:2525 send-proxy-v2 check|" "$cfg"
    sed -i "s|^    # NODES imap 1143 check|&\n    server $node $node:1143 check|" "$cfg"
    sed -i "s|^    # NODES pop3 1110 check|&\n    server $node $node:1110 check|" "$cfg"
  done
  docker rm -f "$P-balancer" > /dev/null 2>&1 || true
  docker run -d --name "$P-balancer" --network "$FRONT" --network-alias balancer --memory 256m \
    -v "$cfg:/usr/local/etc/haproxy/haproxy.cfg:ro" "${AA_HAPROXY_IMAGE:-haproxy:3.2}" > /dev/null || die "starting HAProxy failed"
  say "balancer: SMTP balancer:25 (PROXY v2), IMAP balancer:143, POP3 balancer:110, over: ${nodes//$'\n'/ }"
}

cmd_load() {
  load_secrets
  local tag="${1:-}" count="${2:-1000}" to="${3:-}" hosts="${4:-}"
  [ -n "$tag" ] && [ -n "$to" ] || die "usage: load <tag> <count> <to> [hosts]"
  if [ -z "$hosts" ]; then
    local node
    for node in $(running_nodes); do hosts="${hosts:+$hosts,}$node:2525"; done
  fi
  # Straight to a node, each connection opens with a PROXY header, which a node
  # wants from the front network; the balancer adds its own.
  local proxy=(--proxy)
  case ",$hosts" in *,balancer:*) proxy=() ;; esac
  local run="$AA_HOME/runs/current"
  docker rm -f "$P-load" > /dev/null 2>&1 || true
  docker run --rm --name "$P-load" --network "$FRONT" -v "$AA_DIR:/aa:ro" -v "$run:/run-dir" "$(node_image)" \
    python3 /aa/aa-load.py send --hosts "$hosts" --count "$count" --to "$to" --threads "${AA_LOAD_THREADS:-8}" \
    --tag "$tag" --out "/run-dir/sent-$tag.jsonl" "${proxy[@]}"
}

cmd_check() {
  local tag="${1:-}"
  [ -n "$tag" ] || die "usage: check <tag>"
  local run="$AA_HOME/runs/current"
  docker run --rm -v "$AA_DIR:/aa:ro" -v "$run:/run-dir" "$(node_image)" \
    python3 /aa/aa-load.py check-sink --sent "/run-dir/sent-$tag.jsonl" --sink /run-dir/sink/sink.jsonl --show
}

cmd_imap() {
  load_secrets
  local account="${1:-}" expect="${2:-}" hosts="" node
  [ -n "$account" ] || die "usage: imap <account> [expect]"
  for node in $(running_nodes); do hosts="${hosts:+$hosts,}$node:1143"; done
  local expect_args=()
  [ -n "$expect" ] && expect_args=(--expect "$expect")
  docker run --rm --network "$FRONT" -e AA_ACCOUNT_PASSWORD -v "$AA_DIR:/aa:ro" "$(node_image)" \
    python3 /aa/aa-load.py imap --hosts "$hosts" --user "$account" "${expect_args[@]}"
}

db_of_run() {
  awk '{print $3}' "$AA_HOME/runs/current/about.txt" 2> /dev/null
}

lab_sql() {
  local db
  db=$(db_of_run)
  [ -n "$db" ] || die "no current run"
  # The password reaches the container through the environment by name, never as an
  # argument: a process's arguments are in the host's process list.
  case "$db" in
    postgresql) PGPASSWORD="$AA_DB_PASSWORD" docker run --rm -i --network "$BACK" -e PGPASSWORD "$(node_image)" \
                  psql -h "$P-$db" -U hmlab -d "$DB_NAME" -At -F ' | ' ;;
    *) MYSQL_PWD="$AA_DB_PASSWORD" docker run --rm -i --network "$BACK" -e MYSQL_PWD "$(node_image)" \
         mariadb -h "$P-$db" -u hmlab --skip-ssl-verify-server-cert -N "$DB_NAME" ;;
  esac
}

cmd_queue() {
  load_secrets
  lab_sql <<'SQL'
select 'queued (messagetype 1)', count(*) from hm_messages where messagetype = 1;
select 'queued and locked', count(*) from hm_messages where messagetype = 1 and messagelocked = 1;
select 'in mailboxes (messagetype 2)', count(*) from hm_messages where messagetype = 2;
select 'folders with a UID given twice', count(*) from (select messagefolderid, messageuid from hm_messages where messagetype = 2 group by messagefolderid, messageuid having count(*) > 1) d;
select 'rows sharing a UID with another', coalesce(sum(n), 0) from (select count(*) as n from hm_messages where messagetype = 2 group by messagefolderid, messageuid having count(*) > 1) d;
SQL
}

cmd_errors() {
  local node
  for node in $(running_nodes); do
    say "== $node"
    docker exec "$P-$node" bash -c 'cat /aa/tree/Logs/ERROR_*.log 2> /dev/null' \
      | grep -o -E 'Code: HM[0-9]+|HM[0-9]{4}' | sed 's/Code: //' | sort | uniq -c | sort -rn | head -20
  done
}

cmd_fault() {
  local kind="${1:-}" node="${2:-}"
  [ -n "$kind" ] && [ -n "$node" ] || die "usage: fault kill|freeze|thaw|cutdb|heal|restart <node>"
  local c="$P-$node"
  case "$kind" in
    kill) docker kill -s KILL "$c" > /dev/null ;;
    freeze) docker pause "$c" > /dev/null ;;
    thaw) docker unpause "$c" > /dev/null ;;
    cutdb) docker network disconnect "$BACK" "$c" ;;
    heal) docker network connect "$BACK" "$c" ;;
    restart) docker restart -t 30 "$c" > /dev/null ;;
    *) die "no fault '$kind' (kill, freeze, thaw, cutdb, heal, restart)" ;;
  esac && say "$kind: $node at $(date -u +%T)"
}

# The baseline (scenario S0 of the build plan): two nodes of this tree on one
# database, each with its own data directory - or one shared, with --shared-data -
# and the two loads the design is held to: mail relayed to the sink, and mail into
# one mailbox. On a tree without the active-active units it is the negative
# control: it shows what two nodes do to each other today.
cmd_baseline() {
  local db="${1:-}"
  shift || true
  [ -n "$db" ] || die "usage: baseline <db> [--shared-data]"
  local settle="${AA_SETTLE_SECONDS:-90}"
  say "== baseline on $db $* at $(date -u +%T); settling ${settle}s after each load"
  cmd_nodes "$db" 2 "$@" || return 1
  cmd_load s0sink "${AA_SINK_COUNT:-2000}" r1@sink.test
  sleep "$settle"
  say "== relayed to the sink"
  cmd_check s0sink
  cmd_queue
  cmd_load s0box "${AA_MAILBOX_COUNT:-1000}" account1@example.test
  sleep "$settle"
  say "== into one mailbox"
  cmd_queue
  cmd_imap account1@example.test
  say "== error codes"
  cmd_errors
  say "== baseline end $(date -u +%T)"
}

cmd_status() {
  docker ps -a --filter "name=^$P-" --format '{{.Names}}\t{{.Status}}' | sort
  local running=()
  mapfile -t running < <(docker ps -q --filter "name=^$P-")
  [ "${#running[@]}" -gt 0 ] || return 0
  docker stats --no-stream --format '{{.Name}}\t{{.MemUsage}}\t{{.CPUPerc}}' "${running[@]}" 2> /dev/null | sort
}

cmd_down_nodes() {
  local c
  for c in $(docker ps -a --format '{{.Names}}' --filter "name=^$P-(node[0-9]+|sink|balancer|load)\$"); do
    docker rm -f "$c" > /dev/null 2>&1 || true
  done
}

cmd_down() {
  cmd_down_nodes
  if [ "${1:-}" = all ]; then
    local c
    for c in $(docker ps -a --format '{{.Names}}' --filter "name=^$P-(postgresql|mariadb|mysql|mssql|minio|postgresql-dst|mariadb-dst|mysql-dst)\$"); do
      docker rm -f "$c" > /dev/null 2>&1 || true
    done
    docker network rm "$FRONT" "$BACK" > /dev/null 2>&1 || true
  fi
  say "down${1:+ ($1)}; volumes $P-* kept (docker volume ls --filter name=$P)"
}

command="${1:-}"
shift || true
case "$command" in
  init) cmd_init "$@" ;;
  image) cmd_image "$@" ;;
  build) cmd_build "$@" ;;
  up) cmd_up "$@" ;;
  store) cmd_store "$@" ;;
  sqlcheck) cmd_sqlcheck "$@" ;;
  up-dst) cmd_up_dst "$@" ;;
  dst) cmd_dst "$@" ;;
  nodes) cmd_nodes "$@" ;;
  balancer) cmd_balancer "$@" ;;
  load) cmd_load "$@" ;;
  check) cmd_check "$@" ;;
  imap) cmd_imap "$@" ;;
  queue) cmd_queue "$@" ;;
  errors) cmd_errors "$@" ;;
  fault) cmd_fault "$@" ;;
  baseline) cmd_baseline "$@" ;;
  status) cmd_status "$@" ;;
  down) cmd_down "$@" ;;
  *) sed -n '6,65p' "$0"; exit 2 ;;
esac
