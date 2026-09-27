# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The CMake toolchain the Debian and Ubuntu arm64 packages are cross-compiled
# with (build/ci/packages/deb-build.sh, .gitlab/ci/packages-deb.yml), on an
# x86-64 host, in the distribution's own amd64 container.
#
# It is Debian's multiarch cross-build, not a sysroot: the compiler is the
# distribution's own GCC for aarch64-linux-gnu (crossbuild-essential-arm64), and
# every library the server links is the distribution's own arm64 package,
# installed beside the host's with apt (libssl-dev:arm64 and the rest) under
# /usr/lib/aarch64-linux-gnu, with the architecture-dependent headers under
# /usr/include/aarch64-linux-gnu. The cross GCC searches both of those on its
# own, so no --sysroot and no CMAKE_FIND_ROOT_PATH is set. CMake is told the
# library architecture so that find_library and find_package look in the arm64
# directories first; the host's own -dev packages are not installed, so there is
# nothing of amd64's for them to find instead.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# Programs (cmake, ninja, the packagers' tools) are the host's; nothing the build
# makes is run during the build (the CMake file adds no custom command).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
