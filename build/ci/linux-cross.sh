#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-cross job of .gitlab-ci.yml: every core translation unit compiled
# FOR AArch64 (build/linux-tu-census.sh --target aarch64-linux-gnu), on linix,
# which is x86-64. It proves what a distribution's build farm needs - that the
# sources are clean enough to be cross-compiled - and not that the server runs
# on that architecture: the old workflow's native AArch64 build did that, on a
# hosted ARM runner, and the project has no ARM machine to run it on.
#
# A syntax census does not link, so the host's Boost, OpenSSL, zlib, libpq and
# LDAP headers serve for the target (the census says why), with the target's own
# C and C++ headers from g++-aarch64-linux-gnu, which the CI image carries.
#
#   bash build/ci/linux-cross.sh
#
# Leaves linux-cross/census.txt, and every file's compiler log in
# linux-cross/log when one failed. Exit 0 when every file compiled.
set -uo pipefail

jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-linux-cross}"
census="${HM_CENSUS_DIR:-/tmp/hm-tu-census}"

rm -rf "$out"
mkdir -p "$out"

OUT="$census" bash build/linux-tu-census.sh -j "$jobs" --target aarch64-linux-gnu | tee "$out/census.txt"
failed=$(sed -n 's/^Failed:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$out/census.txt" | head -n 1)
offered=$(sed -n 's/^Translation units offered:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$out/census.txt" | head -n 1)

echo ""
if [ -z "$failed" ] || [ -z "$offered" ] || [ "$offered" -eq 0 ]; then
  echo "FAILED: the census printed no count; it did not run."
  exit 2
fi
if [ "$failed" -ne 0 ]; then
  cp -r "$census/log" "$out/log" 2> /dev/null || true
  echo "FAILED: $failed of $offered core translation units do not compile for AArch64; linux-cross/log has each one's compiler output."
  exit 1
fi
rm -rf "$census"
echo "All $offered core translation units compile for AArch64."
