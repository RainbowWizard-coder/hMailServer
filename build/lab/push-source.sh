#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Puts one commit of this repository on linix for the test lab, from a checkout on
# the machine you work on: ~/hmlab/src/<first twelve of the commit>, made by git
# archive (the committed tree, line endings as .gitattributes gives them), with the
# .commit file build/lab/lab.sh reads to know which commit it is running.
#
#   bash build/lab/push-source.sh [<commit>]        HEAD when none is named
#
# Then on linix: bash ~/hmlab/src/<commit>/build/lab/lab.sh build, and the rest.
# HMLAB_SSH (chris@192.168.11.154) and HMLAB_SSH_KEY (~/.ssh/hmail_ad) say where.
set -euo pipefail

LAB="${HMLAB_SSH:-chris@192.168.11.154}"
KEY="${HMLAB_SSH_KEY:-$HOME/.ssh/hmail_ad}"
sha=$(git rev-parse --verify "${1:-HEAD}^{commit}")
short=$(printf '%s' "$sha" | cut -c1-12)

git archive --format=tar "$sha" | gzip -1 | ssh -i "$KEY" -o BatchMode=yes "$LAB" \
  "set -e; d=\$HOME/hmlab/src/$short; rm -rf \"\$d\"; mkdir -p \"\$d\"; tar -xzf - -C \"\$d\"; echo $sha > \"\$d/.commit\"; du -sh \"\$d\""
echo "on linix: bash ~/hmlab/src/$short/build/lab/lab.sh build"
