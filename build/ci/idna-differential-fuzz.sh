#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The differential fuzz of the server's UTS 46 against ada
# (fuzz/harness/idna_ada_fuzzer.cpp; hmailserver/docs/Fuzzing.md, "UTS 46
# against ada"): the domain parser, the host of a link's authority, and the link
# reader, each against what ada, the WHATWG URL parser of Node.js, makes of the
# same input. Built with clang, libFuzzer and AddressSanitizer; then ada is held
# to Unicode's conformance tests (fuzz/harness/idna_ada_conformance.cpp), so that
# what the fuzz sets aside as ada's departures from UTS 46 is what the file
# shows, and then the fuzz runs.
#
#   bash build/ci/idna-differential-fuzz.sh          build, check ada, fuzz
#   bash build/ci/idna-differential-fuzz.sh build    build only
#   bash build/ci/idna-differential-fuzz.sh check    build and check ada
#
# Nothing comes from GitHub. ada is the amalgamation (ada.h and ada.cpp) of
# ada 4.0.0 out of its Python package's source distribution on PyPI
# (files.pythonhosted.org), held to the SHA-256 PyPI publishes for it; and the
# Unicode 17.0.0 data its IDNA tables are of ("IDNA 17.0.0" in ada.cpp) comes
# from unicode.org, each file held to its SHA-256, for the table of the code
# points whose UTS 46 data differ between 17.0.0 and the server's 18.0.0 -
# build/generate-idna-tables.py --differences - which the harness sets aside;
# and Unicode 18.0.0's NormalizationTest.txt, likewise pinned, which the
# server's NFC is held to before a difference in NFC is put down to ada.
# To move ada, change the three ADA_ lines and, when its tables move to another
# Unicode version, the UCD_OLD lines with them. The build directory is kept from
# one run to the next: a fetched file already there and still the pinned one is
# not fetched again, and CMake builds only what changed.
#
# Run in the image of build/ci/linux.Dockerfile (clang with libclang-rt-dev,
# CMake, Ninja, python3, curl), from the root of a checkout, with access to
# PyPI and unicode.org. The environment:
#   HM_FUZZ_MINUTES   how long it fuzzes (30)
#   HM_FUZZ_JOBS      libFuzzer's fork mode, processes at once (4)
#   HM_FUZZ_OUT       where the log, the corpus and any finding go (idna-fuzz-results)
#   HM_BUILD_DIR      where the inputs are fetched and CMake builds (/tmp/hm-idna-fuzz),
#                     kept between runs
#   HM_BUILD_JOBS     ninja's -j (8)
#
# Exit 0: no difference. Exit 1: a difference (or a crash), its input in
# HM_FUZZ_OUT, or ada differs from the server on a conformance line in a way
# the fuzz would not set aside. Exit 2: it could not be built or run.
set -uo pipefail

mode="${1:-all}"
minutes="${HM_FUZZ_MINUTES:-30}"
fuzz_jobs="${HM_FUZZ_JOBS:-4}"
out="${HM_FUZZ_OUT:-idna-fuzz-results}"
build_dir="${HM_BUILD_DIR:-/tmp/hm-idna-fuzz}"
build_jobs="${HM_BUILD_JOBS:-8}"

ADA_VERSION=4.0.0
ADA_URL=https://files.pythonhosted.org/packages/98/4e/f62ef579f693298f62a4657a0429823444025f342161d53f5c1f7bd629c6/ada_url-4.0.0.tar.gz
ADA_SHA256=9f399ec867349800a4c2d3aa8df8c533c340494b04d34b1f082b97d9a8b76a07

NORMALIZATION_TEST_URL=https://www.unicode.org/Public/18.0.0/ucd/NormalizationTest.txt
NORMALIZATION_TEST_SHA256=25a50d816764b04abfb4a646d3eb2b2a803284c3873d9a06757b94fe4513dde3

UCD_OLD_VERSION=17.0.0
UCD_OLD=(
  "IdnaMappingTable.txt https://www.unicode.org/Public/17.0.0/idna/IdnaMappingTable.txt 87f05505dc026fdb2bff16132bdc68a8014675836882a9a2b1844540ad3be382"
  "UnicodeData.txt https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt 2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c"
  "CompositionExclusions.txt https://www.unicode.org/Public/17.0.0/ucd/CompositionExclusions.txt 2f239196ef3b5b61db5cc476e9bd80f534d15aa1b74e1be1dea5d042a344c85f"
  "DerivedJoiningType.txt https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedJoiningType.txt f39ebe974825d6736aee15582250307aa532b2cfab3caf3f86bd23fddc9c5c4d"
  "DerivedBidiClass.txt https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedBidiClass.txt 4867b4b7f0731ed1bfcd34cc6251211ff1542541fce0734b6fbda139ee80b3a4"
)

if [ ! -f fuzz/harness/idna_ada_fuzzer.cpp ]; then
  echo "Run this from the root of a checkout (no fuzz/harness/idna_ada_fuzzer.cpp here)."
  exit 2
fi

repository="$PWD"
inputs="$build_dir/inputs"
rm -rf "$out"
mkdir -p "$inputs/ucd-old" "$inputs/differences" "$out"

# A file fetched and held to its SHA-256, or the run ends; one already there
# and still the pinned file is kept.
fetch() {
  local url=$1 sha=$2 to=$3
  if [ -f "$to" ] && echo "$sha  $to" | sha256sum -c --quiet - 2> /dev/null; then
    return 0
  fi
  if ! curl -fsSL --retry 3 --max-time 300 -o "$to" "$url"; then
    echo "FETCH FAILED: $url"
    exit 2
  fi
  if ! echo "$sha  $to" | sha256sum -c --quiet -; then
    echo "NOT THE PINNED FILE: $url has SHA-256 $(sha256sum "$to" | cut -d' ' -f1), and $sha is pinned."
    exit 2
  fi
}

echo "== ada $ADA_VERSION, from PyPI"
fetch "$ADA_URL" "$ADA_SHA256" "$inputs/ada.tar.gz"
# Unpacked beside, and copied over a file from an earlier run only where it
# differs, so that ninja does not build an unchanged ada again.
rm -rf "$inputs/ada-unpacked"
mkdir -p "$inputs/ada-unpacked"
tar -xzf "$inputs/ada.tar.gz" -C "$inputs/ada-unpacked" "ada_url-$ADA_VERSION/ada_url/ada.h" "ada_url-$ADA_VERSION/ada_url/ada.cpp" "ada_url-$ADA_VERSION/LICENSE" || exit 2
ada_dir="$inputs/ada_url-$ADA_VERSION/ada_url"
mkdir -p "$ada_dir"
for name in ada_url/ada.h ada_url/ada.cpp LICENSE; do
  cmp -s "$inputs/ada-unpacked/ada_url-$ADA_VERSION/$name" "$inputs/ada_url-$ADA_VERSION/$name" \
    || cp "$inputs/ada-unpacked/ada_url-$ADA_VERSION/$name" "$inputs/ada_url-$ADA_VERSION/$name" || exit 2
done
if ! grep -q "^// IDNA $UCD_OLD_VERSION\$" "$ada_dir/ada.cpp"; then
  echo "ada.cpp does not say its IDNA tables are of Unicode $UCD_OLD_VERSION: move UCD_OLD with ADA_."
  grep -n '^// IDNA ' "$ada_dir/ada.cpp" | head -n 3
  exit 2
fi
grep -m1 '#define ADA_VERSION' "$ada_dir/ada.h"

echo ""
echo "== Unicode $UCD_OLD_VERSION, from unicode.org, and the code points whose UTS 46 data differ from the server's"
for entry in "${UCD_OLD[@]}"; do
  read -r name url sha <<< "$entry"
  fetch "$url" "$sha" "$inputs/ucd-old/$name"
done
python3 build/generate-idna-tables.py --differences "$inputs/ucd-old" --output "$inputs/differences/idna_version_differences.h" || exit 2
sed -n 4p "$inputs/differences/idna_version_differences.h"

echo ""
echo "== Unicode 18.0.0's NormalizationTest.txt, from unicode.org"
fetch "$NORMALIZATION_TEST_URL" "$NORMALIZATION_TEST_SHA256" "$inputs/NormalizationTest.txt"
head -n 1 "$inputs/NormalizationTest.txt"

echo ""
echo "== build (clang, libFuzzer, AddressSanitizer)"
clang++ --version | head -n 1
if ! cmake -S fuzz/idna -B "$build_dir/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DHM_ADA_DIR="$ada_dir" -DHM_IDNA_DIFFERENCES="$inputs/differences" > "$out/configure.log" 2>&1; then
  tail -n 40 "$out/configure.log"
  echo "BUILD FAILED: configure"
  exit 2
fi
if ! ninja -C "$build_dir/build" -j "$build_jobs" idna-ada-fuzzer idna-ada-conformance > "$out/build.log" 2>&1; then
  grep -E 'error|FAILED' "$out/build.log" | head -n 40
  echo "BUILD FAILED"
  exit 2
fi
fuzzer="$build_dir/build/idna-ada-fuzzer"
echo "built $fuzzer and $build_dir/build/idna-ada-conformance"

[ "$mode" = "build" ] && exit 0

echo ""
echo "== the server's NFC against NormalizationTest.txt, and ada against Unicode's conformance tests"
export ASAN_OPTIONS="abort_on_error=1:detect_leaks=0"
"$build_dir/build/idna-ada-conformance" "$inputs/NormalizationTest.txt" | tee "$out/conformance.log"
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
  echo "The server's NFC failed NormalizationTest.txt, or ada differs from the server on a conformance line in a way the fuzz would not set aside."
  exit 1
fi

[ "$mode" = "check" ] && exit 0

# ---------------------------------------------------------------- the seed corpus
#
# Every source column of IdnaTestV2.txt that is well-formed UTF-8 once its escapes
# are read, and the hosts of SURBL's own test links that are not ASCII.
python3 - "$out/corpus" <<'PY'
import os, re, sys
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
seen = set()
def unescape(text):
    text = re.sub(r"\\u([0-9A-Fa-f]{4})", lambda m: chr(int(m.group(1), 16)), text)
    return re.sub(r"\\x\{([0-9A-Fa-f]+)\}", lambda m: chr(int(m.group(1), 16)), text)
path = "hmailserver/source/Server/Common/Idna/ucd/IdnaTestV2.txt"
for line in open(path, encoding="utf-8"):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    source = line.split(";")[0].strip()
    if source == '""':
        continue
    try:
        data = unescape(source).encode("utf-8")
    except UnicodeEncodeError:
        continue
    if data and data not in seen and len(data) <= 1024:
        seen.add(data)
        open(os.path.join(out, "seed-%05d" % len(seen)), "wb").write(data)
for extra in ["evil.example", "\uff45\uff56\uff49\uff4c\uff0e\uff45\uff58\uff41\uff4d\uff50\uff4c\uff45",
              "evil\u3002example", "b\u00fccher.example", "xn--bcher-kva.example", "ev%C2%ADil.example",
              "\uff11\uff19\uff12\uff0e\uff10\uff0e\uff12\uff0e\uff11", "www.Example.COM"]:
    data = extra.encode("utf-8")
    if data not in seen:
        seen.add(data)
        open(os.path.join(out, "seed-%05d" % len(seen)), "wb").write(data)
print("seed corpus: %d inputs" % len(seen))
PY

echo ""
echo "== fuzz for $minutes minutes, $fuzz_jobs processes"
started=$(date +%s)
"$fuzzer" "$out/corpus" -fork="$fuzz_jobs" -max_total_time=$((minutes * 60)) -max_len=256 \
  -dict=fuzz/dict/idna.dict -artifact_prefix="$out/" -print_final_stats=1 > "$out/fuzz.log" 2>&1
code=$?
ended=$(date +%s)
elapsed=$((ended - started))

grep -E '^#[0-9]+|INFO: (fuzzed|exiting)|stat::' "$out/fuzz.log" | tail -n 8
if [ $code -ne 0 ] || grep -q 'DIFFERENCE (' "$out/fuzz.log"; then
  echo ""
  grep -A4 'DIFFERENCE (' "$out/fuzz.log" | head -n 40
  ls -1 "$out"/crash-* "$out"/leak-* "$out"/timeout-* 2> /dev/null
  echo "A DIFFERENCE, after $elapsed s (exit $code): the input is in $out; replay it with $fuzzer <file>."
  exit 1
fi

echo "No difference in $elapsed s: $(ls -1 "$out/corpus" | wc -l) inputs in the corpus."
exit 0
