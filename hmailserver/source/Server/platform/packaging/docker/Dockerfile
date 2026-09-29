# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The hMailServer container image: the Linux .deb the tag's own pipeline built,
# installed on the distribution release it was built for, run as its own user.
#
#   docker run -d --name hmailserver \
#      -e HM_DB_TYPE=PostgreSQL -e HM_DB_HOST=db -e HM_DB_NAME=hmailserver \
#      -e HM_DB_USER=hmailserver -e HM_DB_PASSWORD=... \
#      -e HM_ADMIN_PASSWORD=... \
#      -e HM_REST_PORT=8045 -e HM_REST_BIND_ADDRESS=0.0.0.0 \
#      -e HM_REST_CERTIFICATE_FILE=/certs/rest.pem -e HM_REST_PRIVATE_KEY_FILE=/certs/rest.key \
#      -v /srv/hmailserver/certs:/certs:ro \
#      -p 25:25 -p 587:587 -p 993:993 -p 8045:8045 \
#      -v hm-data:/var/lib/hmailserver -v hm-logs:/var/log/hmailserver -v hm-etc:/etc/hmailserver \
#      registry.gitlab.com/hmailserver/hmailserver:<version>
#
# That is the project's GitLab container registry, at the path the project has
# in the group hmailserver, the image's lasting address. A v* tag's pipeline has
# the image jobs of .gitlab/ci/container-image.yml, started by hand once the
# release is published: they take the release's signed .deb from the published
# release, check its Sigstore bundle, build this file from it and push
# :<version>, and for a release without a suffix :<major>.<minor> and :latest,
# signed by digest with Sigstore as the release's pipeline, then start what they
# pushed beside PostgreSQL and ask it for SMTP and the REST API. They refuse to
# run until the project's CI/CD variable HM_PUBLISH_IMAGE is 1, which is set once
# the project is at that path; a release built by hand has no image
# (hmailserver/docs/ContainerImage.md). Until a release has one, build it
# yourself (docker build, with the .deb in packages/) and run your own tag. The
# images published before GitHub suspended the project's account on 18 September
# 2026 were on ghcr.io, which has answered 404 since.
#
# The database is not in the image and never will be: point HM_DB_* at a
# PostgreSQL or MariaDB server. On first start the entrypoint writes the
# [Database] section from those variables, the administrator password as a hash
# through the server's own --set-admin-password, creates the database if it does
# not exist, upgrades it if it is behind, stores every setting the environment
# names - the REST listener from HM_REST_*, and any other setting from
# HM_SETTING_<key> or HM_PROPERTY_<name>, or their _FILE forms for a secret -
# through the server's own --apply-environment, and then runs the server in the
# foreground. On Linux the REST API is the
# administration (there is no Control Panel and no COM), and the server requires
# TLS on it unless it is bound to loopback - which inside a container is the
# container's own loopback - so a reachable API means a certificate and key
# mounted in, or host networking. See docker-entrypoint.sh for every variable.
# Ubuntu 24.04 is the base because the package's dependencies are the sonames of
# the runner that built it (see the roadmap's Linux section); a different base
# would refuse the .deb.

FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ARG HM_DEB=packages/hmailserver_*_amd64.deb

# The package's post-install script creates the hmailserver user and the data,
# log and configuration directories, and is written to run where there is no
# systemd. libcap2-bin is for setcap below; ca-certificates for the TLS the
# server does as a client (MTA-STS, ACME, outbound SMTP). 7zip and python3 are
# the package's two Recommends, which --no-install-recommends leaves out, named
# here because the image needs both: the server writes and reads every backup
# through 7-Zip (without it a backup fails with "Could not add the backup
# settings to the archive"), and /usr/bin/hmctl is a Python 3 script.
COPY ${HM_DEB} /tmp/hmailserver.deb
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      /tmp/hmailserver.deb ca-certificates libcap2-bin 7zip python3 \
 && rm -rf /var/lib/apt/lists/* /tmp/hmailserver.deb \
 # Ports 25, 110, 143, 465, 587, 993 and 995 are below 1024 and the process does
 # not run as root: the one capability it needs, on the binary rather than on
 # the container.
 && setcap cap_net_bind_service=+ep /usr/bin/hmailserver

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod 0755 /usr/local/bin/docker-entrypoint.sh \
 && chown -R hmailserver:hmailserver /etc/hmailserver

# What kind of installation this is, for the server's update check: in a
# container a new release is a new image to pull, never a file the server
# downloads and installs over itself, so the check names the image and installs
# nothing (hmailserver/docs/ContainerImage.md, "Updates"). It is not a setting,
# and the entrypoint does not store it as one.
ENV HM_INSTALL_KIND=container

# What the image says of itself, for a registry and for Artifact Hub's listing
# of it (hmailserver/docs/ContainerImage.md). The build adds the version, the
# commit and the commit's time (build/ci/linux-image.sh). ref.name is the base
# image's, Ubuntu's, until it is said again here.
LABEL org.opencontainers.image.title="hMailServer" \
      org.opencontainers.image.description="hMailServer, a mail server for SMTP, IMAP and POP3, on Linux: the release's own .deb, run as its own user, beside a PostgreSQL or MariaDB server of yours" \
      org.opencontainers.image.licenses="AGPL-3.0-or-later" \
      org.opencontainers.image.vendor="Progressive Robot Ltd" \
      org.opencontainers.image.url="https://gitlab.com/hmailserver/hmailserver" \
      org.opencontainers.image.source="https://gitlab.com/hmailserver/hmailserver" \
      org.opencontainers.image.documentation="https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/ContainerImage.md" \
      org.opencontainers.image.ref.name="hmailserver" \
      io.artifacthub.package.readme-url="https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/docs/ContainerImage.md" \
      io.artifacthub.package.license="AGPL-3.0-or-later" \
      io.artifacthub.package.keywords="mail,mail-server,smtp,imap,pop3"

VOLUME ["/var/lib/hmailserver", "/var/log/hmailserver", "/etc/hmailserver"]
EXPOSE 25 110 143 465 587 993 995 4190 8045

USER hmailserver
WORKDIR /var/lib/hmailserver

# Alive means listening: SMTP accepts a connection. No curl in the image; bash
# opens the socket itself.
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --retries=3 \
  CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/25 && exec 3>&-' || exit 1

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["hmailserver", "--foreground"]
