#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# How much of the native server the Linux regression suite reaches: line, region
# and branch coverage, measured with clang's source-based coverage. Run in the
# image of build/ci/linux.Dockerfile from the root of a checkout, beside a
# PostgreSQL, the way linux-suite runs:
#
#   bash build/ci/linux-coverage.sh          build, the testers, the suite, report
#   bash build/ci/linux-coverage.sh build    only the instrumented build
#   bash build/ci/linux-coverage.sh testers  only the *-tests programs
#   bash build/ci/linux-coverage.sh suite    only the suite, against that build
#   bash build/ci/linux-coverage.sh report   only the report, from the counters
#
# By hand on linix, with a PostgreSQL container on a network of its own (no
# listener of either container is published, so nothing on the host is held):
#
#   docker network create hmcov
#   docker run -d --name hmcov-pg --network hmcov -e POSTGRES_USER=hmailserver \
#       -e POSTGRES_PASSWORD=hmtest -e POSTGRES_DB=hmtest postgres:18
#   docker run --rm --network hmcov -e HM_PG_HOST=hmcov-pg -v "$PWD":/src -w /src \
#       hmailserver-ci/linux:<tag> bash build/ci/linux-coverage.sh
#
# What it measures, and what it does not. The figure is a FLOOR for the server as
# a whole: the Linux suite links the fixtures the REST shims can drive
# (LinuxRegressionTests.csproj says which), fewer than the Windows suite runs, and
# the Linux build compiles the portable core, not the COM layer or the Windows
# service. Everything under hmailserver/source/Server that the CMake build
# compiles is in the denominator - headers included, and the server's own
# in-process testers (the *Tester classes ClassTester runs on Windows) - except
# the tester programs' own main files; the vendored third-party code (zlib,
# QuickJS) is left out of the headline and reported separately. The Windows
# figure (build/coverage-windows.ps1) counts what OpenCppCoverage calls a line of
# an optimised build, a smaller denominator: the two percentages are not the
# same measure and are not to be compared point for point.
#
# The environment it reads:
#   HM_COV_DIR      where the build, the counters and the report go
#                   (linux-coverage under the checkout)
#   HM_BUILD_JOBS   ninja's -j (6)
#   HM_PG_HOST      the PostgreSQL linux-suite.sh uses (postgres)
#   HM_COV_FILTER   a dotnet test filter, to measure part of the suite; the
#                   run's scope.txt, and so the report, says so ("all", or
#                   unset, is the whole suite). Handed to linux-suite.sh as
#                   HM_LINUX_SUITE_FILTER, which this script always sets
#   HM_COV_RUN      a name for this run of the suite: its counters and results
#                   go beside an earlier run's instead of replacing them, and
#                   the report counts both
#   HM_COV_RUNS     the report step only: which runs to count, by name
#                   ("testers suite" - the directories profiles-testers and
#                   profiles-suite); every run under HM_COV_DIR when unset
#
# What it leaves in HM_COV_DIR:
#   summary.txt        the totals, and the runs they count, each with its scope
#   report.txt         llvm-cov report, one line per file
#   coverage.json      llvm-cov export -summary-only, for tools
#   lcov.info          llvm-cov export -format=lcov (line and branch records);
#                      both name the source files by their paths in the checkout
#   suite/             linux-suite.sh's results (console.log, the TRX, junit.xml);
#                      suite-<HM_COV_RUN>/ for a named run
#   profiles-*/        the raw counters, one directory per run, and its scope.txt
#                      (what the run was, and its tests: run, passed, failed, skipped)
#
# A run of the suite counts only if it ran to its end - dotnet test wrote its TRX,
# with at least one test run. One that did not (the server did not start, the
# restore failed) is VOID: its counters are removed and the script exits 3, since
# they are the set-up's and a report would give them the suite's name. A failed
# test does not void a run.
set -uo pipefail

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi

root="$PWD"
cov="${HM_COV_DIR:-$root/linux-coverage}"
build_dir="$cov/build"
# One directory of counters per run: the testers', and the suite's (a second run
# of part of the suite, named by HM_COV_RUN, adds to the first rather than
# replacing it). The report merges every one of them.
run="${HM_COV_RUN:+-$HM_COV_RUN}"
profiles="$cov/profiles-suite$run"
suite_results="$cov/suite$run"
tree="$cov/tree$run"
jobs="${HM_BUILD_JOBS:-6}"
src="$root/hmailserver/source/Server"
binary="$build_dir/hmailserver"

build() {
  export CC=clang CXX=clang++
  clang --version | head -n 1
  rm -rf "$build_dir" "$cov"/profiles-*
  mkdir -p "$cov"
  if ! cmake -S "$src" -B "$build_dir" -G Ninja -DHM_COVERAGE=ON \
        -DCMAKE_BUILD_TYPE=Release > "$cov/configure.log" 2>&1; then
    cat "$cov/configure.log"
    echo "CONFIGURE FAILED"
    exit 3
  fi
  started=$(date +%s)
  # The server and the tester programs (spf-tests and the others): what they
  # reach counts, as the unit tests they are.
  if ! ninja -C "$build_dir" -j "$jobs" > "$cov/build.log" 2>&1; then
    # grep -m, not `| head`, which left grep to die of SIGPIPE under pipefail.
    grep -m 60 -E '^FAILED:|error:' "$cov/build.log"
    echo "BUILD FAILED - $cov/build.log"
    exit 3
  fi
  echo "instrumented build in $(( $(date +%s) - started )) s: $binary"
  "$binary" --version | head -n 1
}

suite() {
  if [ ! -f "$binary" ]; then
    echo "REFUSED: no instrumented server at $binary - run the build step first."
    exit 2
  fi
  # The results too, so that a TRX left by an earlier run can never stand for this one.
  rm -rf "$profiles" "$suite_results"
  mkdir -p "$profiles"
  # "all" is the whole suite, as linux-suite.sh reads it.
  local filter="${HM_COV_FILTER:-}"
  [ "$filter" = "all" ] && filter=""
  if [ -n "$filter" ]; then
    # The filter itself beside the counters; the scope says how many terms it has.
    printf '%s\n' "$filter" > "$profiles/filter.txt"
    echo "suite${run}: the tests of a filter of $(tr '|' '\n' <<< "$filter" | grep -c .) terms (profiles-suite${run}/filter.txt), started $(date -u +%FT%TZ)" > "$profiles/scope.txt"
  else
    echo "suite${run}: the whole of LinuxRegressionTests, started $(date -u +%FT%TZ)" > "$profiles/scope.txt"
  fi
  # %p: one file per process (the --create-database and --set-admin-password runs
  # are processes too); %c: counters mapped to the file as they change, so a
  # server that linux-suite.sh has to SIGKILL after its twenty seconds still
  # leaves them.
  export LLVM_PROFILE_FILE="$profiles/hm-%p%c.profraw"
  export HM_SERVER_BINARY="$binary"
  export HM_RESULTS_DIR="$suite_results"
  export HM_TREE="$tree"
  started=$(date +%s)
  # HM_LINUX_SUITE_FILTER is always set here, so that one left in the environment
  # cannot make "the whole" a part.
  if [ -n "$filter" ]; then
    echo "the suite with the filter: $filter"
    HM_LINUX_SUITE_FILTER="$filter" bash build/ci/linux-suite.sh
  else
    HM_LINUX_SUITE_FILTER="" bash build/ci/linux-suite.sh
  fi
  code=$?
  HM_RESULTS_DIR="$suite_results" HM_TREE="$tree" bash build/ci/linux-suite.sh collect > /dev/null
  echo "the suite exited $code after $(( ($(date +%s) - started) / 60 )) min; $(find "$profiles" -name '*.profraw' | wc -l) counter file(s)"
  # A failed test does not void the measure: it is which lines ran. The pass or
  # fail truth is linux-suite's, on the uninstrumented build. A suite that did not
  # run to its end does: the server that did not start, the restore that failed,
  # the database that did not answer still leave counters - those of the database's
  # creation and the server's start - and a report from them would carry this run's
  # scope, the whole suite's. The TRX dotnet test writes at its end is the proof
  # that it ran; its counts go into the scope, and so into summary.txt.
  local counts=""
  if [ -f "$suite_results/linux-suite.trx" ]; then
    counts=$(python3 - "$suite_results/linux-suite.trx" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r"<Counters\b([^>]*)>", text)
if m:
    a = {k: int(v) for k, v in re.findall(r'(\w+)="(\d+)"', m.group(1))}
    total, executed = a.get("total", 0), a.get("executed", 0)
    if executed > 0:
        failed = a.get("failed", 0) + a.get("error", 0) + a.get("timeout", 0) + a.get("aborted", 0)
        print(f"{total:,} tests: {a.get('passed', 0):,} passed, {failed:,} failed, {total - executed:,} skipped")
PY
)
  fi
  if [ -z "$counts" ]; then
    echo "VOID: suite${run} did not run to its end (linux-suite.sh exited $code; no TRX, or one that ran no test). Its counters are the set-up's and not the suite's, and are removed so that no report counts them."
    rm -rf "$profiles"
    exit 3
  fi
  echo "  $counts" >> "$profiles/scope.txt"
  echo "suite${run}: $counts"
  return 0
}

testers() {
  rm -rf "$cov/profiles-testers"
  mkdir -p "$cov/profiles-testers"
  echo "testers: the *-tests programs, started $(date -u +%FT%TZ)" > "$cov/profiles-testers/scope.txt"
  local program name code
  for program in $(find "$build_dir" -maxdepth 1 -type f -name '*-tests' -perm -u+x | sort); do
    name=$(basename "$program")
    ( cd "$build_dir" && LLVM_PROFILE_FILE="$cov/profiles-testers/$name-%p%c.profraw" timeout 900 "$program" ) > "$cov/$name.log" 2>&1
    code=$?
    echo "$name exit $code"
  done
  return 0
}

report() {
  # The runs to count: HM_COV_RUNS names them, or every profiles-* directory.
  local dirs=() name
  if [ -n "${HM_COV_RUNS:-}" ]; then
    for name in $HM_COV_RUNS; do
      if [ ! -d "$cov/profiles-$name" ]; then
        echo "REFUSED: HM_COV_RUNS names $name, and there is no $cov/profiles-$name."
        exit 2
      fi
      dirs+=("$cov/profiles-$name")
    done
  else
    for name in "$cov"/profiles-*; do
      [ -d "$name" ] && dirs+=("$name")
    done
  fi
  if [ ${#dirs[@]} -eq 0 ]; then
    echo "REFUSED: no counters under $cov/profiles-* - run the testers or the suite first."
    exit 2
  fi
  local raws
  raws=$(find "${dirs[@]}" -maxdepth 1 -name '*.profraw' -size +0 2> /dev/null)
  if [ -z "$raws" ]; then
    echo "REFUSED: no counters under $cov/profiles-* - run the testers or the suite first."
    exit 2
  fi
  # shellcheck disable=SC2086
  llvm-profdata merge -sparse $raws -o "$cov/hmailserver.profdata" || exit 4
  # The build maps the source directory to "." (-ffile-prefix-map), which llvm-cov
  # then reads against the build directory: -path-equivalence turns those paths
  # back into the sources', so the report names real files.
  cd "$src" || exit 2
  # Left out of the figure: the vendored code, the system's headers, and the tester
  # programs' own main files, which are test code rather than the server's.
  local ignore='(^|/)(zlib|quickjs|QuickJS|libraries)/|^/usr/|(^|/)platform/[a-z_]+_tests_main[.]cpp$'
  # The server and every tester program: a function that only a tester links is
  # counted too (SnmpTester.cpp, which only snmp-tests links). Given several
  # binaries, llvm-cov warns that some hundreds of functions "have mismatched
  # data" - 642 on 26 September 2026, against one for the server alone - and no
  # server source's figure was lower for it.
  local objects=("$binary") program
  for program in $(find "$build_dir" -maxdepth 1 -type f -name '*-tests' -perm -u+x | sort); do
    objects+=(-object "$program")
  done
  local args=(-instr-profile="$cov/hmailserver.profdata" "${objects[@]}" -path-equivalence="$build_dir,$src"
              -ignore-filename-regex="$ignore")
  llvm-cov report "${args[@]}" -show-branch-summary -show-region-summary > "$cov/report.txt" || exit 4
  llvm-cov export "${args[@]}" -summary-only > "$cov/coverage.json" || exit 4
  llvm-cov export "${args[@]}" -format=lcov > "$cov/lcov.info" || exit 4
  # The two exports name each file by the path the build compiled it under; tools
  # that read them want the path in the checkout.
  sed -i "s|^SF:$build_dir/|SF:$src/|" "$cov/lcov.info"
  sed -i "s|\"filename\":\"$build_dir/|\"filename\":\"$src/|g" "$cov/coverage.json"
  llvm-cov report -instr-profile="$cov/hmailserver.profdata" "${objects[@]}" -path-equivalence="$build_dir,$src" \
      -ignore-filename-regex='^/usr/' > "$cov/report-with-vendored.txt" || true
  cd "$root" || exit 2
  local scopes
  scopes=$(for name in "${dirs[@]}"; do
    if [ -f "$name/scope.txt" ]; then cat "$name/scope.txt"; else echo "$(basename "$name"): no scope recorded"; fi
  done)
  python3 - "$cov/coverage.json" "$scopes" > "$cov/summary.txt" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
t = data["data"][0]["totals"]
def pct(k):
    v = t[k]
    return f'{v["covered"]:,} of {v["count"]:,} ({v["percent"]:.2f}%)'
print("hmailserver (Linux build) under the Linux regression suite")
print("the runs counted:")
for line in sys.argv[2].splitlines():
    print("  " + line)
print("lines:    ", pct("lines"))
print("regions:  ", pct("regions"))
print("branches: ", pct("branches"))
print("functions:", pct("functions"))
print("files:    ", len(data["data"][0]["files"]))
PY
  cat "$cov/summary.txt"
}

case "${1:-all}" in
  build) build ;;
  suite) suite ;;
  testers) testers ;;
  report) report ;;
  all) build && testers && suite && report ;;
  *) echo "usage: linux-coverage.sh [build|testers|suite|report]"; exit 2 ;;
esac
