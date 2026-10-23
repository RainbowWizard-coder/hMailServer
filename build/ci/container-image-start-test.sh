#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The release's container image started twice, once beside PostgreSQL and once
# beside MariaDB, and each asked what a user asks of it first: does port 25 answer
# SMTP, does the REST API - the administration on Linux - answer over TLS, with the
# version the release is, and does its database hold the schema the server laid out
# in it. What the old GitHub job's smoke test did, and what nothing on GitLab did
# until this; the MariaDB start from the release after 6.3.7, whose image carries
# the MariaDB client the server opens (packaging/docker/Dockerfile).
#
#   bash build/ci/container-image-start-test.sh
#       the container-image-start job of .gitlab/ci/container-image.yml: the runner
#       has started the image twice, by its digest, PostgreSQL and MariaDB as the
#       job's services (aliases mail and mail-mariadb for the image, db and mariadb
#       for the databases, on one network per job), and this asks the two named
#       mail and mail-mariadb, and reads the two databases with the job image's
#       own psql and mariadb
#
#   bash build/ci/container-image-start-test.sh --docker IMAGE [--keep]
#       by hand, on a machine with a Docker daemon (linix, a laptop's WSL): the same
#       four containers started here from IMAGE - a registry's reference, or a local
#       name after `docker load` of the tarball linux-image keeps with
#       HM_KEEP_IMAGE=1 - the same questions asked, the databases read with their
#       own containers' clients, and all four removed at the end (--keep leaves
#       them, and says how to remove them)
#
# The image is started as the service starts it, through its own entrypoint, with
# the command START_COMMAND below: the entrypoint writes the [Database] section and
# the administrator's hash from the environment, waits for the database
# (HM_DB_WAIT_SECONDS), creates and upgrades it and stores the REST listener's four
# settings; then the command secures the new installation as SecureDefaults.md says a
# container is secured - hmailserver --apply-new-install-defaults, which makes a
# self-signed certificate and turns STARTTLS on - and starts the server, whose REST
# listener takes that certificate. So the test needs no certificate from outside,
# which a service container could not be given. Beside MariaDB the image is told
# HM_DB_TYPE=MySQL, and reaches it through MariaDB Connector/C (libmariadb.so.3),
# which the server opens at run time: an image without it stops at the entrypoint's
# --create-database, and port 25 never answers.
#
# Environment (the job sets them; --docker sets its own):
#   HM_START_HOST       the address of the image beside PostgreSQL (mail, the
#                       service's alias)
#   HM_START_MARIADB_HOST
#                       the address of the image beside MariaDB (mail-mariadb)
#   HM_START_DB_HOST, HM_START_MARIADB_DB_HOST
#                       the two databases' addresses, for the job's psql and
#                       mariadb (db and mariadb, the services' aliases)
#   HM_DB_USER, HM_DB_NAME, HM_DB_PASSWORD
#                       the database user, database and password both images were
#                       given (hmailserver, hmailserver)
#   HM_ADMIN_PASSWORD   the administrator password the images were started with
#   HM_START_VERSION    the version the REST API must report (default: the tag's,
#                       CI_COMMIT_TAG, without its v and any suffix)
#   HM_START_WAIT       how long to wait for each image to answer, in seconds (300)
#   HM_START_POSTGRES_IMAGE, HM_START_MARIADB_IMAGE
#                       --docker only: the PostgreSQL and the MariaDB image (the
#                       ones the jobs run, by digest)
#   HM_START_PLATFORM   --docker only: the platform to start the image as
#                       (linux/arm64 for the arm64 image on an x86-64 machine, whose
#                       kernel runs it through qemu-user); the job's is its services'
#                       docker platform (container-image-start-arm64)
#   HM_START_NAMES      --docker only: the name of the network, and the start of the
#                       four containers' names (hm-start-<the script's process id>)
#
# Exit status: 0 when both images answered as they must; 1 when not, or when a database
# client could not ask (the test did not finish, and nothing is yet said of the image);
# 2 when it could not be run. Nothing it prints holds a password, and no password is on
# a command line.
set -uo pipefail

# build/check-release-chain.py holds this file whole, by its SHA-256 (START_TEST_SHA256)
# with the two image pins below taken out, which it holds to the pins of the start jobs'
# services instead: a change here fails that check until the digest there changes with
# it, and a change to that file, a gate file, lands only with build/land-via-mr.py's
# --gates-read, a person's word that the change was read whole.

# The command the image runs, here and in the job's service definitions
# (.gitlab/ci/container-image.yml, container-image-start, services: command);
# build/check-release-chain.py holds them to each other.
START_COMMAND='hmailserver --config /etc/hmailserver/hMailServer.ini --apply-new-install-defaults && exec hmailserver --config /etc/hmailserver/hMailServer.ini --foreground'
# The self-signed pair --apply-new-install-defaults writes under the data directory
# (NewInstallDefaults.cpp), which the REST listener is pointed at.
CERTIFICATE=/var/lib/hmailserver/Certificates/self-signed-certificate.pem
PRIVATE_KEY=/var/lib/hmailserver/Certificates/self-signed-key.pem
POSTGRES_IMAGE="${HM_START_POSTGRES_IMAGE:-postgres:18@sha256:5a5a84b19854a9ffaa54082c166ff4ec27473a361e496e5ea167f298f2da9722}"
# The MariaDB the schema-upgrade gate runs (.gitlab-ci.yml, linux-db-upgrade-mysql)
# and container-image-start's service, by digest.
MARIADB_IMAGE="${HM_START_MARIADB_IMAGE:-mariadb:13@sha256:2fd6d83be13d6f95119fe7823c4a445a771d973ce15a4f4621e3c14898f9f1ab}"

mode=ci
image=""
keep=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --docker) [ "$#" -ge 2 ] || { echo "--docker takes an image"; exit 2; }; mode=docker; image="$2"; shift 2 ;;
    --keep) keep=1; shift ;;
    -h|--help) sed -n '5,71p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1"; exit 2 ;;
  esac
done

fail() {
  echo "CONTAINER-IMAGE-START FAILED: $*"
  # In the job, the image was pushed before it could be started: say what now names it.
  if [ "$mode" = ci ] && [ -n "${HM_IMAGE_TAGS:-}" ]; then
    echo "The image ${HM_IMAGE:-} was pushed before this test, and these tags now name an image that"
    echo "did not start as it must; delete them from the container registry (Deploy > Container registry),"
    echo "with its signature and attestation (sha256-<digest>.sig and .att), before anyone pulls it:"
    printf '%s\n' "$HM_IMAGE_TAGS" | tr ',' '\n' | sed '/^$/d; s/^/  /'
  fi
  exit 1
}

# A question this test could not ask - the database client itself failed (a refused
# login, a certificate it would not take, a database it could not reach or that went
# away during the query) - which says nothing of the image either way: still a
# failure, but not the image's, so the tags are not yet said to be deleted.
unasked() {
  echo "CONTAINER-IMAGE-START FAILED: $*"
  if [ "$mode" = ci ] && [ -n "${HM_IMAGE_TAGS:-}" ]; then
    echo "The image ${HM_IMAGE:-} was pushed before this test, which did not finish: find why the client failed"
    echo "(its own words are above) and run this job again before deciding whether these tags must be deleted:"
    printf '%s\n' "$HM_IMAGE_TAGS" | tr ',' '\n' | sed '/^$/d; s/^/  /'
  fi
  exit 1
}

step() {
  echo ""
  echo "== $*"
}

command -v curl > /dev/null || { echo "curl is needed"; exit 2; }
command -v timeout > /dev/null || { echo "timeout (coreutils) is needed"; exit 2; }

expected="${HM_START_VERSION:-}"
if [ -z "$expected" ]; then
  tag="${CI_COMMIT_TAG:-}"
  expected="${tag#v}"
fi
# The server reports Version.h's number; a pre-release's tag has a suffix it does not.
expected="${expected%%[!0-9.]*}"
wait_seconds="${HM_START_WAIT:-300}"
pg_host="${HM_START_HOST:-mail}"
maria_host="${HM_START_MARIADB_HOST:-mail-mariadb}"
pg_db_host="${HM_START_DB_HOST:-db}"
maria_db_host="${HM_START_MARIADB_DB_HOST:-mariadb}"
db_user="${HM_DB_USER:-hmailserver}"
db_name="${HM_DB_NAME:-hmailserver}"
db_password="${HM_DB_PASSWORD:-}"
password="${HM_ADMIN_PASSWORD:-}"
# Each image's ports as this script reaches them: the services' own in the job, the
# ports published on the loopback with --docker.
pg_smtp=25
pg_rest=8045
maria_smtp=25
maria_rest=8045

names=""
work=""
cleanup() {
  code=$?
  if [ "$mode" = docker ] && [ -n "$names" ]; then
    if [ "$code" -ne 0 ]; then
      for container in "$names-mail" "$names-mail-mariadb"; do
        echo ""
        echo "== $container's log (the last 80 lines)"
        docker logs "$container" 2>&1 | tail -n 80
      done
    fi
    if [ "$keep" = 1 ]; then
      echo "Left running (--keep): docker rm -f $names-mail $names-mail-mariadb $names-db $names-mariadb; docker network rm $names"
    else
      docker rm -f "$names-mail" "$names-mail-mariadb" "$names-db" "$names-mariadb" > /dev/null 2>&1
      docker network rm "$names" > /dev/null 2>&1
    fi
  fi
  [ -n "$work" ] && rm -rf "$work"
  exit "$code"
}
trap cleanup EXIT

if [ "$mode" = docker ]; then
  command -v docker > /dev/null || { echo "--docker needs docker"; exit 2; }
  [ -n "$expected" ] || { echo "--docker needs HM_START_VERSION, the version the image is"; exit 2; }
  names="${HM_START_NAMES:-hm-start-$$}"
  work=$(mktemp -d)
  # A throwaway password, in files of the job's own: never on a command line.
  password=$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')
  db_password="$password"
  db_user=hmailserver
  db_name=hmailserver
  pg_db_host="$names-db"
  maria_db_host="$names-mariadb"
  ( umask 077
    printf 'POSTGRES_USER=hmailserver\nPOSTGRES_DB=hmailserver\nPOSTGRES_PASSWORD=%s\n' "$password" > "$work/db.env"
    printf 'MARIADB_USER=hmailserver\nMARIADB_DATABASE=hmailserver\nMARIADB_PASSWORD=%s\nMARIADB_ALLOW_EMPTY_ROOT_PASSWORD=1\n' \
      "$password" > "$work/mariadb.env"
    printf '%s\n' HM_DB_TYPE=PostgreSQL HM_DB_HOST=db HM_DB_NAME=hmailserver HM_DB_USER=hmailserver \
      "HM_DB_PASSWORD=$password" HM_DB_WAIT_SECONDS=120 "HM_ADMIN_PASSWORD=$password" \
      HM_REST_PORT=8045 HM_REST_BIND_ADDRESS=0.0.0.0 \
      "HM_REST_CERTIFICATE_FILE=$CERTIFICATE" "HM_REST_PRIVATE_KEY_FILE=$PRIVATE_KEY" > "$work/mail.env"
    sed -e 's/^HM_DB_TYPE=.*/HM_DB_TYPE=MySQL/' -e 's/^HM_DB_HOST=.*/HM_DB_HOST=mariadb/' "$work/mail.env" > "$work/mail-mariadb.env" )
  step "starting PostgreSQL, MariaDB and $image beside each"
  docker network create "$names" > /dev/null || { echo "docker network create failed"; exit 2; }
  docker run -d --name "$names-db" --network "$names" --network-alias db --env-file "$work/db.env" "$POSTGRES_IMAGE" > /dev/null \
    || { echo "PostgreSQL did not start"; exit 2; }
  docker run -d --name "$names-mariadb" --network "$names" --network-alias mariadb --env-file "$work/mariadb.env" "$MARIADB_IMAGE" \
    > /dev/null || { echo "MariaDB did not start"; exit 2; }
  platform=()
  [ -n "${HM_START_PLATFORM:-}" ] && platform=(--platform "$HM_START_PLATFORM")
  docker run -d --name "$names-mail" --network "$names" --network-alias mail --env-file "$work/mail.env" \
    "${platform[@]}" -p 127.0.0.1::25 -p 127.0.0.1::8045 "$image" bash -c "$START_COMMAND" > /dev/null \
    || { echo "$image did not start"; exit 2; }
  docker run -d --name "$names-mail-mariadb" --network "$names" --network-alias mail-mariadb --env-file "$work/mail-mariadb.env" \
    "${platform[@]}" -p 127.0.0.1::25 -p 127.0.0.1::8045 "$image" bash -c "$START_COMMAND" > /dev/null \
    || { echo "$image did not start beside MariaDB"; exit 2; }
  echo "the image runs as $(docker exec "$names-mail" uname -m) ($(docker image inspect -f '{{.Os}}/{{.Architecture}}' "$image"))"
  pg_host=127.0.0.1
  maria_host=127.0.0.1
  pg_smtp=$(docker port "$names-mail" 25/tcp | head -n 1 | sed 's/.*://')
  pg_rest=$(docker port "$names-mail" 8045/tcp | head -n 1 | sed 's/.*://')
  maria_smtp=$(docker port "$names-mail-mariadb" 25/tcp | head -n 1 | sed 's/.*://')
  maria_rest=$(docker port "$names-mail-mariadb" 8045/tcp | head -n 1 | sed 's/.*://')
  echo "beside PostgreSQL the image's port 25 is 127.0.0.1:$pg_smtp and its 8045 127.0.0.1:$pg_rest"
  echo "beside MariaDB the image's port 25 is 127.0.0.1:$maria_smtp and its 8045 127.0.0.1:$maria_rest"
else
  command -v psql > /dev/null || { echo "psql (the PostgreSQL client) is needed"; exit 2; }
  command -v mariadb > /dev/null || { echo "mariadb (the MariaDB client) is needed"; exit 2; }
fi

[ -n "$password" ] || { echo "HM_ADMIN_PASSWORD, the password the image was started with, is not set"; exit 2; }
[ -n "$db_password" ] || { echo "HM_DB_PASSWORD, the database password the image was started with, is not set"; exit 2; }
[ -n "$expected" ] || { echo "no version to expect: set HM_START_VERSION, or run in a v* tag's pipeline"; exit 2; }

# Whether the image's container still runs (with --docker; the job cannot tell).
still_running() {
  [ "$mode" != docker ] && return 0
  [ "$(docker inspect -f '{{.State.Running}}' "$1" 2> /dev/null)" = true ]
}

# One SMTP conversation on fd 3 with host $1, port $2: the banner, EHLO and its whole
# answer, QUIT. Prints what the server said.
smtp_conversation() {
  exec 3<> "/dev/tcp/$1/$2" || return 1
  local line
  IFS= read -r -t 15 line <&3 || return 1
  line="${line%$'\r'}"
  printf 'S: %s\n' "$line"
  case "$line" in 220[\ -]*) ;; *) exec 3>&-; return 1 ;; esac
  # A banner of more than one line ends at its "220 " line.
  while [ "${line:3:1}" = "-" ]; do
    IFS= read -r -t 15 line <&3 || return 1
    line="${line%$'\r'}"
    printf 'S: %s\n' "$line"
  done
  printf 'EHLO start-test.invalid\r\n' >&3
  printf 'C: EHLO start-test.invalid\n'
  while :; do
    IFS= read -r -t 15 line <&3 || return 1
    line="${line%$'\r'}"
    printf 'S: %s\n' "$line"
    case "$line" in
      250-*) ;;
      250\ *) break ;;
      *) exec 3>&-; return 1 ;;
    esac
  done
  printf 'QUIT\r\n' >&3
  IFS= read -r -t 15 line <&3 && printf 'S: %s\n' "${line%$'\r'}"
  exec 3>&-
  return 0
}

# The schema version the server wrote into its database (hm_dbversion), read with
# that database's own client: $1 PostgreSQL or MariaDB. The password goes in the
# client's environment, never on its command line.
schema_version() {
  if [ "$mode" = docker ]; then
    case "$1" in
      PostgreSQL) printf '%s' "$db_password" | docker exec -i "$pg_db_host" \
                    bash -c 'PGPASSWORD=$(cat) exec psql -h 127.0.0.1 -U "$0" -d "$1" -v VERBOSITY=verbose -tAc "select value from hm_dbversion"' \
                    "$db_user" "$db_name" ;;
      MariaDB) printf '%s' "$db_password" | docker exec -i "$maria_db_host" \
                 bash -c 'MYSQL_PWD=$(cat) exec mariadb -h 127.0.0.1 -u "$0" -N -B -e "select value from hm_dbversion" "$1"' \
                 "$db_user" "$db_name" ;;
    esac
  else
    case "$1" in
      PostgreSQL) PGPASSWORD="$db_password" psql -h "$pg_db_host" -U "$db_user" -d "$db_name" -v VERBOSITY=verbose -tAc "select value from hm_dbversion" ;;
      MariaDB) MYSQL_PWD="$db_password" mariadb -h "$maria_db_host" -u "$db_user" -N -B -e "select value from hm_dbversion" "$db_name" ;;
    esac
  fi
}

# The questions, asked of the image beside one database: $1 the database (PostgreSQL
# or MariaDB), $2 the image's address, $3 its SMTP port, $4 its REST port, $5 its
# container (with --docker). Prints the schema version last, as "schema=<n>".
ask_image() {
  local database="$1" host="$2" smtp_port="$3" rest_port="$4" container="${5:-}"
  local deadline=$((SECONDS + wait_seconds)) said="" url code body plain version

  step "beside $database: port 25 answers SMTP (waiting up to $wait_seconds s)"
  until said="$(smtp_conversation "$host" "$smtp_port" 2> /dev/null)"; do
    still_running "$container" || fail "the image beside $database stopped before port 25 answered"
    [ "$SECONDS" -lt "$deadline" ] || fail "port 25 of $host, the image beside $database, did not answer SMTP within $wait_seconds s"
    sleep 3
  done
  printf '%s\n' "$said"
  # A here-string, not printf into grep -q: grep stops at the match, and the shell's
  # printf, still writing the lines after it, can die of SIGPIPE, which pipefail makes
  # the status - a STARTTLS offered and reported missing.
  grep -Eq '^S: 250[ -]STARTTLS' <<< "$said" \
    || fail "port 25 of the image beside $database offers no STARTTLS: the new installation's defaults were not applied"
  echo "SMTP answers, with STARTTLS, as a new installation secured by --apply-new-install-defaults does."

  step "beside $database: the REST API answers over TLS"
  url="https://$host:$rest_port/api/v1/status"
  code=000
  until [ "$code" != 000 ]; do
    # -k: the certificate is the self-signed one the image made for itself.
    code=$(curl -sk -o /dev/null -w '%{http_code}' --max-time 10 "$url" || true)
    [ "$code" != 000 ] && break
    still_running "$container" || fail "the image beside $database stopped before the REST API answered"
    [ "$SECONDS" -lt "$deadline" ] || fail "the REST API at $url, the image beside $database, did not answer within $wait_seconds s"
    sleep 3
  done
  [ "$code" = 401 ] || fail "beside $database, GET /api/v1/status without credentials answered $code, not 401"
  echo "without credentials: 401"
  # The password goes to curl on its standard input (a config line), never on its command line.
  body=$(printf 'user = "Administrator:%s"\n' "$password" | curl -sk --max-time 10 -K - "$url") \
    || fail "beside $database, GET /api/v1/status as Administrator failed"
  # Its first 400 characters, cut by the shell: head -c stops reading and leaves
  # printf to die of SIGPIPE.
  printf '%s\n' "${body:0:400}"
  echo ""
  case "$body" in
    *"\"version\":\"$expected"*) ;;
    *) fail "beside $database, the REST API does not report version $expected" ;;
  esac
  plain=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://$host:$rest_port/api/v1/status" || true)
  [ "$plain" != 200 ] || fail "beside $database, the REST API answered 200 over plain HTTP off loopback"
  echo "as Administrator: 200, version $expected; over plain HTTP: no answer ($plain)"

  step "beside $database: the database holds the server's schema"
  # What the client printed and what it said went wrong, apart: a warning on its
  # standard error is not read as the version, and its words are shown when it fails.
  local said_errors errors client status
  said_errors=$(mktemp) || { echo "mktemp failed"; exit 2; }
  version=$(schema_version "$database" 2> "$said_errors")
  status=$?
  errors=$(cat "$said_errors")
  rm -f "$said_errors"
  version=$(printf '%s' "$version" | tr -d ' \r\n')
  [ "$database" = PostgreSQL ] && client=psql || client=mariadb
  if [ "$status" -ne 0 ]; then
    # The database answered that there is no hm_dbversion table (PostgreSQL's
    # SQLSTATE 42P01, which VERBOSITY verbose prints; MariaDB's error 1146): the
    # client asked, and the server laid out no schema there.
    if grep -Eq '(^|[^0-9A-Z])42P01([^0-9A-Z]|$)|^ERROR 1146 \(42S02\)' <<< "$errors"; then
      printf '%s\n' "$errors"
      fail "$database holds no hm_dbversion table: the server did not lay out its schema there"
    fi
    # The database answering that an object the query names is missing or unusable -
    # an error of SQLSTATE class 42, as psql's "ERROR:  42<...>: " or MariaDB's
    # "ERROR <n> (42<...>) at line <n>: ", such as a column that is not there: the
    # client asked, and the schema is not the one this test reads. Any other error -
    # a login refused, a server not reached, the connection lost during the query
    # (MariaDB's client prints its own errors, such as 2013 or 2026, in the same
    # form, with SQLSTATE HY000), the server shutting down (57P01, 08S01) - says
    # nothing of the schema.
    if grep -Eq '^ERROR:  42[0-9A-Z]{3}: |^ERROR [0-9]+ \(42[0-9A-Z]{3}\) at line [0-9]+: ' <<< "$errors"; then
      printf '%s\n' "$errors"
      fail "$database answered the schema query with an error (above): the schema the server laid out there is not the one this test reads"
    fi
    printf '%s\n' "$errors"
    unasked "$client could not ask $database for its hm_dbversion (it exited $status; what it said is above): the test could not finish, which says nothing yet of the image"
  fi
  case "$version" in
    ''|*[!0-9]*) fail "$database's hm_dbversion holds no number ('$version'): the server did not lay out its schema there" ;;
  esac
  echo "hm_dbversion in $database: $version"
  schema="$version"
}

schema=""
ask_image PostgreSQL "$pg_host" "$pg_smtp" "$pg_rest" "${names:+$names-mail}"
pg_schema="$schema"
ask_image MariaDB "$maria_host" "$maria_smtp" "$maria_rest" "${names:+$names-mail-mariadb}"
maria_schema="$schema"
[ "$pg_schema" = "$maria_schema" ] \
  || fail "the server laid out schema $pg_schema in PostgreSQL and $maria_schema in MariaDB: one image, one schema"

echo ""
echo "STARTED: beside PostgreSQL and beside MariaDB, the image answers SMTP on 25 with STARTTLS, and the REST API over TLS, as $expected, with schema $pg_schema in each database."
