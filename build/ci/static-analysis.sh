#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The static-analysis job of .gitlab-ci.yml: cppcheck and clang-tidy over the
# server's C++, the part of the old CodeQL job (the GitHub workflow codeql.yml) that
# runs without GitHub. Run in the image of build/ci/linux.Dockerfile, from the root
# of a checkout; by hand it is the same:
#
#   docker run --rm -v "$PWD":/src -w /src hmailserver-ci/linux:<tag> bash build/ci/static-analysis.sh
#
# What it analyses: every translation unit the Linux build compiles
# (hmailserver/source/Server, less the vendored QuickJS), as CMake's compile
# database describes it - the same include paths, definitions and forced
# precompiled header the compiler is given, so the analysers see the code the
# compiler sees. It configures and does not build: no object file is made.
# What that leaves out: the Windows-only sources (the COM API, the service host,
# ADO and SQL Server Compact, the registry) and the Windows arm of every #ifdef.
# CodeQL analysed those through the MSVC build; nothing here does
# (docs/ContinuousIntegration.md, "Security scanning", says what would).
#
# The tools come from Ubuntu's archive, installed into the job's container on the
# way in: cppcheck, and the clang-tidy of the clang the image already has. The
# checks are chosen in build/ci/clang-tidy.yaml and, for cppcheck, below; each
# exclusion there has its reason and its count.
#
# What it leaves, for the job's artifacts (HM_OUT_DIR, security/static-analysis):
#   cppcheck.xml, clang-tidy.txt       the tools' own output, every finding
#   gl-sast-report.json                the job's GitLab SAST report (schema 15), both
#                                      tools' findings: GitLab takes one file a type
#   gl-sast-cppcheck.json,
#   gl-sast-clang-tidy.json            the same, one tool's findings each
#   junit.xml                          a test case per tool and per NEW finding
#   findings.txt, new-findings.txt     readable lists, most severe first
#   versions.json, configure.log
#
# The exit status is build/ci/security-findings.py's: 0 when nothing is new beyond
# build/ci/static-analysis-baseline.txt, 1 when something is, 2 when a tool failed
# or could not analyse a file. The job is allow_failure for now; the day it is not,
# a new finding fails the pipeline and an old one does not.
#
# To regenerate the baseline (after triage: see the document):
#   bash build/ci/static-analysis.sh --write-baseline
# or, from a job's artifacts without running anything again, the command
# security-findings.py's own help gives.
#
# The environment it reads:
#   HM_OUT_DIR        where the results go (security/static-analysis)
#   HM_BUILD_DIR      where CMake configures (/tmp/hm-analysis), outside the checkout
#   HM_ANALYSIS_JOBS  files analysed at once (6: the runner gives a job six CPUs)
#   HM_TIDY_CHECKS    a clang-tidy check list to run INSTEAD of clang-tidy.yaml's,
#                     for trying a check by hand; never set in the job
set -uo pipefail

out="${HM_OUT_DIR:-security/static-analysis}"
build_dir="${HM_BUILD_DIR:-/tmp/hm-analysis}"
jobs="${HM_ANALYSIS_JOBS:-6}"
write_baseline=()
[ "${1:-}" = "--write-baseline" ] && write_baseline=(--write-baseline)

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi
root="$PWD"
started=$(date -u +%Y-%m-%dT%H:%M:%S)
rm -rf "$out"
mkdir -p "$out"

echo "== the tools, from Ubuntu's archive"
if ! command -v cppcheck > /dev/null || ! command -v clang-tidy > /dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  if ! { apt-get update -qq && apt-get install -y -qq -o Acquire::Retries=3 --no-install-recommends cppcheck clang-tidy > /dev/null; }; then
    echo "apt-get could not install cppcheck and clang-tidy"
    exit 2
  fi
fi
cppcheck_version=$(cppcheck --version | sed 's/^Cppcheck //')
tidy_version=$(clang-tidy --version | sed -n 's/.*LLVM version \([0-9.]*\).*/\1/p' | head -n 1)
clang_version=$(clang --version | sed -n 's/.*clang version \([0-9.]*\).*/\1/p' | head -n 1)
echo "cppcheck $cppcheck_version, clang-tidy $tidy_version, clang $clang_version"
if [ "${tidy_version%%.*}" != "${clang_version%%.*}" ]; then
  # clang-tidy parses with its own clang; one major version apart, the builtin
  # headers and flags the compile database names can differ.
  echo "clang-tidy $tidy_version is not the image's clang $clang_version"
  exit 2
fi
printf '{"cppcheck": "%s", "clang-tidy": "%s"}\n' "$cppcheck_version" "$tidy_version" > "$out/versions.json"

echo ""
echo "== the compile database (configure only, with clang, as linux-build.sh does)"
rm -rf "$build_dir"
if ! CC=clang CXX=clang++ cmake -S hmailserver/source/Server -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$out/configure.log" 2>&1; then
  cat "$out/configure.log"
  echo "CONFIGURE FAILED"
  exit 2
fi
database="$build_dir/compile_commands.json"
# The files to analyse: every C++ translation unit but the vendored engine's C.
python3 - "$database" "$out/files.txt" <<'EOF'
import json, sys
entries = json.load(open(sys.argv[1]))
files = sorted({e["file"] for e in entries if "/quickjs/" not in e["file"] and e["file"].endswith(".cpp")})
open(sys.argv[2], "w").write("\n".join(files) + "\n")
print("%d entries in the compile database, %d C++ translation units to analyse" % (len(entries), len(files)))
EOF
count=$(wc -l < "$out/files.txt")
if [ "$count" -lt 100 ]; then
  echo "Only $count files to analyse: the compile database is not the server's."
  exit 2
fi

echo ""
echo "== cppcheck"
# Error, warning and portability findings; style and performance are left out as
# clang-tidy.yaml leaves them out, for the same reason. __linux__ and __unix__ are
# what the compiler defines by itself and cppcheck does not, and hm_platform.h
# refuses to be read without one. The libraries' configurations tell cppcheck what
# the POSIX, Boost, OpenSSL and zlib calls do (allocation, ownership, null
# arguments) without reading their headers, which it is not given: the system
# include directories are not searched, which keeps a run to minutes, and
# missingIncludeSystem, which says so, is suppressed in the list.
#
# Not reading them, cppcheck takes an undefined macro for 0 in an #if - and
# "#if OPENSSL_VERSION_NUMBER < 0x30200000L" then chose the branch for an OpenSSL
# too old to have Argon2id, which the build never compiles, and cppcheck gave up on
# the file there (unknownMacro, measured on 25 September 2026). So the version of
# the OpenSSL the build does compile against is read from its header and given.
ossl=/usr/include/openssl/opensslv.h
ossl_part() { sed -n "s/^# *define OPENSSL_VERSION_$1 *\([0-9][0-9]*\).*/\1/p" "$ossl" | head -n 1; }
ossl_major=$(ossl_part MAJOR); ossl_minor=$(ossl_part MINOR); ossl_patch=$(ossl_part PATCH)
if [ -z "$ossl_major" ] || [ -z "$ossl_minor" ] || [ -z "$ossl_patch" ]; then
  echo "No OpenSSL version in $ossl"
  exit 2
fi
ossl_number=$(printf '0x%x%02x00%02x0L' "$ossl_major" "$ossl_minor" "$ossl_patch")
echo "OpenSSL $ossl_major.$ossl_minor.$ossl_patch: OPENSSL_VERSION_NUMBER=$ossl_number"
SECONDS=0
cppcheck --project="$database" \
  -i "$root/hmailserver/source/Server/quickjs" \
  --enable=warning,portability --inline-suppr \
  --suppressions-list=build/ci/cppcheck-suppressions.txt \
  -D__linux__ -D__unix__ -U_WIN32 -U_MSC_VER \
  -DOPENSSL_VERSION_NUMBER="$ossl_number" \
  -DOPENSSL_VERSION_TEXT="\"OpenSSL $ossl_major.$ossl_minor.$ossl_patch\"" \
  --platform=unix64 --library=posix --library=gnu --library=boost --library=openssl --library=zlib \
  -j "$jobs" --xml --output-file="$out/cppcheck.xml" > "$out/cppcheck.log" 2>&1
code=$?
echo "cppcheck exit $code after $SECONDS s"
if [ "$code" -ne 0 ] || [ ! -s "$out/cppcheck.xml" ]; then
  tail -n 30 "$out/cppcheck.log"
  echo "CPPCHECK FAILED"
  exit 2
fi
grep -c '^Checking ' "$out/cppcheck.log" | sed 's/^/files checked: /'

echo ""
echo "== clang-tidy ($jobs at a time)"
SECONDS=0
mkdir -p "$build_dir/tidy"
if [ -n "${HM_TIDY_CHECKS:-}" ]; then
  config=(--checks="$HM_TIDY_CHECKS" --header-filter='.*/hmailserver/source/Server/.*' --exclude-header-filter='.*/quickjs/.*')
  echo "HM_TIDY_CHECKS=$HM_TIDY_CHECKS - NOT the job's check set"
else
  config=(--config-file=build/ci/clang-tidy.yaml)
fi
# One output file per translation unit, so that parallel runs do not interleave,
# and one exit status per file: a file clang-tidy could not parse is a hole in the
# scan, which the report below fails on.
export build_dir
tidy_one() {
  local file="$1" name
  name=$(printf '%s' "$file" | md5sum | cut -c1-16)
  shift
  clang-tidy -p "$build_dir" --quiet "$@" "$file" > "$build_dir/tidy/$name.txt" 2> "$build_dir/tidy/$name.err"
  echo "$? $file" > "$build_dir/tidy/$name.status"
}
export -f tidy_one
xargs -a "$out/files.txt" -P "$jobs" -I{} bash -c 'tidy_one "$@"' _ {} "${config[@]}"
# clang-tidy's own answer is 0, or 1 for an error it reports. Above 1 is how it
# ended, not what it found: 134 an abort, 137 a kill (the OOM killer's), 139 a
# segmentation fault. Such a file leaves no finding and no error in the report, so
# its baseline findings read as fixed and the job passes - measured on linix on 27
# September 2026, ten files at a time: clang-tidy aborted on two, one of them
# Unicode.cpp, whose nine findings were then "no longer found", and the job exited 0.
# Each such file is run again, alone; one that still does not finish is a hole in
# the scan.
for status in "$build_dir"/tidy/*.status; do
  read -r tidy_code tidy_file < "$status"
  if [ "$tidy_code" -gt 1 ]; then
    echo "clang-tidy ended with $tidy_code on $tidy_file; running it again, alone"
    tidy_one "$tidy_file" "${config[@]}"
  fi
done
unfinished=$(awk '$1 > 1' "$build_dir"/tidy/*.status)
if [ -n "$unfinished" ]; then
  echo "clang-tidy did not finish on these files, twice (exit status, file):"
  echo "$unfinished"
  echo "CLANG-TIDY FAILED"
  exit 2
fi
cat "$build_dir"/tidy/*.txt > "$out/clang-tidy.txt"
failed_files=$(awk '$1 != 0' "$build_dir"/tidy/*.status | wc -l)
statuses=("$build_dir"/tidy/*.status)
done_files=${#statuses[@]}
echo "clang-tidy analysed $done_files files after $SECONDS s; $failed_files exited non-zero (a finding in an error-level check, or a file it could not parse - the report says which)"
if [ "$done_files" -ne "$count" ]; then
  echo "clang-tidy ran on $done_files of $count files"
  exit 2
fi

echo ""
python3 build/ci/security-findings.py --job static-analysis \
  --cppcheck "$out/cppcheck.xml" --clang-tidy "$out/clang-tidy.txt" \
  --versions "$out/versions.json" --started "$started" --out "$out" "${write_baseline[@]}"
code=$?
rm -rf "$build_dir"
exit "$code"
