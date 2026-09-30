# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The NixOS test's question to the running server (nix/test.nix), the same the
# Debian package's autopkgtest asks
# (platform/packaging/debian-src/debian/tests/server-starts): does SMTP greet with
# 220 and offer STARTTLS, which the first start's secure defaults turn on
# (docs/SecureDefaults.md), and does IMAP greet with * OK. bash's /dev/tcp, so the
# machine needs nothing but bash. Run by the test as hmailserver-probe, which
# pkgs.writeShellScriptBin gives the shebang.
# shellcheck shell=bash

set -euo pipefail

smtp="${1:-25}"
imap="${2:-143}"

fail() {
   echo "FAILED: $*" >&2
   exit 1
}

# Reads one reply line from descriptor 3, without its carriage return.
reply() {
   local line
   IFS= read -r -t 15 line <&3 || return 1
   printf '%s\n' "${line%$'\r'}"
}

echo "== SMTP on $smtp"
exec 3<> "/dev/tcp/127.0.0.1/$smtp"
greeting=$(reply) || fail "no greeting on port $smtp"
echo "S: $greeting"
[[ $greeting == 220\ * ]] || fail "the greeting is not 220: $greeting"
printf 'EHLO nixos-test.invalid\r\n' >&3
extensions=""
while line=$(reply); do
   echo "S: $line"
   extensions="$extensions $line"
   [[ $line == 250\ * ]] && break
   [[ $line == 250-* ]] || fail "EHLO answered: $line"
done
[[ $extensions == *STARTTLS* ]] || fail "EHLO does not offer STARTTLS, which the first start's defaults turn on"
printf 'QUIT\r\n' >&3
bye=$(reply) || fail "no answer to QUIT"
echo "S: $bye"
[[ $bye == 221* ]] || fail "QUIT answered: $bye"
exec 3>&-

echo "== IMAP on $imap"
exec 3<> "/dev/tcp/127.0.0.1/$imap"
greeting=$(reply) || fail "no IMAP greeting"
echo "S: $greeting"
[[ $greeting == "* OK"* ]] || fail "the IMAP greeting is not * OK: $greeting"
printf 'a1 LOGOUT\r\n' >&3
exec 3>&-

echo "PASSED: SMTP (with STARTTLS) and IMAP answered."
