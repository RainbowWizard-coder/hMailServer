The evidence pack
=================

What a buyer's security or procurement team asks about a mail server, answered
from this repository - the code, the build and release scripts and the
documents beside them - with the place each answer can be checked. Where
something is not in place, this says so beside what is.

Written 25 September 2026 against the source tree at schema 6100. The latest
release is 6.3.3, published on 15 September 2026 at schema 6040. A feature
marked *(unreleased)* is in the tree after 6.3.3 and not yet released: a server
running 6.3.3 does not have it ([CHANGELOG.md](../../CHANGELOG.md), *Unreleased*).
Since it was written, 6.3.4 (27 September 2026, built and signed by hand),
6.3.5 (29 September 2026) and 6.3.6 (6 October 2026) were published; 6.3.5 was
the first release the GitLab release jobs made, every file with a Sigstore
bundle, so where the text below says those jobs have not run, they have.
"Off by default" means the value a new installation has, read from the defaults
in the code: `SettingsDefaults.h` for the `hm_settings` rows and
`IniSettingsBaseline.h` for the `[Settings]` keys, both in
`hmailserver/source/Server/Common/Application/` and both held to the code by
`build/generate-settings-defaults.py --check` and `build/check-ini-coverage.py`.

**This is not an audit report, and nothing in it is one.** Neither the product
nor the project behind it has been audited, certified or attested against
ISO/IEC 27001, SOC 2 or any other framework. *Control mappings* below maps the
*product*: which controls it gives an operator the means to meet. Whether an
installation meets them depends on how it is configured and run, which is the
operator's part, and the tables say where that part begins.

The sections, in order:

* What the product is, and where the mail is
* Control mappings: ISO/IEC 27001:2022 Annex A and SOC 2
* The software supply chain
* Vulnerability handling
* Secure development
* Cryptography in use
* Access control and logging
* Business continuity
* Regulatory scope
* Not in place, in one list
* Checking this document

What the product is, and where the mail is
------------------------------------------

hMailServer is a mail server - SMTP, IMAP and POP3, with an optional REST API,
a webmail, a browser administration console (the Control Deck), CardDAV and
CalDAV - that the organisation using it installs and runs on its own machine:
64-bit Windows from Windows 10 1607 and Windows Server 2016, or, since 6.3.0,
Linux on x86-64 and AArch64 as a `.deb`, an `.rpm` or an AppImage
([README.md](../../README.md), *Supported platforms* and *Linux and AArch64*).
It is free software under the GNU AGPL, version 3 or later
([LICENSE](../../LICENSE)). The project is a maintained fork of hMailServer 5.7,
stewarded by Progressive Robot Ltd and run by two maintainers, both at that
company ([GOVERNANCE.md](../../GOVERNANCE.md)).

Three things follow for a questionnaire:

* **The project does not process your data.** It runs no hosted service
  ([RegulatoryScope.md](RegulatoryScope.md), *Who is asking*). The mail, the
  accounts and the logs are on machines the operator controls, and nothing in
  the product sends them to the project.
* **The server reports nothing to the project.** There is no telemetry: the
  `sendstatistics` row a create script still seeds is read by no code
  (`EnvironmentSettings.cpp` names it among the rows nothing reads), and the one
  call the server can make to the project, the update check, is off until an
  administrator turns it on (`UpdateCheckEnabled` = 0).
* **The operator configures the security.** A new installation is a working
  mail server with TLS not yet configured (below), and most of what this
  document describes is off until it is switched on.

### Where mail and personal data are kept

| What | Where | At rest, by default |
|---|---|---|
| Messages in mailboxes | One file per message under the data directory (`[Directories] DataFolder`: `<install>\Data` on Windows, `/var/lib/hmailserver` on Linux), by domain and account | As received, RFC 822 byte for byte. With `MessageStoreCompression` on (off by default) a deflated container with large parts held once ([MessageStore.md](MessageStore.md)) *(unreleased)*. With encryption at rest switched on for an account (off for every account) encrypted to that account's own key ([EncryptionAtRest.md](EncryptionAtRest.md)) *(unreleased)* |
| The delivery queue | Files directly in the data directory, and rows in the database | As received, always: a queue file is never compressed or encrypted ([MessageStore.md](MessageStore.md); [EncryptionAtRest.md](EncryptionAtRest.md), *What stays in the clear*) |
| Configuration, accounts and what is indexed about mail | The database: MySQL or MariaDB, Microsoft SQL Server, PostgreSQL, or SQL Server Compact (Windows only, and what the Windows installer picks unless told otherwise - [MigratingDatabaseBackend.md](MigratingDatabaseBackend.md)) | Account passwords as salted hashes and most stored credentials under the machine's secret store (*Cryptography in use*); everything else as the database keeps it, so its encryption at rest is the database's. This includes each message's size, folder, flags and dates, the headers kept for sorting, contacts, calendars, the webmail's preferences, the message trace and the audit trail |
| Stores beside the mailboxes | Directories of the data directory, each made on first use by its feature: `Quarantine`, `Recoverable` (deleted items kept for recovery), `Blobs` (parts held once), `Files` (files sent as links), `Confidential` (messages that expire), `Protected` (password-protected messages, which are ciphertext), `Lists`, `Discovery`, `DKIM` (keys the rotation made), `ACME`, `Sieve` and `Updates` | As written. Encryption at rest covers the messages in an account's folders and not these ([EncryptionAtRest.md](EncryptionAtRest.md), *What stays in the clear*) |
| Logs | The log folder (`<install>\Logs`, `/var/log/hmailserver`); a message the MIME parser cannot read is copied to its `Problematic messages` | In the clear. *Access control and logging* says what they contain and how long they are kept |
| `hMailServer.ini` | Beside the executable on Windows, `/etc/hmailserver` on Linux | The database connection, with its password under the secret store when the server or the setup tool writes it (a password typed into the file with `Passwordencryption=0`, as the Linux package's template has it, is read as plain text); the administrator password's hash and second-factor secret. API keys are in `hMailServerApiKeys.ini` beside it, as SHA-256 digests |
| Backups | The backup destination, as 7-Zip archives | No destination is set and no backup is scheduled by default. Encrypted with AES-256 once a passphrase is set (off by default) *(unreleased)* ([Backup.md](Backup.md)) |
| The archive | `ArchiveDir` | Off: no directory is set by default |
| The person's browser | The webmail keeps the inbox listing and the last thirty-one opened messages in the browser's `localStorage` for reading without a connection, and more only when the person chooses it, up to `ReaderOfflineDaysMax` (30) days ([OfflineMail.md](OfflineMail.md)) | As the browser keeps it. Removed at sign-out (`forgetAccount` in `Portal.js`) |

What is erased, exported and kept for an address is in
[PersonalData.md](PersonalData.md): one archive of everything an account has
*(unreleased)*, and an erasure that reaches the stores beside the mailbox and,
asked by name, the log files.

### The interfaces, and what a new installation has on

| Interface | A new installation | Transport |
|---|---|---|
| SMTP on 25 and 587, POP3 on 110, IMAP on 143 | **Listening, without TLS.** A new installation has no certificate, and the four listeners the create scripts insert have connection security "none" (`hm_tcpipports` in `CreateTables*.sql`) | TLS 1.2 and 1.3, as STARTTLS or implicit TLS, once the operator adds a certificate (by hand, or by ACME, off by default) and sets it on the port. Authentication over a connection without TLS is refused only on a port that requires STARTTLS or from an IP range with *require TLS for authentication*, which neither shipped range has; on SMTP, `AUTH PLAIN` is not offered (`authallowplaintext` = 0) but `AUTH LOGIN` is. The Deck's setup checklist asks for TLS on submission ([SetupChecklist.md](SetupChecklist.md)) |
| Outbound SMTP to recipients' mail servers | On, whenever mail leaves | STARTTLS whenever the receiving server offers it (`SmtpDeliveryConnectionSecurity` = 2). Its certificate is checked (`VerifyRemoteSslCertificate` = 1), but a failure on this opportunistic STARTTLS is forgiven and the delivery goes on encrypted (`CertificateVerifier::OverrideResult_`). A published MTA-STS policy in enforce mode (`MtaStsEnabled` = 1) or DANE (`DaneEnforcementEnabled` = 1, with DNSSEC validation on) makes TLS required and verification binding (`ServerInfo::GetEffectiveConnectionSecurity`). A remote-domain policy can require it per destination *(unreleased)* ([RemoteDomains.md](RemoteDomains.md)) |
| REST API, webmail (`/portal`) and Control Deck (`/`) | Off (`RestApiPort` = 0) | Bound to 127.0.0.1 unless changed. Refuses to start while no administrator password is set, and refuses a non-loopback address without a TLS certificate (`RestApiServer::Start`) |
| Web services listener: autoconfiguration, MTA-STS policy hosting, `security.txt`, CardDAV and CalDAV, the OpenPGP Web Key Directory, an S/MIME authority's root and CRL | Off (`WebServicesHttpPort`, `WebServicesHttpsPort` = 0) | HTTP and HTTPS; CardDAV and CalDAV answer over HTTPS only |
| Prometheus metrics and health probes | Off (`MetricsServerPort` = 0) | Bound to 127.0.0.1 unless changed; optional basic or bearer authentication and TLS |
| ManageSieve | Off (`ManageSieveServerPort` = 0) | Bound to 127.0.0.1 unless changed |
| COM API (Windows) | On | The administrator's credential; the Control Panel is a COM client |
| The database | As configured at install | `[Database] TlsMode` ([DatabaseTls.md](DatabaseTls.md)). Left empty it is *prefer*, and the default does not require TLS. On MySQL, MariaDB and PostgreSQL that is TLS whenever the database server offers it, the server's certificate not checked, and the connection made without TLS when the server offers none. MySQL and MariaDB also try once without TLS when the TLS offered cannot be agreed on (`MySQLConnection.cpp`); `AllowUnencryptedConnection` is still read and changes nothing. PostgreSQL leaves libpq's own *prefer* (or `PGSSLMODE`) in place unless `TlsMode` or `PostgreSQLSslMode` says otherwise (`PGConnection.cpp`). SQL Server encrypts the whole session without checking the certificate through MSOLEDBSQL, which the server uses when no `[Database] Provider` is set and version 18 or later is installed, and through SQLNCLI, which it uses for a failover partner; through SQLOLEDB, which it falls back to when MSOLEDBSQL is not installed, only the login is encrypted unless `TlsMode` is `require` or `verify-full`, and then the certificate is checked (`ADOConnection.cpp`, `DatabaseTls.cpp`). `ConnectionStringOptions` naming the encryption keywords wins while `TlsMode` is empty. `require` refuses a server without TLS, and `verify-full` also checks the certificate's chain and name |
| ClamAV and SpamAssassin | Off (`ClamAVEnabled`, `spamassassinenabled` = 0) | The message is sent to the daemon at the configured host and port (`localhost:3310`, `127.0.0.1:783` by default) over the daemon's own protocol, without TLS |

### What leaves the server

**By default,** a new installation makes only the connections mail delivery
needs:

* **DNS queries** to the resolvers the operating system names, or to `DNSServer`
  if set (`GetDnsServers` in `DnssecResolver.cpp`), with DNSSEC validation on
  (`DnssecValidationEnabled` = 1). A resolver sees which domains the server
  exchanges mail with.
* **SMTP to the recipients' mail servers**, as above.
* **The recipient domain's MTA-STS policy**, fetched over HTTPS from
  `mta-sts.<domain>` when the domain publishes one (`TlsPolicy.cpp`,
  `MtaStsEnabled` = 1), and since 6.3.6 only from a public address, judged by
  the addresses the name resolves to. Through `HttpProxy` when one is set, from
  6.3.6, asked for a tunnel to one of those addresses rather than to the name,
  so that proxy also learns them and, from the TLS handshake inside the tunnel,
  the policy hosts' names.

**Only when an administrator configures it:**

| What leaves | To whom | Switch, default | Document |
|---|---|---|---|
| The connecting address, or a domain in a message, as a DNS query | DNS block list and SURBL operators | Rows for Spamhaus, SpamCop and SURBL are seeded **inactive** | [README.md](../../README.md), *Sender authentication and anti-abuse* |
| This server's own sending addresses, once a day, as DNS queries | Four block-list operators (`ReputationWatchLists`) | On in a new installation, but it asks nothing until an address is configured (`ReputationWatch::RunLocked_` skips a list with no address to check); off after an upgrade *(unreleased)* | [ReputationWatch.md](ReputationWatch.md) |
| A request to the release feed, and a download of the installer | The project's release host | `UpdateCheckEnabled` = 0, `UpdateAutoDownload` = 0 | [README.md](../../README.md), *Update check* |
| DMARC and TLS-RPT aggregate reports | The domains that ask for them in their DNS | `DmarcRptFromAddress`, `TlsRptFromAddress` empty: each day's statistics are discarded unsent (`DmarcRptReporterTask.cpp`, `TlsRptReporterTask.cpp`) | [DmarcReports.md](DmarcReports.md) |
| Event notifications, signed | The subscriber's URL | No subscription exists *(unreleased)* | [Webhooks.md](Webhooks.md) |
| A local recipient's mail, parsed | The route's URL | No route exists *(unreleased)* | [InboundWebhooks.md](InboundWebhooks.md) |
| Alerts by mail and webhook | The administrator's address and URL | Alerts are on, but nothing is sent until `AlertRecipient` and `AlertSenderAddress` are set *(unreleased)* | [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md) |
| Five characters of a new password's SHA-1 | The breach-check endpoint (`api.pwnedpasswords.com` unless changed) | `BreachCheckEnabled` = 0 *(unreleased)* | [PasswordBreachCheck.md](PasswordBreachCheck.md) |
| Attachments of mail from outside | A sandbox service the administrator names | `SandboxEnabled` = 0 *(unreleased)* | [Sandbox.md](Sandbox.md) |
| The message | An HTTP filter (`FilterHookUrl`); ClamAV and SpamAssassin, as above | Empty; off | [README.md](../../README.md) |
| Sign-in, to verify it | An OpenID Connect issuer or a SAML identity provider | Nothing until an issuer or provider is named *(unreleased)* | [PasskeysAndSingleSignOn.md](PasskeysAndSingleSignOn.md) |
| Tokens, to validate them | An OAuth 2.0 key set or introspection endpoint | `OAuth2Enabled` = 0 | [README.md](../../README.md) |
| Log entries, traces and metrics | A syslog collector (UDP, TCP or TLS) or an OpenTelemetry endpoint | `SyslogEnabled` = 0 *(unreleased)*; OTLP endpoints empty | [Syslog.md](Syslog.md) |
| Certificate orders | Let's Encrypt, or the ACME directory set | `AcmeEnabled` = 0 | [README.md](../../README.md) |
| Mail collected, migrated or relayed | Other mail servers, a smart host, Microsoft 365 or Google | Nothing until an external account, a migration, a route or a smart host is set up | [Migration.md](Migration.md) |
| Web Push notifications | The browser vendor's push service | `WebPushEnabled` = 0 *(unreleased)* | |

**At a person's request, in the webmail** (which needs the REST listener on):
looking up a correspondent's OpenPGP key in the correspondent's own domain
(`OpenPGPLookupWkd` = 1, on), a request the server makes and refuses to send to
any address inside its own network, or at a keyserver (`OpenPGPLookupKeyserver`
= 0, off) ([OpenPGP.md](OpenPGP.md), *Finding a correspondent's key*)
*(unreleased)*; fetching a sender's brand logo, which also needs
`AuthenticationResultsEnabled` (0) ([BrandLogos.md](BrandLogos.md))
*(unreleased)*; and a one-click unsubscribe, posted to a public address only.
These three, a Web Push notification and an inbound webhook route held to
public addresses go through `HttpProxy` when one is set, from 6.3.6, as the
MTA-STS policy fetch does: the proxy is asked for a tunnel to the address the
name was judged by, so it learns those addresses and, from the TLS handshake
inside the tunnel, the hosts' names. Before 6.3.6 they went directly whatever
`HttpProxy` said.

**No AI or machine-learning service** is called by the server. A search of
`hmailserver/source/Server` for the names of such services and their APIs finds
none; the webmail's importance marker is worked out in the browser
([ImportantMail.md](ImportantMail.md)).

Control mappings: ISO/IEC 27001:2022 Annex A and SOC 2
------------------------------------------------------

Both tables map the product, not the project's organisation. They list the
controls a mail server can speak to, what hMailServer provides for each - the
feature, its setting and default, the document - and what stays the operator's.
Controls a product cannot speak to (people, physical security, supplier
contracts, most of governance) are not listed. The SOC 2 criteria are
paraphrased, not quoted.

### ISO/IEC 27001:2022, Annex A

| Control | What hMailServer provides | The operator's part |
|---|---|---|
| 5.14 Information transfer | TLS 1.2 and 1.3 on every listener once a certificate is set; opportunistic STARTTLS outbound, made binding by MTA-STS and DANE (both on) and by remote-domain policies *(unreleased)*; S/MIME and OpenPGP in the webmail and gateway encryption to partner domains *(unreleased)*, all off until used (*Cryptography in use*) | Install a certificate, require TLS on submission and for authentication, set the policies for partners |
| 5.15 Access control, 8.3 Information access restriction | Every mailbox access is an account's; folders shared by IMAP ACL; administration behind the administrator password, an account's admin level, or a role *(unreleased)* with domain scope ([Roles.md](Roles.md)) | Decide who holds which role, and which domains |
| 5.16 Identity management | Accounts made by hand, by LDAP provisioning (off), or by SCIM 2.0 from Entra ID or Okta *(unreleased)* ([Scim.md](Scim.md)); a SCIM suspension ends the account's sessions | The joiner, mover and leaver process in the directory |
| 5.17 Authentication information | Salted, key-stretched password hashes (PBKDF2 by default, Argon2id and scrypt available); an optional password policy (every rule off by default); a breach check (off) *(unreleased)*; the administrator password written as MD5 by the Windows installer (*Cryptography in use*) | Set a password policy; set the administrator password again after installing |
| 5.18 Access rights | Roles and assignments over REST, COM and the Deck, each change on the audit trail *(unreleased)*; app passwords per account; devices and sessions listed and revocable *(unreleased)* ([DevicesAndSessions.md](DevicesAndSessions.md)) | Review assignments and keys; API keys expire (90 days when none is given) |
| 5.28 Collection of evidence, 5.33 Protection of records | The audit trail, hash-chained and verifiable (on) *(unreleased)*; eDiscovery cases, holds and exports with SHA-256 manifests *(unreleased)* ([Discovery.md](Discovery.md)); journaling (no rule by default) *(unreleased)* ([Journaling.md](Journaling.md)); the archive with legal holds (off) | Decide retention; protect the database the trail is in |
| 5.29 Information security during disruption, 5.30 ICT readiness for business continuity | Backup and restore, verified after every run; warm standby documented; active-active not built (*Business continuity*) | Schedule backups, keep them elsewhere, run a standby and rehearse failover |
| 5.34 Privacy and protection of PII | One archive of everything an account has *(unreleased)*, an erasure of every trace of an address, a legal hold that outranks it ([PersonalData.md](PersonalData.md)); message retention per domain and account (off) | Be the controller: decide lawful basis, retention and requests |
| 5.37 Documented operating procedures | The operator documents in `hmailserver/docs`, embedded in the server as its help pages ([Help.md](Help.md)) | Keep local procedures |
| 8.2 Privileged access rights, 8.18 Use of privileged utility programs | The administrator password with an optional TOTP second factor enforced on COM and REST; roles narrower than administrator *(unreleased)*; event scripts, which can run code, off (`usescriptserver` = 0) and, on Windows, bounded by `ScriptAllowedObjects` (`*`, every COM class, by default) | Treat administrator access as host access ([SECURITY.md](../../SECURITY.md), *Out of scope*); enrol the second factor; narrow `ScriptAllowedObjects` before enabling scripts |
| 8.5 Secure authentication | SCRAM-SHA-256 and SCRAM-SHA-256-PLUS; TOTP for accounts, set by an administrator; passkeys, OpenID Connect and SAML into the webmail and Deck *(unreleased)*; LDAP authentication (off); auto-ban of an address after 3 failures in 30 minutes, for 60 (on); per-name lockout (`AccountLockoutThreshold` = 0, off) | Require TLS for authentication; enable lockout and a second factor |
| 8.6 Capacity management | Quotas per account and domain with a quota policy *(unreleased)*; maximum message size (20,480 KB); connection limits per protocol (none set by default); disk-space warning and floor; metrics and their history (7 days) | Size the machine, set limits, watch the metrics |
| 8.7 Protection against malware | ClamAV, a command-line scanner, attachment blocking and a sandbox *(unreleased)* - all off by default; SpamAssassin, DNS block lists and greylisting, off | Run and update a scanner and enable it |
| 8.8 Management of technical vulnerabilities | [SECURITY.md](../../SECURITY.md) and the release process (*Vulnerability handling*); the update check and a live update that verifies the installer's Sigstore bundle, both off | Follow the releases and apply them |
| 8.9 Configuration management | What differs from stock, per setting ([ChangedFromStock.md](ChangedFromStock.md)); setting profiles with a diff and a way back ([Profiles.md](Profiles.md)); configuration exported and applied as a document ([CommandLine.md](CommandLine.md)); every administrative change on the audit trail - all *(unreleased)* | Keep the baseline under change control |
| 8.10 Information deletion | Message retention (off); an address's traces erased, including its log lines on request ([PersonalData.md](PersonalData.md)); log retention (`LogDeleteDays` = 0, keep); the recover-deleted window (0, off) | Set retention; freed disk blocks are the file system's |
| 8.12 Data leakage prevention | Outbound content policy - card numbers, IBANs, patterns, attachment types - as mail flow rules; restricted delivery to named domains; TLS required per destination - all off until configured *(unreleased)* ([OutboundContentPolicy.md](OutboundContentPolicy.md), [RestrictedDelivery.md](RestrictedDelivery.md)) | Write the rules |
| 8.13 Information backup | Built-in backup of settings, domains and messages, read back and checked after every run (`BackupVerifyRestore` = 1); encryption with a passphrase *(unreleased)*; one mailbox or one message restored *(unreleased)* ([Backup.md](Backup.md)) | Set the destination and schedule, which are empty by default; keep copies off the machine; keep the passphrase, the Linux key file and the certificate files |
| 8.14 Redundancy of information processing facilities | Warm standby over a shared database ([WarmStandby.md](WarmStandby.md), [HighAvailabilityRunbook.md](HighAvailabilityRunbook.md)); no active-active | The database's own high availability, and the standby machine |
| 8.15 Logging | Application, protocol, error, backup and event logs, JSON optional; the audit trail *(unreleased)*; the message trace (off); syslog *(unreleased)* and OpenTelemetry export (off) (*Access control and logging*) | Turn on the logging needed, set retention, collect centrally |
| 8.16 Monitoring activities | Prometheus metrics and health probes (off); alerts by mail and webhook *(unreleased)*; compromised-account detection in watch mode (on) *(unreleased)* ([CompromisedAccounts.md](CompromisedAccounts.md)); following one connection live *(unreleased)* | Watch them, and move detection to enforce |
| 8.17 Clock synchronization | The server takes time from the operating system | Keep the host's clock synchronised: TOTP, DKIM expiry, sessions and the audit trail depend on it |
| 8.19 Installation of software on operational systems | An Authenticode-signed Windows installer (since 6.3.1); Sigstore bundles for every release asset; a live update that refuses an installer whose bundle does not verify (*The software supply chain*) | Verify downloads; decide the update window |
| 8.20 Networks security, 8.21 Security of network services, 8.22 Segregation of networks | IP ranges with per-range policy; administration listeners bound to loopback by default; country blocking (off) *(unreleased)*; PROXY protocol and XCLIENT only from listed addresses (off) | Firewalling, network segregation, and where the listeners are exposed |
| 8.24 Use of cryptography | *Cryptography in use* | Choose certificates, cipher policy and keys; keep passphrases |
| 8.25 to 8.29 Secure development | *Secure development* | - |
| 8.32 Change management | Administrative changes recorded with their values before and after, and revertible from their row *(unreleased)*; when a saved setting takes effect ([SettingsApply.md](SettingsApply.md)) | Change control on the server |

### SOC 2: the 2017 Trust Services Criteria, with the 2022 points of focus

| Criterion (paraphrased) | What hMailServer provides | The operator's part |
|---|---|---|
| CC2.3 Communicating with external parties | A private reporting route with stated targets ([SECURITY.md](../../SECURITY.md)); an RFC 9116 `security.txt` naming each hosted domain's postmaster (*Vulnerability handling*) | Publish the organisation's own contacts |
| CC6.1 Logical access security, including encryption | Accounts, roles *(unreleased)*, hashed passwords, most stored secrets under the machine's secret store, encryption at rest per account *(unreleased)*, encrypted backups *(unreleased)* | Configure them |
| CC6.2 Registering and authorising users | Accounts made by an administrator, by LDAP or SCIM provisioning *(unreleased)*, or by single sign-on with provisioning (off) *(unreleased)* | The approval process |
| CC6.3 Role-based access, least privilege | Four built-in roles and custom ones over 27 permissions, confinable to named domains *(unreleased)*; API keys read-only unless made otherwise and confinable to domains | Assign least privilege |
| CC6.6 Protection against threats from outside the boundary | TLS; auto-ban; login and recipient tarpits (off); IP ranges; country blocking (off) *(unreleased)*; bounded request handling on the HTTP listeners | Perimeter and exposure |
| CC6.7 Restricting and protecting information in transmission | TLS, MTA-STS, DANE, remote-domain TLS policies *(unreleased)*, outbound content policy *(unreleased)*, restricted delivery *(unreleased)* | Require TLS and write the policies |
| CC6.8 Preventing or detecting unauthorised or malicious software | Virus scanning and sandbox hooks (off); a signed installer, and a live update that verifies what it installs | Enable scanning; verify what is installed |
| CC7.1 Detecting configuration changes and new vulnerabilities | What differs from stock *(unreleased)*; the audit trail *(unreleased)*; the update check (off) | Review them |
| CC7.2 Monitoring for anomalies | Metrics, alerts *(unreleased)*, compromised-account detection *(unreleased)*, a line per counted logon failure in the application log once logging is on | Watch and respond |
| CC7.3, CC7.4 Evaluating and responding to incidents | Revoking a session or device and dropping its connections *(unreleased)*; locking and unlocking an account; verifying the audit trail's chain; the message trace (off) | Run the incident process |
| CC7.5 Recovering from incidents | Backup and restore, one mailbox or message restored *(unreleased)* | Test restores |
| CC8.1 Change management | *Secure development* for the product's own changes; the audit trail for an installation's *(unreleased)* | Change control on the server |
| CC9.2 Vendor and business partner risk | This document; the SBOM, provenance and signatures (*The software supply chain*) | Assess the project as a supplier |
| A1.1 Capacity | Quotas, limits, metrics and their history | Plan capacity |
| A1.2 Backup and recovery infrastructure | Backup, warm standby | Build and run them |
| A1.3 Testing recovery | Every backup read back and checked before it counts ([Backup.md](Backup.md), *What the verification proves*) | Rehearse a restore |
| C1.1 Protecting confidential information | Encryption at rest *(unreleased)*, zero-access mailboxes *(unreleased)* ([ZeroAccess.md](ZeroAccess.md)), messages that expire *(unreleased)* | Decide which mailboxes need them |
| C1.2 Disposing of confidential information | Message retention, erasure, log retention | Set retention |
| P4.2, P4.3 Retaining and disposing of personal information | Retention settings for messages, logs, the trace (30 days), the quarantine (30 days), the audit trail (0, keep) and the rest (*Access control and logging*) | Choose the periods |
| P5.1 Data subjects' access | The person downloads everything the server holds on the account from the webmail *(unreleased)* ([PersonalData.md](PersonalData.md)) | Answer requests |

The software supply chain
-------------------------

### The SBOM

Releases from 6.2.3 on carry two SBOMs, SPDX and CycloneDX, apart from the
withdrawn pre-release 6.2.22-pre4 and -pre5, which -pre6 superseded
([SECURITY.md](../../SECURITY.md), *Supply chain*). Up to 6.3.3 they were made
by the GitHub workflow `sbom.yml` (in the git history at `v6.3.3`), and on
GitLab the `linux-sbom` job (`build/ci/linux-sbom.sh`) makes them the same way:
Syft over the release tag's tree, after a check
that the tree is the tag's, and `build/merge-native-dependencies-into-sbom.ps1`
adds the native libraries Syft cannot see, which `build/native-dependencies.ps1`
reads from `hMailServer.vcxproj` and the tree: OpenSSL 4.0.3, Boost 1.92.0,
PostgreSQL 18.6's libpq, QuickJS 2026-06-04 from its `VERSION` file, and zlib
1.3.2, vendored as source; the files the Windows installer carries but git does
not - the .NET Desktop Runtime 10.0.12 (pinned by version and SHA-512), 7-Zip,
MariaDB Connector/C 3.4.11, the MSVC runtime and SQL Server Compact 4.0 SP1, each
with the version and hash [third-party-binaries.json](third-party-binaries.json)
pins; and, for the Linux packages, the distribution's glibc, OpenSSL, zlib, Boost,
libpq, OpenLDAP and libstdc++, by name (their versions are the distribution's,
and the build log records them - [RELEASE.md](../../RELEASE.md), step 8). The job
fails if either SBOM lacks one of them, and `build/check-sbom-inputs.py --sbom`
holds both documents to every pinned input's version and hash. On GitLab the
`linux-sbom` job does the same for `master`, `batch*` branches and tags
([ContinuousIntegration.md](ContinuousIntegration.md)).

From 6.3.4 on, each compiled file a release carries - the Windows installer, the
`.deb` and the `.rpm` - also goes out with an SBOM of its own,
`<file>.cdx.json` (CycloneDX 1.6), whose subject is that file by its SHA-256 and
whose components are what that file holds and needs, and nothing only another
file carries: for the installer, the linked and packed native components, each
packed file by its pinned SHA-256, and the tools' NuGet packages from their lock
files; for a package, the distribution libraries with the development package
and version it was built against, Boost and QuickJS as linked into it, and the
package's declared dependencies. `build/asset-sbom.py` makes them and checks one
against a file; the release jobs refuse a release without them. How a buyer
checks one is in [SECURITY.md](../../SECURITY.md), *Software bills of materials*.

### Reproducible builds

Two clean Release builds of the same tree produce a byte-identical
`hMailServer.exe`: `/Brepro`, `/d1trimfile`, `/pdbaltpath` and
`OPENSSL_NO_FILENAMES` are in `hMailServer.vcxproj`, and RELEASE.md step 8
repeats the check for every release, once more from a checkout at another
path, and puts the hash in the release notes. The Linux build passes
`-ffile-prefix-map` and exports `SOURCE_DATE_EPOCH` from the tagged commit,
and the `linux-repro` job builds the commit of every protected pipeline that
changes more than documentation - a release tag's among them - a second time,
from another path, and requires the binary and the x86-64 `.deb` and `.rpm`
to be byte for byte the ones the release ships; the image's packages are not
pinned, and the job records their versions. The AArch64 `.deb` and `.rpm` a
release carries from 6.3.6 are built once, by the package jobs, and not
compared. What has not been done is an independent
rebuild by someone outside the project, with independently built libraries
([ReproducibleBuilds.md](ReproducibleBuilds.md) has the procedure and what was
measured; [ASSURANCE-CASE.md](../../ASSURANCE-CASE.md), *Residual risk*).

### Third-party binaries

No binary has been committed to the repository since 11 September 2026.
[third-party-binaries.json](third-party-binaries.json) lists the 31 files the
Windows x64 build uses, and `build/get-installer-binaries.ps1` places them:

| Kind | Files | How each is checked |
|---|---|---|
| Fetched | 18: 7-Zip's console 26.03, the MariaDB Connector/C 3.4.11 client and its seven plugins (two copies), the SQL Server Compact 4.0 SP1 x64 runtime | SHA-256 against the manifest, from the project's `build-inputs-2` (the MariaDB client) and `build-inputs-1` releases, or the publisher (7-Zip) |
| Gathered | 10: the MSVC v145 runtime, 14.51.36231.0 | Its version, against the toolset that built the server |
| System | 2: the ADO type libraries, Windows' own | Present; the hash is reported, not enforced |
| Generated | 1: the COM interop assembly, made from the project's own type library | Never committed; the provenance check refuses it |

The .NET Desktop Runtime the installer bundles is not in the manifest:
`build/get-dotnet-runtime.ps1` downloads the current 10.0 release from
Microsoft's `aka.ms` link and checks only that the file is larger than 40 MB,
so it is neither pinned to a version nor checked against a hash.

`get-installer-binaries.ps1` fetches `build-inputs-1` and `build-inputs-2` from
the project's package registry on GitLab: the first was re-published there byte
for byte after GitHub suspended the project's account (until 26 September 2026
the script fetched GitHub's copy, which has answered 404 since 18 September), and
the second, the MariaDB Connector/C 3.4.11 client built from MariaDB's source, has
been there alone
([ThirdPartyBinaries.md](ThirdPartyBinaries.md)). One more thing is not in
place: 7-Zip 19.00, which the Windows backup runs, is from 2019:
ThirdPartyBinaries.md names it the file most overdue a refresh. That
document's narrative still describes the files as committed, and is being
rewritten under Roadmap2's *Forge-neutral text in the product*; the JSON is the
current record.

OpenSSL, Boost and libpq are built from their upstream source archives by
`libraries/build-openssl.ps1`, `build-boost.ps1` and `build-pgsql.ps1`, each
of which checks the archive against a SHA-256 pinned in the script.

### Pinned dependencies

* **.NET:** eleven `packages.lock.json` files are committed
  (`RestorePackagesWithLockFile` in `hmailserver/source/Tools/Directory.Build.props`),
  and the projects are restored with `--locked-mode`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md), *Building*), so a package cannot
  change under a build without a reviewed commit.
* **Native:** by version in `hMailServer.vcxproj` and by the source archive's
  SHA-256, as above.
* **CI actions:** the GitHub workflows, which ran until 18 September 2026 and
  are in the git history at `v6.3.3`, pinned every action by commit hash, except
  the SLSA generator, which was referenced by tag because its verifier requires
  it (`sign-release.yml` said why). Where each GitLab job's image and tools come
  from is in [ContinuousIntegration.md](ContinuousIntegration.md), *Where the
  tools come from*.

### Signatures and provenance

* **Authenticode.** Since 6.3.1 the Windows installer is Authenticode-signed
  through Azure Artifact Signing, with an RFC 3161 timestamp
  ([RELEASE.md](../../RELEASE.md), *Authenticode*). The Linux packages carry no
  package-manager signature of their own; their Sigstore bundle, below, is what
  verifies them.
* **Sigstore.** Every asset of every release from 6.2.19 to 6.3.3 (the
  withdrawn 6.2.22-pre4 and the superseded -pre5 aside) has a keyless cosign
  bundle, recorded in the public Rekor log and verified in the same job before
  upload (the GitHub workflow `sign-release.yml`, in the git history at
  `v6.3.3`). 6.3.4, built and signed by hand, has none; from 6.3.5 the
  `release-sign` and `release-verify` jobs of `.gitlab-ci.yml` make and check
  them, and have not run yet. [SECURITY.md](../../SECURITY.md), *Verifying a
  release*, has the command.
* **SLSA provenance.** From 6.3.2, each release carries
  `hmailserver-<tag>.intoto.jsonl`, made by the SLSA project's generic generator
  over the digests of the assets as attached, and checked with `slsa-verifier`
  before publication (`sign-release.yml`, the `provenance` and
  `attach-provenance` jobs; RELEASE.md step 12 says 6.3.1 and earlier have none).
  The generator is a GitHub reusable workflow, so 6.3.3 is the last release
  with provenance; the GitLab release jobs make none
  ([ContinuousIntegration.md](ContinuousIntegration.md), *The release*).
* **Signed tags.** Release tags from `v6.2.23-alpha2` on are annotated and
  signed with a maintainer's SSH key listed in `.gitlab/allowed_signers`, which
  holds one key today.
* **The live update** reads the release feed, downloads the installer (or the
  Linux package *(unreleased)*) with its bundle, and verifies the bundle
  in-process against the release workflow's identity before running anything
  (`SigstoreVerifier.cpp`); it is off by default.

### Since 18 September 2026: the release chain and GitLab

GitHub suspended the project's account on 18 September 2026, and the project's
home has been GitLab since 22 September. Every release up to 6.3.3 is
re-published on GitLab with its original files and signatures, byte for byte
(Roadmap2, *The release chain off a single forge*); a bundle names the GitHub
workflow that signed it, and the cosign command in SECURITY.md verifies it
without contacting GitHub.

Until that row is done, **a new release is built and signed by hand**, as
6.3.4 was, with Authenticode and no Sigstore bundle. The GitLab jobs that
replace GitHub's signing and packaging workflows are in `.gitlab-ci.yml` for
6.3.5 onwards and have not run ([ContinuousIntegration.md](ContinuousIntegration.md),
*The release*); the package-manager manifests are rendered by hand, and the
container image appears in the project's container registry, as
`registry.gitlab.com/hmailserver/hmailserver`, pushed from a release's own
pipeline by jobs a maintainer starts, which refuse until the project variable
`HM_PUBLISH_IMAGE` is 1 (unset until the project's move to the group
`hmailserver` on 29 September 2026; nothing is pushed from any other path), signed with Sigstore
as the same pipeline ([ContainerImage.md](ContainerImage.md)); 6.3.4 has none. The rest of the row is in this tree: an installed server's update check reads the project's own feed,
https://updates.progressiverobot.com, published from GitLab Pages
([UpdateFeed.md](UpdateFeed.md)), and its live update trusts a list of two
signers - the GitHub workflow identity for the releases up to 6.3.3, and this
project's GitLab CI pipeline for the ones after (`SigstoreVerifier.cpp`). No
release has been signed on GitLab yet. A server from 6.2.28 to 6.3.3 still reads GitHub's
address until one line of its settings points it at the feed (UpdateFeed.md).

Vulnerability handling
----------------------

**Reporting.** Privately, by a confidential GitLab issue or by mail to the
project's Service Desk address, which GitLab files as a confidential issue
([SECURITY.md](../../SECURITY.md), *Reporting a vulnerability*). The targets,
as SECURITY.md states them:

| Stage | Target |
|---|---|
| Acknowledgement that the report was received | 5 working days |
| Initial assessment: reproduced or more information requested | 10 working days |
| Fix released for a confirmed vulnerability | 90 days from the acknowledgement |

They are targets, "not a contractual commitment", from "a small project, not a
staffed security team" with two maintainers, one of whom receives private
reports today. Disclosure is coordinated on a 90-day timetable; CVE IDs are
requested through GitLab, which is a CVE Numbering Authority; there is no
bounty. The 6.3 line is the one supported, and a fix is not back-ported to 6.2
or older.

**security.txt.** There are two. The project's own is
[.well-known/security.txt](../../.well-known/security.txt) in the repository:
the confidential-issue link and the Service Desk address, `Expires:
2027-09-15`, and a `Policy:` line naming SECURITY.md on GitLab. The one the
**server** serves, at `/.well-known/security.txt` on the web services listener
(off by default), is the installation's, not the project's: it names the
requested domain's postmaster as `Contact:`, sets `Expires:` 90 days ahead on
each request, and is not served for a domain that is not hosted or has no
postmaster (`WebServicesServer::HandleSecurityTxt_`). Its `Policy:` line names
the project's `SECURITY.md` on GitLab since 6.3.4; a server up to 6.3.3 names
the project's former GitHub address, which answers 404.

**How a fix ships.** In the next release, with a regression test that fails
against the build before the fix, and the full regression suite on the binary
that ships; the release notes describe the vulnerability and credit the
reporter, and the issue is made public (SECURITY.md, *What to expect*). Past
fixes were announced exactly there, under a **Security** heading in the
release's notes, collected in [CHANGELOG.md](../../CHANGELOG.md): 6.2.10's
fifteen COM methods that answered a refused call with success, 6.2.14's DKIM
test mode that turned a failed signature into a pass and its logged IMAP SASL
credentials, 6.2.27's missing-DS proof, and 6.3.3's REST routes that let a
read-only API key read the `[Settings]` secrets. The repository records no CVE
and no separate advisory. The OpenVEX document
(`.gitlab/hmailserver.openvex.json`) holds no statement: no advisory against a
component has been dismissed as not affecting the project.

**What that means today.** Until the GitLab release jobs have made a release
(*The software supply chain*), a fix ships only in a release built and signed
by hand; if that holds one past 90 days, SECURITY.md says the reporter is told
before the target passes. One such fix waited for 6.3.4: the event-script
escaping of 24 September 2026, found by the project's own review
([EventScripts.md](EventScripts.md); Roadmap.md, *Event invocation by
source-text concatenation*). A 6.3.3 server is exposed only with event
scripting switched on, which it is not by default. Then, with JScript as the
language (VBScript is the default) and a handler for the event, a value from
outside - a password at logon, a remote server's reply, a failed recipient's
address - could run code in the server's process; and in either language, so
could a domain administrator who names a rule's script function.

Secure development
------------------

**Review.** Changes are reviewed adversarially before they land: independent
reviewers per dimension, each finding checked by a second reviewer instructed
to refute it, as RELEASE.md step 2 requires of a release's complete diff before
it is stamped; Roadmap2's rows record the reviews and what was done about their
findings. On GitLab no second maintainer's approval is required:
`master` is protected, and a change lands by fast-forward only after passing
the regression gate ([GOVERNANCE.md](../../GOVERNANCE.md), *Status since 18
September 2026*).

**The regression gate.** NUnit against a real service over SMTP, IMAP, POP3,
COM and REST, with live ClamAV and SpamAssassin, real TLS, and only DNS faked
([CONTRIBUTING.md](../../CONTRIBUTING.md), *Testing*). The Windows suite is
split by namespace across two Windows benches, and the two halves together are
one gate ([RegressionEnvironment.md](RegressionEnvironment.md), *A second
bench, and the gate in halves*); the Linux suite compiles the same fixtures in
place and runs them against the Linux server over REST. A release runs the
whole suite on the exact binary that ships, first on a build that keeps every
internal assertion (RELEASE.md, steps 8b and 9), and every defect fix needs a
test that fails without it (step 3). The native server's line coverage over
the Windows suite was measured at 73.48% *(unreleased)*
([CHANGELOG.md](../../CHANGELOG.md), *Build and tests*).
On Linux, with branches, the suite and the tester programs reach 49.01% of the
Linux build's lines and 38.87% of its branches *(unreleased)*
(`build/ci/linux-coverage.sh`, the suite in two runs merged) - a floor, for the reasons
[ContinuousIntegration.md](ContinuousIntegration.md) gives (*The coverage
schedule*).

**Static analysis.** The C++ server compiles with warnings as errors;
`build/analyze.ps1` runs MSVC's `/analyze` by hand; SQL is parameterised as a
project rule ([CONTRIBUTING.md](../../CONTRIBUTING.md)), and
`build/check-sql-parameters.py` checks the order parameters are bound in.
CodeQL ran on GitHub until 18 September 2026 - the C# tools on every push and
pull request, the C++ server on every push to `master`, weekly and on demand
([ASSURANCE-CASE.md](../../ASSURANCE-CASE.md)) - and does not run on GitLab.
It has also been run by hand, which is how 6.2.10's COM authorisation defects
were found. More than thirty checkers under `build/` hold rules that past
defects taught (CONTRIBUTING.md, *The checkers*). What runs now, and the
remediation thresholds, are in SECURITY.md, *Vulnerability management policy*.

**Fuzzing.** Three libFuzzer harnesses with AddressSanitizer cover the MIME
parser (`fuzz/`, [Fuzzing.md](Fuzzing.md)). A timed run on the release source is
required for every minor release (RELEASE.md, step 8c). The nightly job ran on
GitHub Actions until 18 September 2026; on GitLab it is the `linux-fuzz` job,
which waits for the owner to create its schedule ([ContinuousIntegration.md](ContinuousIntegration.md),
*The fuzz schedule*). HTML, iCalendar, vCard, Sieve, ManageSieve
and the SMTP command parser have no harness yet (Fuzzing.md, *What is not
covered yet*).

**The binary.** Control Flow Guard (`/guard:cf`) is on; nothing in
`hMailServer.vcxproj` switches off the toolchain's `/GS`, ASLR or DEP, which
are on by default. The Linux build is position-independent and adds no further
hardening flags of its own, so what the distribution's compiler enables by
default applies. On Linux the service runs as its own unprivileged user with
one capability and a strict systemd sandbox (`hmailserver.service`). **On
Windows the service runs as LocalSystem**; a dedicated low-privilege account is
an open Roadmap2 row.

**Continuous integration** is being introduced on GitLab: a `.gitlab-ci.yml`
that runs the checkers and the page harnesses on every push and merge request,
and the builds and both regression suites on the project's own runners
for `master`, `batch*` branches and release tags only, described in
`ContinuousIntegration.md` when it lands (Roadmap2, *Checks on GitLab CI*).
Until then every check is run by hand, as CONTRIBUTING.md lists them.

Cryptography in use
-------------------

The server's cryptography is OpenSSL's - 4.0.3 in the Windows build, the
distribution's (3.0 or later) on Linux - with four exceptions: DPAPI protects
stored secrets on Windows, Windows' own certificate chain verification checks
outbound TLS peers there (`CertificateVerifier.cpp`), the legacy Blowfish
scheme below is the project's own code (`LegacyBlowfish.cpp`, written in
2026 from the cipher's published description), and the webmail's
S/MIME and OpenPGP run in the browser on the Web Crypto API.

### TLS

| Setting | Default | Where |
|---|---|---|
| Protocol versions (`SslVersions`) | TLS 1.2 and 1.3 (24); TLS 1.0 and 1.1 can be switched on | `SocketConstants.h`, `SslContextInitializer.cpp` |
| TLS 1.2 cipher list (`SslCipherList`) | The long-standing hMailServer list: ECDHE and DHE with AES-GCM first, but it also admits CBC-mode suites and static-RSA key exchange. `AEAD-ONLY` is a preset of forward-secret AES-GCM and ChaCha20-Poly1305 suites only | `SettingsDefaults.h`; `SslContextInitializer::SetCipherList_` |
| TLS 1.3 suites (`TlsCipherSuites13`) | Empty: OpenSSL's own | `IniSettingsBaseline.h` |
| Key exchange groups (`TlsKeyExchangeGroups`) | `X25519MLKEM768:SecP256r1MLKEM768:X25519:secp384r1:secp256r1`: hybrid post-quantum first, inbound and outbound | `IniSettingsBaseline.h` |
| Session tickets | On (`TlsSessionTicketsEnabled` = 1); key rotation off (`TlsTicketKeyRotationSeconds` = 0) | `IniSettingsBaseline.h` |
| Outbound certificate verification | On (`VerifyRemoteSslCertificate` = 1): forgiven on opportunistic STARTTLS, binding under MTA-STS enforce, DANE or a policy that requires it | `TCPConnection::AsyncHandshake`, `CertificateVerifier.cpp` |
| DNSSEC and DANE | Validation on; DANE enforced where a TLSA record validates. An MX or TLSA lookup that does not complete, or that a signed zone answers without its signatures or its proof of absence, leaves the host unused and defers the delivery (RFC 7672 section 2.1.1); a name counts as unsigned only below a delegation proven unsigned | `IniSettingsBaseline.h`, `DnssecResolver.cpp` |
| Client certificates | SASL EXTERNAL on a port that asks for one (off on every port) | [README.md](../../README.md) |

### Message signing

DKIM signing with `rsa-sha256`, or `ed25519-sha256` for an Ed25519 key, per
domain once it has a selector and a key (none by default). `rsa-sha1` is
neither used to sign - a domain set to it is signed with SHA-256 - nor accepted
in verification unless `DkimAcceptSha1` is set (0; `DKIMSigner.cpp`,
`DKIM.cpp`); an expired signature is refused (`DKIMEnforceSignatureExpiry` =
1). Key rotation per domain is off *(unreleased)*
([DkimKeyRotation.md](DkimKeyRotation.md)). ARC sealing (RFC 8617) is off
(`ArcSealingEnabled` = 0).

### Passwords

| Credential | How it is stored |
|---|---|
| Account passwords | `PreferredHashAlgorithm` = 4: **PBKDF2-HMAC-SHA256**, 210,000 iterations, a 16-byte salt from `RAND_bytes`. Argon2id (5; 19,456 KiB, 2 passes, parallelism 1) and scrypt (7; N = 2^17, r = 8, p = 1) are the alternatives; a setting that is not a password hash is refused and read as 4. A stored hash weaker or cheaper than the setting is re-hashed at the next successful logon. `MinimumAcceptedHashAlgorithm` (0, off) refuses logons against weaker hashes, and `PasswordPepper` (empty) adds an HMAC pepper to Argon2id hashes (`HashCreator.h`, `IniFileSettings.cpp`, `PasswordValidator.cpp`) |
| App passwords and recovery codes | Hashed the same way |
| The administrator password | In `hMailServer.ini`. **The Windows installer writes the password it collects as an unsalted MD5 digest** (`GetHashedPassword` in `hMailServerInnoExtension.iss`), and nothing re-hashes it at sign-in. Set again from the Control Panel or COM (`Settings.SetAdministratorPassword`), or with `hmailserver --set-admin-password` on Linux, it is stored as PBKDF2 (`IniFileSettings::SetAdministratorPassword`). Set it again after installing |
| API keys | A SHA-256 digest of the token; every key has an expiry, 90 days when none is given (`RestApiServer.cpp`) |
| Browser sessions | An `HttpOnly`, `SameSite=Strict` cookie, ended after 30 idle minutes or 12 hours (`SessionPolicies.h`) |

### Stored secrets

Credentials the server must be able to use again are kept reversibly, under the
platform's secret store (`ProtectStoredSecretsWithDPAPI` = 1): route, relay and
smart-host passwords, external accounts' passwords and tokens, certificate
private-key passphrases, the database password, the administrator's
second-factor secret, the backup passphrase, webhook and inbound-webhook
secrets, the sandbox token, migration jobs' source credentials, the Web Push
key and the OpenID Connect and import client secrets (the `ProtectSecret`
callers, and `PropertySet::IsCryptedProperty_`).

* **Windows:** DPAPI at machine scope, so a value copied to another machine
  cannot be opened there - and does not travel to a standby
  ([WarmStandby.md](WarmStandby.md)). If DPAPI fails, or the setting is 0, the
  value is written with the legacy Blowfish scheme, whose key is in the source
  (`Crypt::ProtectSecret`).
* **Linux:** AES-256-GCM, a fresh 12-byte nonce per value, under 32 random
  bytes in `<data directory>/.hmailserver-secret-key`, mode 0600, checked on
  every read; there is no Blowfish fallback. Whoever can read the key file can
  open the secrets, and a data directory restored without it has lost them
  (`DataProtector.h`).

Not under the secret store: **an account's TOTP secret**, kept in the database
as it was generated (`accounttotpsecret`, `PersistentAccount.cpp`), and the
`[Settings]` keys that hold secrets - `SRSSecret`, `BATVSecret`,
`OAuth2HmacSecret`, the metrics listener's password and the like - which are
kept as they were written, in the database's copy of the settings and in
`hMailServer.ini`, and masked wherever the server shows them.

### Mail at rest

* **The message store** is as received unless encryption at rest is switched on
  for an account *(unreleased)*: each message encrypted to the account's X25519
  key through an ephemeral X25519 agreement, HKDF-SHA256 and AES-256-GCM; the
  account's private key wrapped by its password (Argon2id) and sealed to the
  domain's recovery key, whose passphrase the server never stores
  (`MailboxCrypto.cpp`). It protects the disk, the backups and the database, not
  the mail from the running server ([EncryptionAtRest.md](EncryptionAtRest.md)).
* **Zero-access mailboxes** *(unreleased)*, per account where allowed
  (`ZeroAccessAllowed` = 0): delivered mail encrypted to the person's own
  OpenPGP key, which the server never holds ([ZeroAccess.md](ZeroAccess.md)).
* **Backups:** 7-Zip AES-256 with the headers encrypted, once a passphrase is
  set *(unreleased)* ([Backup.md](Backup.md)).

### End to end

* **S/MIME in the webmail:** signing, verifying, encrypting and decrypting in
  the browser; the private key is wrapped with the account's password before
  the server stores it. A domain's own S/MIME authority, a P-256 or RSA-3072
  root constrained to the domain *(unreleased)*, certifies keys made in the
  browser; there is no key escrow ([SmimeAuthority.md](SmimeAuthority.md)).
* **OpenPGP in the webmail** *(unreleased)*: keys made or imported in the
  browser under a passphrase the server never sees; PGP/MIME; Autocrypt; the
  Web Key Directory, with publishing off ([OpenPGP.md](OpenPGP.md)).
* **Gateway encryption** *(unreleased)*: mail to a named partner domain
  encrypted at the server to the partner's OpenPGP key or S/MIME certificate,
  off for every domain ([RemoteDomains.md](RemoteDomains.md)).

### Other uses

The audit trail's chain is SHA-256 (`AuditTrail.cpp`); webhook bodies are
signed with HMAC-SHA256 ([Webhooks.md](Webhooks.md)); security-relevant
randomness comes from OpenSSL's `RAND_bytes`
([ASSURANCE-CASE.md](../../ASSURANCE-CASE.md), *Weak randomness*).

Access control and logging
--------------------------

### Who can administer

* **The administrator password** opens everything, over COM (the Control Panel)
  and REST. A TOTP second factor can be enrolled for it and is then enforced on
  both ([README.md](../../README.md), *Account security and authentication*).
* **Roles** *(unreleased)*: four built in, whose permissions are in the code and
  not the database - `serveradmin`; `domainadmin`, for its own domain;
  `helpdesk`, which finds an account, resets its password, unlocks it and reads
  its trace; and `auditor`, which reads everything, **mailboxes included**, and
  changes nothing - and custom roles over 27 permissions, assigned per account
  and confinable to named domains ([Roles.md](Roles.md), `RoleManager.cpp`). An
  account with no assignment keeps the authority its admin level always gave
  it.
* **API keys** have a scope that is read-only unless made otherwise, a role, a
  list of domains, an optional source address and an expiry. No key can make or
  revoke keys.
* **Passkeys, OpenID Connect and SAML** into the webmail and the Deck
  *(unreleased)*: passkeys are offered (`PasskeysEnabled` = 1) and nobody has
  one until they enrol it; single sign-on does nothing until a provider is
  named, provisions no account unless told to (`OidcAutoProvision`,
  `SamlAutoProvision` = 0), and cannot make anybody an administrator that the
  server's own list does not name
  ([PasskeysAndSingleSignOn.md](PasskeysAndSingleSignOn.md)).

### Who can reach a mailbox

The account itself - with its password, an app password, SCRAM, a client
certificate, an OAuth 2.0 token (off), or in the webmail a passkey or single
sign-on *(unreleased)*. A TOTP second factor, set by an administrator, stops the
password being a mail-client credential, and app passwords take its place
([AccountRecovery.md](AccountRecovery.md), *Second factors*). Another account,
through an IMAP ACL; and an administrator, or a role holding `view_mailbox`. An
IMAP master user is possible and not set (`ImapMasterUser` empty).

Failed logons: an address is banned for 60 minutes after 3 failures in 30 (on);
a per-name lockout is off (`AccountLockoutThreshold` = 0); every rule of the
password policy is off. Sessions and devices can be listed and revoked, and
idle, lifetime and one-per-device policies set *(unreleased)*
([DevicesAndSessions.md](DevicesAndSessions.md)).

### The audit trail *(unreleased)*

On by default (`AuditTrailEnabled` = 1). One row per administrative change over
COM, REST or the Deck - who, which interface, from which address (none for
COM), what, and the values before and after - with secrets recorded as
`(changed)`. Each row carries the SHA-256 of the row before it, so an edited or
removed row is found by `GET /api/v1/audit/verify`; that makes tampering
detectable, not impossible, since whoever can write the table can recompute the
chain. It records changes, and from 6.3.6 one kind of read: somebody's mail
read through an administrator's REST route - an account's folders and
messages, a message's headers or its source, a backup's listing of a mailbox,
an export, a held message's headers or text, the archive index searched or an
entry of it read, an eDiscovery search's hits - a support session opened, and a
mailbox the IMAP master user opens, the same read by the same credential from
the same address once a minute; and a message added to somebody's mailbox
through those routes, its flags set, copied or deleted, or every one deleted
([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md), *Mail read through an
administrator's route*). Not recorded: a read or a change over COM, an API key
the Control Panel makes or revokes (it writes the key store file itself; over
REST both are recorded), releasing or deleting quarantined mail, public folders,
the administrator password, and what a restore replaces
([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md), *What this does not do*).

### Logs

| Record | Default | Kept |
|---|---|---|
| Application, SMTP, POP3, IMAP, TCP/IP and debug logs | **Off** (`logging` = 0) | Until `LogDeleteDays` says otherwise: 0, for ever (`LogRetentionTask.cpp`) |
| The error log | Always written, whatever the log settings (`Logger::LogError`) | As above |
| Windows event log | Errors only (`WindowsEventLogEnabled` = 1) | As Windows keeps it |
| Message trace: each delivery and failure | Off (`MessageTraceEnabled` = 0) | 30 days |
| Audit trail | On *(unreleased)* | For ever (`AuditRetentionDays` = 0) |
| Quarantine | Off | 30 days |
| Metric history | On | 7 days |
| Webhook deliveries | - | 7 days *(unreleased)* |
| DMARC and TLS reports received | Nothing until a domain names a report mailbox | 365 days *(unreleased)* |
| Export to syslog or the journal, and to OpenTelemetry | Off | As the collector keeps it |

What the protocol logs contain, once switched on: each session's commands and
replies with the client's address - so mail addresses, host names and user
names. Passwords are masked in the logon lines, and in the SMTP log every line
of a SASL exchange but the user name (`SMTPConnection.cpp`, `IMAPConnection.cpp`,
`POP3Connection.cpp`, `PasswordRemover.cpp`). An address's lines can be
rewritten to `[erased]` on request ([PersonalData.md](PersonalData.md)). JSON
logging, per-service files and a slow-query log with SQL literals redacted are
options ([README.md](../../README.md), *Operations and observability*).

Business continuity
-------------------

* **Backup and restore.** One 7-Zip archive per run: the settings, the domains
  and accounts with everything an account has, and, when asked, the whole
  message store; read back and checked before it counts; encrypted with a
  passphrase *(unreleased)*; one mailbox or one message restored on its own
  *(unreleased)*. A full restore is destructive and has no transaction. What an
  archive deliberately leaves out - `hMailServer.ini`'s machine sections, the
  administrator password, `PasswordPepper`, certificate and DKIM key files, the
  queue, the quarantine and the message trace - is listed with what to do about
  each ([Backup.md](Backup.md)). No destination and no schedule are set by
  default. On Linux the stored secrets open only with the key file; on Windows
  DPAPI-protected secrets do not move to another machine and are entered again
  there.
* **Warm standby, not active-active.** Exactly one service may run against the
  database at a time: two would deliver a queued message twice
  ([WarmStandby.md](WarmStandby.md), *Warm, not hot*). A second machine shares
  the database and a replicated data directory with its service stopped, and
  failover is starting it ([HighAvailabilityRunbook.md](HighAvailabilityRunbook.md)).
  Active-active nodes and object storage for the message store are open
  Roadmap2 rows.
* **The database.** MySQL or MariaDB, Microsoft SQL Server or PostgreSQL for
  anything that must survive a machine, their own high availability being the
  database's; SQL Server Compact is local to one machine. Moving between them
  is back up, repoint, restore
  ([MigratingDatabaseBackend.md](MigratingDatabaseBackend.md)).
* **Upgrades** keep configuration and mail and run the schema steps forward
  only. There is no downgrade: a rollback is the snapshot taken first
  ([Upgrading.md](Upgrading.md)).

Regulatory scope
----------------

Whether the EU Cyber Resilience Act and the revised Product Liability Directive
place obligations on this project is answered, with a date and the triggers
that would change it, in [RegulatoryScope.md](RegulatoryScope.md). Its list of
what is done anyway was written before the move off GitHub; *The software
supply chain* above is the current account.

Not in place, in one list
-------------------------

* TLS on a new installation's listeners: it needs a certificate the operator
  provides.
* Authentication refused on a plaintext connection: only where a port or an IP
  range requires TLS, and neither ships that way.
* A salted hash for the administrator password the Windows installer writes.
* An account's TOTP secret, and the `[Settings]` secrets, under the secret
  store.
* A low-privilege Windows service account: the service runs as LocalSystem.
* A release made by the GitLab release jobs: they are written and have not
  run, and 6.3.4 was built and signed by hand; the live update in servers up to
  6.3.3 trusts only the GitHub signing identity.
* CI on GitLab blocking on the security scans: they run on the project's own
  runner but do not fail a pipeline yet ([ContinuousIntegration.md](ContinuousIntegration.md)).
* CodeQL, the dependency review and the nightly fuzzing: none has run since
  18 September 2026.
* Fuzz harnesses beyond MIME.
* Versions for the Linux packages' system libraries at run time: each package's
  own SBOM names the version it was built against and the range it requires, and
  the machine's own package manager decides the rest.
* An independent rebuild confirming a released binary.
* Active-active operation.
* Any audit, certification or attestation.

Checking this document
----------------------

Every default above was read from `SettingsDefaults.h` and
`IniSettingsBaseline.h`, and every behaviour from the file named beside it, on
25 September 2026. The *(unreleased)* markers were read from CHANGELOG.md's
*Unreleased* section and from the server's sources at the `v6.3.3` tag. The
parts most likely to go stale are the release chain, the `security.txt` policy
line, continuous integration and the *(unreleased)* markers, which a release
turns into version numbers; read them again before sending this anywhere.
