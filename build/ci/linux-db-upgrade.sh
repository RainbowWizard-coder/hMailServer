#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-db-upgrade-pgsql and linux-db-upgrade-mysql jobs of .gitlab-ci.yml:
# build/db-upgrade-gate.sh - a database at this build's schema, wound back to
# 6029, seeded with the orphaned rows that exposed the 6029 -> 6030 ordering
# defect, then --upgrade-database as the installer runs it - with the server the
# linux-build job made, against the job's database service. It is the old GitHub
# workflow linux-build.yml's db-upgrade job; the gate script is unchanged.
#
#   bash build/ci/linux-db-upgrade.sh pgsql    beside a "postgres" service
#   bash build/ci/linux-db-upgrade.sh mysql    beside a "mariadb" service
#
# Run in the image of build/ci/linux.Dockerfile, which has psql, the mariadb
# client and libmariadb.so.3 (the library the server loads for MySQL). The
# environment it reads, all with the services' defaults:
#   HM_SERVER_BINARY   the server (linux-build/hmailserver)
#   DB_HOST, DB_USER, DB_PASSWORD
set -uo pipefail

backend="${1:-}"
binary="${HM_SERVER_BINARY:-$PWD/linux-build/hmailserver}"

case "$backend" in
  pgsql)
    export DB_HOST="${DB_HOST:-postgres}" DB_USER="${DB_USER:-hmailserver}" DB_PASSWORD="${DB_PASSWORD:-hmtest}"
    ready() { PGPASSWORD="$DB_PASSWORD" pg_isready -q -h "$DB_HOST" -U "$DB_USER" -d postgres; }
    ;;
  mysql)
    export DB_HOST="${DB_HOST:-mariadb}" DB_USER="${DB_USER:-root}" DB_PASSWORD="${DB_PASSWORD:-hmtest}"
    ready() { mariadb-admin --host="$DB_HOST" --user="$DB_USER" --password="$DB_PASSWORD" ping > /dev/null 2>&1; }
    ;;
  *)
    echo "usage: $0 pgsql|mysql"
    exit 64
    ;;
esac

if [ ! -f "$binary" ]; then
  echo "REFUSED: no server binary at $binary - the linux-build job's artifact is missing."
  exit 2
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$binary" "$work/hmailserver"
chmod +x "$work/hmailserver"

# GitLab starts the service and waits for its port, not for the database behind
# it: MariaDB listens a few seconds before it accepts a login.
up=""
for _ in $(seq 1 90); do
  if ready; then
    up=yes
    break
  fi
  sleep 1
done
if [ -z "$up" ]; then
  echo "REFUSED: the $backend service at $DB_HOST did not accept $DB_USER in 90 seconds."
  exit 2
fi

HM_BIN="$work/hmailserver" HM_SCRIPTS="$PWD/hmailserver/source/DBScripts" bash build/db-upgrade-gate.sh "$backend"
