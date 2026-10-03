#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# WHAT THIS PROVES
#
# hmailserver/source/Server/platform/packaging/hmailserver-update is the live
# update's apply step on Linux: root's side of it, which stops the unit, installs
# the package or swaps the AppImage, starts the unit, rolls back when it does not
# come back, and writes the one outcome line the server reads. Every one of those
# outcomes is a path through a shell script, and each path is exercised here
# against FAKES on PATH - a systemctl that records what it was asked and answers
# the state a plan tells it to, an apt-get/dpkg/dnf/rpm that records and exits
# as planned, an hmailserver whose --verify-update passes when a .cosign.bundle
# is beside the file and its content is not marked as tampered - so that the
# whole of the script's logic runs, unprivileged, in seconds, with no package
# manager and no systemd touched.
#
# The cases: the five outcomes the Windows helper writes (ok, failed,
# rolled-back, rollback-failed, no-rollback) for a .deb; the dpkg fallback when
# apt-get is absent; the .rpm with dnf and its rollback through rpm --oldpackage;
# the AppImage swap and its rollback; the refusals a root helper owes an
# unprivileged requester (a package outside the Updates directory, a symlink,
# a unit that is not the one root's configuration names, an unknown key, an
# outcome path elsewhere, a line that is a wildcard, an AppImage request where
# root's configuration names no image or another image, a package request where
# it names one) - each refused in the journal alone, with nothing written into
# the requester's directory; the symlinks a requester might plant where root is
# about to write, followed by nothing; a stop that did not stop the unit, and a
# process of the Updates directory's owner that outlived it, followed by
# nothing; the root re-verification refusing a tampered file, made by the
# configured verifier and never by one the request names, on root's own copy,
# so that a file replaced after the verification is not what is installed or
# run; a package or a rollback image replaced by a symlink between the check
# and the stop, neither copied nor read; root's copies readable by root alone
# until they have verified, and an AppImage's made beside the image rather than
# under a /var/tmp that may be noexec; the version rule (a signed package that is not newer than the installed
# one is not installed, nor one where no installed version can be read, the
# request's version line is not believed over the package's, a rollback image
# that is not the installed version is dropped); an image or a directory anyone
# else can write to; the log file that is only written after the stop;
# --dry-run touching nothing; and the database rule - the earlier version put
# back only onto the database it left, its schema version read with
# `hmailserver --schema-version` as the unit's user before the install and
# after the new version did not come back: a version the package's schema step
# or the new server's start moved (all the way, or part-way), one that cannot be
# read before or after, one that did not move and one at what the new version
# needs that cannot be read after, with and without a rollback image, for a
# .deb and an AppImage, and the backup the server names in the outcome of the
# update not put back. And what the server can do once it runs again, while
# root still has the log and the outcome to write and rename: a symlink put at
# the log's file, at the names the log and the outcome are to take (to a
# directory), at the outcome's own file, and in place of the Updates directory
# itself, each followed by nothing - what it leads to the same afterwards in
# content, mode and owner. And the schema probe in hmailserver.service's own
# sandbox where systemd runs the machine (a fake systemd-run): the unit's
# properties passed and nothing else, an AppImage's two variables, a probe
# systemd does not start never run outside it, no systemd-run never run at all,
# and `check` from a terminal, and an apply without systemd, never lending the
# probe that terminal.
#
# What it cannot prove, and says so: that a real dpkg -i under a real unit puts
# the new binary in place - that needs root and a signed package, and is the
# packaging README's "what was run for real" paragraph; and that a real systemd
# takes the unit's properties as systemctl show reports them and runs the probe
# under them, which build/ci/package-systemd-test.sh shows with systemd as
# PID 1. One case, a process of the Updates directory's owner still running
# after the stop, needs root to run a process as another user; as anyone else
# it says it is skipped (the update-helper job runs as root), and the cases
# after the restart run their hooks as the test's own user instead of uid
# 65534, which shows what is written and moved but not whose it becomes.
#
# USAGE
#   build/test-hmailserver-update.sh [--repo-root DIR] [--keep]
#
# Exit status: 0 when every case passes, 1 when any does not.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"
KEEP=0

while [ "$#" -gt 0 ]; do
   case "$1" in
      --repo-root)
         [ "$#" -ge 2 ] || { echo "--repo-root needs a directory." >&2; exit 2; }
         REPO_ROOT="$(cd "$2" && pwd)"
         shift 2
         ;;
      --keep) KEEP=1; shift ;;
      -h|--help)
         sed -n '5,77p' "${BASH_SOURCE[0]}"
         exit 0
         ;;
      *)
         echo "Unknown option: $1" >&2
         exit 2
         ;;
   esac
done

HELPER="${REPO_ROOT}/hmailserver/source/Server/platform/packaging/hmailserver-update"
[ -f "${HELPER}" ] || { echo "No helper at ${HELPER}." >&2; exit 2; }

WORK="$(mktemp -d)"
if [ "${KEEP}" = 0 ]; then
   trap 'rm -rf "${WORK}"' EXIT
else
   echo "Keeping ${WORK}"
fi

failures=0
passes=0

fail()
{
   echo "  FAIL: $*"
   failures=$((failures + 1))
}

pass()
{
   passes=$((passes + 1))
}

# ---------------------------------------------------------------- the fakes

FAKE_BIN="${WORK}/bin"
REAL_BIN="${WORK}/real"
mkdir -p "${FAKE_BIN}" "${REAL_BIN}"

# The real tools the script needs, by symlink, so that PATH can hold exactly
# these and the fakes - and nothing else, which is how "apt-get is absent" is
# arranged for the dpkg case. Those that are shell builtins in dash are listed
# too, for a sh that has them as programs.
for tool in sh cat head tail sort sed tr cut env timeout setsid setpriv realpath stat date sleep mktemp mkdir cp chown chmod ln mv rm ls dirname basename grep id printf echo test; do
   path="$(command -v "${tool}" 2>/dev/null || true)"
   case "${path}" in
      /*) ln -sf "${path}" "${REAL_BIN}/${tool}" ;;
   esac
done

# Every fake reads its plan from and writes its record to FAKE_DIR, which each
# case sets afresh.
cat > "${FAKE_BIN}/systemctl" <<'FAKE'
#!/bin/sh
echo "systemctl $*" >> "$FAKE_DIR/calls"
case "$1" in
   stop)
      # What the service user did last, just before it was stopped - after
      # root had checked the request: $FAKE_DIR/on-stop, run once.
      if [ -f "$FAKE_DIR/on-stop" ]; then
         sh "$FAKE_DIR/on-stop"
         rm -f "$FAKE_DIR/on-stop"
      fi
      # inactive, unless the case says the stop left the unit otherwise.
      if [ -f "$FAKE_DIR/stop-state" ]; then
         cat "$FAKE_DIR/stop-state" > "$FAKE_DIR/state"
      else
         echo inactive > "$FAKE_DIR/state"
      fi
      ;;
   start)
      # What the service user does once it runs again, at the first start -
      # after root took the Updates directory, the log and the outcome while
      # the unit was stopped, and before it has written the outcome or renamed
      # either into place: $FAKE_DIR/on-start, run once, as the uid in
      # $FAKE_DIR/on-start-as when there is one (a root run's service user).
      if [ -f "$FAKE_DIR/on-start" ]; then
         if [ -f "$FAKE_DIR/on-start-as" ]; then
            as="$(cat "$FAKE_DIR/on-start-as")"
            setpriv --reuid="$as" --regid="$as" --clear-groups sh "$FAKE_DIR/on-start"
         else
            sh "$FAKE_DIR/on-start"
         fi
         rm -f "$FAKE_DIR/on-start"
      fi
      # One line of the plan per start: active or failed; active when the
      # plan is exhausted.
      plan=""
      if [ -f "$FAKE_DIR/start-plan" ]; then
         plan="$(head -n 1 "$FAKE_DIR/start-plan")"
         sed -i '1d' "$FAKE_DIR/start-plan"
      fi
      [ -n "$plan" ] || plan=active
      echo "$plan" > "$FAKE_DIR/state"
      # The server's own schema step at its first start ([Database]
      # UpgradeOnStart=1): the database's version moved, once.
      if [ -f "$FAKE_DIR/start-moves-db-to" ]; then
         cat "$FAKE_DIR/start-moves-db-to" > "$FAKE_DIR/db-version"
         rm -f "$FAKE_DIR/start-moves-db-to"
      fi
      ;;
   show)
      # The unit's User= and Environment=, as the case sets them; empty
      # otherwise, which is root and nothing. And its sandbox, asked for as
      # Name=value lines: $FAKE_DIR/unit-sandbox, whole - with lines that were
      # not asked for, which the helper must leave out.
      case "$*" in
         *ActiveState*) cat "$FAKE_DIR/state" 2>/dev/null || echo inactive ;;
         *NRestarts*) echo 0 ;;
         *"-p User "*) cat "$FAKE_DIR/unit-user" 2>/dev/null || echo "" ;;
         *"-p Environment "*) cat "$FAKE_DIR/unit-environment" 2>/dev/null || echo "" ;;
         *"-p NoNewPrivileges "*) cat "$FAKE_DIR/unit-sandbox" 2>/dev/null || true ;;
         *) echo "" ;;
      esac
      ;;
   is-active)
      cat "$FAKE_DIR/state" 2>/dev/null || echo inactive
      ;;
esac
exit 0
FAKE

# The versions the fakes answer with. A package's version is the one in its
# file name (hmailserver_9.9.9_amd64.deb, hmailserver-9.9.9-1.x86_64.rpm,
# hMailServer-9.9.9-x86_64.AppImage), unless $FAKE_DIR/package-version says
# otherwise; the installed version is $FAKE_DIR/installed-version, 9.9.8 when
# the case has not set one. Reads of either are not recorded as calls: they
# change nothing, and a dry run makes them too.
cat > "${FAKE_BIN}/fake-versions.sh" <<'FAKE'
version_in_name()
{
   n="$(basename "$1")"
   n="${n#hmailserver_}"; n="${n#hmailserver-}"; n="${n#hMailServer-}"
   printf '%s' "$n" | sed 's/[_-].*$//'
}
package_version()
{
   if [ -f "$FAKE_DIR/package-version" ]; then cat "$FAKE_DIR/package-version"; else version_in_name "$1"; fi
}
installed_version()
{
   if [ -f "$FAKE_DIR/installed-version" ]; then cat "$FAKE_DIR/installed-version"; else printf '9.9.8'; fi
}
# GNU sort's idea of version order: $1 gt $2.
version_gt()
{
   [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | tail -n 1)" = "$1" ]
}
FAKE

# apt-get, dpkg, dnf and rpm: record, then exit with the next line of the
# install plan (0 when there is none). dpkg --compare-versions and rpm's
# queries (-q, -qp) are reads: answered from the versions above, not recorded,
# and they consume no plan.
for tool in apt-get dpkg dnf rpm; do
   cat > "${FAKE_BIN}/${tool}" <<'FAKE'
#!/bin/sh
me="$(basename "$0")"
. "$(dirname "$0")/fake-versions.sh"
case "$me $1" in
   "dpkg --compare-versions")
      case "$3" in
         gt) version_gt "$2" "$4"; exit $? ;;
         *) echo "the fake dpkg compares with gt only" >&2; exit 2 ;;
      esac
      ;;
   "rpm -qp")
      package_version "$4"; exit 0 ;;
   "rpm -q")
      installed_version; exit 0 ;;
esac
echo "$me $*" >> "$FAKE_DIR/calls"
# A forward install - not the rollback's downgrade - puts the new program in
# place, which says the version $FAKE_DIR/install-required names and answers
# --schema-version, and runs the package's schema step (postinst's
# --upgrade-database), which moves the database to $FAKE_DIR/install-moves-db-to,
# or leaves it unreadable with $FAKE_DIR/install-breaks-db-read.
case " $* " in
   *" --allow-downgrades "*|*" --oldpackage "*) ;;
   *)
      rm -f "$FAKE_DIR/no-schema-verb"
      [ ! -f "$FAKE_DIR/install-required" ] || cat "$FAKE_DIR/install-required" > "$FAKE_DIR/required"
      [ ! -f "$FAKE_DIR/install-moves-db-to" ] || cat "$FAKE_DIR/install-moves-db-to" > "$FAKE_DIR/db-version"
      [ ! -f "$FAKE_DIR/install-breaks-db-read" ] || : > "$FAKE_DIR/db-unreadable"
      ;;
esac
# The first line of the file it was given to install: what was installed, as
# against what was verified. And its mode: apt hands a local package to an
# unprivileged user of its own, who must be able to read it.
for last in "$@"; do :; done
case "$last" in
   /*)
      if [ -f "$last" ]; then
         echo "$me $(head -n 1 "$last")" >> "$FAKE_DIR/installed-bytes"
         echo "$me $(stat -c %a "$last")" >> "$FAKE_DIR/installed-modes"
      fi
      ;;
esac
code=""
if [ -f "$FAKE_DIR/install-plan" ]; then
   code="$(head -n 1 "$FAKE_DIR/install-plan")"
   sed -i '1d' "$FAKE_DIR/install-plan"
fi
[ -n "$code" ] || code=0
exit "$code"
FAKE
done

# dpkg-deb -f FILE Version and dpkg-query -W -f '${Version}' hmailserver.
cat > "${FAKE_BIN}/dpkg-deb" <<'FAKE'
#!/bin/sh
. "$(dirname "$0")/fake-versions.sh"
[ "$1" = "-f" ] && [ "$3" = "Version" ] || { echo "the fake dpkg-deb answers -f FILE Version only" >&2; exit 2; }
package_version "$2"
FAKE
cat > "${FAKE_BIN}/dpkg-query" <<'FAKE'
#!/bin/sh
. "$(dirname "$0")/fake-versions.sh"
installed_version
FAKE

# hmailserver --verify-update FILE: the bundle beside the file, and content
# that has not been marked. --version answers the installed version, because
# a copy of this fake stands in for the RUNNING AppImage, whose --version is
# what the helper takes the installed version from. When the case has written
# $FAKE_DIR/swap-after-verify, the first file that verifies is followed by a
# replacement of the file that file names with $FAKE_DIR/swap-with - what a
# process of the service user that outlived the stop could do to the package
# in the Updates directory between root's verification and root's install.
cat > "${FAKE_BIN}/hmailserver" <<'FAKE'
#!/bin/sh
if [ "$1" = "--version" ]; then
   if [ -f "$FAKE_DIR/installed-version" ]; then v="$(cat "$FAKE_DIR/installed-version")"; else v=9.9.8; fi
   [ -z "$v" ] || echo "hMailServer $v build 0"
   exit 0
fi
echo "hmailserver $*" >> "$FAKE_DIR/calls"
# --schema-version as the server answers it: the version this program needs
# ($FAKE_DIR/required, 6108 when the case has not said), then the database's
# ($FAKE_DIR/db-version, 6108 likewise), or why not - a database it cannot
# reach ($FAKE_DIR/db-unreadable), or a program from before the option
# ($FAKE_DIR/no-schema-verb, which a forward install removes). What it was given
# of the environment that says where an AppImage's configuration is goes to
# $FAKE_DIR/schema-env.
if [ "$1" = "--schema-version" ]; then
   if [ -f "$FAKE_DIR/no-schema-verb" ]; then
      echo "Unknown option: --schema-version" >&2
      echo "Try --help." >&2
      exit 2
   fi
   env | grep -E '^(HMAILSERVER_HOME|XDG_DATA_HOME|EVIL)=' >> "$FAKE_DIR/schema-env"
   # Whether it has a controlling terminal it could reach: one it shares with
   # an administrator's root shell, under `check`.
   if (: < /dev/tty) 2>/dev/null; then echo tty; else echo none; fi >> "$FAKE_DIR/probe-tty"
   if [ -f "$FAKE_DIR/required" ]; then echo "required $(cat "$FAKE_DIR/required")"; else echo "required 6108"; fi
   if [ -f "$FAKE_DIR/db-unreadable" ]; then
      echo "Could not connect to the database: (fake) connection refused" >&2
      exit 1
   fi
   if [ -f "$FAKE_DIR/db-version" ]; then echo "database $(cat "$FAKE_DIR/db-version")"; else echo "database 6108"; fi
   exit 0
fi
if [ "$1" = "--verify-update" ]; then
   # What it was given to read, by its first line, and the modes of the file and
   # of its directory: what anybody but root could have read of it while it was
   # being verified.
   [ -f "$2" ] && head -n 1 "$2" >> "$FAKE_DIR/verified-bytes"
   echo "$(stat -c %a "$2") $(stat -c %a "$(dirname "$2")")" >> "$FAKE_DIR/verified-modes"
   [ -f "$2.cosign.bundle" ] || { echo "$2 has no Sigstore bundle beside it" >&2; exit 1; }
   if grep -q TAMPERED "$2"; then echo "$2 did not verify" >&2; exit 1; fi
   echo "$2 verifies"
   if [ -f "$FAKE_DIR/swap-after-verify" ]; then
      cp "$FAKE_DIR/swap-with" "$(head -n 1 "$FAKE_DIR/swap-after-verify")"
      rm -f "$FAKE_DIR/swap-after-verify"
   fi
   exit 0
fi
echo "hMailServer 0.0.0 build 0"
FAKE

# runuser -u USER -- COMMAND: recorded, and COMMAND run as whoever runs the
# test - what the helper asks as the unit's user is what the case looks at.
cat > "${FAKE_BIN}/runuser" <<'FAKE'
#!/bin/sh
echo "runuser $*" >> "$FAKE_DIR/calls"
[ "$1" = "-u" ] && [ "$3" = "--" ] || { echo "the fake runuser takes -u USER -- COMMAND only" >&2; exit 2; }
shift 3
exec "$@"
FAKE

# systemd-run OPTIONS -- COMMAND: recorded, and its arguments one per line in
# $FAKE_DIR/systemd-run-argv, a line "==" after each call; COMMAND then run as
# whoever runs the test, with what --setenv gives it, in a session of its own
# as systemd would start it - or, with $FAKE_DIR/systemd-run-refuses, not
# started at all, as systemd refuses a transient unit it cannot set up.
cat > "${FAKE_BIN}/systemd-run" <<'FAKE'
#!/bin/sh
echo "systemd-run $*" >> "$FAKE_DIR/calls"
{ for argument in "$@"; do printf '%s\n' "$argument"; done; echo "=="; } >> "$FAKE_DIR/systemd-run-argv"
if [ -f "$FAKE_DIR/systemd-run-refuses" ]; then
   echo "Failed to start transient service unit: (fake) Unit has a bad setting." >&2
   exit 1
fi
set_env=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
   case "$1" in
      --setenv=*) set_env="$set_env ${1#--setenv=}" ;;
   esac
   shift
done
[ $# -gt 0 ] || { echo "the fake systemd-run takes OPTIONS -- COMMAND only" >&2; exit 2; }
shift
# shellcheck disable=SC2086
exec env $set_env setsid -w "$@"
FAKE

chmod +x "${FAKE_BIN}"/*

# ---------------------------------------------------------------- one case

# A fresh Updates directory with a package and a rollback image, each with a
# bundle, and a request built from the arguments. Runs the helper with the
# fakes first on PATH and nothing but the real tools after them.
CASE=""
UPDATES=""
FAKE_DIR=""
PRIVATE_TMP=""
REQUEST=""
OUTCOME=""
LOG=""
RC=0
OUT=""

setup()
{
   CASE=$1
   local dir="${WORK}/${CASE}"
   mkdir -p "${dir}/updates/rollback"
   UPDATES="${dir}/updates"
   FAKE_DIR="${dir}/fake"
   PRIVATE_TMP="${dir}/tmp"
   mkdir -p "${FAKE_DIR}"
   printf 'new package bytes\n' > "${UPDATES}/hmailserver_9.9.9_amd64.deb"
   printf 'bundle\n' > "${UPDATES}/hmailserver_9.9.9_amd64.deb.cosign.bundle"
   printf 'old package bytes\n' > "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb"
   printf 'bundle\n' > "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb.cosign.bundle"
   REQUEST="${dir}/apply-request"
   OUTCOME="${UPDATES}/last-apply.txt"
   LOG="${UPDATES}/apply-9.9.9.log"
   # systemd does not run the machine, whatever runs the test, unless the case
   # says it does (systemd_here).
   SYSTEMD_HERE="${dir}/no-systemd"
   echo "== ${CASE}"
}

# What the helper takes for systemd running the machine: a directory where it
# looks for /run/systemd/system.
systemd_here()
{
   SYSTEMD_HERE="${WORK}/${CASE}/systemd"
   mkdir -p "${SYSTEMD_HERE}"
}

# The default request: a .deb with a rollback. Callers append or override
# lines by writing REQUEST themselves.
write_request()
{
   cat > "${REQUEST}" <<EOF
kind=deb
version=9.9.9
package=${UPDATES}/hmailserver_9.9.9_amd64.deb
rollback=${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb
rollback_version=9.9.8
unit=hmailserver.service
outcome=${OUTCOME}
log=${LOG}
wait=2
EOF
}

run_helper()
{
   local without="${1:-}"
   local path="${FAKE_BIN}:${REAL_BIN}"
   if [ -n "${without}" ]; then
      # The same fakes minus one program, for "apt-get is not installed".
      local thinned="${WORK}/${CASE}/bin"
      mkdir -p "${thinned}"
      for f in "${FAKE_BIN}"/*; do
         [ "$(basename "$f")" = "${without}" ] || ln -sf "$f" "${thinned}/$(basename "$f")"
      done
      path="${thinned}:${REAL_BIN}"
   fi
   # TMPDIR is where the helper makes the directory its copies go in, one per
   # case so that a case can see it made and removed.
   mkdir -p "${PRIVATE_TMP}"
   OUT="$(env -i PATH="${path}" FAKE_DIR="${FAKE_DIR}" TMPDIR="${PRIVATE_TMP}" \
      HMAILSERVER_UPDATE_SETTLE_SECONDS=1 HMAILSERVER_UPDATE_POLL_SECONDS=1 HMAILSERVER_UPDATE_PAUSE_SECONDS=0 \
      HMAILSERVER_UPDATE_PROGRAM="${FAKE_BIN}/hmailserver" HMAILSERVER_UPDATE_SYSTEMD_DIR="${SYSTEMD_HERE}" \
      sh "${HELPER}" apply --request "${REQUEST}" --updates-dir "${UPDATES}" "${@:2}" 2>&1)"
   RC=$?
}

# The same on a terminal of its own, as from an administrator's shell: script(1)
# gives the command a pseudo-terminal as its controlling terminal. $1 is
# "apply" or "check"; the rest is for the helper. Answers false, with RC 125,
# where there is no script.
run_helper_on_a_terminal()
{
   local verb=$1 wrapper="${WORK}/${CASE}/on-a-terminal.sh"
   shift
   command -v script >/dev/null 2>&1 || { RC=125; return 1; }
   mkdir -p "${PRIVATE_TMP}"
   {
      printf 'exec env -i'
      printf " '%s'" "PATH=${FAKE_BIN}:${REAL_BIN}" "FAKE_DIR=${FAKE_DIR}" "TMPDIR=${PRIVATE_TMP}" \
         HMAILSERVER_UPDATE_SETTLE_SECONDS=1 HMAILSERVER_UPDATE_POLL_SECONDS=1 HMAILSERVER_UPDATE_PAUSE_SECONDS=0 \
         "HMAILSERVER_UPDATE_PROGRAM=${FAKE_BIN}/hmailserver" "HMAILSERVER_UPDATE_SYSTEMD_DIR=${SYSTEMD_HERE}" \
         sh "${HELPER}" "${verb}"
      [ "${verb}" != apply ] || printf " '%s'" --request "${REQUEST}" --updates-dir "${UPDATES}"
      [ "$#" = 0 ] || printf " '%s'" "$@"
      printf '\n'
   } > "${wrapper}"
   OUT="$(SHELL=/bin/sh script -qec "sh '${wrapper}'" /dev/null < /dev/null 2>&1)"
   RC=$?
   return 0
}

# A file's or a directory's state as a person with the machine would see it:
# what is in it, its mode, its owner.
state_of()
{
   if [ -d "$1" ] && [ ! -L "$1" ]; then
      printf '%s|%s' "$(ls -A "$1" | tr '\n' ' ')" "$(stat -c '%a %u:%g' "$1")"
   else
      printf '%s|%s' "$(cat "$1" 2>/dev/null)" "$(stat -c '%a %u:%g' "$1" 2>/dev/null)"
   fi
}

# The service user, for a case whose hook acts as the service user does: as
# root, the case's files are given to uid 65534, which then owns the Updates
# directory and the one above it as the service user does, and the hook runs as
# 65534 (on-start-as); as anyone else, the test's own user owns all of it and
# the hook runs as that user. What a case plants its links to - the victims -
# is made afterwards, and stays whoever runs the test's.
as_the_service_user()
{
   if [ "$(id -u)" = 0 ] && command -v setpriv >/dev/null 2>&1; then
      chmod 0711 "${WORK}"
      chown -R 65534:65534 "${WORK}/${CASE}"
      echo 65534 > "${FAKE_DIR}/on-start-as"
   fi
}

# A string as an extended regular expression that matches it alone.
re_quote()
{
   printf '%s' "$1" | sed 's/[][\.*^$+?(){}|]/\\&/g'
}

# The pattern of the copy the helper made of a file it was given: in a
# directory of its own under TMPDIR, in "package" or "rollback", by the same
# name.
copy_of()
{
   printf '%s/hmailserver-update\\.[A-Za-z0-9]{6}/%s/%s' "$(re_quote "${PRIVATE_TMP}")" "$1" "$(re_quote "$2")"
}

expect_call_re()
{
   if calls | grep -qE -- "^$1\$"; then pass; else fail "no call matching '$1'; calls were:"; calls | sed 's/^/        /'; fi
}

# What the fake package manager was handed to install, by its first line.
expect_installed_bytes()
{
   local got
   got="$(cat "${FAKE_DIR}/installed-bytes" 2>/dev/null)"
   [ "${got}" = "$1" ] && pass || fail "installed '${got}', expected '$1'"
}

# The helper's copies are gone when it is.
expect_no_copies_left()
{
   [ -z "$(ls -A "${PRIVATE_TMP}" 2>/dev/null)" ] && pass || fail "the helper left its copies behind: $(ls -A "${PRIVATE_TMP}" | tr '\n' ' ')"
}

outcome_line()
{
   [ -f "${OUTCOME}" ] && head -n 1 "${OUTCOME}" || echo "(no outcome file)"
}

calls()
{
   [ -f "${FAKE_DIR}/calls" ] && cat "${FAKE_DIR}/calls" || true
}

expect_outcome_starts()
{
   local line
   line="$(outcome_line)"
   case "${line}" in
      "$1"*) pass ;;
      *) fail "outcome is '${line}', expected it to start with '$1'" ;;
   esac
}

expect_outcome_contains()
{
   local line
   line="$(outcome_line)"
   case "${line}" in
      *"$1"*) pass ;;
      *) fail "outcome is '${line}', expected it to contain '$1'" ;;
   esac
}

expect_rc()
{
   [ "${RC}" = "$1" ] && pass || fail "exit status ${RC}, expected $1"
}

# A call is matched from the start of its line, so that "dnf" does not match
# a path with "rpm-without-dnf" in it.
called()
{
   local pattern
   pattern="$(printf '%s' "$1" | sed 's/[][\.*^$]/\\&/g')"
   calls | grep -q -- "^${pattern}"
}

expect_call()
{
   if called "$1"; then pass; else fail "no call '$1'; calls were:"; calls | sed 's/^/        /'; fi
}

expect_no_call()
{
   if called "$1"; then fail "unexpected call '$1'"; calls | sed 's/^/        /'; else pass; fi
}

expect_output()
{
   # A here-string: printf into grep -q, which stops at the match, can die of SIGPIPE
   # writing the rest, and pipefail then reads the output as lacking it.
   if grep -qF -- "$1" <<< "${OUT}"; then pass; else fail "output lacks '$1'; output was:"; printf '%s\n' "${OUT}" | sed 's/^/        /'; fi
}

expect_one_line()
{
   [ -f "${OUTCOME}" ] || { fail "no outcome file"; return; }
   [ "$(wc -l < "${OUTCOME}")" = 1 ] && pass || fail "the outcome file has $(wc -l < "${OUTCOME}") lines, not one"
}

# ---------------------------------------------------------------- the cases

setup ok-deb
write_request
run_helper
expect_rc 0
expect_outcome_starts "ok 9.9.9 the package was installed and the service is running"
expect_one_line
expect_call "systemctl stop hmailserver.service"
# Root verifies and installs its own copies, never the files in the Updates
# directory, and removes them when it is done.
expect_call_re "hmailserver --verify-update $(copy_of package hmailserver_9.9.9_amd64.deb)"
expect_call_re "hmailserver --verify-update $(copy_of rollback hmailserver_9.9.8_amd64.deb)"
expect_call_re "apt-get install -y -q --no-install-recommends -o DPkg::Lock::Timeout=300 -o Dpkg::Use-Pty=0 -o Dpkg::Progress-Fancy=0 $(copy_of package hmailserver_9.9.9_amd64.deb)"
expect_installed_bytes "apt-get new package bytes"
expect_no_copies_left
expect_call "systemctl start hmailserver.service"
expect_no_call "dpkg -i"
expect_no_call "hmailserver --verify-update ${UPDATES}/"
[ -f "${REQUEST}" ] && fail "the request was not removed" || pass
[ -f "${LOG}" ] && grep -q "outcome: ok" "${LOG}" && pass || fail "the log file was not written with the outcome"
# The stop comes first; the verification and the install come after it - and
# so does the first line in the log file, which is not written before the stop.
first_call="$(calls | head -n 1)"
[ "${first_call}" = "systemctl stop hmailserver.service" ] && pass || fail "the first call was '${first_call}', not the stop"
grep -q "verifying the package" "${LOG}" && pass || fail "the log file lacks what came after the stop"
grep -q "applying hMailServer 9.9.9" "${LOG}" && fail "the log file holds a line from before the stop" || pass
# The outcome and the log were made under names of the helper's own choosing
# and renamed into place: nothing of theirs is left behind, and the forward
# install was not allowed to be a downgrade.
leftover=""
for f in "${UPDATES}"/last-apply.?????? "${UPDATES}"/apply.??????; do
   [ -e "${f}" ] && leftover="${leftover} ${f##*/}"
done
[ -z "${leftover}" ] && pass || fail "a temporary file was left in the Updates directory:${leftover}"
calls | grep -q -- '--allow-downgrades' && fail "the forward install allowed a downgrade" || pass
[ "$(stat -c %a "${OUTCOME}")" = 640 ] && pass || fail "the outcome file's mode is $(stat -c %a "${OUTCOME}"), not 640"

# Symlinks planted where root is about to write. One where a guessable
# temporary name would be (the name an earlier draft of the helper used) is
# never used, because the names are random; one at the log path and one at the
# outcome path are refused before anything is stopped, with nothing written.
# The victim they point at is the same afterwards in every case.
setup planted-symlinks
write_request
victim="${WORK}/${CASE}/victim"
printf 'untouched\n' > "${victim}"
ln -s "${victim}" "${UPDATES}/last-apply.txt.tmp"
run_helper
expect_rc 0
expect_outcome_starts "ok 9.9.9"
[ "$(cat "${victim}")" = "untouched" ] && pass || fail "a planted symlink was followed: the victim now holds '$(cat "${victim}")'"
[ -L "${UPDATES}/last-apply.txt.tmp" ] && pass || fail "the planted .tmp symlink was touched"
rm -f "${OUTCOME}" "${LOG}" "${FAKE_DIR}/calls"
ln -s "${victim}" "${LOG}"
write_request
run_helper
expect_rc 4
expect_output "log is a symbolic link"
[ "$(cat "${victim}")" = "untouched" ] && pass || fail "the log symlink was followed"
expect_no_call "systemctl"
rm -f "${LOG}" "${FAKE_DIR}/calls"
ln -s "${victim}" "${OUTCOME}"
write_request
run_helper
expect_rc 4
expect_output "outcome is a symbolic link"
[ "$(cat "${victim}")" = "untouched" ] && pass || fail "the outcome symlink was followed"
expect_no_call "systemctl"

setup rolled-back-deb
write_request
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database is at schema version 6108, as before the install; hMailServer 9.9.8 was reinstalled and the service is running"
expect_call_re "apt-get install -y -q --no-install-recommends --allow-downgrades -o DPkg::Lock::Timeout=300 -o Dpkg::Use-Pty=0 -o Dpkg::Progress-Fancy=0 $(copy_of rollback hmailserver_9.9.8_amd64.deb)"
expect_no_copies_left
[ "$(calls | grep -c 'systemctl stop')" = 2 ] && pass || fail "expected two stops (one before the rollback)"
[ "$(calls | grep -c 'systemctl start')" = 2 ] && pass || fail "expected two starts"

setup no-rollback-deb
write_request
sed -i '/^rollback/d' "${REQUEST}"
printf 'failed\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "no-rollback 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database is at schema version 6108, as before the install; no rollback image was available"
[ "$(calls | grep -c 'apt-get')" = 1 ] && pass || fail "expected exactly one install"

setup rollback-failed-deb
write_request
printf 'failed\nfailed\n' > "${FAKE_DIR}/start-plan"
printf '0\n7\n' > "${FAKE_DIR}/install-plan"
run_helper
expect_rc 1
expect_outcome_starts "rollback-failed 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database is at schema version 6108, as before the install; the rollback to 9.9.8 exited with 7 and the service is not back (failed)"

setup failed-but-running-deb
write_request
printf '1\n' > "${FAKE_DIR}/install-plan"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.9 the install step exited with 1 but the service is running"
[ "$(calls | grep -c 'apt-get')" = 1 ] && pass || fail "a running service is not rolled back"

setup installer-failed-and-down-deb
write_request
printf '2\n' > "${FAKE_DIR}/install-plan"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9 the install step exited with 2 and the service was not back (failed); the database is at schema version 6108, as before the install; hMailServer 9.9.8 was reinstalled"

setup dpkg-fallback
write_request
run_helper apt-get
expect_rc 0
expect_outcome_starts "ok 9.9.9"
expect_call_re "dpkg -i $(copy_of package hmailserver_9.9.9_amd64.deb)"
expect_no_call "apt-get"

setup rpm-with-dnf
write_request
sed -i 's/^kind=.*/kind=rpm/' "${REQUEST}"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9"
expect_call_re "dnf install -y $(copy_of package hmailserver_9.9.9_amd64.deb)"
expect_call_re "rpm -Uvh --oldpackage $(copy_of rollback hmailserver_9.9.8_amd64.deb)"

setup rpm-without-dnf
write_request
sed -i 's/^kind=.*/kind=rpm/' "${REQUEST}"
run_helper dnf
expect_rc 0
expect_call_re "rpm -Uvh $(copy_of package hmailserver_9.9.9_amd64.deb)"
expect_no_call "dnf"

setup refused-outside
write_request
outside="${WORK}/${CASE}/elsewhere.deb"
printf 'outside\n' > "${outside}"
printf 'bundle\n' > "${outside}.cosign.bundle"
sed -i "s|^package=.*|package=${outside}|" "${REQUEST}"
run_helper
expect_rc 4
expect_output "the request was refused: package is not inside ${UPDATES}"
# A refusal is journal-only: the unit was not stopped, so the owner of the
# Updates directory may be alive, and root writes nothing there - not even
# the outcome.
[ -f "${OUTCOME}" ] && fail "a refusal wrote the outcome file, into a directory whose owner may be running" || pass
leftover=""
for f in "${UPDATES}"/*; do
   case "${f##*/}" in
      hmailserver_9.9.9_amd64.deb|hmailserver_9.9.9_amd64.deb.cosign.bundle|rollback) ;;
      *) leftover="${leftover} ${f##*/}" ;;
   esac
done
[ -z "${leftover}" ] && pass || fail "a refusal left something in the Updates directory:${leftover}"
expect_no_call "systemctl"
[ -f "${REQUEST}" ] && fail "a refused request was left in place" || pass

setup refused-symlink
write_request
ln -s /etc/hostname "${UPDATES}/hmailserver_9.9.9_amd64.deb.link"
sed -i "s|^package=.*|package=${UPDATES}/hmailserver_9.9.9_amd64.deb.link|" "${REQUEST}"
run_helper
expect_rc 4
expect_output "the request was refused: package is a symbolic link"
[ -f "${OUTCOME}" ] && fail "a refusal wrote the outcome file" || pass
expect_no_call "systemctl"

# Signed is not enough. A package older than the installed version is refused
# after the stop, with the unit started again; the request's own version line
# is not believed over the package's; and a rollback image that is not the
# installed version is dropped.
setup refused-downgrade
write_request
mv "${UPDATES}/hmailserver_9.9.9_amd64.deb" "${UPDATES}/hmailserver_9.9.7_amd64.deb"
mv "${UPDATES}/hmailserver_9.9.9_amd64.deb.cosign.bundle" "${UPDATES}/hmailserver_9.9.7_amd64.deb.cosign.bundle"
sed -i "s|^package=.*|package=${UPDATES}/hmailserver_9.9.7_amd64.deb|; s|^version=.*|version=9.9.7|" "${REQUEST}"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.7 the package is version 9.9.7 and the installed version is 9.9.8: not an upgrade, so nothing was installed"
expect_call "systemctl stop hmailserver.service"
expect_call "systemctl start hmailserver.service"
expect_no_call "apt-get"
expect_no_call "dpkg -i"

setup refused-same-version
write_request
printf '9.9.9' > "${FAKE_DIR}/installed-version"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.9 the package is version 9.9.9 and the installed version is 9.9.9: not an upgrade"
expect_no_call "apt-get"

setup request-version-not-believed
write_request
printf '9.9.10' > "${FAKE_DIR}/package-version"
run_helper
expect_rc 0
expect_outcome_starts "ok 9.9.10 the package was installed"
expect_output "the request says version 9.9.9 and the package says 9.9.10"

setup rollback-not-the-installed-version
write_request
mv "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb" "${UPDATES}/rollback/hmailserver_9.9.6_amd64.deb"
mv "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb.cosign.bundle" "${UPDATES}/rollback/hmailserver_9.9.6_amd64.deb.cosign.bundle"
sed -i "s|^rollback=.*|rollback=${UPDATES}/rollback/hmailserver_9.9.6_amd64.deb|; s|^rollback_version=.*|rollback_version=9.9.8|" "${REQUEST}"
printf 'failed\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "no-rollback 9.9.9"
expect_output "the rollback image is version '9.9.6' and the installed version is '9.9.8'; continuing without one"
[ "$(calls | grep -c 'apt-get')" = 1 ] && pass || fail "a rollback image of the wrong version was installed"

# No installed version, no install. The version rule cannot be held without
# one, and a machine whose package database does not know hmailserver is not
# one installed the way the request's kind says: a kind=rpm request on a .deb
# machine reads nothing from rpm -q, and before 6.3.6 that was installed "over
# whatever is here", signed but of any version.
setup no-installed-version
write_request
printf '' > "${FAKE_DIR}/installed-version"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.9 the installed version could not be read (dpkg has no installed hmailserver package), so this is not a deb installation that can be upgraded and nothing was installed"
expect_call "systemctl start hmailserver.service"
expect_no_call "apt-get"
expect_no_call "dpkg -i"

# The unit stopped is root's: hmailserver.service, or the one --unit names.
# The request's unit= is held to it, and refused when it differs - postgresql's
# unit, and as much a unit of the server's name the helper was not given. Before
# 6.3.6 the second was stopped in place of the server's, and everything after
# rested on a stop that had not stopped the server.
setup refused-unit
write_request
sed -i 's/^unit=.*/unit=postgresql.service/' "${REQUEST}"
run_helper
expect_rc 4
expect_output "the request says the server runs under 'postgresql.service', and this program stops hmailserver.service"
expect_no_call "systemctl"

setup refused-unit-not-configured
write_request
sed -i 's/^unit=.*/unit=hmailserver-test.service/' "${REQUEST}"
run_helper
expect_rc 4
expect_output "the request says the server runs under 'hmailserver-test.service', and this program stops hmailserver.service"
expect_no_call "systemctl"
[ -f "${OUTCOME}" ] && fail "a refusal wrote the outcome file" || pass

setup configured-unit
write_request
sed -i 's/^unit=.*/unit=hmailserver-test.service/' "${REQUEST}"
run_helper "" --unit hmailserver-test.service
expect_rc 0
expect_outcome_starts "ok 9.9.9"
expect_call "systemctl stop hmailserver-test.service"
expect_call "systemctl start hmailserver-test.service"
expect_no_call "systemctl stop hmailserver.service"

# A request without unit= - none the server writes, but the key was always
# optional - stops the configured unit.
setup request-without-unit
write_request
sed -i '/^unit=/d' "${REQUEST}"
run_helper
expect_rc 0
expect_call "systemctl stop hmailserver.service"

setup refused-own-unit
write_request
sed -i 's/^unit=.*/unit=hmailserver-update.service/' "${REQUEST}"
run_helper "" --unit hmailserver-update.service
expect_rc 4
expect_output "the unit to stop is this program's own"
expect_no_call "systemctl"

# Stopped is looked at, not assumed. A stop that leaves the unit active - a
# unit that is not the one the server runs under, or one that would not stop -
# is followed by nothing: no verification, no install, no log and no outcome in
# a directory whose owner may still be running; the unit is started again.
setup stop-not-confirmed
write_request
printf 'active\n' > "${FAKE_DIR}/stop-state"
run_helper
expect_rc 1
expect_output "hmailserver.service is active after the stop"
expect_call "systemctl stop hmailserver.service"
expect_call "systemctl start hmailserver.service"
expect_no_call "hmailserver --verify-update"
expect_no_call "apt-get"
[ -f "${OUTCOME}" ] && fail "an unconfirmed stop wrote the outcome file" || pass
[ -f "${LOG}" ] && fail "an unconfirmed stop wrote the log file" || pass

# And no process may run as the owner of the Updates directory (or of a
# directory above it) once the unit has stopped: here one runs as uid 65534,
# which owns the case's directories. Root alone can arrange that, so as any
# other user the case says it is skipped.
if [ "$(id -u)" = 0 ] && command -v setpriv >/dev/null 2>&1; then
   setup service-user-process-alive
   write_request
   chown -R 65534:65534 "${WORK}/${CASE}"
   setpriv --reuid=65534 --regid=65534 --clear-groups sleep 60 &
   sleeper=$!
   run_helper
   kill "${sleeper}" 2>/dev/null
   wait "${sleeper}" 2>/dev/null
   expect_rc 1
   expect_output "process ${sleeper} runs as uid 65534, which owns ${UPDATES} or a directory above it, after hmailserver.service was stopped"
   expect_call "systemctl start hmailserver.service"
   expect_no_call "hmailserver --verify-update"
   expect_no_call "apt-get"
   [ -f "${OUTCOME}" ] && fail "a live service user's directory got the outcome file" || pass
   # The same directories with nobody of theirs running: the apply goes on.
   rm -f "${FAKE_DIR}/calls"
   write_request
   run_helper
   expect_rc 0
   expect_outcome_starts "ok 9.9.9"
   [ "$(stat -c %u "${OUTCOME}")" = 65534 ] && pass || fail "the outcome is not owned like the Updates directory"
else
   echo "== service-user-process-alive: skipped (needs root and setpriv)"
fi

# What is installed is what was verified. A file in the Updates directory that
# is replaced after root's verification - by a process that outlived the stop -
# is not what root installs: root verified and installs its own copy.
setup verified-copy-installed
write_request
printf 'SWAPPED bytes\n' > "${FAKE_DIR}/swap-with"
printf '%s\n' "${UPDATES}/hmailserver_9.9.9_amd64.deb" > "${FAKE_DIR}/swap-after-verify"
run_helper
expect_rc 0
expect_installed_bytes "apt-get new package bytes"
expect_no_copies_left

# Root's copies are root's alone until they have verified: whatever the
# requester named - a hard link to a file of somebody else's is a plain file
# too - is readable by nobody else while it is copied and verified. Once the
# package has verified it is a signed release, and apt's own unprivileged user
# reads it by its name.
setup copies-private-until-verified
write_request
run_helper
expect_rc 0
modes="$(sort -u "${FAKE_DIR}/verified-modes" 2>/dev/null)"
[ "${modes}" = "600 700" ] && pass || fail "a file was verified where others could read it (the file, its directory): ${modes:-none recorded}"
[ "$(cat "${FAKE_DIR}/installed-modes" 2>/dev/null)" = "apt-get 644" ] && pass || fail "the verified package was not readable by apt's own user: $(cat "${FAKE_DIR}/installed-modes" 2>/dev/null)"

# The request is checked while the service user still runs, and what it does
# after that, before it is stopped, is not believed: a package or a rollback
# image replaced by a symlink to a file of somebody else's is looked at again
# once the service has stopped, and is neither copied nor read. The unit is
# started again and the outcome says why.
setup swapped-for-symlink-before-stop
write_request
secret="${WORK}/${CASE}/secret"
printf 'SECRET of somebody else\n' > "${secret}"
chmod 0600 "${secret}"
printf 'rm -f "%s"; ln -s "%s" "%s"\n' "${UPDATES}/hmailserver_9.9.9_amd64.deb" "${secret}" "${UPDATES}/hmailserver_9.9.9_amd64.deb" > "${FAKE_DIR}/on-stop"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.9 the package was no longer a plain file inside ${UPDATES} when the service had stopped"
expect_call "systemctl start hmailserver.service"
expect_no_call "hmailserver --verify-update"
expect_no_call "apt-get"
grep -q SECRET "${FAKE_DIR}/verified-bytes" 2>/dev/null && fail "root read the symlink's target as the package" || pass
expect_no_copies_left

setup rollback-swapped-for-symlink-before-stop
write_request
secret="${WORK}/${CASE}/secret"
printf 'SECRET of somebody else\n' > "${secret}"
chmod 0600 "${secret}"
printf 'rm -f "%s"; ln -s "%s" "%s"\n' "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb" "${secret}" "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb" > "${FAKE_DIR}/on-stop"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.9 the rollback image was no longer a plain file inside ${UPDATES} when the service had stopped"
expect_no_call "apt-get"
grep -q SECRET "${FAKE_DIR}/verified-bytes" 2>/dev/null && fail "root read the symlink's target as the rollback image" || pass
expect_no_copies_left

# A key this script does not know is said and skipped, not refused: an AppImage
# host keeps the script it was given while the swap refreshes the image, so a
# newer server must be able to write a key an older script has never seen. The
# apply goes on exactly as ok-deb does.
setup tolerated-unknown-key
write_request
echo "helper=/tmp/evil" >> "${REQUEST}"
run_helper
expect_rc 0
expect_output "ignoring a key this program does not know: helper"
expect_call "systemctl stop hmailserver.service"
expect_call "systemctl start hmailserver.service"

# A request's lines are read as written, never as patterns. The request is split
# by an unquoted expansion with IFS a newline, and without set -f a line holding
# a wildcard was expanded against the helper's working directory: here one that
# holds a file named "kind=deb", which turned the line "kind=de?" into kind=deb
# and applied the update. Read as written, the kind is refused.
setup no-pathname-expansion
write_request
sed -i 's/^kind=.*/kind=de?/' "${REQUEST}"
mkdir -p "${WORK}/${CASE}/cwd"
: > "${WORK}/${CASE}/cwd/kind=deb"
test_directory="$(pwd)"
cd "${WORK}/${CASE}/cwd" || exit 2
run_helper
cd "${test_directory}" || exit 2
expect_rc 4
expect_output "kind must be deb, rpm or appimage: 'de?'"
expect_no_call "systemctl"

setup refused-outcome-elsewhere
write_request
sed -i "s|^outcome=.*|outcome=${WORK}/${CASE}/last-apply.txt|" "${REQUEST}"
run_helper
expect_rc 4
expect_output "outcome is not directly inside ${UPDATES}"
[ -f "${WORK}/${CASE}/last-apply.txt" ] && fail "an outcome was written outside the Updates directory" || pass
expect_no_call "systemctl"

setup tampered
write_request
printf 'TAMPERED bytes\n' > "${UPDATES}/hmailserver_9.9.9_amd64.deb"
run_helper
expect_rc 1
expect_outcome_starts "failed 9.9.9 the package did not verify against its Sigstore bundle when checked as root, so nothing was installed"
expect_call "systemctl stop hmailserver.service"
expect_call "systemctl start hmailserver.service"
expect_no_call "apt-get"

setup rollback-image-unverified
write_request
printf 'TAMPERED bytes\n' > "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb"
printf 'failed\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "no-rollback 9.9.9"
expect_output "the rollback image did not verify; continuing without one"

# The AppImage: the image is a script that answers --verify-update the way the
# fake server does, because the RUNNING image is the verifier of the new one -
# and answers --version, because it is also where the installed version comes
# from. The new image answers --version too: a verified image is run for it.
new_image()
{
   # It answers --schema-version as the fake server does, from the case's plan.
   printf '#!/bin/sh\n# NEW IMAGE\n[ "$1" = "--schema-version" ] && exec "%s" --schema-version\necho "hMailServer %s build 0"\n' "${FAKE_BIN}/hmailserver" "$1" > "$2"
   chmod 0755 "$2"
   printf 'bundle\n' > "$2.cosign.bundle"
}

# An AppImage's copies are made beside the image, not under TMPDIR: the copy is
# run for its version, and /var/tmp is often mounted noexec, while the image's
# directory - root's own - is one the image already runs from.
image_copy_of()
{
   printf '%s/\\.hmailserver-update\\.[A-Za-z0-9]{6}/%s/%s' "$(re_quote "${IMAGE_DIR}")" "$1" "$(re_quote "$2")"
}

expect_no_image_copies_left()
{
   local left
   left="$(find "${IMAGE_DIR}" -maxdepth 1 -name '.hmailserver-update.*' 2>/dev/null)"
   [ -z "${left}" ] && pass || fail "the helper left its copies beside the image: ${left}"
}

setup appimage-ok
write_request
IMAGE_DIR="${WORK}/${CASE}/opt"
mkdir -p "${IMAGE_DIR}"
chmod 0755 "${IMAGE_DIR}"
IMAGE="${IMAGE_DIR}/hMailServer.AppImage"
cp "${FAKE_BIN}/hmailserver" "${IMAGE}"
chmod 0755 "${IMAGE}"
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
printf 'OLD IMAGE\n' > "${UPDATES}/rollback/hMailServer-9.9.8-x86_64.AppImage"
printf 'bundle\n' > "${UPDATES}/rollback/hMailServer-9.9.8-x86_64.AppImage.cosign.bundle"
cat > "${REQUEST}" <<EOF
kind=appimage
version=9.9.9
package=${UPDATES}/hMailServer-9.9.9-x86_64.AppImage
rollback=${UPDATES}/rollback/hMailServer-9.9.8-x86_64.AppImage
rollback_version=9.9.8
unit=hmailserver.service
outcome=${OUTCOME}
log=${LOG}
wait=2
image=${IMAGE}
EOF
run_helper "" --image "${IMAGE}"
expect_rc 0
expect_outcome_starts "ok 9.9.9"
grep -q "NEW IMAGE" "${IMAGE}" && pass || fail "the image was not replaced"
[ -f "${IMAGE}.previous" ] && grep -q "verify-update" "${IMAGE}.previous" && pass || fail "the previous image was not kept"
[ "$(stat -c %a "${IMAGE}")" = 755 ] && pass || fail "the new image did not take the old one's mode"
expect_no_call "apt-get"
expect_no_call "dpkg"
# Verified as root's own copy beside the image, readable and runnable by root
# alone (the copy, its directory), and gone afterwards.
expect_call_re "hmailserver --verify-update $(image_copy_of package hMailServer-9.9.9-x86_64.AppImage)"
expect_no_call "hmailserver --verify-update ${PRIVATE_TMP}/"
[ "$(head -n 1 "${FAKE_DIR}/verified-modes" 2>/dev/null)" = "700 700" ] && pass || fail "the AppImage was verified where others could read it: $(head -n 1 "${FAKE_DIR}/verified-modes" 2>/dev/null)"
expect_no_image_copies_left

setup appimage-not-newer
IMAGE_DIR="${WORK}/${CASE}/opt"
mkdir -p "${IMAGE_DIR}"
chmod 0755 "${IMAGE_DIR}"
IMAGE="${IMAGE_DIR}/hMailServer.AppImage"
cp "${FAKE_BIN}/hmailserver" "${IMAGE}"
chmod 0755 "${IMAGE}"
new_image 9.9.8 "${UPDATES}/hMailServer-9.9.8-x86_64.AppImage"
cat > "${REQUEST}" <<EOF
kind=appimage
version=9.9.8
package=${UPDATES}/hMailServer-9.9.8-x86_64.AppImage
unit=hmailserver.service
outcome=${OUTCOME}
wait=2
image=${IMAGE}
EOF
run_helper "" --image "${IMAGE}"
expect_rc 1
expect_outcome_starts "failed 9.9.8 the package is version 9.9.8 and the installed version is 9.9.8: not an upgrade"
grep -q "verify-update" "${IMAGE}" && pass || fail "the running image was replaced by one that is not newer"

setup appimage-rolled-back
IMAGE_DIR="${WORK}/${CASE}/opt"
mkdir -p "${IMAGE_DIR}"
chmod 0755 "${IMAGE_DIR}"
IMAGE="${IMAGE_DIR}/hMailServer.AppImage"
cp "${FAKE_BIN}/hmailserver" "${IMAGE}"
chmod 0755 "${IMAGE}"
old_sum="$(cat "${IMAGE}" | wc -c)"
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
cat > "${REQUEST}" <<EOF
kind=appimage
version=9.9.9
package=${UPDATES}/hMailServer-9.9.9-x86_64.AppImage
rollback=${UPDATES}/hMailServer-9.9.9-x86_64.AppImage
rollback_version=9.9.8
unit=hmailserver.service
outcome=${OUTCOME}
log=${LOG}
wait=2
image=${IMAGE}
EOF
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper "" --image "${IMAGE}"
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9"
[ "$(cat "${IMAGE}" | wc -c)" = "${old_sum}" ] && grep -q "verify-update" "${IMAGE}" && pass || fail "the old image was not put back"
[ -f "${IMAGE}.previous" ] && fail "the .previous copy was left behind after the rollback" || pass

# An image anyone else can write to is one root will not run: every mode with
# a write bit for the group or for others is refused - 0777, and the ones a
# check of "digits up to 5" would let through, 0620 and 0642.
for mode in 0777 0620 0642 0664; do
setup "appimage-refused-writable-${mode}"
IMAGE_DIR="${WORK}/${CASE}/opt"
mkdir -p "${IMAGE_DIR}"
chmod 0755 "${IMAGE_DIR}"
IMAGE="${IMAGE_DIR}/hMailServer.AppImage"
cp "${FAKE_BIN}/hmailserver" "${IMAGE}"
chmod "${mode}" "${IMAGE}"
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
cat > "${REQUEST}" <<EOF
kind=appimage
version=9.9.9
package=${UPDATES}/hMailServer-9.9.9-x86_64.AppImage
unit=hmailserver.service
outcome=${OUTCOME}
wait=2
image=${IMAGE}
EOF
run_helper "" --image "${IMAGE}"
expect_rc 4
expect_output "image is writable by its group or by others"
expect_no_call "systemctl"
done

setup appimage-refused-writable-directory
IMAGE_DIR="${WORK}/${CASE}/opt"
mkdir -p "${IMAGE_DIR}"
chmod 0730 "${IMAGE_DIR}"
IMAGE="${IMAGE_DIR}/hMailServer.AppImage"
cp "${FAKE_BIN}/hmailserver" "${IMAGE}"
chmod 0755 "${IMAGE}"
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
cat > "${REQUEST}" <<EOF
kind=appimage
version=9.9.9
package=${UPDATES}/hMailServer-9.9.9-x86_64.AppImage
unit=hmailserver.service
outcome=${OUTCOME}
wait=2
image=${IMAGE}
EOF
run_helper "" --image "${IMAGE}"
expect_rc 4
expect_output "the image's directory is writable by its group or by others (mode 730)"
expect_no_call "systemctl"

# The verifier of an AppImage is the image root's own configuration names
# (--image), never one the request names, and an AppImage request is refused
# where none is named. Before 6.3.6 the request's image= was the verifier: any
# program owned by root that exits 0 - a copy of true here - "verified"
# anything, and the package, a script the service user wrote into the Updates
# directory, was then run as root for its version and swapped in over that
# program. The payload records that it ran; it must not.
image_setup()
{
   IMAGE_DIR="${WORK}/${CASE}/opt"
   mkdir -p "${IMAGE_DIR}"
   chmod 0755 "${IMAGE_DIR}"
   IMAGE="${IMAGE_DIR}/hMailServer.AppImage"
   cp "${FAKE_BIN}/hmailserver" "${IMAGE}"
   chmod 0755 "${IMAGE}"
}

true_copy()
{
   TRUE_DIR="${WORK}/${CASE}/usr-bin"
   mkdir -p "${TRUE_DIR}"
   chmod 0755 "${TRUE_DIR}"
   cp "$(type -P true)" "${TRUE_DIR}/true"
   chmod 0755 "${TRUE_DIR}/true"
}

payload()
{
   printf '#!/bin/sh\necho "ran as $(id -u) with $*" > "%s/PAYLOAD_RAN"\necho "hMailServer 99.0.0 build 0"\n' "${FAKE_DIR}" > "$1"
   chmod 0755 "$1"
}

# An AppImage request: $1 the package's name in the Updates directory, $2 the
# version, $3 the image= line's value (none when empty).
appimage_request()
{
   {
      echo "kind=appimage"
      echo "version=$2"
      echo "package=${UPDATES}/$1"
      echo "unit=hmailserver.service"
      echo "outcome=${OUTCOME}"
      echo "log=${LOG}"
      echo "wait=2"
      [ -z "$3" ] || echo "image=$3"
   } > "${REQUEST}"
}

expect_no_payload()
{
   [ -f "${FAKE_DIR}/PAYLOAD_RAN" ] && fail "the requester's package ran: $(cat "${FAKE_DIR}/PAYLOAD_RAN")" || pass
}

setup appimage-refused-unconfigured
true_copy
payload "${UPDATES}/evil.AppImage"
appimage_request evil.AppImage 99.0.0 "${TRUE_DIR}/true"
run_helper
expect_rc 4
expect_output "kind=appimage, and this program's configuration names no AppImage (--image)"
expect_no_payload
cmp -s "${TRUE_DIR}/true" "$(type -P true)" && pass || fail "the program the request named as the image was replaced"
expect_no_call "systemctl"
[ -f "${OUTCOME}" ] && fail "a refusal wrote the outcome file" || pass

setup appimage-refused-other-image
image_setup
true_copy
payload "${UPDATES}/evil.AppImage"
appimage_request evil.AppImage 99.0.0 "${TRUE_DIR}/true"
run_helper "" --image "${IMAGE}"
expect_rc 4
expect_output "the request names the image ${TRUE_DIR}/true, and this program's configuration names ${IMAGE}"
expect_no_payload
cmp -s "${TRUE_DIR}/true" "$(type -P true)" && pass || fail "the program the request named as the image was replaced"
expect_no_call "systemctl"

# With the image configured, an unsigned package is verified by that image, on
# root's copy of it, refuses, and is never run.
setup appimage-unsigned-not-run
image_setup
payload "${UPDATES}/evil.AppImage"
appimage_request evil.AppImage 99.0.0 "${IMAGE}"
run_helper "" --image "${IMAGE}"
expect_rc 1
expect_outcome_starts "failed 99.0.0 the package did not verify against its Sigstore bundle when checked as root"
expect_call_re "hmailserver --verify-update $(image_copy_of package evil.AppImage)"
expect_no_payload
grep -q "verify-update" "${IMAGE}" && pass || fail "the image was replaced by a package that did not verify"
expect_no_image_copies_left

# What is run for its version, and swapped in, is root's verified copy: the
# file in the Updates directory replaced by a payload after the verification
# is neither.
setup appimage-verified-copy-run
image_setup
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
payload "${FAKE_DIR}/swap-with"
printf '%s\n' "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage" > "${FAKE_DIR}/swap-after-verify"
appimage_request hMailServer-9.9.9-x86_64.AppImage 9.9.9 "${IMAGE}"
run_helper "" --image "${IMAGE}"
expect_rc 0
expect_outcome_starts "ok 9.9.9"
expect_no_payload
grep -q "NEW IMAGE" "${IMAGE}" && pass || fail "what was swapped in is not the image that verified"

# An image request on a machine configured as an AppImage is the only kind it
# takes; a package request there is refused.
setup deb-refused-on-appimage-host
image_setup
write_request
run_helper "" --image "${IMAGE}"
expect_rc 4
expect_output "kind=deb, and this program's configuration names the AppImage ${IMAGE}"
expect_no_call "systemctl"

# An image that does not say its version cannot be held to the version rule.
setup appimage-no-installed-version
image_setup
printf '' > "${FAKE_DIR}/installed-version"
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
appimage_request hMailServer-9.9.9-x86_64.AppImage 9.9.9 "${IMAGE}"
run_helper "" --image "${IMAGE}"
expect_rc 1
expect_outcome_starts "failed 9.9.9 the installed version could not be read (the image ${IMAGE} did not say its version)"
grep -q "verify-update" "${IMAGE}" && pass || fail "the image was replaced although the installed version was not known"

setup dry-run
write_request
run_helper "" --dry-run
expect_rc 0
expect_output "would run: systemctl stop hmailserver.service"
expect_output "would verify the package"
expect_output "would run: apt-get install"
expect_output "would read the database's schema version with ${FAKE_BIN}/hmailserver --schema-version"
expect_output "outcome: ok 9.9.9"
[ -f "${OUTCOME}" ] && fail "a dry run wrote the outcome file" || pass
[ -f "${FAKE_DIR}/calls" ] && fail "a dry run ran something: $(calls | tr '\n' ';')" || pass
expect_no_copies_left

# ---------------------------------------------------------------- the database
#
# A rollback puts back the earlier version's program and never its database, and
# that version refuses a database newer than the one it needs. The new version's
# schema step - the package's postinst running --upgrade-database, or the server's
# start with UpgradeOnStart - may have moved the database before its server failed
# to come back. So the helper reads the database's version with
# `hmailserver --schema-version`, as the unit's user, before the install and again
# then, and puts the earlier version back only while it is unchanged: once it has
# moved, or nothing can show it has not, the outcome is upgraded-incomplete and
# nothing is reinstalled. Before 6.3.6 every one of these was rolled back.

# The program as the unit runs it, and the calls in the order they came: the
# read before the install, the install, the read after it.
schema_read_as()
{
   printf 'runuser -u %s -- env %s --schema-version' "$1" "$(re_quote "${FAKE_BIN}/hmailserver")"
}

setup db-moved-not-rolled-back
write_request
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6105' > "${FAKE_DIR}/required"
printf '6108' > "${FAKE_DIR}/install-required"
printf '6108' > "${FAKE_DIR}/install-moves-db-to"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6105 before the install and is at 6108 now; hMailServer 9.9.8 was not put back, since it may not run on that database; put right what keeps the service from starting (journalctl -u hmailserver.service) and start it - a schema step that did not finish is finished with hmailserver --upgrade-database, run as the user the service runs as; or, to go back to hMailServer 9.9.8, restore the backup taken before the update, if one was taken (UpdateBackupBeforeApply: the newest archive in the backup destination), and reinstall it; see ${LOG}"
expect_one_line
[ "$(calls | grep -c '^apt-get')" = 1 ] && pass || fail "the earlier version was put back onto the database the new one's schema step had moved: $(calls | grep '^apt-get' | tr '\n' ';')"
[ "$(calls | grep -c '^systemctl stop')" = 1 ] && pass || fail "the unit was stopped again for a rollback"
[ "$(calls | grep -cE "^$(schema_read_as hmailserver)\$")" = 2 ] && pass || fail "the database's version was not read twice as the unit's user, hmailserver: $(calls | grep -E 'schema' | tr '\n' ';')"
order="$(calls | grep -E '^(runuser|apt-get|systemctl start)' | cut -d' ' -f1-2 | tr '\n' ',')"
[ "${order}" = "runuser -u,apt-get install,systemctl start,runuser -u," ] && pass || fail "the reads were not one before the install and one after the start: ${order}"
expect_output "hMailServer 9.9.8 is not put back: the database has moved, or may have"
grep -q "the database is at schema version 6105 before the install" "${LOG}" && pass || fail "the log lacks the version read before the install"
expect_no_copies_left

# The schema step stopped part-way: moved, and short of what the new one needs.
setup db-moved-part-way-not-rolled-back
write_request
printf '6105' > "${FAKE_DIR}/db-version"
printf '6105' > "${FAKE_DIR}/required"
printf '6108' > "${FAKE_DIR}/install-required"
printf '6106' > "${FAKE_DIR}/install-moves-db-to"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6105 before the install and is at 6106 now, short of the 6108 hMailServer 9.9.9 needs; hMailServer 9.9.8 was not put back, since it may not run on that database; the database upgrade stopped part-way: once its cause is put right, finish it with hmailserver --upgrade-database, run as the user the service runs as, and start the service; or restore the backup taken before the update, if one was taken (UpdateBackupBeforeApply: the newest archive in the backup destination), and reinstall hMailServer 9.9.8"
[ "$(calls | grep -c '^apt-get')" = 1 ] && pass || fail "a database the schema step left part-way was rolled back"

# A schema-changing release whose step did not move the database (it failed at
# its first step, which a transaction takes back): unchanged, and put back - and
# the outcome says what the rollback rested on.
setup db-unchanged-rolled-back
write_request
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6105' > "${FAKE_DIR}/required"
printf '6108' > "${FAKE_DIR}/install-required"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database is at schema version 6105, as before the install; hMailServer 9.9.8 was reinstalled and the service is running"
expect_call_re "apt-get install -y -q --no-install-recommends --allow-downgrades -o DPkg::Lock::Timeout=300 -o Dpkg::Use-Pty=0 -o Dpkg::Progress-Fancy=0 $(copy_of rollback hmailserver_9.9.8_amd64.deb)"
[ "$(calls | grep -cE "^$(schema_read_as hmailserver)\$")" = 2 ] && pass || fail "the database's version was not read twice as hmailserver"

# Not readable after the install - the new program does not reach the database -
# with a database short of what the new version needs: the install may have
# moved it, and nothing is put back.
setup db-unreadable-after-not-rolled-back
write_request
printf '6105' > "${FAKE_DIR}/db-version"
printf '6105' > "${FAKE_DIR}/required"
printf '6108' > "${FAKE_DIR}/install-required"
: > "${FAKE_DIR}/install-breaks-db-read"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6105 before the install, and its version now could not be read (${FAKE_BIN}/hmailserver --schema-version exited with 1: Could not connect to the database: (fake) connection refused); hMailServer 9.9.9 needs 6108, so the install may have moved it; hMailServer 9.9.8 was not put back"
[ "$(calls | grep -c '^apt-get')" = 1 ] && pass || fail "a database that may have moved was rolled back"

# The same, with a database already at what the new version needs: nothing in
# the install moves such a database, so it is unchanged, and put back.
setup db-unreadable-at-required-rolled-back
write_request
: > "${FAKE_DIR}/install-breaks-db-read"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6108 before the install, at or past the 6108 hMailServer 9.9.9 needs, so nothing in the install moved it, though its version now could not be read (${FAKE_BIN}/hmailserver --schema-version exited with 1: Could not connect to the database: (fake) connection refused); hMailServer 9.9.8 was reinstalled"
[ "$(calls | grep -c '^apt-get')" = 2 ] && pass || fail "an update that changes no schema was not put back"

# Not readable before the install - a program from before --schema-version -
# leaves nothing to hold the database to afterwards: not put back.
setup db-unreadable-before-not-rolled-back
write_request
: > "${FAKE_DIR}/no-schema-verb"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database's schema version could not be read before the install (${FAKE_BIN}/hmailserver --schema-version exited with 2: Unknown option: --schema-version Try --help.), so nothing shows that the install left it unchanged; hMailServer 9.9.8 was not put back"
expect_output "the database's schema version could not be read before the install"
[ "$(calls | grep -c '^apt-get')" = 1 ] && pass || fail "the earlier version was put back with nothing to show the database unchanged"

# No rollback image, and the database moved: upgraded-incomplete, not
# no-rollback - what has to be done about it is not the same.
setup db-moved-without-rollback-image
write_request
sed -i '/^rollback/d' "${REQUEST}"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6108' > "${FAKE_DIR}/install-moves-db-to"
printf 'failed\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6105 before the install and is at 6108 now; hMailServer 9.9.8 was not put back"

# The backup the server took for this update, named in the outcome; a value with
# a control character in it is left out, and the backup named in general.
setup db-moved-backup-named
write_request
echo "backup=/var/backups/hmailserver (finished 2026-10-02 03:00:00)" >> "${REQUEST}"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6108' > "${FAKE_DIR}/install-moves-db-to"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_contains "or, to go back to hMailServer 9.9.8, restore the backup taken before the update, in /var/backups/hmailserver (finished 2026-10-02 03:00:00), and reinstall it"
grep -q "ignoring a key this program does not know: backup" <<< "${OUT}" && fail "the backup key was not known" || pass

setup db-moved-backup-with-control-character
write_request
printf 'backup=/var/backups/\033[31mhmailserver\n' >> "${REQUEST}"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6108' > "${FAKE_DIR}/install-moves-db-to"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_output "backup holds a control character and is left out"
expect_outcome_contains "restore the backup taken before the update, if one was taken (UpdateBackupBeforeApply: the newest archive in the backup destination), and reinstall it"
grep -q "$(printf '\033')" "${OUTCOME}" && fail "the control character reached the outcome" || pass

# The unit's User= is root's word, but not a user name is not run as anybody.
setup db-unit-user-not-a-name
write_request
printf 'some one\n' > "${FAKE_DIR}/unit-user"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database's schema version could not be read before the install (hmailserver.service runs the server as 'some one', which is not a user name this program runs anything as)"
expect_no_call "runuser"
expect_no_call "hmailserver --schema-version"

# An AppImage whose new image moved the database at its own start
# (UpgradeOnStart): not put back, the new image kept, and the version read with
# the unit's HMAILSERVER_HOME and XDG_DATA_HOME and nothing else of its
# environment.
setup appimage-db-moved-at-start-not-put-back
image_setup
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
printf 'OLD IMAGE\n' > "${UPDATES}/rollback/hMailServer-9.9.8-x86_64.AppImage"
printf 'bundle\n' > "${UPDATES}/rollback/hMailServer-9.9.8-x86_64.AppImage.cosign.bundle"
appimage_request hMailServer-9.9.9-x86_64.AppImage 9.9.9 "${IMAGE}"
{
   echo "rollback=${UPDATES}/rollback/hMailServer-9.9.8-x86_64.AppImage"
   echo "rollback_version=9.9.8"
} >> "${REQUEST}"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6108' > "${FAKE_DIR}/start-moves-db-to"
printf 'HMAILSERVER_HOME=/srv/hm "XDG_DATA_HOME=/srv/a b" EVIL=/x XDG_DATA_HOME=/srv/xdg\n' > "${FAKE_DIR}/unit-environment"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper "" --image "${IMAGE}"
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6105 before the install and is at 6108 now; hMailServer 9.9.8 was not put back"
expect_outcome_contains "finished with ${IMAGE} --upgrade-database"
grep -q "NEW IMAGE" "${IMAGE}" && pass || fail "the earlier image was put back onto the database the new one had moved"
[ "$(sort -u "${FAKE_DIR}/schema-env" 2>/dev/null | tr '\n' ' ')" = "HMAILSERVER_HOME=/srv/hm XDG_DATA_HOME=/srv/xdg " ] && pass || fail "the version was not read with the unit's HMAILSERVER_HOME and XDG_DATA_HOME alone: $(sort -u "${FAKE_DIR}/schema-env" 2>/dev/null | tr '\n' ' ')"
expect_no_image_copies_left

# ---------------------------------------------------------------- after the restart
#
# The stop is confirmed, and the Updates directory is quiet - until the unit is
# started again. From then the server runs as the directory's owner while root
# still has lines to log, the outcome to write and both to rename into place;
# before 6.3.6's review, root did all of that by name, appending to the log,
# chown-ing and chmod-ing it and the outcome without -h, and renaming onto
# names the server could have made links to directories. Here the fake
# systemctl's start runs a hook as the service user (as root, uid 65534, which
# owns the case's directories; as anyone else, the test's user) that plants
# links where root still has to write, and what the links lead to - a file or a
# directory that is not the service user's - must be the same afterwards, in
# what is in it, its mode and its owner.

# The log's file, under the random name it has until the end, made a symlink
# to somebody else's file.
setup log-swapped-after-start
write_request
as_the_service_user
victim="${WORK}/${CASE}/victim"
printf 'untouched\n' > "${victim}"
chmod 0600 "${victim}"
before="$(state_of "${victim}")"
cat > "${FAKE_DIR}/on-start" <<EOF
for f in "${UPDATES}"/apply.??????; do
   [ -e "\$f" ] || continue
   rm -f "\$f" && ln -s "${victim}" "\$f" && echo "\${f##*/}" >> "${FAKE_DIR}/hook-did"
done
EOF
run_helper
expect_rc 0
expect_outcome_starts "ok 9.9.9 the package was installed and the service is running"
[ -s "${FAKE_DIR}/hook-did" ] && pass || fail "the hook found no log file to swap at the start"
after="$(state_of "${victim}")"
[ "${after}" = "${before}" ] && pass || fail "the symlink planted at the log's name was followed: '${before}' became '${after}'"

# The names the log and the outcome are to take, made symlinks to directories
# that are somebody else's: a rename onto such a name must replace the link,
# not move root's file into the directory.
setup names-linked-to-directories-after-start
write_request
as_the_service_user
victim_a="${WORK}/${CASE}/victim-a"
victim_b="${WORK}/${CASE}/victim-b"
mkdir -p "${victim_a}" "${victim_b}"
chmod 0700 "${victim_a}" "${victim_b}"
before_a="$(state_of "${victim_a}")"
before_b="$(state_of "${victim_b}")"
cat > "${FAKE_DIR}/on-start" <<EOF
ln -s "${victim_a}" "${OUTCOME}" && echo outcome >> "${FAKE_DIR}/hook-did"
ln -s "${victim_b}" "${LOG}" && echo log >> "${FAKE_DIR}/hook-did"
EOF
run_helper
expect_rc 0
[ "$(wc -l < "${FAKE_DIR}/hook-did" 2>/dev/null)" = 2 ] && pass || fail "the hook did not plant both links at the start"
after_a="$(state_of "${victim_a}")"
after_b="$(state_of "${victim_b}")"
[ "${after_a}" = "${before_a}" ] && pass || fail "the outcome was moved into the directory a link at its name led to: '${before_a}' became '${after_a}'"
[ "${after_b}" = "${before_b}" ] && pass || fail "the log was moved into the directory a link at its name led to: '${before_b}' became '${after_b}'"
[ -f "${OUTCOME}" ] && [ ! -L "${OUTCOME}" ] && pass || fail "the outcome did not take its name in place of the link"
expect_outcome_starts "ok 9.9.9"
[ -f "${LOG}" ] && [ ! -L "${LOG}" ] && grep -q "outcome: ok" "${LOG}" && pass || fail "the log did not take its name in place of the link"

# The Updates directory itself renamed, and a symlink to somebody else's
# directory put where it was: what root still writes goes into the directory it
# took while the unit was stopped, wherever that now is, and nowhere else.
setup directory-swapped-after-start
write_request
as_the_service_user
victim="${WORK}/${CASE}/victim-dir"
mkdir -p "${victim}"
chmod 0700 "${victim}"
before="$(state_of "${victim}")"
cat > "${FAKE_DIR}/on-start" <<EOF
mv "${UPDATES}" "${UPDATES}.moved" && ln -s "${victim}" "${UPDATES}" && echo moved >> "${FAKE_DIR}/hook-did"
EOF
run_helper
expect_rc 0
[ -s "${FAKE_DIR}/hook-did" ] && pass || fail "the hook did not move the Updates directory at the start"
after="$(state_of "${victim}")"
[ "${after}" = "${before}" ] && pass || fail "root wrote into the directory a link at the Updates directory's name led to: '${before}' became '${after}'"
case "$(head -n 1 "${UPDATES}.moved/last-apply.txt" 2>/dev/null)" in
   "ok 9.9.9 "*) pass ;;
   *) fail "the outcome is not in the Updates directory the helper took: '$(head -n 1 "${UPDATES}.moved/last-apply.txt" 2>/dev/null)'" ;;
esac
grep -q "outcome: ok" "${UPDATES}.moved/apply-9.9.9.log" 2>/dev/null && pass || fail "the log is not in the Updates directory the helper took"

# The outcome's own file, made while the unit was stopped and so there at the
# restart under its random name, made a symlink to somebody else's file: the
# line is written through the descriptor, never through the link. (The helper
# before 6.3.6's review made this file only once the wait was over, so the
# hook finds none to swap there: its exposure was a race between mktemp and
# the write, which no hook can hit every time. This case holds the new
# design to what it opened up.)
setup outcome-file-swapped-after-start
write_request
as_the_service_user
victim="${WORK}/${CASE}/victim"
printf 'untouched\n' > "${victim}"
chmod 0600 "${victim}"
before="$(state_of "${victim}")"
cat > "${FAKE_DIR}/on-start" <<EOF
for f in "${UPDATES}"/last-apply.??????; do
   [ -e "\$f" ] || continue
   rm -f "\$f" && ln -s "${victim}" "\$f" && echo "\${f##*/}" >> "${FAKE_DIR}/hook-did"
done
EOF
run_helper
expect_rc 0
after="$(state_of "${victim}")"
[ "${after}" = "${before}" ] && pass || fail "the symlink planted at the outcome's file was followed: '${before}' became '${after}'"

# ---------------------------------------------------------------- the probe's sandbox
#
# `hmailserver --schema-version` is the server's binary, run as the service
# user, and the server is held in hmailserver.service's sandbox. Where systemd
# runs the machine the helper has systemd run the probe in the same sandbox -
# a transient service with the unit's user and the unit's own sandboxing
# properties, read from systemd - and never runs it outside one; where it does
# not, the probe runs through runuser, off any terminal. Before 6.3.6's review
# it always ran through runuser, outside the sandbox, and from `check` on the
# administrator's terminal.

# The unit's sandbox as `systemctl show` reports it, with two lines the helper
# did not ask for and must not pass on, and one empty value.
unit_sandbox()
{
   cat > "${FAKE_DIR}/unit-sandbox" <<'EOF'
Group=hmailserver
SupplementaryGroups=
WorkingDirectory=/var/lib/hmailserver
UMask=0027
NoNewPrivileges=yes
CapabilityBoundingSet=cap_net_bind_service
AmbientCapabilities=cap_net_bind_service
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/var/lib/hmailserver /var/log/hmailserver
PrivateTmp=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_NETLINK AF_UNIX
SystemCallArchitectures=x86-64
SystemCallFilter=_llseek accept access
SystemCallErrorNumber=EPERM
ExecStart={ path=/usr/bin/hmailserver ; argv[]=/usr/bin/hmailserver --foreground }
EOF
}

# The arguments of the first systemd-run call, one per line.
first_systemd_run()
{
   sed -n '1,/^==$/p' "${FAKE_DIR}/systemd-run-argv" 2>/dev/null | sed '$d'
}

# An argument of the first systemd-run call, by its whole line.
expect_sandbox_argument()
{
   first_systemd_run | grep -qxF -- "$1" && pass || fail "systemd-run was not given '$1'; it was given: $(first_systemd_run | tr '\n' ' ')"
}

expect_no_sandbox_argument()
{
   first_systemd_run | grep -qF -- "$1" && fail "systemd-run was given '$1'" || pass
}

setup schema-read-in-the-units-sandbox
write_request
systemd_here
unit_sandbox
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
printf '6105' > "${FAKE_DIR}/db-version"
printf '6105' > "${FAKE_DIR}/required"
printf '6108' > "${FAKE_DIR}/install-required"
printf '6108' > "${FAKE_DIR}/install-moves-db-to"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database was at schema version 6105 before the install and is at 6108 now; hMailServer 9.9.8 was not put back"
[ "$(calls | grep -c '^systemd-run ')" = 2 ] && pass || fail "the database's version was not asked through systemd-run before the install and after it: $(calls | grep -E 'systemd-run|runuser' | tr '\n' ';')"
expect_no_call "runuser"
[ "$(calls | grep -c '^hmailserver --schema-version')" = 2 ] && pass || fail "the probe did not run twice"
for argument in --quiet --wait --pipe --collect --uid=hmailserver RuntimeMaxSec=60 Group=hmailserver WorkingDirectory=/var/lib/hmailserver \
   UMask=0027 NoNewPrivileges=yes CapabilityBoundingSet=cap_net_bind_service ProtectSystem=strict ProtectHome=yes \
   "ReadWritePaths=/var/lib/hmailserver /var/log/hmailserver" PrivateTmp=yes "RestrictAddressFamilies=AF_INET AF_INET6 AF_NETLINK AF_UNIX" \
   SystemCallArchitectures=x86-64 "SystemCallFilter=_llseek accept access" SystemCallErrorNumber=EPERM \
   "${FAKE_BIN}/hmailserver" --schema-version; do
   expect_sandbox_argument "${argument}"
done
expect_no_sandbox_argument "AmbientCapabilities"
expect_no_sandbox_argument "ExecStart"
expect_no_sandbox_argument "SupplementaryGroups"
grep -q "the database is at schema version 6105 before the install (asked as hmailserver, in hmailserver.service's sandbox)" "${LOG}" && pass || fail "the log does not say the version was asked in the unit's sandbox"
[ "$(sort -u "${FAKE_DIR}/probe-tty" 2>/dev/null)" = none ] && pass || fail "the probe had a terminal: $(sort -u "${FAKE_DIR}/probe-tty" 2>/dev/null)"

# An AppImage's: the unit's HMAILSERVER_HOME and XDG_DATA_HOME given to the
# transient service, and nothing else of the unit's environment.
setup appimage-schema-read-in-the-units-sandbox
image_setup
systemd_here
unit_sandbox
new_image 9.9.9 "${UPDATES}/hMailServer-9.9.9-x86_64.AppImage"
appimage_request hMailServer-9.9.9-x86_64.AppImage 9.9.9 "${IMAGE}"
printf 'HMAILSERVER_HOME=/srv/hm "XDG_DATA_HOME=/srv/a b" EVIL=/x XDG_DATA_HOME=/srv/xdg\n' > "${FAKE_DIR}/unit-environment"
run_helper "" --image "${IMAGE}"
expect_rc 0
expect_outcome_starts "ok 9.9.9"
expect_sandbox_argument "--setenv=HMAILSERVER_HOME=/srv/hm"
expect_sandbox_argument "--setenv=XDG_DATA_HOME=/srv/xdg"
expect_no_sandbox_argument "EVIL"
expect_no_sandbox_argument "--uid="
expect_sandbox_argument "${IMAGE}"
[ "$(sort -u "${FAKE_DIR}/schema-env" 2>/dev/null | tr '\n' ' ')" = "HMAILSERVER_HOME=/srv/hm XDG_DATA_HOME=/srv/xdg " ] && pass || fail "the version was not read with the unit's HMAILSERVER_HOME and XDG_DATA_HOME alone: $(sort -u "${FAKE_DIR}/schema-env" 2>/dev/null | tr '\n' ' ')"

# systemd does not start the probe in the sandbox: the version is not read,
# and the probe is not run outside it - and with nothing to show the database
# unchanged, nothing is put back.
setup schema-read-sandbox-not-started
write_request
systemd_here
unit_sandbox
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
: > "${FAKE_DIR}/systemd-run-refuses"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "upgraded-incomplete 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the database's schema version could not be read before the install (systemd-run did not start ${FAKE_BIN}/hmailserver --schema-version in hmailserver.service's sandbox (exit 1: Failed to start transient service unit: (fake) Unit has a bad setting.), and it is not run outside it), so nothing shows that the install left it unchanged; hMailServer 9.9.8 was not put back"
expect_no_call "runuser"
expect_no_call "hmailserver --schema-version"
[ "$(calls | grep -c '^apt-get')" = 1 ] && pass || fail "the earlier version was put back with nothing to show the database unchanged"

# And a machine systemd runs with no systemd-run on it.
setup schema-read-without-systemd-run
write_request
systemd_here
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper systemd-run
expect_rc 1
expect_outcome_contains "the database's schema version could not be read before the install (systemd runs this machine and there is no systemd-run to ask ${FAKE_BIN}/hmailserver --schema-version in hmailserver.service's sandbox, and it is not asked outside it)"
expect_no_call "runuser"
expect_no_call "hmailserver --schema-version"

# `check`, from an administrator's terminal: the probe is handed to systemd
# (here the fake, which starts it in a session of its own as systemd does), and
# never run on that terminal.
setup check-asks-in-the-units-sandbox
systemd_here
unit_sandbox
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
if run_helper_on_a_terminal check; then
   expect_output "database:    at schema version 6108 (this server needs 6108), asked as hmailserver, in hmailserver.service's sandbox"
   expect_call "systemd-run"
   expect_no_call "runuser"
   [ "$(sort -u "${FAKE_DIR}/probe-tty" 2>/dev/null)" = none ] && pass || fail "the probe run by check had the terminal: $(sort -u "${FAKE_DIR}/probe-tty" 2>/dev/null)"
else
   echo "   skipped: there is no script(1) to give it a terminal"
fi

# Without systemd, on a terminal: runuser in a session of its own, which has
# no controlling terminal, so the probe cannot reach the one the helper was
# started from.
setup probe-off-the-terminal-without-systemd
write_request
printf 'hmailserver\n' > "${FAKE_DIR}/unit-user"
if run_helper_on_a_terminal apply; then
   expect_rc 0
   expect_outcome_starts "ok 9.9.9"
   [ "$(calls | grep -c '^runuser -u hmailserver')" = 1 ] && pass || fail "the probe was not run through runuser: $(calls | grep -E 'runuser|systemd-run' | tr '\n' ';')"
   [ "$(sort -u "${FAKE_DIR}/probe-tty" 2>/dev/null)" = none ] && pass || fail "the probe had the terminal the helper was started from: $(sort -u "${FAKE_DIR}/probe-tty" 2>/dev/null)"
   grep -q "asked as hmailserver, without hmailserver.service's sandbox: systemd does not run this machine" "${LOG}" && pass || fail "the log does not say the probe ran without the unit's sandbox"
else
   echo "   skipped: there is no script(1) to give it a terminal"
fi

# ---------------------------------------------------------------- the verdict

echo
if [ "${failures}" -ne 0 ]; then
   echo "hmailserver-update: ${failures} check(s) failed, ${passes} passed."
   exit 1
fi
echo "hmailserver-update: every path holds - ${passes} checks across the cases above."
exit 0
