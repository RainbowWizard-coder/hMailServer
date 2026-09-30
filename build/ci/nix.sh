#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The nix job of .gitlab/ci/nix.yml: the project's flake (flake.nix, nix/) built and
# tested in Docker Hub's nixos/nix image (hmailserver/docs/Nix.md). Run as root from
# the root of a checkout, with GitHub kept out of the job (.no-github):
#
#   bash build/ci/nix.sh
#
# What it does, each step failing the job:
#   1. holds flake.lock to the rule that nothing comes from GitHub: every locked
#      input is a tarball from releases.nixos.org;
#   2. evaluates every output of the flake for both systems (nix flake check
#      --no-build), which catches a package or module that does not evaluate on
#      aarch64-linux, where nothing here builds;
#   3. nix build .#hmailserver: the server built with nixpkgs' libraries, and its
#      checkPhase - the server's --version and every tester program; then the
#      built server's --version;
#   4. the NixOS test (checks.x86_64-linux.nixos, nix/test.nix), where it can run:
#      with /dev/kvm, or with HM_NIXOS_TEST_TCG=1 under QEMU's TCG, which needs no
#      KVM and is many times slower. Neither: it says so and the job passes on the
#      first three (Nix.md, "Where the NixOS test runs").
#
# The flake is read from git (git+file, shallow, since the job's clone is), so the
# build sees the commit's tracked files and nothing else in the checkout. Nix's own
# settings come from NIX_CONFIG (the job's variables): the flakes feature, the flake
# registry off - it names GitHub - and four cores. Every path it substitutes comes
# from cache.nixos.org and is checked against cache.nixos.org-1, the key the image
# trusts; what is not there is built, and nothing but hmailserver was on 29 September
# 2026 (a dry run lists what would be).
#
# What it leaves (HM_OUT_DIR, nix-results/ by default; nix/ is the flake's own):
# metadata.json, check.log, build.log, version.txt and, where the test ran, test.log.
set -euo pipefail

OUT="${HM_OUT_DIR:-nix-results}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
rm -rf "$OUT"
mkdir -p "$OUT"

if [ ! -f flake.nix ] || [ ! -f flake.lock ]; then
  echo "Run this from the root of a checkout (no flake.nix and flake.lock here)."
  exit 2
fi

nix --version
# The flake as git holds it: the clone is shallow (GIT_DEPTH), which Nix refuses
# unless it is told.
flake="git+file://$PWD?shallow=1"

echo ""
echo "== 1. every input from releases.nixos.org, none from GitHub"
nix flake metadata --json "$flake" > "$OUT/metadata.json"
# nixos/nix has no jq and no python: nix eval reads the lock file itself.
problems=$(nix eval --raw --impure --expr '
  let
    lock = builtins.fromJSON (builtins.readFile ./flake.lock);
    inputs = builtins.removeAttrs lock.nodes [ lock.root ];
    bad = builtins.filter (name:
      let l = inputs.${name}.locked; in
      !(l.type == "tarball" && builtins.match "https://releases\\.nixos\\.org/.*" l.url != null))
      (builtins.attrNames inputs);
  in builtins.concatStringsSep " " bad')
if [ -n "$problems" ]; then
  echo "REFUSED: flake.lock locks input(s) not taken as a tarball from releases.nixos.org: $problems"
  exit 1
fi
echo "every locked input is a releases.nixos.org tarball"

echo ""
echo "== 2. every output evaluates, for x86_64-linux and aarch64-linux"
nix flake check --no-build --all-systems "$flake" 2>&1 | tee "$OUT/check.log"

echo ""
echo "== 3. nix build .#hmailserver"
started=$EPOCHSECONDS
if ! nix build -L --out-link "$OUT/result" "$flake#hmailserver" > "$OUT/build.log" 2>&1; then
  grep -E 'error|FAILED|failed' "$OUT/build.log" | sort -u | head -n 60
  tail -n 60 "$OUT/build.log"
  echo "BUILD FAILED - the whole log is $OUT/build.log"
  exit 1
fi
echo "built in $(( EPOCHSECONDS - started )) s: $(readlink "$OUT/result")"
grep -E '^hmailserver> == ' "$OUT/build.log" || true
"$OUT/result/bin/hmailserver" --version | tee "$OUT/version.txt"
rm -f "$OUT/result"

echo ""
echo "== 4. the NixOS test"
if [ -c /dev/kvm ]; then
  # Nix builds as its build users (nixbld*), not as root, and a /dev/kvm passed into
  # a container keeps the host's group, which they are not in: QEMU is then refused
  # KVM and falls back to TCG without failing, which is how a first run in WSL on 29
  # September 2026 took TCG's time with the device right there. The job's own device
  # node is opened to them.
  chmod a+rw /dev/kvm
  how="with KVM"
  extra=()
elif [ "${HM_NIXOS_TEST_TCG:-}" = 1 ]; then
  # A NixOS test asks for the build machine's kvm feature, which Nix sees only
  # where /dev/kvm is. QEMU, asked for KVM it cannot have, falls back to TCG.
  how="under QEMU's TCG (no /dev/kvm here; HM_NIXOS_TEST_TCG=1)"
  extra=(--option system-features "nixos-test benchmark big-parallel kvm")
else
  echo "not run: no /dev/kvm in this job, and HM_NIXOS_TEST_TCG is not 1 (Nix.md, \"Where the NixOS test runs\")."
  echo "PASSED: the flake's inputs, its evaluation and the package's build and checks."
  exit 0
fi
echo "running $how"
started=$EPOCHSECONDS
if ! nix build -L --no-link "${extra[@]}" "$flake#checks.x86_64-linux.nixos" > "$OUT/test.log" 2>&1; then
  tail -n 80 "$OUT/test.log"
  echo "THE NIXOS TEST FAILED - the whole log is $OUT/test.log"
  exit 1
fi
grep -E 'PASSED|S: |wait_for|succeed' "$OUT/test.log" | tail -n 40 || true
echo "the NixOS test passed $how in $(( EPOCHSECONDS - started )) s"
echo "PASSED: the flake's inputs, its evaluation, the package's build and checks, and the NixOS test."
