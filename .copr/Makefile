# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Fedora Copr's make_srpm (hmailserver/docs/Channels.md, "Fedora Copr"). Copr clones
# this repository at the package's committish (master) and runs, in a Fedora mock
# chroot, as root, with the network:
#
#   make -f .copr/Makefile srpm outdir="<outdir>" spec="<spec>"
#
# The source RPM is not made from the clone. build/ci/channels/copr-build.py srpm
# takes the release the update feed lists as the latest, downloads its source tarball
# and verifies it against its Sigstore bundle as this project's pipeline for the tag
# (cosign out of the image .gitlab-ci.yml pins, from gcr.io), then builds the source
# RPM with the spec and Source1 inside that tarball. So a push to master changes
# nothing Copr builds; a release does. spec= is not used: the spec is the release's.

top := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
outdir ?= $(CURDIR)

.PHONY: srpm
srpm:
	dnf -y install python3 rpm-build
	python3 $(top)/build/ci/channels/copr-build.py srpm --outdir "$(outdir)"
