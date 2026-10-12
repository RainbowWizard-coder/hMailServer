The test lab on linix
=====================

The services hMailServer's tests can reach on the project's own machine: a database
matrix, an identity provider, two interoperability peers and an always-on monitor,
on linix (192.168.11.154, Ubuntu 26.04), in Docker, in the port range 10300-10399.
[hmailserver/docs/TestLab.md](../../hmailserver/docs/TestLab.md) says what each is
for, what it measured and what it found; this page is how to run it.

Everything is driven by one script, `lab.sh`, run ON linix as a member of the
`docker` group, from a copy of the tree. Nothing needs sudo.

The files
---------

| File | What it is |
|---|---|
| `docker-compose.yml` | The services, one compose profile each, images pinned by digest |
| `lab.sh` | Everything below: credentials, profiles, the build, the bench, the checks |
| `db-suite.sh` | The Linux server laid out on a lab database, and the Linux suite against it; run by `lab.sh` and by the manual `db-matrix` CI job |
| `mssql-script.py` | A SQL Server script split as the server's `SQLScriptParser` splits it, for `sqlcmd` |
| `keycloak/hmailserver-realm.json` | Realm `hmailserver`: the OIDC client, the token client, the SAML client, alice and bob (no secret in it) |
| `keycloak-setup.py` | Sets the realm's two secrets from the credentials file, and checks the realm as a relying party |
| `idp-signin.py` | OIDC and SAML sign-in through Keycloak into the served server, playing the browser |
| `dovecot/hmlab.conf` | Plain LMTP on 31025 for Postfix, and cleartext login on the lab network |
| `interop.sh` | Postfix and Dovecot against the served server: mail out, mail in, and mbsync both ways |
| `imaptest-each.sh` | imaptest's scripted tests one script at a time, inside Dovecot's imaptest image |
| `kuma/setup.js` | Uptime Kuma's administrator and monitors, through Kuma's own socket API |
| `publish-packages.sh` | A .deb or .rpm to Forgejo's package registries, the token read from a file |
| `push-source.sh` | Run where you work: puts a commit on linix as `~/hmlab/src/<commit>` |
| `active-active/` | A lab of its own, on server2 rather than linix: two or more nodes of the server on one database, for [ActiveActive.md](../../hmailserver/docs/ActiveActive.md); its README says how to run it and what it has measured |

Where things are on linix
-------------------------

| Path | What |
|---|---|
| `~/hmlab/` | The lab's home, mode 700 |
| `~/hmlab/secrets.env` | **Every credential, mode 600.** Made once by `lab.sh init`; never printed, never committed |
| `~/hmlab/ca/` | The lab CA (`lab-ca.pem`, its key 600) and Keycloak's certificate |
| `~/hmlab/src/<commit>/` | A tree, from `push-source.sh` |
| `~/hmlab/build/<commit>/` | Its Linux server (`hmailserver`, `build-info.txt`, the logs) |
| `~/hmlab/runs/` | Every run's results: the TRX, `junit.xml`, the console, the server's error logs |
| `~/hmlab/imaptest/<commit>/` | imaptest's scripted tests, from codeberg.org |
| `~/hmlab/gitlab-token` | Optional, mode 600: a `read_api` token for the two runner monitors |
| `~/.docker/cli-plugins/docker-compose` | Compose 2.40.3, from Ubuntu's `docker-compose-v2` package (below) |
| Docker volumes `hmlab_*` | Each service's data; kept when a service is stopped |
| Docker volume `hmlab-nuget` | The suite's NuGet packages, kept between runs |
| Docker image `hmlab/bench:<CI tag>` | The CI image plus `libmariadb3`, `mariadb-client` and `isync`, built by `lab.sh` |

Once, on a new linix
--------------------

Docker Compose is not installed on linix, and there is no sudo. The plugin comes from
Ubuntu's own archive, as the user (done on 25 September 2026):

    d=$(mktemp -d) && cd "$d" && apt-get download docker-compose-v2
    dpkg-deb -x docker-compose-v2_*.deb x
    mkdir -p ~/.docker/cli-plugins && cp x/usr/libexec/docker/cli-plugins/docker-compose ~/.docker/cli-plugins/
    cd && rm -rf "$d" && docker compose version

(`sudo apt install docker-compose-v2` does the same for every user.) Then, from any tree:

    bash build/lab/lab.sh init          # ~/hmlab, secrets.env, the lab CA
    bash build/lab/lab.sh up core       # Uptime Kuma on 10300, restarted with Docker
    bash build/lab/lab.sh kuma-setup    # its administrator and monitors

Kuma's administrator is `admin`, with `HMLAB_KUMA_ADMIN_PASSWORD` from the credentials
file. For the two GitLab runner monitors, put a `read_api` token of a maintainer of
the project in `~/hmlab/gitlab-token` (mode 600) and run `kuma-setup` again: they are
made paused until then.

Day to day
----------

Where you work, put the commit on linix; there, build it once:

    bash build/lab/push-source.sh                      # HEAD, or name a commit
    ssh chris@192.168.11.154
    cd ~/hmlab/src/<commit> && bash build/lab/lab.sh build   # 15-30 minutes, once

The database matrix:

    bash build/lab/lab.sh up mariadb                   # or mysql, postgresql, mssql
    bash build/lab/lab.sh suite mariadb                # the database-sensitive fixtures
    bash build/lab/lab.sh suite mariadb all            # the whole suite
    bash build/lab/lab.sh suite mariadb 'FullyQualifiedName~IMAP.Search'
    bash build/lab/lab.sh createdb mssql               # an empty database, for a Windows server
    bash build/lab/lab.sh down mariadb                 # the volume stays

The peers, the identity provider and imaptest, against a served server:

    bash build/lab/lab.sh up peers idp
    bash build/lab/lab.sh serve mariadb                # container hmlab-bench
    bash build/lab/lab.sh interop
    bash build/lab/lab.sh imaptest                     # the scripted tests; a number of seconds instead runs its checking load
    bash build/lab/lab.sh idp-check
    bash build/lab/lab.sh unserve
    bash build/lab/lab.sh down peers idp

The published numbers (hmailserver/docs/PublishedNumbers.md has the whole recipe and
the fairness rules), one side at a time, with nothing else running on linix:

    HMLAB_SERVE_STORE=hmlab-numbers-store bash build/lab/lab.sh serve mariadb
    bash build/lab/lab.sh numbers hmailserver prepare
    bash build/lab/lab.sh numbers hmailserver smtp-in --clients 32 --messages 20000 --processes 4
    bash build/lab/lab.sh unserve
    bash build/lab/lab.sh up numbers
    bash build/lab/lab.sh numbers postfix-dovecot smtp-in --clients 32 --messages 20000 --processes 4
    bash build/lab/lab.sh down numbers

And `bash build/lab/lab.sh status` for what runs and what it uses.

The rules it keeps
------------------

* **Ports 10300-10399 only.** 10300 Kuma; 10310-10313 the databases; 10320 and 10321
  Keycloak; 10330 Postfix; 10340 Dovecot; 10350-10353 the served server; 10365 the
  load generator's sink, on linix itself, while `lab.sh numbers ... smtp-out` runs.
* **Memory.** Only Kuma runs all the time. Start a profile for the run that needs it
  and stop it after. SQL Server alone takes 2 GB; never run the whole matrix at once
  beside a suite.
* **Nothing else on linix is touched:** not the host's PostgreSQL 18, MariaDB or
  Postfix, not `~/hmbench`, not the GitLab runner or its image, not Forgejo, and no
  other agent's container. `lab.sh down` removes only the named profile's containers,
  and never the core's.
* **No secret on a command line.** Passwords reach containers through the
  environment by name (`docker exec -e NAME`), SQL clients by `MYSQL_PWD`,
  `PGPASSWORD` and `SQLCMDPASSWORD`, curl by netrc and header files, mbsync by
  `PassCmd`; a process in a container is in the host's process list.
* **Nothing from GitHub.** Images from Docker Hub, quay.io and mcr.microsoft.com,
  packages from Ubuntu's archive, imaptest's tests from codeberg.org.
