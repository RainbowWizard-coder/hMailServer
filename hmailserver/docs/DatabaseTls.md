# Encrypted database connections, and certificates for IP addresses

**TLS between this server and its database, the same four words on every
backend - and an HTTPS endpoint named by its address accepted on Linux as it
always was on Windows.**

Two gaps the test lab on linix found on 25 September 2026 (`TestLab.md`, "What the
first runs found", 4 and 7):

* **The connection to MySQL and MariaDB was never encrypted through MariaDB
  Connector/C**, which is the client this server bundles on Windows and the one
  Ubuntu packages (3.4.9 both), and which asks for TLS only when it is told to -
  which this server never did. (Oracle's `libmysqlclient`, which the Linux server
  loads only where there is no `libmariadb`, used its own default: TLS when
  offered.) A database account made with
  `REQUIRE SSL` refused the server; every other one was reached in plaintext,
  password exchange, queries and rows alike, even from a server that offered
  TLS. The code and this repository's own documents said the bundled client
  required TLS by default. It does not, and that was measured, not read.
* **On Linux, a certificate that names an address only as an address was
  refused.** An object store, a webhook, an identity provider or the update feed
  at `https://192.0.2.10/` presents a certificate with the address in an
  `iPAddress` entry, which is where RFC 5280 puts it and the only place a public
  CA may; the Linux check read `dNSName` entries and the common name and never
  that one.

---

## The four modes

`[Database]` in `hMailServer.ini`, beside `Server`, `Username` and `Password`:

```
[Database]
TlsMode=verify-full
TlsCaFile=C:\hMailServer\certs\database-ca.pem
```

| `TlsMode` | What the connection does |
|---|---|
| *(empty, the default)* | **prefer**: TLS whenever the server offers it, and the connection without it when the server offers none - or when the TLS it offers cannot be agreed on, which is one attempt with TLS and one without, and only for that failure. The server's certificate is not checked. |
| `disable` | No TLS. SQL Server's providers still encrypt the login itself. |
| `prefer` | As the default, said explicitly. |
| `require` | TLS or no connection; the certificate is not checked. Encryption against a listener on the wire; not against someone who can stand in for the server. On MySQL and MariaDB through MariaDB Connector/C the refusal of a server without TLS comes after the login exchange, not before it (*What is thinner*, below). |
| `verify-full` | TLS or no connection, the server's certificate checked: its chain to a CA in `TlsCaFile` (or, when that is empty, the system's trust store), and its name - or its address, when `Server=` is an address - against `Server=`. |

`TlsCaFile` is a PEM file of CA certificates, read by `verify-full` and by nothing
else, so that a CA file left beside `require` cannot quietly make it something
stronger or weaker than it says. Keep its path to ASCII: the MySQL client takes it
in the system's code page. A `TlsCaFile` that `verify-full` would read and that is
not there refuses the connection (HM7531) rather than falling back to the system's
store.

Anything else is refused without an attempt and reported (HM7530) - a typo in the
one setting meant to require verification must not switch verification off.
`verify-ca` is refused by name: MariaDB Connector/C and SQL Server's providers
check a certificate's name whenever they check who issued it, so a `verify-ca`
here would be `verify-full` on two backends out of three. PostgreSQL's own
`PostgreSQLSslMode=verify-ca` is there for PostgreSQL.

The keys are read at the service's start, as the rest of `[Database]` is; they are
in the file and not the settings store because they say how the store is reached.

---

## What each backend did, and does

Measured on 25 September 2026: the two MariaDB client builds against MySQL 8.4 and
MariaDB 11.4 - each with a server of its own certificate, one from a CA of the
test's, and one with TLS switched off - and every combination of the client's
options, through the client library itself; SQL Server's providers are Windows'
own.

| Backend | Before | After |
|---|---|---|
| **MySQL / MariaDB, Windows** (the bundled `libmysql.dll`, MariaDB Connector/C 3.4.9 from MariaDB 12.3.2) | **Plaintext, always** - to a server offering TLS as well; an account with `REQUIRE SSL` refused. `AllowUnencryptedConnection=1` set `MYSQL_OPT_SSL_ENFORCE` off, which was already the case: no difference. No way to ask for TLS or a certificate check. | **prefer** by default: TLS whenever offered, unchecked; plaintext when not; one attempt without TLS when the TLS offered cannot be agreed on (client error 2026). `require`, `verify-full` (SChannel, the CA file or the Windows store) and `disable` as above. |
| **MySQL / MariaDB, Linux** (`libmariadb.so.3`, Ubuntu 26.04's 1:11.8.6, Connector/C 3.4.9) | **Plaintext, always**, as on Windows. | As on Windows; the CA file or the system's store (`/etc/ssl/certs`). |
| **MySQL / MariaDB, Linux, Oracle's client** (`libmysqlclient`, loaded only where no `libmariadb` is) | Its own default, PREFERRED: TLS when offered, unchecked. `AllowUnencryptedConnection=1` set option 38 - which to this client is not TLS at all but `MYSQL_OPT_OPTIONAL_RESULTSET_METADATA`, harmlessly to its default. | The same four modes through its own option, `MYSQL_OPT_SSL_MODE`: DISABLED, PREFERRED, REQUIRED, VERIFY_IDENTITY, and `MYSQL_OPT_SSL_CA` - whose number is not MariaDB's, which is why the client is told apart when it is loaded. |
| **PostgreSQL, both platforms** (libpq) | libpq's default, prefer - or `PGSSLMODE` from the service's environment - unless `PostgreSQLSslMode` and `PostgreSQLSslRootCert` said otherwise. | **Unchanged when nothing is set**: no `sslmode` is added, so libpq's prefer (which falls back without TLS on its own) or `PGSSLMODE` stands. `TlsMode` and `TlsCaFile` become `sslmode` and `sslrootcert` when the two PostgreSQL keys are not set; those keys, which came first, win when they are. `verify-full` with no `TlsCaFile` is `sslrootcert=system` on Linux (libpq 16 and later), and libpq's own default file (`%APPDATA%\postgresql\root.crt` of the service's account) on Windows, where OpenSSL's store is not Windows'. |
| **SQL Server, Windows** (ADO) | The provider's default: MSOLEDBSQL 18 and SQL Native Client encrypt the login and nothing after it unless the server forces encryption; `ConnectionStringOptions` appended verbatim. | **The whole session encrypted by default** through the providers this server picks for itself - MSOLEDBSQL, and SQLNCLI for a failover partner - with `Use Encryption for Data=True;Trust Server Certificate=True`: every SQL Server since 2005 can, with the certificate it makes for itself. A failure that reads as TLS takes those two keywords away again for one more attempt, so a server that connected before still does. `MSOLEDBSQL19`, which encrypts and checks by default, is left to its default; SQLOLEDB, which cannot encrypt without checking the certificate against the Windows store, stays unencrypted unless `TlsMode` is `require` or `verify-full`, where it encrypts and checks. (SQLOLEDB takes `Trust Server Certificate` without complaint and does not honour it: against SQL Server 2022 and the certificate it made for itself, it was refused at the handshake - `[DBNETLIB][ConnectionOpen (SECDoClientHandshake()).]SSL Security error` - with the keyword as without it, measured on 25 September 2026.) `verify-full` is `Trust Server Certificate=False` against the Windows store; `TlsCaFile` is not read (HM7532 says so once). `ConnectionStringOptions` naming `Encrypt`, `Use Encryption for Data` or `TrustServerCertificate` wins while `TlsMode` is not set, as before, and is refused beside a `TlsMode` (HM7533). |
| **SQL Server, Linux** | Refused: ADO is Windows (HM6390). | Unchanged. |
| **SQL Server Compact** | A file on this machine. | Unchanged; nothing crosses a network. |

`AllowUnencryptedConnection` is still read and changes nothing: what it described -
TLS preferred, plaintext allowed - is the default now.

---

## Seeing it

**The application log** says, once the service has started and whenever it changes,
how the connection is protected:

```
Database connection: MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate not checked (TlsMode prefer, the default)
Database connection: PostgreSQL: NOT encrypted: the server offered no TLS (TlsMode prefer, the default)
```

For PostgreSQL the line reads the `sslmode` libpq actually used, so a `PGSSLMODE` in
the service's environment is reflected, and named: `..., sslmode verify-full from
libpq's environment (PGSSLMODE)`.

**Diagnostics** - the Control Panel's Diagnostics page and the Control Deck's, over
`POST /api/v1/diagnostics/run` - carry the same line under *Server details*,
beside the database type.

**`--check-database`** tries it before the service is given it. It reads the
configuration, makes one connection with the TLS settings in it, prints the line or
why there is no connection, disconnects and exits - 0 connected, 1 not, 2 not
configured - and changes nothing, so it runs beside a running service:

```
hmailserver --config /etc/hmailserver/hMailServer.ini --check-database
hMailServer.exe --check-database --config C:\hMailServer\try.ini
```

On Windows `--config` names a file other than the installed one for this command
alone (a full path), and the process logs where that file's `[Directories]`
`LogFolder` says. Without it, the installed file is read.

```
Configuration: /etc/hmailserver/hMailServer.ini
Database: MySQL/MariaDB on 192.0.2.20:3306, database hmailserver, as hmailserver
TLS: TlsMode verify-full, the certificate checked against /etc/hmailserver/db-ca.pem
Connected. MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate checked (TlsMode verify-full)
```

**In a container**, the entrypoint writes `[Database]` from the environment at every
start, so a line added to the file by hand does not survive one: `HM_DB_TLS_MODE`
and `HM_DB_TLS_CA` (the path of a PEM file mounted into the container - not a
`_FILE` form) become `TlsMode` and `TlsCaFile` ([EnvironmentConfiguration.md](EnvironmentConfiguration.md)).

---

## What the server does not do for you

The server cannot switch TLS on at the database server, give it a certificate, or
put your CA where the database's client will find it. For `verify-full`:

* **MySQL and MariaDB**: the server's certificate must carry the name or address in
  `Server=` and chain to `TlsCaFile`. A MariaDB 11.4 server's own self-signed
  certificate is also accepted, without a CA file, when the account's password
  proves it (MariaDB's zero-configuration TLS, for `mysql_native_password`,
  `ed25519` and `parsec` accounts); with a CA file named, it is not.
* **PostgreSQL**: `ssl=on` and a certificate in `postgresql.conf`; `hostssl` lines in
  `pg_hba.conf` to make the server insist on it too.
* **SQL Server**: a certificate for the server's name bound to the instance, and its
  CA in the Trusted Root Certification Authorities of the computer hMailServer runs
  on.

A database that must refuse a plaintext connection should say so itself -
`REQUIRE SSL` on the MySQL account, `hostssl` only in `pg_hba.conf`, Force
Encryption on SQL Server - since that holds whatever any client is configured to
do.

---

## Certificates for an address

The HTTPS client (`HttpsClient`), and every other connection that checks a server's
certificate through `CertificateVerifier` - an SMTP route given by address, the
syslog sink, the IMAP migration source, the token endpoint - accepts a certificate
for a host written as an address when the certificate carries that address:

| The host is | Accepted when the certificate has |
|---|---|
| a name, `mail.example.com` | a `dNSName` entry for it (wildcards as RFC 6125 has them), or - with no `dNSName` at all - the common name |
| an address, `192.0.2.10`, `2001:db8::10` or `[2001:db8::10]` in a URL | an `iPAddress` entry for it - **new on Linux** - or, as before, the address written as a `dNSName` (never matched by a wildcard), or as the common name of a certificate that has no `iPAddress` entry |

Windows asked CryptoAPI and had this already: its SSL policy reads `iPAddress`
entries, which was measured on 25 September 2026 (IPv4 and IPv6, and the two older
forms too). On Linux the name was checked with `X509_check_host` alone; now an
address is checked with `X509_check_ip_asc` first. Two things `X509_check_host`
accepted for an address are refused on Linux now, both measured on OpenSSL 4.0.2: a
wildcard (`*.1.2.3` taken to be for `10.1.2.3`), and the common name of a
certificate whose `iPAddress` entries name other addresses (a certificate for
`10.1.2.3` with the common name `10.1.2.4`, taken to be for `10.1.2.4`). Nothing
else that was accepted is refused.

`HttpsClient` also no longer sends an address as the TLS server name: RFC 6066
allows only a host name there, and a server that holds to it ends the handshake.
The IMAP migration client already did the same.

---

## In the error log

| Code | When |
|---|---|
| HM7530 | `TlsMode` is not one of the four (or empty). The connection was not attempted |
| HM7531 | `TlsMode=verify-full` and `TlsCaFile` names a file that does not exist or cannot be read. The connection was not attempted |
| HM7532 | `TlsCaFile` is set and the database is SQL Server, whose provider checks against the Windows store and reads no file. Once a process; nothing is refused |
| HM7533 | `TlsMode` is set and `ConnectionStringOptions` names SQL Server's encryption as well. The connection was not attempted |

A connection refused for TLS - no TLS offered to `require`, a certificate
`verify-full` does not accept - is the connect failure it always was (HM4354 at the
service's start), with the database client's own reason and a sentence saying which
setting to change.

None of these is raised on a server whose `[Database]` has neither `TlsMode` nor
`TlsCaFile`.

---

## What is thinner than it could be

* **No editor in the Control Panel or the Deck.** `[Database]` is the file the
  service reads before it has a database, and none of its keys has one; DBSetup
  writes the rest of the section, and not these. `--check-database` is the way to
  try a change, and the diagnostics line the way to see the result.
* **`require` on the MariaDB client is checked after the login.** Its options cannot
  say "TLS or nothing" without also checking the certificate, so the login is made,
  the connection is asked whether it is encrypted, and one that is not is closed
  before a statement crosses it. The login exchange has then crossed in the clear:
  for `mysql_native_password`, `caching_sha2_password` and `ed25519` accounts a
  challenge and a response, not the password; for an account that signs in through
  PAM or the clear-text plugin, the password. The Oracle client's REQUIRED, and
  `verify-full` on both clients, refuse before the login. And `require` on MySQL
  and MariaDB, through either client, protects against someone who can listen to
  the traffic and not against someone who can alter it: nothing checks whose
  certificate it is, so a machine in the path can present its own. Only
  `verify-full` protects against that.
* **prefer is not proof against someone on the path**, on any backend: a server that
  seems to offer no TLS is connected to without it. That is what `require` and
  `verify-full` are for.
* **SQL Server's `prefer` and `require` differ only at the edges.** Through
  MSOLEDBSQL and SQLNCLI both send the same two keywords, since SQL Server always
  offers TLS; but `prefer` tries once more without them when the failure reads as
  TLS, and `require` does not. On SQLOLEDB, which cannot encrypt without checking
  the certificate, `prefer` asks for nothing and `require` encrypts and checks.
* **Only a login with VIEW SERVER STATE lets SQL Server say whether its session is
  encrypted** (`sys.dm_exec_connections`; `CONNECTIONPROPERTY` has no such property
  and answers NULL). For any other login - an application's, usually - the line says
  what the provider was set to do, which it does or refuses the connection, and a
  server that forces encryption on a session nobody asked to encrypt is described as
  not encrypted.
* **Oracle's `libmysqlclient.so.24`** (MySQL 8.4's client, which Ubuntu 26.04
  packages) is not among the names the Linux server loads; `libmariadb3` is what the
  packages depend on.
* **Only Oracle's 8.x client is given TLS options.** 5.x numbered its options as
  MariaDB does, and 9.x's list was not read, so any other Oracle client is told
  nothing: its own default stands for `prefer` and `disable`, and `require` and
  `verify-full` are refused with the version it reports rather than claimed.

---

## Tests

**In the internal suite** (`ClassTester`, which the Windows suite runs through
`Utilities.RunTestSuite`), with no database and no network:

* `DatabaseTlsTester`: the four words with spacing and case, and `verify-ca`,
  `verify_full`, `true` and `1` refused; the SQL Server keywords for every mode and
  provider kind, `ConnectionStringOptions` naming encryption winning over the
  default and refused beside a mode - and Always Encrypted's column setting not
  taken for connection encryption; which SQL Server failures read as TLS (a wrong
  password does not); the line an administrator reads.
* `CertificateVerifierTester`: `CertificateVerifier::MatchesHost` - the matcher the
  Linux branch uses, compiled on both platforms so that it runs here too - against
  certificates made for the purpose: an address named only as an `iPAddress` entry
  (IPv4, and IPv6 short, long and in brackets) accepted, and another address, a name
  and the empty host refused; an address as a `dNSName` and as the common name still
  accepted, and refused through a wildcard and through the common name of a
  certificate with an `iPAddress` entry; a name matched without regard to case and
  never by an address. The
  first of these is the negative control: `X509_check_host` alone refuses it.

**`RegressionTests/SSL/HttpsAddressCertificates.cs`** (ports 10290 to 10293), with a
TLS listener of its own and the update check as the client: a feed at `127.0.0.1`
and at `[::1]` whose certificate names the address only as an `iPAddress` entry is
read; a trusted certificate for `127.0.0.2`, and one for the name `localhost`, are
refused for `127.0.0.1` at the handshake, for their name (CryptoAPI's
`CERT_E_CN_NO_MATCH`) and not their trust, with nothing asked of the endpoint. The
certificates are trusted by putting them in the computer's Trusted Root store for
the fixture's length, so it needs an elevated run. On Windows these pass before
this change and after it - CryptoAPI read `iPAddress` entries all along - and are
the guard that it stays so.

**`RegressionTests/Infrastructure/DatabaseTlsLab.cs`**, against the test lab's
databases, skipped unless `HMAILSERVER_DBTLS_LAB` names the lab's host and
`HMAILSERVER_DBTLS_PASSWORD` its application user's password (the fixture's header
has the rest). Each test writes a configuration file of its own and runs
`hMailServer.exe --check-database --config` beside the service: MySQL 8.4 and
MariaDB 11.4 encrypted by default and by `require` and not by `disable`, and
`verify-full` refusing their certificates against a CA the fixture made; `verify-ca`
refused without an attempt; PostgreSQL's default connecting whether or not the
server offers TLS, and `require` agreeing with what the default found; SQL Server
encrypted by default through MSOLEDBSQL (or saying why not through SQLOLEDB), and
`verify-full` refusing its own certificate.

**On the lab VM, the Linux server built from this row**: see *The Linux proof*, below.

## The Linux proof

On 25 September 2026, on linix: the Linux server built from this row in the lab's way
(`build/lab/lab.sh build` - clang 21 in the CI image; "The server built, and every
tester program passed"), run in the lab's bench image (Ubuntu 26.04, `libmariadb.so.3`
1:11.8.6, which is Connector/C 3.4.9, and libpq 18.6). The databases: the lab's MySQL
8.4.11 and MariaDB 11.4.13, each with an account `hmtls` made `REQUIRE SSL`; and,
beside them for the run, PostgreSQL 17 with TLS and a `pg_hba.conf` of `hostssl` lines
only, MariaDB 11.4 and MySQL 8.4 with a certificate from a CA of the run's own, and a
MariaDB and a PostgreSQL with TLS switched off. Each line below is one
`hmailserver --check-database` with a configuration of its own, as it printed (the
guidance sentence after a refusal left out where it is the same as above it):

```
-- the lab's MySQL 8.4 and MariaDB 11.4, as hmtls (REQUIRE SSL); the two answer alike
TlsMode not set  Connected. MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate not checked (TlsMode prefer, the default)
require          Connected. MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate not checked (TlsMode require)
disable          Not connected: Access denied for user 'hmtls'@'172.18.0.1' (using password: YES)
                 The connection was made without TLS. If the database account requires it (CREATE USER ... REQUIRE SSL), the server
                 refuses it this way: set TlsMode under [Database] in hMailServer.ini to prefer or stronger.
verify-full, TlsCaFile = a CA that did not issue the server's certificate
                 Not connected: TLS/SSL error: self-signed certificate in certificate chain      (MySQL)
                 Not connected: TLS/SSL error: self-signed certificate                           (MariaDB)
                 TlsMode=verify-full under [Database] in hMailServer.ini requires TLS with a certificate that chains to TlsCaFile
                 (or, when that is empty, to the system's trust store) and names the host or address in Server=. Correct the
                 certificate or TlsCaFile, or set TlsMode=require to encrypt without checking the certificate.

-- PostgreSQL 17, hostssl only
TlsMode not set  Connected. PostgreSQL: encrypted with TLS (TLSv1.3, TLS_AES_256_GCM_SHA384), the server's certificate not checked (TlsMode prefer, the default)
disable          Not connected: connection to server at "192.168.11.154", port 10295 failed: FATAL:  no pg_hba.conf entry for host
                 "172.19.0.1", user "plain", database "postgres", no encryption
verify-full, another CA
                 Not connected: connection to server at "192.168.11.154", port 10295 failed: SSL error: certificate verify failed
verify-full, the CA that issued it
                 Connected. PostgreSQL: encrypted with TLS (TLSv1.3, TLS_AES_256_GCM_SHA384), the server's certificate checked (TlsMode verify-full)

-- verify-full that succeeds: MariaDB 11.4 and MySQL 8.4 with a certificate from the run's CA, for 192.168.11.154
MariaDB          Connected. MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate checked (TlsMode verify-full)
MySQL            Connected. MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate checked (TlsMode verify-full)
MySQL, another CA
                 Not connected: TLS/SSL error: self-signed certificate in certificate chain

-- servers with TLS switched off: the default still connects
MariaDB, not set Connected. MySQL/MariaDB: NOT encrypted: the server offered no TLS (TlsMode prefer, the default)
MariaDB, require Not connected: The MySQL/MariaDB server at 192.168.11.154 did not offer TLS, and TlsMode is require: the
                 connection was closed before it was used.
MariaDB, verify-full
                 Not connected: TLS/SSL error: SSL is required, but the server does not support it
PostgreSQL, not set
                 Connected. PostgreSQL: NOT encrypted: the server offered no TLS (TlsMode prefer, the default)
PostgreSQL, require
                 Not connected: connection to server at "192.168.11.154", port 10296 failed: server does not support SSL, but SSL was required

-- a mode the server does not know
verify-ca        Not connected: TlsMode under [Database] in hMailServer.ini is 'verify-ca'; it must be disable, prefer, require or
                 verify-full, or left empty for prefer. (...) The connection was not attempted.
```

Then the server itself, on the lab's MariaDB as `hmtls` with nothing about TLS in its
configuration: `--create-database` made the database at schema 6100, the server started,
and the application log and `POST /api/v1/diagnostics/run` said

```
Database connection: MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate not checked (TlsMode prefer, the default)
```

while MariaDB listed six sessions of `hmtls` from the server - each of them TLS, since the
account accepts nothing else.

**Before this row**, the same lab's earlier build of the Linux server (from 5bff11257,
whose MySQL connection code and `CertificateVerifier` are those this row changed), given
the same account: `Could not connect to the database server: Access denied for user
'hmtls'@'172.18.0.1' (using password: YES)`, from MySQL 8.4 and from MariaDB 11.4 alike.

**Certificates for an address**, the update check (`POST /api/v1/update/check`) against
three HTTPS feeds at `127.0.0.1`, each with a certificate from the run's CA, which the
container trusts:

| The certificate names | This row's build | The earlier build |
|---|---|---|
| `iPAddress` 127.0.0.1, nothing else | read: state 1, up to date | refused: `certificate verify failed`; "The certificate is not valid for the expected host 127.0.0.1." |
| `iPAddress` 127.0.0.2 | refused, the same words | - |
| `dNSName` localhost | refused, the same words | refused, the same words |
| The server name the client sent | none | `127.0.0.1` |

## Where it lives

| | |
|---|---|
| The modes, the keywords, the line, `--check-database` | `hmailserver/source/Server/Common/SQL/DatabaseTls.h` and `.cpp` |
| MySQL and MariaDB | `MySQLConnection::ConnectOnce_` and `ApplyTlsOptions_`; the two clients' option numbers in `MySQLInterface.h` |
| PostgreSQL | `PGConnection::Connect` |
| SQL Server | `ADOConnection::Connect`, `OpenOnce_` and `ReadEncryptOption_` |
| The two keys | `IniFileSettings` (`GetDatabaseTlsMode`, `GetDatabaseTlsCaFile`); `build/ini-settings-baseline.txt` |
| The line in the diagnostics | `Common/Diagnostics/TestInformationGatherer.cpp` |
| The command | `hMailServer/hMailServer.cpp` and `platform/main.cpp` |
| The container | `platform/packaging/docker/docker-entrypoint.sh` |
| Certificates for an address | `CertificateVerifier::MatchesHost` (`Common/TCPIP/CertificateVerifier.cpp`); the server name in `HttpsClient.cpp` |
