#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# WHAT THIS PROVES
#
# hmailserver/source/Server/platform/packaging/autoban-hook is what the Linux
# server runs to put an auto-ban below itself. With iptables, which has no
# timeout, an "unban" that fails is a block that never lifts, and the server
# only asks again for one it was not run for; so the hook's iptables and
# ip6tables calls must wait for the xtables lock (-w) rather than fail at once
# while fail2ban, a firewall manager or another run of the hook holds it - and
# "reset", which the server says at every start, must take away everything the
# hook made. Both are exercised here against FAKES on a PATH of their own: an
# iptables and an ip6tables that keep their chains in files, record every call,
# and hold to the xtables lock as legacy iptables does (without -w a call while
# the lock is held fails with exit 4 and iptables' own words; with -w it waits),
# and an nft that records. Nothing is run as root and no firewall is touched.
#
# The cases: ban, ban again (one rule), an unban while another program holds
# the lock for two seconds (it waits and lifts the rule), the same for IPv6
# through ip6tables, reset taking both chains and every INPUT jump away - two
# to one chain included, which two runs of the hook at once can leave - check;
# every iptables and ip6tables call carrying -w; and nft's reset deleting the
# table only when there is one. Run it with bash; the hook runs under sh, the
# way the server runs it (dash where /bin/sh is dash).
#
#   bash build/test-autoban-hook.sh [path-to-hook]
#
# Exit 0 when every case holds; 1 with the cases that did not.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
hook=${1:-$here/../hmailserver/source/Server/platform/packaging/autoban-hook}
[ -f "$hook" ] || { echo "no hook at $hook"; exit 2; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/fake" "$T/tools" "$T/state"

# The tools the hook uses, and nothing else, so that a real nft or iptables on
# this machine is never found.
for tool in sh grep sed head cat sleep flock mkdir rm touch printf tr; do
  path=$(command -v "$tool") || { echo "this machine has no $tool"; exit 2; }
  ln -s "$path" "$T/tools/$tool"
done

# iptables and ip6tables: chains as files under state/<family>/, one rule a line
# in iptables -S form. Only what the hook asks is understood.
cat > "$T/fake/iptables" <<'FAKE'
#!/bin/bash
name=$(basename "$0")
echo "$name $*" >> "$STATE/calls.log"
wait=no
args=()
for a in "$@"; do
  if [ "$a" = "-w" ]; then wait=yes; else args+=("$a"); fi
done
if [ "$wait" = yes ]; then
  exec 9> "$STATE/xtables.lock"; flock 9
else
  exec 9> "$STATE/xtables.lock"
  if ! flock -n 9; then
    echo "Another app is currently holding the xtables lock. Perhaps you want to use the -w option?" >&2
    exit 4
  fi
fi
dir="$STATE/$name"; mkdir -p "$dir"
set -- "${args[@]}"
[ "$1" = "-n" ] && shift
op=$1; chain=${2:-}; shift 2 2>/dev/null || shift
rest="$*"
case "$op" in
  -L) [ -f "$dir/$chain" ] ;;
  -N) [ -f "$dir/$chain" ] && exit 1; : > "$dir/$chain" ;;
  # A chain still jumped to is not deleted, as iptables refuses it.
  -X) grep -qxF -- "-A INPUT -j $chain" "$dir/INPUT" 2>/dev/null && { echo "iptables: Too many links." >&2; exit 1; }
      rm -f "$dir/$chain" ;;
  -F) [ -f "$dir/$chain" ] && : > "$dir/$chain" ;;
  -S) [ "$chain" = INPUT ] && exit 0; [ -f "$dir/$chain" ] && cat "$dir/$chain" ;;
  -C|-I|-D)
    # "-s ADDR" as iptables -S prints it, with its prefix length.
    line="-A $chain $(echo "$rest" | sed -E 's/-s ([0-9.]+) /-s \1\/32 /; s/-s ([0-9a-fA-F:]+:[0-9a-fA-F:]*) /-s \1\/128 /')"
    [ "$chain" = INPUT ] || [ -f "$dir/$chain" ] || exit 1
    touch "$dir/$chain"
    case "$op" in
      -C) grep -qxF -- "$line" "$dir/$chain" ;;
      -I) { echo "$line"; cat "$dir/$chain"; } > "$dir/$chain.new" && mv "$dir/$chain.new" "$dir/$chain" ;;
      # The first rule that matches, and only that one, as iptables deletes.
      -D) grep -qxF -- "$line" "$dir/$chain" || exit 1
          awk -v l="$line" '!d && $0 == l { d = 1; next } { print }' "$dir/$chain" > "$dir/$chain.new"; mv "$dir/$chain.new" "$dir/$chain" ;;
    esac ;;
  *) exit 2 ;;
esac
FAKE
chmod +x "$T/fake/iptables"
ln -s iptables "$T/fake/ip6tables"
for tool in mv basename awk; do ln -s "$(command -v $tool)" "$T/tools/$tool"; done

export STATE="$T/state"
PATHS="$T/fake:$T/tools"
failed=0
fail() { echo "FAIL: $*"; failed=1; }
ok() { echo "ok:   $*"; }
run() { PATH="$PATHS" sh "$hook" "$@"; }

echo "== iptables (no nft on the PATH)"
run ban 192.0.2.7 30 25,587 && ok "ban" || fail "ban exited $?"
grep -qxF -- "-A HMAILSERVER-AUTOBAN -s 192.0.2.7/32 -p tcp -m multiport --dports 25,587 -j DROP" "$STATE/iptables/HMAILSERVER-AUTOBAN" \
  && ok "the rule is there" || fail "no rule after ban: $(cat "$STATE/iptables/HMAILSERVER-AUTOBAN" 2>/dev/null)"
grep -qxF -- "-A INPUT -j HMAILSERVER-AUTOBAN" "$STATE/iptables/INPUT" && ok "INPUT jumps to the chain" || fail "no jump from INPUT"
run ban 192.0.2.7 30 25,587 && [ "$(grep -c 192.0.2.7 "$STATE/iptables/HMAILSERVER-AUTOBAN")" = 1 ] \
  && ok "ban again: one rule" || fail "ban again left $(grep -c 192.0.2.7 "$STATE/iptables/HMAILSERVER-AUTOBAN") rules"

# Another program holds the lock for two seconds while the hook un-bans.
( exec 9> "$STATE/xtables.lock"; flock 9; sleep 2 ) &
holder=$!
sleep 0.3
started=$(date +%s)
run unban 192.0.2.7; code=$?
waited=$(( $(date +%s) - started ))
wait "$holder"
[ "$code" = 0 ] && ok "unban while the lock was held exited 0 after ${waited} s" || fail "unban while the lock was held exited $code: the address stays blocked"
grep -q 192.0.2.7 "$STATE/iptables/HMAILSERVER-AUTOBAN" && fail "the rule is still there after unban" || ok "the rule is gone"

run ban 2001:db8::7 30 - && grep -qxF -- "-A HMAILSERVER-AUTOBAN -s 2001:db8::7/128 -p tcp -j DROP" "$STATE/ip6tables/HMAILSERVER-AUTOBAN" \
  && ok "an IPv6 ban goes to ip6tables" || fail "IPv6 ban: $(cat "$STATE/ip6tables/HMAILSERVER-AUTOBAN" 2>/dev/null)"
( exec 9> "$STATE/xtables.lock"; flock 9; sleep 2 ) &
holder=$!
sleep 0.3
run unban 2001:db8::7; code=$?
wait "$holder"
[ "$code" = 0 ] && ! grep -q 2001:db8::7 "$STATE/ip6tables/HMAILSERVER-AUTOBAN" \
  && ok "an IPv6 unban while the lock was held lifted it" || fail "IPv6 unban while the lock was held exited $code"

run ban 192.0.2.8 30 25 && run ban 2001:db8::8 30 25 && ok "two more bans" || fail "two more bans"
# Two runs of ipt_ensure at once each find no jump and each insert one.
echo "-A INPUT -j HMAILSERVER-AUTOBAN" >> "$STATE/iptables/INPUT"
[ "$(grep -cxF -- "-A INPUT -j HMAILSERVER-AUTOBAN" "$STATE/iptables/INPUT")" = 2 ] \
  && ok "INPUT jumps to the chain twice" || fail "the second jump was not added: $(cat "$STATE/iptables/INPUT")"
( exec 9> "$STATE/xtables.lock"; flock 9; sleep 1 ) &
holder=$!
sleep 0.3
run reset; code=$?
wait "$holder"
[ "$code" = 0 ] && ok "reset exited 0" || fail "reset exited $code"
[ ! -e "$STATE/iptables/HMAILSERVER-AUTOBAN" ] && [ ! -e "$STATE/ip6tables/HMAILSERVER-AUTOBAN" ] \
  && ok "reset removed both chains" || fail "reset left a chain"
grep -q HMAILSERVER-AUTOBAN "$STATE/iptables/INPUT" "$STATE/ip6tables/INPUT" 2>/dev/null \
  && fail "reset left a jump from INPUT" || ok "reset removed the jumps"
run reset && ok "reset with nothing to remove exits 0" || fail "reset with nothing to remove exited $?"
run check > "$T/check.out" 2>&1 && grep -q "ok (iptables)" "$T/check.out" && ok "check" || fail "check: $(cat "$T/check.out")"

without=$(grep -E '^ip6?tables ' "$STATE/calls.log" | grep -v -- ' -w' || true)
[ -z "$without" ] && ok "every one of $(grep -cE '^ip6?tables ' "$STATE/calls.log") iptables and ip6tables calls waited for the lock" \
  || fail "calls without -w: $(echo "$without" | head -3 | tr '\n' ';')"

echo "== nft"
cat > "$T/fake/nft" <<'FAKE'
#!/bin/bash
echo "nft $*" >> "$STATE/nft.log"
case "$*" in
  "list table inet hmailserver") [ -e "$STATE/nft-table" ] ;;
  "delete table inet hmailserver") rm -f "$STATE/nft-table" ;;
  *) exit 0 ;;
esac
FAKE
chmod +x "$T/fake/nft"
: > "$STATE/nft-table"
: > "$STATE/calls.log"
run reset && [ ! -e "$STATE/nft-table" ] && grep -qxF "nft delete table inet hmailserver" "$STATE/nft.log" \
  && ok "nft: reset deletes the table" || fail "nft: reset did not delete the table: $(cat "$STATE/nft.log")"
: > "$STATE/nft.log"
run reset && ! grep -q "delete" "$STATE/nft.log" && ok "nft: reset with no table deletes nothing" || fail "nft: reset with no table: $(cat "$STATE/nft.log")"

if [ "$failed" = 0 ]; then
  echo "AUTOBAN HOOK TESTS PASSED"
  exit 0
fi
echo "AUTOBAN HOOK TESTS FAILED"
exit 1
