# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The portable build of the hMailServer core.
#
# WHAT THIS IS AND IS NOT
#
# It is not a second build of the shipping server. hMailServer.vcxproj remains
# the only thing that produces hMailServer.exe, and nothing here is compiled on
# Windows. This builds the core - Common, IMAP, POP3, SMTP, ExternalFetcher -
# on POSIX, so that the Linux and AArch64 section of the roadmap has something
# to measure and something to grow.
#
# It deliberately does NOT build:
#   Server/COM                 the ATL administration API. Windows by definition;
#                              it is what the "Administration without COM" row is
#                              about, and the core reaches it through one file.
#   Server/zlib                vendored upstream C with its own build; the system
#                              zlib is used here instead.
#   Server/hMailServer         the ATL executable and its service registration.
#                              The "service host" row replaces it with a main().
#   Server/hMailServer.Updater the live-update apply helper: it runs a Windows
#                              installer. Its counterpart here is the shell
#                              script platform/packaging/hmailserver-update,
#                              installed below with the two units that run it -
#                              unless HM_LIVE_UPDATE is OFF (below).
#   Server/hMailServer.Minidump  Windows Error Reporting.
#
# And it builds one thing the Windows project does not: Server/quickjs, the
# JavaScript engine the event scripts run in on this platform (the release has
# no MSVC support; Windows runs VBScript and JScript). See "the JavaScript
# engine" below.
#
# WINDOWS_ONLY_SOURCES below is the list of core files that wrap a Win32
# subsystem with no POSIX equivalent yet. Each one is a roadmap row, not an
# oversight, and each is named with the row it belongs to. Removing a name from
# that list is how a row gets finished.
#
# HOW TO BUILD
#   cmake -S hmailserver/source/Server -B build/linux -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/linux
#
# HOW TO MEASURE INSTEAD OF BUILD
#   build/linux-tu-census.sh compiles every core source on its own and prints how
#   many a POSIX compiler accepts. That number, not this target, is what the
#   roadmap's first Linux row asks for.

cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------- the version
#
# NOT written here. Common/Application/Version.h is the one place the number
# lives - the Windows resources, the installer and the update checker all stamp
# from it - and this file's PROJECT_VERSION becomes CPACK_PACKAGE_VERSION, which
# decides the package FILE NAMES that the GitLab CI release job
# (build/ci/release-chain.py) matches by exact equality and that the Linux
# update checker builds from a tag.
# A hand-written number here that lagged Version.h by one release produced
# packages nothing would ever find, and nothing checked it.
#
# So it is read out of the header at configure time. The other Linux stamp -
# pkgver in platform/packaging/PKGBUILD - cannot be derived this way, because
# makepkg parses that file as a shell script and never runs CMake; it is verified
# instead, by build/check-linux-version-stamp.sh, which GitLab CI's checkers
# and linux-build jobs run (.gitlab-ci.yml). CMAKE_CURRENT_LIST_DIR rather than
# CMAKE_CURRENT_SOURCE_DIR only because this runs before project().
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/Common/Application/Version.h HMAILSERVER_VERSION_DEFINE
   REGEX "^#define[ \t]+HMAILSERVER_VERSION[ \t]+\"")

string(REGEX REPLACE "^.*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$" "\\1"
   HMAILSERVER_VERSION "${HMAILSERVER_VERSION_DEFINE}")

if(NOT HMAILSERVER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
   message(FATAL_ERROR
      "Could not read HMAILSERVER_VERSION out of Common/Application/Version.h. "
      "That header is where the version of everything this repository builds comes "
      "from; the line read was: '${HMAILSERVER_VERSION_DEFINE}'.")
endif()

project(hmailserver_core
   VERSION ${HMAILSERVER_VERSION}
   DESCRIPTION "hMailServer core, portable build"
   LANGUAGES C CXX)

message(STATUS "hMailServer version ${PROJECT_VERSION}, read from Common/Application/Version.h")

if(WIN32)
   message(FATAL_ERROR
      "This CMake build is the POSIX one. On Windows the server is built from "
      "hmailserver/source/Server/hMailServer/hMailServer.sln with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Reproducibility: the same promise the Windows build makes with /Brepro and
# /pdbaltpath, by the mechanisms this toolchain has. SOURCE_DATE_EPOCH is honoured
# by the archiver and by __DATE__/__TIME__ when the environment sets it.
add_compile_options(
   -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.
   -fno-common)

# A build under AddressSanitizer and UndefinedBehaviorSanitizer, for the test
# programs that feed a decoder bytes nobody meant it to see - snmp-tests above
# all. Off, and never for a package. Every unit is instrumented, not only the
# program's, because the code under test lives in the core library; so it is a
# build directory of its own:
#
#   cmake -S hmailserver/source/Server -B build/linux-sanitize -DHM_SANITIZE=ON \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build/linux-sanitize --target snmp-tests
#   ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
#         ./build/linux-sanitize/snmp-tests
#
# An undefined behaviour stops the run rather than being printed and passed
# over, so a finding fails the job that ran it: the linux-sanitize CI job,
# which builds this directory and runs snmp-tests in it.
option(HM_SANITIZE "Build with -fsanitize=address,undefined, for the test programs" OFF)
if(HM_SANITIZE)
   add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
   add_link_options(-fsanitize=address,undefined)
endif()

# A build under clang's source-based coverage, to measure what the regression
# suite reaches: line, region and branch counts in one pass. Off, and never for a
# package - every unit is instrumented and the server is slower for it. The
# counters are written where LLVM_PROFILE_FILE says, continuously ("%c") so that a
# server stopped by SIGKILL still leaves them. build/ci/linux-coverage.sh builds
# this directory, runs the whole Linux suite against it and writes the report:
#
#   cmake -S hmailserver/source/Server -B build/linux-coverage -DHM_COVERAGE=ON \
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
option(HM_COVERAGE "Build with clang's source-based coverage, to measure the suite" OFF)
if(HM_COVERAGE)
   if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "HM_COVERAGE needs clang: -fprofile-instr-generate is clang's")
   endif()
   add_compile_options(-fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation)
   add_link_options(-fprofile-instr-generate)
endif()

# The live update (hmailserver/docs/LiveUpdateKinds.md). ON, the default and what
# the project's own packages and AppImage are built with, the server can
# download a release's package, verify it and hand it to hmailserver-update,
# which is installed below with its .path and .service units. OFF is for a
# build that something else updates - a distribution's package, a store's, a
# build of one's own: the apply step is not compiled, hmailserver-update and its
# units are not installed, nothing downloads a release file, and the update check
# still says when a newer release exists and names HM_UPDATE_COMMAND, the command
# the distribution tells its users:
#
#   cmake -S hmailserver/source/Server -B build/linux -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release -DHM_LIVE_UPDATE=OFF \
#         -DHM_UPDATE_COMMAND="apt upgrade hmailserver"
#
# A snap and a container are told apart at run time instead (SNAP, and the
# image's HM_INSTALL_KIND=container), whichever way the build was configured.
option(HM_LIVE_UPDATE "Build the live update's apply step, and install hmailserver-update and its units" ON)
set(HM_UPDATE_COMMAND "" CACHE STRING
   "With HM_LIVE_UPDATE OFF: the command that updates this build, which the update check names (for example: apt upgrade hmailserver)")
# The command is shown to administrators and compiled in as a string literal, so
# it is held to the characters a package manager's command line needs: no quote,
# backslash, dollar, semicolon or line break that the compiler's command line,
# the C string or a reader's shell could take for something else.
if(NOT HM_UPDATE_COMMAND MATCHES "^[A-Za-z0-9 ._/:=+@,~-]*$")
   message(FATAL_ERROR
      "HM_UPDATE_COMMAND may hold letters, digits, spaces and the characters '._/:=+@,~-' only; "
      "'${HM_UPDATE_COMMAND}' has another character.")
endif()
if(HM_LIVE_UPDATE AND NOT HM_UPDATE_COMMAND STREQUAL "")
   message(WARNING
      "HM_UPDATE_COMMAND is named by the update check only in a build with -DHM_LIVE_UPDATE=OFF; "
      "this build has the live update, which updates it, and the command is not used.")
endif()
if(HM_LIVE_UPDATE)
   message(STATUS "Live update: ON (the apply step, hmailserver-update and its units)")
elseif(HM_UPDATE_COMMAND STREQUAL "")
   message(STATUS "Live update: OFF; the update check names no command (HM_UPDATE_COMMAND is empty)")
else()
   message(STATUS "Live update: OFF; the update check names: ${HM_UPDATE_COMMAND}")
endif()

# ---------------------------------------------------------------- the sources

set(SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
   ${SERVER_DIR}/Common/*.cpp
   ${SERVER_DIR}/IMAP/*.cpp
   ${SERVER_DIR}/POP3/*.cpp
   ${SERVER_DIR}/SMTP/*.cpp
   ${SERVER_DIR}/ExternalFetcher/*.cpp)

# Each of these wraps a Windows subsystem the POSIX build does not have. The
# roadmap row that would replace it is named beside it; when one is written,
# delete the line.
#
# A name may only be here if NOTHING THIS BUILD COMPILES REFERENCES IT. Removing
# a translation unit does not remove the calls to it, and a file left out because
# it "looks like Windows" takes its symbols with it - which is why the four that
# used to be on this list are not any more: DNSResolverWinApi, DataProtector and
# SSPIValidation are each called by name from code that does build (DNSResolver,
# Crypt, PasswordValidator), and ScriptServer is called from twenty places. They
# have POSIX arms of their own instead. Every name below was checked the same way
# before it was left here.
#
# The seven SQL files are a milder case: each is wholly inside
# "#ifndef HM_PLATFORM_POSIX" and so compiles here to an empty translation unit.
# Leaving them out costs nothing and says plainly that the backends are deferred;
# DALConnectionFactory refuses those two backends by name on this platform, so
# nothing reaches them.
set(WINDOWS_ONLY_SOURCES
   Common/SQL/ADOConnection.cpp                 # "Database backends that survive"
   Common/SQL/ADORecordset.cpp                  # "Database backends that survive"
   Common/SQL/ADOInt64Helper.cpp                # "Database backends that survive"
   Common/SQL/SQLCEConnection.cpp               # "Database backends that survive"
   Common/SQL/SQLCERecordset.cpp                # "Database backends that survive"
   Common/SQL/Macros/MSSQLMacroExpander.cpp     # "Database backends that survive"
   Common/SQL/Macros/SQLCEMacroExpander.cpp     # "Database backends that survive"
   Common/Util/ServiceManager.cpp               # "The service host": systemd owns this
   Common/Util/Registry.cpp                     # the registry; the INI is the POSIX answer
)

# A file in the tree that no build compiles - not this one and not
# hMailServer.vcxproj, which is the only thing that produces the shipping server -
# is deleted rather than listed. Eleven such sources were listed here until 6.3.6
# (LoggerFile, ServerThreads, Collection.cpp, AccountCache, Cache.cpp,
# CacheReaderWithDbFallback.cpp, OutboundPortConnection, TestConnectionResult.cpp,
# Mutex, ISMTPCommand and SMTPCommandHelp) are gone, and with them the seven headers
# of theirs that nothing that builds includes, IThread.h, and COM/InterfaceDKIM,
# which no build compiled either. Collection.h, Cache.h, CacheReaderWithDbFallback.h
# and TestConnectionResult.h stay - they are used - and their .cpp files, which held
# nothing, went. The rule this build follows is simply that it compiles the set the
# Windows project compiles, less the Windows-only rows above.

foreach(relative IN LISTS WINDOWS_ONLY_SOURCES)
   list(REMOVE_ITEM CORE_SOURCES ${SERVER_DIR}/${relative})
endforeach()

list(LENGTH CORE_SOURCES CORE_SOURCE_COUNT)
message(STATUS "hMailServer core: ${CORE_SOURCE_COUNT} translation units offered to this build")

# ---------------------------------------------------------------- dependencies

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(ZLIB REQUIRED)
# Boost.System is not asked for as a component, and Boost::headers is linked
# below in place of Boost::system. It has been header-only since 1.69 and 1.90
# removed the compiled library altogether, so asking for the component makes a
# build against a current Boost fail at CONFIGURE time - "Could not find a
# package configuration file provided by boost_system" - before a single source
# is compiled, and naming the target makes it fail at generate time.
# Boost.Chrono is a library, not headers, in 1.83 - the Boost of Ubuntu 24.04 and
# Debian 12 - and Boost.Thread reaches it for every sleep_for and steady_clock
# the server calls. Against 1.90 it links without being named, which is how the
# first CI build came to fail at the link step on a runner and not on the
# machine the port was written on.
# Boost is linked STATICALLY. A Boost library's soname carries its version
# (libboost_thread.so.1.83.0), so a package built against the Boost of one
# distribution release installs on that release and no other: the 6.3.1 .deb,
# built on Ubuntu 24.04, was refused by Ubuntu 26.04 for five Boost sonames and
# nothing else (11 September 2026). With the archives linked in, dpkg-shlibdeps
# and rpmbuild's find-requires see no Boost at all and the package depends on
# what every release ships under a stable soname: libssl3, libpq5, libldap2,
# libstdc++6. The -dev packages carry the archives on Debian, Ubuntu and Fedora;
# the AppImage is unaffected (it bundled the shared libraries and now has none
# to bundle).
#
# Boost.Regex is not linked at all: it has been header-only since 1.76, and
# Debian's compiled archive is built with ICU support, so linking it statically
# drags in libicu - whose soname also changes with every release (libicu74 on
# Ubuntu 24.04, libicu76 on 26.04). The first static-Boost package proved that:
# no Boost in its Depends, and libicu74 still there. The headers alone give
# the same boost::regex, and nothing here includes boost/regex/icu.hpp.
#
# HM_BOOST_STATIC OFF links the distribution's shared Boost instead, which is what
# a distribution's own package does: its archive rebuilds the package whenever its
# Boost moves, so the soname it names is always one the release carries, and its
# policy asks for shared libraries over embedded copies. The source packages under
# platform/packaging/debian-src and platform/packaging/rpm build so
# (hmailserver/docs/SourcePackages.md). The project's own packages keep ON, for the
# reason above. A variable of its own rather than Boost_USE_STATIC_LIBS on the
# command line, because the set() below would hide a cache value of that name.
option(HM_BOOST_STATIC "Link Boost statically (the project's own packages); OFF links the distribution's shared Boost" ON)
set(Boost_USE_STATIC_LIBS ${HM_BOOST_STATIC})
find_package(Boost 1.83 REQUIRED COMPONENTS thread chrono filesystem)
if(HM_BOOST_STATIC)
   message(STATUS "Boost ${Boost_VERSION}: linked statically")
else()
   message(STATUS "Boost ${Boost_VERSION}: the shared libraries, linked dynamically (HM_BOOST_STATIC OFF)")
endif()

# libpq. PGConnection.h and PGRecordset.h include <libpq-fe.h> by that bare name,
# the way the Windows project reaches it through an include directory, and Debian
# and Ubuntu put the header under /usr/include/postgresql where no compiler looks
# by default. It is REQUIRED because six sources in Common/SQL name it and none of
# them is on the Windows-only list: PostgreSQL is a backend this build has, not
# one it defers. MySQL needs no such package - that client is loaded at run time
# with dlopen and the tree declares its own entry points.
find_package(PostgreSQL REQUIRED)

# iconv. Unicode::FromCodePage and Unicode::ToCodePage are what
# MultiByteToWideChar and WideCharToMultiByte are on this platform, and iconv is
# how they convert - which every MIME header in a character set other than the
# message's own goes through. On glibc it is part of the C library and this finds
# nothing to link; elsewhere (musl with libiconv, the BSDs, macOS) it is a
# separate library, and the imported target below is what makes that difference
# invisible here.
find_package(Iconv REQUIRED)

# And what that iconv converts. The server hands it the character sets of the
# code page table (Common/Util/Unicode.cpp, IconvNameForCodePage), and not every
# C library's iconv has them all: musl's - Alpine's - has no UTF-7, which IMAP
# folder names are written in (modified UTF-7, RFC 3501), so a server built on it
# gives every folder name outside ASCII as nothing, and says so nowhere (musl
# 1.2.6 on Alpine 3.24, 29 September 2026; hmailserver/docs/Alpine.md, "What musl
# changed"). So the build asks the iconv it found for UTF-7 and US-ASCII, both
# ways, and refuses one without them, naming GNU libiconv, which has every
# character set the table names; the Alpine package builds with it
# (platform/packaging/alpine/APKBUILD). musl's has no ISO-2022-JP-2 either, and
# writes no BIG5. A glibc has UTF-7 and US-ASCII everywhere; Fedora's, EL's and
# openSUSE's keep BIG5, ISO-2022-JP and most code pages in a package of their own,
# which the .rpm asks for (CPACK_RPM_PACKAGE_REQUIRES below). That is a question
# for the machine the server runs on, not the one that built it, so it is not
# asked here: hmailserver --check-config and every start say what the iconv the
# server runs with cannot convert (platform/main.cpp).
# Not asked when cross-compiling, where the program cannot run.
if(NOT CMAKE_CROSSCOMPILING)
   include(CheckCSourceRuns)
   # Asked again at every configure, so that an iconv changed on the command line
   # is the one answered for, not the answer cached for the one before.
   unset(HM_ICONV_CONVERTS_UTF7 CACHE)
   set(CMAKE_REQUIRED_LIBRARIES Iconv::Iconv)
   check_c_source_runs([=[
#include <iconv.h>
int main(void)
{
   static const char *names[] = { "UTF-7", "US-ASCII" };
   for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
   {
      iconv_t to = iconv_open(names[i], "WCHAR_T");
      iconv_t from = iconv_open("WCHAR_T", names[i]);
      if (to == (iconv_t) -1 || from == (iconv_t) -1)
         return 1;
      iconv_close(to);
      iconv_close(from);
   }
   return 0;
}
]=] HM_ICONV_CONVERTS_UTF7)
   unset(CMAKE_REQUIRED_LIBRARIES)
   if(NOT HM_ICONV_CONVERTS_UTF7)
      message(FATAL_ERROR
         "This C library's iconv does not convert UTF-7, in which IMAP folder names are written, "
         "or US-ASCII: the server would lose every folder name outside ASCII. musl's is such an iconv. "
         "Build against GNU libiconv instead (Alpine: apk add gnu-libiconv-dev, then configure with "
         "-DIconv_IS_BUILT_IN=OFF -DIconv_INCLUDE_DIR=/usr/include/gnu-libiconv "
         "-DIconv_LIBRARY=/usr/lib/libiconv.so).")
   endif()
endif()

# OpenLDAP. LdapClient.cpp is written against wldap32 on Windows and against
# libldap here - the same function names over a different library, which is why
# that file has two arms - and directory authentication on this platform is that
# client or nothing, so it is REQUIRED. Found with find_path and find_library
# rather than a package module because CMake ships none for it, and Debian's
# libldap-dev, Fedora's openldap-devel and Arch's libldap all put ldap.h and the
# libraries where the default search looks. liblber is named as well as libldap:
# the BER functions the client calls by name (ber_memfree) are exported from it,
# and a link line that relies on libldap pulling it in is one that breaks under
# --as-needed.
find_path(LDAP_INCLUDE_DIR NAMES ldap.h)
find_library(LDAP_LIBRARY NAMES ldap)
find_library(LBER_LIBRARY NAMES lber)

if(NOT LDAP_INCLUDE_DIR OR NOT LDAP_LIBRARY OR NOT LBER_LIBRARY)
   message(FATAL_ERROR
      "OpenLDAP was not found (ldap.h, libldap and liblber are all needed). "
      "Directory authentication on this platform is built on it. Install "
      "libldap-dev (Debian, Ubuntu), openldap-devel (Fedora, RHEL) or libldap "
      "(Arch) and configure again.")
endif()

# ---------------------------------------------------------------- the JavaScript engine
#
# QuickJS, Fabrice Bellard's release 2026-06-04 (MIT), vendored as source under
# Server/quickjs: the engine, its regular expression and Unicode libraries,
# cutils and dtoa, byte for byte as released, with its LICENSE and VERSION. The
# event scripts run in it on this platform (Common/Scripting/ScriptBinding.cpp,
# hmailserver/docs/EventScripts.md). quickjs-libc.c - QuickJS's std and os
# modules, files and processes and sockets - is not in the tree at all.
#
# A static library of its own, in C, for two reasons: hmailserver_core forces
# portable_stdafx.h into every file it compiles, which is C++ and must not reach
# a C file, and upstream's code is built with upstream's flags. Those are the
# ones its Makefile gives the library's objects - _GNU_SOURCE, CONFIG_VERSION
# from VERSION, and -fwrapv, which it passes so that signed overflow wraps as the
# engine's arithmetic expects. The Makefile's warning switches are for its own
# -Wall, which this build passes to nothing; HAVE_CLOSEFROM, the other definition
# it can add, is read only by quickjs-libc.c. No C standard is named, as the
# Makefile names none. The engine takes pthread for Atomics and libm for Math.
file(READ ${SERVER_DIR}/quickjs/VERSION QUICKJS_VERSION)
string(STRIP "${QUICKJS_VERSION}" QUICKJS_VERSION)

add_library(quickjs STATIC
   ${SERVER_DIR}/quickjs/quickjs.c
   ${SERVER_DIR}/quickjs/libregexp.c
   ${SERVER_DIR}/quickjs/libunicode.c
   ${SERVER_DIR}/quickjs/cutils.c
   ${SERVER_DIR}/quickjs/dtoa.c)

target_compile_definitions(quickjs PRIVATE
   _GNU_SOURCE
   "CONFIG_VERSION=\"${QUICKJS_VERSION}\"")

target_compile_options(quickjs PRIVATE -fwrapv)

target_link_libraries(quickjs PRIVATE Threads::Threads m)

message(STATUS "QuickJS ${QUICKJS_VERSION}, read from quickjs/VERSION")

# ---------------------------------------------------------------- the target

add_library(hmailserver_core STATIC ${CORE_SOURCES})

# Both spellings of the precompiled header have to resolve on a case-sensitive
# filesystem. Only StdAfx.h is in the repository - a second file differing by
# case cannot be checked out on Windows - so the lowercase one is generated here.
set(PCH_LOWER ${CMAKE_CURRENT_BINARY_DIR}/pch-lower)
file(MAKE_DIRECTORY ${PCH_LOWER})
configure_file(${SERVER_DIR}/platform/pch/StdAfx.h ${PCH_LOWER}/stdafx.h COPYONLY)

target_include_directories(hmailserver_core PUBLIC
   ${SERVER_DIR}/platform/pch
   ${PCH_LOWER}
   ${SERVER_DIR}
   ${SERVER_DIR}/Common
   ${SERVER_DIR}/Common/Util
   ${LDAP_INCLUDE_DIR})

target_compile_definitions(hmailserver_core PUBLIC
   HM_PLATFORM_POSIX=1
   UNICODE
   _UNICODE
   BOOST_BIND_GLOBAL_PLACEHOLDERS)

# The live update, above: always stated, 1 or 0, so that no unit of this build
# falls back on the default Common/Util/UpdateChecker.h gives the Windows project.
# PUBLIC, so that every program linked against the core sees what it was built as.
if(HM_LIVE_UPDATE)
   target_compile_definitions(hmailserver_core PUBLIC HM_LIVE_UPDATE=1)
else()
   target_compile_definitions(hmailserver_core PUBLIC
      HM_LIVE_UPDATE=0
      "HM_UPDATE_COMMAND=\"${HM_UPDATE_COMMAND}\"")
endif()

# The precompiled-header substitute, forced into every translation unit exactly
# as the MSVC build forces its own.
target_compile_options(hmailserver_core PUBLIC
   -include ${SERVER_DIR}/platform/portable_stdafx.h)

# clang and GCC are both first-class here. An earlier version of this file passed
# clang -fdelayed-template-parsing and warned that GCC would fail in
# Common/Util/StdString.h; measured on 8 September 2026, every core file compiles
# under clang without the flag and under GCC 13 and 15, and the CI matrix builds
# with both.

target_link_libraries(hmailserver_core PUBLIC
   Threads::Threads
   OpenSSL::SSL
   OpenSSL::Crypto
   ZLIB::ZLIB
   PostgreSQL::PostgreSQL
   Iconv::Iconv
   ${LDAP_LIBRARY}
   ${LBER_LIBRARY}
   Boost::headers
   Boost::thread
   Boost::chrono
   Boost::filesystem
   ${CMAKE_DL_LIBS})

# The JavaScript engine, above. PRIVATE because nothing outside the core names
# it: ScriptBinding.cpp includes quickjs/quickjs.h by its path from the source
# tree, so the engine's own directory - where a list.h and a cutils.h live - is
# on nobody's include path. A static library's private dependency still reaches
# every program linked against the core.
target_link_libraries(hmailserver_core PRIVATE quickjs)

# ---------------------------------------------------------------- the service

# The executable a package installs and systemd starts. platform/main.cpp is the
# whole of it: the Windows service host is 500 lines of COM registration and SCM
# callbacks, none of which has a meaning here.
# platform/root_command.cpp beside it: how a command run as root runs, its own
# unit so that root-command-tests, below, holds it without a server.
add_executable(hmailserver ${SERVER_DIR}/platform/main.cpp ${SERVER_DIR}/platform/root_command.cpp)
target_link_libraries(hmailserver PRIVATE hmailserver_core)
set_target_properties(hmailserver PROPERTIES OUTPUT_NAME hmailserver)

# ---------------------------------------------------------------- the SPF tests
#
# The SPF module's four testers as a program of their own: the openspf.org
# conformance suite for RFC 7208, all 203 of its cases, and the three testers that
# cover what the corpus does not - the grammar, macro expansion, and the corners of
# section 4 no case visits.
#
# It exists because the Windows route to them does not exist here. There they run
# inside hMailServer.exe: ClassTester::DoTests calls SPFTester::Test and the
# regression suite reaches it through the COM method Utilities.RunTestSuite, which
# the Linux suite's shim skips because there is no COM to reach it by. So the same
# testers are linked into a binary instead, and GitLab CI's linux-build job
# runs it (build/ci/linux-build.sh) - on x86-64 under clang; the GitHub
# workflow linux-build.yml ran it on AArch64 and under GCC as well until GitHub
# suspended the project's account on 18 September 2026.
#
# Not installed, and deliberately not part of the packages: it is a bench tool.
# Nothing here touches the network or reads the configuration - every tester
# answers the evaluation's DNS queries out of a table - so the run is milliseconds
# and depends on nothing outside this repository.
add_executable(spf-tests ${SERVER_DIR}/platform/spf_tests_main.cpp)
target_link_libraries(spf-tests PRIVATE hmailserver_core)
set_target_properties(spf-tests PROPERTIES OUTPUT_NAME spf-tests)

# ---------------------------------------------------------------- the event-script binding's tests
#
# ScriptBindingTester (Common/Scripting/ScriptBinding.h) as a program of its own,
# for the reason spf-tests is one: nothing inside the server runs it on this
# platform - ClassTester is reached only through COM and the Windows /Test switch -
# and the Linux regression suite does not run it either, so this program, which
# linux-build.yml runs and anybody can run from the build directory, is the only
# thing that does. It proves the JavaScript
# binding's members against the contract EventScripts.md lists, values both ways,
# and the engine's limits - the timeout, the memory ceiling, no std or os module.
# No database, no configuration, no network. Not installed; a bench tool.
add_executable(script-binding-tests ${SERVER_DIR}/platform/script_binding_tests_main.cpp)
target_link_libraries(script-binding-tests PRIVATE hmailserver_core)
set_target_properties(script-binding-tests PROPERTIES OUTPUT_NAME script-binding-tests)

# ---------------------------------------------------------------- the SNMP agent's tests
#
# SnmpTester (Common/Util/SnmpTester.h) as a program of its own, for the reason
# spf-tests is one: on Windows it runs inside hMailServer.exe through
# ClassTester, which is reached only through COM, so here this program is what
# runs it. The agent's own units (Common/Util/Snmp*.cpp) are in the core by the
# glob above, like every other unit; this adds only the program. It feeds the
# BER decoder and the SNMPv3 engine requests it built and packets it broke -
# truncated at every length and one element at a time inside true lengths,
# malformed in each way the decoder refuses, nested too deep, too long, turned
# over byte by byte - and checks the USM cryptography: AES-128-CFB against NIST
# SP 800-38A, HMAC-SHA-256-192 and the SHA-256 key localisation against values
# computed independently. No database, no configuration, no socket, no network.
# Not installed; a bench tool. Built under HM_SANITIZE (above), it is the
# program the linux-sanitize CI job runs.
add_executable(snmp-tests ${SERVER_DIR}/platform/snmp_tests_main.cpp)
target_link_libraries(snmp-tests PRIVATE hmailserver_core)
set_target_properties(snmp-tests PROPERTIES OUTPUT_NAME snmp-tests)

# ---------------------------------------------------------------- the backup index's XML
#
# XmlTreeTester (Common/Util/XmlTreeTester.h) as a program of its own, for the
# reason spf-tests is one: on Windows it runs inside hMailServer.exe through
# ClassTester, which is reached only through COM, so here this program is what
# runs it - and here wchar_t is 32 bits, so it is also the only run that writes
# and reads back every code point rather than every UTF-16 unit. The tree's own
# unit (Common/Util/XmlTree.cpp) is in the core by the glob above. No database,
# no configuration, no file, no network. Not installed; a bench tool.
add_executable(xml-tree-tests ${SERVER_DIR}/platform/xml_tree_tests_main.cpp)
target_link_libraries(xml-tree-tests PRIVATE hmailserver_core)
set_target_properties(xml-tree-tests PROPERTIES OUTPUT_NAME xml-tree-tests)

# ---------------------------------------------------------------- the Control Deck's script hash
#
# InlineScriptHashTester (Common/Util/InlineScriptHash.h) as a program of its
# own, for the reason spf-tests is one: on Windows it runs inside the server
# through ClassTester, which nothing reaches here. The Deck is served under a
# script-src that is the SHA-256 of its one inline script; this holds the hash
# to the text a browser hashes - CR LF and a lone CR made LF, the script found
# as the HTML tokenizer finds it - against values another implementation
# computed, and to 'none' for a page with no single inline script. No
# database, configuration, file or network. Given a page's path, it prints the
# script-src the server sends that page under instead. Not installed; a bench
# tool.
add_executable(inline-script-hash-tests ${SERVER_DIR}/platform/inline_script_hash_tests_main.cpp)
target_link_libraries(inline-script-hash-tests PRIVATE hmailserver_core)
set_target_properties(inline-script-hash-tests PROPERTIES OUTPUT_NAME inline-script-hash-tests)

# ---------------------------------------------------------------- the account signature's HTML
#
# SignatureHtmlSanitizerTester (Common/Util/SignatureHtmlSanitizer.h) as a
# program of its own, for the reason spf-tests is one: on Windows it runs inside
# the server through ClassTester, which nothing reaches here. An account's
# formatted signature is reduced on the server to the subset the webmail's
# cleaner admits; this holds the result to that cleaner's - the elements and
# attributes kept and dropped, a dangerous element dropped whole, the
# http(s)/mailto and data: image rules, the fixed point on the page's own output
# and idempotence. No database, configuration, file or network. Not installed; a
# bench tool.
add_executable(signature-html-sanitizer-tests ${SERVER_DIR}/platform/signature_html_sanitizer_tests_main.cpp)
target_link_libraries(signature-html-sanitizer-tests PRIVATE hmailserver_core)
set_target_properties(signature-html-sanitizer-tests PROPERTIES OUTPUT_NAME signature-html-sanitizer-tests)

# ---------------------------------------------------------------- the DEFLATE streams
#
# DeflateStreamsTester (Common/TCPIP/DeflateStreams.h) as a program of its own,
# for the reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. IMAP's COMPRESS=DEFLATE inflates what
# a client sends through it; this holds it to a round trip, to a stream of
# command lines hundreds of times its compressed size coming out in pieces no
# larger than the limit the connection asks for and whole when put together -
# what zlib still holds once the input is all taken included - to a limit of
# nothing, and to a corrupt stream refused. No database, configuration, file or
# network. Not installed; a bench tool.
add_executable(deflate-tests ${SERVER_DIR}/platform/deflate_tests_main.cpp)
target_link_libraries(deflate-tests PRIVATE hmailserver_core)
set_target_properties(deflate-tests PROPERTIES OUTPUT_NAME deflate-tests)

# ---------------------------------------------------------------- DANE
#
# DaneTester (SMTP/TlsPolicy.h) as a program of its own, for the reason spf-tests
# is one: on Windows it runs inside the server through ClassTester, which nothing
# reaches here. Which TLSA records are usable, what a DNSSEC-validated RRset of
# none of them means (TLS required without authentication, RFC 7672 section
# 2.2), and TLS handshakes over a BIO pair against the distribution's OpenSSL:
# DANE-TA authenticating a chain that carries its anchor and refusing another
# anchor, a missing one, another name and an expired certificate, DANE-EE
# authenticating its key alone. No database, configuration, file or network. Not
# installed; a bench tool.
add_executable(dane-tests ${SERVER_DIR}/platform/dane_tests_main.cpp)
target_link_libraries(dane-tests PRIVATE hmailserver_core)
set_target_properties(dane-tests PROPERTIES OUTPUT_NAME dane-tests)

# ---------------------------------------------------------------- the Public Suffix List's cut
#
# PublicSuffixListTester (Common/AntiSpam/DMARC/PublicSuffixList.h) as a program
# of its own, for the reason spf-tests is one: on Windows it runs inside the
# server through ClassTester, which nothing reaches here. The name the SURBL
# lookups ask about and the MX-record spam test falls back to, cut from a host by
# the compiled-in list: wildcard and exception rules, PRIVATE names, suffixes,
# addresses, and DMARC's organizational domain the same name. With it
# SURBLTester (Common/AntiSpam/SURBL.h), the links the cut is handed: the host
# after a link's userinfo, ports, IPv6, percent-encoding, the forms of an IPv4
# address, HTML's character references, and the lists' error codes. And
# FqdnCheckTester (Common/AntiSpam/FqdnCheck.h), a HELO name or a sender's domain
# that is not a fully qualified domain name by the same list, and the carve-outs
# of the refusal made with it. No network, no configuration, no database. Not
# installed; a bench tool.
add_executable(public-suffix-tests ${SERVER_DIR}/platform/public_suffix_tests_main.cpp)
target_link_libraries(public-suffix-tests PRIVATE hmailserver_core)
set_target_properties(public-suffix-tests PROPERTIES OUTPUT_NAME public-suffix-tests)

# ---------------------------------------------------------------- the never-ban list by reverse name
#
# NeverBanNameListTester (Common/Util/NeverBanNameList.h, GitLab work item #56)
# as a program of its own, for the reason public-suffix-tests is one: on Windows
# it runs inside the server through ClassTester, which nothing reaches here. The
# patterns the list holds and refuses, the reverse names it may match, the
# forward confirmation of a covered name in both address families, and the
# verdicts it keeps and forgets. No network - the tester answers its own forward
# lookups - no configuration, no database. Not installed; a bench tool.
add_executable(never-ban-tests ${SERVER_DIR}/platform/never_ban_tests_main.cpp)
target_link_libraries(never-ban-tests PRIVATE hmailserver_core)
set_target_properties(never-ban-tests PROPERTIES OUTPUT_NAME never-ban-tests)

# ---------------------------------------------------------------- an auto-ban widened to its neighbours
#
# AutoBanWidenerTester (Common/Util/AutoBanWidener.h, GitLab work item #55) as a
# program of its own, for the same reason: the run of a shared registrable domain
# inside one /24, each direction's stops, the banned address's own reverse name
# forward-confirmed, a lookup not answered leaving the single address, and the
# shapes the firewall mirrors. No network - the tester answers its own lookups -
# no configuration, no database. Not installed; a bench tool.
add_executable(autoban-widen-tests ${SERVER_DIR}/platform/autoban_widen_tests_main.cpp)
target_link_libraries(autoban-widen-tests PRIVATE hmailserver_core)
set_target_properties(autoban-widen-tests PROPERTIES OUTPUT_NAME autoban-widen-tests)

# ---------------------------------------------------------------- the sign-in tables' room
#
# SignInBudgetTester (Common/Util/SignInBudget.h) as a program of its own, for
# the reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. Who may push whom out of a full table
# of browser sessions or passkey registrations - a holder at its cap gives up its
# own oldest, a full table the oldest of the holder with the most - and the
# source a guess of the administrator password counts against: the address, an
# IPv6 one as its /64, a NAT64 or Teredo client as its IPv4 address; and
# PasskeyStoreTester and SignInStateTester, the challenges and the sealed state
# the sign-ins that begin with no credential carry. No network, configuration or
# database. Not installed; a bench tool.
add_executable(sign-in-budget-tests ${SERVER_DIR}/platform/sign_in_budget_tests_main.cpp)
target_link_libraries(sign-in-budget-tests PRIVATE hmailserver_core)
set_target_properties(sign-in-budget-tests PROPERTIES OUTPUT_NAME sign-in-budget-tests)

# ---------------------------------------------------------------- the types files and photos go out under
#
# InertMediaTypeTester (Common/Util/InertMediaType.h) as a program of its own,
# for the reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. The type a file sent as a link and a
# contact's photo are served under: a type on the list of those no browser runs,
# applies or renders as a page, or application/octet-stream - for every
# JavaScript name, the types a browser applies or renders, a list of types,
# whitespace or a control character in a type, and a parameter other than a
# charset. No network, configuration or database. Not installed; a bench tool.
add_executable(media-type-tests ${SERVER_DIR}/platform/media_type_tests_main.cpp)
target_link_libraries(media-type-tests PRIVATE hmailserver_core)
set_target_properties(media-type-tests PROPERTIES OUTPUT_NAME media-type-tests)

# ---------------------------------------------------------------- mailbox encryption's cryptography
#
# MailboxCrypto::Test (Common/Util/MailboxCrypto.h) as a program of its own, for
# the reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here, and until 6.3.6 nothing ran it on
# Linux. X25519 and HKDF against their RFCs' vectors, every seal, wrap and stage
# with its refusals, and ForgetOnExit - a private key written over on every way
# out of its scope, an exception among them. No network, configuration or
# database. Not installed; a bench tool.
add_executable(mailbox-crypto-tests ${SERVER_DIR}/platform/mailbox_crypto_tests_main.cpp)
target_link_libraries(mailbox-crypto-tests PRIVATE hmailserver_core)
set_target_properties(mailbox-crypto-tests PROPERTIES OUTPUT_NAME mailbox-crypto-tests)

# ---------------------------------------------------------------- the OpenPGP writer
#
# The one OpenPGP message this server writes - a delivery to a zero-access
# mailbox, encrypted to the account's own key (Common/Util/OpenPgpWriter.h) -
# as a command that takes a public key and a file and prints the message. It
# exists so that the writer can be proved against an implementation that is
# not this one: linux-build.yml runs it against the fixture keys GnuPG made and
# hands the output to gpg --decrypt on the runner, which is the only honest
# proof that a message written here opens elsewhere. Not installed; a bench tool.
add_executable(openpgp-writer ${SERVER_DIR}/platform/openpgp_writer_main.cpp)
target_link_libraries(openpgp-writer PRIVATE hmailserver_core)
set_target_properties(openpgp-writer PROPERTIES OUTPUT_NAME openpgp-writer)

# ---------------------------------------------------------------- the object store's signatures
#
# ObjectStoreSignatureTester (Common/Util/ObjectStoreSignature.h) as a program of
# its own, for the reason spf-tests is one: on Windows it runs inside the server
# through ClassTester, which nothing reaches here. AWS Signature Version 4 against
# AWS's published test suite and the S3 examples, and Azure's Shared Key; no
# network, no configuration, no database. Not installed; a bench tool.
add_executable(object-store-signature-tests ${SERVER_DIR}/platform/object_store_signature_tests_main.cpp)
target_link_libraries(object-store-signature-tests PRIVATE hmailserver_core)
set_target_properties(object-store-signature-tests PROPERTIES OUTPUT_NAME object-store-signature-tests)

# ---------------------------------------------------------------- the legacy Blowfish scheme
#
# LegacyBlowfishTester (Common/Util/LegacyBlowfish.h) as a program of its own, for
# the reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. Blowfish against Eric Young's published
# test vectors, and the scheme of stored secrets against values 6.3.4 wrote and
# read - here, where the cipher's 32-bit words meet LP64. No network, no
# configuration, no database. Not installed; a bench tool.
add_executable(legacy-blowfish-tests ${SERVER_DIR}/platform/legacy_blowfish_tests_main.cpp)
target_link_libraries(legacy-blowfish-tests PRIVATE hmailserver_core)
set_target_properties(legacy-blowfish-tests PROPERTIES OUTPUT_NAME legacy-blowfish-tests)

# ---------------------------------------------------------------- iMIP on delivery
#
# ImipTester (Common/Util/Imip.h) as a program of its own, for the reason
# spf-tests is one: on Windows it runs inside the server through ClassTester,
# which nothing reaches here. The From a header names and DMARC for exactly its
# domain - what vouches that mail from outside was sent by an address before
# its REPLY, CANCEL or updated REQUEST changes a stored event - and what a REPLY
# or a CANCEL changes given who sent it, with the cases that must not change or
# vouch for anything. No network, configuration or database. Not installed; a
# bench tool.
add_executable(imip-tests ${SERVER_DIR}/platform/imip_tests_main.cpp)
target_link_libraries(imip-tests PRIVATE hmailserver_core)
set_target_properties(imip-tests PROPERTIES OUTPUT_NAME imip-tests)

# ---------------------------------------------------------------- the greylisting key
#
# PreReqGreyListKeysTester (Common/SQL/Prerequisites/PreReqGreyListKeys.h) as a
# program of its own, for the reason spf-tests is one: on Windows it runs inside
# the server through ClassTester, which nothing reaches here. Schema 6107's
# cutover: the key every row is given is the one new rows are written with, a
# row cut to the old columns' 200 characters is deleted and every other keyed,
# the copy of a triplet kept, and nothing but a key in the statement that writes
# them. No network, configuration or database (build/db-upgrade-gate.sh runs the
# pass against PostgreSQL and MariaDB). Not installed; a bench tool.
add_executable(greylist-keys-tests ${SERVER_DIR}/platform/greylist_keys_tests_main.cpp)
target_link_libraries(greylist-keys-tests PRIVATE hmailserver_core)
set_target_properties(greylist-keys-tests PROPERTIES OUTPUT_NAME greylist-keys-tests)

# ---------------------------------------------------------------- the singletons' first call
#
# Singleton<T>::Instance (Common/Util/Singleton.h) with eight threads asking for
# a fresh singleton together, sixty-four times: one T made each time, and every
# caller given it whole. As it compiles, it also holds that nothing but T can make
# a Singleton<T> or a BusinessObject<T> (Common/BO/BusinessObject.h). The headers
# are the whole of what it tests, so nothing of the core is linked and the program
# builds in a second; linux-build.sh runs it with the other *-tests programs. The
# Windows build compiles the same headers with MSVC, whose function-local statics
# are as thread-safe as these compilers'. Not installed; a bench tool.
add_executable(singleton-tests ${SERVER_DIR}/platform/singleton_tests_main.cpp)
target_link_libraries(singleton-tests PRIVATE Threads::Threads)
set_target_properties(singleton-tests PROPERTIES OUTPUT_NAME singleton-tests)

# ---------------------------------------------------------------- the date class
#
# DateTimeTests (Common/Util/DateTime.cpp) and TimeTester (Common/Util/Time.cpp) as
# a program of their own, for the reason spf-tests is one: on Windows they run
# inside the server through ClassTester, which nothing reaches here. The date
# class is the project's own since 6.3.6, written to the behaviour of the one it
# replaced; these hold it to known dates, and the MIME, IMAP and system-date
# parsers to their examples. No clock, time zone, network or configuration.
# Not installed; a bench tool.
add_executable(datetime-tests ${SERVER_DIR}/platform/datetime_tests_main.cpp)
target_link_libraries(datetime-tests PRIVATE hmailserver_core)
set_target_properties(datetime-tests PROPERTIES OUTPUT_NAME datetime-tests)

# ---------------------------------------------------------------- the configuration file's owner
#
# hMailServer.INI rewritten by the profile API of the POSIX build
# (Common/Util/IniFile.cpp), which nothing on Windows runs: a program of its own,
# as backup-tests is one. The replacement keeps the owner and mode of the file it
# replaces, and when that file is gone between the read and the write - removed,
# or made again by another user - it is the writer's own 0600 or the new file's,
# never an owner nobody chose; with a race against another user's process doing
# exactly that. Run as root, as linux-build.sh runs it in the CI image; the cases
# that need root say they are skipped otherwise. A scratch directory it removes
# again; no database, configuration or network. Not installed; a bench tool.
add_executable(ini-file-tests ${SERVER_DIR}/platform/ini_file_tests_main.cpp)
target_link_libraries(ini-file-tests PRIVATE hmailserver_core)
set_target_properties(ini-file-tests PROPERTIES OUTPUT_NAME ini-file-tests)

# ---------------------------------------------------------------- SCRAM's iteration count
#
# The rounds SCRAM-SHA-256 asks a client for, held to the bound a password logon
# holds a stored hash's count to (Common/Util/Hashing/ScramSha256.cpp): a program
# of its own, as backup-tests is one, since the regression suite's case writes the
# stored hash through COM's Database, which the Linux suite cannot reach. No
# database, configuration file or network. Not installed; a bench tool.
add_executable(scram-tests ${SERVER_DIR}/platform/scram_tests_main.cpp)
target_link_libraries(scram-tests PRIVATE hmailserver_core)
set_target_properties(scram-tests PROPERTIES OUTPUT_NAME scram-tests)

# ---------------------------------------------------------------- the backup's exclusions
#
# BackupExecuterTester (Common/Application/BackupExecuter.h) as a program of its
# own, for the reason spf-tests is one: on Windows it runs inside the server
# through ClassTester, which nothing reaches here. A backup's copy of the data
# directory and a restore's emptying of it, on a scratch tree under the system's
# temp directory that it removes again: a destination inside the data directory,
# the temp directory and the live update's folder left out of the one and left in
# place by the other - the case the first real Linux live update found, whose
# destination under /var/lib/hmailserver copied itself into itself. No database,
# configuration or network. Not installed; a bench tool.
add_executable(backup-tests ${SERVER_DIR}/platform/backup_tests_main.cpp)
target_link_libraries(backup-tests PRIVATE hmailserver_core)
set_target_properties(backup-tests PROPERTIES OUTPUT_NAME backup-tests)

# ---------------------------------------------------------------- a written file's close
#
# File::Close and the writers of stored mail that take its answer, against files
# that cannot take their last bytes: /dev/full, a quota of the process's own
# (RLIMIT_FSIZE), and - when the program may mount one - a full tmpfs in a mount
# namespace of a child's own. The close writes what a write left buffered, and its
# answer is the only report of whether that arrived; FileUtilitiesTester's part of
# it runs on Windows through ClassTester, which nothing reaches here. Scratch files
# under the system's temp directory, removed again; no database, configuration or
# network. Not installed; a bench tool.
add_executable(file-utilities-tests ${SERVER_DIR}/platform/file_utilities_tests_main.cpp)
target_link_libraries(file-utilities-tests PRIVATE hmailserver_core)
set_target_properties(file-utilities-tests PROPERTIES OUTPUT_NAME file-utilities-tests)

# ---------------------------------------------------------------- the live update's trust
#
# IniFileSettingsTester::TestTrustAnchors (Common/Application/IniFileSettings.h) as
# a program of its own, for the reason spf-tests is one: on Windows it runs inside
# the server through ClassTester, which nothing reaches here. The five settings
# the live update checks a package against are read from hMailServer.INI alone,
# as `hmailserver --verify-update` - root's check, which opens no database - reads
# them: with a configuration file of its own and a database overlay holding other
# values, the server has to read the file's and build the same trust either way,
# and an administrator's change to one has to be refused. No database or network.
# Not installed; a bench tool.
add_executable(trust-anchor-tests ${SERVER_DIR}/platform/trust_anchor_tests_main.cpp)
target_link_libraries(trust-anchor-tests PRIVATE hmailserver_core)
set_target_properties(trust-anchor-tests PROPERTIES OUTPUT_NAME trust-anchor-tests)

# ---------------------------------------------------------------- the business objects' copies
#
# AccountTester (Common/BO/Account.h) and the checks beside it as a program of
# their own, for the reason spf-tests is one: on Windows AccountTester runs inside
# the server through ClassTester, which nothing reaches here. An account's copy -
# what COM's Authenticate hands a webmail, which saves it - holds every member,
# its retention and its mailbox key wrap among them; a message's copy carries what
# it is meant to; four types start with every scalar set in memory that held
# something else; and a REST create takes an IP range's priority as its update
# does. No database, configuration or network. Not installed; a bench tool.
add_executable(business-object-tests ${SERVER_DIR}/platform/business_object_tests_main.cpp)
target_link_libraries(business-object-tests PRIVATE hmailserver_core)
set_target_properties(business-object-tests PROPERTIES OUTPUT_NAME business-object-tests)

# ---------------------------------------------------------------- the TLS contexts' options
#
# SslContextInitializerTester (Common/TCPIP/SslContextInitializer.h) as a program
# of its own, for the reason spf-tests is one: on Windows it runs inside the server
# through ClassTester, which nothing reaches here. Every combination of the TLS
# versions and the cipher-order settings, set on a listener's context and a
# client's as the server sets them, read back with SSL_CTX_get_options against the
# distribution's OpenSSL: exactly what was asked for, and nothing from bit 32 up
# but certificate compression off - until 6.3.6 an int held the mask and every
# option from bit 32 up was set. No database, configuration or network. Not
# installed; a bench tool.
add_executable(tls-options-tests ${SERVER_DIR}/platform/tls_options_tests_main.cpp)
target_link_libraries(tls-options-tests PRIVATE hmailserver_core)
set_target_properties(tls-options-tests PROPERTIES OUTPUT_NAME tls-options-tests)

# ---------------------------------------------------------------- the update feed's release page
#
# UpdateCheckerTester (Common/Util/UpdateChecker.h) as a program of its own, for
# the reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. A release's html_url is shown as a
# link, so the check keeps it only as an https:// address with a host and no
# space or control character; this reads feed documents with the project's page,
# the longest kept, and values of every other shape, and requires the one kept,
# the others dropped and the release read either way. No network or
# configuration. Not installed; a bench tool.
add_executable(update-feed-tests ${SERVER_DIR}/platform/update_feed_tests_main.cpp)
target_link_libraries(update-feed-tests PRIVATE hmailserver_core)
set_target_properties(update-feed-tests PROPERTIES OUTPUT_NAME update-feed-tests)

# ---------------------------------------------------------------- the network deadlines
#
# The server's own network clients and raw-socket listeners held to their
# deadlines against peers that never answer, as a program of its own: nothing
# on Windows needs it, where the socket timeouts these used to set were always
# honoured, and on Linux, where they were refused, it is the only test that can
# see one hang. The OAuth2 token request, an ACME request, the DANE TLSA query
# and the ACME http-01 responder, each against a peer of its own on 127.0.0.1;
# the MTA-STS policy fetch, held to public addresses, refusing the loopback and
# then going through a silent proxy of its own on 127.0.0.1; and the ACME header
# lookup (platform/network_deadline_tests_main.cpp says what each must do). A
# configuration file of its own under the system's temp directory, removed
# again; no database, no name asked of DNS, no connection but to 127.0.0.1 (the
# http-01 responder listens on every address for the seconds its case takes),
# no root - but the loopback, so the source packages' builds, which refuse every
# socket, leave it out of their lists. About three quarters of a minute: the
# longest deadline, the cases running at once, then the proxied fetch's. Not
# installed; a bench tool.
add_executable(network-deadline-tests ${SERVER_DIR}/platform/network_deadline_tests_main.cpp)
target_link_libraries(network-deadline-tests PRIVATE hmailserver_core)
set_target_properties(network-deadline-tests PROPERTIES OUTPUT_NAME network-deadline-tests)

# ---------------------------------------------------------------- the live log
#
# The Logger's live log switched on and off while four threads log and one drains
# it (Common/Application/Logger.h, enable_live_log_): no entry appended once the
# switch is off, the buffer's limit switching it off, and every entry whole. Built
# with -fsanitize=thread (the program's header says how) it is the harness that
# holds the switch to being no data race. Linux only: the Windows ClassTester runs
# inside a live service, whose live log belongs to its Control Panel. No database,
# configuration of the machine's or network. Not installed; a bench tool.
add_executable(live-log-tests ${SERVER_DIR}/platform/live_log_tests_main.cpp)
target_link_libraries(live-log-tests PRIVATE hmailserver_core)
set_target_properties(live-log-tests PROPERTIES OUTPUT_NAME live-log-tests)

# ---------------------------------------------------------------- COM's languages
#
# LanguageTester (Common/Util/Language.h) as a program of its own, for the reason
# spf-tests is one: on Windows it runs inside the server through ClassTester,
# which nothing reaches here. With it, a ProgramFolder changed in a configuration
# file of its own under a loaded set, as a reinitialise reads one: a language
# taken before keeps the directory its English came from. Scratch files under the
# system's temp directory; no database or network. Not installed; a bench tool.
add_executable(language-tests ${SERVER_DIR}/platform/language_tests_main.cpp)
target_link_libraries(language-tests PRIVATE hmailserver_core)
set_target_properties(language-tests PROPERTIES OUTPUT_NAME language-tests)

# ---------------------------------------------------------------- the database connection string
#
# PGConnectionTester (Common/SQL/PGConnection.h) as a program of its own, for the
# reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. The conninfo the server hands libpq,
# read back by libpq's own parser: passwords, users and database names with
# quotes, backslashes and spaces byte for byte, and none moving the host, the port
# or sslmode. No database, configuration or network. Not installed; a bench tool.
add_executable(connection-string-tests ${SERVER_DIR}/platform/connection_string_tests_main.cpp)
target_link_libraries(connection-string-tests PRIVATE hmailserver_core)
set_target_properties(connection-string-tests PROPERTIES OUTPUT_NAME connection-string-tests)

# ---------------------------------------------------------------- the profiles' lock
#
# ProfilesTester (Common/Application/Profiles.h) as a program of its own, for the
# reason spf-tests is one: on Windows it runs inside the server through
# ClassTester, which nothing reaches here. An apply, a revert, a discard or a
# decline asked for on the thread that holds the profiles' lock - by an event
# script one of their own writes ran - refused at once rather than locking the
# mutex a second time, which here would wait for ever. No database, configuration
# or network. Not installed; a bench tool.
add_executable(profiles-tests ${SERVER_DIR}/platform/profiles_tests_main.cpp)
target_link_libraries(profiles-tests PRIVATE hmailserver_core)
set_target_properties(profiles-tests PROPERTIES OUTPUT_NAME profiles-tests)

# ---------------------------------------------------------------- the IMAP parsers' time
#
# StringNoCaseTester (Common/Util/Parsing/StringParser.h), IMAPFetchParserTester,
# IMAPThreadSubjectTester, IMAPSequenceSetTester, IMAPStoreKeywordsTester,
# IMAPFetchHeaderFieldsTester and IMAPFetchSectionsTester (IMAP/) as a program of
# their own, for the reason spf-tests is one: on Windows they run inside the
# server through ClassTester, which nothing reaches here. What String's
# case-ignoring search and replace, the items of a FETCH, the base subject, a
# message set, a STORE's keywords, a FETCH's HEADER.FIELDS sections and the
# bodies its part numbers name make, held to what 6.3.5's code - kept in the
# testers - made of the same generated cases, and the time each takes held to a
# pass over what it reads. No network, configuration or database. Not installed;
# a bench tool.
add_executable(imap-parse-tests ${SERVER_DIR}/platform/imap_parse_tests_main.cpp)
target_link_libraries(imap-parse-tests PRIVATE hmailserver_core)
set_target_properties(imap-parse-tests PROPERTIES OUTPUT_NAME imap-parse-tests)

# The readers of delivery status notifications and abuse feedback reports, as
# ClassTester runs them on Windows: report_parser_tests_main.cpp says what.
add_executable(report-parser-tests ${SERVER_DIR}/platform/report_parser_tests_main.cpp)
target_link_libraries(report-parser-tests PRIVATE hmailserver_core)
set_target_properties(report-parser-tests PROPERTIES OUTPUT_NAME report-parser-tests)

# The quadratic-time sweep of 6.3.6 (Common/Util/InputCostTester.h), as
# ClassTester runs it on Windows: input_cost_tests_main.cpp says what. Nine
# readers of what a remote party sends, each held to what 6.3.5's code - kept
# in the tester - made of the same generated cases and to a single pass's
# time over the input that was that code's worst. Not installed; a bench tool.
add_executable(input-cost-tests ${SERVER_DIR}/platform/input_cost_tests_main.cpp)
target_link_libraries(input-cost-tests PRIVATE hmailserver_core)
set_target_properties(input-cost-tests PROPERTIES OUTPUT_NAME input-cost-tests)

# How a command that works in the service's folders runs when root runs it
# (platform/root_command.h): as the data directory's owner, on as root over a
# tree that is root's alone, or refused. root_command_tests_main.cpp says what
# it lays out; most of it needs root, to give folders and links to another
# account, and is said to be skipped without it - linux-build runs it as root in
# its container. Not installed; a bench tool.
add_executable(root-command-tests ${SERVER_DIR}/platform/root_command_tests_main.cpp ${SERVER_DIR}/platform/root_command.cpp)
target_link_libraries(root-command-tests PRIVATE hmailserver_core)
set_target_properties(root-command-tests PROPERTIES OUTPUT_NAME root-command-tests)

include(GNUInstallDirs)
install(TARGETS hmailserver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${SERVER_DIR}/../DBScripts/
   DESTINATION ${CMAKE_INSTALL_DATADIR}/hmailserver/DBScripts
   FILES_MATCHING PATTERN "*.sql")

# QuickJS's MIT licence asks for its notice in every copy, and the engine is
# compiled into the binary: the release's LICENSE, installed beside the data the
# server reads, under /usr/share/hmailserver rather than /usr/share/doc so that
# no package has to own a directory another package owns.
install(FILES ${SERVER_DIR}/quickjs/LICENSE
   DESTINATION ${CMAKE_INSTALL_DATADIR}/hmailserver/licenses
   RENAME QuickJS.txt)

# And hMailServer's own licence beside it, the AGPL-3.0-or-later: its section 4
# asks for a copy of the licence to go with every copy of the program, and
# section 6 conveys the object code on the terms of sections 4 and 5. The
# repository's LICENSE is the FSF's text, which .gitattributes checks out with
# CRLF line ends on every platform (and the source tarball holds as a checkout
# does); what is installed is the FSF's file byte for byte, without them, as the
# AppImage carries it (SHA-256 0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0).
# Every install gets it, a distribution's own build of the source packages and
# the PKGBUILD's included. The copyright file each package format keeps in its
# own place is the packages' alone: CPACK_INSTALL_SCRIPTS, below.
set(HM_AGPL_LICENSE ${SERVER_DIR}/../../../LICENSE)
file(READ ${HM_AGPL_LICENSE} HM_AGPL_TEXT)
string(REPLACE "\r" "" HM_AGPL_TEXT "${HM_AGPL_TEXT}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/packaging/AGPL-3.0.txt "${HM_AGPL_TEXT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HM_AGPL_LICENSE})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/AGPL-3.0.txt
   DESTINATION ${CMAKE_INSTALL_DATADIR}/hmailserver/licenses)

# The Control Deck - the administration page the REST listener serves at GET /.
#
# RestApiServer::HandleWebAdminPage_ builds the path as
# GetProgramDirectory() + "WebAdmin" + "index.html", and IniFileSettings gives
# the program directory a trailing separator when it reads it, so with the
# ProgramFolder=/usr/share/hmailserver that platform/packaging/hMailServer.ini
# sets, the file the server opens is exactly
# /usr/share/hmailserver/WebAdmin/index.html - which is what the DESTINATION
# below resolves to under the /usr prefix these packages are built with, and it
# is the same derivation the DBScripts rule above relies on.
#
# Until this rule existed, every packaged Linux server answered GET / with the
# built-in "Web administration page not installed" stub: the page was in the
# repository, the code looked for it, and nothing put it there. The install
# check of GitLab CI's linux-package-deb and linux-package-rpm jobs
# (build/ci/linux-package-install.sh) now asks the installed server for
# the page and refuses the stub.
install(FILES ${SERVER_DIR}/../../installation/WebAdmin/index.html
   DESTINATION ${CMAKE_INSTALL_DATADIR}/hmailserver/WebAdmin)
# The page's catalogues, one JSON file per language, which
# RestApiServer::HandleWebAdminLanguage_ reads from
# <ProgramFolder>/WebAdmin/languages/<code>.json - the directory name is kept
# because the source below has no trailing slash.
install(DIRECTORY ${SERVER_DIR}/../../installation/WebAdmin/languages
   DESTINATION ${CMAKE_INSTALL_DATADIR}/hmailserver/WebAdmin
   FILES_MATCHING PATTERN "*.json")
# Absolute for the same reason, and /usr/lib rather than /lib because that is
# where a distribution's own units live and where systemd looks first.
install(FILES ${SERVER_DIR}/platform/packaging/hmailserver.service
   DESTINATION /usr/lib/systemd/system)

# The live update's apply step: the helper the server cannot be (it runs as its
# own user under a read-only /usr), and the two units that run it as root when
# the server writes its request to /run/hmailserver. hmailserver-update's header
# has the whole mechanism; the Windows counterpart is hMailServer.Updater.exe.
# None of the three in a build with HM_LIVE_UPDATE OFF, which has no apply step
# to hand anything to them (build/test-install-kind.sh checks an install).
if(HM_LIVE_UPDATE)
   install(PROGRAMS ${SERVER_DIR}/platform/packaging/hmailserver-update
      DESTINATION lib/hmailserver)
   install(FILES
      ${SERVER_DIR}/platform/packaging/hmailserver-update.path
      ${SERVER_DIR}/platform/packaging/hmailserver-update.service
      DESTINATION /usr/lib/systemd/system)
endif()
# /etc, written out rather than taken from CMAKE_INSTALL_SYSCONFDIR. CPack
# prepends the packaging prefix to every RELATIVE destination, and GNUInstallDirs
# gives a relative "etc", so the indirection puts the configuration at
# /usr/etc/hmailserver - which the package then installs, the server never reads,
# and rpm rejects when its %config list names the real path.
# 0640 in the package itself, as the maintainer scripts leave it (root:hmailserver,
# once the group exists): the file is where the database password and the
# administrator password's hash are written, and between the unpacking and the
# script that chgrps it, nobody but root has any business reading it. It used to be
# packed 0644 and narrowed only by the script.
install(FILES ${SERVER_DIR}/platform/packaging/hMailServer.ini
   DESTINATION /etc/hmailserver
   PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)
install(FILES ${SERVER_DIR}/platform/packaging/hmailserver.logrotate
   RENAME hmailserver
   DESTINATION /etc/logrotate.d)

# The data file the server reads from its own directory and cannot work without:
# dh2048.pem, the Diffie-Hellman group. SslContextInitializer reports a critical
# 5603 for EVERY TLS context when it is missing - which is to say the server would
# install and then refuse every encrypted connection. It goes beside the binary,
# which is where the code looks: Utilities::GetBinDirectory is the running
# executable's own directory. Until 6.3.6 a second file went with it, SURBL's
# list of two-level domains of 2006; the SURBL lookups and the MX-record test cut
# host names by the Public Suffix List compiled in now.
install(FILES
   ${SERVER_DIR}/../../installation/Extras/dh2048.pem
   DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------- packages
#
# One version, one set of packages, built from this one description. CPack makes
# the .deb and the .rpm; the AppImage and the Arch PKGBUILD are scripts beside
# this file, because neither is something CPack does well.
# What auto-ban does below the server on Linux: the hook the server runs when
# AutoBanFirewall=1 is set, and a fail2ban filter and jail for the other half,
# which needs no capability at all because fail2ban runs as root and reads the
# application log. The jail is installed disabled, as Debian asks of a shipped
# jail; the filter and the jail are configuration, kept on upgrade.
#
# The server runs the hook where this rule puts it, which it is told here
# (HM_AUTOBAN_HOOK, AutoBanFirewall::PackagedHook): /usr/lib/hmailserver/autoban-hook
# for every package, whose prefix is /usr, and the hook below another prefix where
# the build has one - the Nix store's for the flake (nix/package.nix), /usr/local
# for a build installed by hand. Until 6.3.6 the server named /usr/lib/hmailserver
# whatever the prefix, which NixOS and a /usr/local install do not have.
set(HM_AUTOBAN_HOOK_DIR lib/hmailserver)
set(HM_AUTOBAN_HOOK "${CMAKE_INSTALL_PREFIX}/${HM_AUTOBAN_HOOK_DIR}/autoban-hook")
# A C string literal, and the server splits the command line it runs at spaces
# (ProcessLauncher): a prefix with a space, a quote or a backslash would make
# another command of it.
if(NOT HM_AUTOBAN_HOOK MATCHES "^/[A-Za-z0-9._+/-]*$")
   message(FATAL_ERROR
      "The auto-ban hook would be installed at '${HM_AUTOBAN_HOOK}', which the server cannot run as one "
      "command: CMAKE_INSTALL_PREFIX must be an absolute path of letters, digits and '._+/-' only.")
endif()
install(PROGRAMS ${SERVER_DIR}/platform/packaging/autoban-hook
   DESTINATION ${HM_AUTOBAN_HOOK_DIR})
target_compile_definitions(hmailserver_core PRIVATE "HM_AUTOBAN_HOOK=\"${HM_AUTOBAN_HOOK}\"")
install(FILES ${SERVER_DIR}/platform/packaging/fail2ban/filter.d/hmailserver.conf
   DESTINATION /etc/fail2ban/filter.d)
install(FILES ${SERVER_DIR}/platform/packaging/fail2ban/jail.d/hmailserver.conf
   DESTINATION /etc/fail2ban/jail.d)

# The command-line client, beside the server it administers. It is one Python 3
# file with nothing but the standard library behind it, and it speaks the REST
# API rather than anything in this binary - so it administers this server, a
# server on another machine, or a Windows one, from a shell. python3 is a
# recommendation and not a dependency: a server that never runs hmctl should not
# pull an interpreter in, and the package manager offers it where it is wanted.
install(PROGRAMS ${SERVER_DIR}/../Tools/Cli/hmctl
   DESTINATION ${CMAKE_INSTALL_BINDIR})

set(CPACK_PACKAGE_NAME hmailserver)
set(CPACK_PACKAGE_VENDOR "Progressive Robot Ltd")
set(CPACK_PACKAGE_CONTACT "christopher.j.holloway@outlook.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "hMailServer, a mail server for SMTP, IMAP and POP3")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://gitlab.com/hmailserver/hmailserver")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
# DESTDIR mode, stated rather than left to CPack's version to decide. The unit,
# the configuration and the logrotate rule are installed to absolute paths (see
# the install rules above and why), and how CPack treats an absolute DESTINATION
# without this flag has varied between CMake releases: the 4.x on the machine
# the port was written on packaged them where they were written, and the 3.28
# on ubuntu-24.04 - the first CI packaging run, on the ARM runner - did not,
# which the install check caught as a missing /etc/hmailserver/hMailServer.ini.
# With DESTDIR set, every install rule runs exactly as `make install` would
# into the staging tree, and the layout is the one the rules say.
set(CPACK_SET_DESTDIR ON)
# The copyright file, where each format's distributions look for it: the .deb's
# /usr/share/doc/hmailserver/copyright (Debian Policy 12.5) and the .rpm's
# /usr/share/licenses/hmailserver/copyright, marked %license below. CPack runs the
# script into each package's staging tree, and `cmake --install` never runs it, so
# the source packages, which install with CMake and place their copyright files
# with debhelper and %license, are left to do that (the script says more). The file
# is the Debian source package's; build/ci/packages/check-licenses.sh holds both
# packages to it.
set(CPACK_INSTALL_SCRIPTS ${SERVER_DIR}/platform/packaging/cpack-licenses.cmake)
set(CPACK_HMAILSERVER_COPYRIGHT_FILE ${SERVER_DIR}/platform/packaging/debian-src/debian/copyright)
set(CPACK_STRIP_FILES ON)
# Reproducible packages (hmailserver/docs/ReproducibleBuilds.md). Every
# directory the packages hold is 0755, whatever the umask of the machine that
# builds them: without this, the staging directories CPack creates take the
# umask, and a build under umask 0002 made a .deb and an .rpm whose directories
# were 0775 - the same files, another package (found by build/ci/linux-repro.sh
# on 26 September 2026). The files' own modes come from the install rules above.
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
   OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
# The .deb stamps SOURCE_DATE_EPOCH (linux-build.sh sets it) on its own; rpmbuild
# stamps the clock and the build container's host name into the header, and the
# time each file was staged into the payload, unless it is told otherwise.
set(CPACK_RPM_SPEC_MORE_DEFINE
   "%define use_source_date_epoch_as_buildtime 1\n%define clamp_mtime_to_source_date_epoch 1\n%define _buildhost reproducible")

# The architecture each packager names differently for the same machine, and
# rpm's name for it in a dependency on another package's build for the same
# architecture (%{_isa}: "glibc(x86-64)").
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
   set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
   set(CPACK_RPM_PACKAGE_ARCHITECTURE aarch64)
   set(HM_RPM_ISA aarch-64)
else()
   set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
   set(CPACK_RPM_PACKAGE_ARCHITECTURE x86_64)
   set(HM_RPM_ISA x86-64)
endif()

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION mail)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Two things the server can do without and most servers want: the system's
# 7-Zip, which the backup writes its archive with (Compression.cpp looks for
# 7zz, 7z, 7za or 7zr on PATH), and Python 3, which /usr/bin/hmctl is written
# in. Recommended rather than required: a server that never takes a backup and
# is administered from somewhere else needs neither, and a mail server should
# not drag an interpreter onto a machine to sit unused.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "7zip | p7zip-full, python3")
set(CPACK_RPM_PACKAGE_SUGGESTS "p7zip python3")
# The certificate authorities every HTTPS request this server makes is checked
# against: the update check and its download, a JWKS, an OAuth2 token endpoint,
# an object store, a webhook. OpenSSL finds none of its own; the distribution's
# bundle (/etc/ssl/certs) is ca-certificates. A real Ubuntu or Fedora server has
# it already, which is why nothing had failed; a minimal image or container does
# not, and there the update check fails on the feed's certificate - the first
# Linux live update found the .deb did not ask for it, and PostgreSQL happened to
# pull it in (29 September 2026). A dependency, not a recommendation: it costs
# nothing where it is already installed. dpkg-shlibdeps' libraries follow it on
# the Depends line, and rpm's automatic requirements beside it.
#
# And the .rpm asks for glibc's character-set converters. Debian's and Ubuntu's
# libc6 carries every one; Fedora's and EL's glibc keeps all but a few in
# glibc-gconv-extra, and openSUSE's in glibc-gconv-modules-extra, which glibc only
# recommends, so that a minimal installation and every container image of those
# distributions goes without: there the base package has UTF-7, CP1252, ISO-8859-1
# and -15 and Unicode's, and the server converted no text in BIG5, ISO-2022-JP,
# the other Windows and ISO-8859 code pages, or the PST import's KOI8 and CP932,
# 936 and 949 (Fedora 44, UBI 9 and 10, Leap 16.0 and Tumbleweed, 1 October 2026).
# One rich dependency that each of them meets with its own package, for every
# .rpm: the generic one is installed on all of them, and the live update offers it
# to a server installed from any of them (docs/PackagesRpm.md).
#
# Required where dnf can install them, and only recommended elsewhere. The live
# update installs an .rpm with dnf where there is a dnf command and with rpm -Uvh
# where there is not (hmailserver-update, install_package); rpm -Uvh resolves
# nothing and refuses a package whose requirements are not met already, and dnf
# refuses one whose requirements its repositories cannot meet. A plain requirement
# left every openSUSE server without the converters - each minimal and container
# one - on 6.3.5, its update to 6.3.6 refused by 6.3.5's helper. A plain
# recommendation does not serve Fedora and EL instead: glibc recommends the same
# package, and dnf does not install a recommendation that an installed package's
# recommendation already left out (exclude_from_weak_autodetect, on by default), so
# a dnf install never brought them. Both measured with a package of these lines in
# Fedora 44, UBI 9 and 10, Leap 16.0 and Tumbleweed on 2 October 2026. So the
# requirement holds "if (dnf and system-release)": where dnf is installed on
# Fedora or EL, whose release package (fedora-release, redhat-release,
# almalinux-release and the rest) provides system-release, dnf installs the
# converters with the package, as it did, and rpm -Uvh refuses it there as before,
# where the live update does not use it. Everywhere else it is void: on openSUSE,
# which provides no system-release, rpm -Uvh installs the package, and the
# recommendation is what zypper installs (with solver.onlyRequires off, a system's
# default; container images set it on). "if dnf" alone was not enough: openSUSE
# packages dnf (Leap 16.0's dnf 4.23, Tumbleweed's dnf5), and where it has been
# installed beside zypper the live update installs with it - and that dnf has no
# repositories, so it refused the package for converters it could not find, as it
# refused the plain requirement (3 October 2026). A UBI minimal image, with
# microdnf and no dnf, has the requirement void and is updated with rpm -Uvh. Where
# they are missing the server names them and the package at every start and at
# --check-config (platform/main.cpp). rpm takes rich dependencies from 4.13, and
# every release built for has 4.16 or later. The architecture's own build of the
# package, as %{_isa} names it: the i686 build of the converters, beside an x86-64
# glibc, would satisfy the bare name and convert nothing for this server. The RPM
# spec, whose packages have no live update, requires its distribution's package.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates")
set(HM_RPM_CONVERTERS "(glibc-gconv-extra(${HM_RPM_ISA}) or glibc-gconv-modules-extra(${HM_RPM_ISA}))")
set(CPACK_RPM_PACKAGE_REQUIRES "ca-certificates, (${HM_RPM_CONVERTERS} if (dnf and system-release))")
set(CPACK_RPM_PACKAGE_RECOMMENDS "${HM_RPM_CONVERTERS}")
# An upgrade must not overwrite the configuration. CPack's DEB generator writes no
# conffiles list of its own, so one is generated here and handed to dpkg with the
# maintainer scripts; the RPM equivalent is the %config(noreplace) list below.
# Without these two, upgrading replaces /etc/hmailserver/hMailServer.ini and takes
# the database credentials with it.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/packaging/conffiles
   "/etc/hmailserver/hMailServer.ini
/etc/logrotate.d/hmailserver
/etc/fail2ban/filter.d/hmailserver.conf
/etc/fail2ban/jail.d/hmailserver.conf
")

set(CPACK_RPM_USER_FILELIST
   "%config(noreplace) /etc/hmailserver/hMailServer.ini"
   "%config(noreplace) /etc/logrotate.d/hmailserver"
   "%config(noreplace) /etc/fail2ban/filter.d/hmailserver.conf"
   "%config(noreplace) /etc/fail2ban/jail.d/hmailserver.conf")

set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
   "${SERVER_DIR}/platform/packaging/debian/postinst;${SERVER_DIR}/platform/packaging/debian/prerm;${SERVER_DIR}/platform/packaging/debian/postrm;${CMAKE_CURRENT_BINARY_DIR}/packaging/conffiles")
# The control member gives the three maintainer scripts 0755 and conffiles 0644,
# as Debian policy asks, whatever their modes on disk. Without this CPack copies
# each file's own mode: conffiles took the umask it was written under, and the
# scripts the mode the checkout gave them (git records them as executable, so
# 0775 in a clone made under umask 0002, 0755 under 0022, and 0777 in a GitLab
# docker executor's checkout, made under umask 0000), so the .deb depended on
# who cloned the tree (hmailserver/docs/ReproducibleBuilds.md). CPack asks only
# whether the variable is set: set to OFF, it is on all the same.
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

# The licence files, marked %license as Fedora's guidelines ask, so that rpm -qL
# lists them: the copyright file cpack-licenses.cmake puts in, and the two texts
# beside the data.
list(APPEND CPACK_RPM_USER_FILELIST
   "%license /usr/share/licenses/hmailserver/copyright"
   "%license ${CMAKE_INSTALL_FULL_DATADIR}/hmailserver/licenses/AGPL-3.0.txt"
   "%license ${CMAKE_INSTALL_FULL_DATADIR}/hmailserver/licenses/QuickJS.txt")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
# The .rpm's %changelog. Without a file of its own CPack writes a placeholder -
# "Sun Jul 04 2010 Eric Noulard ... Generated by CPack RPM (no Changelog file
# were provided)" - which rpm -q --changelog showed for every release until 6.3.6
# (found in the first Linux live update, 29 September 2026). One entry, for this
# version: the release's own heading in CHANGELOG.md - its date and its page -
# when the version has been released, and otherwise what the tree is, a build
# before its release. Dated by SOURCE_DATE_EPOCH, which string(TIMESTAMP) reads
# and linux-build.sh sets to the commit's time, so that two builds of one commit
# make the same header (docs/ReproducibleBuilds.md); by hand, without it, today.
string(TIMESTAMP HM_RPM_CHANGELOG_DATE "%a %b %d %Y" UTC)
set(HM_RPM_CHANGELOG_TEXT
   "- hMailServer ${PROJECT_VERSION}, built from the tree before that release was made: what it changes is in CHANGELOG.md, under Unreleased.")
set(HM_CHANGELOG_MD ${SERVER_DIR}/../../../CHANGELOG.md)
if(EXISTS ${HM_CHANGELOG_MD})
   string(REPLACE "." "\\." HM_VERSION_PATTERN "${PROJECT_VERSION}")
   file(STRINGS ${HM_CHANGELOG_MD} HM_CHANGELOG_HEADING REGEX "^## \\[${HM_VERSION_PATTERN}\\]\\(" LIMIT_COUNT 1)
   if(HM_CHANGELOG_HEADING MATCHES "^## \\[([0-9.]+)\\]\\(([^)]+)\\) - ([0-9-]+)")
      set(HM_RPM_CHANGELOG_TEXT
         "- hMailServer ${CMAKE_MATCH_1}, released ${CMAKE_MATCH_3}. What it changes: ${CMAKE_MATCH_2}")
   endif()
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/packaging/rpm-changelog
   "* ${HM_RPM_CHANGELOG_DATE} ${CPACK_PACKAGE_VENDOR} <${CPACK_PACKAGE_CONTACT}> - ${PROJECT_VERSION}-1\n${HM_RPM_CHANGELOG_TEXT}\n")
set(CPACK_RPM_CHANGELOG_FILE ${CMAKE_CURRENT_BINARY_DIR}/packaging/rpm-changelog)
# The licences the tag can name of what the package carries: the server's own, and
# of the code compiled into it QuickJS (MIT), the MIME coder (BSD-1-Clause),
# CStdString (a permissive notice of its own), the Public Suffix List's rules
# (MPL-2.0) and Boost (BSL-1.0), linked statically. The source package's spec names
# the same less Boost, whose shared libraries it links. Neither can name a notice
# no identifier names yet: the copyright file declares those
# (hmailserver/docs/SourcePackages.md).
set(CPACK_RPM_PACKAGE_LICENSE
   "AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND LicenseRef-Fedora-UltraPermissive AND BSL-1.0")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${SERVER_DIR}/platform/packaging/rpm/post)
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${SERVER_DIR}/platform/packaging/rpm/preun)
# Without a %postun, rpm -e leaves systemd holding a unit file that is gone.
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${SERVER_DIR}/platform/packaging/rpm/postun)
# /usr/lib/systemd/system, /etc/logrotate.d and /usr/share/licenses belong to
# other packages.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
   /usr/lib/systemd /usr/lib/systemd/system /etc/logrotate.d /usr/share/licenses)

set(CPACK_GENERATOR "DEB;RPM")
include(CPack)

# ---------------------------------------------------------------- the census

add_custom_target(tu-census
   COMMAND ${CMAKE_COMMAND} -E env SERVER=${SERVER_DIR}
           bash ${SERVER_DIR}/../../../build/linux-tu-census.sh
   COMMENT "Counting the core translation units a POSIX compiler accepts"
   USES_TERMINAL)
