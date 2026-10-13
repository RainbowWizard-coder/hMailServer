The reasons behind the settings pages
=====================================

The Control Panel's settings pages keep each hint to two sentences: what a setting
does, and its safe value or what follows from it. What a hint said beyond that -
why a default is what it is, what a choice costs, how one setting meets another -
is kept here, page by page, under the setting's caption as the page shows it. The
Anti-spam settings and Anti-virus settings pages' reasons are in
[SmtpPolicy.md](SmtpPolicy.md#the-reasons-behind-the-anti-spam-settings-page) and
[SealedMail.md](SealedMail.md#the-virus-scanner).

## Protocols

*Warn the mailbox owner at this percentage of their quota.* One notice, not one per
message: the server treats the threshold as something the mailbox crosses rather
than a state it is in, so a mailbox sitting at 95% is not warned about again on
every delivery - which would fill it faster than the mail does. Emptying the
mailbox and filling it again warns again.

*Still accept incoming mail past a mailbox's limit by.* The owner's own saves and
copies stop at the limit itself. The setting applies at once.

*Stop an account sending at.* The setting applies at once.

*Recalculate folder UID counters.* Each IMAP folder keeps a counter for the next
message UID it will issue. If that counter falls behind the messages already in the
folder - which a restored or hand-edited database can do - the folder issues a UID it
has already used, and a client that caches by UID shows one message where another
should be.

*ManageSieve (RFC 5804).* STARTTLS is offered only when a TLS-capable mailbox port
has a certificate to borrow - the status on the card says whether it will be.

## Web services & autoconfiguration

*Calendar and contacts discovery (CalDAV / CardDAV).* Leave both addresses empty
unless you run such a server: empty means the discovery URLs answer 404, which is the
right response when there is nothing to point at. They need the web services
listener to be running, like everything else on the page.

## Webmail

*Let people make their mailbox zero-access.* Off by default.

*Base address new certificates name for revocation.* A certificate already issued
keeps what it was issued with.

## DNS resolver

*Name servers.* Leave the override empty to use the name servers Windows is
configured with.

*DNS cache.* The setting is ignored when an override DNS server is set: those
lookups always bypass the system cache, because its answers would come from the
wrong resolver.

## Delivery of e-mail

*Complaint mailbox.* Only a report delivered there is read, because arriving where
the provider was told to send it is what tells a report from a forgery.

*Base address of the list pages.* Empty, the default, sends mailto addresses only.

*Fixed token* (the smart host's OAuth2). The setting is for tokens obtained outside this server.

*Classify bounces and keep a suppression list.* A database that cannot be read sends
the mail as if the address were not listed, and says so in the error log once an hour.

*What happens to a listed recipient.* Mail already queued to an address meets the list
on its next delivery attempt.

*Read delivery reports that arrive for this server's senders.* Turning this off stops
reports being read at all; refusals met while delivering are still counted.


## Rules

*Account-number pattern.* Empty, the default, is an account-number detector that never fires. A pattern the
server cannot compile is refused.

## TLS versions & ciphers

*Key exchange groups, in order of preference.* An empty list, or one OpenSSL rejects
or that would leave no group enabled, is not applied: the server logs it and uses
the classical fallback, so a mistake here cannot take TLS down - but it does silently
give up the hybrids, which is why the shipped list is shown rather than a blank.
Change it against a scan rather than in the dark.

*Cipher list for TLS 1.2 and below.* TLS 1.3 is unaffected by the AEAD-ONLY preset:
its suites are AEAD by construction and are configured separately. A misspelled
preset name is rejected and reported, not silently ignored.

*TLS 1.3 cipher suites.* Leave the list empty to keep OpenSSL's defaults; a name this
build does not recognise is skipped and reported rather than failing the whole list.

*Issue TLS session tickets so clients can resume sessions.* Turning tickets off is
for an administrator whose policy is that nothing derived from a long-lived key ever
goes on the wire.

*Server-side session cache size.* A positive value replaces OpenSSL's default cap of
20,480 sessions. -1 turns the server-side cache off entirely, which stops session-ID
resumption but leaves tickets - which cost the server no memory - unaffected. 0 leaves
OpenSSL alone.

*Rotate the session-ticket key every.* What rotation defends: OpenSSL's default
ticket key is generated once, when the listener starts, and is never rotated, so
every ticket that listener issues for the life of the process is sealed under the
same key - and that key, recovered later, decrypts every ticket ever recorded. 86,400
seconds is a day.

## Transport security

*ARC sealing.* The Setup checklist page checks your domains for a DKIM key that
sealing can borrow.

*ARC inbound filtering.* With the trusted-sealer list empty the feature does nothing
at all, by design: anyone can fabricate an entire ARC chain and seal it with keys
published in their own DNS, and it will validate, so a passing chain proves nothing
unless you already trust the sealer. The list is not an option of the feature - it
is the feature. The offset never exceeds the DMARC failure score, and applies only
while DMARC scoring is enabled on the Anti-spam settings page.

*Validity window for the signatures this server makes.* A signing timestamp is
always sent regardless of this value.

*Write an Authentication-Results header on inbound mail.* An arriving message that
already carries an Authentication-Results header claiming this server's own identity
has that header removed first, so a sender cannot have a verdict written in this
server's name believed downstream (RFC 8601 section 5).

*Forwarded mail & bounce protection (SRS / BATV).* Forwarding a message keeps the
original envelope sender, so the next hop checks SPF for a domain that never
authorised this server and the message fails. SRS and BATV are the two answers.

*Rewrite the envelope sender when forwarding.* The setting replaces the envelope
sender with the forwarding account's own address.

## Administrative access

*Range endpoint.* Plain http is accepted only to localhost, 127.0.0.1 or ::1.

*Expire passwords after.* IMAP, POP3 and SMTP have no way to change a password, and
the self-service portal refuses an expired one at sign-in like every other surface.
An expired password is therefore renewed by an administrator - or by the person
themselves only if they hold an app password, which still signs them in to the
portal, where the expired password is accepted as the current one when setting a new
one. Existing app passwords keep working, which is also the one way an affected
person can still collect mail; Active Directory accounts are exempt, because their
password lives in the directory and expires by its policy, not this one.

## Auto-ban

*Also block a banned address in the operating system's firewall.* On Linux the
packaged autoban-hook keeps an nftables set with a timeout, and the service needs
CAP_NET_ADMIN for it - the hook says exactly how to grant it.

*Program to run when an address is banned or unbanned.* A non-zero exit or a program
that cannot start is reported once in the error log.

*Names that are never auto-banned.* The reverse lookup is never waited for on a
connection: a ban placed before the answer is in is lifted once it is, before the
firewall or fail2ban is told.

*Widen an auto-ban to the neighbouring addresses whose reverse names share its
domain.* Only when the banned address's own reverse name resolves back to it; at most
32 addresses each way; never into another /24, across an address either never-ban
list spares, or across an IP range of yours that covers one address and not the
other; never for IPv6, nor for a private, shared, loopback or link-local address,
whose neighbours are your own network's machines. A reverse lookup that is not
answered leaves the single address.

## Directory authentication (LDAP)

The page shows under each setting the `[LDAP]` key it is stored as in
hMailServer.ini.

*Use LDAP directory authentication.* Accounts that are not directory-linked are never
affected either way.

*Directory server host name.* Required.

*Verify the directory server's certificate.* On by default, and only an explicit 0
in the INI turns it off. The server notes the disabling in its application log once
per service start.

*Allow sending the password over an unprotected connection.* With unprotected LDAP
and a simple bind, the server does not send the password and does not guess what you
meant: it refuses the sign-in and reports the contradiction, once per minute.

*User DN template.* The same %u, %d and %m placeholders as the search filter, escaped
for a distinguished name.

*Service account password.* It only ever leaves the server over the transport
configured above it.

*Retry through Windows logon when the directory is unavailable.* When off, an
unreachable directory refuses the sign-in and the server reports the real reason
(throttled to once per minute per distinct problem) instead of retrying through
Windows logon - which cannot succeed on a host that is not domain-joined, and whose
failure would bury a precise diagnostic under an indistinguishable one. A password
the directory rejected never falls back either way: retrying it would double every
failed attempt and let one wrong password trigger a directory lockout policy.

## Directory synchronisation

*Also mark accounts inactive when their directory entry has gone.* An account that
stops being visible in the directory is far more often a filter or a search that
changed than a person who left, and the two mistakes do not cost the same.

*Run every (minutes).* Values between 1 and 14 are treated as 15, and anything over a
week as a week.

## Domains

*Membership query* (a distribution list's dialog). Accounts that are switched off are
never included.

## Logging

*Facility.* 16 to 23 are local0 to local7, for a site that files its own applications
there.

*Log level.* There is only one step, at 2. Turning on Debug messages restores full
detail whatever the level.

*Send log entries to syslog.* Log files are unaffected: this sends a copy, it does not
move anything.

*Windows Event Log.* Protocol traffic and routine logging never go there - a mail
server that logs every session to the Application log gets itself uninstalled.

## Monitoring & alerts

*REST administration API and the Control Deck.* The API is authenticated with the
administrator password, or with a scoped API key - a key can be read-only, limited to
named domains and source addresses, given an expiry and revoked on its own, none of
which the administrator password can. Keys are managed on the REST API keys page.

*Keep metric history for.* The history is what the dashboard's 24 hours, 7 days and
30 days views and GET /api/v1/metrics/history read. A week of minute-resolution
history is about a hundred and forty thousand small rows.

*Bearer token for /metrics.* Leading and trailing spaces are trimmed.

*OpenTelemetry OTLP endpoint for traces.* Prometheus metrics are the port above, and are
the same counters the metrics endpoint pushes rather than a second tally.

*TLS private key file for the metrics listener.* If TLS is configured but cannot be
prepared (an unreadable file, a key that does not match), the health probes stay on
plain HTTP and /metrics answers 503 rather than serving the exposition in the clear.

## Backup & restore

*Verify each backup by extracting its message store.* After the archive is written
its message store is extracted to the temp folder through the same code a restore
uses, held to exactly the files the backup staged, and every message row is checked
for its file; the result is in the backup log.

*Encryption passphrase.* At least 12 printable ASCII characters.

*Delete archives older than.* Retention runs on the server after every backup into
the folder - scheduled or started by hand with Start backup now - and only once the
new archive has been written and verified readable, so a failed backup never deletes
anything.

## Updates

*Unattended window.* A service that does not come back is rolled back to the previous installer, unless the new
version has already changed the database.

*Repository the signing certificate must name, and the four settings after it.*
Setting any of them narrows or replaces the trust in this project's two release
workflows for both, and getting one wrong means either refusing genuine releases or
trusting somebody else's.

## Performance

*Full-text term index for IMAP SEARCH BODY and TEXT.* Results are identical on and
off: the index only narrows which messages the substring scan has to read.

*Index now, and show how far behind it is.* With indexing switched off there is no
indexer to wake, and the button says so rather than reporting success.

*Discard the index and rebuild it.* Worth doing when the index has more entries than
the server has messages, which means entries for mail that no longer exists.

*Attempts to reach the database at start-up.* A mail server usually starts with the
rest of the machine, and on a machine where the database service starts second, the
first few attempts fail.

*Seconds a single SQL statement may run.* The upgrade and maintenance scripts run
under a 30-minute per-statement ceiling of their own regardless of this value.

*Worker thread priority.* It is shown rather than removed so that an administrator who
set it years ago can see it is still there and why nothing happened, and it is not
editable because a box that accepts a value nothing acts on is worse than no box at all.

*Domain, account, alias and distribution-list cache maximum sizes.* The TTLs and the
enable switch on the Cache card are stored and do survive restarts.

## Advanced

*Only archive mail for these domains.* An archive is usually kept for a legal or
contractual reason, and that reason usually applies to particular domains rather than
to every domain this server hosts.

*COM objects a script may create.* An event script runs inside the server with the
service account's rights, so what it may instantiate is a security decision rather
than a convenience. Name only what a script of yours actually needs - a scripting
file system object, an HTTP client - and everything else is refused with the object's
name in the error log; empty refuses everything beyond hMailServer's own objects.

## Server limits & expert settings

*Timeouts and queue bounds.* The defaults are deliberate; each one is there because
there is a diagnosable situation in which it is the right thing to change.

*Front-end proxies (PROXY protocol and XCLIENT).* Put a load balancer, a TLS
terminator or a Postfix relay in front of the SMTP listener and every connection
appears to come from that device - which silently breaks DNSBL checks, SPF,
greylisting, auto-ban and the IP range rules all at once, while everything still
reports success. The upstream has to be configured to send the real address or
nothing here changes: HAProxy needs send-proxy or send-proxy-v2 on its server line,
and a Postfix relay needs XCLIENT enabled towards this host. hMailServer cannot check
that from here - and once an address is listed as a PROXY protocol proxy the header
becomes required from it, so a proxy that does not send one will have its connections
dropped.

*Proxies allowed to send a PROXY header.* Each is matched against the real TCP peer. An entry that does not
parse matches nothing rather than everything, and is reported in the error log once per run.

*Upstreams allowed to use XCLIENT.* XCLIENT is not even advertised in EHLO to an upstream that is not listed, so
an attacker learns nothing about the deployment by asking.

*Refused connections.* Locking out an address that keeps failing to sign in is a
different mechanism entirely and is on the Auto-ban page.

*Per-account sending limits.* Unlike the rest of the page these are re-read within a
couple of seconds of a change - saving applies them without a service restart.
Counters survive a restart via a state file in the data directory.

*Per-address overrides.* A malformed line is ignored with a log entry and the global
limit still applies to that account. Comment lines in this INI section are not shown
on the page and are dropped if the list is saved.

*Submission rate limits.* Throttling your own outbound rate to a destination is on
the Delivery of e-mail page.

*Server-generated mail.* Left empty, the server uses its own host name, then the
local domain the message involves, then this computer's name. It is not a delivery
setting and does not change where mail is routed.

*Stored secret protection.* It does not affect backup and restore - nothing in a
backup archive is DPAPI-protected, and a restore re-protects each secret under the
destination machine's own setting - so a backup taken here restores onto another
machine with this switched on.

*Protect stored secrets with Windows DPAPI.* Those bound to this machine - the
database password in hMailServer.ini, the smart host's password, and the passwords,
tokens and passphrases of routes, external fetch accounts, SSL certificates and the
rest kept in the database - are put in a Windows DPAPI envelope when they are next
saved, or in the legacy Blowfish scheme while this is switched off. The secrets in the
[Settings] section - the SRS and BATV secrets, the OAuth2 secrets, the metrics bearer
token and HTTP Basic password, the Windows service account password and the GSSAPI
service password - and each account's second factor are sealed under the
installation's stored-secrets key once it has one, at the next start and at every
save; switched off, the next start stores them as written again.

*Folders the service account needs.* A folder the configuration names elsewhere, such
as a backup or archive folder set later, is approved only after a question that names
it; key files outside those folders are listed.

*Settings that used to be on this page.* The page used to collect every
hMailServer.ini value without a home, which meant it was filed by where the value is
stored rather than by what it does - and where a value is stored is the one thing you
never need to know.
