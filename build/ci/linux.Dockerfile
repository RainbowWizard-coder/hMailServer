# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The image the two Linux jobs of .gitlab-ci.yml run in (linux-build and
# linux-suite), on the project's own runner, linix. It is built ON linix and
# never pushed anywhere: the runner's pull policy is if-not-present, so a job
# finds it in the host's own image store by its tag.
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
# What is in it, and why each part is here:
#   the toolchain and the -dev packages   the server's CMake build: the list the
#                                         Linux build has always installed
#                                         (CONTRIBUTING.md, "Prerequisites", "Linux")
#   gnupg                                 the OpenPGP writer's round trip in
#                                         linux-build: what the writer seals, gpg opens
#   postgresql-client                     psql, to lay out the suite's database
#   curl, procps, iproute2                to wait for the server, stop it, and say
#                                         what is listening when it did not start
#   python3                               build/ci/trx-to-junit.py
#   libicu78, tzdata                      what .NET needs at run time on Ubuntu
#   the .NET SDK                          the Linux suite (dotnet test); copied from
#                                         Microsoft's own SDK image, below
#
# Why the SDK is copied rather than installed with apt: Ubuntu 26.04's own
# dotnet-sdk-10.0 is a 10.0.1xx SDK, and global.json asks for 10.0.303 or a later
# feature band of .NET 10, so dotnet refuses it inside a checkout of this
# repository. Microsoft's SDK image carries a portable build under
# /usr/share/dotnet, pinned here by digest like the base image.

FROM mcr.microsoft.com/dotnet/sdk:10.0@sha256:2fa828c68761b1b8c23d7662dc134421b9d3b59fe1425fdbc80804e390cdb24d AS dotnet

FROM ubuntu:26.04@sha256:513c074113a871b51a8d16ab445c88779d6452d937a164fb5cc479f32668a41d

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      build-essential cmake ninja-build clang \
      libssl-dev zlib1g-dev libpq-dev libldap-dev \
      libboost-thread-dev libboost-chrono-dev libboost-system-dev \
      libboost-filesystem-dev libboost-regex-dev libboost-program-options-dev \
      file ca-certificates \
      gnupg \
      postgresql-client \
      curl procps iproute2 \
      python3 \
      libicu78 tzdata \
 && rm -rf /var/lib/apt/lists/*

COPY --from=dotnet /usr/share/dotnet /usr/share/dotnet
RUN ln -s /usr/share/dotnet/dotnet /usr/bin/dotnet

ENV LANG=C.UTF-8 \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    NUGET_XMLDOC_MODE=skip

# Fails the build here, not in the first job, if the copy did not give a
# working SDK.
RUN dotnet --list-sdks && clang --version | head -n 1 && cmake --version | head -n 1 && psql --version
