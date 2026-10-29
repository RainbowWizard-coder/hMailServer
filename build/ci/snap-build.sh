#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The snap (snap/snapcraft.yaml; hmailserver/docs/Snap.md), built from the Ubuntu
# 24.04 .deb of the same pipeline, in an Ubuntu 24.04 job container of the snap's own
# architecture: the jobs snap-build-amd64 and snap-build-arm64 (.gitlab/ci/snap.yml).
#
#   1. snapcraft, pinned, from the Snap Store (build/ci/snap-tools.sh);
#   2. the .deb - deb-ubuntu24.04-<arch>'s hmailserver_<v>-1~ubuntu24.04_<arch>.deb,
#      or the file HM_SNAP_DEB names - put where snapcraft.yaml's part reads it;
#   3. snapcraft pack in destructive mode, here, for this architecture - no LXD, no
#      remote build: core24 is Ubuntu 24.04, which this container is;
#   4. the snap held to what it must be: its meta/snap.yaml's name, version (the
#      .deb's, without the Debian revision), architecture, base and strict
#      confinement; the files it must carry (snapcraft.yaml's icon among them, as
#      meta/gui/icon.png) and the ones it must not (the systemd
#      units, the live update's helper); its modes git's and the .deb's, every entry
#      root's, each directory 0755 and each file 0644 or 0755, and each file it takes
#      from snap/local and the icon the mode git's index records, whatever the umask
#      and the modes the checkout was written with (a change of mode that git status
#      shows is refused, not undone); the server it carries started with the
#      snap's libraries and answering --version with that version; and
#      hmailserver-start, as snap_daemon, making the configuration from the snap's;
#   5. snap/hmailserver_<version>_<arch>.snap, which snap-upload takes
#      (build/ci/channels/snap-upload.py's contract), and in snap-build/<arch>/ the
#      snap's meta/snap.yaml, SHA-256 and SHA3-384, its file list and snapcraft's
#      log. (snapcraft writes no manifest.yaml: that needs SNAPCRAFT_BUILD_INFO, which
#      would put one into the snap.)
#
# No .snap.source is written: the snap is this pipeline's commit's, which
# snap-upload holds to the release tag's.
#
# Usage, as root, from the root of a checkout: bash build/ci/snap-build.sh
set -euo pipefail

say() { printf 'snap-build: %s\n' "$*"; }
fail() { printf 'snap-build: FAIL %s\n' "$*" >&2; exit 1; }

root="$(pwd)"
[ -f snap/snapcraft.yaml ] || fail "run from the root of a checkout (no snap/snapcraft.yaml here)"

# The snap records each entry's mode as it finds it, so the modes are set here rather
# than inherited. umask 022 for what snapcraft and the parts make - meta/, the
# directories of prime, the files they write - whatever umask this runs under: a job's
# image gives 0022, a shell on Ubuntu 0002, which would make them group-writable.
umask 022

bash build/ci/snap-tools.sh
arch="$(dpkg --print-architecture)"

deb="${HM_SNAP_DEB:-}"
if [ -z "$deb" ]; then
   shopt -s nullglob
   candidates=(packages-deb/ubuntu24.04-"$arch"/hmailserver_*~ubuntu24.04_"$arch".deb)
   shopt -u nullglob
   [ "${#candidates[@]}" = 1 ] || fail "packages-deb/ubuntu24.04-$arch holds ${#candidates[@]} .deb files of this architecture, not one (deb-ubuntu24.04-$arch's artifacts)"
   deb="${candidates[0]}"
fi
[ -f "$deb" ] || fail "no .deb at $deb"
[ "$(dpkg-deb -f "$deb" Package)" = hmailserver ] || fail "$deb is not the hmailserver package"
[ "$(dpkg-deb -f "$deb" Architecture)" = "$arch" ] || fail "$deb is $(dpkg-deb -f "$deb" Architecture), and this container is $arch"
deb_version="$(dpkg-deb -f "$deb" Version)"
version="${deb_version%%-*}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "$deb's version $deb_version is not <major>.<minor>.<patch>-<revision>"
say "the server from $deb ($deb_version)"

rm -rf snap-build/input parts stage prime
mkdir -p snap-build/input "snap-build/$arch"
cp "$deb" "snap-build/input/hmailserver_$arch.deb"

# What the snap takes from the checkout would keep the checkout's modes: the wrappers
# part copies snap/local with them (craft-parts' pull and build, then the dump plugin's
# cp --archive), and snapcraft copies the icon into meta/gui with its mode. A
# checkout's modes are not git's: GitLab Runner's helper writes a job's checkout under
# umask 0000, so that its files are 0666 and its executables and directories 0777, and
# a working copy the runner keeps from job to job keeps for each file the mode of the
# checkout that last wrote it. So each file is given the mode git records for it in
# its index (git ls-files -s) - 0755 for 100755 and 0644 for 100644, the modes of the
# .deb's files - and each directory 0755, before the snap is packed. The mode is read
# from the index, not from the checkout's execute bit, which a working tree chmodded by
# hand or copied from a filesystem without modes may have where git has none; an entry
# git does not record is refused - a file, a symbolic link or anything else that is
# not in the index, and a directory that holds none of the index's files, since the
# dump plugin would copy each into the snap as the checkout has it - and so is a
# symbolic link git records (120000): the plugin copies a link as it is, wherever it
# points, and one that is absolute or leaves snap/local, which becomes the snap's
# root, points outside the snap, which the Snap Store's review refuses (an external
# symlink), so the snap takes none; a file the index records must be a regular file
# in the checkout. The modes are given in the checkout itself, the one place both
# are read from, the icon when snapcraft packs, after the parts' steps, and git
# shows no change: git keeps no mode but the execute bit, and where the checkout's
# differs from the index's while core.fileMode is true - a change of mode that git
# status shows - the build refuses rather than undo it (with core.fileMode false, as
# for a tree copied from a filesystem without modes, git shows none, and the file is
# given the index's mode). Only a tree that is no git checkout, a source tarball, whose
# modes git archive wrote, takes the owner's execute bit for git's. A directory's
# 00755 clears a set-group-ID bit, which GNU's chmod keeps under 0755; snapcraft's own
# directories, prime/ and meta/, still take that bit from a checkout in a
# set-group-ID directory, and the rule on the snap's modes below refuses such a snap,
# as the Snap Store's review refuses any mode but r, w and x. (snapcraft gives each
# hook 0755 itself.) After the pack, the snap's entries from snap/local and the icon
# are held to the modes given here.
icon="$(sed -n 's/^icon: *//p' snap/snapcraft.yaml)"
if [ -z "$icon" ] || [ ! -f "$icon" ]; then
   fail "snapcraft.yaml's icon '$icon' is not a file of the checkout"
fi
# A name that holds a tab or a newline is refused, in a tree that is no git checkout
# as well: unsquashfs's listing, which the rules after the pack and the Snap Store's
# review read a line at a time, cannot carry one.
find snap/local snap/hooks "$icon" -print0 > snap-build/input/checkout-names
while IFS= read -r -d '' name; do
   case "$name" in
      *$'\t'* | *$'\n'*) fail "$(printf '%q' "$name") holds a tab or a newline: the snap's listing, which this build and the Snap Store's review read a line at a time, cannot carry such a name" ;;
   esac
done < snap-build/input/checkout-names
top="$(pwd -P)"
in_git() { git -c safe.directory="$top" "$@"; }
if [ "$(in_git rev-parse --show-toplevel 2> /dev/null)" = "$top" ]; then
   modes_from="git's index"
   in_git ls-files -s -z -- snap/local snap/hooks "$icon" > snap-build/input/git-modes \
      || fail "git ls-files could not list snap/local, snap/hooks and the icon"
   filemode="$(in_git config --bool core.fileMode || echo true)"
   # The directories git's entries are in: git records no directory of its own, so
   # one that holds none of its files is the checkout's alone.
   declare -A recorded=() holds=()
   while IFS= read -r -d '' entry; do
      path="${entry#*$'\t'}"
      recorded["$path"]="${entry%% *}"
      while [[ "$path" == */* ]]; do
         path="${path%/*}"
         holds["$path"]=1
      done
   done < snap-build/input/git-modes
   find snap/local snap/hooks -type d -print0 > snap-build/input/checkout-dirs
   while IFS= read -r -d '' dir; do
      [ -n "${holds[$dir]:-}" ] || fail "$dir holds no file git's index records: the snap takes the entries git records, and this directory is the checkout's alone"
   done < snap-build/input/checkout-dirs
   # Every other entry - a file, a symbolic link, anything but a directory - must be
   # a file git records: a link (120000) would be copied as it is, wherever it points.
   find snap/local snap/hooks "$icon" ! -type d -print0 > snap-build/input/checkout-files
   while IFS= read -r -d '' file; do
      case "${recorded[$file]:-}" in
         100755) mode=0755 ;;
         100644) mode=0644 ;;
         120000) fail "$file is a symbolic link in git's index: the snap takes the files of snap/local, snap/hooks and its icon as regular files, since a link would be copied into it as it is, and one that is absolute or leaves snap/local points outside the snap, which the Snap Store's review refuses" ;;
         '') fail "$file is not in git's index: the snap takes the entries git records, with their modes" ;;
         *) fail "$file is ${recorded[$file]} in git's index, not a file's 100644 or 100755" ;;
      esac
      if [ -L "$file" ] || [ ! -f "$file" ]; then
         fail "$file is a file in git's index (${recorded[$file]}) and not a regular file in the checkout"
      fi
      if [ "$filemode" = true ] && [ $((0$(stat -c %a "$file") & 0100)) != $((mode & 0100)) ]; then
         fail "$file is $(stat -c %a "$file") in the checkout and ${recorded[$file]} in git's index, a change of mode git status shows: the snap takes git's modes, and this build does not undo the change"
      fi
      chmod "$mode" "$file"
   done < snap-build/input/checkout-files
else
   modes_from="the owner's execute bit (no git checkout)"
   find snap/local snap/hooks "$icon" -type f -perm -0100 -exec chmod 0755 {} +
   find snap/local snap/hooks "$icon" -type f ! -perm -0100 -exec chmod 0644 {} +
fi
find snap/local snap/hooks -type d -exec chmod 00755 {} +
# What the snap must record for each: snap/local/X as /X, the icon as /meta/gui/icon.png,
# a line each, the mode, a tab and the path, which may hold a space.
find snap/local "$icon" -type f -print0 > snap-build/input/checkout-files
while IFS= read -r -d '' file; do
   case "$file" in
      "$icon") entry=/meta/gui/icon.png ;;
      *) entry="${file#snap/local}" ;;
   esac
   printf '%s\t%s\n' "$(stat -c %A "$file")" "$entry"
done < snap-build/input/checkout-files | LC_ALL=C sort -t "$(printf '\t')" -k 2 > snap-build/input/modes
say "the modes of snap/local, snap/hooks and the icon given from $modes_from"

say "snapcraft pack, destructive mode, for $arch"
started=$SECONDS
if ! snapcraft pack --destructive-mode --platform "$arch" --verbosity brief > "snap-build/$arch/snapcraft.log" 2>&1; then
   tail -n 60 "snap-build/$arch/snapcraft.log" >&2
   fail "snapcraft pack failed (snap-build/$arch/snapcraft.log)"
fi
say "packed in $((SECONDS - started)) s"
grep -iE 'lint|warning' "snap-build/$arch/snapcraft.log" || true

built="hmailserver_${version}_$arch.snap"
[ -f "$built" ] || fail "snapcraft made no $built: $(printf '%s ' ./*.snap)"

# --- what the snap is ---------------------------------------------------------
meta="$(unsquashfs -cat "$built" meta/snap.yaml)"
printf '%s\n' "$meta" > "snap-build/$arch/snap.yaml"
field() { printf '%s\n' "$meta" | sed -n "s/^$1: //p" | head -n 1 | tr -d "'\""; }
[ "$(field name)" = hmailserver ] || fail "meta/snap.yaml names $(field name), not hmailserver"
[ "$(field version)" = "$version" ] || fail "meta/snap.yaml's version is $(field version), not the .deb's $version"
[ "$(field base)" = core24 ] || fail "meta/snap.yaml's base is $(field base), not core24"
[ "$(field confinement)" = strict ] || fail "meta/snap.yaml's confinement is $(field confinement), not strict"
[ "$(field grade)" = stable ] || fail "meta/snap.yaml's grade is $(field grade), not stable"
printf '%s\n' "$meta" | grep -qx -- "- $arch" || fail "meta/snap.yaml's architectures are not [$arch]"
printf '%s\n' "$meta" | grep -qx '  snap_daemon: shared' || fail "meta/snap.yaml does not ask for the snap_daemon account"
say "meta/snap.yaml: hmailserver $version, $arch, core24, strict, stable, snap_daemon"

unsquashfs -l "$built" | sed 's|^squashfs-root||' | sort > "snap-build/$arch/files.txt"
for must in /usr/bin/hmailserver /usr/bin/dh2048.pem /usr/bin/hmctl /bin/hmailserver-daemon /bin/hmailserver-start \
            /bin/hmailserver-run /bin/hmailserver-cli /bin/hmailserver-snap-env /bin/hmailserver-snap-config \
            /bin/hmailserver-set-database-password /bin/hmailserver-entrypoint /meta/hooks/configure \
            /etc/hmailserver/hMailServer.ini /usr/share/hmailserver/WebAdmin/index.html /usr/bin/timeout \
            /usr/share/hmailserver/DBScripts/CreateTablesPGSQL.sql /usr/share/hmailserver/licenses/AGPL-3.0.txt \
            /meta/gui/icon.png; do
   grep -qx -- "$must" "snap-build/$arch/files.txt" || fail "the snap has no $must"
done
for never in /usr/lib/systemd /usr/lib/hmailserver /etc/fail2ban /etc/logrotate.d; do
   if grep -q -- "^$never" "snap-build/$arch/files.txt"; then
      fail "the snap carries $never, which a snap does not use"
   fi
done
say "the snap's files: what it must carry is there, the units and the live update's helper are not ($(wc -l < "snap-build/$arch/files.txt") entries)"

# Its modes: every entry root's, as snap pack makes them, and each directory 0755 and
# each file 0644 or 0755 - readable by snap_daemon, whom the commands run as, and
# writable by none but root - so that what the snap records is git's modes and the
# .deb's, not the checkout's or a umask's.
odd="$(unsquashfs -lls "$built" | awk '
   $1 ~ /^l/ { next }
   { path = substr($0, index($0, "squashfs-root") + length("squashfs-root")) }
   $2 != "root/root" || ($1 ~ /^d/ && $1 != "drwxr-xr-x") || ($1 !~ /^d/ && $1 != "-rw-r--r--" && $1 != "-rwxr-xr-x") { print $1, $2, (path == "" ? "/" : path) }
')"
[ -z "$odd" ] || fail "the snap has entries that are not root's with mode 0755 (a directory or an executable) or 0644:
$odd"
# And what it took from snap/local and the icon has the modes given them above.
odd="$(unsquashfs -lls "$built" | awk '
   NR == FNR { tab = index($0, "\t"); want[substr($0, tab + 1)] = substr($0, 1, tab - 1); next }
   { path = substr($0, index($0, "squashfs-root") + length("squashfs-root")); if ($1 ~ /^l/) sub(/ -> .*$/, "", path) }
   path in want { if ($1 != want[path]) print $1, path, "where", want[path], "was given"; delete want[path] }
   END { for (path in want) print "no", path }
' snap-build/input/modes -)"
[ -z "$odd" ] || fail "the snap's entries from snap/local and the icon do not have the modes given them ($modes_from):
$odd"
say "the snap's modes: every entry root's, each directory 0755 and each file 0644 or 0755, and the $(wc -l < snap-build/input/modes) files from snap/local and the icon as given, from $modes_from"

# The server, with the snap's libraries over core24's (this container's are core24's
# own release): every library found, and --version the snap's.
case "$arch" in amd64) triplet=x86_64-linux-gnu ;; arm64) triplet=aarch64-linux-gnu ;; esac
missing="$(LD_LIBRARY_PATH="$root/prime/usr/lib/$triplet:$root/prime/usr/lib" ldd prime/usr/bin/hmailserver | grep 'not found' || true)"
[ -z "$missing" ] || fail "the server's libraries are not all in the snap or core24: $missing"
said="$(LD_LIBRARY_PATH="$root/prime/usr/lib/$triplet:$root/prime/usr/lib" prime/usr/bin/hmailserver --version 2>&1 | head -n 1)"
case "$said" in
   *"$version"*) say "prime/usr/bin/hmailserver --version: $said" ;;
   *) fail "the snap's server says '$said', not version $version" ;;
esac
python3 prime/bin/hmailserver-snap-config --self-test --source "$root"

# The step every start and every sudo hmailserver takes as the server's account:
# hmailserver-start, run as snap_daemon (uid 584788) with no group and nothing of
# root's, from the snap's files as a mount shows them - unsquashfs, as root, keeps
# their owners and modes, which a kernel's squashfs enforces - makes the configuration
# from the snap's etc/hmailserver/hMailServer.ini and runs the server against it.
started_as="$(mktemp -d)"
chmod 0755 "$started_as"
unsquashfs -q -n -d "$started_as/snap" "$built" > /dev/null
mkdir -m 0755 "$started_as/common"
mkdir -m 0700 "$started_as/common/etc"
mkdir -m 0750 "$started_as/common/data" "$started_as/common/log"
chown 584788:584788 "$started_as/common/etc" "$started_as/common/data" "$started_as/common/log"
if ! said="$(cd / && env -i PATH=/usr/bin:/bin SNAP="$started_as/snap" SNAP_COMMON="$started_as/common" SNAP_ARCH="$arch" \
      setpriv --reuid=584788 --regid=584788 --clear-groups --no-new-privs -- \
      "$started_as/snap/bin/hmailserver-start" cli --version 2>&1)"; then
   fail "hmailserver-start, as snap_daemon, did not make the configuration and run the server: $said"
fi
case "$said" in
   *"$version"*) ;;
   *) fail "hmailserver-start cli --version, as snap_daemon, said '$said', not version $version" ;;
esac
[ "$(stat -c '%u %a' "$started_as/common/etc/hMailServer.ini")" = "584788 600" ] \
   || fail "the configuration hmailserver-start made is $(stat -c '%u %a' "$started_as/common/etc/hMailServer.ini"), not snap_daemon's and 0600"
rm -rf "$started_as"
say "hmailserver-start as snap_daemon: the configuration made from the snap's, snap_daemon's and 0600, and the server's --version $version"

mv -f "$built" "snap/$built"
(
   cd snap
   sha256sum "$built" > "../snap-build/$arch/SHA256SUMS.txt"
   python3 -c 'import hashlib, sys; h = hashlib.sha3_384(); f = open(sys.argv[1], "rb"); [h.update(b) for b in iter(lambda: f.read(1 << 20), b"")]; print(h.hexdigest() + "  " + sys.argv[1])' "$built" > "../snap-build/$arch/SHA3-384SUMS.txt"
)
rm -rf snap-build/input parts stage prime
say "PASS snap/$built ($(stat -c %s "snap/$built") bytes, SHA-256 $(cut -d' ' -f1 "snap-build/$arch/SHA256SUMS.txt"))"
