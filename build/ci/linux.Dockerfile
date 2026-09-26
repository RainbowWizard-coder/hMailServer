# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The image the Linux jobs of .gitlab-ci.yml that build or run the server run in
# (linux-build, linux-suite, linux-cross, linux-db-upgrade-*, linux-sanitize,
# linux-sbom and linux-fuzz), on the project's own runner, linix. It is built ON
# linix and never pushed anywhere: the runner's pull policy is if-not-present, so
# a job finds it in the host's own image store by its tag.
#
# The tag is not chosen by hand. It is 26.04- and the first twelve hex digits of
# the SHA-256 of this file (line endings made LF), so that an edit here is a new
# tag: build/check-ci-image.py fails the checkers job until .gitlab-ci.yml names
# that tag, and a Linux job whose tag has not been built on linix fails at its
# first step, naming the image it could not find, instead of running on an image
# made from an older version of this file. build/ci/build-linux-image.sh computes
# the tag the same way and builds it; docs/ContinuousIntegration.md says when to
# run it.
#
# What is in it, and why each part is here. Every package comes from Ubuntu's own
# archive unless the line says otherwise; nothing is downloaded from GitHub.
#   the toolchain and the -dev packages   the server's CMake build: the list the
#                                         Linux build has always installed
#                                         (CONTRIBUTING.md, "Prerequisites", "Linux")
#   libclang-rt-dev, llvm                 clang's AddressSanitizer and libFuzzer
#                                         runtimes, and llvm-symbolizer, which turns
#                                         a sanitizer's addresses into file and line:
#                                         linux-sanitize and linux-fuzz
#   g++-aarch64-linux-gnu                 the AArch64 headers and GCC installation
#                                         the cross census compiles against (linux-cross)
#   rpm                                   rpmbuild, which CPack's RPM generator runs
#                                         (the packaging step of linux-build)
#   gnupg                                 the OpenPGP writer's round trip in
#                                         linux-build: what the writer seals, gpg opens
#   postgresql-client                     psql, to lay out the suite's database
#   mariadb-client, libmariadb3           the mariadb client the schema-upgrade gate
#                                         drives, and the library the server loads
#                                         (libmariadb.so.3) to reach a MySQL database
#   curl, procps, iproute2                to wait for the server, stop it, and say
#                                         what is listening when it did not start
#   python3                               build/ci/trx-to-junit.py, build/seed-orphans.py
#   7zip                                  7z, the archiver the server runs to write and
#                                         read a backup (the .deb recommends it): the
#                                         suite's backup tests fail without it
#   libicu78, tzdata                      what .NET needs at run time on Ubuntu
#   the .NET SDK and PowerShell           the Linux suite (dotnet test), and pwsh for
#                                         the SBOM's native-dependency merge; copied
#                                         from Microsoft's own SDK image, below
#   syft                                  the SBOM (linux-sbom); copied from Anchore's
#                                         own image on Docker Hub, below
#
# Why the SDK is copied rather than installed with apt: Ubuntu 26.04's own
# dotnet-sdk-10.0 is a 10.0.1xx SDK, and global.json asks for 10.0.303 or a later
# feature band of .NET 10, so dotnet refuses it inside a checkout of this
# repository. Microsoft's SDK image carries a portable build under
# /usr/share/dotnet, pinned here by digest like the base image, and PowerShell
# beside it under /usr/share/powershell as a tool that runs on that same runtime.
# Syft publishes its binaries on GitHub releases and as an image; the image, on
# Docker Hub, is the one of the two this project may fetch, and its one file is
# a static binary.

FROM mcr.microsoft.com/dotnet/sdk:10.0@sha256:2fa828c68761b1b8c23d7662dc134421b9d3b59fe1425fdbc80804e390cdb24d AS dotnet

FROM anchore/syft:v1.52.0@sha256:500e2d872ac019436926e8322b4fc1f39441d94d21f6f4046c6ff29b30e8cb02 AS syft

FROM ubuntu:26.04@sha256:513c074113a871b51a8d16ab445c88779d6452d937a164fb5cc479f32668a41d

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      build-essential cmake ninja-build clang libclang-rt-dev llvm \
      g++-aarch64-linux-gnu \
      libssl-dev zlib1g-dev libpq-dev libldap-dev \
      libboost-thread-dev libboost-chrono-dev libboost-system-dev \
      libboost-filesystem-dev libboost-regex-dev libboost-program-options-dev \
      file ca-certificates rpm \
      gnupg \
      postgresql-client \
      mariadb-client libmariadb3 \
      curl procps iproute2 \
      python3 \
      7zip \
      libicu78 tzdata \
 && rm -rf /var/lib/apt/lists/*

COPY --from=dotnet /usr/share/dotnet /usr/share/dotnet
COPY --from=dotnet /usr/share/powershell /usr/share/powershell
COPY --from=syft /syft /usr/local/bin/syft
RUN ln -s /usr/share/dotnet/dotnet /usr/bin/dotnet \
 && ln -s /usr/share/powershell/pwsh /usr/bin/pwsh

ENV LANG=C.UTF-8 \
    DOTNET_ROOT=/usr/share/dotnet \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    NUGET_XMLDOC_MODE=skip \
    POWERSHELL_TELEMETRY_OPTOUT=1 \
    POWERSHELL_UPDATECHECK=Off

# Fails the build here, not in the first job, if a copy did not give a working
# tool or a runtime a job needs is missing.
RUN dotnet --list-sdks && pwsh -NoProfile -Command '$PSVersionTable.PSVersion.ToString()' \
 && syft version | head -n 2 \
 && clang --version | head -n 1 && cmake --version | head -n 1 && psql --version && mariadb --version \
 && rpmbuild --version && command -v 7z && 7z i | head -n 2 && aarch64-linux-gnu-g++ --version | head -n 1 && llvm-symbolizer --version | head -n 1 \
 && ls /usr/lib/llvm-*/lib/clang/*/lib/*/libclang_rt.fuzzer*.a /usr/lib/llvm-*/lib/clang/*/lib/*/libclang_rt.asan*.a
