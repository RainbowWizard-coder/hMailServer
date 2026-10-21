#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# WHAT THIS PRODUCES, AND WHAT IT IS NOT FOR
#
# An AppImage of a mail server is a try-it-out artefact and nothing more. Read
# this paragraph before you deploy one, because everything about it is different
# from the .deb and the .rpm:
#
#   * It runs as whoever double-clicked it. There is no hmailserver system user,
#     no privilege separation, and no systemd unit with the hardening this
#     repository spent a file on - the whole of ProtectSystem, the capability
#     bounding set and the syscall filter are simply absent.
#   * Its data lives under $HOME. The mail store, the logs and the configuration
#     go to ${XDG_DATA_HOME:-$HOME/.local/share}/hmailserver unless
#     HMAILSERVER_HOME says otherwise - which is exactly where a mail store
#     should not be. A backup that covers the machine's services will not cover
#     it, and a home directory on NFS will not survive a message store on it.
#   * It cannot bind port 25. An ordinary user has no CAP_NET_BIND_SERVICE, and
#     the listener ports live in the DATABASE (hm_tcpipports: 25, 587, 110 and
#     143 as created), not in the configuration file - so trying it out means
#     changing those rows to ports above 1024, or granting the capability to the
#     extracted binary by hand.
#   * It still needs a database. There is no such thing as a self-contained
#     hMailServer: PostgreSQL or MySQL/MariaDB has to be running somewhere and
#     the schema has to have been created, exactly as for a package install.
#
# A real installation uses the .deb or the .rpm, which put the store in
# /var/lib/hmailserver under a user that owns it, and run the server under a unit
# that constrains it. Use this to see the thing work on a laptop, to reproduce a
# bug against a specific build, or to hand somebody a binary without asking them
# to install anything. Not to run mail.
#
# USAGE
#   build-appimage.sh --runtime FILE [--runtime-licenses DIR] [--build-dir DIR]
#                     [--output-dir DIR] [--no-strip]
#
#   --runtime     the AppImage runtime to put in front of the filesystem: the
#                 project's own, built from appimage/runtime.c beside this script
#                 by build/ci/packages/appimage-runtime.sh, for the binary's
#                 architecture. Also taken from APPIMAGE_RUNTIME. Nothing is
#                 downloaded: the runtime the AppImage tools use is published
#                 only on GitHub, and this script no longer uses those tools.
#   --runtime-licenses
#                 the licence files of the libraries linked into that runtime,
#                 one directory per library, which the appimage-runtime-sources
#                 job extracts from each library's upstream archive
#                 (build/ci/packages/appimage-runtime-sources.sh, licenses-<arch>/):
#                 copied to usr/share/hmailserver/licenses/appimage-runtime/, beside
#                 the runtime's own notice and the GPL-3.0's text (appimage/GPL-3.0.txt,
#                 which goes in always). The LGPL-2.1 of libfuse3 and the GPL of
#                 lzo ask for their text to go with the binary, and the BSD and MIT
#                 licences of the others for their notice. Also taken from
#                 APPIMAGE_RUNTIME_LICENSES.
#   --build-dir   a tree that has ALREADY been built, i.e. one where
#                 "cmake --build" has produced the hmailserver executable.
#                 Default: build/linux relative to the repository root.
#   --output-dir  where the .AppImage is written. Default: the build directory.
#   --no-strip    keep the symbols in the AppImage's copy of the server.
#
# It builds nothing itself. Build first:
#
#   cmake -S hmailserver/source/Server -B build/linux -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build/linux --target hmailserver
#
# What it needs on PATH: ldd (glibc's), readelf and strip (binutils), patchelf,
# and mksquashfs (squashfs-tools) - each a distribution package. And dpkg, for
# the licences of the libraries it copies (below): without it, as on Fedora, it
# says the image will not carry them, and makes it all the same.
#
# HOW THE AppImage IS MADE
#
# An AppImage is the runtime - a small static ELF - with a SquashFS image of an
# AppDir appended. This script makes the AppDir (the server, its data files,
# the shared libraries it needs, an AppRun, a desktop file and an icon), packs
# it with mksquashfs and writes the runtime and the image one after the other.
# linuxdeploy used to do the middle of that; it is gone because it is published
# only on GitHub, and what it did for this program is short:
#
#   * the libraries: every one ldd resolves for the server is copied into
#     usr/lib, except the ones the AppImage convention leaves to the host (the
#     EXCLUDED list below says which and why), and the server's RUNPATH is set
#     to $ORIGIN/../lib and each copied library's to $ORIGIN, so they are found
#     inside the image and nowhere else;
#   * the desktop file and the icon at the AppDir's root, and .DirIcon.
#
# And the licences, in usr/share/hmailserver/licenses/: hMailServer's own, the
# AGPL-3.0 (AGPL-3.0.txt, the repository's LICENSE); the notices of what is
# compiled into the server, QuickJS's (QuickJS.txt), Unicode's for its data for
# UTS 46 (Unicode-data.txt) and the WHATWG's for HTML's list of named character
# references (HTML-named-character-references.txt); the runtime's
# notice and its libraries' licence files (appimage-runtime.txt and
# appimage-runtime/); and for the libraries copied into usr/lib, the copyright
# file of the package each came from, as the distribution ships it
# (bundled/<package>/copyright), the licence texts those files name in
# /usr/share/common-licenses (bundled/common-licenses/), and bundled.txt, which
# lists each library with its package, version and source package, and names the
# release file with their source, hmailserver-<version>-appimage-bundled-sources.tar.gz
# (build/ci/packages/appimage-bundled-sources.sh; docs/PackagesAppImage.md,
# "Licences").
#
# The server is built on the oldest distribution release that has the Boost it
# needs (1.83): Ubuntu 24.04, glibc 2.39. The AppImage runs where glibc is 2.39
# or later - Ubuntu 24.04 and later, Debian 13, Fedora 40 and later, RHEL 10,
# openSUSE Leap 16 and Tumbleweed - and not on Debian 12, Ubuntu 22.04, RHEL 9
# or Leap 15, whose glibc is older.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# packaging -> platform -> Server -> source -> hmailserver -> repository root
REPO_ROOT="$(cd "${HERE}/../../../../.." && pwd)"
SERVER_DIR="${REPO_ROOT}/hmailserver/source/Server"

BUILD_DIR="${REPO_ROOT}/build/linux"
OUTPUT_DIR=""
RUNTIME="${APPIMAGE_RUNTIME:-}"
RUNTIME_LICENSES="${APPIMAGE_RUNTIME_LICENSES:-}"
STRIP=1

while [ "$#" -gt 0 ]; do
   case "$1" in
      --runtime)
         [ "$#" -ge 2 ] || { echo "--runtime needs a file." >&2; exit 2; }
         RUNTIME="$2"
         shift 2
         ;;
      --runtime-licenses)
         [ "$#" -ge 2 ] || { echo "--runtime-licenses needs a directory." >&2; exit 2; }
         RUNTIME_LICENSES="$2"
         shift 2
         ;;
      --no-strip)
         STRIP=0
         shift
         ;;
      --build-dir)
         [ "$#" -ge 2 ] || { echo "--build-dir needs a directory." >&2; exit 2; }
         BUILD_DIR="$2"
         shift 2
         ;;
      --output-dir)
         [ "$#" -ge 2 ] || { echo "--output-dir needs a directory." >&2; exit 2; }
         OUTPUT_DIR="$2"
         shift 2
         ;;
      -h|--help)
         sed -n '5,106p' "${BASH_SOURCE[0]}"
         exit 0
         ;;
      *)
         echo "Unknown option: $1" >&2
         exit 2
         ;;
   esac
done

BUILD_DIR="$(cd "${BUILD_DIR}" 2>/dev/null && pwd || true)"
if [ -z "${BUILD_DIR}" ]; then
   echo "The build directory does not exist. Build the tree first; see --help." >&2
   exit 1
fi

BINARY="${BUILD_DIR}/hmailserver"
if [ ! -x "${BINARY}" ]; then
   echo "No executable at ${BINARY}. This script packages an already-built tree; it does not build one." >&2
   exit 1
fi

[ -n "${OUTPUT_DIR}" ] || OUTPUT_DIR="${BUILD_DIR}"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

# ---------------------------------------------------------------- version, arch

# The binary is the authority on its own version - it prints what Version.h said
# when it was compiled, which is the only answer that cannot be stale. Version.h
# itself is the fallback for a cross-built tree whose binary will not run here;
# it used to be the CMakeLists, which stopped working the day the CMakeLists
# began reading the header rather than carrying a literal of its own.
VERSION="$("${BINARY}" --version 2>/dev/null | awk '{ print $2 }' || true)"
if [ -z "${VERSION}" ]; then
   VERSION="$(sed -n 's/^#define[[:space:]][[:space:]]*HMAILSERVER_VERSION[[:space:]][[:space:]]*"\([0-9.]*\)".*/\1/p' \
      "${SERVER_DIR}/Common/Application/Version.h" | head -n 1)"
fi
if [ -z "${VERSION}" ]; then
   echo "Could not determine the version from the binary or from Common/Application/Version.h." >&2
   exit 1
fi

# ---------------------------------------------------------------- the tools

missing=""
for tool in ldd readelf strip patchelf mksquashfs; do
   command -v "${tool}" > /dev/null 2>&1 || missing="${missing} ${tool}"
done
if [ -n "${missing}" ]; then
   echo "Missing:${missing}. On Debian and Ubuntu: apt-get install libc-bin binutils patchelf squashfs-tools." >&2
   exit 1
fi

# The architecture is the BINARY's, not this machine's: the name the update
# checker looks for (UpdateChecker.cpp) carries uname -m's spelling of the
# machine the server was compiled for, and the runtime in front of it must be
# for the same one.
machine_of()
{
   readelf -h "$1" 2>/dev/null | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p'
}

case "$(machine_of "${BINARY}")" in
   *X86-64*) ARCH="x86_64" ;;
   AArch64) ARCH="aarch64" ;;
   *)
      echo "${BINARY} is for $(machine_of "${BINARY}"); only x86_64 and aarch64 are packaged as AppImages." >&2
      exit 1
      ;;
esac

if [ -z "${RUNTIME}" ]; then
   echo "No runtime given. Build one with build/ci/packages/appimage-runtime.sh and pass --runtime." >&2
   exit 1
fi
if [ ! -f "${RUNTIME}" ]; then
   echo "The runtime ${RUNTIME} does not exist." >&2
   exit 1
fi
RUNTIME="$(cd "$(dirname "${RUNTIME}")" && pwd)/$(basename "${RUNTIME}")"
if [ -n "${RUNTIME_LICENSES}" ] && [ -z "$(find "${RUNTIME_LICENSES}" -mindepth 2 -maxdepth 2 -type f 2> /dev/null)" ]; then
   echo "--runtime-licenses ${RUNTIME_LICENSES} holds no licence file under a library's directory." >&2
   exit 1
fi
if [ -z "${RUNTIME_LICENSES}" ]; then
   echo "WARNING: no --runtime-licenses: this image will not carry the licence files of the libraries linked into its" >&2
   echo "         runtime, which their licences ask for; the CI jobs pass appimage-runtime-sources/licenses-<arch>." >&2
fi
# "AI" and 2 at offset 8: the AppImage type 2 magic appimage-runtime.sh writes.
if [ "$(od -An -tx1 -j8 -N3 "${RUNTIME}" | tr -d ' \n')" != "414902" ]; then
   echo "${RUNTIME} does not carry the AppImage type 2 magic at offset 8; it is not a runtime." >&2
   exit 1
fi
if [ "$(machine_of "${RUNTIME}")" != "$(machine_of "${BINARY}")" ]; then
   echo "The runtime is for $(machine_of "${RUNTIME}") and the server for $(machine_of "${BINARY}")." >&2
   exit 1
fi

# ldd resolves the libraries by running the binary's own dynamic loader, so the
# binary has to be runnable here: natively, or through qemu-user where binfmt_misc
# is registered for its machine.
if ! ldd "${BINARY}" > /dev/null 2>&1; then
   echo "ldd cannot resolve ${BINARY} on this machine ($(uname -m)). Package it on its own architecture, or in a container of it under qemu-user." >&2
   exit 1
fi

echo "hMailServer ${VERSION} (${ARCH}) from ${BUILD_DIR}, with the runtime ${RUNTIME}"

# ---------------------------------------------------------------- the AppDir

APPDIR="${BUILD_DIR}/AppDir"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/share/applications" \
         "${APPDIR}/usr/share/hmailserver"

install -m 0755 "${BINARY}" "${APPDIR}/usr/bin/hmailserver"

# Beside the binary, because that is where the server looks for it:
# Utilities::GetBinDirectory() on this platform is the directory of the running
# image (/proc/self/exe), not ProgramFolder. The .deb and .rpm put it in the
# same relative place for the same reason.
#
#   dh2048.pem  the Diffie-Hellman group. Without it SslContextInitializer
#               reports a CRITICAL 5603 and every TLS context refuses the
#               connection - an AppImage without this file is a mail server
#               that cannot do TLS at all.
install -m 0644 "${REPO_ROOT}/hmailserver/installation/Extras/dh2048.pem" \
   "${APPDIR}/usr/bin/dh2048.pem"

# The SQL the administrator has to run to create the schema. It is small, it is
# what the .deb and .rpm install to the same relative path, and an AppImage
# without it cannot get as far as a database.
cp -r "${REPO_ROOT}/hmailserver/source/DBScripts" "${APPDIR}/usr/share/hmailserver/DBScripts"

# QuickJS's licence. The engine is compiled into the binary, and its MIT licence
# asks for the notice in every copy; the .deb, .rpm and PKGBUILD install it
# through CMake's install rule, which this script does not run, so it is placed
# here at the same path they give it.
install -Dm 0644 "${REPO_ROOT}/hmailserver/source/Server/quickjs/LICENSE" \
   "${APPDIR}/usr/share/hmailserver/licenses/QuickJS.txt"

# And the notices of the two data sets compiled into the server as tables, for the
# same reason and from the same files as the install rules take them
# (CMakeLists.txt): Unicode's data for UTS 46, whose Unicode License v3 asks for its
# notice with every copy, and HTML's list of named character references, under the
# BSD 3-Clause License, whose second condition asks a binary to carry its notice,
# conditions and disclaimer.
install -m 0644 "${REPO_ROOT}/hmailserver/source/Server/Common/Idna/ucd/unicode.LICENSE" \
   "${APPDIR}/usr/share/hmailserver/licenses/Unicode-data.txt"
install -m 0644 "${REPO_ROOT}/hmailserver/source/Server/Common/AntiSpam/whatwg/entities.LICENSE" \
   "${APPDIR}/usr/share/hmailserver/licenses/HTML-named-character-references.txt"

# hMailServer's own licence, the AGPL-3.0-or-later. Its section 4 asks for a copy
# of the licence to go with every copy of the program, and section 6 conveys the
# object code on the terms of sections 4 and 5. The repository's LICENSE is the
# FSF's text; .gitattributes checks it out with CRLF line ends on every platform,
# and the image carries it with them taken off, which is the FSF's file byte for
# byte (SHA-256 0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0).
tr -d '\r' < "${REPO_ROOT}/LICENSE" > "${APPDIR}/usr/share/hmailserver/licenses/AGPL-3.0.txt"
chmod 0644 "${APPDIR}/usr/share/hmailserver/licenses/AGPL-3.0.txt"

# The Control Deck. AppRun points ProgramFolder at this same directory, and
# RestApiServer::HandleWebAdminPage_ opens <ProgramFolder>/WebAdmin/index.html;
# without the file the REST listener answers GET / with a stub saying the page
# is not installed, which for a try-it-out artefact is the first thing anybody
# would see.
install -Dm 0644 "${REPO_ROOT}/hmailserver/installation/WebAdmin/index.html" \
   "${APPDIR}/usr/share/hmailserver/WebAdmin/index.html"
# Its catalogues, one per language, which the listener serves at
# /deck-lang/<code>.json from the languages directory beside the page.
install -d "${APPDIR}/usr/share/hmailserver/WebAdmin/languages"
install -m 0644 "${REPO_ROOT}/hmailserver/installation/WebAdmin/languages/"*.json \
   "${APPDIR}/usr/share/hmailserver/WebAdmin/languages/"

# The live update's apply step, at the path the packages install it to, so that
# an AppImage run under a unit can swap itself: the server copies the script out
# of the image into its Updates directory and hands it to systemd-run, and the
# script replaces the image file under the unit and restarts it. The two unit
# files beside it are references for an administrator who runs the image as a
# system service and wants the path-unit route the packages use instead;
# hmailserver-update's header and packaging/README.md say when each applies.
# None of the three from a tree configured with -DHM_LIVE_UPDATE=OFF, whose
# server has no apply step to hand anything to them - as its `cmake --install`
# leaves them out (CMakeLists.txt, docs/LiveUpdateKinds.md). The cache holds the
# value as it was given, so every spelling CMake takes for false is read as OFF; a
# tree from before the option has no line, and carries the live update.
live_update="ON"
if grep -q '^HM_LIVE_UPDATE:BOOL=' "${BUILD_DIR}/CMakeCache.txt" 2> /dev/null; then
   live_update="$(sed -n 's/^HM_LIVE_UPDATE:BOOL=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n 1 | tr '[:upper:]' '[:lower:]' || true)"
fi
case "${live_update}" in
   ""|0|off|no|false|n|ignore|notfound|*-notfound)
      echo "The tree was built with HM_LIVE_UPDATE=${live_update:-(empty)}: no hmailserver-update in the AppImage."
      ;;
   *)
      install -Dm 0755 "${HERE}/hmailserver-update" "${APPDIR}/usr/lib/hmailserver/hmailserver-update"
      install -Dm 0644 "${HERE}/hmailserver-update.path" "${APPDIR}/usr/share/hmailserver/systemd/hmailserver-update.path"
      install -Dm 0644 "${HERE}/hmailserver-update.service" "${APPDIR}/usr/share/hmailserver/systemd/hmailserver-update.service"
      ;;
esac

# The configuration the AppRun below copies out on first run. It is the packaged
# default with its absolute paths still in it; AppRun rewrites them, so this file
# is a template and never read in place.
install -m 0644 "${HERE}/hMailServer.ini" "${APPDIR}/usr/share/hmailserver/hMailServer.ini.default"

# There is no Linux icon in this repository - the only icon is a Windows .ico for
# the Control Panel - so one is drawn here rather than a placeholder being
# committed. Replace it when the project has artwork.
cat > "${BUILD_DIR}/hmailserver.svg" <<'ICON'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">
  <rect width="128" height="128" rx="16" fill="#1f4e79"/>
  <rect x="20" y="38" width="88" height="56" rx="6" fill="#ffffff"/>
  <path d="M20 44 L64 74 L108 44" fill="none" stroke="#1f4e79" stroke-width="7"
        stroke-linecap="round" stroke-linejoin="round"/>
</svg>
ICON

cat > "${BUILD_DIR}/hmailserver.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=hMailServer (Progressive Robot's fork)
Comment=Progressive Robot's fork of hMailServer, a mail server for SMTP, IMAP and POP3
Exec=hmailserver
Icon=hmailserver
Terminal=true
Categories=Network;Email;
DESKTOP

# The AppRun. A bare symlink to the executable - what the AppImage tools write
# when given none - would leave the server looking for hMailServer.ini beside a
# binary inside a read-only mount and finding none.
#
# What this does instead is the whole difference between an AppImage that works
# out of the box and one that prints a configuration error: it keeps a
# per-invoking-user home, creates the directory layout there the first time, and
# hands the server that configuration explicitly with --config.
cat > "${BUILD_DIR}/AppRun" <<'APPRUN'
#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The AppImage entry point. See build-appimage.sh for what this artefact is and
# is not; the short version is that everything below keeps state in the invoking
# user's home, which is the right answer for trying something out and the wrong
# answer for running mail.

set -e

APPDIR="$(dirname "$(readlink -f "$0")")"

HMAILSERVER_HOME="${HMAILSERVER_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/hmailserver}"
CONFIG="${HMAILSERVER_HOME}/hMailServer.ini"

# The mail, the logs that name who wrote to whom, the database and the event
# scripts are the invoking user's alone: every folder made here, and every file
# the server makes in them, is made under umask 077. They used to take the user's
# umask - 0755 under the usual 022 - and a home directory other users may enter
# (0755, as many distributions still make one) let them read every message.
# The five folders are restated 0700 on every run, as the configuration is 0600
# below, so that a tree an earlier AppImage made is closed too.
umask 077
mkdir -p "${HMAILSERVER_HOME}/data" \
         "${HMAILSERVER_HOME}/logs" \
         "${HMAILSERVER_HOME}/temp" \
         "${HMAILSERVER_HOME}/events" \
         "${HMAILSERVER_HOME}/database"
chmod 0700 "${HMAILSERVER_HOME}/data" \
           "${HMAILSERVER_HOME}/logs" \
           "${HMAILSERVER_HOME}/temp" \
           "${HMAILSERVER_HOME}/events" \
           "${HMAILSERVER_HOME}/database"

if [ ! -f "${CONFIG}" ]; then
   # The packaged default with its six [Directories] values repointed under the
   # per-user home. Every other line - the [Database] section that still has to
   # be filled in, and the comments explaining it - is carried across unchanged.
   sed \
      -e "s|^ProgramFolder=.*|ProgramFolder=${APPDIR}/usr/share/hmailserver|" \
      -e "s|^DataFolder=.*|DataFolder=${HMAILSERVER_HOME}/data|" \
      -e "s|^LogFolder=.*|LogFolder=${HMAILSERVER_HOME}/logs|" \
      -e "s|^TempFolder=.*|TempFolder=${HMAILSERVER_HOME}/temp|" \
      -e "s|^EventFolder=.*|EventFolder=${HMAILSERVER_HOME}/events|" \
      -e "s|^DatabaseFolder=.*|DatabaseFolder=${HMAILSERVER_HOME}/database|" \
      "${APPDIR}/usr/share/hmailserver/hMailServer.ini.default" > "${CONFIG}"

   chmod 0600 "${CONFIG}"

   echo "A configuration has been created at ${CONFIG}." >&2
   echo "It has no database configured yet; edit the [Database] section, create" >&2
   echo "the schema from ${APPDIR}/usr/share/hmailserver/DBScripts, then run this" >&2
   echo "AppImage again with --check-config." >&2
fi

# ProgramFolder is the one value in this file that CANNOT be allowed to persist,
# so it is rewritten on EVERY run and not only when the file is created. It
# points inside the AppImage's own mount, and that path is different every time:
# FUSE mounts the image at a fresh /tmp/.mount_XXXXXX. Left as the first run
# wrote it, the second run would look for DBScripts and for the Control Deck
# under a mount point that no longer exists - and the server would answer GET /
# with the "not installed" stub on every run after the first.
#
# Only this one line is touched. Everything else in the file is the
# administrator's, including the [Database] section they filled in, and stays
# exactly as they wrote it.
sed -i "s|^ProgramFolder=.*|ProgramFolder=${APPDIR}/usr/share/hmailserver|" "${CONFIG}"
# sed -i writes a new file and renames it over the old one. GNU sed carries the
# original's mode across; not every sed does, and this file holds a database
# password, so the mode is restated rather than assumed.
chmod 0600 "${CONFIG}"

# --config is given explicitly rather than relying on the search order, because
# the first place the server looks is beside its own executable - which here is
# inside a read-only mount.
exec "${APPDIR}/usr/bin/hmailserver" --config "${CONFIG}" "$@"
APPRUN
chmod +x "${BUILD_DIR}/AppRun"

# ---------------------------------------------------------------- the AppDir's root

# What the format asks for at the top of an AppDir: the entry point, one desktop
# file and the icon it names, and .DirIcon, which file managers show. The same
# desktop file and icon also go where the freedesktop layout puts them, for a
# desktop integration tool that looks there.
install -m 0755 "${BUILD_DIR}/AppRun" "${APPDIR}/AppRun"
install -m 0644 "${BUILD_DIR}/hmailserver.desktop" "${APPDIR}/hmailserver.desktop"
install -m 0644 "${BUILD_DIR}/hmailserver.desktop" "${APPDIR}/usr/share/applications/hmailserver.desktop"
install -m 0644 "${BUILD_DIR}/hmailserver.svg" "${APPDIR}/hmailserver.svg"
install -Dm 0644 "${BUILD_DIR}/hmailserver.svg" "${APPDIR}/usr/share/icons/hicolor/scalable/apps/hmailserver.svg"
ln -s hmailserver.svg "${APPDIR}/.DirIcon"
if command -v desktop-file-validate > /dev/null 2>&1; then
   desktop-file-validate "${APPDIR}/hmailserver.desktop"
fi

# What the runtime was built from, and under which licences, beside the other
# notices: the runtime sits outside the filesystem, so its notice goes in.
if [ -f "${RUNTIME}.txt" ]; then
   install -Dm 0644 "${RUNTIME}.txt" "${APPDIR}/usr/share/hmailserver/licenses/appimage-runtime.txt"
fi
# The GPL-3.0's own text: the runtime takes lzo, which is GPL-2.0-or-later, under
# the GPL-3.0 its "or later" allows (runtime.c is AGPL-3.0-or-later, and section 13
# of each lets the two be combined), and that licence asks for a copy of itself to
# go with the program (section 4, which section 6 applies to object code). The
# FSF's text, beside runtime.c (appimage/GPL-3.0.txt; SHA-256 3972dc97..., as
# Ubuntu's base-files and xz's upstream archive carry it).
install -Dm 0644 "${HERE}/appimage/GPL-3.0.txt" "${APPDIR}/usr/share/hmailserver/licenses/appimage-runtime/GPL-3.0.txt"
# And the licence files of the libraries linked into it, one directory each.
if [ -n "${RUNTIME_LICENSES}" ]; then
   for library in "${RUNTIME_LICENSES}"/*/; do
      name="$(basename "${library}")"
      for licence in "${library}"*; do
         [ -f "${licence}" ] || continue
         install -Dm 0644 "${licence}" "${APPDIR}/usr/share/hmailserver/licenses/appimage-runtime/${name}/$(basename "${licence}")"
      done
   done
fi

# ---------------------------------------------------------------- the libraries

# What is NOT copied into the image: the AppImage convention (the "excludelist"
# the AppImage project keeps, which linuxdeploy applies) leaves to the host the
# libraries that are part of every installation, that are tied to the host's
# kernel, loader or drivers, or that break when two copies meet in one process -
# above all glibc, which cannot be bundled at all: its loader is named by an
# absolute path in the binary, and a libc from another release than the loader
# fails in the first call. The convention also lists the graphics and sound
# stack, which a mail server does not link.
#
# This list is that convention applied to what this server links, with two
# decisions of the project's beside it, each for a reason of its own:
#
#   glibc and its loader     ld-linux-*, libc, libm, libdl, libpthread, librt,
#                            libresolv, libutil, libanl, libnsl, libmvec,
#                            libBrokenLocale, libthread_db, libc_malloc_debug and
#                            the NSS modules: the convention's own core
#   libgcc_s, libstdc++      the compiler's runtime. The convention bundles
#                            libstdc++ when it must; here it need not, because the
#                            glibc this build needs (2.39) is only found on
#                            releases whose libstdc++ is newer than Ubuntu 24.04's,
#                            and a bundled older copy would be the one loaded by
#                            anything of the host's that the server loads (the
#                            MariaDB client, below) and wants a newer one
#   libz                     in every installation, with an ABI that has not
#                            changed in twenty years: the convention's
#   libssl, libcrypto        OpenSSL 3, from the host - the project's decision:
#                            its ABI is stable across 3.x, every release with this
#                            glibc ships it, and the host's copy is the one its
#                            distribution patches, that reads its configuration
#                            (and on Fedora and RHEL its crypto policy), and that
#                            finds its certificate store where that distribution
#                            keeps it. A bundled Ubuntu OpenSSL on Fedora would do
#                            none of that.
#
# libmariadb.so.3 is not linked: the server opens it with dlopen when a MySQL or
# MariaDB database is configured (MySQLInterface.cpp), so it is the host's, as
# it was with linuxdeploy, and a host with MySQL wants its client installed.
EXCLUDED_PATTERNS="
ld-linux*.so.*
libc.so.*
libm.so.*
libdl.so.*
libpthread.so.*
librt.so.*
libresolv.so.*
libutil.so.*
libanl.so.*
libnsl.so.*
libmvec.so.*
libBrokenLocale.so.*
libthread_db.so.*
libc_malloc_debug.so.*
libnss_*.so.*
libgcc_s.so.*
libstdc++.so.*
libz.so.*
libssl.so.*
libcrypto.so.*
"

is_excluded()
{
   local name="$1" pattern
   for pattern in ${EXCLUDED_PATTERNS}; do
      # shellcheck disable=SC2254
      case "${name}" in
         ${pattern}) return 0 ;;
      esac
   done
   return 1
}

mkdir -p "${APPDIR}/usr/lib"

# ldd lists the whole closure - every library the server needs, directly or
# through another - as this machine's loader resolves it. Its lines are
# "soname => /path (0xaddress)", "soname (0xaddress)" for the vDSO, and
# "/lib64/ld-linux-x86-64.so.2 (0x...)" for the loader; a "not found" is fatal.
LDD_OUTPUT="$(ldd "${BINARY}")"
# A here-string, not printf into grep -q: grep stops at the match, and the shell's
# printf, which writes the variable a line at a time, can die of SIGPIPE writing the
# rest, which pipefail makes the status - a library not found, and the check passed.
if grep -q 'not found' <<< "${LDD_OUTPUT}"; then
   printf '%s\n' "${LDD_OUTPUT}" >&2
   echo "The server needs a library this machine does not have." >&2
   exit 1
fi

BUNDLED=""
LEFT_TO_HOST=""
while read -r soname arrow path _; do
   [ "${arrow}" = "=>" ] || continue
   [ -n "${path}" ] && [ -f "${path}" ] || continue
   if is_excluded "${soname}"; then
      LEFT_TO_HOST="${LEFT_TO_HOST} ${soname}"
      continue
   fi
   # -L: the file the soname's symbolic link points at, under the soname.
   cp -L "${path}" "${APPDIR}/usr/lib/${soname}"
   chmod 0644 "${APPDIR}/usr/lib/${soname}"
   BUNDLED="${BUNDLED} ${soname}"
done <<< "${LDD_OUTPUT}"

# The server finds them in ../lib from wherever the image is mounted, and each of
# them finds the others beside itself. RUNPATH (patchelf writes DT_RUNPATH) is
# searched for the object's own direct dependencies, and for its dlopen calls,
# which is why each copied library carries one too.
patchelf --set-rpath '$ORIGIN/../lib' "${APPDIR}/usr/bin/hmailserver"
for soname in ${BUNDLED}; do
   patchelf --set-rpath '$ORIGIN' "${APPDIR}/usr/lib/${soname}"
done

if [ "${STRIP}" -eq 1 ]; then
   strip --strip-unneeded "${APPDIR}/usr/bin/hmailserver"
fi

echo "Bundled:${BUNDLED:- none}"
echo "Left to the host:${LEFT_TO_HOST:- none}"

# Proved rather than assumed: resolved again with no help from the environment,
# every library the image's own server needs is either one of the copies in the
# image or one the list above leaves to the host - none comes from this
# machine's /usr/lib by accident.
while read -r soname arrow path _; do
   [ "${arrow}" = "=>" ] || continue
   # The loader prints $ORIGIN expanded and unnormalised: .../usr/bin/../lib/x.
   path="$(realpath -m -- "${path}")"
   case "${path}" in
      "${APPDIR}/usr/lib/"*) ;;
      *)
         if ! is_excluded "${soname}"; then
            echo "${soname} resolves to ${path}, outside the image, and is not one the host is meant to provide." >&2
            exit 1
         fi
         ;;
   esac
done <<< "$(env -u LD_LIBRARY_PATH ldd "${APPDIR}/usr/bin/hmailserver")"

# ---------------------------------------------------------------- their licences

# Each library copied into usr/lib goes out under its own licence, and the image
# carries what those licences ask to go with the binary:
#
#   bundled/<package>/copyright   the copyright file of the distribution package
#                                 the library's file came from, as the distribution
#                                 ships it (/usr/share/doc/<package>/copyright, which
#                                 Ubuntu's container images keep when they drop the
#                                 rest of /usr/share/doc);
#   bundled/common-licenses/      every licence text those files name by its place
#                                 in /usr/share/common-licenses, as the distribution
#                                 ships it there (the GPL's and the LGPL's among
#                                 them), with the GPL-3.0 beside the LGPL-3.0, which
#                                 is written as permissions added to it and asks for
#                                 both texts (its section 4(b));
#   bundled.txt                   each library, its package and version, and the
#                                 source package and version it was built from; and
#                                 the release file with the source of every one of
#                                 them, hmailserver-<version>-appimage-bundled-sources.tar.gz,
#                                 which the appimage-bundled-sources job makes from
#                                 the archive's source packages at exactly these
#                                 versions (build/ci/packages/appimage-bundled-sources.sh).
#
# The LGPL of GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils asks
# for that source to be offered with the binary, from the same place (docs/
# PackagesAppImage.md, "Licences", says which library is under which licence).
#
# The package of each file is dpkg's answer, so this needs a Debian or Ubuntu
# machine; where there is no dpkg it says the image carries none of it.
BUNDLED_SOURCES="hmailserver-${VERSION}-appimage-bundled-sources.tar.gz"
LICENSES="${APPDIR}/usr/share/hmailserver/licenses"

# The package that owns a file ldd resolved, as <package>:<architecture>. dpkg names
# a file by the path the package ships it at, which since the /usr merge may be
# /usr/lib/... where ldd says /lib/..., or the real file behind the soname's link.
owner_of()
{
   local path="$1" real candidate owner
   real="$(readlink -f "${path}")"
   for candidate in "${path}" "${real}" "/usr${path}" "/usr${real}" "${path#/usr}" "${real#/usr}"; do
      owner="$(dpkg -S "${candidate}" 2> /dev/null | head -n 1 | sed -n 's/^\([^ ,]*\): .*/\1/p' || true)"
      if [ -n "${owner}" ]; then
         case "${owner}" in
            *:*) printf '%s\n' "${owner}" ;;
            *) printf '%s:%s\n' "${owner}" "$(dpkg --print-architecture)" ;;
         esac
         return 0
      fi
   done
   return 1
}

if ! command -v dpkg > /dev/null 2>&1 || ! command -v dpkg-query > /dev/null 2>&1; then
   echo "WARNING: no dpkg here: this image does not carry the copyright files of the libraries in its usr/lib, which" >&2
   echo "         their licences ask for, nor bundled.txt; the CI jobs pack on Ubuntu 24.04, which has it." >&2
else
   mkdir -p "${LICENSES}/bundled"
   bundled_list="${BUILD_DIR}/bundled.list"
   : > "${bundled_list}"
   for soname in ${BUNDLED}; do
      # A here-string, not printf into awk: awk exits at the match, and printf of the
      # lines after it could die of SIGPIPE, which pipefail and set -e make the script's end.
      path="$(awk -v s="${soname}" '$1 == s && $2 == "=>" { print $3; exit }' <<< "${LDD_OUTPUT}")"
      if ! owner="$(owner_of "${path}")"; then
         echo "dpkg names no package that owns ${path} (${soname}), which the image carries: its licence cannot go with it." >&2
         exit 1
      fi
      package="${owner%%:*}"
      # shellcheck disable=SC2016
      read -r version source_package source_version <<< "$(dpkg-query -W -f '${Version} ${source:Package} ${source:Version}' "${owner}" 2> /dev/null || true)"
      if [ -z "${version:-}" ] || [ -z "${source_package:-}" ] || [ -z "${source_version:-}" ]; then
         echo "dpkg gives no version, source package and source version of ${owner} (${soname})." >&2
         exit 1
      fi
      copyright="$(readlink -f "/usr/share/doc/${package}/copyright" || true)"
      if [ -z "${copyright}" ] || [ ! -f "${copyright}" ] || [ ! -s "${copyright}" ]; then
         echo "${owner} ships no /usr/share/doc/${package}/copyright here: the licence of ${soname} cannot go with it." >&2
         exit 1
      fi
      install -Dm 0644 "${copyright}" "${LICENSES}/bundled/${package}/copyright"
      printf 'library usr/lib/%s %s %s %s %s\n' "${soname}" "${owner}" "${version}" "${source_package}" "${source_version}" \
         >> "${bundled_list}"
   done

   # The names the copyright files give, "/usr/share/common-licenses/GPL-2", sometimes
   # at the end of a sentence, or several at once in the shell's braces,
   # "/usr/share/common-licenses/{MPL-1.1,GPL-2,LGPL-2.1}" (libffi8's). One this
   # machine does not have stops the build: the image would point at a text it does
   # not carry. appimage-test.sh reads them the same way.
   licence_names="$(grep -hoE '/usr/share/common-licenses/(\{[A-Za-z0-9._+,-]+\}|[A-Za-z0-9][A-Za-z0-9._+-]*)' "${LICENSES}/bundled/"*/copyright 2> /dev/null \
      | sed 's|^/usr/share/common-licenses/||; s/^{//; s/}$//' | tr ',' '\n' | sed 's/\.*$//' | grep . | LC_ALL=C sort -u || true)"
   needs_gpl3=0
   for name in ${licence_names}; do
      text="$(readlink -f "/usr/share/common-licenses/${name}" || true)"
      case "${text}" in
         /usr/share/common-licenses/*) ;;
         *) text="" ;;
      esac
      if [ -z "${text}" ] || [ ! -f "${text}" ]; then
         echo "A bundled library's copyright file names /usr/share/common-licenses/${name}, which this machine does not have." >&2
         exit 1
      fi
      install -Dm 0644 "${text}" "${LICENSES}/bundled/common-licenses/$(basename "${text}")"
      # A name that is a link there (GPL -> GPL-3, LGPL -> LGPL-3) is the same link here.
      if [ "$(basename "${text}")" != "${name}" ]; then
         ln -sfn "$(basename "${text}")" "${LICENSES}/bundled/common-licenses/${name}"
      fi
      [ "$(basename "${text}")" != "LGPL-3" ] || needs_gpl3=1
   done
   if [ "${needs_gpl3}" -eq 1 ] && [ ! -f "${LICENSES}/bundled/common-licenses/GPL-3" ]; then
      [ -f /usr/share/common-licenses/GPL-3 ] || { echo "The LGPL-3.0 goes with the GPL-3.0, and this machine has no /usr/share/common-licenses/GPL-3." >&2; exit 1; }
      install -Dm 0644 /usr/share/common-licenses/GPL-3 "${LICENSES}/bundled/common-licenses/GPL-3"
   fi

   # shellcheck source=/dev/null
   distribution="$(. /etc/os-release && echo "${ID} ${VERSION_ID}")"
   {
      echo "# The shared libraries this AppImage carries in usr/lib, each copied by build-appimage.sh from the"
      echo "# package of the distribution it was packed on that owns the file: the file, the package and its version,"
      echo "# and the source package and version it was built from (library <file> <package>:<arch> <version> <source>"
      echo "# <source version>)."
      echo "# Each package's copyright file, as the distribution ships it, is bundled/<package>/copyright beside this"
      echo "# file; a licence text one names as /usr/share/common-licenses/<name> is bundled/common-licenses/<name>."
      echo "# The source of every one of these libraries, as the distribution built it - each source package at the"
      echo "# version above, as its archive holds it - is the release file ${BUNDLED_SOURCES},"
      echo "# published with the AppImages of release v${VERSION}:"
      echo "#   https://gitlab.com/api/v4/projects/86730559/packages/generic/hmailserver/${VERSION}/${BUNDLED_SOURCES}"
      echo "# and on the release's page, ${CI_PROJECT_URL:-https://gitlab.com/hmailserver/hmailserver}/-/releases/v${VERSION},"
      echo "# as downloads/hmailserver-appimage-bundled-sources.tar.gz. Its README.txt says what each licence asks, and how"
      echo "# to run this AppImage with a changed copy of a library (section 6 of the LGPL-2.1, 4(d) of the LGPL-3.0)."
      echo "# hMailServer itself is under the AGPL-3.0-or-later: AGPL-3.0.txt, beside this file."
      echo "distribution ${distribution}"
      echo "arch ${ARCH}"
      echo "sources ${BUNDLED_SOURCES}"
      LC_ALL=C sort "${bundled_list}"
   } > "${LICENSES}/bundled.txt"
   chmod 0644 "${LICENSES}/bundled.txt"
   rm -f "${bundled_list}"
   echo "Bundled libraries' licences: $(grep -c '^library ' "${LICENSES}/bundled.txt") file(s) listed in usr/share/hmailserver/licenses/bundled.txt," \
      "$(find "${LICENSES}/bundled" -mindepth 2 -maxdepth 2 -name copyright | wc -l) copyright file(s)," \
      "$(find "${LICENSES}/bundled/common-licenses" -mindepth 1 -maxdepth 1 2> /dev/null | wc -l) licence text(s) of common-licenses"
fi

# ---------------------------------------------------------------- build it

OUTPUT_NAME="hMailServer-${VERSION}-${ARCH}.AppImage"
OUTPUT="${OUTPUT_DIR}/${OUTPUT_NAME}"
SQUASHFS="${BUILD_DIR}/AppDir.squashfs"

rm -f "${OUTPUT}" "${SQUASHFS}"

# -root-owned: every file root's, as in a package, not the builder's uid.
# zstd: what the runtime reads fastest; the runtime is linked with every
# decompressor squashfuse was built with, and appimage-runtime.sh's self-test
# packs with zstd. SOURCE_DATE_EPOCH, when the caller sets it (the CI jobs set
# the commit's time), is what mksquashfs stamps on the image and every inode, so
# the same tree packs to the same bytes.
mksquashfs "${APPDIR}" "${SQUASHFS}" -root-owned -noappend -comp zstd -no-progress -quiet

cat "${RUNTIME}" "${SQUASHFS}" > "${OUTPUT}"
chmod 0755 "${OUTPUT}"
rm -f "${SQUASHFS}"

echo
echo "${OUTPUT}"
echo
echo "Remember what this is: it runs as you, keeps its mail store under"
echo "\$HOME, cannot bind port 25, and still needs a database. Install the"
echo ".deb or the .rpm to run mail."
