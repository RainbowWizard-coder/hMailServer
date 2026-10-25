# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The operator's image, as operator-sdk's Helm plugin makes one: the Helm operator,
# operator-sdk 1.42.3's, by the digest of its multi-platform index as quay.io served
# it on 25 October 2026 (published 26 June 2026), with this watches.yaml and the
# project's chart. Build it from the context build/ci/operator-context.sh writes,
# which holds the chart copied into it to the tree's ../hmailserver; there is no RUN,
# so kaniko builds it for any platform the base image has without running anything
# of that platform's (build/ci/operator-image.sh builds linux/amd64 and linux/arm64).
# The base image runs as its own user, 1001, and starts
# helm-operator run --watches-file=./watches.yaml in /opt/helm. No licence label: the
# image is the base image's - Red Hat's UBI and operator-sdk's helm-operator, under
# their own licences - with the project's chart and watches.yaml beside them, so no one
# identifier names it.
FROM quay.io/operator-framework/helm-operator:v1.42.3@sha256:80e0787ff36e85129d7983dc219736ba8a61e69857fbe46ec7389fddcbbf92f1

ARG HM_OPERATOR_VERSION=0.1.0
LABEL org.opencontainers.image.title="hMailServer operator (Progressive Robot's fork)" \
      org.opencontainers.image.description="A Kubernetes operator for Progressive Robot's fork of hMailServer, not the original hMailServer by Martin Knafve: each HMailServer resource is one installation of the project's Helm chart." \
      org.opencontainers.image.source="https://gitlab.com/hmailserver/hmailserver" \
      org.opencontainers.image.url="https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/source/Server/platform/packaging/helm/operator/README.md" \
      org.opencontainers.image.vendor="Progressive Robot Ltd" \
      org.opencontainers.image.version="${HM_OPERATOR_VERSION}"

ENV HOME=/opt/helm
COPY watches.yaml ${HOME}/watches.yaml
COPY helm-charts ${HOME}/helm-charts
WORKDIR ${HOME}
