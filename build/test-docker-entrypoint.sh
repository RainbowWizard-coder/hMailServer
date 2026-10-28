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
# And what reaches the environment of the programs it runs: neither password, in
# its plain or its _FILE form, reaches any of them - the cat that reads a _FILE,
# the server's own verbs it runs or the command it execs, the server or another -
# the entrypoint's own --apply-environment sees every one of the settings'
# variables (HM_SETTING_*, HM_PROPERTY_*, their _FILE forms and the four
# HM_REST_*), HM_INSTALL_KIND and a variable of a name the entrypoint does not
# use (name) reach the command as they were given, HM_DB_TYPE given in another
# spelling the entrypoint accepts reaches it in the entrypoint's (postgres as
# PostgreSQL), and the settings' variables reach a command that may run
# --apply-environment and not the server itself. HMAILSERVER_RECOVERY_PASSPHRASE,
# which --decrypt-store alone reads, reaches none of the entrypoint's own programs,
# and the command as the settings' variables do: every command but the server.
#
# ENTRYPOINT defaults to the tree's; given another (an older copy), the same cases
# are its negative control. The entrypoint runs with a stub `hmailserver` first on
# PATH that records its arguments and the names (never the values) of its
# environment and succeeds, and a `cat` before the system's that records the names
# in its environment and runs it; HM_INI names a scratch file, and `true` is the
# command it execs unless a case names another. The harness's own variables are
# TEST_*, outside the HM_ family the entrypoint looks at, so that whether it runs
# --apply-environment depends on a case's variables alone. Needs bash, awk, sed
# and mktemp. Reports every case, and exits non-zero when any did not hold.
set -uo pipefail

entrypoint="${1:-hmailserver/source/Server/platform/packaging/docker/docker-entrypoint.sh}"
[ -f "$entrypoint" ] || { echo "no entrypoint at $entrypoint (run from the root of a checkout)"; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/secrets" "$work/env"
# Each run of the stub: its arguments a line of TEST_CALLS, the names of its
# environment, one a line, in TEST_ENVS/<the number of that line>, and NAME=VALUE
# for each variable TEST_VALUES names - none of them a secret - in
# TEST_ENVS/<that number>.values.
cat > "$work/bin/hmailserver" <<'STUB'
#!/bin/sh
printf '%s\n' "$*" >> "$TEST_CALLS"
n="$(wc -l < "$TEST_CALLS" | tr -d ' ')"
awk 'BEGIN { for (name in ENVIRON) print name }' > "$TEST_ENVS/$n"
awk -v names="${TEST_VALUES:-}" 'BEGIN { c = split(names, v, " "); for (i = 1; i <= c; i++) print v[i] "=" (v[i] in ENVIRON ? ENVIRON[v[i]] : "(not set)") }' \
  > "$TEST_ENVS/$n.values"
exit 0
STUB
# Each run of cat: the names of its environment in a TEST_ENVS/cat.* of its own.
cat > "$work/bin/cat" <<'STUB'
#!/bin/sh
awk 'BEGIN { for (name in ENVIRON) print name }' > "$(mktemp "$TEST_ENVS/cat.XXXXXX")"
PATH=/usr/bin:/bin exec cat "$@"
STUB
chmod +x "$work/bin/hmailserver" "$work/bin/cat"

failures=0
fail() {
  echo "FAIL  $*"
  failures=$((failures + 1))
}

# run NAME EXPECTED-EXIT VAR=VALUE... : the entrypoint with the variables given and
# no others of its family, against a fresh ini holding one line of its own, execing
# the words of the array command (true when it is empty).
command=()
run() {
  local name="$1" expected="$2"
  shift 2
  printf '[Directories]\nDataFolder=%s/data\n' "$work" > "$work/hMailServer.ini"
  : > "$work/calls"
  rm -f "$work/env/"*
  local exec_words=(true)
  [ "${#command[@]}" -gt 0 ] && exec_words=("${command[@]}")
  env -i PATH="$work/bin:/usr/bin:/bin" HOME="$work" HM_INI="$work/hMailServer.ini" TEST_CALLS="$work/calls" \
    TEST_ENVS="$work/env" TEST_VALUES="HM_INSTALL_KIND name HM_DB_TYPE" HM_CREATE_DATABASE=0 HM_UPGRADE_DATABASE=0 "$@" \
    bash "$entrypoint" "${exec_words[@]}" > "$work/out" 2>&1
  local code=$?
  if [ "$code" -ne "$expected" ]; then
    fail "$name: exit $code, not $expected ($(tr '\n' ' ' < "$work/out"))"
    return 1
  fi
  [ "${run_quiet:-0}" = 1 ] || echo "ok    $name (exit $code)"
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

# What reaches the programs the entrypoint runs. Each case gives both passwords, in
# one form or the other, six settings' variables, HM_INSTALL_KIND, a variable
# named name, HMAILSERVER_RECOVERY_PASSPHRASE and HM_DB_TYPE as postgres, and lets the entrypoint run every verb it would (the database created
# and upgraded, the administrator password set, the environment applied); the stub
# records the names in the environment of each of those and of the command execed
# after them, and the cat recorder those of each cat.
printf 'db-secret\n' > "$work/secrets/db"
printf 'admin-secret\n' > "$work/secrets/admin"
printf '20480\n' > "$work/secrets/maxmessagesize"
password_names="HM_DB_PASSWORD HM_DB_PASSWORD_FILE HM_ADMIN_PASSWORD HM_ADMIN_PASSWORD_FILE"
setting_names="HM_SETTING_MaxSubmissionsPerIPPerMinute HM_PROPERTY_maxmessagesize_FILE HM_REST_PORT HM_REST_BIND_ADDRESS HM_REST_CERTIFICATE_FILE HM_REST_PRIVATE_KEY_FILE"
settings=(HM_SETTING_MaxSubmissionsPerIPPerMinute=30 "HM_PROPERTY_maxmessagesize_FILE=$work/secrets/maxmessagesize"
  HM_REST_PORT=8045 HM_REST_BIND_ADDRESS=0.0.0.0 HM_REST_CERTIFICATE_FILE=/certs/rest.pem HM_REST_PRIVATE_KEY_FILE=/certs/rest.key)
plain=(HM_DB_PASSWORD=db-secret HM_ADMIN_PASSWORD=admin-secret)
from_files=("HM_DB_PASSWORD_FILE=$work/secrets/db" "HM_ADMIN_PASSWORD_FILE=$work/secrets/admin")

# env_case NAME FORM LAST-CALL SETTINGS(kept|removed) WORD... : the entrypoint
# execing the words given, with the passwords in FORM (plain or file); the stub's
# last run must be the command's own, with LAST-CALL as its arguments. One line per
# case: ok, or FAIL and every reason it does not hold.
env_case() {
  local name="$1" form="$2" last_call="$3" settings_expected="$4" n file variable calls reached records cats apply own
  shift 4
  local problems=()
  command=("$@")
  local passwords=("${plain[@]}")
  [ "$form" = file ] && passwords=("${from_files[@]}")
  run_quiet=1 run "$name" 0 HM_CREATE_DATABASE=1 HM_UPGRADE_DATABASE=1 HM_DB_TYPE=postgres HM_INSTALL_KIND=container \
    name=keepme HMAILSERVER_RECOVERY_PASSPHRASE=recovery-secret "${passwords[@]}" "${settings[@]}" || { command=(); return; }
  command=()
  calls="$(wc -l < "$work/calls" | tr -d ' ')"
  # The entrypoint used both passwords before taking them away.
  [ "$(database_value Password)" = db-secret ] || problems+=("the [Database] section does not hold the database password")
  grep -q -- '--set-admin-password$' "$work/calls" || problems+=("the administrator password was not set")
  # Its own --apply-environment, the first run of the verb, ran and saw every one
  # of the settings' variables: taken out before it, they would be stored at no
  # start, and given back at every one after the first.
  apply="$(grep -n -x -F -- "--config $ini --apply-environment" "$work/calls" | head -n 1 | cut -d: -f1)"
  if [ -z "$apply" ]; then
    problems+=("the entrypoint did not run --apply-environment")
  elif [ -f "$work/env/$apply" ]; then
    for variable in $setting_names; do
      grep -qx "$variable" "$work/env/$apply" || problems+=("the entrypoint's --apply-environment did not see $variable")
    done
  fi
  [ "$(sed -n "${calls}p" "$work/calls")" = "$last_call" ] \
    || problems+=("the last program run was [$(sed -n "${calls}p" "$work/calls")], not the command [$last_call]")
  # No program run, a cat that read a file, the verbs the entrypoint ran or the
  # command, has a password.
  for n in $(seq 1 "$calls"); do
    [ -f "$work/env/$n" ] || problems+=("run $n left no record of its environment")
  done
  records=("$work/env/"cat.*)
  [ -e "${records[0]}" ] || records=()
  cats="${#records[@]}"
  [ "$cats" -gt 0 ] || problems+=("no run of cat was recorded")
  for n in $(seq 1 "$calls"); do
    records+=("$work/env/$n")
  done
  for variable in $password_names; do
    reached=0
    for file in "${records[@]}"; do
      [ -f "$file" ] && grep -qx "$variable" "$file" && reached=$((reached + 1))
    done
    [ "$reached" -eq 0 ] || problems+=("$variable reached $reached of the ${#records[@]} programs run ($cats of them cat)")
  done
  # The recovery passphrase reaches none of the entrypoint's own programs - each
  # cat, and its verbs up to its last, --apply-new-install-defaults --if-marked,
  # where a shell's command line runs that verb too - and the command unless the
  # command is the server itself, below.
  own="$(grep -n -x -F -- "--config $ini --apply-new-install-defaults --if-marked" "$work/calls" | head -n 1 | cut -d: -f1)"
  [ -n "$own" ] || own=$((calls - 1))
  reached=0
  for file in "$work/env/"cat.*; do
    [ -f "$file" ] && grep -qx HMAILSERVER_RECOVERY_PASSPHRASE "$file" && reached=$((reached + 1))
  done
  for n in $(seq 1 "$own"); do
    [ -f "$work/env/$n" ] && grep -qx HMAILSERVER_RECOVERY_PASSPHRASE "$work/env/$n" && reached=$((reached + 1))
  done
  [ "$reached" -eq 0 ] || problems+=("HMAILSERVER_RECOVERY_PASSPHRASE reached $reached of the entrypoint's own $((own + cats)) programs ($cats of them cat)")
  file="$work/env/$calls"
  if [ -f "$file" ]; then
    if [ "$settings_expected" = kept ]; then
      grep -qx HMAILSERVER_RECOVERY_PASSPHRASE "$file" || problems+=("HMAILSERVER_RECOVERY_PASSPHRASE did not reach the command, which may run --decrypt-store")
    else
      grep -qx HMAILSERVER_RECOVERY_PASSPHRASE "$file" && problems+=("HMAILSERVER_RECOVERY_PASSPHRASE reached the server's environment")
    fi
    grep -qx HM_INSTALL_KIND "$file" || problems+=("HM_INSTALL_KIND did not reach the command")
    for variable in "HM_INSTALL_KIND=container" "name=keepme" "HM_DB_TYPE=PostgreSQL"; do
      grep -qx -F -- "$variable" "$file.values" \
        || problems+=("${variable%%=*} reached the command as [$(sed -n "s/^${variable%%=*}=//p" "$file.values")], not [${variable#*=}]")
    done
    for variable in $setting_names; do
      if [ "$settings_expected" = kept ]; then
        grep -qx "$variable" "$file" || problems+=("$variable did not reach the command, which may run --apply-environment")
      else
        grep -qx "$variable" "$file" && problems+=("$variable reached the server's environment")
      fi
    done
  fi
  if [ "${#problems[@]}" -eq 0 ]; then
    echo "ok    $name"
  else
    local reasons
    reasons="$(printf '%s; ' "${problems[@]}")"
    fail "$name: ${reasons%; }"
  fi
}

ini="$work/hMailServer.ini"
for form in plain file; do
  env_case "the server's command, the passwords $form: neither password nor a setting's variable reaches it, HM_INSTALL_KIND does" \
    "$form" "--config $ini --foreground" removed hmailserver --foreground
  env_case "hmailserver --apply-environment as the command, the passwords $form: no password reaches it, every setting's variable and HM_INSTALL_KIND do" \
    "$form" "--config $ini --apply-environment" kept hmailserver --apply-environment
done
env_case "the server by its path with no verb: neither password nor a setting's variable reaches it" \
  plain "" removed "$work/bin/hmailserver"
env_case "a shell's command line as the command (the Compose file's and the Helm chart's form): no password reaches it, every setting's variable does" \
  file "--config $ini --foreground" kept \
  bash -c 'hmailserver --config "$HM_INI" --apply-new-install-defaults --if-marked; exec hmailserver --config "$HM_INI" --foreground'
env_case "the server with another option is not the server alone: no password reaches it, every setting's variable does" \
  plain "--config $ini --foreground --check-config" kept hmailserver --foreground --check-config
env_case "hmailserver --decrypt-store as the command: no password reaches it, the recovery passphrase and every setting's variable do" \
  file "--config $ini --decrypt-store --domain example.com" kept hmailserver --decrypt-store --domain example.com

echo ""
if [ "$failures" -ne 0 ]; then
  echo "$failures check(s) failed"
  exit 1
fi
echo "Every case passed: the [Database] section holds what the environment gave, or nothing is written; no password reaches a program the entrypoint runs, its --apply-environment sees every setting's variable, and a setting's variable and the recovery passphrase reach every command but the server itself, the passphrase none of the entrypoint's own programs."
