#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The srcpkg-<release> jobs of .gitlab/ci/source-packages.yml for Fedora, EL and
# openSUSE: the RPM spec, platform/packaging/rpm/hmailserver.spec, built from
# the source tarball as a distribution builds it (hmailserver/docs/SourcePackages.md).
#
#   HM_SRCPKG_TARGET=fedora44 bash build/ci/packages/srcpkg-rpm.sh
#
# run from the root of a checkout, as root, in that release's own container, with
# the source-tarball job's artifact in HM_SOURCE_TARBALL. The steps:
#
#   1. with the network: rpm-build, rpmlint and a C compiler, and later every
#      BuildRequires the spec names (rpmspec -q --buildrequires), from the
#      release's own repositories - for EL10 with CodeReady Builder, which
#      carries boost-devel and ninja-build, and EPEL, which carries rpmlint;
#   2. the spec and Source1 (rpm/hmailserver.sysusers) taken from the tarball
#      itself, never from the checkout, and the tarball as Source0;
#   3. rpmbuild -ba under build/ci/packages/offline.c, which refuses every socket
#      but a local one (docker run --network none and unshare -n are not open to
#      the job: offline.c says why), with %check running the tester programs;
#   4. rpmlint over the spec, the source package and the binary packages: an
#      error fails the job, and the warnings are listed in rpmlint.txt and the log;
#   5. the binary package held to what the build was asked for: a Boost soname
#      among its requirements (linked dynamically), bundled(quickjs) among what it
#      provides, no live-update helper in it, and its licences marked %license -
#      LICENSE, the AGPL-3.0 text and the notices of what is compiled into it.
#
# What it leaves in HM_OUT_DIR (source-packages/<release>): the .src.rpm, the
# binary packages without the debug information ones (built and linted, not kept),
# build.log, rpmlint.txt and build-info.txt. The install job
# (srcpkg-install.sh) takes them into a clean container.
#
# The environment it reads:
#   HM_SRCPKG_TARGET    fedora44, el10, leap16 or tumbleweed (required)
#   HM_SOURCE_TARBALL   the tarball, or a directory holding exactly one
#                       hmailserver-*.tar.gz (source-tarball)
#   HM_OUT_DIR          where the results go (source-packages/<release>)
#   HM_WORK_DIR         where it builds (/tmp/hm-srcpkg), emptied first
#   HM_BUILD_JOBS       the build's parallel jobs (4)
set -uo pipefail

target="${HM_SRCPKG_TARGET:-}"
source_from="${HM_SOURCE_TARBALL:-source-tarball}"
out="${HM_OUT_DIR:-source-packages/$target}"
work="${HM_WORK_DIR:-/tmp/hm-srcpkg}"
jobs="${HM_BUILD_JOBS:-4}"
RPM_DIR=hmailserver/source/Server/platform/packaging/rpm

refuse() {
  echo "REFUSED: $*"
  exit 2
}
fail() {
  echo "FAILED: $*"
  exit 1
}
step() {
  echo ""
  echo "== $*"
}

[ -f build/ci/packages/offline.c ] || refuse "run this from the root of a checkout (no build/ci/packages/offline.c here)."
[ "$(id -u)" = 0 ] || refuse "run this as root in the release's container: it installs the build's dependencies."
. /etc/os-release
# What /etc/os-release must say for each target, so that a job given the wrong
# image fails at once instead of building under a wrong name.
case "$target" in
  fedora44) family=dnf; [ "$ID" = fedora ] && [ "$VERSION_ID" = 44 ] ;;
  el10) family=dnf; [[ " ${ID_LIKE:-} $ID " == *" rhel "* ]] && [ "${VERSION_ID%%.*}" = 10 ] ;;
  leap16) family=zypper; [ "$ID" = opensuse-leap ] && [ "${VERSION_ID%%.*}" = 16 ] ;;
  tumbleweed) family=zypper; [ "$ID" = opensuse-tumbleweed ] ;;
  *) refuse "HM_SRCPKG_TARGET='$target' is not fedora44, el10, leap16 or tumbleweed." ;;
esac
[ $? -eq 0 ] || refuse "HM_SRCPKG_TARGET=$target, but this container is $PRETTY_NAME."
here=$(pwd)

if [ -d "$source_from" ]; then
  # A glob, not find: this runs before the packaging tools are installed, and
  # openSUSE Tumbleweed's image has no findutils (pipeline 2896082335's
  # srcpkg-tumbleweed stopped here with "find: command not found").
  shopt -s nullglob
  found=("$source_from"/hmailserver-*.tar.gz)
  shopt -u nullglob
  [ "${#found[@]}" = 1 ] \
    || refuse "not exactly one hmailserver-*.tar.gz in $source_from (found: ${found[*]:-none}); the source-tarball job's artifact is that directory."
  tarball=${found[0]}
else
  tarball=$source_from
fi
[ -f "$tarball" ] || refuse "no source tarball at $tarball."
tarball=$(cd "$(dirname "$tarball")" && pwd)/$(basename "$tarball")
name=$(basename "$tarball")
version=${name#hmailserver-}
version=${version%.tar.gz}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || refuse "$name is not hmailserver-<major>.<minor>.<patch>.tar.gz."

case "$out" in /*) ;; *) out="$here/$out" ;; esac
case "$work" in
  / | "$here" | "$here"/*) refuse "HM_WORK_DIR=$work would empty the checkout." ;;
esac
rm -rf "$out" "$work"
mkdir -p "$out" "$work/meta"
topdir="$work/rpmbuild"
mkdir -p "$topdir"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}
started=$EPOCHSECONDS
echo "$PRETTY_NAME, $name, $jobs jobs"

# ------------------------------------------------------------------ with the network
# A download that failed is tried again, up to three times; a package the release
# does not have fails at once.
retry_fetch() {
  local log=$1 try code
  shift
  for try in 1 2 3; do
    "$@" > /tmp/hm-srcpkg-pm.log 2>&1
    code=$?
    cat /tmp/hm-srcpkg-pm.log >> "$log"
    [ "$code" -eq 0 ] && return 0
    grep -q -i -E 'curl error|Could not resolve|Timeout was reached|Failed to download|Download .*failed|Connection (refused|reset)|Temporary failure' /tmp/hm-srcpkg-pm.log || break
    echo "try $try of 3: $(grep -i -E 'curl error|Could not resolve|Failed to download|Timeout' /tmp/hm-srcpkg-pm.log | head -n 1)"
    [ "$try" = 3 ] || sleep 15
  done
  tail -n 30 /tmp/hm-srcpkg-pm.log
  return 1
}
repos=()
[ "$target" = el10 ] && repos=(--enablerepo=crb)
install_packages() {
  case "$family" in
    dnf) retry_fetch "$out/packages.log" dnf -y -q "${repos[@]}" --setopt=install_weak_deps=False install "$@" ;;
    zypper) retry_fetch "$out/packages.log" zypper -n -q install --no-recommends "$@" ;;
  esac
}

step "the packaging tools, with the network"
# zypper's repositories first, by zypper-refresh.sh, which points those on
# download.opensuse.org or cdn.opensuse.org at openSUSE's origin when a mirror
# serves a wrong file and says so; the installs below find them fresh.
if [ "$family" = zypper ]; then
  bash build/ci/packages/zypper-refresh.sh "$out/refresh.log" \
    || { tail -n 30 "$out/refresh.log"; fail "zypper could not refresh $PRETTY_NAME's repositories"; }
fi
if [ "$target" = el10 ]; then
  # rpmlint is EPEL's on EL; epel-release is in the distribution's own extras.
  install_packages epel-release || fail "epel-release did not install"
fi
# With the macros the spec's preamble names, so that rpmspec reads it whole before
# its BuildRequires are installed.
extra=(systemd-rpm-macros)
[ "$family" = zypper ] && extra+=(sysuser-tools)
# And python3-packaging for rpmlint on openSUSE: Leap 16.0's rpmlint 2.7 imports it
# and neither requires nor recommends it, and without it dies at its first check
# ("No module named 'packaging'", 29 September 2026); Tumbleweed's has it anyway.
[ "$family" = zypper ] && extra+=(python3-packaging)
install_packages rpm-build rpmlint gcc tar gzip findutils "${extra[@]}" || fail "the packaging tools did not install"
echo "$(rpm -q rpm rpm-build rpmlint | tr '\n' ' ')"

step "the spec and its sysusers file, from the tarball"
tar -xzf "$tarball" -C "$work/meta" "hmailserver-$version/$RPM_DIR/hmailserver.spec" "hmailserver-$version/$RPM_DIR/hmailserver.sysusers" \
    "hmailserver-$version/$RPM_DIR/hmailserver.rpmlintrc" \
  || fail "$name carries no $RPM_DIR/hmailserver.spec, hmailserver.sysusers or hmailserver.rpmlintrc"
spec="$topdir/SPECS/hmailserver.spec"
cp "$work/meta/hmailserver-$version/$RPM_DIR/hmailserver.spec" "$spec"
cp "$work/meta/hmailserver-$version/$RPM_DIR/hmailserver.sysusers" "$topdir/SOURCES/"
cp "$tarball" "$topdir/SOURCES/"
define=(--define "_topdir $topdir")
spec_version=$(rpmspec "${define[@]}" -q --srpm --qf '%{VERSION}' "$spec" 2> "$out/rpmspec.log") \
  || { cat "$out/rpmspec.log"; fail "rpmspec cannot parse the spec"; }
[ "$spec_version" = "$version" ] || fail "the spec says Version: $spec_version, and the tarball is $version"
echo "hmailserver.spec: Version $spec_version, Release $(rpmspec "${define[@]}" -q --srpm --qf '%{RELEASE}' "$spec")"

step "the spec's BuildRequires, with the network"
# The names alone: the versions are held by rpmbuild itself, which refuses to
# build when one is not met.
mapfile -t build_requires < <(rpmspec "${define[@]}" -q --buildrequires "$spec" | sed 's/[[:space:]].*//' | sort -u)
echo "${build_requires[*]}"
install_packages "${build_requires[@]}" || fail "a BuildRequires that $PRETTY_NAME cannot meet"
{
  echo "distribution $PRETTY_NAME"
  echo "tarball $name $(sha256sum "$tarball" | cut -d' ' -f1)"
  echo "compiler $(g++ --version | head -n 1)"
  echo "cmake $(cmake --version | head -n 1 | sed 's/^cmake version //')"
  echo "rpm $(rpm -q --qf '%{VERSION}' rpm)"
  echo "boost $(rpm -q --whatprovides boost-devel libboost_headers-devel 2> /dev/null | grep -v 'no package' | head -n 1)"
  echo "openssl $(rpm -q --whatprovides 'pkgconfig(openssl)' | head -n 1)"
} | tee "$out/build-info.txt"

# ------------------------------------------------------------------ without it
step "the network refused from here on (build/ci/packages/offline.c)"
cc -O2 -Wall -Wextra -o "$work/offline" build/ci/packages/offline.c || fail "offline.c did not compile"
if "$work/offline" getent ahosts gitlab.com > /dev/null 2>&1; then
  fail "a name was looked up under offline: the network is not refused"
fi
echo "under offline: getent ahosts gitlab.com fails, as every socket but a local one does"

step "rpmbuild -ba"
build_started=$EPOCHSECONDS
"$work/offline" rpmbuild -ba "${define[@]}" --define "_smp_build_ncpus $jobs" --define "_smp_mflags -j$jobs" \
  "$spec" > "$out/build.log" 2>&1
code=$?
echo "exit $code after $(( EPOCHSECONDS - build_started )) s"
if [ "$code" -ne 0 ]; then
  # grep -m, not `| head`, which left grep to die of SIGPIPE under pipefail.
  grep -m 40 -E 'error:|^FAILED|RPM build errors|^    ' "$out/build.log"
  tail -n 40 "$out/build.log"
  fail "rpmbuild; the whole log is build.log"
fi
grep -E '^== |PASSED|passed|failed' "$out/build.log" | tail -n 12

step "rpmlint"
srpm=$(ls "$topdir"/SRPMS/hmailserver-*.src.rpm)
mapfile -t rpms < <(ls "$topdir"/RPMS/*/*.rpm)
# With the package's own filters (rpm/hmailserver.rpmlintrc), each with its reason:
# the modes the mail store and the configuration keep on purpose, and the two data
# files the server reads from beside its binary. Given as a configuration (-c) and
# not as an rpmlintrc (-r): rpmlint makes an error of an rpmlintrc filter that
# matched nothing, and one spec serves Fedora, EL and openSUSE, whose rpmlint
# configurations do not report alike. The filters' own lines are the file's.
filters="$work/rpmlint-filters.toml"
{
  echo 'Filters = ['
  sed -n 's/^addFilter("\([^"]*\)")[[:space:]]*$/    "\1",/p' "$work/meta/hmailserver-$version/$RPM_DIR/hmailserver.rpmlintrc"
  # And two errors about the debug packages, which this job builds and lints but
  # does not keep, that Tumbleweed's rpmlint 2.10 reported in a run of this job on
  # 30 September 2026: the separate debug-information file has no program
  # interpreter, so it reads as statically linked (the server itself is linked
  # dynamically, and rpmlint says nothing of it), and the debug sources package
  # holds no program, which is what it is for. Neither is about a package a
  # distribution ships, and neither belongs in the spec's own rpmlintrc.
  echo '    "hmailserver-debuginfo[.][^:]*: E: statically-linked-binary /usr/lib/debug/",'
  echo '    "hmailserver-debugsource[.][^:]*: E: no-binary",'
  echo ']'
} > "$filters"
[ "$(grep -c '^    "' "$filters")" -gt 0 ] || fail "no filter read from hmailserver.rpmlintrc"
rpmlint -c "$filters" "$spec" "$srpm" "${rpms[@]}" > "$out/rpmlint.txt" 2>&1
rpmlint_code=$?
summary=$(grep -E '[0-9]+ errors?, [0-9]+ warnings?' "$out/rpmlint.txt" | tail -n 1)
errors=$(sed -n 's/.* \([0-9][0-9]*\) errors\{0,1\},.*/\1/p' <<< "$summary")
warnings=$(sed -n 's/.* \([0-9][0-9]*\) warnings\{0,1\}.*/\1/p' <<< "$summary")
echo "rpmlint $(rpm -q --qf '%{VERSION}' rpmlint): ${summary:-no summary line}; exit $rpmlint_code"
grep -E ': (E|W): ' "$out/rpmlint.txt" || true
[ -n "$errors" ] || fail "rpmlint printed no summary (rpmlint.txt)"
[ "$errors" -eq 0 ] || fail "rpmlint reports $errors error(s) (rpmlint.txt)"
# rpmlint 2 exits 64 for errors and 66 when the badness passes its threshold.
[ "$rpmlint_code" -eq 0 ] || fail "rpmlint exited $rpmlint_code (rpmlint.txt)"

step "the packages"
# The debug information packages are built and linted above and not kept: the
# install job does not read them, and Fedora's debuginfo alone is 245 MB, which each
# pipeline would upload from linix and keep for two weeks.
kept=()
for rpm in "${rpms[@]}"; do
  case "$(basename "$rpm")" in
    *-debuginfo-* | *-debugsource-*) echo "built, linted, not kept: $(basename "$rpm") ($(du -h "$rpm" | cut -f1))" ;;
    *) kept+=("$rpm") ;;
  esac
done
cp "$srpm" "${kept[@]}" "$out/"
server_rpm=$(ls "$out"/hmailserver-[0-9]*.x86_64.rpm)
server_requires=$(rpm -qp --requires "$server_rpm")
grep -q '^libboost_' <<< "$server_requires" \
  || fail "the package requires no Boost soname: the server was not linked against the distribution's shared Boost"
# glibc's character-set converters, which glibc only recommends: the spec names its
# release's package, for the server's architecture (docs/PackagesRpm.md, "The
# character sets"); install-time, srcpkg-install.sh's server-starts holds the server
# to converting every character set it names.
case "$family" in
  dnf) converters='glibc-gconv-extra(x86-64)' ;;
  zypper) converters='glibc-gconv-modules-extra(x86-64)' ;;
esac
grep -qxF "$converters" <<< "$server_requires" \
  || fail "the package does not require $converters, glibc's character-set converters"
# Each list read whole before it is searched, as $server_requires is: rpm piped into
# grep -q, which stops at its match, can die of SIGPIPE while still writing, and
# pipefail then fails the check with the line present.
server_provides=$(rpm -qp --provides "$server_rpm")
grep -q '^bundled(quickjs) = ' <<< "$server_provides" || fail "the package does not provide bundled(quickjs)"
listing=$(rpm -qlp "$server_rpm")
if grep -q 'hmailserver-update' <<< "$listing"; then
  fail "the package carries the live update's helper, which a build with HM_LIVE_UPDATE OFF does not install"
fi
grep -q '^/usr/lib/sysusers.d/hmailserver.conf$' <<< "$listing" || fail "no sysusers.d file in the package"
cli_listing=$(rpm -qlp "$out"/hmailserver-cli-*.noarch.rpm)
grep -q '^/usr/bin/hmctl$' <<< "$cli_listing" || fail "hmailserver-cli carries no hmctl"
# The licences it goes out under, each marked %license, which is what rpm -qL lists:
# the project's LICENSE, and the five texts the install rules put in
# /usr/share/hmailserver/licenses - hMailServer's AGPL-3.0 text and the notices of
# QuickJS, of Unicode's data for UTS 46, of HTML's list of named character
# references and of the Feather icons the webmail's page is drawn after, which are
# compiled into the server and whose licences ask for them with every copy.
# Their marking is checked here, not their contents, which check-licenses.sh
# compares for the .deb and the .rpm the project builds itself.
licence_listing=$(rpm -qpL "$server_rpm")
for licence in /usr/share/licenses/hmailserver/LICENSE /usr/share/hmailserver/licenses/AGPL-3.0.txt \
    /usr/share/hmailserver/licenses/QuickJS.txt /usr/share/hmailserver/licenses/Unicode-data.txt \
    /usr/share/hmailserver/licenses/HTML-named-character-references.txt \
    /usr/share/hmailserver/licenses/Feather-icons.txt; do
  grep -qxF "$licence" <<< "$licence_listing" || fail "the package does not carry $licence marked %license"
done
echo "licences: $(tr '\n' ' ' <<< "$licence_listing")"
echo "requires: $(grep -E '^lib(boost|ssl|pq|ldap|crypto)' <<< "$server_requires" | tr '\n' ' ')"
( cd "$out" && sha256sum -- *.rpm > SHA256SUMS.txt )
{
  echo "packages $(cd "$out" && ls -- *.rpm | tr '\n' ' ')"
  echo "rpmlint errors ${errors:-?} warnings ${warnings:-?}"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
  echo "seconds $(( EPOCHSECONDS - started ))"
} | tee -a "$out/build-info.txt"

rm -rf "$work"
echo ""
echo "BUILT: $(basename "$server_rpm") for $PRETTY_NAME from $name, with the network refused, rpmlint clean of errors (${warnings:-?} warning(s)), in $(( EPOCHSECONDS - started )) s."
