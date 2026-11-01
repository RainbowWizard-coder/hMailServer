# Encrypted database connections, and certificates for IP addresses

**TLS between this server and its database, the same four words on every
backend - and an HTTPS endpoint named by its address accepted on Linux as it
always was on Windows.**

Two gaps the test lab on linix found on 25 September 2026 (`TestLab.md`, "What the
first runs found", 4 and 7):

* **The connection to MySQL and MariaDB was never encrypted through MariaDB
  Connector/C**, which is the client this server bundles on Windows (3.4.11 since
  6.3.7, 3.4.9 before it; see below) and the one Ubuntu packages, and which asks
  for TLS only when it is told to -
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
| `verify-full` | TLS or no connection, the server's certificate checked: its chain to a CA in `TlsCaFile` (or, when that is empty, the system's trust store), and its name - or its address, when `Server=` is an address - against `Server=`. Two exceptions are MariaDB Connector/C's, the MySQL and MariaDB client this server bundles and the one the Linux packages suggest: on a connection it takes as one to this machine - `Server=` exactly `127.0.0.1`, `::1` or `localhost`, or empty (a Unix socket on Linux), or `.` for a named pipe on Windows, or a `Server=` with a comma, which the client reads as a list of servers to try in turn, holding one of those, since which of them answers is known only once one has - it checks the chain against `TlsCaFile` and never the name, and with no `TlsCaFile` neither the issuer nor the name, only the certificate's dates, so `verify-full` there without a `TlsCaFile` is refused without an attempt (HM7535); and over the network with no `TlsCaFile`, a certificate the system's store does not vouch for is still taken from a MariaDB server that proves it holds the account's password (*What the server does not do for you*, below). Oracle's client refuses `verify-full` without a `TlsCaFile`. |

`TlsCaFile` is a PEM file of CA certificates, read by `verify-full` and by nothing
else, so that a CA file left beside `require` cannot quietly make it something
stronger or weaker than it says. A `TlsCaFile` that `verify-full` would read and that
is not there refuses the connection (HM7531) rather than falling back to the system's
store, and so does one whose name cannot be handed to the database's client as it is
named (HM7534). The client opens the file by its name in an encoding of its own: on
Linux the file system's bytes, UTF-8, which every name outside ASCII can be written
in; on Windows the ANSI code page, in which MariaDB Connector/C's Schannel build, the
client this server bundles, opens it (`CreateFileA`) and libpq looks for it, so that a
name with a character the code page does not have - a Cyrillic or CJK letter on code
page 1252 - is refused there whichever client is loaded, since the name is judged
before the client is known. libpq hands those bytes to OpenSSL, which reads a name as
UTF-8 first and opens the file at that reading when there is one: a name whose bytes
in the code page also read as UTF-8 is therefore refused too, `TlsCaFile` and
`PostgreSQLSslRootCert` alike. No other name outside ASCII reads as UTF-8 in a code
page that is not UTF-8. Within the code page, Oracle's client is handed the name's
UTF-8, which its OpenSSL reads before the code page's bytes and which names the same
file. The TLS line `--check-database` prints names the file that was handed over - for
PostgreSQL that is `PostgreSQLSslRootCert` when it is set, which wins over `TlsCaFile`,
and the line says so and that `TlsCaFile` is not used - and the log's line names no
file: it says whether the certificate was checked, and how where the client checks
less than `verify-full`'s words: on a MariaDB client's connection to this machine,
`the server's certificate checked for its issuer alone, not for its name, on a
connection to this machine`; and for PostgreSQL under `require`, `prefer` or `allow`
with a root certificate file handed to libpq, which libpq reads under any mode, `...
checked for its issuer alone, against the root certificate libpq was handed, which it
reads under sslmode require too, not for its name`, where up to 6.3.7 that line said
the certificate was not checked.

**Up to 6.3.7 a name with a character outside ASCII on Linux, or above U+00FF on
Windows, was handed to the MySQL and MariaDB client as nothing**, so that the client
was given no CA file and checked the server's certificate against the system's trust
store, or with some versions of MariaDB Connector/C less than that: on a connection
the client takes as one to this machine, with 3.4.1 and later, neither its issuer nor
its name; over the network, with 3.4.2 to 3.4.10 CONC-853, and with 3.4.1 to 3.4.9
CVE-2026-107819 (under *What is thinner than it could be*) - while `--check-database`
said it was checked against `TlsCaFile`, and the log that it had been checked. With
`LANG` unset, and on Alpine, such a name failed the connection instead. From 6.3.6
PostgreSQL's root certificate was handed to libpq, in UTF-8 on Linux and in the code
page on Windows (in 6.3.4 and 6.3.5 its whole connection string went through the same
conversion, which 6.3.6 fixed). From the release after 6.3.7 the name is passed as
above, or refused.

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
| **MySQL / MariaDB, Windows** (the bundled `libmysql.dll`, MariaDB Connector/C 3.4.11 since 6.3.7, 3.4.9 before it) | **Plaintext, always** - to a server offering TLS as well; an account with `REQUIRE SSL` refused. `AllowUnencryptedConnection=1` set `MYSQL_OPT_SSL_ENFORCE` off, which was already the case: no difference. No way to ask for TLS or a certificate check. | **prefer** by default: TLS whenever offered, unchecked; plaintext when not; one attempt without TLS when the TLS offered cannot be agreed on (client error 2026). `require`, `verify-full` (SChannel, the CA file or the Windows store) and `disable` as above. |
| **MySQL / MariaDB, Linux** (`libmariadb.so.3`, Ubuntu 26.04's 1:11.8.6, Connector/C 3.4.9) | **Plaintext, always**, as on Windows. | As on Windows; the CA file or the system's store (`/etc/ssl/certs`). The distribution's client, which the distribution patches: CVE-2026-107819 and MariaDB's CONC-853, below, are the distribution's to fix, and Connector/C 3.4.11 or later fixes both; until then `TlsCaFile` set is the mitigation, from 6.3.8, which hands its name over or refuses it (HM7534) - before 6.3.8 a name outside ASCII was handed over as none. |
| **MySQL / MariaDB, Linux, Oracle's client** (`libmysqlclient`, loaded only where no `libmariadb` is) | Its own default, PREFERRED: TLS when offered, unchecked. `AllowUnencryptedConnection=1` set option 38 - which to this client is not TLS at all but `MYSQL_OPT_OPTIONAL_RESULTSET_METADATA`, harmlessly to its default. | The same four modes through its own option, `MYSQL_OPT_SSL_MODE`: DISABLED, PREFERRED, REQUIRED, VERIFY_IDENTITY, and `MYSQL_OPT_SSL_CA` - whose number is not MariaDB's, which is why the client is told apart when it is loaded. |
| **PostgreSQL, both platforms** (libpq) | libpq's default, prefer - or `PGSSLMODE` from the service's environment - unless `PostgreSQLSslMode` and `PostgreSQLSslRootCert` said otherwise. | **Unchanged when nothing is set**: no `sslmode` is added, so libpq's prefer (which falls back without TLS on its own) or `PGSSLMODE` stands. `TlsMode` and `TlsCaFile` become `sslmode` and `sslrootcert` when the two PostgreSQL keys are not set; those keys, which came first, win when they are. `verify-full` with no `TlsCaFile` is `sslrootcert=system` on Linux (libpq 16 and later), and libpq's own default file (`%APPDATA%\postgresql\root.crt` of the service's account) on Windows, where OpenSSL's store is not Windows'. |
| **SQL Server, Windows** (ADO) | The provider's default: MSOLEDBSQL 18 and SQL Native Client encrypt the login and nothing after it unless the server forces encryption; `ConnectionStringOptions` appended verbatim. | **The whole session encrypted by default** through two of the three providers this server picks for itself when `[Database] Provider` is empty - MSOLEDBSQL, when version 18 or later is installed, and SQLNCLI for a failover partner - with `Use Encryption for Data=True;Trust Server Certificate=True`: every SQL Server since 2005 can, with the certificate it makes for itself. The third is SQLOLEDB, which the server picks when MSOLEDBSQL 18 or later is not installed, and on it only the login is encrypted (below). A failure that reads as TLS takes those two keywords away again for one more attempt, so a server that connected before still does. `MSOLEDBSQL19`, which encrypts and checks by default, is left to its default; SQLOLEDB, which cannot encrypt without checking the certificate against the Windows store, encrypts only the login unless `TlsMode` is `require` or `verify-full`, where it encrypts the session and checks. (SQLOLEDB takes `Trust Server Certificate` without complaint and does not honour it: against SQL Server 2022 and the certificate it made for itself, it was refused at the handshake - `[DBNETLIB][ConnectionOpen (SECDoClientHandshake()).]SSL Security error` - with the keyword as without it, measured on 25 September 2026.) `verify-full` is `Trust Server Certificate=False` against the Windows store; `TlsCaFile` is not read (HM7532 says so once). `ConnectionStringOptions` naming `Encrypt`, `Use Encryption for Data` or `TrustServerCertificate` wins while `TlsMode` is not set, as before, and is refused beside a `TlsMode` (HM7533). |
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
  `ed25519` and `parsec` accounts); with a CA file named, it is not, and the
  settings line of `--check-database` and the log's line say so. To a database on
  this machine - `Server=127.0.0.1`, `::1`, `localhost` or empty, or a list of servers
  with a comma that holds one of those - MariaDB Connector/C
  checks the issuer and not the name, so `TlsCaFile` must name a CA that issues
  nothing but the database's certificates (not the system's whole bundle), and
  without it `verify-full` is refused (HM7535). **Up to 6.3.7 such a connection with
  no `TlsCaFile` was made, with neither the certificate's issuer nor its name
  checked**, while
  `--check-database` said it had been checked against the system's trust store and
  the log that it had been checked.
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

## File names handed to OpenSSL on Windows

The same rule holds for every file the server itself hands to OpenSSL by its name
on Windows (`FileUtilities::OpenSslPath`): a TLS certificate's certificate and
private key files - and `RestApiCertificateFile`, `MetricsServerCertificateFile` and
`WebServicesCertificateFile` with their key files - the Diffie-Hellman parameters beside the program
(`dh2048.pem`), a TCP/IP port's client-certificate CA file, the object store's
`MessageStoreObjectCaFile`, `OAuth2PublicKeyFile`, and the files in
`AcmeCertificateDirectory` - `fullchain.pem`, `privkey.pem` and `account.key`. Each
is handed over in the ANSI code page, or in UTF-8 when the code page is UTF-8, and a
name with a character the code page does not have, with characters whose bytes in
it also read as another name in UTF-8, or with a U+0000 is refused: the file is not
read (nor, in the ACME directory, written), and HM7536 says which it was. Such a
name refuses what that file was for: a port whose certificate or key it names does
not start, as one whose file cannot be loaded does not; a port's client-certificate
CA file verifies no certificate, so a port that requires one does not start and one
that requests one starts verifying none; the object store's CA file makes no
request; the public key verifies no token; the ACME client neither reads nor writes
its files there and orders no certificate; and `dh2048.pem` is left out, as a
missing one is. A name in ASCII is handed over as before; on Linux any name goes
over in UTF-8, which names the file itself.
Give such a file, and the folders above it, a name in the code page that does not
also read as UTF-8 - a name in ASCII always does. `OAuth2PublicKeyFile` was handed
over in UTF-8 up to 6.3.7, so a name with a character outside the code page, which
worked then, is refused now.

The Control Panel holds the same rule where it reads these files on the server's
own computer, since it reads them by their wide names, which always work: the SSL
certificates page shows such a certificate or key file as *Name refused*, at
Critical, and the setup checklist's items on the TLS private keys, the client
certificate CA files and OAuth2 say that the server does not read it, with error
7536, rather than calling the file good.

---

## In the error log

| Code | When |
|---|---|
| HM7530 | `TlsMode` is not one of the four (or empty). The connection was not attempted |
| HM7531 | `TlsMode=verify-full` and `TlsCaFile` names a file that does not exist or cannot be read. The connection was not attempted |
| HM7532 | `TlsCaFile` is set and the database is SQL Server, whose provider checks against the Windows store and reads no file. Once a process; nothing is refused |
| HM7533 | `TlsMode` is set and `ConnectionStringOptions` names SQL Server's encryption as well. The connection was not attempted |
| HM7534 | `TlsMode=verify-full` and `TlsCaFile`, or for PostgreSQL `PostgreSQLSslRootCert`, names a file whose name cannot be handed to the database's client as it is named: on Windows a character the ANSI code page does not have, or characters whose bytes in it also read as another name in UTF-8, which OpenSSL opens first; on either platform half of a surrogate pair or a U+0000. The connection was not attempted |
| HM7535 | `TlsMode=verify-full` on MySQL or MariaDB through MariaDB Connector/C, with no `TlsCaFile`, and `Server=` a connection the client takes as one to this machine (`127.0.0.1`, `::1`, `localhost`, an empty `Server=`, a socket or a named pipe), or a list of servers with a comma that holds one of them, on which it would check neither the certificate's issuer nor its name. The connection was not attempted |
| HM7536 | On Windows, a file the server hands to OpenSSL by its name - a TLS certificate's certificate or key file (the REST API's, the metrics listener's and the web services' among them), `dh2048.pem`, a port's client-certificate CA file, `MessageStoreObjectCaFile`, `OAuth2PublicKeyFile`, a file in `AcmeCertificateDirectory` - has a name with a character the ANSI code page does not have, characters whose bytes in it also read as another name in UTF-8, which OpenSSL opens first, or a U+0000. The file was not read, and what it was for was refused (*File names handed to OpenSSL on Windows*, above). The certificate alert, the expiry metric, the object store's requests, token verification and the ACME client say it once for a name |
| HM7537 | On Windows, with MariaDB Connector/C as the client, the client's authentication-plugin folder, `Bin\plugin`, cannot be handed to it by a name it reads, and neither can the folder under the Windows system folder that stands in for it ([ThirdPartyBinaries.md](ThirdPartyBinaries.md), the Connector/C row). The connection was not attempted |

A connection refused for TLS - no TLS offered to `require`, a certificate
`verify-full` does not accept - is the connect failure it always was (HM4354 at the
service's start), with the database client's own reason and a sentence saying which
setting to change.

None of these but HM7537, which is not about TLS, is raised on a server whose
`[Database]` has neither `TlsMode` nor `TlsCaFile`.

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
  packages suggest and the server loads.
* **CVE-2026-107819, in MariaDB Connector/C 3.4.1 to 3.4.9, reached only
  `verify-full` with no CA file handed to the client.** When the client checked the
  certificate's host name against `Server=` and it failed - a certificate a trusted
  authority issued, for another name - the authentication switch that follows did
  not refuse a plugin that sends the password unhashed, so a man in the middle with
  such a certificate could ask for `mysql_clear_password` and read the password
  inside the TLS connection it stood up. It was reached only with
  `TlsMode=verify-full`, a database on another host and no CA file handed to the
  client (so the system's store decides who is trusted): an empty `TlsCaFile`, or,
  before 6.3.8, a `TlsCaFile` whose name held a character outside ASCII on Linux,
  or above U+00FF on Windows, which the client was handed as no file at all - so
  for such a name a named `TlsCaFile` was no mitigation. 6.3.8 hands the name over
  or refuses it (HM7534). `prefer` and `require` do not check the certificate,
  `disable` does not use TLS, and a CA file handed to the client refuses any
  mismatch before the login. The Windows build ships Connector/C 3.4.11 since
  6.3.7, which refuses the switch on any failed check (the fix is 3.4.10's, a
  version MariaDB tagged and never released:
  [ThirdPartyBinaries.md](ThirdPartyBinaries.md), "Connector/C 3.4.11, built from
  MariaDB's source"); on Linux it is the distribution's client, which should be
  Connector/C 3.4.11 or later: that fixes CVE-2026-107819 and MariaDB's CONC-853,
  by which 3.4.2 to 3.4.10, with no CA file, take a certificate whose issuer they
  cannot find unchecked (a client with CVE-2026-107819's fix alone still does). On a
  distribution that has not patched them, a `TlsCaFile` naming the CA, on 6.3.8 or
  later, is the mitigation. What shows the fix is the source - 3.4.11's `my_auth.c` refuses after any
  failed check, 3.4.9's after a failed trust check alone - and the built
  `libmysql.dll`, which carries 3.4.11's refusal message where 3.4.9's does not. The
  flaw itself needs a certificate that a root in Windows' own store trusts, issued
  for another name, and no test here stages one. What was measured, on 12 October
  2026 with `--check-database` through both clients against MariaDB 11.8 and MySQL
  8.4, is the mitigation and that the swap changes nothing else: with a `TlsCaFile`,
  over the network (on a connection to this machine the client checks no name:
  *The four modes*, above), `verify-full` refuses a certificate for another host
  name before the login on
  3.4.9 and 3.4.11 alike, and the nineteen other cases - each authentication plugin,
  `REQUIRE SSL`, a server without TLS, a wrong password, a certificate the CA did
  not issue, and a MariaDB server's own certificate taken on the password's proof -
  came out the same on both.
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
