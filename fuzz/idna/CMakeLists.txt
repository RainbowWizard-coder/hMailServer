# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The differential fuzz of UTS 46 against ada (fuzz/harness/idna_ada_fuzzer.cpp),
# as a CMake project of its own: the server's core library, from the server's
# own CMakeLists.txt, built with clang's libFuzzer coverage and AddressSanitizer,
# the harness linked against it and against ada, and the check of ada against
# Unicode's conformance tests that runs before it (idna_ada_conformance.cpp).
# Configured and built by build/ci/idna-differential-fuzz.sh, which says where
# ada and the table of the code points that differ between the two Unicode
# versions are:
#
#   HM_ADA_DIR          a directory holding ada.h and ada.cpp (ada 4.0.0's
#                       amalgamation, from PyPI)
#   HM_IDNA_DIFFERENCES a directory holding idna_version_differences.h
#
# Nothing here is part of the server's build, its packages or its tests.

cmake_minimum_required(VERSION 3.22)
project(idna_ada_differential CXX C)

if(NOT HM_ADA_DIR OR NOT EXISTS "${HM_ADA_DIR}/ada.cpp")
   message(FATAL_ERROR "HM_ADA_DIR must name a directory holding ada.h and ada.cpp")
endif()
if(NOT HM_IDNA_DIFFERENCES OR NOT EXISTS "${HM_IDNA_DIFFERENCES}/idna_version_differences.h")
   message(FATAL_ERROR "HM_IDNA_DIFFERENCES must name a directory holding idna_version_differences.h")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every unit is instrumented - the code under test is in the core library -
# and linked with AddressSanitizer; the fuzzer's own runtime only into the
# harness.
add_compile_options(-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=address)

set(REPOSITORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${REPOSITORY}/hmailserver/source/Server server EXCLUDE_FROM_ALL)

# ada, and the half of the harness that asks it, without the server's forced
# precompiled header.
add_library(ada_oracle STATIC ${HM_ADA_DIR}/ada.cpp ${REPOSITORY}/fuzz/harness/idna_ada_side.cpp)
target_include_directories(ada_oracle PRIVATE ${HM_ADA_DIR})

add_executable(idna-ada-fuzzer ${REPOSITORY}/fuzz/harness/idna_ada_fuzzer.cpp)
target_include_directories(idna-ada-fuzzer PRIVATE ${HM_IDNA_DIFFERENCES})
target_link_libraries(idna-ada-fuzzer PRIVATE hmailserver_core ada_oracle)
target_link_options(idna-ada-fuzzer PRIVATE -fsanitize=fuzzer)

add_executable(idna-ada-conformance ${REPOSITORY}/fuzz/harness/idna_ada_conformance.cpp)
target_include_directories(idna-ada-conformance PRIVATE ${HM_IDNA_DIFFERENCES})
target_link_libraries(idna-ada-conformance PRIVATE hmailserver_core ada_oracle)
target_link_options(idna-ada-conformance PRIVATE -fsanitize=fuzzer-no-link)
