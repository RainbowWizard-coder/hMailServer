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
# a unit that is not the server's, an unknown key, an outcome path elsewhere) -
# each refused in the journal alone, with nothing written into the requester's
# directory; the symlinks a requester might plant where root is about to write,
# followed by nothing; the root re-verification refusing a tampered file; the
# version rule (a signed package that is not newer than the installed one is
# not installed, the request's version line is not believed over the package's,
# a rollback image that is not the installed version is dropped); an image or
# a directory anyone else can write to; the log file that is only written after
# the stop; and --dry-run touching nothing.
#
# What it cannot prove, and says so: that a real dpkg -i under a real unit puts
# the new binary in place - that needs root and a signed package, and is the
# packaging README's "what was run for real" paragraph.
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
         sed -n '5,36p' "${BASH_SOURCE[0]}"
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
for tool in sh cat head tail sort sed tr realpath stat date sleep mktemp cp chown chmod ln mv rm dirname basename grep id printf echo test; do
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
      echo inactive > "$FAKE_DIR/state"
      ;;
   start)
      # One line of the plan per start: active or failed; active when the
      # plan is exhausted.
      plan=""
      if [ -f "$FAKE_DIR/start-plan" ]; then
         plan="$(head -n 1 "$FAKE_DIR/start-plan")"
         sed -i '1d' "$FAKE_DIR/start-plan"
      fi
      [ -n "$plan" ] || plan=active
      echo "$plan" > "$FAKE_DIR/state"
      ;;
   show)
      case "$*" in
         *ActiveState*) cat "$FAKE_DIR/state" 2>/dev/null || echo inactive ;;
         *NRestarts*) echo 0 ;;
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
# what the helper takes the installed version from.
cat > "${FAKE_BIN}/hmailserver" <<'FAKE'
#!/bin/sh
if [ "$1" = "--version" ]; then
   v="$(cat "$FAKE_DIR/installed-version" 2>/dev/null)"
   echo "hMailServer ${v:-9.9.8} build 0"
   exit 0
fi
echo "hmailserver $*" >> "$FAKE_DIR/calls"
if [ "$1" = "--verify-update" ]; then
   [ -f "$2.cosign.bundle" ] || { echo "$2 has no Sigstore bundle beside it" >&2; exit 1; }
   if grep -q TAMPERED "$2"; then echo "$2 did not verify" >&2; exit 1; fi
   echo "$2 verifies"
   exit 0
fi
echo "hMailServer 0.0.0 build 0"
FAKE

chmod +x "${FAKE_BIN}"/*

# ---------------------------------------------------------------- one case

# A fresh Updates directory with a package and a rollback image, each with a
# bundle, and a request built from the arguments. Runs the helper with the
# fakes first on PATH and nothing but the real tools after them.
CASE=""
UPDATES=""
FAKE_DIR=""
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
   mkdir -p "${FAKE_DIR}"
   printf 'new package bytes\n' > "${UPDATES}/hmailserver_9.9.9_amd64.deb"
   printf 'bundle\n' > "${UPDATES}/hmailserver_9.9.9_amd64.deb.cosign.bundle"
   printf 'old package bytes\n' > "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb"
   printf 'bundle\n' > "${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb.cosign.bundle"
   REQUEST="${dir}/apply-request"
   OUTCOME="${UPDATES}/last-apply.txt"
   LOG="${UPDATES}/apply-9.9.9.log"
   echo "== ${CASE}"
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
   OUT="$(env -i PATH="${path}" FAKE_DIR="${FAKE_DIR}" \
      HMAILSERVER_UPDATE_SETTLE_SECONDS=1 HMAILSERVER_UPDATE_POLL_SECONDS=1 HMAILSERVER_UPDATE_PAUSE_SECONDS=0 \
      HMAILSERVER_UPDATE_PROGRAM="${FAKE_BIN}/hmailserver" \
      sh "${HELPER}" apply --request "${REQUEST}" --updates-dir "${UPDATES}" "${@:2}" 2>&1)"
   RC=$?
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
   if printf '%s' "${OUT}" | grep -qF -- "$1"; then pass; else fail "output lacks '$1'; output was:"; printf '%s\n' "${OUT}" | sed 's/^/        /'; fi
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
expect_call "hmailserver --verify-update ${UPDATES}/hmailserver_9.9.9_amd64.deb"
expect_call "hmailserver --verify-update ${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb"
expect_call "apt-get install -y --no-install-recommends -o DPkg::Lock::Timeout=300 ${UPDATES}/hmailserver_9.9.9_amd64.deb"
expect_call "systemctl start hmailserver.service"
expect_no_call "dpkg -i"
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
[ -z "$(ls "${UPDATES}" | grep -E '^(last-apply|apply)\.[A-Za-z0-9]{6}$')" ] && pass || fail "a temporary file was left in the Updates directory: $(ls "${UPDATES}" | tr '\n' ' ')"
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
expect_outcome_starts "rolled-back 9.9.9 the package was installed but the service was not back after 2 seconds (failed); hMailServer 9.9.8 was reinstalled and the service is running"
expect_call "apt-get install -y --no-install-recommends --allow-downgrades -o DPkg::Lock::Timeout=300 ${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb"
[ "$(calls | grep -c 'systemctl stop')" = 2 ] && pass || fail "expected two stops (one before the rollback)"
[ "$(calls | grep -c 'systemctl start')" = 2 ] && pass || fail "expected two starts"

setup no-rollback-deb
write_request
sed -i '/^rollback/d' "${REQUEST}"
printf 'failed\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "no-rollback 9.9.9 the package was installed but the service was not back after 2 seconds (failed); no rollback image was available"
[ "$(calls | grep -c 'apt-get')" = 1 ] && pass || fail "expected exactly one install"

setup rollback-failed-deb
write_request
printf 'failed\nfailed\n' > "${FAKE_DIR}/start-plan"
printf '0\n7\n' > "${FAKE_DIR}/install-plan"
run_helper
expect_rc 1
expect_outcome_starts "rollback-failed 9.9.9 the package was installed but the service was not back after 2 seconds (failed); the rollback to 9.9.8 exited with 7 and the service is not back (failed)"

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
expect_outcome_starts "rolled-back 9.9.9 the install step exited with 2 and the service was not back (failed); hMailServer 9.9.8 was reinstalled"

setup dpkg-fallback
write_request
run_helper apt-get
expect_rc 0
expect_outcome_starts "ok 9.9.9"
expect_call "dpkg -i ${UPDATES}/hmailserver_9.9.9_amd64.deb"
expect_no_call "apt-get"

setup rpm-with-dnf
write_request
sed -i 's/^kind=.*/kind=rpm/' "${REQUEST}"
printf 'failed\nactive\n' > "${FAKE_DIR}/start-plan"
run_helper
expect_rc 1
expect_outcome_starts "rolled-back 9.9.9"
expect_call "dnf install -y ${UPDATES}/hmailserver_9.9.9_amd64.deb"
expect_call "rpm -Uvh --oldpackage ${UPDATES}/rollback/hmailserver_9.9.8_amd64.deb"

setup rpm-without-dnf
write_request
sed -i 's/^kind=.*/kind=rpm/' "${REQUEST}"
run_helper dnf
expect_rc 0
expect_call "rpm -Uvh ${UPDATES}/hmailserver_9.9.9_amd64.deb"
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
[ -z "$(ls "${UPDATES}" | grep -vE '^(hmailserver_9\.9\.9_amd64\.deb(\.cosign\.bundle)?|rollback)$')" ] && pass || fail "a refusal left something in the Updates directory: $(ls "${UPDATES}" | tr '\n' ' ')"
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

setup no-installed-version
write_request
printf '' > "${FAKE_DIR}/installed-version"
run_helper
expect_rc 0
expect_outcome_starts "ok 9.9.9"
expect_output "no installed version is recorded for hmailserver; installing 9.9.9 over whatever is here"

setup refused-unit
write_request
sed -i 's/^unit=.*/unit=postgresql.service/' "${REQUEST}"
run_helper
expect_rc 4
expect_output "unit must be the server's own unit"
expect_no_call "systemctl"

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
   printf '#!/bin/sh\n# NEW IMAGE\necho "hMailServer %s build 0"\n' "$1" > "$2"
   chmod 0755 "$2"
   printf 'bundle\n' > "$2.cosign.bundle"
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
run_helper
expect_rc 0
expect_outcome_starts "ok 9.9.9"
grep -q "NEW IMAGE" "${IMAGE}" && pass || fail "the image was not replaced"
[ -f "${IMAGE}.previous" ] && grep -q "verify-update" "${IMAGE}.previous" && pass || fail "the previous image was not kept"
[ "$(stat -c %a "${IMAGE}")" = 755 ] && pass || fail "the new image did not take the old one's mode"
expect_no_call "apt-get"
expect_no_call "dpkg"

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
run_helper
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
run_helper
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
run_helper
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
run_helper
expect_rc 4
expect_output "the image's directory is writable by its group or by others (mode 730)"
expect_no_call "systemctl"

setup dry-run
write_request
run_helper "" --dry-run
expect_rc 0
expect_output "would run: systemctl stop hmailserver.service"
expect_output "would verify the package"
expect_output "would run: apt-get install"
expect_output "outcome: ok 9.9.9"
[ -f "${OUTCOME}" ] && fail "a dry run wrote the outcome file" || pass
[ -f "${FAKE_DIR}/calls" ] && fail "a dry run ran something: $(calls | tr '\n' ';')" || pass

# ---------------------------------------------------------------- the verdict

echo
if [ "${failures}" -ne 0 ]; then
   echo "hmailserver-update: ${failures} check(s) failed, ${passes} passed."
   exit 1
fi
echo "hmailserver-update: every path holds - ${passes} checks across the cases above."
exit 0
