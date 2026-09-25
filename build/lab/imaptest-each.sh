#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# imaptest's scripted tests, one script at a time, inside Dovecot's imaptest image
# (lab.sh imaptest). One at a time because the program reads a whole directory of
# scripts before it runs any, and refuses the lot when it cannot read one: the image
# on Docker Hub is built from a newer tree than the scripts at the last Codeberg
# commit, and its parser no longer reads a few of them. Run alone, a script it
# cannot read is reported as such and the others still run.
#
# Reads HOST, PORT, USER1, USER2 and IMAPTEST_PASS; the scripts are in /tests, one
# log per script goes to /out, and one line per script to standard output:
#   <script> ok|FAILED|skipped|unreadable|timeout  <what imaptest said>
for t in /tests/*; do
  name=$(basename "$t")
  # The .mbox files are the scripts' messages; broken/ is a directory of scripts
  # imaptest's own authors keep aside.
  case "$name" in *.mbox) continue ;; esac
  [ -f "$t" ] || continue
  d="/tmp/one/$name"
  mkdir -p "$d"
  cp "$t" "$d/"
  cp /tests/*.mbox "$d/" 2> /dev/null
  timeout 60 /usr/local/bin/imaptest host="$HOST" port="$PORT" user="$USER1" user2="$USER2" \
    pass="$IMAPTEST_PASS" test="$d" > "/out/$name.log" 2>&1
  code=$?
  groups=$(grep -m1 "test groups:" "/out/$name.log")
  base=$(grep -m1 "^base protocol:" "/out/$name.log" | sed 's/^base protocol: //; s/ individual commands failed//')
  ext=$(grep -m1 "^extensions:" "/out/$name.log" | sed 's/^extensions: //; s/ individual commands failed//')
  if [ "$code" -eq 124 ]; then
    verdict="timeout"
    said="no answer within 60 s"
  elif grep -q "Failed to read tests" "/out/$name.log"; then
    verdict="unreadable"
    said=$(grep -m1 "^Error:" "/out/$name.log" | sed 's#^Error: /tmp/one/[^ ]* ##')
  elif echo "$groups" | grep -q " 1 failed"; then
    verdict="FAILED"
    said="base $base, extensions $ext"
  elif echo "$groups" | grep -q "1 skipped"; then
    verdict="skipped"
    said="missing capabilities"
  elif [ -n "$groups" ]; then
    verdict="ok"
    said="base $base, extensions $ext"
  else
    verdict="error"
    said=$(tail -n 1 "/out/$name.log")
  fi
  printf '%-34s %-10s %s\n' "$name" "$verdict" "$said"
done
