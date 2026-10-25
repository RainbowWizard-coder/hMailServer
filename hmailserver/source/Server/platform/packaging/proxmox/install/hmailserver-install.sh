#!/usr/bin/env bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Progressive Robot's fork of hMailServer in a Debian 13 container: the half of
# the Proxmox VE scripts that runs inside the container (hmailserver/docs/Proxmox.md).
# ../ct/hmailserver.sh makes the container on the Proxmox VE host, copies this file
# in as /usr/local/sbin/hmailserver-lxc and runs it; it runs as well on any Debian 13
# machine with systemd, as root.
#
#   hmailserver-lxc install [--env-file FILE]   install the release and start it
#   hmailserver-lxc update  [--env-file FILE]   move an installed server to the release
#
# What install does, in order:
#   1. curl, jq, OpenSSL and cosign from Debian's own archive (cosign 2.5 in Debian 13).
#   2. The release: var_hm_version, or the newest release of the project at
#      https://gitlab.com/hmailserver/hmailserver whose tag is v<major>.<minor>.<patch>
#      (6.3.6 or later: the first with a Debian 13 package signed at the group's path).
#   3. The release's Debian 13 package for this machine's architecture (amd64 or arm64),
#      the release's Linux sums and the Sigstore bundle of each, from the release's
#      package registry, each named by the release's own links. Each bundle is verified
#      with cosign verify-blob as signed by the project's pipeline for that tag
#      (https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>,
#      issuer https://gitlab.com), the project's id, 86730559, read from each signing
#      certificate's extension with OpenSSL (SECURITY.md, "Verifying a release"), and the
#      package's SHA-256 held to its line in the signed sums. Nothing is installed from a
#      file that fails any of these. Nothing is fetched from GitHub.
#   4. The package, with apt-get, which takes its dependencies from Debian's archive.
#   5. The database: PostgreSQL from Debian's archive in this container (var_db=local,
#      the default), with a role and an empty database of its own and a generated
#      password; or a PostgreSQL server elsewhere (var_db=external) with the role and,
#      unless the role may create one, the database made there first.
#   6. The [Database] section of /etc/hmailserver/hMailServer.ini, the administrator's
#      password (generated, given to hmailserver --set-admin-password on standard input
#      and shown once at the end, never written to a file or a log), the schema
#      (hmailserver --create-database, as the service's user), the stored-secrets key,
#      made and exported to /root/hmailserver-stored-secrets.key (mode 0600), and the
#      REST API and Control Deck on var_rest_port (8443) over TLS with a self-signed
#      certificate made here, since the server serves them off the loopback only with one.
#   7. The service started, and asked: SMTP's 220 on port 25 and STARTTLS in its EHLO,
#      and GET /api/v1/status answered 200 with the administrator's password.
#
# update does 2-4 for a newer release - after a pg_dump of a database in this container
# to /var/backups/hmailserver-lxc - with the service stopped, and then 7 without the
# password, which it does not have: the status answers 401 to a request without one.
#
# The variables, from the environment or from --env-file, which is read as shell
# assignments and then deleted (ct/hmailserver.sh writes it with mode 0600, so that a
# database password is never on a command line):
#   var_hm_version    the release to install or update to (6.3.7); default the newest
#   var_db            local (default) or external
#   var_db_host, var_db_port (5432), var_db_name (hmailserver), var_db_user (hmailserver),
#   var_db_password   the external database; the password may be given in
#   var_db_password_file instead, a file whose first line is the password
#   var_rest_port     the REST API's and the Control Deck's port (8443); 0 leaves it off
#   var_force         yes: update even when the installed release is the same or newer
set -Eeuo pipefail

readonly GITLAB=https://gitlab.com
readonly PROJECT_PATH=hmailserver/hmailserver
readonly PROJECT_ID=86730559
readonly API="$GITLAB/api/v4/projects/$PROJECT_ID"
readonly ISSUER=https://gitlab.com
readonly MINIMUM_VERSION=6.3.6
readonly INI=/etc/hmailserver/hMailServer.ini
readonly STATE=/etc/hmailserver-lxc.conf
readonly SECRETS_KEY_EXPORT=/root/hmailserver-stored-secrets.key
readonly REST_CERTIFICATE=/etc/hmailserver/rest-api.crt
readonly REST_KEY=/etc/hmailserver/rest-api.key
readonly BACKUPS=/var/backups/hmailserver-lxc

work=""
cleanup() {
  if [ -n "$work" ] && [ -d "$work" ]; then
    rm -rf -- "$work"
  fi
}
trap cleanup EXIT

say() { printf '  - %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
fail() {
  printf '\nHMAILSERVER-LXC FAILED: %s\n' "$*" >&2
  exit 1
}
trap 'fail "line $LINENO: \"$BASH_COMMAND\" exited $?"' ERR

usage() {
  sed -n '/^#   hmailserver-lxc install/,/^#   hmailserver-lxc update/p' "$0" | sed 's/^# \{0,1\}//'
  exit 64
}

mode="${1:-install}"
case "$mode" in
  install | update) ;;
  -h | --help | help) usage ;;
  *) printf 'Unknown action %s\n' "$mode" >&2; usage ;;
esac
shift || true
env_file=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --env-file)
      [ "$#" -ge 2 ] || fail "--env-file needs a file"
      env_file="$2"
      shift 2
      ;;
    *) fail "unknown option $1" ;;
  esac
done

[ "$(id -u)" = 0 ] || fail "run this as root"
if [ -n "$env_file" ]; then
  [ -f "$env_file" ] || fail "$env_file is not a file"
  # Written by ct/hmailserver.sh with printf %q, root's alone; read and then deleted,
  # so that a database password it holds stays in no file.
  # shellcheck disable=SC1090
  . "$env_file"
  rm -f -- "$env_file"
fi

var_db="${var_db:-local}"
var_db_port="${var_db_port:-5432}"
var_db_name="${var_db_name:-hmailserver}"
var_db_user="${var_db_user:-hmailserver}"
var_rest_port="${var_rest_port:-8443}"
var_force="${var_force:-no}"

# ------------------------------------------------------------------ the machine
# shellcheck disable=SC1091
. /etc/os-release
if [ "${ID:-}" != debian ] || [ "${VERSION_ID:-}" != 13 ]; then
  fail "this is ${PRETTY_NAME:-an unknown system}; the release's package for this script is Debian 13's"
fi
[ -d /run/systemd/system ] || fail "systemd is not running here; the server runs as its systemd unit"
arch="$(dpkg --print-architecture)"
case "$arch" in
  amd64 | arm64) ;;
  *) fail "the release has no Debian 13 package for $arch (amd64 and arm64 only)" ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt_install() {
  local attempt
  for attempt in 1 2 3; do
    if apt-get install -y -q --no-install-recommends -o Dpkg::Use-Pty=0 "$@"; then
      return 0
    fi
    [ "$attempt" -lt 3 ] || return 1
    sleep 10
    apt-get update -q || true
  done
}

fetch() {
  # fetch URL FILE: over HTTPS only, three tries; what it fetched is trusted only
  # once it has been verified.
  curl -fsSL --proto '=https' --tlsv1.2 --retry 3 --retry-delay 5 --connect-timeout 20 \
    -A hmailserver-lxc -o "$2" "$1"
}

version_ok() {
  printf '%s\n' "$1" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+'
}

# The newest release whose tag is a version: the project has releases of other
# kinds (build inputs), which are never a server's.
newest_release() {
  fetch "$API/releases?per_page=50&order_by=released_at&sort=desc" "$work/releases.json" \
    || fail "the project's releases could not be read from $API/releases"
  jq -r '[.[] | select(.upcoming_release != true) | .tag_name
          | select(test("^v[0-9]+\\.[0-9]+\\.[0-9]+$"))] | first // empty' "$work/releases.json"
}

# The installed package's release, without Debian's revision (6.3.7-1~debian13 is 6.3.7).
installed_version() {
  local v
  v="$(dpkg-query -W -f='${Status} ${Version}\n' hmailserver 2> /dev/null || true)"
  case "$v" in
    "install ok installed "*) v="${v##* }"; printf '%s\n' "${v%%-*}" ;;
    *) ;;
  esac
}

# The project's id in a bundle's signing certificate: Fulcio's source repository
# identifier, 1.3.6.1.4.1.57264.1.15, which OpenSSL prints as the extension's DER
# tag and length (two dots) and the id. cosign checks the identity, a path; the id
# is the project's own, which no other project is given.
repository_id_ok() {
  local bundle="$1" der="$work/certificate.der" line
  jq -er '.verificationMaterial.certificate.rawBytes' "$bundle" | base64 -d > "$der" \
    || fail "$(basename "$bundle") holds no signing certificate"
  line="$(openssl x509 -inform DER -in "$der" -noout -text \
    | grep -A1 -F '1.3.6.1.4.1.57264.1.15:' | sed -n 2p | tr -d '[:space:]')"
  [ "$line" = "..$PROJECT_ID" ] \
    || fail "the certificate of $(basename "$bundle") names the repository id '${line#..}', not $PROJECT_ID"
}

# verified FILE BUNDLE: signed by the project's pipeline for this release's tag.
verified() {
  if ! cosign verify-blob --new-bundle-format --bundle "$2" \
    --certificate-identity "$identity" --certificate-oidc-issuer "$ISSUER" "$1" > "$work/verify.log" 2>&1; then
    cat "$work/verify.log" >&2
    fail "$(basename "$1") does not verify as signed by $identity"
  fi
  repository_id_ok "$2"
  say "$(basename "$1"): signed by $identity, repository id $PROJECT_ID"
}

# Downloads and verifies the release's package; leaves its path in $package.
take_release() {
  local tag="v$version" deb="hmailserver_${version}-1~debian13_${arch}.deb"
  local sums="hmailserver-linux-${version}-SHA256SUMS.txt" registry="$API/packages/generic/hmailserver/$version"
  local name
  identity="$GITLAB/$PROJECT_PATH//.gitlab-ci.yml@refs/tags/$tag"

  step "The release $tag: $deb"
  fetch "$API/releases/$tag" "$work/release.json" || fail "the release $tag could not be read from $API/releases/$tag"
  [ "$(jq -r .tag_name "$work/release.json")" = "$tag" ] || fail "$API/releases/$tag is not the release $tag"
  for name in "$deb" "$deb.sigstore.json" "$sums" "$sums.sigstore.json"; do
    jq -e --arg url "$registry/$name" 'any(.assets.links[]; .url == $url)' "$work/release.json" > /dev/null \
      || fail "the release $tag does not carry $name"
    fetch "$registry/$name" "$work/$name" || fail "$name could not be downloaded from $registry"
    [ -s "$work/$name" ] || fail "$name is empty"
  done

  verified "$work/$sums" "$work/$sums.sigstore.json"
  verified "$work/$deb" "$work/$deb.sigstore.json"
  held_to_sums "$work/$deb" "$work/$sums"
  package="$work/$deb"
}

# held_to_sums FILE SUMS: FILE's SHA-256 is the one line SUMS gives its name.
held_to_sums() {
  local name want have
  name="$(basename "$1")"
  want="$(awk -v n="$name" '$2 == n || $2 == "*" n { print $1 }' "$2")"
  case "$want" in *$'\n'*) fail "the signed sums name $name more than once" ;; *) ;; esac
  printf '%s\n' "$want" | grep -Eqx '[0-9a-f]{64}' || fail "the signed sums have no line for $name"
  have="$(sha256sum "$1" | cut -d' ' -f1)"
  [ "$have" = "$want" ] || fail "$name's SHA-256 is $have; the signed sums say $want"
  say "$name: SHA-256 $have, as the signed sums say"
}

# The [Database] section replaced, the rest of the file kept as it is (the container
# image's entrypoint does the same), the file's owner and mode kept.
ini_value_ok() {
  case "$2" in
    *[[:cntrl:]]*) fail "$1 holds a line break or another control character, which hMailServer.ini cannot hold" ;;
    [[:space:]]* | *[[:space:]]) fail "$1 begins or ends with white space, which hMailServer.ini does not keep" ;;
    \'*\' | \"*\") fail "$1 begins and ends with the same quotation mark, which hMailServer.ini takes off" ;;
    *) ;;
  esac
}
write_database_section() {
  local tmp="$work/ini"
  awk 'BEGIN { skip = 0 } /^\[/ { skip = ($0 == "[Database]") } !skip { print }' "$INI" > "$tmp"
  printf '\n[Database]\nType=PostgreSQL\nServer=%s\nPort=%s\nDatabase=%s\nUsername=%s\nPassword=%s\nPasswordencryption=0\n' \
    "$db_host" "$var_db_port" "$var_db_name" "$var_db_user" "$db_password" >> "$tmp"
  cat "$tmp" > "$INI"
  rm -f -- "$tmp"
}

random_text() {
  # $1 letters and digits from the kernel's generator.
  local text=""
  while [ "${#text}" -lt "$1" ]; do
    text="$text$(openssl rand -base64 48 | tr -dc 'A-Za-z0-9')"
  done
  printf '%s\n' "${text:0:$1}"
}

wait_for_port() {
  local port="$1" seconds="$2" started=$SECONDS
  until (exec 3<> "/dev/tcp/127.0.0.1/$port") 2> /dev/null; do
    [ "$((SECONDS - started))" -lt "$seconds" ] || return 1
    sleep 2
  done
}

# SMTP's greeting and EHLO on 25, then the REST API's status: with the password
# when there is one (200), without it otherwise (401, which only the REST API gives).
ask_server() {
  local line extensions="" code
  systemctl is-active --quiet hmailserver || { journalctl -u hmailserver -n 40 --no-pager >&2 || true; fail "hmailserver.service is not active"; }
  wait_for_port 25 120 || { journalctl -u hmailserver -n 40 --no-pager >&2 || true; fail "nothing answers on port 25 two minutes after the start"; }
  exec 3<> /dev/tcp/127.0.0.1/25
  IFS= read -r -t 15 line <&3 || fail "no greeting on port 25"
  line="${line%$'\r'}"
  case "$line" in 220\ *) say "SMTP on 25: $line" ;; *) fail "port 25 greets with '$line', not 220" ;; esac
  printf 'EHLO hmailserver-lxc.invalid\r\n' >&3
  while IFS= read -r -t 15 line <&3; do
    line="${line%$'\r'}"
    extensions="$extensions $line"
    case "$line" in 250\ *) break ;; 250-*) ;; *) fail "EHLO answered '$line'" ;; esac
  done
  case "$extensions" in *STARTTLS*) say "EHLO offers STARTTLS" ;; *) fail "EHLO does not offer STARTTLS" ;; esac
  printf 'QUIT\r\n' >&3
  exec 3>&-
  if [ "$var_rest_port" = 0 ]; then
    say "the REST API is off (var_rest_port=0)"
    return 0
  fi
  wait_for_port "$var_rest_port" 60 || fail "nothing answers on the REST API's port $var_rest_port"
  if [ -n "${admin_password:-}" ]; then
    # The password through curl's configuration on standard input, not its command line.
    code="$(printf 'user = "Administrator:%s"\n' "$admin_password" \
      | curl -sS -o "$work/status.json" -w '%{http_code}' -K - --cacert "$REST_CERTIFICATE" \
        "https://127.0.0.1:$var_rest_port/api/v1/status")" || code=000
    [ "$code" = 200 ] || fail "GET /api/v1/status answered $code with the administrator's password, not 200"
    say "GET https://127.0.0.1:$var_rest_port/api/v1/status: 200 $(jq -c '{version, state} | with_entries(select(.value != null))' "$work/status.json" 2> /dev/null || true)"
  else
    code="$(curl -sS -o /dev/null -w '%{http_code}' --cacert "$REST_CERTIFICATE" \
      "https://127.0.0.1:$var_rest_port/api/v1/status")" || code=000
    [ "$code" = 401 ] || fail "GET /api/v1/status answered $code without a password, not 401"
    say "GET https://127.0.0.1:$var_rest_port/api/v1/status: 401 without a password, as it should"
  fi
}

work="$(mktemp -d /var/tmp/hmailserver-lxc.XXXXXX)"

step "Tools from Debian's archive"
apt-get update -q || fail "apt-get update"
apt_install ca-certificates curl jq openssl cosign || fail "the tools could not be installed"
cosign verify-blob --help 2>&1 | grep -q -- '--new-bundle-format' \
  || fail "this cosign has no verify-blob --new-bundle-format (cosign 2.4 or later is needed)"
say "cosign $(dpkg-query -W -f='${Version}' cosign), from Debian's archive"

installed="$(installed_version)"
if [ -n "${var_hm_version:-}" ]; then
  version_ok "$var_hm_version" || fail "var_hm_version is '$var_hm_version', not a version such as 6.3.7"
  version="$var_hm_version"
else
  tag="$(newest_release)"
  [ -n "$tag" ] || fail "the project has no release with a version tag"
  version="${tag#v}"
fi
dpkg --compare-versions "$version" ge "$MINIMUM_VERSION" \
  || fail "$version is older than $MINIMUM_VERSION, the first release with a Debian 13 package this script can verify"

if [ "$mode" = install ]; then
  # ------------------------------------------------------------------ install
  [ -z "$installed" ] || fail "hmailserver $installed is installed already: run 'hmailserver-lxc update'"
  case "$var_db" in
    local) db_host=127.0.0.1 ;;
    external)
      db_host="${var_db_host:-}"
      [ -n "$db_host" ] || fail "var_db=external needs var_db_host"
      ;;
    *) fail "var_db is '$var_db': local or external" ;;
  esac
  printf '%s\n' "$var_db_port" | grep -Eqx '[0-9]{1,5}' || fail "var_db_port is '$var_db_port', not a port"
  if ! printf '%s\n' "$var_rest_port" | grep -Eqx '[0-9]{1,5}' || [ "$var_rest_port" -gt 65535 ]; then
    fail "var_rest_port is '$var_rest_port', not a port"
  fi
  # The names go into SQL below as identifiers: letters, digits and underscores alone.
  printf '%s\n' "$var_db_name" | grep -Eqx '[a-z_][a-z0-9_]{0,62}' || fail "var_db_name '$var_db_name': lower-case letters, digits and _"
  printf '%s\n' "$var_db_user" | grep -Eqx '[a-z_][a-z0-9_]{0,62}' || fail "var_db_user '$var_db_user': lower-case letters, digits and _"

  take_release
  step "The package"
  apt_install "$package" || fail "the package could not be installed"
  say "$(hmailserver --version 2>&1 | head -n 1)"

  step "The database"
  if [ "$var_db" = local ]; then
    apt_install postgresql || fail "PostgreSQL could not be installed"
    systemctl enable --now postgresql > /dev/null 2>&1 || true
    started=$SECONDS
    until runuser -u postgres -- pg_isready -q 2> /dev/null; do
      [ "$((SECONDS - started))" -lt 60 ] || fail "PostgreSQL did not accept connections within a minute"
      sleep 2
    done
    db_password="$(random_text 32)"
    # On standard input, not a command line: the password is in no process's arguments.
    runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 > /dev/null << SQL || fail "the database role and database could not be made"
CREATE ROLE $var_db_user LOGIN PASSWORD '$db_password';
CREATE DATABASE $var_db_name OWNER $var_db_user ENCODING 'UTF8' TEMPLATE template0;
SQL
    say "PostgreSQL $(runuser -u postgres -- psql -tAc 'show server_version' | cut -d' ' -f1) in this container: role $var_db_user, database $var_db_name, a generated password"
  else
    if [ -n "${var_db_password_file:-}" ]; then
      [ -r "$var_db_password_file" ] || fail "var_db_password_file names $var_db_password_file, which cannot be read"
      IFS= read -r db_password < "$var_db_password_file" || true
    else
      db_password="${var_db_password:-}"
    fi
    [ -n "$db_password" ] || fail "var_db=external needs var_db_password or var_db_password_file"
    say "PostgreSQL at $db_host:$var_db_port, role $var_db_user, database $var_db_name"
  fi
  for pair in "var_db_host=$db_host" "var_db_password=$db_password"; do
    ini_value_ok "${pair%%=*}" "${pair#*=}"
  done
  write_database_section
  unset pair

  step "The server's configuration"
  admin_password="$(random_text 24)"
  printf '%s\n' "$admin_password" | hmailserver --set-admin-password > /dev/null \
    || fail "hmailserver --set-admin-password"
  say "the administrator's password: set (shown at the end)"
  runuser -u hmailserver -- hmailserver --check-config > "$work/check-config.txt" 2>&1 \
    || { cat "$work/check-config.txt" >&2; fail "hmailserver --check-config"; }
  runuser -u hmailserver -- hmailserver --create-database > "$work/create.txt" 2>&1 \
    || { cat "$work/create.txt" >&2; fail "hmailserver --create-database"; }
  say "$(grep -m1 -i 'schema\|database' "$work/create.txt" || echo 'the schema: created')"
  hmailserver --create-stored-secrets-key > "$work/key.txt" 2>&1 \
    || { cat "$work/key.txt" >&2; fail "hmailserver --create-stored-secrets-key"; }
  hmailserver --export-stored-secrets-key "$SECRETS_KEY_EXPORT" > "$work/export.txt" 2>&1 \
    || { cat "$work/export.txt" >&2; fail "hmailserver --export-stored-secrets-key"; }
  say "the stored-secrets key: made, and exported to $SECRETS_KEY_EXPORT"

  if [ "$var_rest_port" != 0 ]; then
    fqdn="$(hostname -f 2> /dev/null || hostname)"
    short="$(hostname -s 2> /dev/null || hostname)"
    san="DNS:$fqdn"
    [ "$short" = "$fqdn" ] || san="$san,DNS:$short"
    san="$san,DNS:localhost,IP:127.0.0.1"
    for address in $(hostname -I 2> /dev/null || true); do
      san="$san,IP:$address"
    done
    openssl req -x509 -newkey rsa:3072 -nodes -days 825 -sha256 -subj "/CN=$fqdn" \
      -addext "subjectAltName=$san" -addext "basicConstraints=critical,CA:FALSE" \
      -addext "extendedKeyUsage=serverAuth" \
      -keyout "$work/rest.key" -out "$work/rest.crt" > /dev/null 2>&1 || fail "the REST API's certificate could not be made"
    install -o root -g hmailserver -m 0640 "$work/rest.key" "$REST_KEY"
    install -o root -g hmailserver -m 0644 "$work/rest.crt" "$REST_CERTIFICATE"
    hmailserver --set-setting "RestApiCertificateFile=$REST_CERTIFICATE" > /dev/null
    hmailserver --set-setting "RestApiPrivateKeyFile=$REST_KEY" > /dev/null
    hmailserver --set-setting RestApiBindAddress=0.0.0.0 > /dev/null
    hmailserver --set-setting "RestApiPort=$var_rest_port" > /dev/null
    say "the REST API and the Control Deck: port $var_rest_port on every address, over TLS ($REST_CERTIFICATE, $san)"
    say "the certificate's SHA-256 fingerprint: $(openssl x509 -in "$REST_CERTIFICATE" -noout -fingerprint -sha256 | cut -d= -f2)"
  fi

  {
    printf '# Written by hmailserver-lxc install; read by hmailserver-lxc update.\n'
    printf 'db=%s\nrest_port=%s\ninstalled=%s\n' "$var_db" "$var_rest_port" "$version"
  } > "$STATE"

  step "Starting the server"
  systemctl enable hmailserver > /dev/null 2>&1
  systemctl restart hmailserver
  ask_server
else
  # ------------------------------------------------------------------ update
  [ -n "$installed" ] || fail "hmailserver is not installed here: run 'hmailserver-lxc install'"
  if [ -f "$STATE" ]; then
    var_rest_port="$(sed -n 's/^rest_port=//p' "$STATE")"
    state_db="$(sed -n 's/^db=//p' "$STATE")"
  else
    state_db=""
  fi
  var_rest_port="${var_rest_port:-0}"
  say "installed: $installed; the release: $version"
  if dpkg --compare-versions "$installed" ge "$version" && [ "$var_force" != yes ]; then
    say "nothing to do: $installed is $version or newer (var_force=yes installs $version over it)"
    exit 0
  fi
  take_release
  if [ "$state_db" = local ]; then
    step "A copy of the database before the update"
    database="$(awk -F= 'tolower($0) ~ /^\[database\]/ { s = 1; next } /^\[/ { s = 0 } s && tolower($1) == "database" { print $2; exit }' "$INI")"
    install -d -m 0700 "$BACKUPS"
    backup="$BACKUPS/hmailserver-$installed-$(date -u +%Y%m%dT%H%M%SZ).dump"
    runuser -u postgres -- pg_dump -Fc "$database" > "$backup" || fail "pg_dump of $database"
    chmod 0600 "$backup"
    say "$backup ($(du -h "$backup" | cut -f1)); pg_restore puts it back"
  fi
  step "The package"
  systemctl stop hmailserver
  # The package's postinst runs hmailserver --upgrade-database on an upgrade.
  apt_install "$package" || fail "the package could not be installed; the server is stopped, and 'systemctl start hmailserver' starts $installed again"
  say "$(hmailserver --version 2>&1 | head -n 1)"
  if [ -f "$STATE" ]; then
    sed -i "s/^installed=.*/installed=$version/" "$STATE"
  fi
  step "Starting the server"
  systemctl start hmailserver
  admin_password=""
  ask_server
fi

step "Done"
if [ "$mode" = install ]; then
  addresses="$(hostname -I 2> /dev/null | tr ' ' '\n' | grep -v '^$' || true)"
  printf '\nProgressive Robot'"'"'s fork of hMailServer %s is running.\n\n' "$version"
  printf 'The administrator'"'"'s password, shown this once and kept in no file:\n\n    %s\n\n' "$admin_password"
  printf 'Sign in to the Control Deck as Administrator with it:\n'
  if [ "$var_rest_port" != 0 ]; then
    for address in $addresses; do
      printf '    https://%s:%s/\n' "$address" "$var_rest_port"
    done
  fi
  printf '\nCopy %s out of this container and keep it with the database backups.\n' "$SECRETS_KEY_EXPORT"
  printf 'Update later with: hmailserver-lxc update\n'
else
  printf '\nhmailserver %s is running (it was %s).\n' "$version" "$installed"
fi
