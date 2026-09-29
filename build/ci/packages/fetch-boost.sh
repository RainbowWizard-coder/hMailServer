#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# One Boost release tarball, held to its SHA-256, for the package builds that
# build Boost from source: deb-build.sh (Debian 12) and rpm-build.sh (EL9, EL10,
# openSUSE Leap 16 and Tumbleweed).
#
#   bash build/ci/packages/fetch-boost.sh <version> <tar.gz|tar.bz2> <sha256> <destination> [cache directory]
#
# Where it comes from, in this order, and the tarball is taken only when its
# SHA-256 is the one given - from whichever source, the cache included:
#   1. the cache directory, when one is given and it holds the file: the job's
#      GitLab cache, keyed by that SHA-256 (.gitlab/ci/packages-deb.yml and
#      packages-rpm.yml), so a job whose cache holds it downloads nothing;
#   2. boost.org's own archive, archives.boost.io;
#   3. SourceForge's boost project, which carries the same release files
#      (downloads.sourceforge.net/project/boost/boost/<version>/, redirected to
#      a mirror). Checked on 29 September 2026: its boost_1_92_0.tar.gz and
#      boost_1_83_0.tar.bz2 have the SHA-256 the two scripts pin.
# Never GitHub.
#
# Each source is tried with curl's own retries: six attempts with a growing
# pause between them (1, 2, 4, 8, 16 s), any error retried - a refused or timed
# out connection included - a connection given 10 s, and a transfer that falls
# below 10 KB/s for a minute abandoned. On 28 and 29 September 2026 some of
# archives.boost.io's Fastly addresses refused linix's connections for 21 s at a
# time while others answered in 0.1 s, and three openSUSE builds of one
# pipeline failed on the single attempt each made. A tarball fetched is put in
# the cache directory for the next job.
#
# Exit status: 0 with the tarball at <destination>; 3 when no source gave the
# file with that SHA-256, each source's host named on the way; 64 for a wrong
# call.
#
# HM_BOOST_SOURCES   the sources to try instead of the two above, as URL
#                    prefixes the file name is appended to, separated by
#                    spaces: for a test, or a mirror of one's own. The SHA-256
#                    check applies to every one of them.
set -uo pipefail

version="${1:-}"
extension="${2:-}"
sha256="${3:-}"
destination="${4:-}"
cache="${5:-}"

if [ -z "$version" ] || [ -z "$extension" ] || [ -z "$sha256" ] || [ -z "$destination" ]; then
  echo "usage: $0 <version> <tar.gz|tar.bz2> <sha256> <destination> [cache directory]"
  exit 64
fi

file="boost_${version//./_}.$extension"

# Whether a file is the one pinned.
pinned() {
  [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d ' ' -f 1)" = "$sha256" ]
}

# 1. The job's cache.
if [ -n "$cache" ] && [ -f "$cache/$file" ]; then
  if pinned "$cache/$file"; then
    cp "$cache/$file" "$destination" || exit 3
    echo "$file: from the job's cache ($cache), SHA-256 $sha256"
    exit 0
  fi
  echo "$file in the job's cache ($cache) is not the one pinned: removed, and fetched again"
  rm -f "$cache/$file"
fi

# 2 and 3. The archives.
if [ -n "${HM_BOOST_SOURCES:-}" ]; then
  read -r -a prefixes <<< "$HM_BOOST_SOURCES"
else
  prefixes=("https://archives.boost.io/release/$version/source"
            "https://downloads.sourceforge.net/project/boost/boost/$version")
fi

tried=()
for prefix in "${prefixes[@]}"; do
  url="$prefix/$file"
  host=${url#*://}
  host=${host%%/*}
  tried+=("$host")
  rm -f "$destination.part"

  started=$(date +%s)
  if ! curl -fsSL --retry 5 --retry-all-errors --retry-max-time 300 --connect-timeout 10 \
            --speed-limit 10240 --speed-time 60 -o "$destination.part" "$url"; then
    echo "$file did not download from $host ($url): six attempts in $(( $(date +%s) - started )) s. Trying the next source."
    continue
  fi

  if ! pinned "$destination.part"; then
    echo "$file from $host is not the one pinned: its SHA-256 is $(sha256sum "$destination.part" | cut -d ' ' -f 1), not $sha256. Trying the next source."
    continue
  fi

  mv -f "$destination.part" "$destination" || exit 3
  echo "$file: from $host in $(( $(date +%s) - started )) s, SHA-256 $sha256"

  # Kept for the next job. A copy made beside it and renamed, so that a cache
  # the job is stopped while writing never holds half a tarball under its name
  # (and one that did would be refused above, and fetched again).
  if [ -n "$cache" ]; then
    if mkdir -p "$cache" && cp "$destination" "$cache/$file.part" && mv -f "$cache/$file.part" "$cache/$file"; then
      echo "kept in the job's cache: $cache/$file"
    else
      rm -f "$cache/$file.part"
      echo "not kept in the job's cache ($cache): it could not be written"
    fi
  fi
  exit 0
done

rm -f "$destination.part"
echo "FAILED: $file with SHA-256 $sha256 came from none of: ${tried[*]}"
exit 3
