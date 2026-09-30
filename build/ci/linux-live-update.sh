#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-live-update job (.gitlab/ci/live-update.yml): the live update off, and
# aware of where it runs (docs/LiveUpdateKinds.md). It builds the server again
# with -DHM_LIVE_UPDATE=OFF and a distribution's command, and runs
# build/test-install-kind.sh against it and against linux-build's server (the
# default build): a snap, a container, the OFF build and its install. Run in the
# image of build/ci/linux.Dockerfile from the root of a checkout, beside a
# PostgreSQL the job starts as a service; by hand, with HM_PG_* naming one:
#
#   HM_PG_HOST=127.0.0.1 bash build/ci/linux-live-update.sh
#
# The environment it reads:
#   HM_SERVER_BINARY   the default build's server (linux-build/hmailserver)
#   HM_OFF_BUILD_DIR   where the OFF build is configured (/tmp/hm-live-update-off)
#   HM_BUILD_JOBS      ninja's -j (6: the runner gives a job six CPUs)
#   HM_OUT_DIR         where the logs go (linux-live-update)
#   HM_PG_*            the database, as build/test-install-kind.sh reads them
set -uo pipefail

server="${HM_SERVER_BINARY:-$PWD/linux-build/hmailserver}"
build_dir="${HM_OFF_BUILD_DIR:-/tmp/hm-live-update-off}"
jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-linux-live-update}"
# What a Debian package would be built with; any command the option allows would do.
command="apt upgrade hmailserver"

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi
if [ ! -x "$server" ]; then
  echo "REFUSED: no server binary at $server - the linux-build job's artifact is missing."
  exit 2
fi

rm -rf "$out"
mkdir -p "$out"

export CC=clang CXX=clang++
echo "== configure with -DHM_LIVE_UPDATE=OFF -DHM_UPDATE_COMMAND=\"$command\""
rm -rf "$build_dir"
if ! cmake -S hmailserver/source/Server -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DHM_LIVE_UPDATE=OFF "-DHM_UPDATE_COMMAND=$command" > "$out/configure.log" 2>&1; then
  cat "$out/configure.log"
  echo "CONFIGURE FAILED"
  exit 3
fi
grep -E '^-- Live update' "$out/configure.log"

echo ""
echo "== build the server (ninja -j $jobs hmailserver)"
started=$(date +%s)
if ! ninja -C "$build_dir" -j "$jobs" hmailserver > "$out/build.log" 2>&1; then
  # grep -m, not `| head`, which left grep to die of SIGPIPE under pipefail.
  grep -m 40 -E '^FAILED:|error:' "$out/build.log"
  echo "BUILD FAILED - the whole log is the job's artifact $out/build.log"
  exit 3
fi
finished=$(date +%s)
echo "built in $((finished - started)) s"

echo ""
bash build/test-install-kind.sh --server "$server" --off-server "$build_dir/hmailserver" \
  --off-command "$command" --install-off "$build_dir" 2>&1 | tee "$out/test-install-kind.log"
code=${PIPESTATUS[0]}

rm -rf "$build_dir"
exit "$code"
