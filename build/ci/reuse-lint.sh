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
# Then the original hMailServer's files (build/generate-reuse-original.py). The list of
# them, build/reuse-original-paths.txt, is made from the original's three tags and the
# fork's first commit: a clone that has not got them - CI's depth-1 one - has them
# fetched from its origin into a git directory of their own in the job's temporary
# folder, the commits and trees alone (--filter=blob:none, about 4 MB), so that the
# checkout itself is left as it was, each fetch bounded to 120 seconds and tried up
# to three times; HM_REUSE_ORIGINAL_GIT_DIR tells the generator
# where they are. REUSE.toml's tables of the original's files must be what the
# generator writes from that list and the tree, and the list what the tags give
# (--check --require-tags, which exits 2 when the tags are not at hand), and reuse's
# record of each of the original's files but the third parties' must name Martin
# Knafve, and of none that the fork made at the original's paths (--holders, over
# reuse lint --json). Then debian/copyright: every file the Debian source package
# carries has there the licences reuse records for it (build/ci/reuse-debian.py).
#
# The negative controls, after the tree passes: five files the job puts in the
# checkout and takes out again, each of which reuse lint must refuse, by name -
#   a new C++ source in the server tree without the project's header, which no table
#     of REUSE.toml covers;
#   a new registration script among the original's in Server/COM, and a new fixture
#     among the original's in the Windows suite's Resources, neither with a header:
#     the original's files are named one by one, so a file the fork adds beside them
#     is refused until a table classifies it;
#   a file whose header names a licence that LICENSES/ has no text for;
#   a licence text in LICENSES/ that no file names;
# one table, put at the end of REUSE.toml and taken out again, that gives one of the
# original's files, IMAPFetch.cpp, the maintainers alone: reuse lint passes it, and
# --holders must refuse it, by name; one of the fork's files, the generator itself,
# added to build/reuse-original-paths.txt with REUSE.toml's tables written from that
# list, as an edit of both would leave them: --check --require-tags, holding the list
# to the tags, must refuse it, by name; and a copy of debian/copyright that has lost
# the RFC 7208 suite's change log from its paragraph, which reuse-debian.py must
# refuse, by name.
# So a REUSE.toml table grown to cover the whole tree or a directory of the
# original's, a reuse that has stopped reading the tree, a check of the original's
# files that has stopped seeing them or their tags, or a Debian record that has
# stopped being compared, fails the job too.
#
# Exit status: 0 the tree is compliant, its record of the original's files names
# Martin Knafve, debian/copyright agrees with it, and every control was refused; 1 the
# tree is not compliant, a record of the original's does not name him or one of the
# fork's does, debian/copyright says otherwise, or a control passed; 2 reuse could not
# be installed or run, the original's tags could not be fetched, or this is not the
# root of a checkout.
set -uo pipefail

for need in REUSE.toml LICENSES build/ci/reuse-requirements.txt build/ci/reuse-build-requirements.txt; do
  if [ ! -e "$need" ]; then
    echo "Run this from the root of a checkout (no $need here)."
    exit 2
  fi
done

TAGS=(hMailServer-5.7.0-b2788 hMailServer-5.7.0-b2843 hMailServer-5.7.0-b3013)
FIRST_COMMIT=fe45f43c3db6c40dfdb42c57f3db2cc312d53af1
LIST=build/reuse-original-paths.txt
COPYRIGHT=hmailserver/source/Server/platform/packaging/debian-src/debian/copyright

work=$(mktemp -d /tmp/hm-reuse.XXXXXX)
controls=()
cleanup() {
  for f in "${controls[@]}"; do
    rm -f -- "$f"
  done
  # REUSE.toml and the list as they were, if a control was interrupted with its
  # change in.
  if [ -f "$work/REUSE.toml" ]; then
    cp -- "$work/REUSE.toml" REUSE.toml
  fi
  if [ -f "$work/paths.txt" ]; then
    cp -- "$work/paths.txt" "$LIST"
  fi
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

echo ""
echo "== the original's tags, which build/reuse-original-paths.txt is made from"
have_original() {
  for tag in "${TAGS[@]}"; do
    git "$@" rev-parse --verify --quiet "refs/tags/$tag^{commit}" > /dev/null || return 1
  done
  git "$@" rev-parse --verify --quiet "$FIRST_COMMIT^{commit}" > /dev/null
}
if have_original; then
  echo "The checkout has them."
else
  # Into a git directory of its own, so that the checkout, which the runner keeps
  # for the next job, is left as it was. The origin's address is not printed: in a
  # pipeline it carries the job's token.
  original="$work/original.git"
  git init --quiet --bare "$original"
  if ! url=$(git remote get-url origin 2> /dev/null) || [ -z "$url" ]; then
    echo "This checkout has no origin to fetch the original's tags from."
    exit 2
  fi
  git --git-dir="$original" remote add origin "$url"
  refspecs=()
  for tag in "${TAGS[@]}"; do
    refspecs+=("+refs/tags/$tag:refs/tags/$tag")
  done
  # Each fetch is bounded, so that an origin that accepts the connection and never
  # answers does not hold the job to its own timeout: git gives up on a transfer
  # slower than 1,000 bytes a second for 30 seconds (http.lowSpeedLimit and
  # http.lowSpeedTime), and timeout stops it after 120 seconds in any case, the
  # connection's set-up included, which those two do not time. A fetch that fails is
  # tried again, three times in all, so that one passing network error does not fail
  # the job.
  fetch_original() {
    local try status
    for try in 1 2 3; do
      timeout 120 git -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=30 --git-dir="$original" \
        fetch --quiet --no-tags --filter=blob:none origin "$@"
      status=$?
      if [ "$status" -eq 0 ]; then
        return 0
      fi
      if [ "$status" -eq 124 ]; then
        echo "The fetch from the origin took more than 120 s and was stopped (try $try of 3)."
      else
        echo "The fetch from the origin failed, exit $status (try $try of 3)."
      fi
      if [ "$try" -lt 3 ]; then
        sleep 5
      fi
    done
    return 1
  }
  started=$(date +%s)
  if ! fetch_original "${refspecs[@]}" || ! fetch_original "$FIRST_COMMIT"; then
    echo "The original's tags (${TAGS[*]}) or the fork's first commit could not be fetched from the origin."
    exit 2
  fi
  if ! have_original --git-dir="$original"; then
    echo "The fetch did not bring the original's tags and the fork's first commit."
    exit 2
  fi
  echo "Fetched from the origin, their commits and trees alone, in $(( $(date +%s) - started )) s."
  export HM_REUSE_ORIGINAL_GIT_DIR="$original"
fi

echo ""
echo "== the original's files: the list and REUSE.toml's tables of them, and Martin Knafve in reuse's record of each"
for script in build/generate-reuse-original.py build/ci/reuse-debian.py; do
  if ! python3 "$script" --self-test; then
    echo "$script's self-test failed."
    exit 1
  fi
done
python3 build/generate-reuse-original.py --check --require-tags
status=$?
if [ "$status" -ne 0 ]; then
  echo "build/reuse-original-paths.txt is not what the original's tags give, or REUSE.toml's tables of the"
  echo "original's files are not what build/generate-reuse-original.py writes (it exited $status): run it,"
  echo "and commit what it writes."
  exit "$status"
fi
if ! "$reuse" lint --json > "$work/lint.json"; then
  echo "reuse lint --json did not pass where reuse lint did."
  exit 2
fi
python3 build/generate-reuse-original.py --holders "$work/lint.json"
status=$?
if [ "$status" -ne 0 ]; then
  echo "A table of REUSE.toml that names one of the original's files has to name Martin Knafve as a"
  echo "holder too, and one that names a file the fork made at the original's paths must not"
  echo "(build/generate-reuse-original.py says why; it exited $status)."
  exit 1
fi

echo ""
echo "== debian/copyright: the licences of every file the Debian source package carries"
python3 build/ci/reuse-debian.py "$work/lint.json"
status=$?
if [ "$status" -ne 0 ]; then
  echo "debian/copyright gives a file the Debian source package carries other licences than REUSE.toml's"
  echo "record does (build/ci/reuse-debian.py exited $status): say the same in both."
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

control=hmailserver/source/Server/COM/InterfaceReuseNegativeControl.rgs
controls+=("$control")
printf 'HKCR\n{\n}\n' > "$control"
refused "a new file without a header among the original's COM scripts" "$control" "InterfaceReuseNegativeControl.rgs" || failed=1

control=hmailserver/test/RegressionTests/Resources/ReuseNegativeControl.txt
controls+=("$control")
printf 'A fixture that no table of REUSE.toml classifies.\n' > "$control"
refused "a new fixture without a header among the original's test resources" "$control" "ReuseNegativeControl.txt" || failed=1

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

# The holders' control: a table at the end of REUSE.toml, the last to match and so the
# one reuse takes, gives one of the original's files the maintainers alone.
control=hmailserver/source/Server/IMAP/IMAPFetch.cpp
cp -- REUSE.toml "$work/REUSE.toml"
printf '\n[[annotations]]\npath = "%s"\nprecedence = "override"\n%s = "2026 Christopher Holloway / Progressive Robot Ltd"\n%s = "AGPL-3.0-or-later"\n' \
  "$control" "SPDX-FileCopyrightText" "SPDX-License-Identifier" >> REUSE.toml
"$reuse" lint --json > "$work/control.json"
code=$?
cp -- "$work/REUSE.toml" REUSE.toml
rm -f -- "$work/REUSE.toml"
if [ "$code" -ne 0 ]; then
  echo "reuse lint refused the holders' control's table itself (exit $code): the control proves nothing."
  failed=1
else
  out=$(python3 build/generate-reuse-original.py --holders "$work/control.json" 2>&1)
  code=$?
  if [ "$code" -ne 1 ] || ! printf '%s\n' "$out" | grep -q -F -- "$control"; then
    printf '%s\n' "$out" | tail -n 20
    echo "--holders did not refuse a table that gives $control, one of the original's files, the maintainers alone (exit $code)."
    failed=1
  else
    echo "refused: a table that drops Martin Knafve from one of the original's files"
  fi
fi

# The list's control: one of the fork's files added to the list of the original's,
# and REUSE.toml's tables written from that list by the generator with no tags at
# hand (an empty git directory in their place), so that tables and list agree, as an
# edit of both would leave them. --check --require-tags, with the tags, must refuse
# the list and name the file.
control=build/generate-reuse-original.py
cp -- REUSE.toml "$work/REUSE.toml"
cp -- "$LIST" "$work/paths.txt"
printf '%s\n' "$control" >> "$LIST"
git init --quiet --bare "$work/no-tags.git"
HM_REUSE_ORIGINAL_GIT_DIR="$work/no-tags.git" python3 build/generate-reuse-original.py > "$work/control-write.txt" 2>&1
written=$?
grep -q -F -- "    \"$control\"," REUSE.toml
tabled=$?
out=$(python3 build/generate-reuse-original.py --check --require-tags 2>&1)
code=$?
cp -- "$work/REUSE.toml" REUSE.toml
cp -- "$work/paths.txt" "$LIST"
rm -f -- "$work/REUSE.toml" "$work/paths.txt"
if [ "$written" -ne 0 ] || [ "$tabled" -ne 0 ]; then
  cat "$work/control-write.txt"
  echo "The list's control did not get REUSE.toml's tables written from the changed list (exit $written): it proves nothing."
  failed=1
elif [ "$code" -ne 1 ] || ! printf '%s\n' "$out" | grep -q -F -- "+ $control"; then
  printf '%s\n' "$out" | tail -n 20
  echo "--check --require-tags did not refuse a list of the original's files that names $control, one of the fork's (exit $code)."
  failed=1
else
  echo "refused: one of the fork's files added to the list of the original's, with the tables written from it"
fi

# The Debian control: debian/copyright without the RFC 7208 suite's change log in its
# paragraph, which then falls to "Files: *" and the project's licence, as it did until
# the paragraph named it.
control=hmailserver/source/Server/SMTP/SPF/Conformance/rfc7208-tests.CHANGES
grep -v -F -- "$control" "$COPYRIGHT" > "$work/copyright.control"
out=$(python3 build/ci/reuse-debian.py "$work/lint.json" "$work/copyright.control" 2>&1)
code=$?
if [ "$code" -ne 1 ] || ! printf '%s\n' "$out" | grep -q -F -- "$control"; then
  printf '%s\n' "$out" | tail -n 20
  echo "build/ci/reuse-debian.py did not refuse a debian/copyright that gives $control the project's licence (exit $code)."
  failed=1
else
  echo "refused: a debian/copyright that gives a third party's file the project's licence"
fi

if [ "$failed" -ne 0 ]; then
  exit 1
fi
echo ""
echo "Every file's copyright and licence are known, every licence's text is in LICENSES/, every one of"
echo "the original's files names Martin Knafve and the list of them is the original's tags', and"
echo "debian/copyright gives every file the Debian source package carries the same licences; a file"
echo "without them, a file the fork adds among the original's, a licence without its text, a text no"
echo "file uses, a table that drops him, a fork file in the list and a Debian paragraph that lost a"
echo "file are each refused."
