#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# One node of the active-active lab: the Linux server of a tree, on the lab's shared
# database, in a container of its own (aa-lab.sh nodes starts one per node). Run as
# root inside the lab's node image, with the tree at /src (read-only) and the server
# built from it at /build/hmailserver (read-only).
#
# The first node is the one that lays the database out (AA_LAYOUT=1): it drops and
# makes the database with the server's own --create-database, moves the listeners
# off the privileged ports, turns auto-ban off, trusts the lab's front network for
# the PROXY protocol, and - once it serves - makes the domain example.test, its
# accounts, and the route that sends sink.test to the lab's sink. Every other node
# waits until that database is there and joins it as it is: nothing tells it that
# another node is running, which is the point - today the server does not know.
#
# What it reads (aa-lab.sh sets all of them):
#   AA_NODE              this node's name, also its host name
#   AA_DB                postgresql, mysql or mariadb (the Linux server has no SQL
#                        Server backend, HM6390)
#   AA_DB_HOST, AA_DB_PORT, AA_DB_USER, AA_DB_NAME
#   AA_DB_PASSWORD       the database user's password (the environment, by name)
#   AA_ADMIN_PASSWORD    the server's administrator password
#   AA_ACCOUNT_PASSWORD  the password of the accounts the layout makes
#   AA_LAYOUT            1 on the node that lays the database out
#   AA_DATA              the data directory: the node's own (the default,
#                        /aa/tree/Data) or a volume every node mounts (the shared
#                        variant, which docs/WarmStandby.md says not to build)
#   AA_FRONT_SUBNET      the front network, trusted for the PROXY protocol on SMTP
#   AA_SINK_HOST         the sink's name on the front network
#   AA_ACCOUNTS          how many accountN@example.test to make (default 4)
set -uo pipefail

T=/aa/tree
DATA="${AA_DATA:-$T/Data}"
NODE="${AA_NODE:-node}"
case "${AA_DB:-}" in
  mysql|mariadb) TYPE=MySQL ;;
  postgresql) TYPE=PostgreSQL ;;
  *) echo "REFUSED: AA_DB is '${AA_DB:-}'; it must be postgresql, mysql or mariadb."; exit 2 ;;
esac
REST=8045

sql() {
  local database="$1"
  case "$AA_DB" in
    postgresql) PGPASSWORD="$AA_DB_PASSWORD" psql -h "$AA_DB_HOST" -p "$AA_DB_PORT" -U "$AA_DB_USER" -d "${database:-postgres}" -v ON_ERROR_STOP=1 -At -q ;;
    *) MYSQL_PWD="$AA_DB_PASSWORD" mariadb -h "$AA_DB_HOST" -P "$AA_DB_PORT" -u "$AA_DB_USER" --skip-ssl-verify-server-cert -N -B ${database:+"$database"} ;; # nosemgrep
  esac
}

rest() {
  # rest METHOD PATH [BODY]: the administrator's credentials from a netrc file, so
  # that the password is on no command line.
  local method="$1" path="$2" body="${3:-}"
  if [ -n "$body" ]; then
    printf '%s' "$body" | curl -s -o /dev/null -w '%{http_code}' --netrc-file "$T/netrc" -X "$method" \
      -H 'Content-Type: application/json' --data-binary @- "http://127.0.0.1:$REST$path"
  else
    curl -s -o /dev/null -w '%{http_code}' --netrc-file "$T/netrc" -X "$method" "http://127.0.0.1:$REST$path"
  fi
}

rm -rf "$T"
mkdir -p "$T/program" "$T/Logs" "$T/Temp" "$T/Events" "$T/Database" "$DATA"
cp /build/hmailserver "$T/program/hmailserver"
chmod +x "$T/program/hmailserver"
cp -r /src/hmailserver/source/DBScripts "$T/program/DBScripts"
cp /src/hmailserver/installation/Extras/dh2048.pem "$T/program/"
umask 077
printf 'machine 127.0.0.1 login Administrator password %s\n' "$AA_ADMIN_PASSWORD" > "$T/netrc"
cat > "$T/program/hMailServer.ini" <<INI
[Directories]
ProgramFolder=$T/program
DatabaseFolder=$T/Database
DataFolder=$DATA
LogFolder=$T/Logs
TempFolder=$T/Temp
EventFolder=$T/Events
[Database]
Type=$TYPE
Server=$AA_DB_HOST
Port=$AA_DB_PORT
Username=$AA_DB_USER
Password=$AA_DB_PASSWORD
Database=$AA_DB_NAME
Internal=0
[Settings]
ProtectStoredSecretsWithDPAPI=0
RestApiPort=$REST
RestApiBindAddress=127.0.0.1
WindowsEventLogEnabled=0
SMTPProxyProtocolEnabled=1
SMTPProxyProtocolTrustedIPs=${AA_FRONT_SUBNET:-}
INI
umask 022
server=("$T/program/hmailserver" --config "$T/program/hMailServer.ini")

if [ "${AA_LAYOUT:-0}" = 1 ]; then
  echo "== $NODE: laying out $AA_DB_NAME on $AA_DB at $AA_DB_HOST:$AA_DB_PORT"
  for _ in $(seq 1 60); do echo "select 1;" | sql "" > /dev/null 2>&1 && break; sleep 2; done
  case "$AA_DB" in
    postgresql) echo "drop database if exists \"$AA_DB_NAME\" with (force);" | sql "" ;;
    *) echo "drop database if exists \`$AA_DB_NAME\`;" | sql "" ;;
  esac || { echo "REFUSED: $AA_DB_NAME could not be dropped"; exit 2; }
  "${server[@]}" --create-database > "$T/Logs/create-database.log" 2>&1 || { cat "$T/Logs/create-database.log"; exit 3; }
  sql "$AA_DB_NAME" <<'SQL' || { echo "LAYOUT FAILED"; exit 3; }
update hm_tcpipports set portnumber = 2525 where portnumber = 25;
update hm_tcpipports set portnumber = 1110 where portnumber = 110;
update hm_tcpipports set portnumber = 1143 where portnumber = 143;
update hm_settings set settinginteger = 0 where settingname = 'AutoBanOnLogonFailureEnabled';
SQL
else
  echo "== $NODE: waiting for the database the first node lays out"
  ready=""
  for _ in $(seq 1 180); do
    if [ -f /aa/run/layout-done ] && echo "select value from hm_dbversion;" | sql "$AA_DB_NAME" > /dev/null 2>&1; then
      ready=yes
      break
    fi
    sleep 1
  done
  [ -n "$ready" ] || { echo "REFUSED: the first node never finished the layout"; exit 2; }
fi
printf '%s\n' "$AA_ADMIN_PASSWORD" | "${server[@]}" --set-admin-password > /dev/null || { echo "REFUSED: --set-admin-password failed"; exit 2; }

echo "== $NODE: start"
( cd "$T" || exit 1; exec "${server[@]}" --foreground > "$T/Logs/stdout.log" 2>&1 ) &
pid=$!
trap 'kill "$pid" 2> /dev/null; wait "$pid"; exit 0' TERM INT
up=""
for _ in $(seq 1 90); do
  if [ "$(rest GET /api/v1/status)" = 200 ]; then up=yes; break; fi
  kill -0 "$pid" 2> /dev/null || break
  sleep 1
done
[ -n "$up" ] || { echo "REFUSED: $NODE did not answer on its REST port"; tail -n 40 "$T/Logs/stdout.log"; exit 2; }

if [ "${AA_LAYOUT:-0}" = 1 ]; then
  made=$(rest POST /api/v1/domains '{"name":"example.test","active":true}')
  echo "domain example.test: HTTP $made"
  for i in $(seq 1 "${AA_ACCOUNTS:-4}"); do
    body=$(printf '{"address":"account%d@example.test","password":"%s"}' "$i" "$AA_ACCOUNT_PASSWORD")
    echo "account$i@example.test: HTTP $(rest POST /api/v1/domains/example.test/accounts "$body")"
  done
  route=$(printf '{"domain_name":"sink.test","target_smtp_host":"%s","target_smtp_port":2525,"number_of_tries":5,"minutes_between_try":1,"treat_recipient_as_local_domain":true}' "${AA_SINK_HOST:-sink}")
  echo "route sink.test: HTTP $(rest POST /api/v1/routes "$route")"
  touch /aa/run/layout-done
fi
echo "SERVING: $NODE, $AA_DB database $AA_DB_NAME, data $DATA, pid $pid"
wait "$pid"
echo "$NODE: the server stopped by itself"
exit 1
