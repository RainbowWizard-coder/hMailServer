#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The Proxmox VE install script's choice of the release
# (hmailserver/source/Server/platform/packaging/proxmox/install/hmailserver-install.sh,
# newest_release): the release of the highest version among those GitLab's releases
# API lists, not the one published last. The project publishes an older line's release
# after a newer line's (a 6.2 release after 6.3.0; a paired security release whose
# older line's tag comes last), and a choice by date would install that older line on
# a new container, and make 'update' say there is nothing to do on one already on the
# newer line, withholding its fix.
#
# The function is taken from the script as it is - the text from "newest_release() {"
# to the first line that is "}" - and run in this shell under bash, with jq, against
# release lists written here in the shape GitLab's API gives them (newest first by
# released_at, as the script asks for them), its fetch replaced by a copy of the
# list and its fail by a line and exit 1. The script itself is not run: it installs
# packages, as root, on Debian 13.
#
#   bash build/test-proxmox-install.sh [the install script]
#
# Exit status: 0 when every case chooses the release it should, 1 when one does not,
# 2 when the function or jq cannot be found. The checkers job runs it.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="${1:-$here/../hmailserver/source/Server/platform/packaging/proxmox/install/hmailserver-install.sh}"

if ! command -v jq > /dev/null 2>&1; then
  echo "jq is not on PATH: the install script's choice of the release is made with jq." >&2
  exit 2
fi
if [ ! -f "$script" ]; then
  echo "$script is not a file." >&2
  exit 2
fi
function_text="$(tr -d '\r' < "$script" | sed -n '/^newest_release() {$/,/^}$/p')"
if [ -z "$function_text" ] || [ "$(printf '%s\n' "$function_text" | tail -n 1)" != "}" ]; then
  echo "$script has no newest_release() { ... } at the start of a line, ending in a line that is }." >&2
  exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
API=https://gitlab.com/api/v4/projects/86730559
fixture=""
# What it asked for, in a file: the function runs in a subshell of its own.
fetch() {
  printf '%s\n' "$1" > "$work/asked"
  cp -- "$fixture" "$2"
}
fail() {
  printf 'newest_release failed: %s\n' "$*"
  exit 1
}
eval "$function_text"

# A release as the API lists it: the members the function reads, and a few it does not.
release() {
  printf '{"name":"%s","tag_name":"%s","released_at":"%s","upcoming_release":%s,"description":"x"}' "$1" "$1" "$2" "${3:-false}"
}
list() {
  local first=1 item
  printf '['
  for item in "$@"; do
    [ "$first" = 1 ] || printf ','
    printf '%s' "$item"
    first=0
  done
  printf ']\n'
}

failures=0
case_count=0
chooses() {
  local what="$1" expected="$2" got status
  shift 2
  case_count=$((case_count + 1))
  fixture="$work/case-$case_count.json"
  list "$@" > "$fixture"
  rm -f -- "$work/asked"
  got="$( (newest_release) 2>&1)"
  status=$?
  if [ "$status" -ne 0 ] || [ "$got" != "$expected" ]; then
    printf 'FAIL: %s: chose "%s" (exit %s), where it should choose "%s"\n' "$what" "$got" "$status" "$expected"
    failures=$((failures + 1))
  else
    printf 'ok: %s: "%s"\n' "$what" "$got"
  fi
}

chooses "an older line's release published after the newer line's" "v6.3.8" \
  "$(release v6.2.29 2026-11-02T10:00:00Z)" "$(release v6.3.8 2026-11-01T10:00:00Z)" \
  "$(release v6.3.7 2026-10-17T23:52:00Z)" "$(release v6.2.28 2026-09-08T10:00:00Z)"
chooses "a paired security release whose older line's tag comes last" "v6.4.1" \
  "$(release v6.3.10 2027-02-01T12:05:00Z)" "$(release v6.4.1 2027-02-01T12:00:00Z)" \
  "$(release v6.4.0 2027-01-10T12:00:00Z)" "$(release v6.3.9 2027-01-02T12:00:00Z)"
chooses "versions compared as numbers, not as text" "v6.3.10" \
  "$(release v6.3.9 2026-12-02T10:00:00Z)" "$(release v6.3.10 2026-12-01T10:00:00Z)" \
  "$(release v6.3.2 2026-09-20T10:00:00Z)"
chooses "a major version above every minor one" "v7.0.0" \
  "$(release v6.12.4 2028-01-02T10:00:00Z)" "$(release v7.0.0 2027-12-01T10:00:00Z)"
chooses "an upcoming release and releases of other kinds left out" "v6.3.7" \
  "$(release v6.4.0 2026-12-24T00:00:00Z true)" "$(release build-inputs-2 2026-10-20T00:00:00Z)" \
  "$(release v6.4.0-rc1 2026-10-19T00:00:00Z)" "$(release v6.3.7 2026-10-17T23:52:00Z)" \
  "$(release 6.9.9 2026-10-16T00:00:00Z)"
chooses "the newest release alone, published last" "v6.3.8" \
  "$(release v6.3.8 2026-11-01T10:00:00Z)" "$(release v6.3.7 2026-10-17T23:52:00Z)"
chooses "no release whose tag is a version" "" \
  "$(release build-inputs-1 2026-09-11T00:00:00Z)"
chooses "no release at all" ""

asked="$(cat "$work/asked" 2> /dev/null || true)"
case "$asked" in
  "$API/releases?"*) ;;
  *)
    printf 'FAIL: newest_release asked for %s, not the project'"'"'s releases (%s/releases)\n' "${asked:-nothing}" "$API"
    failures=$((failures + 1))
    ;;
esac

if [ "$failures" -ne 0 ]; then
  printf '\n%d of %d cases of the install script'"'"'s choice of the release failed.\n' "$failures" "$case_count"
  exit 1
fi
printf '\nThe install script chooses the release of the highest version in all %d cases.\n' "$case_count"
