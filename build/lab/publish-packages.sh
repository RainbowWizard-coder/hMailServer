#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Publishes the Linux packages - .deb and .rpm - to the package registries of the
# project's own forge, Forgejo on linix (http://192.168.11.154:3000), where the
# package-install tests can take them with apt and dnf as a user would. Forgejo has a
# Debian and an RPM registry built in; nothing is installed for them.
#
#   bash build/lab/publish-packages.sh --token-file <file> [options] <package>...
#
#   --token-file FILE     a Forgejo access token with the write:package scope, alone
#                         in the file (mode 600). Required. Read once, never printed,
#                         never on a command line: curl takes it from a header file.
#   --owner NAME          the registry's owner, the organisation (Progressiverobot)
#   --distribution NAME   the Debian distribution the .deb goes under (stable)
#   --component NAME      its component (main)
#   --group NAME          the RPM group (hmailserver)
#   --replace             delete a version already there before uploading it again
#                         (Forgejo refuses the same version twice, with 409)
#   --dry-run             say what would be sent, send nothing
#
# docs/TestLab.md, "Packages on the forge", has what a test machine then runs.
set -uo pipefail

FORGEJO="${HMLAB_FORGEJO_URL:-http://192.168.11.154:3000}"
OWNER=Progressiverobot
DIST=stable
COMPONENT=main
GROUP=hmailserver
TOKEN_FILE=""
REPLACE=""
DRY=""
packages=()

while [ $# -gt 0 ]; do
  case "$1" in
    --token-file) TOKEN_FILE="$2"; shift 2 ;;
    --owner) OWNER="$2"; shift 2 ;;
    --distribution) DIST="$2"; shift 2 ;;
    --component) COMPONENT="$2"; shift 2 ;;
    --group) GROUP="$2"; shift 2 ;;
    --replace) REPLACE=yes; shift ;;
    --dry-run) DRY=yes; shift ;;
    -*) echo "unknown option $1"; exit 64 ;;
    *) packages+=("$1"); shift ;;
  esac
done
[ ${#packages[@]} -gt 0 ] || { sed -n '6,25p' "$0"; exit 64; }
[ -n "$TOKEN_FILE" ] || { echo "--token-file is required"; exit 64; }
[ -r "$TOKEN_FILE" ] || { echo "cannot read $TOKEN_FILE"; exit 64; }

WORK=$(mktemp -d)
chmod 700 "$WORK"
trap 'rm -rf "$WORK"' EXIT
# printf is the shell's own: the token is never a process's argument.
( umask 077; printf 'Authorization: token %s\n' "$(tr -d ' \r\n' < "$TOKEN_FILE")" > "$WORK/auth" )

send() {
  local method="$1" url="$2" file="${3:-}"
  if [ -n "$DRY" ]; then
    echo "   would: curl -H @<token header> -X $method ${file:+--upload-file $file }$url" >&2
    return 0
  fi
  if [ -n "$file" ]; then
    curl -s -o "$WORK/answer" -w '%{http_code}' -H @"$WORK/auth" -X "$method" --upload-file "$file" "$url"
  else
    curl -s -o "$WORK/answer" -w '%{http_code}' -H @"$WORK/auth" -X "$method" "$url"
  fi
}

failed=0
for package in "${packages[@]}"; do
  [ -f "$package" ] || { echo "no file $package"; failed=$((failed + 1)); continue; }
  case "$package" in
    *.deb)
      name=$(dpkg-deb -f "$package" Package) version=$(dpkg-deb -f "$package" Version) arch=$(dpkg-deb -f "$package" Architecture)
      echo "== $package: $name $version $arch -> debian $DIST/$COMPONENT"
      if [ -n "$REPLACE" ]; then
        send DELETE "$FORGEJO/api/packages/$OWNER/debian/pool/$DIST/$COMPONENT/$name/$version/$arch" > /dev/null
      fi
      code=$(send PUT "$FORGEJO/api/packages/$OWNER/debian/pool/$DIST/$COMPONENT/upload" "$package")
      ;;
    *.rpm)
      echo "== $package -> rpm group $GROUP"
      if [ -n "$REPLACE" ]; then
        name=$(rpm -qp --qf '%{NAME}' "$package" 2> /dev/null) version=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$package" 2> /dev/null) arch=$(rpm -qp --qf '%{ARCH}' "$package" 2> /dev/null)
        [ -n "$name" ] && send DELETE "$FORGEJO/api/packages/$OWNER/rpm/$GROUP/package/$name/$version/$arch" > /dev/null
      fi
      code=$(send PUT "$FORGEJO/api/packages/$OWNER/rpm/$GROUP/upload" "$package")
      ;;
    *) echo "$package is neither a .deb nor an .rpm"; failed=$((failed + 1)); continue ;;
  esac
  [ -n "$DRY" ] && continue
  case "$code" in
    201) echo "   published (201)" ;;
    409) echo "   that version is already there (409); --replace sends it again"; failed=$((failed + 1)) ;;
    401|403) echo "   refused ($code): the token is not one with write:package for $OWNER"; failed=$((failed + 1)) ;;
    *) echo "   failed ($code): $(head -c 300 "$WORK/answer")"; failed=$((failed + 1)) ;;
  esac
done

echo ""
echo "apt:  $FORGEJO/api/packages/$OWNER/debian  $DIST $COMPONENT  (key: $FORGEJO/api/packages/$OWNER/debian/repository.key)"
echo "dnf:  $FORGEJO/api/packages/$OWNER/rpm/$GROUP.repo"
exit $((failed > 0))
