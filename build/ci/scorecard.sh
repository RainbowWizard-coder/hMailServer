#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The scorecard job of .gitlab/ci/scorecard.yml: OpenSSF Scorecard run on this
# project, its result kept as scorecard/scorecard.json for the Pages site, where
# build/ci/scorecard-pages.py publishes it with its badge under /scorecard/
# (docs/ContinuousIntegration.md, "OpenSSF Scorecard"; Roadmap3, "OpenSSF Scorecard").
#
#   bash build/ci/scorecard.sh [OUT]        run and write OUT/scorecard.json (scorecard/)
#   bash build/ci/scorecard.sh --self-test  what the script checks, without Scorecard
#
# Dormant until the token exists. Scorecard reads a GitLab project through the API
# with GITLAB_AUTH_TOKEN, which must hold read_api, read_user and read_repository;
# read_user exists only for a personal access token, so the token is the owner's to
# make (a project access token cannot carry it). Without the variable this says
# "not configured" and exits 0, and nothing is published: a weekly pipeline stays
# green until the owner adds it. The variable is protected and masked, scoped to the
# environment "scorecard", which is this job's alone. It is read by Scorecard from
# the environment and never written anywhere by this script: not to the log, not to
# a file of the artifact. Scorecard's own messages go to the job log, which GitLab
# masks the token in, and not into the artifact, which it would not be masked in.
#
# Where Scorecard comes from, and why not elsewhere (the no-GitHub rule): its
# releases, its ghcr.io image and Homebrew's bottles are on GitHub; its gcr.io image
# answers with a billing error (28 September 2026); Debian and Ubuntu do not package
# it. Nixpkgs does, and cache.nixos.org serves the build, signed with
# cache.nixos.org-1, the key the nixos/nix image trusts: nix-store --realise of the
# store path below fetches it and its closure (glibc, tzdata, mailcap, iana-etc)
# from cache.nixos.org alone, checks every path's signature and hash, and the store
# path with its NAR hash (below) pins the exact build as an image digest does -
# Scorecard 5.5.0, commit c395761df6afe1a69e476bc60a013a94bcbc153f, which its own
# "version" reports. Moving it is a new store path and NAR hash, read from
# https://cache.nixos.org/<hash>.narinfo, the path from hydra.nixos.org's job
# nixpkgs/unstable/scorecard.x86_64-linux, never a nixpkgs flake reference (the flake
# registry names GitHub). Measured on 28 September 2026 in that image with every
# GitHub host at 0.0.0.0: the closure realised in 4 s, and an anonymous run over
# this project's two public checks answered.
#
# Scorecard reports the checks it can answer on GitLab: Binary-Artifacts,
# Branch-Protection and CI-Tests supported; Code-Review, Contributors, Fuzzing,
# License, Maintained, Pinned-Dependencies, Packaging, Security-Policy,
# Signed-Releases and Vulnerabilities "validating"; Dangerous-Workflow,
# Dependency-Update-Tool, SAST and Token-Permissions not at all (its README, v5.5.0).
# The score is self-published: api.scorecard.dev takes results only from GitHub's
# action, so neither it nor deps.dev can carry this project's.
set -uo pipefail

STORE_PATH=/nix/store/d3zxf860sbva21lgynwsadsdp4bsh9xv-scorecard-5.5.0
VERSION=v5.5.0
# A store path is input-addressed: it names the build's recipe, and the bytes are
# whatever cache.nixos.org-1 signs for it. So every path of the closure is pinned by
# its NAR hash as well, as cache.nixos.org's narinfo files gave them on 28 September
# 2026 (following each narinfo's References): Nix checks each fetched NAR against its
# narinfo's hash while it substitutes, and closure_problems holds what the store then
# records to these, so the store path and these hashes pin the bytes as an image
# digest does. Each hash twice: Nix's base32, as nix-store --query --hash prints it,
# and the same hash in SRI form. A new version is a new table, read from
# https://cache.nixos.org/<hash>.narinfo for the path and each path it references.
CLOSURE='549nkjs10xhma2f9kx3rxw61kym0abjx-mailcap-2.1.54 sha256:1xbjd7qddfb3r809w1dyp7bhbpa8xvll9n295q1x38fa05v68spz sha256-/2pkdgHKodEDLknYROnuSN0F17m+BZ4AymO51vBpcvU=
9v00p4m9cdzn23gmy5m9d3a6c3gfwhl7-iana-etc-20251215 sha256:09l5qqk4528hji1b91ijihdl6hlrlyqz8szya4sp2hldvy65y12i sha256-UQRfjN+NQnE1Uf5r9LGnmUJDG4wyhrRClBCJQibGhSY=
bv3lx708clpz3m6zbd5742np4dlry0yj-libunistring-1.4.2 sha256:1gz22gfl2lcahqzma75rvxgmk9zcfrvarzywlarwy4hkf1k4iz44 sha256-hPxIZnATEs+zotz/rHZ27KdZX9+5HFU/hopRQd0T4r8=
d3zxf860sbva21lgynwsadsdp4bsh9xv-scorecard-5.5.0 sha256:0brx32y7m1z68clfasdp3rypih6l5r0liy2bh21jmds4q6fvkipj sha256-8sa5ncFEtyqDgEv4SEEu1MB4fR63aeUoQ+aHerwYPS8=
fh8svr4ds87z84iarfnxrlfpjlbmc816-tzdata-2026c sha256:16yz4hiyv18nx0p537kgbmggl6aky903z58rpcpaqxjcrnqnq4zm sha256-9RNssc1MdqwuuxmVP0DyUxn6Xl1vnlEu6BaF7SMk35s=
i3jw341xs88r6wxf1j22rjx1mmd8cfjw-libidn2-2.3.8 sha256:0g2qsgc7dv9zl3i5nc523gicpy0xk2z4r1x4msr0inmsqjpm1ym9 sha256-qfpQr8S62giyrqSHTL6YHfjL4huiMFvioD/tdtjTWDw=
lm3pknxi0ipypy3lxh1wmm8wvvavdwrn-glibc-2.42-84 sha256:06kkplxk31y11zksgcrdjv0hf6ymlmxvlc0i2h08g3zc22is465s sha256-uhiioxDsj4cAFBEwunul1RsHwZYts6fnD8GHMTu9cxo=
m07syxhld8hpprrdmzq565ziia6vlw9l-xgcc-15.3.0-libgcc sha256:1bvhsl1iqnbbgmxgp2hi0j8nj5ip5n6h6m2awanbnhwrl8fazjvh sha256-cMuvHKKZQ7us4kpUA40tNxZpkQQRivt6fWtZHAPVcK8='

# closure_problems: reads "PATH HASH" lines - each path of the realised closure and
# its NAR hash as nix-store --query --hash printed it - on standard input, and prints
# one line for each way they differ from CLOSURE; nothing when they agree.
closure_problems() {
  local path got name pinned base32 sri seen=" "
  while read -r path got; do
    [ -n "$path" ] || continue
    name=${path##*/}
    pinned=""
    while read -r pinned base32 sri; do
      [ "$pinned" = "$name" ] && break
      pinned=""
    done <<< "$CLOSURE"
    if [ -z "$pinned" ]; then
      echo "${name} is in the closure and not pinned"
    elif [ "$got" != "$base32" ] && [ "$got" != "$sri" ]; then
      echo "${name} has the NAR hash '${got}', not the pinned ${base32}"
    fi
    seen="${seen}${name} "
  done
  while read -r pinned base32 sri; do
    case "$seen" in *" ${pinned} "*) ;; *) echo "${pinned}, pinned, is not in the realised closure" ;; esac
  done <<< "$CLOSURE"
}

# check_result FILE REPO: 0 when FILE is what Scorecard's --format=json writes for REPO
# - the date, the repository and its commit, this Scorecard's version and a score
# from -1 to 10, in that order, at the start of the one line it writes - else 1,
# saying what is wrong. Its score is left in $score.
check_result() {
  local file="$1" repo="$2" text re
  score=""
  if [ ! -s "$file" ]; then
    echo "Scorecard wrote no result (${file}): it names why above - an unreachable repository is 'Skipping ... repo unreachable', with exit 0"
    return 1
  fi
  text=$(head -c 4096 "$file")
  re='^\{"date":"[0-9T:Z.-]+","repo":\{"name":"([^"]+)","commit":"[0-9a-f]{40}"\},"scorecard":\{"version":"([^"]+)","commit":"[0-9a-f]{40}"\},"score":(-?[0-9]+(\.[0-9]+)?),"checks":\['
  if ! [[ $text =~ $re ]]; then
    echo "${file} is not what Scorecard ${VERSION}'s --format=json writes (its first 200 bytes: ${text:0:200})"
    return 1
  fi
  if [ "${BASH_REMATCH[1]}" != "$repo" ]; then
    echo "${file} scores ${BASH_REMATCH[1]}, not ${repo}"
    return 1
  fi
  if [ "${BASH_REMATCH[2]}" != "$VERSION" ]; then
    echo "${file} was written by Scorecard ${BASH_REMATCH[2]}, not the pinned ${VERSION}"
    return 1
  fi
  score="${BASH_REMATCH[3]}"
  case "$score" in
    -1|-1.0|[0-9]|[0-9].[0-9]*|10|10.0) ;;
    *) echo "${file} gives the score ${score}, which is not between -1 and 10"; return 1 ;;
  esac
  return 0
}

self_test() {
  local work failures=0 sample repo=gitlab.com/hmailserver/hmailserver
  work=$(mktemp -d)
  expect() {
    if [ "$1" != "$2" ]; then
      echo "FAILED: $3 (got $1, wanted $2)"
      failures=$((failures + 1))
    fi
  }
  # Scorecard 5.5.0's own output for this project's two public checks, anonymously,
  # on 28 September 2026 (shortened to one check; the path is the group's).
  sample='{"date":"2026-09-28T18:23:35Z","repo":{"name":"gitlab.com/hmailserver/hmailserver","commit":"6bc09509df0869ac444f635f3ccf354af6a3e004"},"scorecard":{"version":"v5.5.0","commit":"c395761df6afe1a69e476bc60a013a94bcbc153f"},"score":10.0,"checks":[{"details":null,"score":10,"reason":"license file detected","name":"License","documentation":{"url":"https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/docs/checks.md#license","short":"Determines if the project has defined a license."}}],"metadata":null}'
  printf '%s\n' "$sample" > "$work/good.json"
  check_result "$work/good.json" "$repo" > /dev/null; expect "$?/$score" "0/10.0" "Scorecard's own result is accepted, with its score"
  printf '%s\n' "${sample/\"score\":10.0/\"score\":7.3}" > "$work/seven.json"
  check_result "$work/seven.json" "$repo" > /dev/null; expect "$?/$score" "0/7.3" "a fractional score is read"
  printf '%s\n' "${sample/\"score\":10.0/\"score\":-1}" > "$work/none.json"
  check_result "$work/none.json" "$repo" > /dev/null; expect "$?/$score" "0/-1" "-1, no check answered, is a score"
  : > "$work/empty.json"
  check_result "$work/empty.json" "$repo" > /dev/null; expect "$?" 1 "an empty result (Scorecard's 'Skipping', exit 0) is refused"
  printf '%s\n' "${sample/\"score\":10.0/\"score\":11}" > "$work/eleven.json"
  check_result "$work/eleven.json" "$repo" > /dev/null; expect "$?" 1 "a score above 10 is refused"
  check_result "$work/good.json" "gitlab.com/Progressiverobot/hmailserver" > /dev/null; expect "$?" 1 "another repository's result is refused"
  printf '%s\n' "${sample/v5.5.0/v5.6.0}" > "$work/other.json"
  check_result "$work/other.json" "$repo" > /dev/null; expect "$?" 1 "another Scorecard's result is refused"
  printf '{\n  "date": "2026-09-28T18:23:35Z"\n}\n' > "$work/pretty.json"
  check_result "$work/pretty.json" "$repo" > /dev/null; expect "$?" 1 "JSON in another shape is refused, not guessed at"
  # The pinned closure, as the store would report it, and each way it can differ.
  # Bash alone, no sed: the job's nixos/nix image has none.
  local store said name base32 sri
  store=$(while read -r name base32 _; do printf '/nix/store/%s %s\n' "$name" "$base32"; done <<< "$CLOSURE")
  said=$(printf '%s\n' "$store" | closure_problems)
  expect "${said:-none}" none "the pinned closure at its NAR hashes passes"
  said=$(while read -r name _ sri; do printf '/nix/store/%s %s\n' "$name" "$sri"; done <<< "$CLOSURE" | closure_problems)
  expect "${said:-none}" none "the hashes in SRI form pass too"
  said=$(printf '%s\n' "${store/sha256:0brx32/sha256:1brx32}" | closure_problems)
  case "$said" in *"scorecard-5.5.0 has the NAR hash"*) ;; *) echo "FAILED: another NAR hash for Scorecard is refused (said: $said)"; failures=$((failures + 1)) ;; esac
  said=$(printf '%s\n/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-extra-1.0 sha256:x\n' "$store" | closure_problems)
  case "$said" in *"extra-1.0 is in the closure and not pinned"*) ;; *) echo "FAILED: a path the pin does not name is refused (said: $said)"; failures=$((failures + 1)) ;; esac
  said=$(while read -r name base32 _; do [ "${name#*-glibc-}" = "$name" ] && printf '/nix/store/%s %s\n' "$name" "$base32"; done <<< "$CLOSURE" | closure_problems)
  case "$said" in *"glibc-2.42-84, pinned, is not in the realised closure"*) ;; *) echo "FAILED: a pinned path missing from the closure is refused (said: $said)"; failures=$((failures + 1)) ;; esac
  said=$(printf '' | closure_problems)
  case "$said" in *"scorecard-5.5.0, pinned, is not in"*) ;; *) echo "FAILED: nothing realised is refused (said: $said)"; failures=$((failures + 1)) ;; esac
  said=$(printf '%s\n' "${store/sha256:0brx32y7m1z68clfasdp3rypih6l5r0liy2bh21jmds4q6fvkipj/error: path is not valid}" | closure_problems)
  case "$said" in *"scorecard-5.5.0 has the NAR hash 'error: path"*) ;; *) echo "FAILED: an error in place of a hash is refused (said: $said)"; failures=$((failures + 1)) ;; esac
  # Without the token: "not configured", exit 0, nothing written.
  out=$(env -u GITLAB_AUTH_TOKEN bash "${BASH_SOURCE[0]}" "$work/out" 2>&1); code=$?
  expect "$code" 0 "without GITLAB_AUTH_TOKEN the run exits 0"
  case "$out" in *"not configured"*) ;; *) echo "FAILED: without the token it says 'not configured' (said: $out)"; failures=$((failures + 1)) ;; esac
  if [ -e "$work/out/scorecard.json" ]; then echo "FAILED: without the token nothing is written"; failures=$((failures + 1)); fi
  out=$(GITLAB_AUTH_TOKEN="" bash "${BASH_SOURCE[0]}" "$work/out" 2>&1); code=$?
  expect "$code" 0 "an empty GITLAB_AUTH_TOKEN is not configured either"
  rm -rf "$work"
  echo "self-test: ${failures} failure(s)"
  [ "$failures" -eq 0 ]
}

if [ "${1:-}" = "--self-test" ]; then
  self_test
  exit $?
fi

out="${1:-scorecard}"
repo="${CI_SERVER_HOST:-gitlab.com}/${CI_PROJECT_PATH:-hmailserver/hmailserver}"

if [ -z "${GITLAB_AUTH_TOKEN:-}" ]; then
  echo "Scorecard is not configured: the CI/CD variable GITLAB_AUTH_TOKEN is not set for this pipeline, so ${repo} was"
  echo "not scored and nothing is published. It is the owner's personal access token (read_api, read_user,"
  echo "read_repository), protected, masked and scoped to the environment \"scorecard\": docs/ContinuousIntegration.md,"
  echo "\"OpenSSF Scorecard\"."
  exit 0
fi

if ! command -v nix-store > /dev/null 2>&1; then
  echo "No nix-store here: the job runs in the nixos/nix image (.gitlab/ci/scorecard.yml), which fetches Scorecard."
  exit 2
fi

echo "== Scorecard ${VERSION} from cache.nixos.org: ${STORE_PATH}"
if ! nix-store --realise "$STORE_PATH"; then
  echo "cache.nixos.org did not give ${STORE_PATH} with a signature the image trusts: nothing was run."
  exit 2
fi
problems=$(
  for path in $(nix-store --query --requisites "$STORE_PATH"); do
    printf '%s %s\n' "$path" "$(nix-store --query --hash "$path" 2>&1 | head -n 1)"
  done | closure_problems
)
if [ -n "$problems" ]; then
  echo "$problems"
  echo "The realised closure is not the pinned one (CLOSURE, above): nothing was run."
  exit 2
fi
echo "every path of the closure at its pinned NAR hash"
scorecard="$STORE_PATH/bin/scorecard"
"$scorecard" version 2>&1 | grep -E '^(GitVersion|GitCommit):' || true

rm -rf "$out"
mkdir -p "$out"
echo ""
echo "== scoring ${repo}"
# The result on standard output, to the file; everything Scorecard says, to the log.
"$scorecard" --repo="$repo" --format=json --show-details > "$out/scorecard.json"
code=$?
if [ "$code" -ne 0 ]; then
  echo "Scorecard exited ${code}: no result is published."
  rm -f "$out/scorecard.json"
  exit 1
fi
if ! check_result "$out/scorecard.json" "$repo"; then
  rm -f "$out/scorecard.json"
  exit 1
fi
echo ""
echo "Scorecard ${VERSION} scored ${repo} ${score} / 10; ${out}/scorecard.json is this job's artifact, and the"
echo "pages job's next deployment publishes it under /scorecard/ with its badge."
