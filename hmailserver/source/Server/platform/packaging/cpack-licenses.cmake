# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The copyright file of the project's own .deb and .rpm, put where each format's
# distributions look for it. CPack runs this script once for each package it
# makes, into that package's staging tree, before the install rules
# (CPACK_INSTALL_SCRIPTS in CMakeLists.txt); `cmake --install` never runs it. So
# the .deb and the .rpm each get their own place, and a build that installs with
# CMake and packages with its own tools - the Debian source package's debhelper,
# the RPM spec's %files, the Arch PKGBUILD - gets neither and does what its
# distribution does instead.
#
# The file is the Debian source package's copyright file,
# debian-src/debian/copyright, in Debian's machine-readable format: who holds the
# copyright of what the server is built from and under which licence, with the
# AGPL-3.0-or-later in full and the terms of what is compiled into the binary -
# QuickJS, the MIME coder, CStdString, the Public Suffix List's rules and the
# rest - with what is known of the files that state no licence, tlds.txt among
# them. It is the same file the source package's own .deb carries, not a second
# one to keep in step with it.
#
#   .deb  /usr/share/doc/hmailserver/copyright, where Debian Policy 12.5 wants
#         it, and which dpkg keeps where an image's path-exclude drops the rest
#         of /usr/share/doc (Ubuntu's container images do).
#   .rpm  /usr/share/licenses/hmailserver/copyright, marked %license by
#         CPACK_RPM_USER_FILELIST as Fedora's guidelines ask: rpm -qL lists it,
#         and rpm installs it under dnf's tsflags=nodocs, which Fedora's
#         container images set.
#
# Each package owns only its own directory: /usr/share/doc is one dpkg shares
# between packages, and /usr/share/licenses is the filesystem package's, which
# CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION leaves out of the .rpm's list.
# build/ci/packages/check-licenses.sh holds both packages to this.

if(CPACK_GENERATOR STREQUAL "DEB")
   set(hm_copyright_destination /usr/share/doc/hmailserver)
elseif(CPACK_GENERATOR STREQUAL "RPM")
   set(hm_copyright_destination /usr/share/licenses/hmailserver)
else()
   # A generator with no convention of its own (TGZ, say) carries the licences
   # the install rules put under share/hmailserver/licenses, and nothing more.
   return()
endif()

# The destinations are absolute, and DESTDIR, which CPack sets to the package's
# staging tree while it runs the script, is what puts them inside it (CMake 4.2's
# CPack sets it with CPACK_SET_DESTDIR off as well). Run any other way, without
# DESTDIR, they would be this machine's own /usr/share, so the script refuses.
if("$ENV{DESTDIR}" STREQUAL "")
   message(FATAL_ERROR "cpack-licenses.cmake: DESTDIR is not set, so ${hm_copyright_destination} would be "
      "this machine's own and not a ${CPACK_GENERATOR} package's staging tree. CPack runs this script; "
      "nothing else should.")
endif()
# This runs before the install rules, so the directories it makes are the first
# in the staging tree - usr, usr/share and the rest - and the install rules find
# them made. They take CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS (0755), as the
# install rules' own do, and not the umask of whoever builds: under umask 0002
# they were 0775, and the package another one (hmailserver/docs/ReproducibleBuilds.md).
if(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS)
   set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS ${CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS})
else()
   set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
if(NOT EXISTS "${CPACK_HMAILSERVER_COPYRIGHT_FILE}")
   message(FATAL_ERROR "cpack-licenses.cmake: no copyright file at '${CPACK_HMAILSERVER_COPYRIGHT_FILE}' "
      "(CPACK_HMAILSERVER_COPYRIGHT_FILE, CMakeLists.txt): the ${CPACK_GENERATOR} package would go out without one.")
endif()
file(INSTALL "${CPACK_HMAILSERVER_COPYRIGHT_FILE}" DESTINATION "${hm_copyright_destination}"
   FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
