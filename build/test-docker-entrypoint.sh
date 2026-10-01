#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container's entrypoint (platform/packaging/docker/docker-entrypoint.sh)
# writing the [Database] section of hMailServer.ini from HM_DB_*: every value is
# written as it is - the password too, with Passwordencryption=0 - so a value the
# file would not give back is refused with exit 64 before the file is touched,
# and any other value reaches the file byte for byte.
#
#   bash build/test-docker-entrypoint.sh [ENTRYPOINT]
#
# ENTRYPOINT defaults to the tree's; given another (an older copy), the same cases
# are its negative control. The entrypoint runs with a stub `hmailserver` first on
# PATH that records its arguments and succeeds, HM_INI naming a scratch file, and
# `true` as the command it execs. Needs bash, awk, sed and mktemp. Exits non-zero
# on the first case that does not hold.
set -uo pipefail

entrypoint="${1:-hmailserver/source/Server/platform/packaging/docker/docker-entrypoint.sh}"
[ -f "$entrypoint" ] || { echo "no entrypoint at $entrypoint (run from the root of a checkout)"; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
cat > "$work/bin/hmailserver" <<'STUB'
#!/bin/sh
printf '%s\n' "$*" >> "$HM_TEST_CALLS"
exit 0
STUB
chmod +x "$work/bin/hmailserver"

failures=0
fail() {
  echo "FAIL  $*"
  failures=$((failures + 1))
}

# run NAME EXPECTED-EXIT VAR=VALUE... : the entrypoint with the variables given and
# no others of its family, against a fresh ini holding one line of its own.
run() {
  local name="$1" expected="$2"
  shift 2
  printf '[Directories]\nDataFolder=%s/data\n' "$work" > "$work/hMailServer.ini"
  : > "$work/calls"
  env -i PATH="$work/bin:/usr/bin:/bin" HOME="$work" HM_INI="$work/hMailServer.ini" HM_TEST_CALLS="$work/calls" \
    HM_CREATE_DATABASE=0 HM_UPGRADE_DATABASE=0 "$@" \
    bash "$entrypoint" true > "$work/out" 2>&1
  local code=$?
  if [ "$code" -ne "$expected" ]; then
    fail "$name: exit $code, not $expected ($(tr '\n' ' ' < "$work/out"))"
    return 1
  fi
  echo "ok    $name (exit $code)"
  return 0
}

# The value of a key in the [Database] section, exactly as the file holds it after
# the "=" (the server trims the ends, which the cases below keep free of spaces).
database_value() {
  awk -v key="$1" 'BEGIN{s=0} /^\[/{s=($0=="[Database]"); next} s { i=index($0,"="); if (i>0 && substr($0,1,i-1)==key) { print substr($0,i+1); exit } }' \
    "$work/hMailServer.ini"
}

# Values the server now passes on as they are: the file must hold each exactly.
hostile="it's a back\\slash; a \"quote\" inside, a # and = and 'x'"
if run "a password with quotes, a backslash, a semicolon and a hash inside is written as it is" 0 \
     HM_DB_TYPE=PostgreSQL HM_DB_HOST=db.example HM_DB_USER="o'brien" HM_DB_PASSWORD="$hostile"; then
  [ "$(database_value Password)" = "$hostile" ] || fail "  ... the file holds [$(database_value Password)], not [$hostile]"
  [ "$(database_value Username)" = "o'brien" ] || fail "  ... the user is [$(database_value Username)], not [o'brien]"
  [ "$(database_value Passwordencryption)" = "0" ] || fail "  ... the password is not marked as written in the clear"
  [ "$(grep -c '^\[Database\]$' "$work/hMailServer.ini")" = 1 ] || fail "  ... the file holds the section more than once"
fi
run "a password in UTF-8 is written as it is" 0 HM_DB_TYPE=PostgreSQL HM_DB_PASSWORD="p$(printf '\303\244')ssw$(printf '\303\266')rd" \
  && { [ "$(database_value Password)" = "p$(printf '\303\244')ssw$(printf '\303\266')rd" ] || fail "  ... the UTF-8 password was changed"; }

# Values the file would not give back: refused, the file untouched. ($'...', not
# $(printf ...), whose command substitution drops a trailing line break.)
nl=$'\n'
cr=$'\r'
tab=$'\t'
for case in \
    "a line break in the password|HM_DB_PASSWORD=secret${nl}[Settings]${nl}AutoBanEnabled=0" \
    "a carriage return in the password|HM_DB_PASSWORD=secret${cr}" \
    "a tab in the user|HM_DB_USER=hm${tab}ail" \
    "a password that ends with a space|HM_DB_PASSWORD=secret " \
    "a password that begins with a space|HM_DB_PASSWORD= secret" \
    "a password in single quotes|HM_DB_PASSWORD='secret'" \
    "a password in double quotes|HM_DB_PASSWORD=\"secret\"" \
    "a host in quotes|HM_DB_HOST='db'" \
    "a database name with a line break|HM_DB_NAME=hm${nl}x" ; do
  name="${case%%|*}"
  assignment="${case#*|}"
  if run "$name is refused" 64 HM_DB_TYPE=PostgreSQL "$assignment"; then
    grep -q '^\[Database\]' "$work/hMailServer.ini" && fail "  ... and yet the [Database] section was written"
    grep -q 'AutoBanEnabled' "$work/hMailServer.ini" && fail "  ... and a line of the value's own reached the file"
    [ -s "$work/calls" ] && fail "  ... and yet hmailserver was run: $(tr '\n' ' ' < "$work/calls")"
  fi
done

# One quote on its own is a value's, not a pair.
run "a password of a single quote is written as it is" 0 HM_DB_TYPE=PostgreSQL "HM_DB_PASSWORD='" \
  && { [ "$(database_value Password)" = "'" ] || fail "  ... the file holds [$(database_value Password)], not [']"; }

echo ""
if [ "$failures" -ne 0 ]; then
  echo "$failures case(s) failed"
  exit 1
fi
echo "Every case passed: the [Database] section holds what the environment gave, or nothing is written."
