#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The release's container image started beside PostgreSQL and asked what a user asks
# of it first: does port 25 answer SMTP, and does the REST API - the administration
# on Linux - answer over TLS, with the version the release is. What the old GitHub
# job's smoke test did, and what nothing on GitLab did until this.
#
#   bash build/ci/container-image-start-test.sh
#       the container-image-start job of .gitlab/ci/container-image.yml: the runner
#       has started the image, by its digest, and PostgreSQL as the job's services
#       (aliases mail and db, on one network per job), and this asks the one named
#       mail
#
#   bash build/ci/container-image-start-test.sh --docker IMAGE [--keep]
#       by hand, on a machine with a Docker daemon (linix, a laptop's WSL): the same
#       two containers started here from IMAGE - a registry's reference, or a local
#       name after `docker load` of the tarball linux-image keeps with
#       HM_KEEP_IMAGE=1 - the same questions asked, and both removed at the end
#       (--keep leaves them, and says how to remove them)
#
# The image is started as the service starts it, through its own entrypoint, with
# the command START_COMMAND below: the entrypoint writes the [Database] section and
# the administrator's hash from the environment, waits for the database
# (HM_DB_WAIT_SECONDS), creates and upgrades it and stores the REST listener's four
# settings; then the command secures the new installation as SecureDefaults.md says a
# container is secured - hmailserver --apply-new-install-defaults, which makes a
# self-signed certificate and turns STARTTLS on - and starts the server, whose REST
# listener takes that certificate. So the test needs no certificate from outside,
# which a service container could not be given.
#
# Environment (the job sets them; --docker sets its own):
#   HM_START_HOST       the image's address (mail, the service's alias)
#   HM_ADMIN_PASSWORD   the administrator password the image was started with
#   HM_START_VERSION    the version the REST API must report (default: the tag's,
#                       CI_COMMIT_TAG, without its v and any suffix)
#   HM_START_WAIT       how long to wait for the image to answer, in seconds (300)
#   HM_START_POSTGRES_IMAGE   --docker only: the PostgreSQL image (the one the jobs
#                       run, by digest)
#   HM_START_PLATFORM   --docker only: the platform to start the image as
#                       (linux/arm64 for the arm64 image on an x86-64 machine, whose
#                       kernel runs it through qemu-user); the job's is its service's
#                       docker platform (container-image-start-arm64)
#
# Exit status: 0 when both answered as they must, 1 when not, 2 when it could not
# be run. Nothing it prints holds a password.
set -uo pipefail

# The command the image runs, here and in the job's service definition
# (.gitlab/ci/container-image.yml, container-image-start, services: command);
# build/check-release-chain.py holds the two to each other.
START_COMMAND='hmailserver --config /etc/hmailserver/hMailServer.ini --apply-new-install-defaults && exec hmailserver --config /etc/hmailserver/hMailServer.ini --foreground'
# The self-signed pair --apply-new-install-defaults writes under the data directory
# (NewInstallDefaults.cpp), which the REST listener is pointed at.
CERTIFICATE=/var/lib/hmailserver/Certificates/self-signed-certificate.pem
PRIVATE_KEY=/var/lib/hmailserver/Certificates/self-signed-key.pem
POSTGRES_IMAGE="${HM_START_POSTGRES_IMAGE:-postgres:16@sha256:f1c3376c26f2609ab9f29f71f824103fe2fcd8ee0346485cb6122a4f93df6f94}"

mode=ci
image=""
keep=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --docker) [ "$#" -ge 2 ] || { echo "--docker takes an image"; exit 2; }; mode=docker; image="$2"; shift 2 ;;
    --keep) keep=1; shift ;;
    -h|--help) sed -n '5,48p' "$0"; exit 0 ;;
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
host="${HM_START_HOST:-mail}"
smtp_port=25
rest_port=8045
password="${HM_ADMIN_PASSWORD:-}"

names=""
work=""
cleanup() {
  code=$?
  if [ "$mode" = docker ] && [ -n "$names" ]; then
    if [ "$code" -ne 0 ]; then
      echo ""
      echo "== the image's log (the last 80 lines)"
      docker logs "$names-mail" 2>&1 | tail -n 80
    fi
    if [ "$keep" = 1 ]; then
      echo "Left running (--keep): docker rm -f $names-mail $names-db; docker network rm $names"
    else
      docker rm -f "$names-mail" "$names-db" > /dev/null 2>&1
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
  names="hm-start-$$"
  work=$(mktemp -d)
  # A throwaway password, in files of the job's own: never on a command line.
  password=$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')
  ( umask 077
    printf 'POSTGRES_USER=hmailserver\nPOSTGRES_DB=hmailserver\nPOSTGRES_PASSWORD=%s\n' "$password" > "$work/db.env"
    printf '%s\n' HM_DB_TYPE=PostgreSQL HM_DB_HOST=db HM_DB_NAME=hmailserver HM_DB_USER=hmailserver \
      "HM_DB_PASSWORD=$password" HM_DB_WAIT_SECONDS=120 "HM_ADMIN_PASSWORD=$password" \
      HM_REST_PORT=8045 HM_REST_BIND_ADDRESS=0.0.0.0 \
      "HM_REST_CERTIFICATE_FILE=$CERTIFICATE" "HM_REST_PRIVATE_KEY_FILE=$PRIVATE_KEY" > "$work/mail.env" )
  step "starting PostgreSQL and $image"
  docker network create "$names" > /dev/null || { echo "docker network create failed"; exit 2; }
  docker run -d --name "$names-db" --network "$names" --network-alias db --env-file "$work/db.env" "$POSTGRES_IMAGE" > /dev/null \
    || { echo "PostgreSQL did not start"; exit 2; }
  platform=()
  [ -n "${HM_START_PLATFORM:-}" ] && platform=(--platform "$HM_START_PLATFORM")
  docker run -d --name "$names-mail" --network "$names" --network-alias mail --env-file "$work/mail.env" \
    "${platform[@]}" -p 127.0.0.1::25 -p 127.0.0.1::8045 "$image" bash -c "$START_COMMAND" > /dev/null \
    || { echo "$image did not start"; exit 2; }
  echo "the image runs as $(docker exec "$names-mail" uname -m) ($(docker image inspect -f '{{.Os}}/{{.Architecture}}' "$image"))"
  host=127.0.0.1
  smtp_port=$(docker port "$names-mail" 25/tcp | head -n 1 | sed 's/.*://')
  rest_port=$(docker port "$names-mail" 8045/tcp | head -n 1 | sed 's/.*://')
  echo "the image's port 25 is 127.0.0.1:$smtp_port and its 8045 127.0.0.1:$rest_port"
fi

[ -n "$password" ] || { echo "HM_ADMIN_PASSWORD, the password the image was started with, is not set"; exit 2; }
[ -n "$expected" ] || { echo "no version to expect: set HM_START_VERSION, or run in a v* tag's pipeline"; exit 2; }

still_running() {
  [ "$mode" != docker ] && return 0
  [ "$(docker inspect -f '{{.State.Running}}' "$names-mail" 2> /dev/null)" = true ]
}

# One SMTP conversation on fd 3: the banner, EHLO and its whole answer, QUIT. Prints
# what the server said.
smtp_conversation() {
  exec 3<> "/dev/tcp/$host/$smtp_port" || return 1
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

deadline=$((SECONDS + wait_seconds))

step "port 25 answers SMTP (waiting up to $wait_seconds s)"
said=""
until said="$(smtp_conversation 2> /dev/null)"; do
  still_running || fail "the image stopped before port 25 answered"
  [ "$SECONDS" -lt "$deadline" ] || fail "port 25 of $host did not answer SMTP within $wait_seconds s"
  sleep 3
done
printf '%s\n' "$said"
# A here-string, not printf into grep -q: grep stops at the match, and the shell's
# printf, still writing the lines after it, can die of SIGPIPE, which pipefail makes
# the status - a STARTTLS offered and reported missing.
grep -Eq '^S: 250[ -]STARTTLS' <<< "$said" \
  || fail "port 25 offers no STARTTLS: the new installation's defaults were not applied"
echo "SMTP answers, with STARTTLS, as a new installation secured by --apply-new-install-defaults does."

step "the REST API answers over TLS"
url="https://$host:$rest_port/api/v1/status"
code=000
until [ "$code" != 000 ]; do
  # -k: the certificate is the self-signed one the image made for itself.
  code=$(curl -sk -o /dev/null -w '%{http_code}' --max-time 10 "$url" || true)
  [ "$code" != 000 ] && break
  still_running || fail "the image stopped before the REST API answered"
  [ "$SECONDS" -lt "$deadline" ] || fail "the REST API at $url did not answer within $wait_seconds s"
  sleep 3
done
[ "$code" = 401 ] || fail "GET /api/v1/status without credentials answered $code, not 401"
echo "without credentials: 401"
# The password goes to curl on its standard input (a config line), never on its command line.
body=$(printf 'user = "Administrator:%s"\n' "$password" | curl -sk --max-time 10 -K - "$url") \
  || fail "GET /api/v1/status as Administrator failed"
# Its first 400 characters, cut by the shell: head -c stops reading and leaves
# printf to die of SIGPIPE.
printf '%s\n' "${body:0:400}"
echo ""
case "$body" in
  *"\"version\":\"$expected"*) ;;
  *) fail "the REST API does not report version $expected" ;;
esac
plain=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://$host:$rest_port/api/v1/status" || true)
[ "$plain" != 200 ] || fail "the REST API answered 200 over plain HTTP off loopback"
echo "as Administrator: 200, version $expected; over plain HTTP: no answer ($plain)"

echo ""
echo "STARTED: the image answers SMTP on 25 with STARTTLS, and the REST API on $rest_port over TLS, as $expected."
