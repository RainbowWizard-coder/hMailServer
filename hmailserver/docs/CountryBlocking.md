Country blocking
================

Refusing connections and sign-ins by the country the address is in, from a
geographic database you supply.

This server ships no geographic database and downloads none. There is no default
country policy, and with nothing configured the feature is entirely inert: the
accept path asks one setting, finds it empty, and does what it always did.

------------------------------------------------------------------------------

What it is
----------

Two kinds of policy, in one grammar:

* **Per listener family** — SMTP, IMAP, POP3, and the HTTP listeners (the REST
  API and the web services share one). Applied **when a connection is accepted**:
  before the greeting, before the TLS handshake, and before anything touches the
  database. That is the same place, and for the same reasons, that an IP range
  refuses a connection.
* **Per domain** — applied **when somebody signs in** to an account of that
  domain, on any protocol. It is about signing in rather than about connecting
  because a domain is not known when a connection is accepted: the name arrives
  later.
  A password sign-in meets it before the password is checked, and SCRAM-SHA-256,
  which proves the password without handing it over, meets it before its proof is
  checked, with the same ordinary refusal. A sign-in that proves who the client
  is without a password meets it as soon as the account is known: an OAuth2
  bearer token (`XOAUTH2`, `OAUTHBEARER`), a client certificate (`EXTERNAL`) or a
  Kerberos ticket (`GSSAPI`) on SMTP, IMAP and POP3, and a passkey or OpenID
  Connect single sign-on into the webmail and the REST API.
  The domain is the one the account belongs to, however the name was written: a
  name under one of the domain's aliases, or a bare local part the default domain
  completes, meets that domain's policy (and the account's own, where it has one).
  The IMAP master user signing in as another account (`AUTHENTICATE PLAIN` with an
  authorisation identity) meets both accounts' policies: its own before the
  password, and the mailbox's after it - and the mailbox's compromise hold - so a
  master user in a domain with no policy cannot open a mailbox from a country the
  mailbox's domain refuses. Until 26 September 2026 only the master user's was asked.

Each is written as one string, because it is one decision:

| Value | What it means |
|---|---|
| *(empty)* | Nothing is judged by country. This is the default everywhere. |
| `allow:GB,IE,FR` | Only these countries may connect, or sign in. |
| `block:CN,RU` | These countries may not connect, or sign in. |

A country is its ISO 3166-1 alpha-2 code. Case and spacing do not matter on the
way in — `ALLOW: gb , ie` is accepted — and the value is stored in one canonical
spelling, `allow:GB,IE`, whichever surface wrote it. Anything else is refused
with a sentence that says what to write instead, in the same words on every
surface.

At most 255 characters, which is a block list of about eighty countries. An
administrator who wants more than that wants an allow list, and is told so.

------------------------------------------------------------------------------

The database
------------

Point `GeoIpDatabaseFile` at a **MaxMind DB** file — the `.mmdb` that MaxMind's
GeoLite2 and GeoIP2 country and city databases come in, which is what most
machines that need one already have on them for some other program.

```
C:\GeoIP\GeoLite2-Country.mmdb
/var/lib/GeoIP/GeoLite2-Country.mmdb
```

A GeoLite2-**City** database answers the same question and works just as well;
only the country is read out of it.

**Why the binary format and not the CSV.** The CSV form exists, but it is two
files that have to be joined, it carries no build date, and a server reading it
would have to hold a few million parsed ranges in memory behind an index of its
own invention. The `.mmdb` is one file, it carries its own build epoch — which
is what "stale" is measured against — and its index is part of the file, so a
lookup is a walk of 32 or 128 bits with nothing to build at start-up. It is also
what the administrator already has.

**The server never fetches it.** Getting the file, and keeping it fresh, is
yours: MaxMind's own `geoipupdate`, your distribution's package, or a scheduled
download. Replace the file in place; the server notices within five minutes (it
re-reads the file's size and modification time on that interval) and reloads it
without a restart. Changing `GeoIpDatabaseFile` reloads at once.

**Staleness.** `GeoIpDatabaseMaxAgeDays` (45 by default, 0 to say nothing) is how
old the file's own build date may be before the application log says so. A stale
database is **still used** — a stale answer beats no answer, because most of the
addresses in it have not moved — and the line names the file and the age so that
whoever reads the log knows what to do about it.

------------------------------------------------------------------------------

What is exempt
--------------

`CountryBlockingExemptRanges` is a comma-separated list of addresses and CIDR
ranges that **no policy ever judges**, in the same grammar the trusted-proxy and
never-ban lists use. What it says is:

```
127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fe80::/10,fc00::/7
```

— the loopback addresses and every private network. There is deliberately **no
second, hidden list compiled into the server**: what is exempt is exactly what
this setting says, so that you can read it, and so that a test can take it away.

The loopback address is in it because the webmail, the Control Panel, `hmctl` on
the machine itself and every local script come from there, and blocking it could
only ever lock you out of your own server. The private ranges are in it because
no geographic database has a country for them — RFC 1918 space is not allocated
to anybody — so a policy that judged them would judge every office machine as
"unknown", which an allow list refuses.

Emptying it is a thing you may mean; it says that your own network is to be
judged like anybody else's. Do it with `country_policy_http` off, or the Control
Deck is the next thing you lose.

**An open session is never disturbed.** Nothing consults a policy again once a
connection has been accepted, so changing a policy, replacing the database or
losing it does not drop a session that is already running and does not interrupt
one that has authenticated. A policy takes effect on the *next* connection.

------------------------------------------------------------------------------

The unknown country, and which way each policy fails
----------------------------------------------------

Two different situations, and they are answered differently on purpose.

**No database at all** — none configured, or the file is missing, unreadable or
not a MaxMind database. **Everything is allowed**, whatever the policies say, and
the reason is written to the application log once:

```
Country blocking: the geographic database could not be read, so no connection is
judged by country. C:\GeoIP\GeoLite2-Country.mmdb does not exist.
```

A mail server that refused every connection because a file was renamed is a worse
failure than one that stops applying a geographic policy — and the administrator
whose database has gone missing is the person who most needs to be able to sign
in and fix it. `Utilities.GeoIpDatabaseStatus` and the `geoip_database_status`
setting both say `"loaded":false` with the reason, so a monitoring check can see
it without reading the log.

A reload that fails keeps the database that was working, so a bad copy does not
silently switch a policy off.

**A database is loaded but has no country for that address** — an unallocated or
reserved range, or one the file is too old to know about. An **allow list refuses
it** (that is what an allow list means) and a **block list allows it** (it is not
in the list). This is the only place the two kinds of policy differ in what they
do about the unknown, and both settings say so in their own description.

------------------------------------------------------------------------------

What a refused client is told
-----------------------------

Nothing.

A refused connection is closed, exactly as a connection refused by an IP range is
closed: no banner, no status code, no text. A refused sign-in gets the ordinary
invalid-credentials reply — byte-identical to a wrong password, so it is not even
distinguishable from one.

That is deliberate. A client that learns "you were refused because you are in
Chile" has learned the policy, and the next thing it does is come back through
somewhere else. Your copy is in the application log, in the counters and in the
reports.

A sign-in refused on its country is **counted against neither the account nor the
address**: it does not feed the per-name lockout and it does not feed the per-IP
auto-ban. The person refused here is very often the account's real owner,
travelling — charging their attempts to the lockout would let a trip abroad lock
them out of their mailbox at home, and charging them to the auto-ban would ban a
hotel's NAT for everyone behind it.

------------------------------------------------------------------------------

What is recorded
----------------

**One line per refusal in the application log**, in a fixed shape, which is also
what a `fail2ban` filter would match:

```
Country blocking: refused SMTP from 203.0.113.9 (CN) for connection.
Country blocking: refused sign-in from 203.0.113.9 (CN) for example.com.
```

`??` in place of the country means the database had no answer for that address
and an allow list refused it for that reason.

**Counters**, in total and split by country:

| Metric | What it is |
|---|---|
| `hmailserver_country_blocked_total` | Every refusal since the server started. |
| `hmailserver_country_blocked_by_country_total{country="CN"}` | The same, by country. Emitted always, unlike the per-domain series: a country code comes from the database and not from the peer, so there are at most about 250 of them and nothing attacker-chosen to truncate. |

`country_blocked_total` is also sampled into the metric history (`hm_metricsamples`),
so `GET /api/v1/metrics/history?metric=country_blocked_total` and the reports can
draw it per day.

Neither is folded into `hmailserver_auth_failures_total`. A connection refused
before its greeting never attempted to authenticate, and a sign-in refused on its
country was not a wrong password; those are the numbers brute-force alerts are
built on and they stay clean.

**A report section**, `GET /api/v1/reports/countries`, and the *Country refusals*
section of the Control Deck's Reports page. Its per-day column is the increase in
`country_blocked_total` across each day of the window. Its split by country is
**not** windowed — nothing records a refusal per country per day — so it is the
counters as they stand since the server last started, and the section's own note
says so rather than letting you assume otherwise.

------------------------------------------------------------------------------

Setting it up
-------------

### The Control Deck

**Settings → Server** holds the database file, the staleness limit, the exempt
ranges, the four listener policies and the read-only status. Every field there is
drawn from the server's own OpenAPI document, so the descriptions above are the
ones you see.

**Domains → (a domain) → Country blocking** holds that domain's sign-in policy.

### REST

```
GET  /api/v1/settings
PUT  /api/v1/settings   {"geoip_database_file": "/var/lib/GeoIP/GeoLite2-Country.mmdb",
                         "country_policy_smtp": "block:CN,RU"}
PUT  /api/v1/domains/example.com  {"active": true, "country_policy": "allow:GB,IE"}
GET  /api/v1/reports/countries
```

`geoip_database_status` is read-only: it is a fact about the server, shown and
never sent. A PUT is all or nothing, so a policy the grammar refuses leaves every
other key in the same request unapplied.

### hmctl

```
hmctl settings get settings geoip_database_status
hmctl settings set --set geoip_database_file=/var/lib/GeoIP/GeoLite2-Country.mmdb
hmctl settings set --set country_policy_smtp=block:CN,RU
hmctl domain update example.com --set active=true --set country_policy=allow:GB,IE
hmctl report countries
```

### PowerShell

```powershell
Set-HmSetting -Property @{ geoip_database_file = '/var/lib/GeoIP/GeoLite2-Country.mmdb'
                           country_policy_smtp = 'block:CN,RU' }
(Get-HmSetting).geoip_database_status
Set-HmDomain -Name example.com -Property @{ active = $true; country_policy = 'allow:GB,IE' }
```

### COM (the desktop Control Panel's interface)

```vbscript
Settings.GeoIpDatabaseFile = "C:\GeoIP\GeoLite2-Country.mmdb"
Settings.GeoIpDatabaseMaxAgeDays = 45
Settings.CountryBlockingExemptRanges = "127.0.0.0/8,::1/128,192.168.0.0/16"
Settings.CountryPolicy("smtp") = "block:CN,RU"

Domain.CountryPolicy = "allow:GB,IE"

Utilities.LookupCountry("8.8.8.8")        ' "US", or "" when nothing is known
Utilities.GeoIpDatabaseStatus()           ' the JSON the Deck and hmctl show
```

`Settings.CountryPolicy` takes the protocol as its parameter: `smtp`, `imap`,
`pop3` or `http`. Anything else is refused.

------------------------------------------------------------------------------

Every setting
-------------

All of them live in `hm_settings` in the database — not in `hMailServer.ini` —
so they are shared by every node of an installation, they are in a configuration
backup, and a change reaches the running server at once.

| Setting | REST key | Default | What it is |
|---|---|---|---|
| `GeoIpDatabaseFile` | `geoip_database_file` | *(empty)* | The `.mmdb` file to read. Empty means nothing is judged by country. |
| `GeoIpDatabaseMaxAgeDays` | `geoip_database_max_age_days` | 45 | How old the file's own build date may be before the log says it is stale. 0 never says so. 0 to 3650. |
| `CountryBlockingExemptRanges` | `country_blocking_exempt_ranges` | loopback and the private networks | Addresses and CIDR ranges no policy judges. |
| `CountryPolicySMTP` | `country_policy_smtp` | *(empty)* | The SMTP listeners' policy. |
| `CountryPolicyIMAP` | `country_policy_imap` | *(empty)* | The IMAP listeners' policy. |
| `CountryPolicyPOP3` | `country_policy_pop3` | *(empty)* | The POP3 listeners' policy. |
| `CountryPolicyHTTP` | `country_policy_http` | *(empty)* | The REST API and the web services. Set it with care: it is the listener the Control Deck arrives on. |
| *(per domain)* | `country_policy` | *(empty)* | The country policy applied when an account of that domain signs in. |

------------------------------------------------------------------------------

What this does not do
---------------------

* **It does not name a country on every session or sign-in line.** The connect
  line and the logon lines are unchanged: looking an address up costs a tree walk
  on every connection, and paying it on a server with no policy configured, for a
  log line, is not a trade worth making. Where a country matters — a refusal — it
  is in the line.
* **It is not a per-recipient or per-message control.** The per-domain policy is
  about signing in. Mail *to* a domain from a blocked country is refused by the
  SMTP listener's policy or not at all.
* **It says nothing about a proxy.** The address judged is the one the TCP
  connection actually came from. On a listener behind HAProxy with the PROXY
  protocol enabled, that is the proxy — which will be in your exempt ranges —
  and the real client's address, which arrives in the PROXY header after the
  connection has been accepted, is not judged. Put the geographic policy on the
  proxy in that topology.
* **It is only as true as the database.** A VPN, a relay or a mobile carrier's
  exit point puts a person in a country they are not in, and a new allocation may
  not be in a file that is a month old. Country blocking narrows the noise; it is
  not an authentication control, and nothing here should be the only thing
  standing between an attacker and a mailbox.
