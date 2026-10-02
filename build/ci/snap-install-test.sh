#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# "check && pass ... || fail ..." is an if-then-else here: pass and fail always return 0.
# shellcheck disable=SC2015

# WHAT THIS PROVES
#
# The snap snap-build made (hmailserver/docs/Snap.md) installs, is configured, starts,
# answers and goes away cleanly, under snapd, on Ubuntu 24.04:
#
#   - snap install --dangerous; the service installed but neither enabled nor
#     started (install-mode: disable), and its start without a database saying so
#     and exiting 0; a database named only in the configuration file's [Database]
#     taken for one, the start going on to it;
#   - snap set refusing what the snap does not take - a password, a port out of
#     range, an unknown key under db - and keeping the configuration as it was;
#   - the database named with snap set and its password with
#     hmailserver.set-database-password (a root-only file, mode 0600); the
#     administrator's with hmailserver --set-admin-password, which runs as snap_daemon
#     and leaves a hash in the configuration;
#   - snap start --enable: the first start creates the database, applies a new
#     installation's defaults, and SMTP on 25 and 587 (220, EHLO with STARTTLS), POP3
#     on 110 (+OK) and IMAP on 143 (* OK, CAPABILITY with STARTTLS) answer;
#   - the running server: uid and gid snap_daemon, CAP_NET_BIND_SERVICE its one
#     effective, permitted and ambient capability, no_new_privs, a seccomp filter, no
#     password in its environment; the configuration snap_daemon's and 0600, the
#     data directories snap_daemon's, the secrets root's and 0700;
#   - snap set rest.port: the configure hook restarts the running service, and the
#     REST API answers /api/v1/status with the administrator's password (401 with
#     a wrong one), its update check reporting the install kind snap and naming snap
#     refresh hmailserver;
#   - snap restart: it answers again;
#   - snap remove --purge: no process, nothing listening, /snap/hmailserver and
#     /var/snap/hmailserver gone, no snap.hmailserver unit left.
#
# Where: a container of Ubuntu 24.04, by digest, with systemd as PID 1 and snapd, on
# a machine with docker (linix, by hand) - privileged, since snapd mounts snaps and
# snap-confine builds each app's sandbox. Two things keep it off the machine it runs
# on, both checked before and after: systemd-binfmt and its binfmt_misc mounts are
# masked in the image, since a systemd in a privileged container on linix removed
# the host's binfmt handlers when it stopped (29 September 2026); and AppArmor is
# kept out - apparmor.service and snapd.apparmor.service masked and securityfs not
# mounted in the container - since profiles loaded from a privileged container go
# into the host's kernel. So snapd confines the snap here with its seccomp filter,
# its mount namespace and its device cgroup, not with AppArmor: what AppArmor adds
# under strict confinement is proven on an Ubuntu machine of its own (Snap.md).
# snapd mounts the snap with squashfuse in a container, so no loop device of the
# host is used. The database is PostgreSQL from Ubuntu's archive, in the same
# container, reached over TCP on the loopback as an external one would be.
#
# Or, with --remote, a machine of its own running Ubuntu with snapd and AppArmor -
# a VM - reached through a command that runs one shell command line there as root
# (an ssh ... sudo). There the snap is confined by AppArmor as well, which the test
# then requires: AppArmor enabled, strict confinement, the daemon's profile loaded
# in enforce mode and the running server under it, and no AppArmor denial of any of
# the snap's profiles in the kernel's log from the test's start to its end. The
# host checks and the container are left out; PostgreSQL is installed from the
# machine's archive when it is not there; SNAP is a path on that machine.
#
# USAGE (on linix, as a user who may run docker):
#   bash build/ci/snap-install-test.sh SNAP [--keep] [--out DIR]
#   bash build/ci/snap-install-test.sh --remote 'COMMAND' SNAP [--out DIR]
#
#   SNAP      snap/hmailserver_<version>_amd64.snap
#   --keep    leave the container running for a look (docker rm -f it after)
#   --out     where the results go (snap-install-test/ by default)
#   --remote  e.g. 'ssh -p 2222 ubuntu@127.0.0.1 sudo': COMMAND, split into words,
#             with one shell command line added, runs it as root on the machine
#
# Exit status: 0 when every check passed, 1 when one failed, 2 when the test could
# not run.
set -uo pipefail

IMAGE_BASE="${HM_SNAP_TEST_IMAGE:-ubuntu:24.04@sha256:496754492fb28b4d3049432f2ca787449331e23fb14f0dd3fffea86bf5a93eb4}"
NAME="hm-snap-test-$$"
keep=0
out=snap-install-test
snap_file=""
remote=""
while [ "$#" -gt 0 ]; do
   case "$1" in
      --keep) keep=1 ;;
      --out) out="$2"; shift ;;
      --remote) remote="$2"; shift ;;
      -*) echo "unknown option $1" >&2; exit 2 ;;
      *) snap_file="$1" ;;
   esac
   shift
done
[ -n "$snap_file" ] && { [ -n "$remote" ] || [ -f "$snap_file" ]; } \
   || { echo "usage: $0 SNAP [--keep] [--out DIR]  or  $0 --remote 'COMMAND' SNAP [--out DIR]" >&2; exit 2; }
mkdir -p "$out"
log="$out/test.log"
: > "$log"

failures=0
say() { printf '%s\n' "$*" | tee -a "$log"; }
pass() { say "PASS $*"; }
fail() { say "FAIL $*"; failures=$((failures + 1)); }
abort() { say "ERROR $*"; cleanup; host_after; say "RESULT could not run"; exit 2; }
if [ -n "$remote" ]; then
   # $remote is split into words on purpose: a command and its arguments. Each
   # argument goes to the machine's shell quoted, so that it arrives as it was, and
   # runs from /, as a container's commands do: root in a snap has no
   # CAP_DAC_READ_SEARCH, so a working directory such as a home of mode 0750 would
   # have AppArmor deny each of the snap's commands a look at it.
   # shellcheck disable=SC2086
   in_box() { $remote "env -C / $(printf '%q ' "$@")" < /dev/null; }
   # shellcheck disable=SC2086
   in_box_i() { $remote "env -C / $(printf '%q ' "$@")"; }
   # shellcheck disable=SC2086
   box_sh() { $remote "env -C / bash -c $(printf '%q' "$1")" < /dev/null; }
else
   in_box() { docker exec "$NAME" "$@"; }
   in_box_i() { docker exec -i "$NAME" "$@"; }
   box_sh() { docker exec "$NAME" bash -c "$1"; }
fi

# The host's binfmt_misc handlers and AppArmor profiles, counted: the first through
# /proc, the second through a throwaway privileged container that mounts securityfs
# read-only (reading the list needs privilege, changing nothing).
host_binfmt() { find /proc/sys/fs/binfmt_misc -mindepth 1 -maxdepth 1 ! -name register ! -name status | wc -l; }
host_profiles() {
   docker run --rm --privileged --network none "$IMAGE_BASE" \
      sh -c 'mount -t securityfs -o ro securityfs /sys/kernel/security 2>/dev/null; wc -l < /sys/kernel/security/apparmor/profiles' 2>/dev/null
}

cleanup() {
   [ -z "$remote" ] || return 0
   if [ "$keep" = 1 ]; then
      say "INFO --keep: container $NAME left running"
      return
   fi
   docker exec "$NAME" systemctl poweroff > /dev/null 2>&1
   for _ in $(seq 1 30); do
      [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = true ] || break
      sleep 1
   done
   docker rm -f "$NAME" > /dev/null 2>&1
   docker rmi "$image" > /dev/null 2>&1
   rm -f "${apparmor_off:-}"
}

# The host as it was before, whatever the test reached: the container is gone by now.
host_after() {
   [ -z "$remote" ] || return 0
   [ "$keep" != 1 ] || return 0
   [ -n "${binfmt_before:-}" ] || return 0
   local binfmt_after profiles_after
   binfmt_after="$(host_binfmt)"
   profiles_after="$(host_profiles)"
   [ "$binfmt_after" = "$binfmt_before" ] && pass "the host's binfmt_misc handlers: $binfmt_after before and after" \
      || fail "the host's binfmt_misc handlers went from $binfmt_before to $binfmt_after: sudo systemctl restart systemd-binfmt"
   [ "$profiles_after" = "$profiles_before" ] && pass "the host's AppArmor profiles: ${profiles_after:-?} before and after" \
      || fail "the host's AppArmor profiles went from $profiles_before to $profiles_after"
}

# The end, from the last check or from one after which the rest cannot run.
finish() {
   cleanup
   host_after
   say "RESULT $failures failure(s)"
   [ "$failures" = 0 ] && exit 0
   exit 1
}

if [ -n "$remote" ]; then
   state="$(in_box systemctl is-system-running --wait 2>/dev/null)"
   case "$state" in
      running|degraded) pass "the machine under test is up (systemd: $state)" ;;
      *) abort "the machine under test did not answer, or is not up (systemd: '$state')" ;;
   esac
   [ "$(in_box cat /sys/module/apparmor/parameters/enabled 2>/dev/null)" = Y ] \
      && pass "AppArmor is enabled on the machine under test" || fail "AppArmor is not enabled on the machine under test"
   in_box test -f "$snap_file" || abort "the machine under test has no $snap_file"
   if in_box snap list hmailserver > /dev/null 2>&1; then
      in_box snap remove --purge hmailserver >> "$log" 2>&1 || abort "an hmailserver snap left by an earlier run could not be removed"
   fi
   if ! in_box test -x /usr/bin/pg_isready; then
      box_sh "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq postgresql > /dev/null" >> "$log" 2>&1 \
         || abort "PostgreSQL could not be installed on the machine under test"
   fi
   since="$(in_box date +%s)"
fi

if [ -z "$remote" ]; then
binfmt_before="$(host_binfmt)"
profiles_before="$(host_profiles)"
say "INFO host before: $binfmt_before binfmt_misc handlers, ${profiles_before:-?} AppArmor profiles"

# --- the image ----------------------------------------------------------------
image="hm-snap-test:$$"
build_dir="$(mktemp -d)"
cat > "$build_dir/Dockerfile" <<EOF
FROM $IMAGE_BASE
RUN apt-get update -qq \\
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \\
      systemd systemd-sysv dbus snapd squashfuse fuse3 postgresql ca-certificates curl python3 iproute2 > /dev/null \\
 && rm -rf /var/lib/apt/lists/* \\
 && systemctl mask systemd-binfmt.service proc-sys-fs-binfmt_misc.automount proc-sys-fs-binfmt_misc.mount \\
      apparmor.service snapd.apparmor.service sys-kernel-security.mount \\
      systemd-udevd.service systemd-udevd-control.socket systemd-udevd-kernel.socket \\
      getty@.service console-getty.service systemd-logind.service \\
 && systemctl enable snapd.socket snapd.service postgresql.service
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
EOF
if ! docker build -q -t "$image" "$build_dir" >> "$log" 2>&1; then
   rm -rf "$build_dir"
   abort "the test image could not be built ($log)"
fi
rm -rf "$build_dir"

# The kernel's AppArmor switch, as the container reads it, says N: with securityfs
# kept out, snap-confine would otherwise find AppArmor enabled with no way to reach
# it, and refuse to run anything (aa_is_enabled, ENOENT).
apparmor_off="$(mktemp)"
printf 'N\n' > "$apparmor_off"
chmod 0444 "$apparmor_off"
docker run -d --name "$NAME" --privileged --cgroupns=private \
   --tmpfs /run --tmpfs /run/lock --tmpfs /tmp \
   -v "$apparmor_off:/sys/module/apparmor/parameters/enabled:ro" \
   -v "$(readlink -f "$snap_file"):/root/under-test.snap:ro" \
   "$image" > /dev/null || abort "the container did not start"

state=""
for _ in $(seq 1 90); do
   state="$(in_box systemctl is-system-running 2>/dev/null)"
   case "$state" in running|degraded) break ;; esac
   sleep 1
done
case "$state" in
   running) pass "systemd is PID 1 and the system is running" ;;
   degraded) say "INFO the system is degraded: $(in_box systemctl --failed --no-legend --plain | tr '\n' ';')"; pass "systemd is PID 1" ;;
   *) abort "systemd did not come up (state '$state')" ;;
esac
[ "$(in_box systemctl is-enabled systemd-binfmt.service 2>/dev/null)" = masked ] && pass "systemd-binfmt is masked" || fail "systemd-binfmt is not masked"
if in_box test -e /sys/kernel/security/apparmor; then
   fail "AppArmor's securityfs is visible in the container, so snapd would load profiles into the host's kernel"
   finish
fi
pass "AppArmor's securityfs is not mounted in the container"
fi

for _ in $(seq 1 120); do
   in_box snap wait system seed.loaded > /dev/null 2>&1 && break
   sleep 2
done
in_box snap wait system seed.loaded > /dev/null 2>&1 || abort "snapd did not finish seeding"
say "INFO snapd $(in_box snap version | awk '/^snapd/ { print $2 }'), confinement $(in_box snap debug confinement)"
if [ -n "$remote" ]; then
   [ "$(in_box snap debug confinement)" = strict ] && pass "snapd confines strictly here" \
      || fail "snapd's confinement here is $(in_box snap debug confinement), not strict"
fi

# The database, as an external one would be: a role with a password, reached over TCP.
box_sh "until pg_isready -q -h 127.0.0.1; do sleep 1; done" || abort "PostgreSQL did not start"
db_password="snap-test-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
admin_password="Snap-Admin-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
box_sh "su postgres -c 'dropdb --if-exists hmsnap && psql -q -c \"DROP ROLE IF EXISTS hmailserver\"'" >> "$log" 2>&1
box_sh "su postgres -c \"psql -q -c \\\"CREATE ROLE hmailserver LOGIN PASSWORD '$db_password'\\\" && createdb -O hmailserver hmsnap\"" \
   || abort "the database could not be made"

# --- install --------------------------------------------------------------------
snap_target=/root/under-test.snap
[ -z "$remote" ] || snap_target="$snap_file"
if ! in_box snap install --dangerous "$snap_target" >> "$log" 2>&1; then
   in_box journalctl -u snapd --no-pager -n 40 >> "$log" 2>&1
   abort "snap install --dangerous failed ($log)"
fi
pass "snap install --dangerous"
if [ -n "$remote" ]; then
   in_box grep -qx 'snap.hmailserver.daemon (enforce)' /sys/kernel/security/apparmor/profiles \
      && pass "the daemon's AppArmor profile is loaded, in enforce mode" || fail "the daemon's AppArmor profile is not loaded in enforce mode"
fi
in_box snap list >> "$log" 2>&1
services="$(in_box snap services hmailserver)"
say "INFO $(printf '%s' "$services" | tail -n +2)"
printf '%s\n' "$services" | awk 'NR > 1 && $1 == "hmailserver.daemon" && $2 == "disabled" && $3 == "inactive" { ok = 1 } END { exit !ok }' \
   && pass "the service is installed disabled and inactive" || fail "the service is not 'disabled inactive' after install"

# Started with no database: it says so and exits 0, and is not restarted. What the
# unit has said since this start, until it says that, or has stopped for 5 s - the
# journal can have its last lines a moment after the unit has stopped, and a machine
# under emulation can take most of a minute over the start.
mark="$(in_box date +%s)"
in_box snap start hmailserver.daemon > /dev/null 2>&1
said=""
stopped=0
for _ in $(seq 1 120); do
   said="$(in_box journalctl -u snap.hmailserver.daemon.service --no-pager -o cat --since "@$mark" 2>/dev/null)"
   case "$said" in *"no database is configured"*) break ;; esac
   if [ "$(in_box systemctl is-active snap.hmailserver.daemon.service)" != active ]; then
      stopped=$((stopped + 1))
      [ "$stopped" -lt 5 ] || break
   fi
   sleep 1
done
case "$said" in
   *"no database is configured"*) pass "without a database the service says what to do" ;;
   *) fail "without a database the service did not say so: $(printf '%s' "$said" | tail -n 3 | tr '\n' ' ')" ;;
esac
result="$(in_box systemctl show -p Result --value snap.hmailserver.daemon.service)"
[ "$result" = success ] && pass "... and exited 0 (Result=success)" || fail "... and its result is '$result', not success"

# A database named only in the configuration's [Database] - db.type unset, the section
# written by hand - is one: the start goes on to it, here a server that does not
# answer (port 1), and fails, rather than saying no database is configured. The file
# is the server account's, and only it can read it; the edits keep its owner and mode.
ini=/var/snap/hmailserver/common/etc/hMailServer.ini
ini_sed() { box_sh "sed -e '$1' $ini > /tmp/hm-ini && cat /tmp/hm-ini > $ini && rm -f /tmp/hm-ini"; }
if in_box test -f "$ini"; then
   ini_sed 's/^Type=$/Type=PostgreSQL/; s/^Server=$/Server=127.0.0.1/; s/^Port=0$/Port=1/'
   mark="$(in_box date +%s)"
   in_box snap start hmailserver.daemon > /dev/null 2>&1
   said=""
   for _ in $(seq 1 60); do
      said="$(in_box journalctl -u snap.hmailserver.daemon.service --no-pager -o cat --since "@$mark" 2>/dev/null)"
      case "$said" in *"Main process exited"*|*"no database is configured"*) break ;; esac
      sleep 1
   done
   in_box snap stop hmailserver.daemon > /dev/null 2>&1
   case "$said" in
      *"no database is configured"*) fail "a [Database] written by hand was not taken for a database: $(printf '%s' "$said" | head -n 2 | tr '\n' ' ')" ;;
      *"Main process exited, code=exited, status=0/"*) fail "a [Database] naming a server that does not answer did not stop the start" ;;
      *"Main process exited"*) pass "a [Database] written by hand is a database: the start went on to it and failed on its port 1" ;;
      *) fail "the start with a [Database] written by hand said nothing in 60 s: $(printf '%s' "$said" | tail -n 3 | tr '\n' ' ')" ;;
   esac
   ini_sed 's/^Type=PostgreSQL$/Type=/; s/^Server=127\.0\.0\.1$/Server=/; s/^Port=1$/Port=0/'
else
   fail "the start without a database made no $ini"
fi

# --- the configuration, and what it refuses ---------------------------------------------------
refused() {
   local what="$1" says="$2"; shift 2
   local answer
   if answer="$(in_box snap set hmailserver "$@" 2>&1)"; then
      fail "snap set $* was taken ($what)"
      in_box snap unset hmailserver "${@%%=*}" > /dev/null 2>&1
      return
   fi
   case "$answer" in
      *"$says"*) pass "snap set $* refused: $what" ;;
      *) fail "snap set $* refused, but not saying '$says': $answer" ;;
   esac
}
refused "a password is never a snap set value" "hmailserver.set-database-password" db.password=x
refused "a port out of range" "from 1 to 65535" db.port=70000
refused "a key under db the snap does not read" "is not a key the snap reads" db.hots=x
refused "a secret setting" "--set-setting" setting.metricspassword=x
refused "a database type the server does not have" "db.type is one of" db.type=oracle
if in_box snap get hmailserver db > /dev/null 2>&1; then
   fail "a refused snap set left something under db: $(in_box snap get -d hmailserver db 2>&1 | tr '\n' ' ')"
else
   pass "the refused values were not kept"
fi

in_box snap set hmailserver db.type=postgresql db.host=127.0.0.1 db.port=5432 db.name=hmsnap db.user=hmailserver db.wait-seconds=30 \
   >> "$log" 2>&1 && pass "snap set db.type, db.host, db.port, db.name, db.user, db.wait-seconds" || fail "snap set of the database failed"
printf '%s\n' "$db_password" | in_box_i /snap/bin/hmailserver.set-database-password >> "$log" 2>&1 \
   && pass "hmailserver.set-database-password" || fail "hmailserver.set-database-password failed"
[ "$(in_box stat -c '%U %a' /var/snap/hmailserver/common/secrets/database-password 2>/dev/null)" = "root 600" ] \
   && pass "the password file is root's, mode 0600" || fail "the password file is not root's with mode 0600"
[ "$(in_box stat -c '%U %a' /var/snap/hmailserver/common/secrets 2>/dev/null)" = "root 700" ] \
   && pass "the secrets directory is root's, mode 0700" || fail "the secrets directory is not root's with mode 0700"

printf '%s\n' "$admin_password" | in_box_i /snap/bin/hmailserver --set-admin-password >> "$log" 2>&1 \
   && pass "hmailserver --set-admin-password" || fail "hmailserver --set-admin-password failed"
[ "$(in_box stat -c '%U %G %a' "$ini" 2>/dev/null)" = "snap_daemon snap_daemon 600" ] \
   && pass "the configuration is snap_daemon's, mode 0600" || fail "the configuration is $(in_box stat -c '%U %G %a' "$ini" 2>&1), not snap_daemon's with mode 0600"
if in_box grep -q "$admin_password" "$ini"; then fail "the administrator's password is in the configuration as written"; else pass "the configuration holds no administrator password as written"; fi
in_box grep -q '^AdministratorPassword=.' "$ini" && pass "... and holds its hash" || fail "... and holds no hash of it"
in_box grep -qx 'DataFolder=/var/snap/hmailserver/common/data' "$ini" && pass "DataFolder is under \$SNAP_COMMON" || fail "DataFolder is not /var/snap/hmailserver/common/data"
in_box grep -qE '^ProgramFolder=/snap/hmailserver/x[0-9]+/usr/share/hmailserver$' "$ini" && pass "ProgramFolder is the revision's" || fail "ProgramFolder is not the running revision's: $(in_box grep ProgramFolder "$ini")"
in_box /snap/bin/hmailserver --check-config >> "$log" 2>&1 && pass "sudo hmailserver --check-config reads the snap's configuration" || fail "hmailserver --check-config failed ($log)"
in_box /snap/bin/hmailserver.hmctl --help >> "$log" 2>&1 && pass "hmailserver.hmctl runs (--help)" || fail "hmailserver.hmctl --help failed ($log)"

# --- start ----------------------------------------------------------------------
in_box snap start --enable hmailserver.daemon >> "$log" 2>&1 || fail "snap start --enable failed"

listening() { box_sh "ss -Hltn 'sport = :$1'" | grep -q LISTEN; }
wait_ports() {
   local port
   for _ in $(seq 1 90); do
      local all=1
      for port in 25 110 143 587; do listening "$port" || all=0; done
      [ "$all" = 1 ] && return 0
      sleep 1
   done
   return 1
}
if wait_ports; then
   pass "the service listens on 25, 110, 143 and 587"
else
   fail "the service did not listen on 25, 110, 143 and 587"
   in_box journalctl -u snap.hmailserver.daemon.service --no-pager -o cat -n 60 >> "$log" 2>&1
   finish
fi

# One line to a port and what it answers, by Python on the container's side.
ask() {
   in_box_i python3 - "$@" <<'PY'
import socket, sys
port = int(sys.argv[1]); lines = sys.argv[2:]
s = socket.create_connection(("127.0.0.1", port), timeout=15)
f = s.makefile("rb")
out = [f.readline().decode(errors="replace").rstrip()]
for line in lines:
    s.sendall((line + "\r\n").encode())
    while True:
        answer = f.readline().decode(errors="replace").rstrip()
        out.append(answer)
        if not answer:
            break
        if port in (25, 587) and (len(answer) < 4 or answer[3] != "-"):
            break
        if port == 143 and answer.startswith("a1 "):
            break
        if port == 110:
            break
print("\n".join(out))
PY
}
for port in 25 587; do
   answer="$(ask "$port" "EHLO snap-test.example" "QUIT")"
   case "$answer" in 220*) pass "SMTP $port: $(printf '%s' "$answer" | head -n 1)" ;; *) fail "SMTP $port answered '$answer'" ;; esac
   printf '%s\n' "$answer" | grep -q '^250[ -]STARTTLS' && pass "SMTP $port offers STARTTLS (a new installation's defaults)" || fail "SMTP $port offers no STARTTLS"
done
answer="$(ask 110 "QUIT")"
case "$answer" in +OK*) pass "POP3 110: $(printf '%s' "$answer" | head -n 1)" ;; *) fail "POP3 110 answered '$answer'" ;; esac
answer="$(ask 143 "a1 CAPABILITY")"
case "$answer" in "* OK"*) pass "IMAP 143: $(printf '%s' "$answer" | head -n 1)" ;; *) fail "IMAP 143 answered '$answer'" ;; esac
printf '%s\n' "$answer" | grep -q 'STARTTLS' && pass "IMAP 143 offers STARTTLS" || fail "IMAP 143 offers no STARTTLS"
in_box test -e /var/snap/hmailserver/common/data/.new-installation && fail "the new-installation marker is still there" || pass "the first start applied the new installation's defaults and removed the marker"

# --- the running server -------------------------------------------------------------
pid="$(in_box systemctl show -p MainPID --value snap.hmailserver.daemon.service)"
exe="$(in_box readlink "/proc/$pid/exe" 2>/dev/null)"
case "$exe" in /snap/hmailserver/x*/usr/bin/hmailserver) pass "the service's main process is the snap's server ($exe)" ;; *) fail "the service's main process is '$exe'" ;; esac
status="$(in_box cat "/proc/$pid/status" 2>/dev/null)"
field() { printf '%s\n' "$status" | awk -v k="$1:" '$1 == k { print $2; exit }'; }
uid="$(in_box id -u snap_daemon)"
[ "$(printf '%s\n' "$status" | awk '$1 == "Uid:" { print $2 $3 $4 $5 }')" = "$uid$uid$uid$uid" ] && pass "it runs as snap_daemon ($uid), every uid" || fail "it runs as uid $(field Uid), not snap_daemon $uid"
[ "$(printf '%s\n' "$status" | awk '$1 == "Gid:" { print $2 $3 $4 $5 }')" = "$uid$uid$uid$uid" ] && pass "... and gid" || fail "its gid is $(field Gid)"
[ "$(printf '%s\n' "$status" | awk '$1 == "Groups:" { print NF }')" = 1 ] && pass "... with no supplementary group" || fail "its groups are '$(printf '%s\n' "$status" | awk '$1 == "Groups:"')'"
for cap in CapEff CapPrm CapAmb; do
   [ "$(field "$cap")" = 0000000000000400 ] && pass "$cap is CAP_NET_BIND_SERVICE alone" || fail "$cap is $(field "$cap"), not 0000000000000400"
done
[ "$(field NoNewPrivs)" = 1 ] && pass "NoNewPrivs" || fail "NoNewPrivs is $(field NoNewPrivs)"
[ "$(field Seccomp)" = 2 ] && pass "a seccomp filter (snapd's)" || fail "Seccomp is $(field Seccomp), not 2"
if [ -n "$remote" ]; then
   label="$(in_box cat "/proc/$pid/attr/current" 2>/dev/null | tr -d '\0')"
   [ "$label" = "snap.hmailserver.daemon (enforce)" ] && pass "it runs under the daemon's AppArmor profile, enforced" \
      || fail "its AppArmor label is '$label', not snap.hmailserver.daemon (enforce)"
fi
if box_sh "tr '\\0' '\\n' < /proc/$pid/environ" | grep -qE '^HM_(DB|ADMIN)_PASSWORD'; then
   fail "the server's environment carries a password"
else
   pass "the server's environment carries no password"
fi
for dir in data data/temp data/events data/database log; do
   [ "$(in_box stat -c '%U %a' "/var/snap/hmailserver/common/$dir")" = "snap_daemon 750" ] || fail "/var/snap/hmailserver/common/$dir is $(in_box stat -c '%U %a' "/var/snap/hmailserver/common/$dir"), not snap_daemon 750"
done
pass "the data and log directories are snap_daemon's, 0750"
root_files="$(in_box find /var/snap/hmailserver/common/data /var/snap/hmailserver/common/log /var/snap/hmailserver/common/etc -user root 2>/dev/null)"
[ -z "$root_files" ] && pass "nothing in the configuration, data or log directories is root's" || fail "root owns: $root_files"

# --- REST, through the configure hook --------------------------------------------------
in_box snap set hmailserver rest.port=8045 >> "$log" 2>&1 || fail "snap set rest.port failed"
code=""
for _ in $(seq 1 60); do
   code="$(in_box curl -s -o /dev/null -w '%{http_code}' -u "Administrator:$admin_password" http://127.0.0.1:8045/api/v1/status 2>/dev/null)"
   [ "$code" = 200 ] && break
   sleep 1
done
newpid="$(in_box systemctl show -p MainPID --value snap.hmailserver.daemon.service)"
[ "$newpid" != "$pid" ] && pass "snap set restarted the running service (main PID $pid, now $newpid)" || fail "snap set did not restart the service"
[ "$code" = 200 ] && pass "REST /api/v1/status: 200 with the administrator's password" || fail "REST /api/v1/status answered '$code'"
code="$(in_box curl -s -o /dev/null -w '%{http_code}' -u "Administrator:wrong-$admin_password" http://127.0.0.1:8045/api/v1/status 2>/dev/null)"
[ "$code" = 401 ] && pass "... 401 with a wrong one" || fail "... '$code' with a wrong one, not 401"
update="$(in_box curl -s -u "Administrator:$admin_password" http://127.0.0.1:8045/api/v1/update 2>/dev/null | tr -d ' ')"
case "$update" in
   *'"installKind":"snap"'*'"updateCommand":"snaprefreshhmailserver"'*|*'"updateCommand":"snaprefreshhmailserver"'*'"installKind":"snap"'*) pass "the update check says installKind snap and snap refresh hmailserver" ;;
   *) fail "the update check said: $(printf '%s' "$update" | head -c 400)" ;;
esac

# --- restart --------------------------------------------------------------------------
in_box snap restart hmailserver.daemon >> "$log" 2>&1 || fail "snap restart failed"
sleep 2
wait_ports && pass "after snap restart it listens again" || fail "after snap restart it does not listen"
answer="$(ask 25 "QUIT")"
case "$answer" in 220*) pass "SMTP 25 after the restart: 220" ;; *) fail "SMTP 25 after the restart answered '$answer'" ;; esac
errors="$(box_sh "grep -h -i -E 'permission denied|operation not permitted|EACCES|EPERM' /var/snap/hmailserver/common/log/*.log 2>/dev/null | head -n 5")"
[ -z "$errors" ] && pass "the server's logs name no denied access" || fail "the server's logs name a denied access: $errors"
in_box journalctl -u snap.hmailserver.daemon.service --no-pager -o cat > "$out/daemon.journal" 2>&1
if [ -n "$remote" ]; then
   denied="$(box_sh "journalctl -k --no-pager -o cat --since @$since | grep 'apparmor=\"DENIED\"' | grep 'profile=\"snap.hmailserver.'")"
   printf '%s\n' "$denied" > "$out/apparmor-denied.txt"
   [ -z "$denied" ] && pass "AppArmor denied the snap nothing (the kernel's log since the test began)" \
      || fail "AppArmor denied the snap $(printf '%s\n' "$denied" | wc -l) time(s): $(printf '%s\n' "$denied" | sed -E 's/.*(operation=[^ ]+).*(profile=[^ ]+).*(name=[^ ]+|capname=[^ ]+).*/\1 \2 \3/' | sort | uniq -c | head -n 8 | tr '\n' ';')"
fi
in_box cat "$ini" 2>/dev/null | sed -E 's/^(Password|AdministratorPassword)=.*/\1=<removed>/' > "$out/hMailServer.ini"

# --- remove --------------------------------------------------------------------------
in_box snap remove --purge hmailserver >> "$log" 2>&1 && pass "snap remove --purge" || fail "snap remove --purge failed"
sleep 2
box_sh "pgrep -u snap_daemon -l" > /dev/null 2>&1 && fail "a snap_daemon process is still running" || pass "no process of the server is left"
for port in 25 110 143 587 8045; do
   listening "$port" && fail "something still listens on $port"
done
pass "nothing listens on 25, 110, 143, 587 or 8045"
in_box test -e /var/snap/hmailserver && fail "/var/snap/hmailserver is still there" || pass "/var/snap/hmailserver is gone"
in_box test -e /snap/hmailserver && fail "/snap/hmailserver is still there" || pass "/snap/hmailserver is gone"
units="$(box_sh "systemctl list-units --all --no-legend --plain 'snap.hmailserver*' 'snap-hmailserver*'; ls /etc/systemd/system | grep -i hmailserver")"
[ -z "$units" ] && pass "no snap.hmailserver unit or mount is left" || fail "left behind: $(printf '%s' "$units" | tr '\n' ' ')"

finish
