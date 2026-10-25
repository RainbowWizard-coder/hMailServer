# Changelog of progressiverobot.hmailserver

The collection is versioned with the release of Progressive Robot's fork of
hMailServer it is built for: its version is that release's, and on Debian its role
installs that release's `.deb`, held to the SHA-256 the release published. The
server's own changes are in the project's
[CHANGELOG.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/CHANGELOG.md).
Each version the collection is built for has a heading of its own here, written
before it is built: the job that builds it for a release refuses one without.

## 6.3.7

The first version of the collection.

- The role `progressiverobot.hmailserver.hmailserver`: installs the server from the
  project's Ubuntu PPA (24.04 and 26.04), the release's `.deb` (Debian 12 and 13),
  its Fedora Copr (Fedora, and Enterprise Linux 10) or its openSUSE Build Service
  project (Leap 16.0 and Tumbleweed), each verified as that channel allows; points it
  at a PostgreSQL or MariaDB database with `--create-database` or an existing one;
  sets the administrator password, never logged; makes and can export the
  stored-secrets key; stores `[Settings]` values; starts and enables the service; and
  makes domains and accounts over the REST API.
- Molecule scenarios for Ubuntu 26.04 and 24.04, Debian 13 (against PostgreSQL and
  against MariaDB) and 12, Fedora 44, AlmaLinux 10, and openSUSE Leap 16.0 and
  Tumbleweed.
