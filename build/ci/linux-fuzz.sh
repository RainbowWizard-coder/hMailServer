#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-fuzz job of .gitlab-ci.yml, which a GitLab pipeline schedule starts
# every night: the three MIME harnesses under fuzz/ built with clang++ and
# libFuzzer, every kept input and every committed reproducer replayed, then each
# harness run coverage-guided for a fixed time on the corpus the previous nights
# grew. It is the Linux half of the old GitHub workflow fuzz-nightly.yml, which
# built the same harnesses with clang-cl on a hosted Windows runner; the harness
# sources, the dictionary and the reproducers are the same files, and
# fuzz/build-fuzz.ps1 and fuzz/run-fuzz.ps1 remain the Windows way to run them
# (hmailserver/docs/Fuzzing.md).
#
#   bash build/ci/linux-fuzz.sh          build, replay, fuzz
#   bash build/ci/linux-fuzz.sh build    build only (and the seed corpus)
#   bash build/ci/linux-fuzz.sh replay   build, then replay only
#
# Run in the image of build/ci/linux.Dockerfile (clang with libclang-rt-dev),
# from the root of a checkout. The environment it reads:
#   HM_FUZZ_MINUTES   minutes each harness is fuzzed for (20; three run in turn)
#   HM_FUZZ_JOBS      libFuzzer workers per harness (2)
#   HM_FUZZ_OUT       where the findings and the logs go (fuzz-results)
#
# What it leaves:
#   fuzz/corpus/<target>/     the grown corpus; the job caches it, so tomorrow
#                             starts from tonight
#   fuzz/bin/, fuzz/build/    the build (ignored by git)
#   fuzz-results/             each harness's log and, on a finding, the input
#                             libFuzzer wrote (the job keeps this as its artifact)
#
# Exit 0: nothing found. Exit 1: a harness found something, or a kept input no
# longer survives. Exit 2: it could not be built or run.
set -uo pipefail

mode="${1:-all}"
minutes="${HM_FUZZ_MINUTES:-20}"
jobs="${HM_FUZZ_JOBS:-2}"
out="${HM_FUZZ_OUT:-fuzz-results}"

if [ ! -f fuzz/harness/fuzz_mime_common.cpp ]; then
  echo "Run this from the root of a checkout (no fuzz/harness here)."
  exit 2
fi

targets=(mime_message_fuzzer mime_header_fuzzer mime_decode_fuzzer)

# The same translation units build-fuzz.ps1 compiles, in the same order: the
# real parser and the four utilities it needs, then the harness's environment.
# RegressionTests/MIME/FuzzHarness.cs holds the Windows list to the tree; this
# one fails at its first missing file.
server_sources=(
  hmailserver/source/Server/Common/Mime/Mime.cpp
  hmailserver/source/Server/Common/Mime/MimeChar.cpp
  hmailserver/source/Server/Common/Mime/MimeCode.cpp
  hmailserver/source/Server/Common/Mime/MimeType.cpp
  hmailserver/source/Server/Common/Mime/CodePages.cpp
  hmailserver/source/Server/Common/Util/Charset.cpp
  hmailserver/source/Server/Common/Util/ByteBuffer.cpp
  hmailserver/source/Server/Common/Util/Unicode.cpp
  hmailserver/source/Server/Common/Util/RegularExpression.cpp
  hmailserver/source/Server/Common/Util/Parsing/StringParser.cpp
)
harness_sources=(
  fuzz/harness/fuzz_environment.cpp
  fuzz/harness/fuzz_mime_common.cpp
)

rm -rf "$out"
mkdir -p "$out"

# ---------------------------------------------------------------- the seed corpus
#
# What fuzz/make-corpus.ps1 writes, byte for byte: every .eml and .txt under the
# three test directories as a message seed; its header block (through the first
# CRLF CRLF) as a header seed; and the same block behind a zero selector byte as
# a decode seed.
seeds() {
  python3 - <<'PY'
import os, re, sys
roots = ["hmailserver/test/RegressionTests/Resources",
         "hmailserver/test/RegressionTests/Messages",
         "hmailserver/test/TestData/DKIM"]
out = "fuzz/corpus/seeds"
dirs = {k: os.path.join(out, k) for k in ("message", "header", "decode")}
for d in dirs.values():
    if os.path.isdir(d):
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
    os.makedirs(d, exist_ok=True)
used, messages, headers = set(), 0, 0
for root in roots:
    if not os.path.isdir(root):
        sys.exit("seed source directory missing: " + root)
    for base, _, files in sorted(os.walk(root)):
        for name in sorted(files):
            if os.path.splitext(name)[1].lower() not in (".eml", ".txt"):
                continue
            path = os.path.join(base, name)
            relative = path.replace("/", "\\")
            flat = re.sub(r"[^A-Za-z0-9._-]", "_", relative)
            seed = flat + ".seed"
            suffix = 2
            while seed in used:
                seed = "%s._%d.seed" % (flat, suffix)
                suffix += 1
            used.add(seed)
            data = open(path, "rb").read()
            if not data:
                continue
            open(os.path.join(dirs["message"], seed), "wb").write(data)
            messages += 1
            end = data.find(b"\r\n\r\n")
            if end < 0:
                continue
            block = data[:end + 4]
            open(os.path.join(dirs["header"], seed), "wb").write(block)
            open(os.path.join(dirs["decode"], seed), "wb").write(b"\x00" + block)
            headers += 1
if messages == 0:
    sys.exit("no seed messages: a run on an empty corpus finds nothing and looks like a clean run")
print("seed corpus: %d message, %d header, %d decode" % (messages, headers, headers))
PY
}

# ---------------------------------------------------------------- the build
build() {
  local build_dir=fuzz/build/linux bin_dir=fuzz/bin
  rm -rf "$build_dir" "$bin_dir"
  mkdir -p "$build_dir/include" "$bin_dir"

  # "stdafx.h" and "StdAfx.h" both resolve to the harness's shim, as build-fuzz.ps1
  # makes the one spelling resolve on Windows; Linux tells the two apart.
  local shim="$PWD/fuzz/harness/shim/stdafx.h"
  printf '#pragma once\n#include "%s"\n' "$shim" > "$build_dir/include/StdAfx.h"

  local flags=(
    -std=c++20 -O1 -g -w
    -DNDEBUG -DUNICODE -D_UNICODE -DHM_PLATFORM_POSIX=1
    -I"$PWD/fuzz/harness/shim"
    -I"$build_dir/include"
    -I"$PWD/fuzz/harness"
    -I"$PWD/hmailserver/source/Server/Common"
  )

  clang++ --version | head -n 1
  local objects=() source object
  for source in "${server_sources[@]}" "${harness_sources[@]}"; do
    [ -f "$source" ] || { echo "BUILD FAILED: $source is not in the tree; fix the list in build/ci/linux-fuzz.sh."; return 2; }
    object="$build_dir/$(basename "$source" .cpp).o"
    echo "  compiling $source"
    if ! clang++ -c -fsanitize=fuzzer-no-link,address "${flags[@]}" "$source" -o "$object" > "$build_dir/compile.log" 2>&1; then
      cat "$build_dir/compile.log" | head -n 60
      cp "$build_dir/compile.log" "$out/compile.log"
      echo "BUILD FAILED: $source"
      return 2
    fi
    objects+=("$object")
  done
  for target in "${targets[@]}"; do
    echo "  linking $target"
    if ! clang++ -fsanitize=fuzzer,address "${flags[@]}" "fuzz/harness/$target.cpp" "${objects[@]}" \
         -o "$bin_dir/$target" > "$build_dir/link.log" 2>&1; then
      cat "$build_dir/link.log" | head -n 60
      cp "$build_dir/link.log" "$out/link.log"
      echo "BUILD FAILED: $target"
      return 2
    fi
  done
  ls -la "$bin_dir"
}

# ---------------------------------------------------------------- running a harness
#
# The flags run-fuzz.ps1 passes, so a finding here reproduces there: the
# dictionary, 16 KB inputs, 2 GB of memory and 25 seconds per input before
# libFuzzer calls it a leak or a hang, and 77 for a finding.
run() {
  local target="$1" how="$2" root="$PWD"
  local seeds_for=() corpus="$root/fuzz/corpus/$target" artifacts="$root/fuzz/artifacts/$target"
  case "$target" in
    mime_message_fuzzer) seeds_for=("$root/fuzz/corpus/seeds/message") ;;
    mime_header_fuzzer)  seeds_for=("$root/fuzz/corpus/seeds/header" "$root/fuzz/corpus/seeds/message") ;;
    mime_decode_fuzzer)  seeds_for=("$root/fuzz/corpus/seeds/decode") ;;
  esac
  [ -d "fuzz/regression/$target" ] && seeds_for+=("$root/fuzz/regression/$target")
  mkdir -p "$corpus" "$artifacts"

  local arguments=(
    -dict="$root/fuzz/dict/mime.dict"
    -artifact_prefix="$artifacts/"
    -max_len=16384 -rss_limit_mb=2048 -timeout=25 -error_exitcode=77 -print_final_stats=1
  )
  if [ "$how" = replay ]; then
    arguments+=(-runs=0)
  else
    arguments+=(-max_total_time=$((minutes * 60)))
    if [ "$jobs" -gt 1 ]; then
      arguments+=(-jobs="$jobs" -workers="$jobs")
    fi
  fi

  echo ""
  echo "== $target: $how$( [ "$how" = fuzz ] && echo " for $minutes minute(s), $jobs worker(s)" ), corpus $(find "$corpus" -type f | wc -l) input(s)"
  local log="$root/$out/$target-$how.log" code
  # In the results directory, because the workers write their fuzz-<n>.log into
  # the directory they start in; the parent's own output is the log above.
  rm -f "$root/$out"/fuzz-*.log
  ( cd "$root/$out" && "$root/fuzz/bin/$target" "${arguments[@]}" "$corpus" "${seeds_for[@]}" ) > "$log" 2>&1
  code=$?
  grep -h -E '(DONE|INITED|stat::number_of_executed_units|stat::peak_rss_mb|ERROR: (AddressSanitizer|libFuzzer)|^SUMMARY)' "$log" "$root/$out"/fuzz-*.log 2> /dev/null | tail -n 12
  for worker in "$root/$out"/fuzz-*.log; do
    [ -f "$worker" ] && mv "$worker" "$root/$out/$target-$how-$(basename "$worker")"
  done
  if [ "$code" -eq 0 ] && ! compgen -G "$artifacts/*" > /dev/null; then
    echo "$target: nothing found (exit 0)"
    return 0
  fi
  if [ "$code" -eq 77 ] || compgen -G "$artifacts/*" > /dev/null; then
    echo "$target: FINDING (exit $code). The input libFuzzer wrote:"
    ls -la "$artifacts"
    mkdir -p "$root/$out/artifacts/$target"
    cp "$artifacts"/* "$root/$out/artifacts/$target/" 2> /dev/null || true
    # A worker's report is in its own log, not in the parent's.
    grep -l -E 'ERROR: (AddressSanitizer|libFuzzer)' "$log" "$root/$out/$target-$how"-fuzz-*.log 2> /dev/null | head -n 1 | xargs -r tail -n 60
    echo "Reproduce it with one execution: fuzz/bin/$target <that file> (fuzz/run-fuzz.ps1 -Reproduce on Windows),"
    echo "then read hmailserver/docs/Fuzzing.md, \"What to do with a finding\"."
    return 1
  fi
  tail -n 40 "$log"
  echo "$target exited $code, which is neither a clean run nor a finding: it did not start, or a flag was refused."
  return 2
}

seeds || exit 2
build || exit 2
[ "$mode" = build ] && exit 0

failed=0
for target in "${targets[@]}"; do
  run "$target" replay || failed=1
done
if [ "$failed" -ne 0 ]; then
  echo ""
  echo "FAILED on replay: a kept input or a committed reproducer no longer survives."
  exit 1
fi
[ "$mode" = replay ] && { echo "Every kept input and every reproducer replays clean."; exit 0; }

found=""
for target in "${targets[@]}"; do
  run "$target" fuzz || found="$found $target"
done
echo ""
if [ -n "$found" ]; then
  echo "FOUND SOMETHING:$found - the reproducers are in the job's artifacts ($out/artifacts)."
  exit 1
fi
echo "Three harnesses, $minutes minute(s) each: nothing found. The corpus grew to:"
for target in "${targets[@]}"; do
  echo "  $target: $(find "fuzz/corpus/$target" -type f | wc -l) input(s)"
done
