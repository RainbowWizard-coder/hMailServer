#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The rules both secret scans run gitleaks with: GitLab's, from the analyzer's image
# (/gitleaks.toml, secret-detection-rules), up to their global [[allowlists]] table,
# and the project's table after them (.gitlab/secret-detection-allowlist.toml), which
# is GitLab's but for the entries that left out every path under a directory named
# build, bin, obj, target or packages - this tree's build/ among them, which neither
# scan read until 10 October 2026. The secret_detection job (.gitlab/ci/
# gitlab-security.yml) runs it before the analyzer and puts what it writes in place of
# /gitleaks.toml, which the analyzer reads; build/scan-secrets.py runs it in the same
# image before gitleaks. GitLab's custom ruleset, which could name another file, is
# Ultimate's (the analyzer reads .gitlab/secret-detection-ruleset.toml only when
# GITLAB_FEATURES has sast_custom_rulesets), and gitleaks' [extend] adds a config's
# allowlists to its base's and takes none away.
#
#   sh build/ci/secret-detection-rules.sh <GitLab's gitleaks.toml> <the project's table> <out>
#
# It refuses - exit 1, writing nothing - when GitLab's file has not exactly one
# [[allowlists]] line, when the project's table has not exactly one "# replaces
# sha256:<hex>" line, or when GitLab's table, from its [[allowlists]] line to the end
# of the file, does not hash to that value: a rules version that changes what GitLab
# leaves out is read before either scan takes it, and the table brought up to date
# with it. The project's file may be checked out CRLF (.gitattributes), and is read
# without its carriage returns. POSIX sh, sed, grep, sha256sum, cut and tr, as the
# analyzer's busybox has them.
set -eu
usage="usage: sh build/ci/secret-detection-rules.sh <gitleaks.toml of the image> <the allowlist table> <out>"
rules=${1:?$usage}
table=${2:?$usage}
out=${3:?$usage}

fail() {
  echo "secret-detection-rules: $*" >&2
  rm -f "$out.part"
  exit 1
}

[ -r "$rules" ] || fail "cannot read GitLab's rules, $rules"
[ -r "$table" ] || fail "cannot read the project's table, $table"
count=$(grep -c '^\[\[allowlists\]\]$' "$rules" || true)
[ "$count" = 1 ] || fail "$rules has $count [[allowlists]] lines, not one: GitLab's rules are not laid out as this was written for - read them, and this script"
wanted=$(tr -d '\r' < "$table" | sed -n 's/^# replaces sha256:\([0-9a-f]\{64\}\)$/\1/p')
[ "$(printf '%s\n' "$wanted" | grep -c .)" = 1 ] || fail "$table has no one \"# replaces sha256:<64 hex>\" line"
found=$(sed -n '/^\[\[allowlists\]\]$/,$p' "$rules" | sha256sum | cut -d' ' -f1)
if [ "$found" != "$wanted" ]; then
  fail "GitLab's [[allowlists]] table in $rules hashes to $found, and $table replaces $wanted: the image's rules leave out another set of paths. Read GitLab's table, bring $table up to date with it - GitLab's entries but the build, bin, obj, target and packages ones - and its replaces line to $found"
fi
sed '/^\[\[allowlists\]\]$/,$d' "$rules" > "$out.part"
tr -d '\r' < "$table" >> "$out.part"
mv "$out.part" "$out"
echo "secret-detection-rules: GitLab's rules with the project's allowlist ($table) in $out"
