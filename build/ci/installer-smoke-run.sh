#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The installer smoke test, run from the VMware host (Git Bash on Windows): the
# throwaway Windows VM reverted to its clean snapshot, the installers and
# build/ci/installer-smoke.ps1 copied to it, the script run there over SSH, its logs
# brought back, and the VM reverted again - whether the run passed, failed or was
# interrupted. docs/ContinuousIntegration.md, "The installer smoke test".
#
#   bash build/ci/installer-smoke-run.sh [options] FIRST [SECOND]
#
# FIRST is installed; SECOND, when given, is installed over it as the upgrade, then
# everything is uninstalled. A release: the previous release's installer, then the new
# one. A batch: the latest release's, then the installer built from the batch, with
# --allow-unsigned, since nobody has signed that one. Either runs it a second time with
# the new installer alone: a new installation, which is what the secure defaults of
# docs/SecureDefaults.md are checked on (an upgrade is checked for keeping what it had).
#
#   bash build/ci/installer-smoke-run.sh \
#      /c/Users/chris/Downloads/hmailserver/6.3.2/hMailServer-6.3.2-x64.exe \
#      /c/Users/chris/Downloads/hmailserver/v6.3.3-1/hMailServer-6.3.3-x64.exe
#
# Options, each with an environment variable of the same meaning:
#   --vmx PATH          the VM's .vmx                        HM_SMOKE_VMX
#   --snapshot NAME     its clean snapshot                   HM_SMOKE_SNAPSHOT
#   --mac MAC           the MAC the .vmx must carry, and the host must answer ARP with
#                                                            HM_SMOKE_MAC
#   --host ADDRESS      the VM's address                     HM_SMOKE_HOST
#   --user NAME         the SSH user                         HM_SMOKE_USER
#   --key PATH          the SSH private key                  HM_SMOKE_KEY
#   --out DIR           where the logs go (a new directory under the temp directory)
#                                                            HM_SMOKE_OUT
#   --allow-unsigned    an installer with no signature is a SKIP, not a FAIL
#   --keep-vm           leave the VM as the run left it, for a look at a failure; the
#                       next run reverts it anyway
#
# The defaults are the smoke VM on the maintainer's laptop, found on 25 September
# 2026: "Win server 2025 small", the one .vmx in VMware's inventory whose MAC is the
# one 192.168.11.175 answers ARP with, and its one snapshot, "firstsnapshot", taken
# with SSH enabled and nothing else installed. The MAC is checked in the .vmx before
# anything is reverted, so a wrong --vmx cannot revert another machine, and against
# ARP before anything is copied, so a wrong --host cannot install on one.
#
# Only the SSH key is used, never a password. The administrator password the script
# gives the installer is made on the VM and never printed.
#
# Exit codes: 0 every check passed; 1 a check failed; 2 bad arguments; 3 the VM could
# not be identified, reverted or reached; 4 the smoke script did not run to a result.
set -uo pipefail

vmrun="${HM_SMOKE_VMRUN:-/c/Program Files (x86)/VMware/VMware Workstation/vmrun.exe}"
vmx="${HM_SMOKE_VMX:-C:\Users\chris\Documents\Virtual Machines\Win server 2025 small\Win server 2025 small.vmx}"
snapshot="${HM_SMOKE_SNAPSHOT:-firstsnapshot}"
mac="${HM_SMOKE_MAC:-00:0c:29:c8:a1:3a}"
host="${HM_SMOKE_HOST:-192.168.11.175}"
user="${HM_SMOKE_USER:-administrator}"
key="${HM_SMOKE_KEY:-$HOME/.ssh/hmail_ad}"
out="${HM_SMOKE_OUT:-}"
allow_unsigned=0
keep_vm=0

usage() {
   sed -n '/^#   bash build/,/^# Exit codes/p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
   case "$1" in
      --vmx) vmx="$2"; shift 2 ;;
      --snapshot) snapshot="$2"; shift 2 ;;
      --mac) mac="$2"; shift 2 ;;
      --host) host="$2"; shift 2 ;;
      --user) user="$2"; shift 2 ;;
      --key) key="$2"; shift 2 ;;
      --out) out="$2"; shift 2 ;;
      --allow-unsigned) allow_unsigned=1; shift ;;
      --keep-vm) keep_vm=1; shift ;;
      -h|--help) usage; exit 0 ;;
      --) shift; break ;;
      -*) echo "unknown option $1" >&2; usage >&2; exit 2 ;;
      *) break ;;
   esac
done

first="${1:-}"
second="${2:-}"
if [ -z "$first" ] || [ $# -gt 2 ]; then
   usage >&2
   exit 2
fi

# scp reads C:\x as host "C", so every local path goes over in its POSIX form.
local_path() { cygpath -u "$1" 2> /dev/null || printf '%s\n' "$1"; }

first_local=$(local_path "$first")
second_local=""
[ -n "$second" ] && second_local=$(local_path "$second")
key_local=$(local_path "$key")
vmx_local=$(local_path "$vmx")

for file in "$first_local" ${second_local:+"$second_local"} "$key_local"; do
   if [ ! -f "$file" ]; then
      echo "not found: $file" >&2
      exit 2
   fi
done
script_dir=$(cd "$(dirname "$0")" && pwd)
smoke_ps1="$script_dir/installer-smoke.ps1"
if [ ! -f "$smoke_ps1" ]; then
   echo "not found: $smoke_ps1" >&2
   exit 2
fi
if [ ! -x "$vmrun" ] && [ ! -f "$vmrun" ]; then
   echo "vmrun not found at $vmrun (HM_SMOKE_VMRUN)" >&2
   exit 3
fi

if [ -z "$out" ]; then
   out="${TMPDIR:-/tmp}/installer-smoke-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$out"
console="$out/console.log"

ssh_opts=(-i "$key_local" -o BatchMode=yes -o StrictHostKeyChecking=accept-new
          -o ServerAliveInterval=30 -o ServerAliveCountMax=20 -o LogLevel=ERROR)
remote="$user@$host"

say() { printf '%s\n' "$*" | tee -a "$console"; }

# ------------------------------------------------------------------ the VM

mac_lower=$(printf '%s' "$mac" | tr 'A-F' 'a-f')

identify_vm() {
   if [ ! -f "$vmx_local" ]; then
      say "no .vmx at $vmx"
      return 1
   fi
   if ! tr -d '\r' < "$vmx_local" | tr 'A-F' 'a-f' |
        grep -Eq "^ethernet[0-9]+\.(generatedaddress|address) = \"$mac_lower\"$"; then
      say "$vmx does not carry the MAC $mac: not the smoke VM, and nothing is reverted"
      return 1
   fi
   if ! "$vmrun" -T ws listSnapshots "$vmx" | tr -d '\r' | grep -Fxq "$snapshot"; then
      say "$vmx has no snapshot named '$snapshot':"
      "$vmrun" -T ws listSnapshots "$vmx" | tr -d '\r' | sed 's/^/   /' | tee -a "$console"
      return 1
   fi
   say "the VM: $vmx, MAC $mac, snapshot '$snapshot'"
}

vm_running() {
   "$vmrun" -T ws list | tr -d '\r' | tr 'A-Z' 'a-z' | grep -Fxq "$(printf '%s' "$vmx" | tr 'A-Z' 'a-z')"
}

# A snapshot taken with the VM powered on restores it suspended in that state; start
# resumes it. One taken powered off leaves it off; start boots it.
revert_vm() {
   say "== reverting to '$snapshot' ($(date -u +%H:%M:%SZ))"
   if ! "$vmrun" -T ws revertToSnapshot "$vmx" "$snapshot" 2>&1 | tr -d '\r' | tee -a "$console"; then
      return 1
   fi
   if ! vm_running; then
      "$vmrun" -T ws start "$vmx" nogui 2>&1 | tr -d '\r' | tee -a "$console" || return 1
   fi
   vm_running
}

# The address the VM's own VMware Tools report for the .vmx, or nothing.
guest_address() {
   "$vmrun" -T ws getGuestIPAddress "$vmx" 2> /dev/null | tr -d '\r' | grep -Eo '^([0-9]{1,3}[.]){3}[0-9]{1,3}$' | head -n 1
}

# The VM's address once it has held for 45 seconds, followed when it is not $host.
# After a revert the VM resumes still holding the address it had when the snapshot
# was taken, and asks VMware's DHCP server (on this host, for the NAT network) for a
# lease again. On 27 September 2026 that server twice found the address answering
# while the VM asked, marked its lease abandoned and gave the VM another address, 15
# to 35 seconds after the revert (vmnetdhcp.leases: 192.168.11.175 abandoned at
# 08:39:40 and 08:56:19, the VM on .185 and .187): SSH had already answered on the
# old address, the first copy ended in "Connection reset by peer", and the run then
# waited ten minutes on an address nobody held. The address the tools report is the
# .vmx's, whose MAC identify_vm checked; check_address checks it against ARP before
# anything is copied. Tools that report nothing leave $host as it was.
settle_address() {
   local deadline=$((SECONDS + 300)) previous="" current="" since=$SECONDS
   while [ $SECONDS -lt $deadline ]; do
      current=$(guest_address)
      if [ -z "$current" ] || [ "$current" != "$previous" ]; then
         previous="$current"
         since=$SECONDS
      elif [ $((SECONDS - since)) -ge 45 ]; then
         break
      fi
      sleep 5
   done
   if [ -z "$current" ]; then
      say "the VM's tools report no address; using $host"
   elif [ "$current" != "$host" ]; then
      say "the VM is at $current, not $host: its DHCP lease moved after the revert"
      host="$current"
      remote="$user@$host"
   fi
}

wait_for_ssh() {
   local deadline=$((SECONDS + 600))
   while [ $SECONDS -lt $deadline ]; do
      if ssh "${ssh_opts[@]}" -o ConnectTimeout=5 "$remote" "echo ready" 2> /dev/null | tr -d '\r' | grep -qx ready; then
         return 0
      fi
      sleep 5
   done
   return 1
}

# A command that only creates or copies, tried up to three times. Just after a revert
# the VM's first SSH answer can come before its network has settled: on 26 September
# 2026 the first scp of an installer, soon after "ready", ended in "Connection
# reset by peer", and the run stopped with nothing tested - the lease moving under it
# (settle_address). Creating the folders and copying a file over again are safe to
# repeat; the smoke script itself is never retried. The copies are the functions
# below, which read $remote when they run, so that an attempt after the address
# moved reaches the new one.
retry_copy() {
   local attempt
   for attempt in 1 2 3; do
      "$@" && return 0
      if [ "$attempt" -lt 3 ]; then
         say "   attempt $attempt failed; waiting for SSH and trying again"
         sleep 10
         settle_address
         wait_for_ssh || return 1
         check_address || return 1
      fi
   done
   return 1
}

make_folders() {
   ssh "${ssh_opts[@]}" "$remote" 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:/smoke/first, C:/smoke/second, C:/smoke/logs | Out-Null"'
}

# A local file to a folder on the VM.
copy_to_vm() {
   scp "${ssh_opts[@]}" -q "$1" "$remote:$2"
}

copy_logs_back() {
   scp "${ssh_opts[@]}" -q -r "$remote:C:/smoke/logs" "$out/"
}

# The address must be the VM's: ARP answers with the MAC of the .vmx.
check_address() {
   ping -n 1 -w 1000 "$host" > /dev/null 2>&1
   local seen
   seen=$(arp -a "$host" 2> /dev/null | tr -d '\r' | tr 'A-F' 'a-f' | grep -Eo '([0-9a-f]{2}-){5}[0-9a-f]{2}' | head -n 1)
   if [ "$seen" != "${mac_lower//:/-}" ]; then
      say "$host answers ARP with '${seen:-nothing}', not $mac: not the smoke VM, and nothing is installed"
      return 1
   fi
   say "the address: $host is $mac"
}

final_revert() {
   local code=$?
   trap - EXIT
   if [ "$keep_vm" = 1 ]; then
      say "the VM is left as the run left it (--keep-vm); the next run reverts it"
   elif ! revert_vm; then
      say "THE FINAL REVERT FAILED: revert $vmx to '$snapshot' by hand"
      [ "$code" = 0 ] && code=3
   fi
   say "logs: $out"
   exit "$code"
}

# ------------------------------------------------------------------ the run

say "== installer smoke test, $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "first:  $first_local ($(sha256sum "$first_local" | cut -c1-64))"
[ -n "$second_local" ] && say "second: $second_local ($(sha256sum "$second_local" | cut -c1-64))"

identify_vm || exit 3
# From here on the VM is known to be the smoke VM, and it is reverted however the run
# ends.
trap final_revert EXIT
trap 'exit 130' INT TERM

revert_vm || { say "the revert failed"; exit 3; }
say "== waiting for the VM's address to settle"
settle_address
say "== waiting for SSH on $host"
wait_for_ssh || { say "no SSH answer from $host within ten minutes"; exit 3; }
check_address || exit 3

say "== copying the installers and the script"
first_name=$(basename "$first_local")
retry_copy make_folders || { say "C:\smoke could not be made on $host"; exit 3; }
retry_copy copy_to_vm "$first_local" C:/smoke/first/ || { say "the first installer could not be copied"; exit 3; }
arguments="-Installer \"C:/smoke/first/$first_name\""
if [ -n "$second_local" ]; then
   second_name=$(basename "$second_local")
   retry_copy copy_to_vm "$second_local" C:/smoke/second/ || { say "the second installer could not be copied"; exit 3; }
   arguments="$arguments -UpgradeInstaller \"C:/smoke/second/$second_name\""
fi
retry_copy copy_to_vm "$smoke_ps1" C:/smoke/installer-smoke.ps1 || { say "installer-smoke.ps1 could not be copied"; exit 3; }
[ "$allow_unsigned" = 1 ] && arguments="$arguments -AllowUnsigned"

say "== running installer-smoke.ps1 on $host"
ssh "${ssh_opts[@]}" "$remote" \
   "powershell -NoProfile -ExecutionPolicy Bypass -File C:/smoke/installer-smoke.ps1 $arguments -LogDirectory C:/smoke/logs" \
   2>&1 | tr -d '\r' | tee -a "$console"
result=${PIPESTATUS[0]}

say "== collecting the logs"
retry_copy copy_logs_back || say "the logs could not be copied back"

passed=$(grep -c '^PASS ' "$console")
failed=$(grep -c '^FAIL ' "$console")
say ""
say "== $passed passed, $failed failed; installer-smoke.ps1 exited $result"
if [ "$failed" -gt 0 ]; then
   grep '^FAIL ' "$console" | sed 's/^/   /'
fi

case "$result" in
   0) [ "$failed" -eq 0 ] && [ "$passed" -gt 0 ] && exit 0; exit 1 ;;
   1|2) exit 1 ;;
   *) exit 4 ;;
esac
