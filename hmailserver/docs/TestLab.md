The test lab
============

The services the project's tests reach on its own machine rather than on GitHub, or
rather than on nothing at all: a database matrix, an identity provider, two
interoperability peers, Dovecot's IMAP tester and an always-on monitor. They run in
Docker on linix, the Ubuntu 26.04 lab VM at 192.168.11.154, in the port range
10300-10399. The owner asked for them on 25 September 2026, a week after GitHub
suspended the project's account; they were set up and first run that day.

This page is what the lab is, what it measured and what it found.
[build/lab/README.md](../../build/lab/README.md) is how to run it, and
`build/lab/lab.sh` is the one script that does.

What runs, and what it costs
----------------------------

linix has 8 cores and about 11 GB of memory, and the GitLab runner, the Linux bench's
PostgreSQL, Forgejo and other work share them. So only the core runs all the time;
everything else is a compose profile, started for the run that needs it and stopped
after, its data kept in a volume.

| Service | Profile | Port on linix | Image (registry) | Memory measured, 25 September 2026 |
|---|---|---|---|---|
| Uptime Kuma 2.5.5 | none: **always on** | 10300 | `louislam/uptime-kuma:2.5.5-slim` (Docker Hub) | 146 MiB |
| MySQL 8.4.11 | `mysql` | 10310 | `mysql:8.4` (Docker Hub, official) | 324 MiB during a suite run |
| MariaDB 11.4.13 LTS | `mariadb` | 10311 | `mariadb:11.4` (Docker Hub, official) | 244 MiB during a suite run, 165 MiB fresh |
| PostgreSQL 17.11 | `postgresql` | 10312 | `postgres:17` (Docker Hub, official) | 113 MiB during a suite run, 69 MiB idle |
| SQL Server 2022 (RTM-CU27, 16.0.4295.3) | `mssql` | 10313 | `mcr.microsoft.com/mssql/server:2022-latest` (Microsoft's registry) | 796 MiB after making a database; held to 2 GB by `MSSQL_MEMORY_LIMIT_MB` |
| Keycloak 26.7.4 | `idp` | 10320 (HTTP), 10321 (HTTPS) | `quay.io/keycloak/keycloak:26.7.4` (quay.io) | 593 MiB |
| Postfix 3.10.7 | `peers` | 10330 | `boky/postfix:5.1.0-alpine` (Docker Hub) | 26 MiB |
| Dovecot 2.4.5 | `peers` | 10340 | `dovecot/dovecot:2.4.5` (Docker Hub, Dovecot's own) | 31 MiB |
| imaptest | none: a container for one run | none | `dovecot/imaptest` (Docker Hub, Dovecot's own) | 2 MiB under its checking load |
| The server under test (`hmlab-bench`) | none: `lab.sh serve` or `suite` | 10350-10353 when served | `hmlab/bench`, built on linix from the CI image | 65 MiB served and idle; 525-700 MiB during a suite run, the server and `dotnet test` together |

Every service with a profile has a memory limit in the compose file, above what it
was measured at: MySQL 1 GB, MariaDB 768 MB, PostgreSQL 512 MB, SQL Server 2.5 GB,
Keycloak 1 GB (half of it heap), Kuma 384 MB, Postfix 128 MB, Dovecot 256 MB.
Everything at once is about 2.5 GB, and 3 GB with a suite running; in practice a
run needs one database and at most the peers and the identity provider beside it.

Every image is pinned by digest in `build/lab/docker-compose.yml`. None comes from
GitHub: Docker Hub, quay.io and mcr.microsoft.com for the images, Ubuntu's archive
for packages, and codeberg.org for imaptest's scripted tests.

The server under test is not a lab service. `lab.sh serve` and `lab.sh suite` run it
in a container of their own (`hmlab-bench`), from the Linux build of the commit under
test, on the lab's network under the name `hmailserver`.

### The credentials

Every password and secret the lab uses is in **`~/hmlab/secrets.env` on linix, mode
600**, in a directory of mode 700. `lab.sh init` generates them once, 24 random
letters and digits each, and never changes one afterwards, because a database
volume keeps the password it was first started with. Nothing prints them, and none is
in the repository: the Keycloak realm file holds no password and no client secret
(`build/lab/keycloak-setup.py` sets both from the file), and the scripts hand
passwords to containers and clients through the environment, netrc files and header
files, never on a command line.

| Name | For |
|---|---|
| `HMLAB_MYSQL_ROOT_PASSWORD`, `HMLAB_MARIADB_ROOT_PASSWORD`, `HMLAB_POSTGRES_PASSWORD`, `HMLAB_MSSQL_SA_PASSWORD` | Each database server's administrator (`root`, `postgres`, `sa`) |
| `HMLAB_DB_PASSWORD` | `hmlab`, the application user on all four, which may make and drop databases named `hmlab_*` (on SQL Server, a member of `dbcreator`) |
| `HMLAB_KEYCLOAK_ADMIN_PASSWORD` | Keycloak's bootstrap administrator, `admin` |
| `HMLAB_IDP_USER_PASSWORD` | The realm's two test users, alice and bob |
| `HMLAB_IDP_CLIENT_SECRET` | The realm's confidential client, `hmailserver-token` |
| `HMLAB_KUMA_ADMIN_PASSWORD` | Uptime Kuma's administrator, `admin` |
| `HMLAB_PEER_PASSWORD` | Every user at `peer.test` on Dovecot |
| `HMLAB_BENCH_ACCOUNT_PASSWORD` | The accounts the interop, imaptest and sign-in checks make on the served server |

The lab's own certificate authority is in `~/hmlab/ca/` (the key mode 600); it signs
Keycloak's certificate, and the bench container trusts it.

The database matrix
-------------------

The Linux server has two database backends: PostgreSQL, through libpq, and MySQL,
through a client library it loads at run time (`libmariadb.so.3` first,
`MySQLInterface.cpp`). SQL Server is reached through ADO, which exists only on
Windows, so the Linux server refuses it (HM6390). The CI's `linux-suite` job runs on
PostgreSQL 16 alone. The lab adds:

* **MySQL 8.4 and MariaDB 11.4 LTS**, the other half of what the Linux server
  supports, which no automated run had reached before;
* **PostgreSQL 17**, beside the host's PostgreSQL 18 that the hand-run Linux bench uses
  and that the lab never touches;
* **SQL Server 2022** (the Developer edition, whose licence covers development and
  testing), for the Windows server: a Windows bench can be pointed at
  `192.168.11.154,10313`.

### An empty hMailServer database on each

`lab.sh createdb <db> [name]` makes `hmlab_<name>`:

* on MySQL, MariaDB and PostgreSQL by the server's own `--create-database`, run in the
  bench container against the lab's database: the same `CREATE DATABASE`, the same
  create script and the same `SQLScriptRunner` the server uses everywhere else;
* on SQL Server, where the Linux server cannot connect, by `sqlcmd` inside the SQL
  Server container, fed `CreateTablesMSSQL.sql` split exactly as `SQLScriptParser`
  splits it (line endings made LF, a statement at every blank line, leading
  whitespace dropped, a macro refused), one statement a batch, as the server sends
  them (`build/lab/mssql-script.py`). The database is then given to `hmlab`.

On 25 September 2026, with the Linux server built from this row's base (5bff11257,
schema 6098):

| Database | How | Result |
|---|---|---|
| MariaDB 11.4.13 | `--create-database` | `hmlab_empty`: schema 6098, 123 tables |
| MySQL 8.4.11 | `--create-database` | `hmlab_empty`: schema 6098, 123 tables |
| PostgreSQL 17.11 | `--create-database` (the suite run's database) | schema 6098, 123 tables |
| SQL Server 2022 | `sqlcmd`, 778 statements | refused at the first run, `Msg 156 ... Procedure hm_drop_table_objects: Incorrect syntax near the keyword 'where'`; after the fix below, `hmlab_empty`: schema 6098, 123 tables |

The SQL Server create script had been broken since 16 September 2026: see
[What the first runs found](#what-the-first-runs-found), 3. The server connected to
MariaDB and to MySQL 8.4 as it ships, through Ubuntu's MariaDB client library, with
no `AllowUnencryptedConnection` in the ini - to MySQL 8.4's `caching_sha2_password`
account too, with MySQL just restarted and no other client logged in first. It
connected **without TLS** to both, though both offer it: see
[What the first runs found](#what-the-first-runs-found), 4.

Also proved: `db-suite.sh create` in the bare CI image, on the default bridge
network, against `192.168.11.154:10311` - the `db-matrix` job's own path (below).

### The Linux suite against a database

`lab.sh suite <db> [filter]` lays the Linux server out on that database exactly as
the `linux-suite` job lays it out on PostgreSQL 16 - the listeners moved off the
privileged ports, auto-ban off, the test domain, the administrator password - and runs
`LinuxRegressionTests` against it (`build/lab/db-suite.sh`). One thing differs: the
test domain is made through `POST /api/v1/domains` once the server is up, and not by
the job's `INSERT`. That statement names the columns PostgreSQL has no default for,
and a MySQL-family database has more (a `TEXT` column cannot have a default there),
so on MariaDB it is refused: `Field 'domainquotawarningtext' doesn't have a default
value`.

The filter is `db` by default: the **database-sensitive fixtures**, the ones whose SQL
differs by dialect or leans on the database's own behaviour - name validation and
uniqueness (collation and case), IMAP search, sort and threading over stored headers,
full-text search's index tables, UTF-8 in every column, the domain rename and delete
cascades, message UIDs, the folder tree and its ACLs and shared mailboxes, the
retention and tombstone sweeps (date arithmetic), greylisting and auto-ban
(timestamps and expiry), IP ranges (addresses stored as numbers), scheduled and
verified backup and restore, the SQL layer's own error handling, the audit trail's
revert and password history. `all` runs the whole suite, and anything else is a
`dotnet test --filter` expression.

The database-sensitive fixtures, on 25 September 2026, against the server built from
this row's base (5bff11257), on linix beside other work:

| Database | Passed | Skipped | Failed | Time |
|---|---|---|---|---|
| MariaDB 11.4.13 | 221 | 93 | 1 | 17 min |
| PostgreSQL 17.11 | 222 | 93 | 0 | 15 min |
| MySQL 8.4.11 | 222 | 93 | 0 | 16 min |

Compared test by test, MySQL's run and PostgreSQL's are the same, and MariaDB's
differs from PostgreSQL's in one test,
`ScheduledBackup.TestScheduleStartsABackupWithNoExternalCaller`, and not for the
database: the server's backup runs the program `7z` to write its archive, and the
bench image of that run had no 7-Zip (`HM5401 ... error launching external process
"7z"`). The bench image and `db-suite.sh` install Ubuntu's `7zip` since; the two
backup fixtures, run again on MariaDB, passed, that test among them (1 min 3 s). So
on all three databases **nothing in the database-sensitive set fails**. The 93
skipped are the same 93 on each: fixtures that need a COM member the REST API has no
route for, which the Linux shims skip with the route they lack.

The whole suite (`all`) was not run against a lab database on 25 September: linix
was shared with the integrator's Linux build and suite and with other rows' work,
and a whole run is one and a half to two and a half hours. `lab.sh suite <db> all`
and the job's `HM_SUITE_FILTER=all` run it; the layout is the same, so nothing but
the time stands in the way.

### In CI: the manual db-matrix job

`.gitlab-ci.yml` has a `db-matrix` job on linix, for a protected ref only (as every
job on the project's own runners), **started by hand** from the pipeline's page. It
takes the `linux-build` job's server and runs `build/lab/db-suite.sh` with:

| Variable | Default | What it is |
|---|---|---|
| `HM_LAB_DB` | `mariadb` | `mysql`, `mariadb` or `postgresql` |
| `HM_SUITE_FILTER` | `db` | `db`, `all`, or a `dotnet test --filter` expression |
| `HM_LAB_DB_HOST` | `192.168.11.154` | linix's own address |
| `HM_LAB_DB_NAME` | `hmlab_ci` | Dropped and made again at every run |
| `HM_LAB_DB_PASSWORD` | (none) | **A protected, masked CI/CD variable** holding `HMLAB_DB_PASSWORD` |

The job's container is on the runner's default bridge network, and reaches the lab on
linix's address and the port the lab publishes (10310, 10311, 10312). That was proved
from a container started on `--network bridge`, and then with the job's own script in
the bare CI image: it installed `libmariadb3` and `mariadb-client` from Ubuntu's
archive itself (the CI image has neither) and made `hmlab_ci` on MariaDB at
`192.168.11.154:10311`. The job has not run on the runner, because nothing is pushed.

The lab's databases start on demand, so the database must be up before the job is
started (`bash build/lab/lab.sh up mariadb` on linix); a job started against one that
is down tries for a minute and then fails, saying so. The results are the job's
artifact `db-matrix/` and its JUnit report.

The identity provider
---------------------

Keycloak 26.7 (profile `idp`), in development mode with its own database in a volume,
and the realm in `build/lab/keycloak/hmailserver-realm.json` imported on its first
start. It serves HTTPS on 10321 with a certificate from the lab CA, because the
server takes plain `http` from a loopback address only (`HttpsClient.h`): an issuer
across a network must be `https`. Plain HTTP on 10320 is for the admin console and
the admin API from linix itself.

The realm, `hmailserver`:

| Client | What it matches |
|---|---|
| `hmailserver-oidc` | The server's OpenID Connect sign-in ([PasskeysAndSingleSignOn.md](PasskeysAndSingleSignOn.md)): an authorization-code client, public, PKCE S256 required, redirect URI `<base>/portal/oidc/callback` for the lab bench's base URL `http://127.0.0.1:8045` |
| `hmailserver-token` | Tokens without a browser, for the OAUTHBEARER and XOAUTH2 fixtures and for token introspection: the password grant, the client-credentials grant, the audience `hmailserver`; confidential, its secret set from the credentials file |
| `http://127.0.0.1:8045/saml` | The server's SAML sign-in: the client id is the service provider's entity ID `<base>/saml`, the Response by HTTP-POST to `<base>/portal/saml/acs`, the Response and the Assertion each signed with RSA-SHA256 over exclusive canonicalisation, the AuthnRequest unsigned (the server holds no SAML key), nothing encrypted, the NameID the email address |

The users are **alice** (`alice@example.test`) and **bob** (`bob@example.test`), both
with a verified address - the suite's test domain, so that each maps to a mailbox
there.

`lab.sh idp-check` reads the realm as a relying party does, over HTTPS with the lab
CA (`build/lab/keycloak-setup.py check`), and on 25 September passed every check: the
discovery document, whose issuer is `https://192.168.11.154:10321/realms/hmailserver`;
PKCE S256 offered; the key set (RSA, RS256); the SAML descriptor, its HTTP-Redirect
sign-in URL and signing certificate; a password grant for alice (audience
`hmailserver`, `email_verified` true, five-minute tokens); a client-credentials
grant; and introspection of alice's token (`active`).

Then, with a server served (`lab.sh serve`), it **signs in to that server through
Keycloak, both ways, end to end** (`build/lab/idp-signin.py`, playing the browser):

* **OpenID Connect, as alice:** `/portal/oidc/start` sent the browser to Keycloak's
  authorize endpoint with client `hmailserver-oidc`, PKCE S256 and scope
  `openid email`; alice signed in on Keycloak's own login page; Keycloak sent her to
  `/portal/oidc/callback`, where the server exchanged the code over HTTPS and
  verified the id_token; the server sent her to `/portal#sso=<ticket>`, and
  `POST /api/v1/session` with the ticket answered **201** with a session for
  `alice@example.test`.
* **SAML, as bob:** `/portal/saml/start` sent the browser to Keycloak with an
  AuthnRequest; bob signed in; Keycloak auto-posted a Response to
  `/portal/saml/acs` - Issuer the realm, NameID `bob@example.test` in the
  emailAddress format, two signatures (the Response's and the Assertion's), not
  encrypted; the server verified both and sent the browser to
  `/portal/saml/finish`, which with the `hmsaml` cookie sent it on with a ticket;
  `POST /api/v1/session` answered **201** with a session for `bob@example.test`.

What the check sets on the served server first, over REST: the two accounts, and
`oidc_issuer`, `oidc_client_id`, `oidc_redirect_base_url`, `saml_idp_entity_id`,
`saml_idp_sso_url` and `saml_idp_certificate` (the last taken from Keycloak's own
descriptor). The server trusts Keycloak because the bench container has the lab CA
in its system store.

Two things the realm needed that the product's documents do not say, because they
are Keycloak's: introspection answers `active: false` for a token whose audience
does not name the introspecting client, so `hmailserver-token` puts itself in the
audience; and a client's description is at most 255 characters, or the import
fails.

### Could the fixtures point at it?

For the path that succeeds, yes, and the sign-in check above is the proof; for the
paths that must fail, no, and those are most of the tests. Every one of these
fixtures plays the identity provider itself on the loopback - a `FakeHttpEndpoint`
on `http://127.0.0.1:<port>`, which the server accepts because plain HTTP to the
loopback is allowed - and the reason it does is that it can make the provider
misbehave on purpose: a token signed with the wrong key, an expired one, a wrong
audience, a replayed nonce, a Response with a wrapped or tampered signature, a
provider that answers 500. Keycloak mints only good tokens and good Responses.

| Fixture | Tests | In the Linux suite | What could move to Keycloak | How |
|---|---|---|---|---|
| `API/RestApiOidcSignIn.cs` | 3 | no (Windows only) | The successful sign-in, and the mapping of a verified address to a mailbox | Settings as above; the fixture scrapes Keycloak's login form (`kc-form-login`) as `idp-signin.py` does, and trusts the lab CA (on Windows: `lab-ca.pem` in the machine's Trusted Root store). The realm's redirect URIs must name the bench's base URL |
| `API/SamlSignIn.cs` | 10 | no (Windows only) | The successful sign-in, against signatures made by another implementation (Keycloak's Santuario, with its KeyInfo, which the server must ignore) - what the fixture's own `SignedXml` cannot show | The same; the SAML client's id must be the bench's `<base>/saml` |
| `Security/OAuth2Jwks.cs` | 7 | yes | A token verified against a real JWK Set | `OAuth2JwksUrl` = `https://192.168.11.154:10321/realms/hmailserver/protocol/openid-connect/certs`, `OAuth2Issuer` = the issuer, `OAuth2Audience` = `hmailserver`; the token from a password grant on `hmailserver-token` |
| `Security/OAuth2Introspection.cs` | 5 | yes | An active token's verdict | `OAuth2IntrospectionUrl` = the introspection endpoint, `OAuth2IntrospectionClientId`/`ClientSecret` = `hmailserver-token` and `HMLAB_IDP_CLIENT_SECRET`; revocation could be tested for real by ending the session in Keycloak |
| `Security/OAuth2Bearer.cs`, `OAuth2BearerEs256.cs` | 13, 3 | yes | Little: most set a public key file or an HMAC secret, which Keycloak does not use; ES256 would need an EC key provider added to the realm | |
| `ExternalAccounts/FetchXOAuth2.cs`, `SMTP/OutboundXOAuth2.cs` | 2, 2 | yes | Nothing useful: the token is Keycloak's, but the IMAP and SMTP servers that check it are the fixture's own | |
| `Security/LdapDirectoryAuthentication.cs`, SCIM | | | No: Keycloak is not an LDAP server and has no SCIM client | |

So the lab adds what the fakes cannot give - the proof that a real identity
provider's tokens and Responses are accepted - and the fakes stay for every refusal.
The way to add it, when someone does, is a fixture of its own in a category of its
own, which skips when the lab is not reachable, so that neither suite depends on
linix; not a change to the existing fixtures. `idp-signin.py` is that fixture's
steps, in Python.

The interoperability peers
--------------------------

Profile `peers`: Postfix (`boky/postfix`, Postfix 3.10) as a peer MTA and Dovecot 2.4
(Dovecot's own image) as a peer IMAP server, for the domain `peer.test`. Postfix takes
mail for `peer.test` and hands it to Dovecot over LMTP, and sends mail for
`example.test` - the suite's domain - to the served server. `lab.sh interop` runs four
checks against the served server (`build/lab/interop.sh`):

1. **out**: a submission here, authenticated, to `peer1@peer.test`, relayed by a
   route to Postfix, delivered to Dovecot, read back from Dovecot over IMAP;
2. **in**: a message handed to Postfix for `lab2@example.test`, sent here by Postfix,
   read back here over IMAP;
3. **Dovecot to here**: three messages in a Dovecot mailbox, copied into a mailbox here
   by mbsync, both sides IMAP, and counted here;
3b. the same, with mbsync sending this server one command at a time
   (`PipelineDepth 1`);
4. **here to Dovecot**: the same the other way, this server the source.

mbsync is isync 1.5.1 from Ubuntu's archive, in the bench image; imapsync is packaged
by neither Ubuntu nor Debian.

On 25 September 2026, against the server on MariaDB:

| Check | Result |
|---|---|
| 1 out | **Pass.** The message's `Received:` lines show the path: submitted here (`mail.example.test`), relayed to `mx.peer.test`, delivered by LMTP, read back from Dovecot |
| 2 in | **Pass.** `Received: from mx.peer.test` here, read back over IMAP |
| 3 Dovecot to here | **Fail, three runs out of three.** mbsync appended one message of three and then timed out: it pipelines its APPENDs, and the server answered the first and nothing after it ([What the first runs found](#what-the-first-runs-found), 5) |
| 3b the same, one command at a time | **Pass.** Every message copied |
| 4 here to Dovecot | **Pass.** This server as the source of a sync |

imaptest
--------

Dovecot's IMAP tester. Neither Ubuntu 26.04 nor Debian (trixie or sid) packages it,
so it comes from where Dovecot publishes it outside GitHub: the program from Docker
Hub (`dovecot/imaptest`, built by Dovecot) and its scripted tests - 111 files of
commands and expected answers, one IMAP feature each - from
`codeberg.org/dovecot/imaptest`, at a pinned commit (`dae2f0d16d64`, the last there).
`lab.sh imaptest` runs the scripts against the served server's IMAP, one at a time
(`build/lab/imaptest-each.sh`): the program on Docker Hub is built from a newer tree
than the scripts on Codeberg, its parser refuses one of them ("Missing client
index", `append` line 43), and it reads a whole directory before running any, so one
unreadable script would stop them all. Building the Codeberg source instead, to
match, did not work: it compiles against neither Dovecot 2.3's library (Ubuntu 24.04)
nor 2.4's (Debian trixie, Ubuntu 26.04). `lab.sh imaptest <seconds>` runs its checking
load instead: four clients doing random operations while it checks every answer
against the mailbox state it tracks. Neither had been run against this server before.

**The findings below are imaptest's, from its first run, and are reported, not
fixed.** Four were confirmed by hand at the protocol level, and are marked so;
the others are what imaptest printed, and some may be imaptest being stricter than
the RFC.

The scripted tests, 71 of them, on 25 September 2026 against the server on MariaDB:

| Outcome | Scripts |
|---|---|
| Passed (23) | close, expunge, expunge2, fetch-binary-mime-base64, id, logout, mutf7, nil, pipeline, search-body, search-flags, search-size, select, store, thread2 to thread8, uidplus, uidvalidity-rename |
| Skipped, capability not offered (14) | catenate, catenate-multiappend, esort, notify, search-context-update 1 to 3, search-partial, sort-display-from, sort-display-to, sort-partial, urlauth, urlauth-binary, urlauth2 |
| Not run (3) | append (the parser mismatch above); append-binary and multiappend, which need LITERAL+, and the server offers LITERAL- ("FIXME: Add support for sync literals") |
| Failed (31) | below |

The first failure of each failed script:

| Script | Command | Expected | This server |
|---|---|---|---|
| atoms | `SELECT` of a mailbox that does not exist | `NO` | `BAD Folder could not be found.` **(confirmed)** |
| fetch | `UID FETCH <uidnext>:*` | the highest UID's message (RFC 3501: `*` is the largest UID in use) | nothing **(confirmed)** |
| fetch-envelope | `FETCH 1:* ENVELOPE` | a missing Subject as NIL; an address with no display name as NIL | `""`, and the address itself as the name **(confirmed)** |
| fetch-body, fetch-body-message-rfc822, -mime, -x2, fetch-body-mime | `FETCH 1 BODY.PEEK[TEXT]` and `BODY.PEEK[]` | a literal of the message's own length (9, 206, 409 bytes ...) | a different body; for `BODY[TEXT]`, two bytes longer - a CRLF at the end that the appended message did not have **(confirmed for BODY[TEXT])** |
| fetch-bodystructure | `FETCH 1:* BODY` | the part's line count 1 | 2 |
| fetch-binary-mime-qp | `FETCH 1 (BINARY.SIZE[1])` | 65, the decoded size | 76 |
| fetch-binary-mime | `FETCH 1 (BINARY.PEEK[])` | a literal of 602 bytes | another length |
| copy | `NOOP` after a `COPY` | `* 3 RECENT` | none |
| move | `MOVE 1 <mailbox>` | `OK [COPYUID ...]` (RFC 6851, with UIDPLUS) | no COPYUID |
| list | `LIST "" imaptest.*test4` | the matching mailbox | none: a wildcard between two levels |
| listext | `LIST "" (pattern1 pattern2)` | both | only one |
| subscribe | `LSUB "" imaptest.%` with a subscribed child | the unsubscribed parent as `\Noselect` | not listed |
| search-addresses | `SEARCH FROM user-from@domain.org` | 1 2 3 4 6 7 | 1 2 4 6 7 |
| search-date | `SEARCH SENTBEFORE 24-mar-2007` | none | 1 2 3 |
| search-header | `SEARCH HEADER SUBJECT ""` | 1 | 1 2 |
| search-sets | `UID SEARCH UID 1:4294967295` | 1 3 4 5 6 | 1 |
| esearch | `SEARCH RETURN (ALL) ALL` | `ALL 1:6` | `ALL 1,2,3,4,5,6` (also a valid set; imaptest wants the compact form) |
| esearch-condstore | `FETCH 1 MODSEQ` | `OK [HIGHESTMODSEQ ...]` on SELECT, which CONDSTORE asks for | none |
| sort-arrival | `SORT (REVERSE ARRIVAL) US-ASCII ALL` | 2 4 5 3 1 | 5 4 2 3 1 |
| sort-date | `SORT (DATE) US-ASCII ALL` | 1 3 7 5 2 4 6 | 1 3 2 4 6 7 5 |
| sort-size | `SORT (REVERSE SIZE) US-ASCII ALL` | 4 7 3 5 2 6 1 8 | 7 4 5 3 6 2 8 1 |
| sort-subject | `SORT (SUBJECT) US-ASCII ALL` | 9 10 1 14 3 5 11 6 15 2 7 12 13 8 4 | 9 10 1 14 3 6 2 11 12 13 7 15 5 8 4 |
| sort-addresses | `SORT (FROM) US-ASCII ALL` | 3 1 2 | 3 2 1 |
| thread | `THREAD REFERENCES US-ASCII ALL` | `(3 2)(1)` | `(1 3 2)` |
| thread-orderedsubject, thread-orderedsubject2 | `THREAD ORDEREDSUBJECT US-ASCII ALL` | each thread's first message as the parent of the rest, `(4 (2) (8) (6))` | the thread flat, `(4 2 8 6)`, and the threads in another order |
| uidvalidity | after a rename and recreate | | an untagged answer it did not expect |

The checking load, 30 seconds, four clients, about 14,100 commands: six errors -
three `Keyword used without being in FLAGS` (a keyword in a `FETCH FLAGS` answer
before an untagged `FLAGS` announcing it, which RFC 3501 7.2.6 asks for) and three
`NO Unable to store message flags` on `STORE` and `DELETE` while other clients
worked on the same mailbox.

The monitor
-----------

Uptime Kuma 2.5, always on, at `http://192.168.11.154:10300`, its data in the volume
`hmlab_kuma-data`, restarted by Docker with the machine. Its administrator is `admin`
with `HMLAB_KUMA_ADMIN_PASSWORD`, made without a browser: `lab.sh kuma-setup` drives
Kuma's own socket API (`build/lab/kuma/setup.js`), and can be run again at any time.

| Monitor | Check | Every |
|---|---|---|
| Forgejo | HTTP `http://192.168.11.154:3000/` | 60 s |
| GitLab runner linix | GitLab API `GET /runners/56743206`, `status` must be `online` | 5 min |
| GitLab runner bench150 | GitLab API `GET /runners/56743216`, the same | 5 min |
| Update feed | HTTP `https://updates.progressiverobot.com/` | 5 min |
| bench150 SMTP | TCP `192.168.11.150:25` | 60 s |
| Smoke VM SSH | TCP `192.168.11.175:22` | 60 s |

**The update feed is expected to be down** until the release-chain row deploys it to
GitLab Pages. **The two runner monitors are made paused**, with a placeholder header:
the GitLab API answers a runner's status only to a token that can see the runner.
Putting a `read_api` token of a maintainer of the project in `~/hmlab/gitlab-token`
(mode 600) and running `lab.sh kuma-setup` again gives them the token and starts them.

What the monitors said at 07:10 UTC on 25 September 2026, read from Kuma:

| Monitor | State | Kuma's message |
|---|---|---|
| Forgejo | up | `200 - OK` |
| GitLab runner linix, bench150 | paused | no token yet |
| Update feed | down, as expected | `Hostname/IP does not match certificate's altnames: Host: updates.progressiverobot.com` - the name answers, with a certificate for another host |
| bench150 SMTP | **down** | `Connection failed`; a plain TCP connection from linix to 192.168.11.150:25 timed out as well. Not looked into further: the lab touches bench150 through this port check only |
| Smoke VM SSH | up | 1 ms |

Packages on the forge
---------------------

Forgejo on linix (13.0) has a Debian and an RPM package registry built in, already on:
for the organisation `Progressiverobot` the Debian and RPM repository keys and the
`.repo` file are served, and an upload without a token is refused with 401. Publishing
the Linux packages there gives the package-install tests a repository to install from
as a user would, without GitHub's releases.

`build/lab/publish-packages.sh` publishes `.deb` and `.rpm` files. The token - a
Forgejo access token with the `write:package` scope, of an account that may write
the organisation's packages - is read from a file and handed to curl in a header
file, never on a command line:

    bash build/lab/publish-packages.sh --token-file ~/.config/forgejo-packages.token \
        hmailserver_6.3.3_amd64.deb hmailserver-6.3.3-1.x86_64.rpm

It sends, per file:

    curl -H @<header file: "Authorization: token ..."> --upload-file <file>.deb \
        http://192.168.11.154:3000/api/packages/Progressiverobot/debian/pool/stable/main/upload
    curl -H @<header file> --upload-file <file>.rpm \
        http://192.168.11.154:3000/api/packages/Progressiverobot/rpm/hmailserver/upload

(`--distribution`, `--component` and `--group` change `stable`, `main` and
`hmailserver`; `--replace` deletes a version already there first, since Forgejo
refuses the same version twice; `--dry-run` sends nothing.) A test machine then
installs from it:

    # Debian and Ubuntu
    curl -fsSL http://192.168.11.154:3000/api/packages/Progressiverobot/debian/repository.key \
        -o /etc/apt/keyrings/hmailserver-forgejo.asc
    echo "deb [signed-by=/etc/apt/keyrings/hmailserver-forgejo.asc] http://192.168.11.154:3000/api/packages/Progressiverobot/debian stable main" \
        > /etc/apt/sources.list.d/hmailserver.list
    apt-get update && apt-get install hmailserver

    # Fedora (dnf5)
    dnf config-manager addrepo --from-repofile=http://192.168.11.154:3000/api/packages/Progressiverobot/rpm/hmailserver.repo
    dnf install hmailserver

Nothing was published: the lab has no token with `write:package`, and the forge's
administrator account is not the lab's to use. The script was run with `--dry-run`,
and once with a token that is not one, which Forgejo refused with 401 and the script
reported as such.

What the first runs found
-------------------------

Two were fixed by this row, 1 and 3, because the lab could not work around them and
each is a line or two; the rest are reported and left alone.

1. **The CI's `linux-build` job would have failed at its OpenPGP step on every
   run.** In the CI image's GnuPG 2.4.8, the agent that `gpg --import` starts
   answers the loopback passphrase of the decryption after it with "Bad
   passphrase", as root and as an ordinary user; a fresh agent takes the same
   passphrase. The job had never run, so nobody had seen it; the lab's first build
   of the server did. **Fixed:** `build/ci/linux-build.sh` stops the agent after
   the import (`gpgconf --kill gpg-agent`). Proved at the GnuPG level - the job's
   own import and decryption, in the CI image, fail as the job had them and pass
   with the agent restarted - and not yet by a whole build, because linix's CPUs
   were the integrator's build's at the time.
2. **The CI image has no 7-Zip, and the server's backup runs `7z`.** So
   `ScheduledBackup.TestScheduleStartsABackupWithNoExternalCaller` fails in any run
   on that image - the `linux-suite` job's too; the hand-run Linux bench passes it
   because linix's host has Ubuntu's `7zip`. The lab's bench image and
   `db-suite.sh` install `7zip`; adding it to `build/ci/linux.Dockerfile` is the CI
   row's change (a new image tag), and is not made here.
3. **`CreateTablesMSSQL.sql` would not run on SQL Server.** Commit 974d64c43 (16
   September 2026, the country-blocking fold) left a comma after `from sysobjects`
   in procedure `hm_drop_table_objects` and after `set nocount on` in
   `hm_drop_table_column`. SQL Server refuses both `CREATE PROC` statements, and
   the server stops at the first statement that fails, so a new installation on SQL
   Server has had no database since then. `check-db-scripts.ps1` did not see it:
   it builds the script on SQL Server Compact, which skips every `CREATE PROC`, as
   the server does for that backend. No release has it (6.3.3 was cut the day
   before). **Fixed:** the two commas; the whole script then made a database on SQL
   Server 2022, 778 statements, schema 6098, 123 tables.
4. **The Linux server's connections to MySQL and MariaDB are not encrypted**,
   though both offer TLS. With the application user set to `REQUIRE SSL`, the
   `mariadb` command-line client still connected and the server was refused
   (`Access denied`), on MariaDB 11.4 and on MySQL 8.4 alike; with `REQUIRE NONE`
   it connects, including to MySQL 8.4 just restarted, so the login itself is sound.
   `MySQLConnection.cpp` says the client "negotiates TLS and refuses to continue
   against a server that has none"; that describes the client bundled on Windows,
   not Ubuntu 26.04's `libmariadb3` (1:11.8.6), which the Linux server loads and
   which, asked nothing, does not negotiate TLS. Every query and every row then
   crosses the network in the clear when the database is on another machine. Not
   fixed.
5. **An IMAP command pipelined after an APPEND with a non-synchronising literal is
   never answered.** `APPEND ... {n+}` and a `NOOP` sent in one write: the APPEND is
   answered, the NOOP never (two NOOPs in one write are both answered). mbsync
   pipelines its APPENDs, so copying a mailbox into this server with mbsync stops
   after the first message and times out; with one command at a time it copies
   everything (interop check 3b). Not fixed.
6. **imaptest's findings**, in the imaptest section above: four confirmed by hand
   (`SELECT` of a missing mailbox is `BAD`, not `NO`; `UID FETCH <n>:*` past the last
   UID answers nothing; ENVELOPE's missing Subject and missing display name are `""`
   and the address, not NIL; `BODY[TEXT]` of an appended message is two bytes longer
   than the message's text), the rest as imaptest printed them. Not fixed.
7. **The server's certificate check reads no iPAddress entry.** On Linux a host
   name is checked with `X509_check_host` alone (`CertificateVerifier.cpp`), which
   reads the certificate's dNSName entries and its common name and never its
   iPAddress entries. So an HTTPS endpoint the server is given by address - an
   issuer, a JWK Set - is accepted only when the address is also written as a
   dNSName, as the lab's Keycloak certificate writes it; a certificate from a public
   CA for an address carries it as an iPAddress only. Read in the code and
   provided for in the lab's certificate; not tested the other way. Not fixed.
8. **bench150's port 25 does not answer from linix** (the monitor above). Not
   looked into: the lab touches bench150 through that check only.

And three of the lab's own, found by running it: `linux-suite.sh`'s domain `INSERT`
fails on MySQL-family databases (`db-suite.sh` makes the domain over REST instead);
Keycloak's introspection needs the introspecting client in the token's audience,
and its client descriptions are limited to 255 characters (both provided for in the
realm file); and Kuma drops anything a client sends before its handlers are in
place, which hung the first setup script on a fresh Kuma (`kuma/setup.js` waits for
`loginRequired`).

What the integrator does once
-----------------------------

The lab is set up and running on linix: the compose plugin installed for the user
`chris`, `~/hmlab` initialised, Kuma up with its administrator and monitors, every
image pulled, every volume made. What is left needs a credential the lab does not
have, or a decision that is not the lab's:

1. **The `db-matrix` job's password.** In GitLab, *Settings*, *CI/CD*, *Variables*:
   add `HM_LAB_DB_PASSWORD`, protected and masked, with the value of
   `HMLAB_DB_PASSWORD` from `~/hmlab/secrets.env` on linix (read it there; do not
   paste it anywhere else).
2. **The runner monitors.** Put a `read_api` token of a maintainer of the project in
   `~/hmlab/gitlab-token` on linix, mode 600, and run
   `bash build/lab/lab.sh kuma-setup` again.
3. **Packages on the forge.** A Forgejo token with `write:package` for an account
   that may write `Progressiverobot`'s packages, in a file of mode 600; then
   `build/lab/publish-packages.sh --token-file <file> <packages>`.
4. **7-Zip in the CI image** (finding 2), with the image's new tag, when the CI row
   next changes the Dockerfile.
5. **A whole build through `lab.sh build`**, once linix is free, to prove the
   OpenPGP fix of finding 1 end to end: `bash ~/hmlab/src/<commit>/build/lab/lab.sh
   build --again`. The build proved on 25 September ran the same script in the same
   image as an ordinary user; `lab.sh build` now runs it as root, as the job does.
6. **For a Windows bench:** to use the SQL Server, `lab.sh up mssql` and point the
   bench at `192.168.11.154,10313` as `hmlab`; to use Keycloak, add the bench's base
   URL to the realm file's redirect URIs and import `~/hmlab/ca/lab-ca.pem` into the
   machine's Trusted Root store.
7. **For another user on linix:** `sudo apt install docker-compose-v2`, since the
   plugin is installed for `chris` alone.

What is not here, and why
-------------------------

* **imapsync.** Neither Ubuntu 26.04 nor Debian packages it, and it is otherwise
  distributed from GitHub; mbsync (`isync`, packaged in Ubuntu) does the IMAP-to-IMAP
  copies instead.
* **The Linux suite on SQL Server.** The Linux server cannot use it (HM6390). The
  SQL Server service is for the Windows server and its suite.
* **Anything that would give a CI job Docker.** The `db-matrix` job cannot start a
  database, because that would need the host's Docker socket in the job's
  container, which is root on linix for every job
  ([ContinuousIntegration.md](ContinuousIntegration.md), "The image"). The databases
  are started on linix by hand.
