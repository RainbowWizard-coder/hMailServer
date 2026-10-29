#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The operator image's build context: platform/packaging/helm/operator's Dockerfile and
# watches.yaml, and the project's Helm chart, platform/packaging/helm/hmailserver,
# copied in as helm-charts/hmailserver, where the Helm operator reads it. The chart is
# copied, not kept a second time in the tree, so that it cannot drift from the chart
# every other way of installing installs; and the copy is held to the tree's after it
# is made, file by file, so that the image carries the chart and nothing else. Plain
# POSIX sh with busybox's tools, so that it runs in kaniko's debug image
# (build/ci/operator-image.sh) as well as on a workstation:
#
#   sh build/ci/operator-context.sh OUT [VERSION [CHART_PACKAGE]]
#
# OUT       a directory this makes, which must not exist yet
# VERSION   the release's: the copy's Chart.yaml then says version: VERSION and
#           appVersion: "VERSION", as the release's packaged chart does (helm package
#           --version --app-version, build/ci/helm-chart-package.sh), so that the
#           operator installs its own release's image by default; without it the copy
#           keeps the tree's 0.1.0 and "latest", the image tag a checkout's chart
#           installs. Those two lines are all that may differ from the tree's chart.
# CHART_PACKAGE
#           the release's packaged chart (hmailserver-chart-VERSION.tgz, which
#           helm-chart-package makes and operator-image.sh takes from the published
#           release): every file of it but Chart.yaml, which helm package
#           writes anew, must be the copy's byte for byte, and the copy must have no
#           file the package has not, so that the operator's chart is the release's.
set -eu

chart=hmailserver/source/Server/platform/packaging/helm/hmailserver
operator=hmailserver/source/Server/platform/packaging/helm/operator

fail() {
  echo "OPERATOR-CONTEXT FAILED: $*"
  exit 1
}

[ $# -ge 1 ] && [ $# -le 3 ] || fail "usage: sh build/ci/operator-context.sh OUT [VERSION [CHART_PACKAGE]]"
out="$1"
version="${2:-}"
package="${3:-}"
[ -d "$chart" ] && [ -f "$chart/Chart.yaml" ] || fail "$chart is not here: run this from the root of the checkout"
for file in Dockerfile watches.yaml; do
  [ -f "$operator/$file" ] || fail "$operator/$file is not here"
done
[ ! -e "$out" ] || fail "$out exists already; this makes it afresh"
if [ -n "$version" ]; then
  printf '%s\n' "$version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?' \
    || fail "$version is not a SemVer 2 version, which Helm requires of a chart"
fi
[ -z "$package" ] || [ -n "$version" ] || fail "a chart package is held to a release's version: give VERSION too"

# The Dockerfile, held to an allow-list and read as Docker and kaniko read it: an
# instruction's first word in any case and after any indentation, comments and blank
# lines dropped, continued lines joined. One instruction a line, its word in upper case
# and its arguments' white space made single spaces, as build/ci/operator-check.py's
# dockerfile_instructions reads it; a parser directive (which could change the escape
# character or fetch a frontend), or a comment or blank line inside a continued
# instruction, is a line of its own that no rule below allows.
instructions=$(awk '
  { sub(/\r$/, "") }
  /^[ \t]*#/ {
    if (tolower($0) ~ /^[ \t]*#[ \t]*(syntax|escape|check)[ \t]*=/) print "!DIRECTIVE line " NR
    if (pending != "") print "!COMMENT-IN-CONTINUATION line " NR
    next
  }
  /^[ \t]*$/ { if (pending != "") print "!BLANK-IN-CONTINUATION line " NR; next }
  {
    text = $0
    continued = sub(/\\[ \t]*$/, "", text)
    pending = pending " " text
    if (continued) next
    gsub(/[ \t]+/, " ", pending)
    sub(/^ /, "", pending)
    sub(/ $/, "", pending)
    word = pending
    sub(/ .*$/, "", word)
    rest = substr(pending, length(word) + 2)
    print toupper(word) (rest == "" ? "" : " " rest)
    pending = ""
  }
  END { if (pending != "") print "!UNFINISHED" }
' "$operator/Dockerfile") || fail "$operator/Dockerfile could not be read"
printf '%s\n' "$instructions" | head -n 1 \
  | grep -Eqx 'FROM quay\.io/operator-framework/helm-operator:v[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}' \
  || fail "$operator/Dockerfile's first instruction is not FROM quay.io/operator-framework/helm-operator by its tag and digest, with no flag or stage name"
[ "$(printf '%s\n' "$instructions" | grep -c '^FROM ')" = 1 ] || fail "$operator/Dockerfile has more than one FROM"
odd=$(printf '%s\n' "$instructions" | grep -Ev '^(FROM|ARG|LABEL|ENV|WORKDIR|COPY) ' | head -n 1)
[ -z "$odd" ] || fail "$operator/Dockerfile has '$odd': it may have FROM, ARG, LABEL, ENV, WORKDIR and its two COPY lines alone, so that it runs and fetches nothing"
# The two files the Helm operator reads, as the Dockerfile copies them, with no flag:
# the Dockerfile's own ${HOME}, as it is written there, not this shell's.
# shellcheck disable=SC2016
[ "$(printf '%s\n' "$instructions" | grep '^COPY ')" = 'COPY watches.yaml ${HOME}/watches.yaml
COPY helm-charts ${HOME}/helm-charts' ] || fail "$operator/Dockerfile's COPY lines are not exactly its two, watches.yaml and helm-charts"
grep -qx '  chart: helm-charts/hmailserver' "$operator/watches.yaml" || fail "$operator/watches.yaml does not name helm-charts/hmailserver"

mkdir -p "$out/helm-charts"
cp "$operator/Dockerfile" "$operator/watches.yaml" "$out/"
cp -R "$chart" "$out/helm-charts/hmailserver"
copy="$out/helm-charts/hmailserver"

if [ -n "$version" ]; then
  [ "$(grep -c '^version: 0\.1\.0$' "$copy/Chart.yaml")" = 1 ] || fail "the chart's Chart.yaml has no one line version: 0.1.0 to stamp"
  [ "$(grep -c '^appVersion: "latest"$' "$copy/Chart.yaml")" = 1 ] || fail "the chart's Chart.yaml has no one line appVersion: \"latest\" to stamp"
  sed -i -e "s|^version: 0\.1\.0\$|version: $version|" -e "s|^appVersion: \"latest\"\$|appVersion: \"$version\"|" "$copy/Chart.yaml"
fi

# The copy, file by file, against the tree's chart: the same files, the same bytes,
# but for the two stamped lines of Chart.yaml.
# The check's own files beside OUT, not under mktemp's /tmp, which kaniko's image has not.
scratch="$out.check"
rm -rf "$scratch"
mkdir -p "$scratch"
trap 'rm -rf "$scratch"' EXIT
listing="$scratch/listing"
( cd "$chart" && find . -type f | LC_ALL=C sort ) > "$listing"
( cd "$copy" && find . -type f | LC_ALL=C sort ) > "$listing.copy"
cmp -s "$listing" "$listing.copy" || { diff "$listing" "$listing.copy" || true; fail "the copy's files are not the chart's"; }
[ -s "$listing" ] || fail "the chart has no files"
while IFS= read -r file; do
  if [ "$file" = ./Chart.yaml ] && [ -n "$version" ]; then
    sed -e "s|^version: $version\$|version: 0.1.0|" -e "s|^appVersion: \"$version\"\$|appVersion: \"latest\"|" "$copy/$file" > "$listing.chart"
    cmp -s "$chart/$file" "$listing.chart" || fail "the copy's Chart.yaml differs from the tree's in more than its version and appVersion"
    if ! grep -qx "version: $version" "$copy/$file" || ! grep -qx "appVersion: \"$version\"" "$copy/$file"; then
      fail "the copy's Chart.yaml was not stamped with $version"
    fi
  else
    cmp -s "$chart/$file" "$copy/$file" || fail "the copy's ${file#./} is not the tree's"
  fi
done < "$listing"

# The context's modes, set rather than inherited. cp gives each copy the checkout's
# mode less the umask, and the Dockerfile's COPY keeps the context's modes, so the
# image would carry the checkout's and the umask's: a job's 0022 over GitLab Runner's
# checkout, written under umask 0000, gives 0644 and 0755, but a build by hand under
# Ubuntu's 0002 makes the image's watches.yaml and chart writable by group root, which
# the base image's user 1001 is in, as an OpenShift container's arbitrary user is, and
# other bytes. So each is given git's mode for it, the modes a job gives: 0644 for
# every file, since git records each file of the chart and the operator as 100644 -
# the Helm operator only reads them, and build/ci/operator-check.py refuses one that
# git records executable - and not the checkout's execute bit, which a working tree
# chmodded by hand or copied from a filesystem without modes may have where git has
# none; and 0755 for a directory, written 00755 so that GNU's chmod, on a workstation
# whose OUT is in a set-group-ID directory, clears that bit as busybox's does.
find "$out" -type d -exec chmod 00755 {} +
find "$out" -type f -exec chmod 0644 {} +
odd=$(find "$out" ! \( -type d -perm 0755 \) ! \( -type f -perm 0644 \) | head -n 1)
[ -z "$odd" ] || fail "$odd in the context is not a directory of mode 0755 or a file of mode 0644"
count=$(wc -l < "$listing" | tr -d ' ')
echo "the chart copied in: $count files, each the tree's$( [ -z "$version" ] || printf ', Chart.yaml stamped %s' "$version")"

if [ -n "$package" ]; then
  [ -f "$package" ] || fail "$package is not here"
  unpacked="$scratch/package"
  mkdir -p "$unpacked"
  tar -xzf "$package" -C "$unpacked" || fail "$package could not be unpacked"
  [ -d "$unpacked/hmailserver" ] || fail "$package holds no hmailserver/"
  ( cd "$unpacked/hmailserver" && find . -type f | LC_ALL=C sort ) > "$listing.copy"
  # The same files: helm package leaves out only what .helmignore names, which no file
  # of the chart is.
  cmp -s "$listing" "$listing.copy" || { diff "$listing" "$listing.copy" || true; fail "$package's files are not the chart's"; }
  while IFS= read -r file; do
    [ "$file" = ./Chart.yaml ] && continue
    cmp -s "$copy/$file" "$unpacked/hmailserver/$file" || fail "$package's ${file#./} is not the copy's"
  done < "$listing"
  grep -Eqx "version: \"?$version\"?" "$unpacked/hmailserver/Chart.yaml" || fail "$package's chart is not version $version"
  grep -Eqx "appVersion: \"?$version\"?" "$unpacked/hmailserver/Chart.yaml" || fail "$package's chart's appVersion is not $version"
  echo "the copy is $package's chart: every file but Chart.yaml byte for byte, and its version and appVersion $version"
fi
echo "CONTEXT: $out (Dockerfile, watches.yaml, helm-charts/hmailserver)"
