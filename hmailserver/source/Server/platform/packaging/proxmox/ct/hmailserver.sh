#!/usr/bin/env bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Progressive Robot's fork of hMailServer in a Proxmox VE container: the half of the
# Proxmox VE scripts that runs on the Proxmox VE host, as root (hmailserver/docs/Proxmox.md).
# It is not the original hMailServer's, and not a script of the Proxmox VE Helper-Scripts
# project, though it is laid out as theirs are: ct/<app>.sh makes the container and
# install/<app>-install.sh installs the application inside it.
#
#   bash -c "$(curl -fsSL https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/source/Server/platform/packaging/proxmox/ct/hmailserver.sh)"
#
# What it does: an unprivileged Debian 13 container from Proxmox VE's own template
# (pveam, which checks the template's checksum from Proxmox's signed index), with the
# resources asked; install/hmailserver-install.sh fetched from the project's GitLab
# repository at the same ref as this file, copied into the container as
# /usr/local/sbin/hmailserver-lxc and run there. Nothing is fetched from GitHub.
#
# Run inside a container it made, it updates the server instead (the 'update' it
# leaves in /usr/bin), as the Helper-Scripts' ct scripts do.
#
# Every answer is a variable, so it runs unattended; asked at a terminal, it shows
# what it will make and waits for a yes (var_yes=yes skips the question):
#   var_ctid          the container's ID; default the next free one (pvesh get /cluster/nextid)
#   var_hostname      hmailserver
#   var_cpu, var_ram, var_swap, var_disk
#                     2 cores, 2048 MiB, 512 MiB of swap, 16 GiB
#   var_storage       the storage for the container's disk; default the first active one
#                     that takes containers (pvesm status -content rootdir)
#   var_template_storage
#                     the storage for the template; default the first active one that
#                     takes templates (pvesm status -content vztmpl)
#   var_brg           vmbr0
#   var_net           dhcp, or an address with its prefix (192.0.2.10/24)
#   var_gateway       the gateway, with a static var_net
#   var_vlan, var_mtu, var_ns (a DNS server), var_ipv6 (none, auto, dhcp or an address)
#   var_ssh_keys      a file of SSH public keys for root in the container
#   var_nesting       1: hmailserver.service is sandboxed by systemd (ProtectSystem=strict,
#                     PrivateTmp, ProtectProc and the rest), and nesting "is also required
#                     by systemd to isolate services" (pct(1)); it also exposes the host's
#                     procfs and sysfs contents to the container. 0 makes the container
#                     without it, and the service may then not start
#   var_onboot        1: start the container with the host
#   var_scripts_ref   the ref of the project's repository the install script is taken
#                     from: master (default), or a release's tag, v<version>
#   var_install_script
#                     a local copy of install/hmailserver-install.sh to use instead
#   var_dry_run       yes: print every command that would change the host, change nothing
#                     (the same as --dry-run: bash ct/hmailserver.sh --dry-run, or with
#                     bash -c, whose first word after the script is the script's $0,
#                     bash -c "$(curl ...)" _ --dry-run; a $0 of --dry-run is taken too)
# and the install script's, passed on to it in a file of mode 0600 (never on a
# command line): var_hm_version, var_db, var_db_host, var_db_port, var_db_name,
# var_db_user, var_db_password, var_rest_port.
set -Eeuo pipefail

readonly APP="hMailServer (Progressive Robot's fork)"
readonly RAW="https://gitlab.com/hmailserver/hmailserver/-/raw"
readonly SCRIPT_PATH="hmailserver/source/Server/platform/packaging/proxmox"
readonly OS_TEMPLATE_PREFIX="debian-13-standard_"

fail() {
  printf '\nFAILED: %s\n' "$*" >&2
  exit 1
}
trap 'fail "line $LINENO: \"$BASH_COMMAND\" exited $?"' ERR
say() { printf '  - %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

var_dry_run="${var_dry_run:-no}"
# bash -c "$(curl ...)" --dry-run hands --dry-run over as $0, not $1: it is read here
# too, and any other $0 that begins with -- is refused rather than ignored.
first=()
case "$0" in --*) first=("$0") ;; *) ;; esac
for argument in "${first[@]}" "$@"; do
  case "$argument" in
    --dry-run) var_dry_run=yes ;;
    -h | --help)
      sed -n '/^# Progressive Robot/,/^set -Eeuo/p' "${BASH_SOURCE[0]:-}" 2> /dev/null | sed '$d' | sed 's/^# \{0,1\}//' \
        || printf 'See hmailserver/docs/Proxmox.md\n'
      exit 0
      ;;
    *) fail "unknown argument $argument (--dry-run, --help)" ;;
  esac
done
dry() { [ "$var_dry_run" = yes ]; }

# Every command that changes the host goes through run: printed in a dry run, as
# one line a shell would take back, and run otherwise.
run() {
  if dry; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

var_scripts_ref="${var_scripts_ref:-master}"
printf '%s\n' "$var_scripts_ref" | grep -Eqx '[A-Za-z0-9][A-Za-z0-9._/-]{0,99}' \
  || fail "var_scripts_ref is '$var_scripts_ref', not a branch or tag name"

fetch_install_script() {
  # fetch_install_script FILE: the local copy, or the project's at var_scripts_ref.
  if [ -n "${var_install_script:-}" ]; then
    [ -r "$var_install_script" ] || fail "var_install_script names $var_install_script, which cannot be read"
    cp -- "$var_install_script" "$1"
  else
    curl -fsSL --proto '=https' --tlsv1.2 --retry 3 --retry-delay 5 -o "$1" \
      "$RAW/$var_scripts_ref/$SCRIPT_PATH/install/hmailserver-install.sh" \
      || fail "the install script could not be fetched from $RAW/$var_scripts_ref/$SCRIPT_PATH/install/"
  fi
  head -n 1 "$1" | grep -qx '#!/usr/bin/env bash' || fail "what was fetched as the install script is not one"
}

# The install script's variables in a file of shell assignments, mode 0600.
write_env_file() {
  local name
  (
    umask 077
    for name in var_hm_version var_db var_db_host var_db_port var_db_name var_db_user var_db_password var_rest_port; do
      if [ -n "${!name:-}" ]; then
        printf '%s=%q\n' "$name" "${!name}"
      fi
    done > "$1"
  )
}

on_pve_host() { command -v pveversion > /dev/null 2>&1 && command -v pct > /dev/null 2>&1; }

# ------------------------------------------------------------------ inside the container
if ! on_pve_host && [ -x /usr/local/sbin/hmailserver-lxc ] && [ -f /etc/hmailserver-lxc.conf ]; then
  step "Updating $APP in this container"
  [ "$(id -u)" = 0 ] || fail "run this as root"
  tmp="$(mktemp)"
  fetch_install_script "$tmp"
  if dry; then
    run install -m 0755 "$tmp" /usr/local/sbin/hmailserver-lxc
    run /usr/local/sbin/hmailserver-lxc update
    rm -f -- "$tmp"
    exit 0
  fi
  install -m 0755 "$tmp" /usr/local/sbin/hmailserver-lxc
  rm -f -- "$tmp"
  exec /usr/local/sbin/hmailserver-lxc update
fi

# ------------------------------------------------------------------ on the host
if ! on_pve_host; then
  dry || fail "this is not a Proxmox VE host (no pveversion or pct): run it on the host, as root"
  say "not a Proxmox VE host: a dry run, with the host's answers made up where it has none"
fi
if ! dry; then
  [ "$(id -u)" = 0 ] || fail "run this as root on the Proxmox VE host"
fi

pve_version=""
if command -v pveversion > /dev/null 2>&1; then
  pve_version="$(pveversion | awk -F/ '{ print $2 }' | cut -d- -f1)"
  case "$pve_version" in
    8.* | 9.*) ;;
    *) fail "Proxmox VE $pve_version: this script is for Proxmox VE 8 and 9" ;;
  esac
fi
host_arch="$(dpkg --print-architecture 2> /dev/null || echo amd64)"
[ "$host_arch" = amd64 ] || fail "this script makes amd64 containers only, and has not been tried on any other kind of host; this host is $host_arch"

var_hostname="${var_hostname:-hmailserver}"
var_cpu="${var_cpu:-2}"
var_ram="${var_ram:-2048}"
var_swap="${var_swap:-512}"
var_disk="${var_disk:-16}"
var_brg="${var_brg:-vmbr0}"
var_net="${var_net:-dhcp}"
var_nesting="${var_nesting:-1}"
var_onboot="${var_onboot:-1}"
var_ipv6="${var_ipv6:-none}"

number() { printf '%s\n' "$2" | grep -Eqx '[1-9][0-9]{0,6}' || fail "$1 is '$2', not a whole number"; }
number var_cpu "$var_cpu"
number var_ram "$var_ram"
number var_disk "$var_disk"
printf '%s\n' "$var_swap" | grep -Eqx '[0-9]{1,7}' || fail "var_swap is '$var_swap', not a whole number"
printf '%s\n' "$var_hostname" | grep -Eqx '[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*' \
  || fail "var_hostname is '$var_hostname', not a host name"
case "$var_nesting" in 0 | 1) ;; *) fail "var_nesting is '$var_nesting': 0 or 1" ;; esac
case "$var_onboot" in 0 | 1) ;; *) fail "var_onboot is '$var_onboot': 0 or 1" ;; esac
if [ "${var_unprivileged:-1}" != 1 ]; then
  fail "var_unprivileged is '${var_unprivileged}': this script makes unprivileged containers only"
fi
# Each network value is one of pct's own words or an address, and holds no comma
# that would add a key of its own to --net0.
for pair in "var_brg=$var_brg" "var_net=$var_net" "var_gateway=${var_gateway:-}" "var_vlan=${var_vlan:-}" \
  "var_mtu=${var_mtu:-}" "var_ns=${var_ns:-}" "var_ipv6=$var_ipv6"; do
  case "${pair#*=}" in
    *[!A-Za-z0-9.:/_-]*) fail "${pair%%=*} is '${pair#*=}', which holds a character no such value has" ;;
    *) ;;
  esac
done

# ------------------------------------------------------------------ what the host has
if command -v pvesh > /dev/null 2>&1; then
  if [ -n "${var_ctid:-}" ]; then
    number var_ctid "$var_ctid"
    pvesh get /cluster/nextid --vmid "$var_ctid" > /dev/null 2>&1 || fail "the ID $var_ctid is taken"
    ctid="$var_ctid"
  else
    ctid="$(pvesh get /cluster/nextid)"
  fi
else
  ctid="${var_ctid:-100}"
fi
number "the container's ID" "$ctid"

first_storage() {
  # first_storage CONTENT: the first active storage that takes it.
  pvesm status -content "$1" 2> /dev/null | awk 'NR > 1 && $3 == "active" { print $1; exit }'
}
storage_takes() {
  # storage_takes STORAGE CONTENT
  pvesm status -content "$2" 2> /dev/null | awk -v s="$1" 'NR > 1 && $1 == s && $3 == "active" { found = 1 } END { exit !found }'
}
if command -v pvesm > /dev/null 2>&1; then
  storage="${var_storage:-$(first_storage rootdir)}"
  template_storage="${var_template_storage:-$(first_storage vztmpl)}"
  [ -n "$storage" ] || fail "no active storage takes containers (rootdir)"
  [ -n "$template_storage" ] || fail "no active storage takes templates (vztmpl)"
  storage_takes "$storage" rootdir || fail "the storage $storage is not active or takes no containers"
  storage_takes "$template_storage" vztmpl || fail "the storage $template_storage is not active or takes no templates"
else
  storage="${var_storage:-local-lvm}"
  template_storage="${var_template_storage:-local}"
fi

# The template: the newest Debian 13 standard one already on the template storage,
# used as it is; or, with none there, the newest one Proxmox VE offers, downloaded
# after the question, once pveam update has refreshed the host's list of templates
# (pveam checks a download against the SHA-512 of Proxmox's signed index).
newest_template() {
  awk -v p="$OS_TEMPLATE_PREFIX" '{ for (i = 1; i <= NF; i++) if (index($i, p) == 1 && $i ~ /_amd64\.tar\.(zst|xz|gz)$/) print $i }' \
    | sort -V | tail -n 1
}
offered_template() { pveam available --section system 2> /dev/null | newest_template || true; }
template=""
if command -v pveam > /dev/null 2>&1; then
  template="$(pveam list "$template_storage" 2> /dev/null | sed 's|^[^ ]*vztmpl/||' | newest_template || true)"
  if [ -z "$template" ]; then
    # The host's list as it stands, to show; the one downloaded is chosen again after
    # pveam update.
    template="$(offered_template)"
    need_template=yes
  else
    need_template=no
  fi
else
  template="${OS_TEMPLATE_PREFIX}13.1-2_amd64.tar.zst"
  need_template=yes
fi

net0="name=eth0,bridge=$var_brg,ip=$var_net"
[ -z "${var_gateway:-}" ] || net0="$net0,gw=$var_gateway"
[ -z "${var_vlan:-}" ] || net0="$net0,tag=$var_vlan"
[ -z "${var_mtu:-}" ] || net0="$net0,mtu=$var_mtu"
[ "$var_ipv6" = none ] || net0="$net0,ip6=$var_ipv6"
create=(pct create "$ctid" "$template_storage:vztmpl/$template"
  --hostname "$var_hostname" --cores "$var_cpu" --memory "$var_ram" --swap "$var_swap"
  --rootfs "$storage:$var_disk" --net0 "$net0" --unprivileged 1 --ostype debian
  --onboot "$var_onboot" --tags "hmailserver;mail"
  --description "Progressive Robot's fork of hMailServer (https://gitlab.com/hmailserver/hmailserver), made by its ct/hmailserver.sh")
[ "$var_nesting" = 0 ] || create+=(--features nesting=1)
[ -z "${var_ns:-}" ] || create+=(--nameserver "$var_ns")
if [ -n "${var_ssh_keys:-}" ]; then
  [ -r "$var_ssh_keys" ] || fail "var_ssh_keys names $var_ssh_keys, which cannot be read"
  create+=(--ssh-public-keys "$var_ssh_keys")
fi

step "$APP: a Proxmox VE container"
say "Proxmox VE ${pve_version:-(none here)}; container $ctid, '$var_hostname', unprivileged, nesting $var_nesting"
say "$var_cpu cores, $var_ram MiB of memory and $var_swap MiB of swap, a $var_disk GiB disk on $storage"
if [ "$need_template" = yes ]; then
  say "the template: none on $template_storage yet; the newest ${OS_TEMPLATE_PREFIX}*_amd64 Proxmox VE offers once 'pveam update' has run (now ${template:-none is listed}), downloaded first"
else
  say "the template $template on $template_storage, as it is"
fi
say "the network: $net0"
say "the install script: ${var_install_script:-$RAW/$var_scripts_ref/$SCRIPT_PATH/install/hmailserver-install.sh}"
say "the database: ${var_db:-local}; the release: ${var_hm_version:-the newest}"
if dry; then
  say "a dry run: the commands that would change the host are printed, and none is run"
else
  say "this changes the host: nothing has been changed yet"
fi
if ! dry && [ "${var_yes:-no}" != yes ] && [ -t 0 ]; then
  read -r -p "Create it? [y/N] " answer
  case "$answer" in y | Y | yes | YES) ;; *) fail "not created" ;; esac
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
fetch_install_script "$work/hmailserver-install.sh"
write_env_file "$work/install.env"

step "The container"
if [ "$need_template" = yes ]; then
  run pveam update
  if command -v pveam > /dev/null 2>&1; then
    template="$(offered_template)"
    [ -n "$template" ] || fail "Proxmox VE offers no ${OS_TEMPLATE_PREFIX}*_amd64 template (pveam available --section system)"
    create[3]="$template_storage:vztmpl/$template"
  fi
  run pveam download "$template_storage" "$template"
fi
run "${create[@]}"
run pct start "$ctid"

if ! dry; then
  say "waiting for the container's network"
  started=$SECONDS
  until pct exec "$ctid" -- getent hosts gitlab.com > /dev/null 2>&1; do
    [ "$((SECONDS - started))" -lt 120 ] || fail "container $ctid cannot resolve gitlab.com two minutes after its start: check $var_brg and var_net"
    sleep 3
  done
fi

step "Installing $APP in container $ctid"
run pct push "$ctid" "$work/hmailserver-install.sh" /usr/local/sbin/hmailserver-lxc --perms 0755
run pct push "$ctid" "$work/install.env" /root/.hmailserver-lxc.env --perms 0600
run pct exec "$ctid" -- bash /usr/local/sbin/hmailserver-lxc install --env-file /root/.hmailserver-lxc.env

# 'update' in the container: this script from the same ref, which then updates the server
# with the install script of that ref. A fetch that fails stops it: bash -c of nothing
# would end with status 0 and update nothing.
# The $script is the wrapper's own, read when 'update' runs.
# shellcheck disable=SC2016
printf '#!/bin/sh\n# Made by ct/hmailserver.sh: run inside its container, that script updates the server.\nset -eu\nurl=%s/%s/%s/ct/hmailserver.sh\nscript=$(curl -fsSL --proto =https "$url") || { echo "update: $url could not be fetched" >&2; exit 1; }\nvar_scripts_ref=%s\nexport var_scripts_ref\nexec bash -c "$script"\n' \
  "$RAW" "$var_scripts_ref" "$SCRIPT_PATH" "$var_scripts_ref" > "$work/update"
run pct push "$ctid" "$work/update" /usr/bin/update --perms 0755

step "Done"
if dry; then
  say "a dry run: nothing was changed"
else
  say "container $ctid: $(pct exec "$ctid" -- hostname -I 2> /dev/null || true)"
  say "inside it ('pct enter $ctid'), 'update' moves the server to the newest release"
fi
