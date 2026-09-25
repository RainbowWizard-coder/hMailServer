#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-sanitize job of .gitlab-ci.yml: the SNMP agent's tester, snmp-tests,
# built with AddressSanitizer (the CMake option HM_SANITIZE) and run so that the
# first memory error aborts it. The agent parses datagrams from the network, and
# a sanitizer is what turns a read one byte past a buffer - which would never
# fault in a release build - into a failed job.
#
#   bash build/ci/linux-sanitize.sh
#
# The option and the target are the SNMP row's. On a tree without them - the
# CMake build offers no HM_SANITIZE (cmake -LH does not list it) - this says so
# and exits 0: there is nothing to sanitize yet, and that is not a failure. On a
# tree with the option and without the target it fails, because then the build
# no longer makes what this job exists to run.
#
# Run in the image of build/ci/linux.Dockerfile (clang and its sanitizer
# runtimes, llvm-symbolizer), from the root of a checkout. The environment:
#   HM_BUILD_DIR    where CMake configures and ninja builds (/tmp/hm-sanitize)
#   HM_BUILD_JOBS   ninja's -j (6)
#   HM_OUT_DIR      where the logs go (linux-sanitize)
set -uo pipefail

build_dir="${HM_BUILD_DIR:-/tmp/hm-sanitize}"
jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-linux-sanitize}"
source_dir=hmailserver/source/Server

if [ ! -f "$source_dir/CMakeLists.txt" ]; then
  echo "Run this from the root of a checkout (no $source_dir/CMakeLists.txt here)."
  exit 2
fi
rm -rf "$out" "$build_dir"
mkdir -p "$out"
export CC=clang CXX=clang++

echo "== does this tree's CMake build offer HM_SANITIZE?"
if ! cmake -S "$source_dir" -B "$build_dir" -G Ninja > "$out/probe.log" 2>&1; then
  cat "$out/probe.log"
  echo "CONFIGURE FAILED"
  exit 3
fi
if ! cmake -N -LH "$build_dir" 2> /dev/null | grep -q '^HM_SANITIZE:'; then
  echo "No: cmake -LH lists no HM_SANITIZE. The SNMP row adds the option and snmp-tests;"
  echo "until they are in this tree there is nothing for this job to build. SKIPPED."
  rm -rf "$build_dir"
  exit 0
fi
cmake -N -LH "$build_dir" 2> /dev/null | grep -B1 '^HM_SANITIZE:'

echo ""
echo "== configure with -DHM_SANITIZE=ON, RelWithDebInfo"
rm -rf "$build_dir"
if ! cmake -S "$source_dir" -B "$build_dir" -G Ninja -DHM_SANITIZE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      > "$out/configure.log" 2>&1; then
  cat "$out/configure.log"
  echo "CONFIGURE FAILED"
  exit 3
fi
tail -n 5 "$out/configure.log"
if ! ninja -C "$build_dir" -t targets all 2> /dev/null | grep -q '^snmp-tests:'; then
  echo "FAILED: the build offers HM_SANITIZE but makes no snmp-tests target, which is what this job runs."
  exit 1
fi

echo ""
echo "== ninja snmp-tests (-j $jobs)"
started=$(date +%s)
ninja -C "$build_dir" -j "$jobs" snmp-tests > "$out/build.log" 2>&1
code=$?
echo "ninja exit $code after $(( $(date +%s) - started )) s"
if [ "$code" -ne 0 ]; then
  grep -E '^FAILED:|error:' "$out/build.log" | sort -u | head -n 60
  echo "BUILD FAILED - the whole log is the job's artifact $out/build.log"
  exit 3
fi

echo ""
echo "== snmp-tests under AddressSanitizer (ASAN_OPTIONS=abort_on_error=1)"
( cd "$build_dir" && ASAN_OPTIONS=abort_on_error=1 timeout 900 ./snmp-tests ) > "$out/snmp-tests.log" 2>&1
code=$?
tail -n 60 "$out/snmp-tests.log"
rm -rf "$build_dir"
echo ""
if [ "$code" -ne 0 ]; then
  echo "FAILED: snmp-tests exited $code under the sanitizer; $out/snmp-tests.log has the report."
  exit 1
fi
echo "snmp-tests passed under AddressSanitizer."
