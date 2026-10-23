#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The reuse job of .gitlab-ci.yml: the tree held to the REUSE Specification 3.3
# (https://reuse.software/spec-3.3/) by the REUSE project's own tool, reuse lint -
# every file's copyright and licence known, by its own header or by a table of
# REUSE.toml, every licence named has its text in LICENSES/, and LICENSES/ holds no
# other (CONTRIBUTING.md, "Code style and licence headers"). Run in
# python:3.14-slim-trixie from the root of a checkout, with git and libmagic
# installed (the job installs both from Debian, git so that reuse leaves out what git
# ignores, libmagic for the encoding of each file); by hand it is the same:
#
#   docker run --rm -v "$PWD":/src -w /src python:3.14-slim-trixie bash -c \
#     'apt-get update -qq && apt-get install -y -qq --no-install-recommends git libmagic1t64 &&
#      git config --global --add safe.directory /src && bash build/ci/reuse-lint.sh'
#
# Where reuse comes from: PyPI, at the version and hashes build/ci/reuse-requirements.txt
# pins (pip --require-hashes, into a virtual environment). PyPI has no wheel of reuse
# for Python 3.14, so pip builds it from its sdist, with --no-build-isolation and its
# build backend, poetry-core, installed first from build/ci/reuse-build-requirements.txt,
# hashed the same way; build/ci/reuse-requirements.in says how to move either. reuse
# reads each file's encoding with python-magic, and REUSE_ENCODING_MODULE=python-magic
# makes a missing libmagic stop it, rather than leave it on a fallback that takes
# some UTF-8 files for binary ones (reuse 6.1.0's change log).
#
# The negative controls, after the tree passes: three files the job puts in the
# checkout and takes out again, each of which reuse lint must refuse, by name -
#   a new C++ source in the server tree without the project's header, which no table
#     of REUSE.toml covers;
#   a file whose header names a licence that LICENSES/ has no text for;
#   a licence text in LICENSES/ that no file names.
# So a REUSE.toml table grown to cover the whole tree, or a reuse that has stopped
# reading the tree, fails the job too.
#
# Exit status: 0 the tree is compliant and every control was refused; 1 the tree is
# not compliant, or a control passed; 2 reuse could not be installed or run, or this
# is not the root of a checkout.
set -uo pipefail

for need in REUSE.toml LICENSES build/ci/reuse-requirements.txt build/ci/reuse-build-requirements.txt; do
  if [ ! -e "$need" ]; then
    echo "Run this from the root of a checkout (no $need here)."
    exit 2
  fi
done

work=$(mktemp -d /tmp/hm-reuse.XXXXXX)
controls=()
cleanup() {
  for f in "${controls[@]}"; do
    rm -f -- "$f"
  done
  rm -rf "$work"
}
trap cleanup EXIT

echo "== reuse, from PyPI, at the hashes build/ci/reuse-requirements.txt pins"
python3 -m venv "$work/venv"
pip=("$work/venv/bin/pip" install --quiet --disable-pip-version-check --no-cache-dir --require-hashes)
if ! "${pip[@]}" -r build/ci/reuse-build-requirements.txt; then
  echo "pip could not install poetry-core from build/ci/reuse-build-requirements.txt"
  exit 2
fi
if ! "${pip[@]}" --no-build-isolation -r build/ci/reuse-requirements.txt; then
  echo "pip could not install reuse from build/ci/reuse-requirements.txt"
  exit 2
fi
reuse="$work/venv/bin/reuse"
export REUSE_ENCODING_MODULE=python-magic
if ! version=$("$reuse" --version 2>&1); then
  printf '%s\n' "$version"
  echo "reuse does not start"
  exit 2
fi
printf '%s\n' "$version" | head -n 1

echo ""
echo "== reuse lint"
"$reuse" lint
status=$?
if [ "$status" -ne 0 ]; then
  echo ""
  echo "The tree is not REUSE-compliant (reuse lint exited $status): give each file named above the"
  echo "project's header, or a table of REUSE.toml for a file that cannot carry one, and put the text"
  echo "of every licence it names in LICENSES/ (CONTRIBUTING.md, \"Code style and licence headers\")."
  exit 1
fi

# Each control: the file is written, reuse lint runs over the tree, the file is taken
# out, and reuse lint must have failed and named it.
refused() {
  local name="$1" control="$2" expect="$3" out code
  out=$("$reuse" lint --lines 2>&1)
  code=$?
  rm -f -- "$control"
  if [ "$code" -eq 0 ]; then
    echo "reuse lint passed $name ($control): it is not checking."
    return 1
  fi
  if ! printf '%s\n' "$out" | grep -q -F -- "$expect"; then
    printf '%s\n' "$out" | tail -n 20
    echo "reuse lint failed with $name ($control), but did not name $expect."
    return 1
  fi
  echo "refused: $name"
  return 0
}

echo ""
echo "== the negative controls"
failed=0
control=hmailserver/source/Server/Common/Util/ReuseNegativeControl.cpp
controls+=("$control")
printf 'int ReuseNegativeControl()\n{\n   return 0;\n}\n' > "$control"
refused "a new source file without the project's header" "$control" "ReuseNegativeControl.cpp" || failed=1

control=reuse-negative-control.txt
controls+=("$control")
# REUSE-IgnoreStart
printf 'SPDX-FileCopyrightText: 2026 Christopher Holloway / Progressive Robot Ltd\nSPDX-License-Identifier: LicenseRef-Reuse-Negative-Control\n' > "$control"
# REUSE-IgnoreEnd
refused "a header naming a licence LICENSES/ has no text for" "$control" "LicenseRef-Reuse-Negative-Control" || failed=1

control=LICENSES/LicenseRef-Reuse-Negative-Control-Unused.txt
controls+=("$control")
printf 'A licence that no file of the tree names.\n' > "$control"
refused "a licence text that no file names" "$control" "LicenseRef-Reuse-Negative-Control-Unused" || failed=1

if [ "$failed" -ne 0 ]; then
  exit 1
fi
echo ""
echo "Every file's copyright and licence are known and every licence's text is in LICENSES/, and a"
echo "file without them, a licence without its text and a text no file uses are each refused."
