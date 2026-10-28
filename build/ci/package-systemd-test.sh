#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The package-systemd jobs (.gitlab/ci/package-systemd.yml): the package started
# under its own systemd unit, with systemd as PID 1, which no other job does. The
# install jobs run in containers with no systemd; linux-package-install.sh starts the
# server the way the unit would, but not under it, so the unit's sandbox
# (ProtectSystem=strict, PrivateTmp, PrivateDevices, the capability bounding set,
# the syscall filter), its first-start step and the package scripts' systemd
# branches have run on no machine of the project's.
#
#   bash build/ci/package-systemd-test.sh ubuntu26.04   linux-build's .deb
#   bash build/ci/package-systemd-test.sh fedora44      rpm-fedora44-x86_64's .rpm
#
# How: the job runs on linix's privileged runner entry (tag linix-systemd), as root,
# beside a PostgreSQL service. It installs podman from Ubuntu's archive and builds,
# from the distribution's own image pinned by digest, an image with systemd in it;
# runs that with systemd as PID 1 (podman --systemd=always) in the job's network
# namespace; installs the package with apt or dnf, which resolve its dependencies
# from the distribution's archive; configures it as packaging/README.md says;
# starts the unit; asks SMTP, POP3, IMAP and the REST API whether they answer;
# runs `hmailserver-update check`, whose schema probe systemd runs in the unit's own
# sandbox; has the server scan a clean message and EICAR with a fake clamd on a Unix
# socket under /run, started outside the unit and closed to all but a group the
# service's user is in; reloads, stops and starts it again; removes the package with
# the server running; and removes the container and its image.
#
# The inner container is NOT privileged. It is given CAP_SYS_ADMIN, which systemd
# needs to build the unit's mount namespace; without it Ubuntu 26.04's systemd 259
# does not start the unit at all (it fails at the step USER, "Failed to keep
# CAP_SYS_ADMIN"), and where a container forbids only part of the sandbox systemd
# starts the unit without that part and says only that it is ignoring it, which the
# journal check below refuses. /proc/sys and /sys stay read-only in it, as in any
# unprivileged container, and systemd-binfmt and the binfmt_misc mount are
# masked in the image as well: a systemd in a privileged container on linix
# removed every binfmt_misc handler of the host when it stopped (29 September
# 2026), and the arm64 jobs then failed with "exec format error". The job counts
# the host's handlers before and after, through a read-only mount, and fails if
# any went.
#
# The environment it reads:
#   HM_PG_HOST, HM_PG_PASSWORD  the database service (postgres, hmtest), user hmailserver
#   HM_SYSTEMD_UBUNTU_IMAGE,    the distribution images systemd runs in, by digest: the
#   HM_SYSTEMD_FEDORA_IMAGE     defaults below, which Renovate's HM_*_IMAGE manager moves
#   HM_DEB, HM_RPM              the package (linux-build/packages/hmailserver_*_amd64.deb,
#                               packages-rpm/fedora44-x86_64/hmailserver-*.fc44.x86_64.rpm)
#   HM_OUT_DIR                  where the logs go (package-systemd/<target>)
#   HM_SYSTEMD_TEST_DROPIN      a drop-in file for hmailserver.service, installed
#                               before its first start. No job sets it: it is how the
#                               negative controls show that each check fails when
#                               what it checks is not so (hmailserver/docs/
#                               PackagesDeb.md, "Started under systemd").
#   HM_SYSTEMD_TEST_CLAMD_DIR   the directory of the fake clamd's socket, /run/hm-clamd
#                               by default. No job sets it either: a negative control
#                               sets it under /tmp, which the unit's PrivateTmp hides.
#                               A directory directly under / (/run, /tmp) is refused.
set -uo pipefail

# The first of the files a glob names, or nothing.
first_file() {
  local f
  for f in "$@"; do
    [ -f "$f" ] && { echo "$f"; return; }
  done
}

target="${1:-}"
case "$target" in
  ubuntu26.04)
    kind=deb
    image="${HM_SYSTEMD_UBUNTU_IMAGE:-docker.io/library/ubuntu:26.04@sha256:da6fc2be547864451aa253836dd926da33623312df4a9a243e35dc877c378a78}"
    package="${HM_DEB:-$(first_file linux-build/packages/hmailserver_*_amd64.deb)}"
    ;;
  fedora44)
    kind=rpm
    image="${HM_SYSTEMD_FEDORA_IMAGE:-registry.fedoraproject.org/fedora:44@sha256:7ab582427716ae32b2e4aa593cdef8f5708d8ec3ab8e3488e1b5e2da28dd9283}"
    package="${HM_RPM:-$(first_file packages-rpm/fedora44-x86_64/hmailserver-*.fc44.x86_64.rpm)}"
    ;;
  *)
    echo "usage: $0 ubuntu26.04|fedora44"
    exit 64
    ;;
esac

PGH="${HM_PG_HOST:-postgres}"
PGPASSWORD="${HM_PG_PASSWORD:-hmtest}"
REST_PORT=8046
out="${HM_OUT_DIR:-package-systemd/$target}"
ctr="hm-systemd-$target-${CI_JOB_ID:-$$}"
tag="localhost/hm-systemd-$target:${CI_JOB_ID:-$$}"
database="hmsystemd_${target//./_}_${CI_JOB_ID:-$$}"
work=$(mktemp -d /tmp/package-systemd.XXXXXX)
storage=/var/lib/containers
# The fake clamd's socket, in a directory of mode 0710 owned by root and the group
# below, as Fedora's /run/clamd.scan is root's, or clamscan's, and virusgroup's.
clamd_dir="${HM_SYSTEMD_TEST_CLAMD_DIR:-/run/hm-clamd}"
clamd_socket="$clamd_dir/clamd.sock"
clamd_group=hm-clamd
clamd_account=scan@clamd.test
case "$clamd_dir" in
  '' | [!/]* | */ | *..* | *//* | */./* | */. | *[!A-Za-z0-9/._-]*)
    echo "REFUSED: HM_SYSTEMD_TEST_CLAMD_DIR is '$clamd_dir', not an absolute path of letters, digits, '/', '.', '_' and '-' below /."
    exit 2
    ;;
esac
# The step makes the directory 0710, root's and $clamd_group's, with install -d,
# which changes the mode and owners of a directory that is there already: one
# directly under /, such as /run or /tmp, would be closed to everybody else in the
# container. So the directory is one below such a directory, as /run/hm-clamd is.
case "${clamd_dir#/}" in
  */*) ;;
  *)
    echo "REFUSED: HM_SYSTEMD_TEST_CLAMD_DIR is '$clamd_dir', a directory directly under /, which the step would make mode 0710 inside its container; give one below it, as /run/hm-clamd is."
    exit 2
    ;;
esac

if [ -z "$package" ] || [ ! -f "$package" ]; then
  echo "REFUSED: no package for $target (${package:-none found}). The job that builds it left none; its log says why."
  exit 2
fi
if [ "$(id -u)" != 0 ]; then
  echo "REFUSED: this runs as root, in the privileged job container of the linix-systemd runner."
  exit 2
fi
mkdir -p "$out"

step() {
  echo ""
  echo "== $*"
}
in_ctr() {
  podman exec "$ctr" "$@"
}
diagnose() {
  podman container exists "$ctr" 2> /dev/null || return 0
  echo "-- the container's console"
  podman logs "$ctr" 2>&1 | tail -n 30
  echo "-- systemctl status hmailserver.service"
  in_ctr systemctl --no-pager --full status hmailserver.service 2>&1 | tail -n 30
  echo "-- the journal of hmailserver.service"
  in_ctr journalctl --no-pager -u hmailserver.service 2>&1 | tail -n 60
  echo "-- failed units"
  in_ctr systemctl --no-pager --failed 2>&1 | tail -n 20
  in_ctr sh -c 'tail -n 40 /var/log/hmailserver/*.log' 2>&1 | tail -n 60
}
fail() {
  echo "FAILED: $*"
  diagnose
  exit 1
}
# A step that fetches from a distribution's archive or a registry, tried up to three
# times, 15 seconds apart, when its output shows only that it could not reach them,
# and failed at once otherwise: a dependency the archive cannot meet is what the job
# is for. rpm-install-test.sh's retry_fetch, with apt's and podman's words for the
# same failure added (Fedora's mirror list timed out on linix on 2 October 2026).
network_failure='Failed to fetch|Unable to connect|Temporary failure|Could not resolve|Couldn.t resolve|Couldn.t connect|Curl error|Cannot download|Failed to download|Download \(curl\) error|Timeout (was reached|exceeded)|Problem retrieving|Connection (refused|timed out|reset)|connection reset by peer|i/o timeout|TLS handshake timeout'
retry_fetch() {
  local log=$1 try
  shift
  for try in 1 2 3; do
    "$@" > "$log" 2>&1 && return 0
    grep -q -E "$network_failure" "$log" || return 1
    echo "the archive or registry could not be reached, try $try of 3: $(grep -m 1 -E "$network_failure" "$log" | cut -c1-200)"
    [ "$try" = 3 ] || sleep 15
  done
  return 1
}
# The mount that holds a path, read from a mountinfo file: its options (field 6),
# its root within its file system (field 4), its file system's device (field 3,
# major:minor, which tells two tmpfs instances apart) or its type (the field after
# "-"). The longest mount point that is the path or a parent of it wins, and of two
# equal ones the later, which is on top.
mount_of() {
  awk -v p="$2" -v what="$3" '
    {
      mp = $5
      if (p == mp || mp == "/" || index(p, mp "/") == 1) {
        if (length(mp) >= best) {
          best = length(mp)
          for (i = 7; i <= NF && $i != "-"; i++) { }
          options = $6; root = $4; dev = $3; type = $(i + 1)
        }
      }
    }
    END { if (what == "options") print options; else if (what == "root") print root; else if (what == "dev") print dev; else print type }' "$1"
}
# The inode of the socket listening on a TCP port, IPv4 or IPv6, in this network
# namespace, which the container shares (podman run --network host): /proc/net/tcp's
# state 0A is LISTEN, field 2 the local address and port in hex, field 10 the inode.
listen_inode() {
  awk -v port="$(printf '%04X' "$1")" '$4 == "0A" && $2 ~ (":" port "$") { print $10; exit }' /proc/net/tcp /proc/net/tcp6 2> /dev/null
}

# ------------------------------------------------------------ the host's binfmt_misc
# A read-only mount of binfmt_misc in a scratch directory: in the initial user
# namespace, which a privileged container shares, it is the host's one instance.
binfmt_dir="$work/binfmt"
binfmt_handlers() {
  mkdir -p "$binfmt_dir"
  mount -t binfmt_misc -o ro,nosuid,nodev,noexec binfmt_misc "$binfmt_dir" 2> /dev/null || { echo "unreadable"; return; }
  local entry count=0
  for entry in "$binfmt_dir"/*; do
    case "${entry##*/}" in
      register | status | '*') ;;
      *) count=$((count + 1)) ;;
    esac
  done
  echo "$count"
  umount "$binfmt_dir"
}
binfmt_before=$(binfmt_handlers)
[ "$binfmt_before" != unreadable ] || { echo "REFUSED: binfmt_misc cannot be mounted here: this is not the privileged runner."; exit 2; }
echo "the host's binfmt_misc handlers before: $binfmt_before"

cleanup() {
  rc=$?
  set +e
  if podman container exists "$ctr" 2> /dev/null; then
    podman rm -f -t 60 "$ctr" > /dev/null 2>&1
  fi
  podman image exists "$tag" 2> /dev/null && podman rmi -f "$tag" > /dev/null 2>&1
  mountpoint -q "$storage" && umount -l "$storage"
  after=$(binfmt_handlers)
  echo ""
  echo "the host's binfmt_misc handlers after: $after (before: $binfmt_before)"
  if [ "$after" = unreadable ] || [ "$after" -lt "$binfmt_before" ]; then
    echo "FAILED: the host's binfmt_misc handlers went from $binfmt_before to $after while this job ran."
    echo "Every arm64 job on linix fails until they are back: on linix, sudo systemctl restart systemd-binfmt."
    rc=1
  fi
  rm -rf "$work"
  exit "$rc"
}
trap cleanup EXIT

# ------------------------------------------------------------ podman in the job container
step "podman, from Ubuntu's archive"
export DEBIAN_FRONTEND=noninteractive
retry_fetch "$out/outer-apt.log" sh -c "apt-get update -qq && apt-get install -y -qq --no-install-recommends podman ca-certificates curl" \
  || { tail -n 20 "$out/outer-apt.log"; fail "apt-get install podman in the job container"; }
podman --version

# cgroup v2: the job's processes sit in the root of its cgroup namespace, where no
# controller can be handed down while they are there. They move to a child, as
# docker's own dind entrypoint does, and the root delegates every controller. The
# list is read whole each time round, since it changes as it is read, and a
# process forked meanwhile is caught on the next round.
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
  mkdir -p /sys/fs/cgroup/job
  for _ in 1 2 3 4 5; do
    pids=$(cat /sys/fs/cgroup/cgroup.procs)
    [ -n "$pids" ] || break
    for pid in $pids; do
      echo "$pid" > /sys/fs/cgroup/job/cgroup.procs 2> /dev/null || true
    done
  done
  controllers=$(sed -e 's/ / +/g' -e 's/^/+/' /sys/fs/cgroup/cgroup.controllers)
  echo "$controllers" > /sys/fs/cgroup/cgroup.subtree_control \
    || fail "the job's cgroup would not delegate its controllers (processes left in it: $(tr '\n' ' ' < /sys/fs/cgroup/cgroup.procs))"
fi
# podman's overlay storage cannot sit on the job container's own overlay root.
mkdir -p "$storage"
mount -t tmpfs -o size=6g,mode=0700 tmpfs "$storage" || fail "a tmpfs for podman's storage"

# The server reaches the database by the service's address, resolved here.
pg_address=$(getent ahostsv4 "$PGH" | awk 'NR == 1 { print $1 }')
[ -n "$pg_address" ] || fail "the database service $PGH has no address"

# ------------------------------------------------------------ the image: systemd in the distribution's own
step "an image of $image with systemd in it, binfmt left alone"
mkdir -p "$work/image"
# Perl runs the fake clamd: Ubuntu's image has it already (perl-base, with
# IO::Socket::UNIX), and Fedora's has none, so perl-IO comes from its archive.
if [ "$kind" = deb ]; then
  install_systemd="apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends systemd systemd-sysv dbus ca-certificates > /dev/null"
else
  install_systemd="dnf -y -q --setopt=install_weak_deps=False install systemd dbus-broker perl-IO > /dev/null"
fi
cat > "$work/image/Containerfile" << EOF
FROM $image
RUN $install_systemd \\
 && systemctl mask systemd-binfmt.service proc-sys-fs-binfmt_misc.automount proc-sys-fs-binfmt_misc.mount
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
EOF
retry_fetch "$out/image-build.log" podman build --network host -t "$tag" "$work/image" \
  || { tail -n 30 "$out/image-build.log"; fail "podman build"; }

step "systemd as PID 1"
podman run -d --name "$ctr" --hostname package-systemd --systemd=always --network host \
  --cap-add SYS_ADMIN --add-host "$PGH:$pg_address" "$tag" > /dev/null || fail "podman run"
# Until systemd's bus is up, systemctl cannot ask; --wait then waits for the boot.
state=""
for _ in $(seq 1 90); do
  state=$(timeout 180 podman exec "$ctr" systemctl is-system-running --wait 2> /dev/null)
  [ -n "$state" ] && break
  podman container exists "$ctr" && [ "$(podman inspect -f '{{.State.Running}}' "$ctr")" = true ] || break
  sleep 2
done
echo "system state: $state"
[ "$state" = running ] || fail "systemd in the container is '$state', not running"
[ "$(in_ctr cat /proc/1/comm)" = systemd ] || fail "PID 1 in the container is not systemd"
in_ctr systemctl --version | head -n 1
masked=$(in_ctr systemctl is-enabled systemd-binfmt.service 2> /dev/null)
[ "$masked" = masked ] || fail "systemd-binfmt.service is '$masked' in the container, not masked"
in_ctr cat /proc/1/mountinfo > "$out/container-mountinfo.txt"
case ",$(mount_of "$out/container-mountinfo.txt" /proc/sys options)," in
  *,ro,*) ;;
  *) fail "/proc/sys is writable in the container" ;;
esac
echo "systemd-binfmt is masked and /proc/sys is read-only in the container"
# .no-github's entries reach the container through the hosts file podman hands on.
outer=$(getent ahostsv4 github.com | awk 'NR == 1 { print $1 }')
if [ "$outer" = 0.0.0.0 ]; then
  inner=$(in_ctr getent ahostsv4 github.com | awk 'NR == 1 { print $1 }')
  [ "$inner" = 0.0.0.0 ] || fail "github.com is '$inner' in the container, where the job has it at 0.0.0.0"
  echo "github.com is 0.0.0.0 in the container, as in the job"
fi

# ------------------------------------------------------------ install
step "install $(basename "$package") with what the distribution resolves for it"
podman cp "$package" "$ctr:/tmp/$(basename "$package")" || fail "podman cp"
if [ "$kind" = deb ]; then
  retry_fetch "$out/install.log" in_ctr sh -c "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends /tmp/$(basename "$package")" \
    || { tail -n 30 "$out/install.log"; fail "apt-get install"; }
else
  retry_fetch "$out/install.log" in_ctr dnf -y --setopt=install_weak_deps=False install "/tmp/$(basename "$package")" \
    || { tail -n 30 "$out/install.log"; fail "dnf install"; }
fi
in_ctr /usr/bin/hmailserver --version || fail "the installed server does not run"

step "what the package's scripts did with systemd running"
expect_unit() {
  local verb=$1 unit=$2 want=$3 got
  got=$(in_ctr systemctl "$verb" "$unit" 2> /dev/null)
  echo "$unit: $verb = $got"
  [ "$got" = "$want" ] || fail "$unit is '$got' ($verb), not $want"
}
expect_unit is-enabled hmailserver.service enabled
expect_unit is-active hmailserver.service inactive
expect_unit is-enabled hmailserver-update.path enabled
expect_unit is-active hmailserver-update.path active
in_ctr systemd-analyze verify hmailserver.service hmailserver-update.path hmailserver-update.service \
  > "$out/verify.txt" 2>&1 || { cat "$out/verify.txt"; fail "systemd-analyze verify"; }
echo "systemd-analyze verify: the three units are clean"

# ------------------------------------------------------------ configure
step "configure it as packaging/README.md tells an administrator to"
INI=/etc/hmailserver/hMailServer.ini
in_ctr sed -i \
  -e 's|^Type=.*|Type=PostgreSQL|' \
  -e "s|^Server=.*|Server=$PGH|" \
  -e "s|^Database=.*|Database=$database|" \
  -e 's|^Username=.*|Username=hmailserver|' \
  -e "s|^Password=.*|Password=$PGPASSWORD|" \
  -e 's|^Port=.*|Port=5432|' \
  -e 's|^ProtectStoredSecretsWithDPAPI=.*|ProtectStoredSecretsWithDPAPI=0|' \
  -e 's|^WindowsEventLogEnabled=.*|WindowsEventLogEnabled=0|' \
  "$INI" || fail "editing $INI"
podman exec --user hmailserver "$ctr" /usr/bin/hmailserver --check-config > "$out/check-config.txt" 2>&1 \
  || { cat "$out/check-config.txt"; fail "--check-config as hmailserver"; }
podman exec --user hmailserver "$ctr" /usr/bin/hmailserver --create-database > "$out/create-database.txt" 2>&1 \
  || { cat "$out/create-database.txt"; fail "--create-database as hmailserver"; }
in_ctr test -e /var/lib/hmailserver/.new-installation \
  || fail "--create-database left no .new-installation for the unit's first start"
admin_password=$(head -c 30 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 24)
printf '%s\n' "$admin_password" | podman exec -i "$ctr" /usr/bin/hmailserver --set-admin-password > /dev/null \
  || fail "--set-admin-password"
in_ctr /usr/bin/hmailserver --set-setting "RestApiPort=$REST_PORT" | grep -F 'stored in the database' \
  || fail "--set-setting RestApiPort=$REST_PORT"
(umask 077 && printf 'machine 127.0.0.1 login Administrator password %s\n' "$admin_password" > "$work/netrc")

# ------------------------------------------------------------ a fake clamd, outside the unit
# What clamd says on its Unix socket, as clamd-socket-tests' fake and the Linux suite's
# answer: one n-prefixed command a connection, PING answered PONG, VERSION with a
# version line, INSTREAM's chunks, each after its length in four octets, read to the
# zero length that ends them and answered "stream: OK", or "stream: Eicar-Signature
# FOUND" (ClamAV 1.4's name) for a stream carrying the EICAR test string. Each stream
# it is given is kept in the directory its third argument names. It binds the socket
# with a umask that leaves it its owner's alone, then gives it to the group and makes
# it 0660: it is never open to others. EICAR is put together from two halves here and
# in the step that sends it, so that no file of the project carries it whole.
cat > "$work/fake-clamd.pl" << 'PERL'
use strict;
use warnings;
use IO::Socket::UNIX;
use Socket qw(SOCK_STREAM);

my ($path, $group, $record) = @ARGV;
my $gid = getgrnam($group);
defined $gid or die "fake clamd: there is no group $group\n";
unlink $path;
umask 0177;
my $listener = IO::Socket::UNIX->new(Type => SOCK_STREAM(), Local => $path, Listen => 16)
  or die "fake clamd: cannot listen on $path: $!\n";
chown(0, $gid, $path) or die "fake clamd: chown $path: $!\n";
chmod(0660, $path) or die "fake clamd: chmod $path: $!\n";
$| = 1;
print "fake clamd: listening on $path, mode 0660, group $group\n";

my $eicar = 'X5O!P%@AP[4\PZX54(P^)7CC)7}' . '$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*';
my $streams = 0;

sub read_exactly {
  my ($client, $size) = @_;
  my $data = '';
  while (length($data) < $size) {
    my $got = sysread($client, $data, $size - length($data), length($data));
    return undef unless $got;
  }
  return $data;
}

while (1) {
  my $client = $listener->accept or next;
  my $command = '';
  while (length($command) < 64) {
    my $c = read_exactly($client, 1);
    last if !defined $c || $c eq "\n";
    $command .= $c;
  }
  if ($command eq 'nPING') {
    syswrite($client, "PONG\n");
    print "fake clamd: PING answered PONG\n";
  } elsif ($command eq 'nVERSION') {
    syswrite($client, "ClamAV 1.4.3/27800/package-systemd-test\n");
    print "fake clamd: VERSION answered\n";
  } elsif ($command eq 'nINSTREAM') {
    my ($stream, $whole) = ('', 0);
    while (1) {
      my $prefix = read_exactly($client, 4);
      last unless defined $prefix;
      my $size = unpack('N', $prefix);
      if ($size == 0) { $whole = 1; last; }
      last if $size > 1048576;
      my $chunk = read_exactly($client, $size);
      last unless defined $chunk;
      $stream .= $chunk;
    }
    if ($whole) {
      $streams++;
      my $verdict = index($stream, $eicar) >= 0 ? 'stream: Eicar-Signature FOUND' : 'stream: OK';
      if (open(my $out, '>', "$record/stream-$streams")) {
        binmode($out);
        print $out $stream;
        close($out);
      }
      syswrite($client, "$verdict\n");
      print "fake clamd: INSTREAM $streams, " . length($stream) . " bytes, answered $verdict\n";
    } else {
      print "fake clamd: an INSTREAM that ended before its zero length\n";
    }
  } else {
    print "fake clamd: '$command' is no command it knows\n";
  }
  close($client);
}
PERL
step "a fake clamd outside the unit, on $clamd_socket, closed to all but the group $clamd_group"
# The group is the service user's before the unit first starts, as an administrator's
# usermod -aG is followed by a restart: systemd gives the service the user's groups
# from the user database when it starts it. The fake runs as root in a transient
# service of its own (systemd-run), with nothing of hmailserver.service's sandbox.
podman cp "$work/fake-clamd.pl" "$ctr:/usr/local/lib/hm-fake-clamd.pl" || fail "podman cp the fake clamd"
in_ctr groupadd --system "$clamd_group" || fail "groupadd $clamd_group"
in_ctr usermod -a -G "$clamd_group" hmailserver || fail "usermod -a -G $clamd_group hmailserver"
in_ctr install -d -m 0710 -o root -g "$clamd_group" "$clamd_dir" || fail "making $clamd_dir"
in_ctr install -d -m 0700 /run/hm-fake-clamd || fail "making /run/hm-fake-clamd"
in_ctr systemd-run --quiet --unit=hm-fake-clamd --property=Type=exec \
  /usr/bin/perl /usr/local/lib/hm-fake-clamd.pl "$clamd_socket" "$clamd_group" /run/hm-fake-clamd \
  || fail "systemd-run the fake clamd"
for _ in $(seq 1 20); do
  in_ctr test -S "$clamd_socket" && break
  sleep 0.5
done
in_ctr test -S "$clamd_socket" || fail "the fake clamd made no socket at $clamd_socket: $(in_ctr journalctl --no-pager -o cat -u hm-fake-clamd.service | tail -n 3 | tr '\n' ' ')"
clamd_modes=$(in_ctr stat -c '%a %U:%G' "$clamd_dir" "$clamd_socket" | tr '\n' ' ')
echo "$clamd_dir and its socket: $clamd_modes"
[ "$clamd_modes" = "710 root:$clamd_group 660 root:$clamd_group " ] \
  || fail "$clamd_dir and $clamd_socket are '$clamd_modes', not 710 and 660, root's and $clamd_group's"
clamd_gid=$(in_ctr getent group "$clamd_group" | cut -d: -f3)
in_ctr id -nG hmailserver | tr ' ' '\n' | grep -qx "$clamd_group" || fail "hmailserver is not in $clamd_group"
echo "hmailserver is in $clamd_group (gid $clamd_gid); the fake clamd is process $(in_ctr systemctl show -p MainPID --value hm-fake-clamd.service) of hm-fake-clamd.service, as root"

if [ -n "${HM_SYSTEMD_TEST_DROPIN:-}" ]; then
  step "the negative control's drop-in"
  cat "$HM_SYSTEMD_TEST_DROPIN"
  if ! { in_ctr mkdir -p /etc/systemd/system/hmailserver.service.d \
    && podman cp "$HM_SYSTEMD_TEST_DROPIN" "$ctr:/etc/systemd/system/hmailserver.service.d/zz-negative-control.conf" \
    && in_ctr systemctl daemon-reload; }; then
    fail "the negative control's drop-in was not installed"
  fi
fi

# ------------------------------------------------------------ the probes
# One reply of a line protocol, read from descriptor 3: SMTP's multi-line form ends
# at "nnn ", the others at their first line.
reply=""
smtp_reply() {
  local line
  reply=""
  while IFS= read -r -t 20 line <&3; do
    line=${line%$'\r'}
    reply+="$line"$'\n'
    [[ $line =~ ^[0-9]{3}- ]] || break
  done
  printf '%s' "$reply" | sed 's/^/  < /'
}
imap_until_tag() {
  local line
  reply=""
  while IFS= read -r -t 20 line <&3; do
    line=${line%$'\r'}
    reply+="$line"$'\n'
    [[ $line == "$1 "* ]] && break
  done
  printf '%s' "$reply" | sed 's/^/  < /'
}
listens() {
  (exec 3<> "/dev/tcp/127.0.0.1/$1") 2> /dev/null
}
probe_smtp() {
  local port=$1
  exec 3<> "/dev/tcp/127.0.0.1/$port" || fail "nothing listens on $port"
  echo "SMTP $port:"
  smtp_reply
  [[ $reply == "220 "* ]] || fail "$port's greeting is not 220"
  printf 'EHLO package-systemd.test\r\n' >&3
  smtp_reply
  [[ $reply == *$'\n'"250 "* || $reply == "250 "* ]] || fail "$port did not answer EHLO with 250"
  grep -q '^250[- ]STARTTLS$' <<< "$reply" || fail "$port does not offer STARTTLS: the first start did not apply the new installation's defaults"
  printf 'QUIT\r\n' >&3
  smtp_reply
  [[ $reply == "221 "* ]] || fail "$port did not answer QUIT with 221"
  exec 3<&- 3>&-
}
probe_pop3() {
  exec 3<> /dev/tcp/127.0.0.1/110 || fail "nothing listens on 110"
  echo "POP3 110:"
  smtp_reply
  [[ $reply == "+OK"* ]] || fail "110's greeting is not +OK"
  printf 'QUIT\r\n' >&3
  smtp_reply
  exec 3<&- 3>&-
}
probe_imap() {
  exec 3<> /dev/tcp/127.0.0.1/143 || fail "nothing listens on 143"
  echo "IMAP 143:"
  smtp_reply
  [[ $reply == "* OK"* ]] || fail "143's greeting is not * OK"
  printf 'a1 CAPABILITY\r\n' >&3
  imap_until_tag a1
  [[ $reply == *$'\n'"a1 OK"* ]] || fail "143 did not answer CAPABILITY with OK"
  grep -q '^\* CAPABILITY .*STARTTLS' <<< "$reply" || fail "143 does not offer STARTTLS: the first start did not apply the new installation's defaults"
  printf 'a2 LOGOUT\r\n' >&3
  imap_until_tag a2
  exec 3<&- 3>&-
}
rest_answers() {
  curl -fsS --netrc-file "$work/netrc" -o "$work/status.json" "http://127.0.0.1:$REST_PORT/api/v1/status" 2> /dev/null
}
wait_for_rest() {
  local _
  for _ in $(seq 1 90); do
    rest_answers && return 0
    [ "$(in_ctr systemctl is-active hmailserver.service)" = active ] || return 1
    sleep 1
  done
  return 1
}
main_pid() {
  in_ctr systemctl show -p MainPID --value hmailserver.service
}

# ------------------------------------------------------------ start
step "systemctl start hmailserver.service"
in_ctr systemctl start hmailserver.service || fail "systemctl start"
wait_for_rest || fail "the REST API did not answer on $REST_PORT: the unit is '$(in_ctr systemctl is-active hmailserver.service)', and a unit that stays active is given 90 seconds"
expect_unit is-active hmailserver.service active
restarts=$(in_ctr systemctl show -p NRestarts --value hmailserver.service)
[ "$restarts" = 0 ] || fail "the unit restarted $restarts time(s) on its way up"
pid=$(main_pid)
echo "main process $pid"

step "the unit's sandbox, read from the process"
# Where a container forbids part of the sandbox, systemd starts the service without
# it and says so in one line ("ProtectHostname=yes is configured, but UTS namespace
# setup is prohibited (container manager?), ignoring namespace setup").
skipped=$(in_ctr journalctl --no-pager -o cat -u hmailserver.service | grep -E 'hmailserver\.service: .*(ignoring|is configured, but)')
[ -z "$skipped" ] || fail "systemd left part of the unit's sandbox out: $skipped"
echo "systemd set up every part of the sandbox the unit asks for"
in_ctr cat "/proc/$pid/status" > "$work/status"
field() { awk -v k="$1:" '$1 == k { print $2 }' "$work/status"; }
uid=$(field Uid)
hm_uid=$(in_ctr id -u hmailserver)
echo "Uid $uid (hmailserver is $hm_uid), CapEff $(field CapEff), CapBnd $(field CapBnd), CapAmb $(field CapAmb), NoNewPrivs $(field NoNewPrivs), Seccomp $(field Seccomp)"
[ "$uid" = "$hm_uid" ] || fail "the server runs as uid $uid, not hmailserver"
for set in CapEff CapPrm CapBnd CapAmb; do
  [ "$(field "$set")" = 0000000000000400 ] || fail "$set is $(field "$set"), not CAP_NET_BIND_SERVICE alone"
done
[ "$(field NoNewPrivs)" = 1 ] || fail "NoNewPrivs is not set"
[ "$(field Seccomp)" = 2 ] || fail "no seccomp filter (SystemCallFilter) on the server"
[ "$(in_ctr readlink "/proc/$pid/ns/mnt")" != "$(in_ctr readlink /proc/1/ns/mnt)" ] \
  || fail "the server shares PID 1's mount namespace: the unit's sandbox was not built"
in_ctr cat "/proc/$pid/mountinfo" > "$out/server-mountinfo.txt"
server_mount() {
  mount_of "$out/server-mountinfo.txt" "$1" "$2"
}
for path in / /usr /etc /var; do
  case ",$(server_mount "$path" options)," in
    *,ro,*) echo "$path: read-only to the server" ;;
    *) fail "$path is not read-only to the server (ProtectSystem=strict): '$(server_mount "$path" options)'" ;;
  esac
done
for path in /var/lib/hmailserver /var/log/hmailserver; do
  case ",$(server_mount "$path" options)," in
    *,rw,*) echo "$path: writable by the server" ;;
    *) fail "$path is not writable by the server (ReadWritePaths): '$(server_mount "$path" options)'" ;;
  esac
done
case "$(server_mount /tmp root)" in
  *systemd-private-*hmailserver.service*) echo "/tmp: the unit's own (PrivateTmp)" ;;
  *) fail "/tmp is not the unit's private one: '$(server_mount /tmp root)'" ;;
esac
# podman gives the container a tmpfs /dev already, so the type alone would hold
# without PrivateDevices: the server's /dev must be another tmpfs than PID 1's.
server_dev=$(server_mount /dev dev)
system_dev=$(mount_of "$out/container-mountinfo.txt" /dev dev)
if [ "$(server_mount /dev type)" != tmpfs ] || [ -z "$server_dev" ] || [ "$server_dev" = "$system_dev" ]; then
  fail "/dev is not the unit's own tmpfs (PrivateDevices): the server's is $(server_mount /dev type) $server_dev, PID 1's $system_dev"
fi
echo "/dev: the unit's own tmpfs (PrivateDevices), device $server_dev where PID 1's is $system_dev"
in_ctr systemd-analyze security --no-pager hmailserver.service > "$out/security.txt" 2>&1
tail -n 1 "$out/security.txt"

step "SMTP, submission, POP3 and IMAP answer, with the first start's STARTTLS"
probe_smtp 25
probe_smtp 587
probe_pop3
probe_imap

step "the first start's step ran, and is spent"
pre=$(in_ctr systemctl show -p ExecStartPre --value hmailserver.service)
echo "$pre"
grep -q 'code=exited ; status=0' <<< "$pre" || fail "ExecStartPre (--apply-new-install-defaults --if-marked) did not exit 0"
in_ctr test ! -e /var/lib/hmailserver/.new-installation || fail "the first start left .new-installation in place"

step "the REST API and the Control Deck answer"
rest_answers || fail "GET /api/v1/status"
head -c 300 "$work/status.json"
echo ""
code=$(curl -s -o "$work/deck.html" -w '%{http_code}' "http://127.0.0.1:$REST_PORT/")
if [ "$code" != 200 ] || ! grep -q 'Control Deck' "$work/deck.html"; then
  fail "GET / answered $code, not the Control Deck"
fi
echo "GET / is the Control Deck"
code=$(curl -s -o /dev/null -w '%{http_code}' -u "Administrator:not-the-password" "http://127.0.0.1:$REST_PORT/api/v1/status")
[ "$code" = 401 ] || fail "a wrong administrator password was answered $code, not 401"
echo "a wrong password is refused with 401"

step "the live update's check: the database's schema version, asked in the unit's sandbox"
# What a rollback of the live update rests on is the database's schema version,
# which hmailserver-update reads with the server's own binary as the service
# user; with systemd as PID 1 it has systemd run that as a transient service in
# hmailserver.service's own sandbox, each property as systemctl show reports it
# for the unit, and reads nothing where systemd will not. `check` asks exactly as
# an apply does, so this is where it shows that systemd takes every one of those
# properties and runs the probe under them, with nothing of the sandbox left out.
probe_since=$(in_ctr date '+%Y-%m-%d %H:%M:%S')
in_ctr /usr/lib/hmailserver/hmailserver-update check > "$out/update-check.txt" 2>&1
check_status=$?
cat "$out/update-check.txt"
grep -q "^database:    at schema version [0-9]* (this server needs [0-9]*), asked as hmailserver, in hmailserver.service's sandbox" "$out/update-check.txt" \
  || fail "hmailserver-update check did not read the database's schema version in hmailserver.service's sandbox"
[ "$check_status" = 0 ] || fail "hmailserver-update check found something missing (exit $check_status)"
skipped=$(in_ctr journalctl --no-pager -o cat --since "$probe_since" | grep -E 'run-[^ ]*\.service: .*(ignoring|is configured, but)')
[ -z "$skipped" ] || fail "systemd left part of the sandbox out of the schema probe: $skipped"
echo "the probe ran as hmailserver in the unit's sandbox, every part of it set up"

# ------------------------------------------------------------ clamd on its Unix socket
step "clamd on a Unix socket under /run, reached from inside the unit"
clamd_started=$SECONDS
# ClamAV turned on with ClamAVHost the fake clamd's socket, over the REST API as an
# administrator of the Linux server does it (PUT /api/v1/settings/antivirus,
# clamav_enabled and clamav_host), with debug and application logging on for the
# scan's own lines. A clean message must reach the fake through the socket and be
# delivered; EICAR must be handled as the server's default anti-virus action says,
# action delete_email (avaction 0, AntiVirusConfiguration::ActionDelete): deleted,
# and SMTPDeliverer's "Message will be deleted (contained virus ...)" logged. Under
# AVFailAction=0, the default, a clamd the server cannot reach lets the clean message
# through unscanned, so it is what the fake was given that tells: where it was given
# nothing, the step fails with the server's own reason from its error log, the
# process's groups and the unit's RestrictAddressFamilies= and InaccessiblePaths=.
api() {
  local method=$1 path=$2 body=${3:-}
  curl -sS --netrc-file "$work/netrc" -o "$work/api.json" -w '%{http_code}' -X "$method" \
    -H 'Content-Type: application/json' ${body:+--data "$body"} "http://127.0.0.1:$REST_PORT$path" 2> "$work/api.err"
}
# The wrong password the REST step sent leaves the administrator's credential
# throttled for a moment (a 429 with Retry-After, up to ten seconds), until the
# right one is accepted: a 429 is waited out, as a client does.
api_expect() {
  local want=$1 code try
  shift
  for try in 1 2 3 4 5 6; do
    code=$(api "$@")
    [ "$code" = 429 ] || break
    echo "$1 $2 answered 429 (the throttle after the wrong password), try $try of 6: waiting 2 s"
    sleep 2
  done
  [ "$code" = "$want" ] || fail "$1 $2 answered '$code', not $want: $(head -c 300 "$work/api.json") $(head -c 200 "$work/api.err")"
}
json_field() {
  grep -o -E "\"$1\": *(true|false|\"[^\"]*\")" "$work/api.json" | head -n 1 | cut -d: -f2- | sed -e 's/^ *//' -e 's/"//g'
}
server_log() {
  in_ctr sh -c 'cat /var/log/hmailserver/*.log 2> /dev/null' | tr -d '\r'
}
clamd_failure() {
  local why=$1 error groups families hidden note=""
  error=$(server_log | grep -F "ClamAV server at $clamd_socket" | tail -n 1 | cut -c1-500)
  groups=$(in_ctr awk '$1 == "Groups:" { $1 = ""; sub(/^ +/, ""); print }' "/proc/$pid/status")
  families=$(in_ctr systemctl show -p RestrictAddressFamilies --value hmailserver.service)
  hidden=$(in_ctr systemctl show -p InaccessiblePaths --value hmailserver.service)
  case "$clamd_dir" in
    /tmp | /tmp/* | /var/tmp | /var/tmp/*) note="; $clamd_dir is under the unit's private /tmp or /var/tmp (PrivateTmp=yes), so the service sees a directory of its own there, not the fake's" ;;
  esac
  echo "-- the fake clamd's journal"
  in_ctr journalctl --no-pager -o cat -u hm-fake-clamd.service | tail -n 10
  fail "$why: the server's error log says '${error:-nothing of $clamd_socket}'; the server's groups are ${groups:-none} ($clamd_group is $clamd_gid), RestrictAddressFamilies=$families, InaccessiblePaths=${hidden}$note"
}
send_message() {
  local subject=$1 token=$2 body=$3 line
  exec 3<> /dev/tcp/127.0.0.1/25 || fail "nothing listens on 25"
  for line in "" "EHLO package-systemd.test" "MAIL FROM:<$clamd_account>" "RCPT TO:<$clamd_account>" "DATA"; do
    [ -z "$line" ] || printf '%s\r\n' "$line" >&3
    reply=""
    while IFS= read -r -t 20 line <&3; do
      line=${line%$'\r'}
      reply+="$line"$'\n'
      [[ $line =~ ^[0-9]{3}- ]] || break
    done
    [[ $reply == 2* || $reply == 354* ]] || fail "port 25 answered '${reply%%$'\n'*}' on the way to sending '$subject'"
  done
  printf '%s\r\n' "From: <$clamd_account>" "To: <$clamd_account>" "Subject: $subject" \
    "Message-ID: <$token@package-systemd.test>" "" "$body" "$token" "." >&3
  IFS= read -r -t 60 line <&3
  line=${line%$'\r'}
  [[ $line == "250 "* ]] || fail "port 25 answered '$line' to the end of '$subject'"
  echo "SMTP 25 took '$subject': $line"
  printf 'QUIT\r\n' >&3
  exec 3<&- 3>&-
}
stream_with() {
  in_ctr sh -c 'grep -l -F -- "$1" /run/hm-fake-clamd/stream-* 2> /dev/null' sh "$1" | head -n 1
}
delivered_with() {
  in_ctr sh -c 'grep -r -l -F -- "$1" /var/lib/hmailserver/clamd.test 2> /dev/null' sh "$1" | grep -c '\.eml$'
}
api_expect 201 POST /api/v1/domains '{"name":"clamd.test"}'
account_password=$(head -c 30 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 24)
api_expect 201 POST /api/v1/domains/clamd.test/accounts "{\"address\":\"$clamd_account\",\"password\":\"$account_password\"}"
api_expect 200 GET /api/v1/settings/logging
logging_before="{\"enabled\":$(json_field enabled),\"log_application\":$(json_field log_application),\"log_debug\":$(json_field log_debug)}"
api_expect 200 PUT /api/v1/settings/logging '{"enabled":true,"log_application":true,"log_debug":true}'
api_expect 200 GET /api/v1/settings/antivirus
av_action=$(json_field action)
[ "$av_action" = delete_email ] || fail "the anti-virus action is '$av_action', not the default, delete_email"
api_expect 200 PUT /api/v1/settings/antivirus "{\"clamav_enabled\":true,\"clamav_host\":\"$clamd_socket\"}"
api_expect 200 GET /api/v1/settings/antivirus
[ "$(json_field clamav_enabled)" = true ] && [ "$(json_field clamav_host)" = "$clamd_socket" ] \
  || fail "the antivirus settings read back as clamav_enabled $(json_field clamav_enabled), clamav_host '$(json_field clamav_host)'"
echo "over REST: $clamd_account made, logging $logging_before turned to application and debug, clamav_enabled true, clamav_host $clamd_socket, action $av_action (the default)"

clean_token="clean-$(head -c 9 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')"
send_message "clean, through clamd's socket" "$clean_token" "Nothing in here for clamd to find."
for _ in $(seq 1 60); do
  [ "$(delivered_with "$clean_token")" -ge 1 ] && break
  sleep 1
done
[ "$(delivered_with "$clean_token")" -ge 1 ] || clamd_failure "the clean message was not delivered in 60 seconds"
clean_stream=$(stream_with "$clean_token")
[ -n "$clean_stream" ] || clamd_failure "the clean message was delivered without the fake clamd having been given it"
clean_line=""
for _ in $(seq 1 20); do
  clean_line=$(server_log | grep -F 'No virus detected: stream: OK' | tail -n 1)
  [ -n "$clean_line" ] && break
  sleep 0.5
done
[ -n "$clean_line" ] || fail "the server's log has no 'No virus detected: stream: OK' for the clean message"
echo "the fake clamd was given the clean message ($(in_ctr stat -c %s "$clean_stream") bytes, $clean_stream), and it was delivered; the server's log:"
echo "  $(cut -c1-300 <<< "$clean_line")"

eicar_token="eicar-$(head -c 9 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')"
eicar_a='X5O!P%@AP[4\PZX54(P^)7CC)7}'
eicar_b='$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*'
send_message "EICAR, through clamd's socket" "$eicar_token" "$eicar_a$eicar_b"
eicar_line=""
for _ in $(seq 1 60); do
  eicar_line=$(server_log | grep -F 'Message will be deleted (contained virus Eicar-Signature).' | tail -n 1)
  [ -n "$eicar_line" ] && break
  sleep 1
done
[ -n "$eicar_line" ] || clamd_failure "the server logged no deletion of the EICAR message in 60 seconds"
eicar_stream=$(stream_with "$eicar_token")
[ -n "$eicar_stream" ] || clamd_failure "the EICAR message was deleted without the fake clamd having been given it"
[ "$(delivered_with "$eicar_token")" = 0 ] || fail "the EICAR message was delivered, where the action delete_email deletes it"
api_expect 200 GET /api/v1/queue
grep -q -E '"count": *0[,}]' "$work/api.json" || fail "the queue is not empty after the EICAR message: $(head -c 300 "$work/api.json")"
virus_line=""
for _ in $(seq 1 20); do
  virus_line=$(server_log | grep -F 'Virus detected: Eicar-Signature' | tail -n 1)
  [ -n "$virus_line" ] && break
  sleep 0.5
done
[ -n "$virus_line" ] || fail "the server's log has no 'Virus detected: Eicar-Signature' for the EICAR message"
echo "the fake clamd was given the EICAR message ($eicar_stream) and answered FOUND; it was not delivered and the queue is empty; the server's log:"
echo "  $(cut -c1-300 <<< "$virus_line")"
echo "  $(cut -c1-300 <<< "$eicar_line")"

groups=$(in_ctr awk '$1 == "Groups:" { $1 = ""; sub(/^ +/, ""); print }' "/proc/$pid/status")
tr ' ' '\n' <<< "$groups" | grep -qx "$clamd_gid" || fail "the server reached the socket without $clamd_group ($clamd_gid) among its groups ($groups)"
# The mount the socket's directory is on in the server's namespace: ProtectSystem=strict
# has / read-only there (the sandbox step), and /run is writable to the service, not
# for strict, which alone would make it read-only too, but because the unit's
# ProtectKernelTunables=, ProtectControlGroups=, ProtectProc= and ProcSubset= imply
# MountAPIVFS=, whose /run systemd leaves out of strict's read-only remount; a drop-in
# putting those four back to their defaults, or one with ReadOnlyPaths= over the
# directory, shows that a read-only mount does not keep the server from the socket
# either, since connecting writes to the socket and not to the file system it is on.
in_ctr cat "/proc/$pid/mountinfo" > "$out/server-mountinfo-clamd.txt"
clamd_mount=$(mount_of "$out/server-mountinfo-clamd.txt" "$clamd_dir" options)
case ",$clamd_mount," in
  *,ro,*) clamd_mount_is=read-only ;;
  *) clamd_mount_is=writable ;;
esac
echo "the server reached $clamd_socket as hmailserver with $clamd_group among its groups ($groups); $clamd_dir is $clamd_mount_is to it ($clamd_mount), and / read-only (ProtectSystem=strict)"

api_expect 200 PUT /api/v1/settings/antivirus '{"clamav_enabled":false,"clamav_host":"localhost"}'
api_expect 200 PUT /api/v1/settings/logging "$logging_before"
in_ctr systemctl stop hm-fake-clamd.service || fail "systemctl stop hm-fake-clamd"
echo "ClamAV turned off again (clamav_host localhost), logging put back to $logging_before, and the fake clamd stopped: the step took $((SECONDS - clamd_started)) s"

# ------------------------------------------------------------ reload, stop, start
step "systemctl reload: the server reinitialises in the same process"
# The unit's ExecReload sends SIGHUP, and the server's answer is
# Application::Reinitialize, which stops its servers and starts them again: the
# socket listening on 25 is then a new one. A SIGHUP that changed nothing would
# leave the old one, and the same process answering proves no more than that.
listener_before=$(listen_inode 25)
[ -n "$listener_before" ] || fail "no socket listens on 25 before the reload"
in_ctr systemctl reload hmailserver.service || fail "systemctl reload"
listener_after=""
for _ in $(seq 1 60); do
  listener_after=$(listen_inode 25)
  [ -n "$listener_after" ] && [ "$listener_after" != "$listener_before" ] && break
  sleep 1
done
if [ -z "$listener_after" ] || [ "$listener_after" = "$listener_before" ]; then
  fail "the reload did not reinitialise the server: 60 seconds after it the socket listening on 25 is '$listener_after', and was $listener_before"
fi
echo "the socket listening on 25 was inode $listener_before and is $listener_after: the servers were started again"
wait_for_rest || fail "the REST API did not answer again after the reload"
[ "$(main_pid)" = "$pid" ] || fail "the reload replaced the process ($pid became $(main_pid))"
probe_smtp 25

step "systemctl stop: it drains and exits cleanly"
in_ctr systemctl stop hmailserver.service || fail "systemctl stop"
expect_unit is-active hmailserver.service inactive
result=$(in_ctr systemctl show -p Result --value hmailserver.service)
[ "$result" = success ] || fail "the stop's result is '$result', not success"
for port in 25 110 143 587 "$REST_PORT"; do
  listens "$port" && fail "$port still answers after the stop"
done
echo "result success; nothing listens on 25, 110, 143, 587 or $REST_PORT"

step "started again, and the package removed with the server running"
in_ctr systemctl start hmailserver.service || fail "the second systemctl start"
wait_for_rest || fail "the REST API did not answer after the second start"
in_ctr journalctl --no-pager -u hmailserver.service > "$out/journal.txt" 2>&1
if [ "$kind" = deb ]; then
  in_ctr sh -c "DEBIAN_FRONTEND=noninteractive apt-get remove -y -qq hmailserver" > "$out/remove.log" 2>&1 \
    || { tail -n 20 "$out/remove.log"; fail "apt-get remove"; }
else
  in_ctr dnf -y remove hmailserver > "$out/remove.log" 2>&1 \
    || { tail -n 20 "$out/remove.log"; fail "dnf remove"; }
fi
in_ctr test ! -e /usr/bin/hmailserver || fail "/usr/bin/hmailserver is still there"
in_ctr test ! -e /usr/lib/systemd/system/hmailserver.service || fail "the unit file is still there"
state=$(in_ctr systemctl show -p ActiveState --value hmailserver.service)
[ "$state" = inactive ] || fail "the unit is '$state' after the removal, not inactive: the removal did not stop it"
for port in 25 110 143 587 "$REST_PORT"; do
  listens "$port" && fail "$port still answers after the removal"
done
left=$(in_ctr sh -c 'ls /proc/[0-9]*/exe -l 2> /dev/null' | grep -c '/usr/bin/hmailserver' || true)
[ "$left" = 0 ] || fail "$left hmailserver process(es) outlived the removal"
# A negative control's drop-in is this script's, not the package's: it goes, and its
# directory with it only if nothing else is in it, so what the package left still shows.
if [ -n "${HM_SYSTEMD_TEST_DROPIN:-}" ]; then
  in_ctr rm -f /etc/systemd/system/hmailserver.service.d/zz-negative-control.conf
  in_ctr rmdir /etc/systemd/system/hmailserver.service.d 2> /dev/null
fi
links=$(in_ctr sh -c 'ls -d /etc/systemd/system/hmailserver* /etc/systemd/system/*/hmailserver* 2> /dev/null')
[ -z "$links" ] || fail "the removal left systemd links behind: $links"
expect_unit is-active hmailserver-update.path inactive
echo "the package's scripts stopped and disabled both units, and nothing of it is running"

step "the container stops as a system does"
podman stop -t 60 "$ctr" > /dev/null || fail "podman stop"
podman rm "$ctr" > /dev/null
echo ""
echo "PACKAGE-SYSTEMD PASSED: $(basename "$package") on $target, under systemd as PID 1: installed, started under its unit with its sandbox in force, answered on SMTP, submission, POP3, IMAP and REST, scanned mail with clamd on a Unix socket under /run, reloaded, stopped, started and removed."
