#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The rules both secret scans run gitleaks with: GitLab's, from the analyzer's image
# (/gitleaks.toml, secret-detection-rules), with two entries of their global
# [[allowlists]] table changed, which left out every path under a directory named
# build, bin, obj, target or packages - this tree's build/ among them, which neither
# scan read until 10 October 2026. The (?:target|build|bin|obj) entry is taken out,
# and packages out of the (?:node_modules|bower_components|packages) one, so that
# node_modules and bower_components stay out as GitLab has them. Nothing else of
# GitLab's file changes, and no list of paths is read from the tree: the table is
# GitLab's, derived here, so that no committed data file can widen what the scans leave
# out. This script is the one file of the tree that decides it, and a gate file (below).
# The secret_detection job (.gitlab/ci/gitlab-security.yml) runs this before the
# analyzer and puts what it writes in place of /gitleaks.toml, which the analyzer reads;
# build/scan-secrets.py runs its own tree's copy in the same image before gitleaks.
# GitLab's custom ruleset, which could name another file, is Ultimate's (the analyzer
# reads .gitlab/secret-detection-ruleset.toml only when GITLAB_FEATURES has
# sast_custom_rulesets), and gitleaks' [extend] adds a config's allowlists to its
# base's and takes none away.
#
#   sh build/ci/secret-detection-rules.sh <GitLab's gitleaks.toml> <out>
#
# It refuses - exit 1, writing nothing - when GitLab's file has not exactly one
# [[allowlists]] line; when GitLab's table, from that line to the end of the file,
# does not hash to GITLAB_TABLE below, so that a rules version that changes what
# GitLab leaves out is read, and this script brought up to date with it, before either
# scan takes it; or when either change does not apply to exactly one line of the
# table. A change to this file changes what both scans read: it is one of the gates'
# own files (build/land-via-mr.py's GATE_FILES), which a batch changes only with a
# person's word that the change was read, and the landing's scan prints the lines a
# range changes here. POSIX sh, sed, grep, awk, sha256sum and cut, as the analyzer's
# busybox has them.
set -eu
usage="usage: sh build/ci/secret-detection-rules.sh <gitleaks.toml of the image> <out>"
rules=${1:?$usage}
out=${2:?$usage}

# GitLab's global [[allowlists]] table, from its line to the end of /gitleaks.toml,
# in secret-detection-rules 0.25.1 (the analyzer 7.40.0's).
GITLAB_TABLE=1005c12cb351a097bb87027a653a6a7052a2dfd0dda8cf6ee4e7b06ae92648e6
# The two entries changed, each as GitLab writes it, and what the second becomes.
LEFT_OUT_DIRECTORIES="    '''(?:^|/)(?:target|build|bin|obj)(?:/.*)?\$''',"
DEPENDENCY_DIRECTORIES="    '''(?:^|/)(?:node_modules|bower_components|packages)(?:/.*)?\$''',"
DEPENDENCY_DIRECTORIES_KEPT="    '''(?:^|/)(?:node_modules|bower_components)(?:/.*)?\$''',"

fail() {
  echo "secret-detection-rules: $*" >&2
  rm -f "$out.part"
  exit 1
}

[ -r "$rules" ] || fail "cannot read GitLab's rules, $rules"
count=$(grep -c '^\[\[allowlists\]\]$' "$rules" || true)
[ "$count" = 1 ] || fail "$rules has $count [[allowlists]] lines, not one: GitLab's rules are not laid out as this was written for - read them, and this script"
found=$(sed -n '/^\[\[allowlists\]\]$/,$p' "$rules" | sha256sum | cut -d' ' -f1)
if [ "$found" != "$GITLAB_TABLE" ]; then
  fail "GitLab's [[allowlists]] table in $rules hashes to $found, not $GITLAB_TABLE, the one this changes: the image's rules leave out another set of paths. Read GitLab's table, and bring this script up to date with it - the entries it changes, and GITLAB_TABLE to $found"
fi
# Each change applies to exactly one line of the table, or nothing is written: the
# awk exits 3 when either line is not there once.
LEFT_OUT_DIRECTORIES=$LEFT_OUT_DIRECTORIES DEPENDENCY_DIRECTORIES=$DEPENDENCY_DIRECTORIES \
  DEPENDENCY_DIRECTORIES_KEPT=$DEPENDENCY_DIRECTORIES_KEPT awk '
  BEGIN {
    left_out = ENVIRON["LEFT_OUT_DIRECTORIES"]
    dependencies = ENVIRON["DEPENDENCY_DIRECTORIES"]
    kept = ENVIRON["DEPENDENCY_DIRECTORIES_KEPT"]
  }
  $0 == "[[allowlists]]" { table = 1 }
  table && $0 == left_out { taken++; next }
  table && $0 == dependencies { print kept; changed++; next }
  { print }
  END { if (taken != 1 || changed != 1) exit 3 }
' "$rules" > "$out.part" || fail "GitLab's table in $rules has not exactly one line of each entry this changes: $LEFT_OUT_DIRECTORIES and $DEPENDENCY_DIRECTORIES"
mv "$out.part" "$out"
echo "secret-detection-rules: GitLab's rules, but for the entries that left out the build, bin, obj, target and packages directories, in $out"
