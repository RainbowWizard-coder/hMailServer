#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-sanitize job of .gitlab-ci.yml: the test programs built with
# AddressSanitizer (the CMake option HM_SANITIZE) and run so that the first memory
# error aborts the job. snmp-tests feeds the agent's BER decoder datagrams from
# the network - a read one byte past a buffer, which would never fault in a
# release build, becomes a failed job. workqueue-teardown-tests removes a work
# queue while one of its worker threads is still inside it, which is where a queue
# freed under a running thread shows as a use-after-free.
#
#   bash build/ci/linux-sanitize.sh
#
# The option is the SNMP row's. On a tree without it - the CMake build offers no
# HM_SANITIZE (cmake -LH does not list it) - this says so and exits 0: there is
# nothing to sanitize yet, and that is not a failure. On a tree with the option
# and without one of the targets it fails, because then the build no longer makes
# what this job exists to run.
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
# Each probe writes the command's whole output to a file and greps the file. Piped
# into `grep -q`, which stops reading at its first match, the command is killed by
# SIGPIPE once its output outgrows the pipe, and under pipefail the test then fails
# with the line present: `ninja -t targets all` over batch41's tree did exactly that
# in pipeline 2887524317.
cmake -N -LH "$build_dir" > "$out/options.txt" 2> /dev/null
if ! grep -q '^HM_SANITIZE:' "$out/options.txt"; then
  echo "No: cmake -LH lists no HM_SANITIZE. The SNMP row adds the option and snmp-tests;"
  echo "until they are in this tree there is nothing for this job to build. SKIPPED."
  rm -rf "$build_dir"
  exit 0
fi
grep -B1 '^HM_SANITIZE:' "$out/options.txt"

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
ninja -C "$build_dir" -t targets all > "$out/targets.txt" 2> /dev/null
# The targets this job builds and runs under the sanitizer. snmp-tests feeds the
# BER decoder bytes nobody meant it to see; workqueue-teardown-tests removes a
# work queue while a worker is still inside it. A tree with HM_SANITIZE but
# without one of these no longer makes what the job exists to run.
sanitize_targets=(snmp-tests workqueue-teardown-tests)
for target in "${sanitize_targets[@]}"; do
  if ! grep -q "^$target:" "$out/targets.txt"; then
    echo "FAILED: the build offers HM_SANITIZE but makes no $target target, which is what this job runs."
    exit 1
  fi
done

echo ""
echo "== ninja ${sanitize_targets[*]} (-j $jobs)"
started=$(date +%s)
ninja -C "$build_dir" -j "$jobs" "${sanitize_targets[@]}" > "$out/build.log" 2>&1
code=$?
ended=$(date +%s)
echo "ninja exit $code after $(( ended - started )) s"
if [ "$code" -ne 0 ]; then
  # sed, not head, reads sort's list to its end: head's early exit left sort to die
  # of SIGPIPE, as the probes above did.
  grep -E '^FAILED:|error:' "$out/build.log" | sort -u | sed -n '1,60p'
  echo "BUILD FAILED - the whole log is the job's artifact $out/build.log"
  exit 3
fi

# Each target run under the sanitiser, each aborting on the first memory error
# (ASAN_OPTIONS=abort_on_error=1). A non-zero exit from any is the job's failure,
# but every one is run so the log carries all of them. The work-queue teardown
# runs its stop's ten-second give-up and a twenty-second task, so it is given a
# longer bound than the decoder tests.
overall=0
for target in "${sanitize_targets[@]}"; do
  echo ""
  echo "== $target under AddressSanitizer (ASAN_OPTIONS=abort_on_error=1)"
  ( cd "$build_dir" && ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 timeout 900 "./$target" ) > "$out/$target.log" 2>&1
  code=$?
  tail -n 60 "$out/$target.log"
  if [ "$code" -ne 0 ]; then
    echo "FAILED: $target exited $code under the sanitizer; $out/$target.log has the report."
    overall=1
  else
    echo "$target passed under AddressSanitizer."
  fi
done

rm -rf "$build_dir"
echo ""
if [ "$overall" -ne 0 ]; then
  exit 1
fi
echo "The sanitizer targets passed under AddressSanitizer."
