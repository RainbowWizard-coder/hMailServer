#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The hMailServer under test against two other people's servers: Postfix and Dovecot,
# the lab's peers (build/lab/docker-compose.yml, profile peers). Runs INSIDE the
# served bench container (lab.sh serve, then lab.sh interop), where the server's
# listeners are on the loopback and the peers are postfix and dovecot on the lab
# network:
#
#   1  out    this server -> Postfix -> Dovecot: an authenticated submission here
#             to peer1@peer.test, relayed by a route to Postfix, delivered by
#             Postfix to Dovecot over LMTP, read back from Dovecot over IMAP
#   2  in     Postfix -> this server: a message handed to Postfix for
#             lab2@example.test, which Postfix's transport sends here, read back
#             from this server over IMAP
#   3  sync   Dovecot -> this server: three messages in a Dovecot mailbox copied
#             into a mailbox here by mbsync (isync, from Ubuntu's archive), both
#             sides IMAP, then counted here
#   3b sync   the same, with mbsync sending this server one command at a time
#   4  sync   this server -> Dovecot: the same the other way, this server the source
#
#   bash interop.sh                      the four checks
#   bash interop.sh accounts <local>...  make those accounts at example.test afresh
#                                        (lab.sh imaptest uses this)
#   bash interop.sh drop <local>...      delete them
#
# Passwords come from the environment docker exec passes through by name
# (HMLAB_BENCH_ACCOUNT_PASSWORD for this server's accounts, HMLAB_PEER_PASSWORD for
# Dovecot's) and reach curl and mbsync through files and PassCmd, never a command
# line: a process in a container is in the host's process list.
set -uo pipefail

REST=http://127.0.0.1:8045
ADMIN="Administrator:${HMTEST_ADMIN_PASSWORD:-testar}"
PW="${HMLAB_BENCH_ACCOUNT_PASSWORD:-}"
PEER_PW="${HMLAB_PEER_PASSWORD:-}"
WORK=$(mktemp -d)
chmod 700 "$WORK"
trap 'rm -rf "$WORK"' EXIT
failed=0

[ -n "$PW" ] || { echo "HMLAB_BENCH_ACCOUNT_PASSWORD is not set (lab.sh passes it from ~/hmlab/secrets.env)"; exit 2; }

# REST as the administrator; the JSON body on standard input. Prints the status.
rest() {
  local method="$1" path="$2" body="${3:-}"
  if [ -n "$body" ]; then
    printf '%s' "$body" | curl -s -o "$WORK/rest.out" -w '%{http_code}' -u "$ADMIN" -X "$method" \
      -H 'Content-Type: application/json' --data-binary @- "$REST$path"
  else
    curl -s -o "$WORK/rest.out" -w '%{http_code}' -u "$ADMIN" -X "$method" "$REST$path"
  fi
}

account() {
  local address="$1@example.test" status
  rest DELETE "/api/v1/accounts/$address" > /dev/null
  status=$(rest POST /api/v1/domains/example.test/accounts "{\"address\":\"$address\",\"password\":\"$PW\",\"active\":true}")
  case "$status" in
    200|201) echo "account $address" ;;
    *) echo "making $address failed: $status $(head -c 300 "$WORK/rest.out")"; return 1 ;;
  esac
}

# A netrc file for one login at one host; curl reads the password from it. printf is
# the shell's own, so the password is never a process's argument.
netrc() {
  local file="$WORK/netrc-$1-$3"
  ( umask 077; printf 'machine %s login %s password %s\n' "$1" "$2" "$4" > "$file" )
  printf '%s' "$file"
}

message() {
  local file="$WORK/$1.eml" from="$2" to="$3" subject="$4" body="$5"
  printf 'From: %s\r\nTo: %s\r\nSubject: %s\r\nMessage-ID: <%s@example.test>\r\nDate: %s\r\n\r\n%s\r\n' \
    "$from" "$to" "$subject" "$1.$RANDOM$RANDOM" "$(date -R)" "$body" > "$file"
  printf '%s' "$file"
}

# Sequence numbers of the messages in INBOX whose subject contains $4.
imap_search() {
  local url="$1" rc="$2" token="$3"
  curl -s --netrc-file "$rc" "$url/INBOX" -X "SEARCH SUBJECT \"$token\"" 2> /dev/null | tr -d '\r' | sed -n 's/^\* SEARCH *//p'
}

wait_for() {
  local url="$1" rc="$2" token="$3" want="${4:-1}" found=""
  for _ in $(seq 1 90); do
    found=$(imap_search "$url" "$rc" "$token")
    if [ "$(wc -w <<< "$found")" -ge "$want" ]; then
      printf '%s' "$found"
      return 0
    fi
    sleep 1
  done
  printf '%s' "$found"
  return 1
}

verdict() {
  if [ "$1" -eq 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; failed=$((failed + 1)); fi
}

if [ "${1:-}" = accounts ]; then
  shift
  for local in "$@"; do account "$local" || exit 1; done
  exit 0
fi
if [ "${1:-}" = drop ]; then
  shift
  for local in "$@"; do echo "drop $local@example.test: $(rest DELETE "/api/v1/accounts/$local@example.test")"; done
  exit 0
fi

[ -n "$PEER_PW" ] || { echo "HMLAB_PEER_PASSWORD is not set"; exit 2; }
postfix_ip=$(getent hosts postfix | awk '{print $1}')
dovecot_ip=$(getent hosts dovecot | awk '{print $1}')
[ -n "$postfix_ip" ] && [ -n "$dovecot_ip" ] || { echo "postfix or dovecot is not on the lab network (lab.sh up peers)"; exit 2; }
echo "peers: postfix $postfix_ip, dovecot $dovecot_ip"
for local in lab1 lab2 lab3; do account "$local" || exit 1; done
# The route: peer.test to the lab's Postfix, by address, so that the check does not
# rest on the server's own resolver finding a container name.
status=$(rest POST /api/v1/routes "{\"domain_name\":\"peer.test\",\"description\":\"the test lab's Postfix\",\"target_smtp_host\":\"$postfix_ip\",\"target_smtp_port\":587,\"number_of_tries\":2,\"minutes_between_try\":1,\"all_addresses\":true}")
echo "route peer.test -> $postfix_ip:587: $status"

here="imap://127.0.0.1:1143"
there="imap://dovecot:31143"
run=$(date +%s)

echo ""
echo "== 1  this server -> Postfix -> Dovecot"
token="out-$run"
eml=$(message out lab1@example.test peer1@peer.test "lab interop $token" "From the hMailServer under test, relayed through Postfix to Dovecot.")
submit=$(netrc 127.0.0.1 lab1@example.test lab1 "$PW")
curl -s --netrc-file "$submit" --url "smtp://127.0.0.1:2525" --mail-from lab1@example.test --mail-rcpt peer1@peer.test --upload-file "$eml" -w 'SMTP %{response_code}\n'
peer1=$(netrc dovecot peer1@peer.test peer1 "$PEER_PW")
found=$(wait_for "$there" "$peer1" "$token")
verdict $? "peer1@peer.test has it in Dovecot (message ${found:-none})"
if [ -n "$found" ]; then
  curl -s --netrc-file "$peer1" "$there/INBOX;MAILINDEX=${found%% *}" | tr -d '\r' | grep -E '^(Received|Return-Path|Delivered-To):' | head -n 6 | sed 's/^/      /'
fi

echo ""
echo "== 2  Postfix -> this server"
token="in-$run"
eml=$(message in peer1@peer.test lab2@example.test "lab interop $token" "From the Postfix peer to the hMailServer under test.")
curl -s --url "smtp://postfix:587" --mail-from peer1@peer.test --mail-rcpt lab2@example.test --upload-file "$eml" -w 'SMTP %{response_code}\n'
lab2=$(netrc 127.0.0.1 lab2@example.test lab2 "$PW")
found=$(wait_for "$here" "$lab2" "$token")
verdict $? "lab2@example.test has it here (message ${found:-none})"
if [ -n "$found" ]; then
  curl -s --netrc-file "$lab2" "$here/INBOX;MAILINDEX=${found%% *}" | tr -d '\r' | grep -E '^(Received|Return-Path):' | head -n 4 | sed 's/^/      /'
fi

# mbsync's configuration: the passwords by PassCmd from the environment.
mbsync_rc() {
  local far_host="$1" far_port="$2" far_user="$3" far_var="$4" near_host="$5" near_port="$6" near_user="$7" near_var="$8"
  local near_depth="${9:-}"
  cat > "$WORK/mbsyncrc" <<RC
IMAPAccount far
Host $far_host
Port $far_port
User $far_user
PassCmd "printenv $far_var"
TLSType None
AuthMechs LOGIN

IMAPStore far
Account far

IMAPAccount near
Host $near_host
Port $near_port
User $near_user
PassCmd "printenv $near_var"
TLSType None
AuthMechs LOGIN
${near_depth:+PipelineDepth $near_depth}

IMAPStore near
Account near

Channel lab
Far :far:
Near :near:
Patterns INBOX
Create Near
Sync Pull
Expunge None
SyncState $WORK/mbsync-state/
RC
  rm -rf "$WORK/mbsync-state"
  mkdir -p "$WORK/mbsync-state"
}

echo ""
echo "== 3  Dovecot -> this server (mbsync)"
token="sync-in-$run"
peer2=$(netrc dovecot peer2@peer.test peer2 "$PEER_PW")
for n in 1 2 3; do
  eml=$(message "syncin$n" peer2@peer.test peer2@peer.test "lab interop $token $n" "Message $n of 3, in Dovecot first.")
  curl -s --netrc-file "$peer2" "$there/INBOX" --upload-file "$eml" || echo "APPEND $n to Dovecot failed"
done
mbsync_rc dovecot 31143 peer2@peer.test HMLAB_PEER_PASSWORD 127.0.0.1 1143 lab3@example.test HMLAB_BENCH_ACCOUNT_PASSWORD
mbsync -c "$WORK/mbsyncrc" lab > "$WORK/mbsync3.log" 2>&1
code=$?
tail -n 5 "$WORK/mbsync3.log" | sed 's/^/      mbsync: /'
lab3=$(netrc 127.0.0.1 lab3@example.test lab3 "$PW")
found=$(imap_search "$here" "$lab3" "$token")
[ "$code" -eq 0 ] && [ "$(wc -w <<< "$found")" -eq 3 ]
verdict $? "mbsync exit $code; lab3@example.test has $(wc -w <<< "$found") of the 3 here"

# The same copy with mbsync sending this server one command at a time. mbsync
# pipelines its APPENDs, and on 25 September 2026 this server answered the first of
# them and nothing sent after it in the same read, so 3 timed out after one message
# (docs/TestLab.md, "What the first runs found"); 3b says whether the copy works
# without the pipelining.
echo ""
echo "== 3b Dovecot -> this server (mbsync, PipelineDepth 1 on this side)"
account lab5 > /dev/null || exit 1
mbsync_rc dovecot 31143 peer2@peer.test HMLAB_PEER_PASSWORD 127.0.0.1 1143 lab5@example.test HMLAB_BENCH_ACCOUNT_PASSWORD 1
mbsync -c "$WORK/mbsyncrc" lab > "$WORK/mbsync3b.log" 2>&1
code=$?
tail -n 2 "$WORK/mbsync3b.log" | sed 's/^/      mbsync: /'
lab5=$(netrc 127.0.0.1 lab5@example.test lab5 "$PW")
found=$(imap_search "$here" "$lab5" "$token")
[ "$code" -eq 0 ] && [ "$(wc -w <<< "$found")" -eq 3 ]
verdict $? "mbsync exit $code; lab5@example.test has $(wc -w <<< "$found") of the 3 here"

echo ""
echo "== 4  this server -> Dovecot (mbsync)"
token="sync-out-$run"
for n in 1 2; do
  eml=$(message "syncout$n" lab1@example.test lab1@example.test "lab interop $token $n" "Message $n of 2, here first.")
  curl -s --netrc-file "$submit" "$here/INBOX" --upload-file "$eml" || echo "APPEND $n here failed"
done
mbsync_rc 127.0.0.1 1143 lab1@example.test HMLAB_BENCH_ACCOUNT_PASSWORD dovecot 31143 peer3@peer.test HMLAB_PEER_PASSWORD
mbsync -c "$WORK/mbsyncrc" lab > "$WORK/mbsync4.log" 2>&1
code=$?
tail -n 5 "$WORK/mbsync4.log" | sed 's/^/      mbsync: /'
peer3=$(netrc dovecot peer3@peer.test peer3 "$PEER_PW")
found=$(imap_search "$there" "$peer3" "$token")
[ "$code" -eq 0 ] && [ "$(wc -w <<< "$found")" -eq 2 ]
verdict $? "mbsync exit $code; peer3@peer.test has $(wc -w <<< "$found") of the 2 in Dovecot"

echo ""
if [ "$failed" -eq 0 ]; then
  echo "INTEROP PASSED: 5 of 5"
  exit 0
fi
echo "INTEROP FAILED: $failed of 5"
exit 1
