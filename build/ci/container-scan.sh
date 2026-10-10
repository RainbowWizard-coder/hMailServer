#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# One platform's pushed release image, by its digest, scanned for known
# vulnerabilities by Trivy: the container_scanning and container_scanning_arm64 jobs
# of .gitlab/ci/container-image.yml, in Docker Hub's aquasec/trivy - the image
# dependency-scan runs, by the same digest (build/check-release-chain.py holds the
# two to each other) - after container-image has pushed both platforms' images.
#
#   sh build/ci/container-scan.sh
#
# Environment:
#   HM_SCAN_ARCH    amd64 or arm64, the job's own variable
#   HM_IMAGE_AMD64, HM_IMAGE_ARM64
#                   each platform's image by its digest, from container-image's
#                   dotenv report (build/ci/container-image-push.sh): the one of
#                   HM_SCAN_ARCH is scanned, and it must be the project's,
#                   registry.gitlab.com/hmailserver/hmailserver@sha256:<digest>
#
# Trivy reads the image's files from the registry, without credentials (the
# project's registry is public) and without a Docker daemon, and runs nothing in it,
# so the platform is no matter to the runner. Its vulnerability database is that
# day's, from mirror.gcr.io or, when that does not answer, public.ecr.aws, as
# dependency-scan takes it (build/ci/dependency-scan.py) - never from GitHub's
# registry, which .no-github blocks in the job. Until 10 October 2026 the jobs ran
# GitLab's Container Scanning analyzer, pinned by a digest GitLab stops serving
# within a day or two of rebuilding the image, which it does daily, and which may
# not be copied (docs/ContinuousIntegration.md, "container_scanning's scanner").
#
# Written to container-scan/, the job's artifact: trivy-<arch>.json, Trivy's own
# report, and gl-container-scanning-report.json, the same findings in GitLab's
# container-scanning report format (Trivy's contrib/gitlab.tpl), the job's
# container_scanning report; the findings are printed as a table. A finding is a
# warning, not a failure: the release is published by the time the image is pushed.
#
# Exit status: 0 the image was scanned, whatever was found; 1 the scan could not be
# made (no database, the image not read); 2 the job is not as this was written for.
set -eu

REPOSITORY=registry.gitlab.com/hmailserver/hmailserver
DATABASES="mirror.gcr.io/aquasec/trivy-db:2 public.ecr.aws/aquasecurity/trivy-db:2"
OUT=container-scan

fail() {
  code=$1
  shift
  echo "container-scan: $*" >&2
  exit "$code"
}

arch=${HM_SCAN_ARCH:-}
case "$arch" in
  amd64) image=${HM_IMAGE_AMD64:-} ;;
  arm64) image=${HM_IMAGE_ARM64:-} ;;
  *) fail 2 "HM_SCAN_ARCH is '$arch', not amd64 or arm64" ;;
esac
digest=${image#"$REPOSITORY@sha256:"}
case "$digest" in
  "$image" | "" | *[!0-9a-f]*) fail 2 "the linux/$arch image is '$image', not $REPOSITORY@sha256:<digest> as container-image's dotenv report names it" ;;
esac
[ "${#digest}" -eq 64 ] || fail 2 "the linux/$arch image's digest, $digest, is not 64 hex digits"
command -v trivy > /dev/null || fail 2 "trivy is not on PATH: this runs in the aquasec/trivy image"

rm -rf "$OUT"
mkdir -p "$OUT"
trivy --version
quiet="--skip-version-check --disable-telemetry --no-progress"
database=""
for repository in $DATABASES; do
  echo "\$ trivy image --download-db-only --db-repository $repository"
  # shellcheck disable=SC2086
  if trivy image --download-db-only --db-repository "$repository" $quiet; then
    database=$repository
    break
  fi
  echo "container-scan: $repository did not answer"
done
[ -n "$database" ] || fail 1 "no vulnerability database from $DATABASES: nothing was scanned"

report="$OUT/trivy-$arch.json"
echo "scanning the linux/$arch image, $image, with the database from $database"
# shellcheck disable=SC2086
trivy image --skip-db-update --scanners vuln --platform "linux/$arch" --format json --output "$report" \
  --timeout 20m $quiet "$image" || fail 1 "Trivy could not scan $image"
[ -s "$report" ] || fail 1 "Trivy wrote no report for $image"
grep -qF "sha256:$digest" "$report" || fail 1 "$report does not name the digest of $image: it is not this image's report"
trivy convert --format template --template @/contrib/gitlab.tpl --output "$OUT/gl-container-scanning-report.json" "$report" \
  || fail 1 "the report could not be put in GitLab's format"
[ -s "$OUT/gl-container-scanning-report.json" ] || fail 1 "no report in GitLab's format was written"
trivy convert --format table --scanners vuln "$report"
counts=""
for severity in CRITICAL HIGH MEDIUM LOW UNKNOWN; do
  counts="$counts $(grep -c "\"Severity\": \"$severity\"" "$report" || true) $severity"
done
echo "container-scan: findings by severity:$counts"
echo "container-scan: the linux/$arch image scanned; the reports are in $OUT/"
