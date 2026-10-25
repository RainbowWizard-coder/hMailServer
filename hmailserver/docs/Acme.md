# Automatic certificates (ACME)

The server can get its own TLS certificate from a certificate authority that
speaks ACME (RFC 8555) - Let's Encrypt by default - and renew it before it
expires. It answers the CA's **http-01** challenge itself, on port 80, so the
names it asks for must resolve to this server (or to a proxy that passes
`/.well-known/acme-challenge/` on to it) and port 80 must be reachable from the
internet. It does not do dns-01, and so no wildcard certificates.

One certificate carries every name. The client asks for one certificate whose
subject alternative names are the hosts `AcmeDomains` lists, writes it to the
certificate directory, records it as the SSL certificate **ACME (automatic)**,
gives it to every TLS listener that has no certificate or holds the self-signed
one a new installation made ([SecureDefaults.md](SecureDefaults.md)), and
restarts the listeners so that they load it. A listener holding a certificate
somebody chose keeps it.

## The settings

All in the `[Settings]` store (the Control Panel's *Automatic certificates
(ACME)* page, `GET /api/v1/settings/ini`, COM `Settings.SetIniSetting`):

| Setting | What it is | Applies |
|---|---|---|
| `AcmeEnabled` | `1` to issue and renew. | at a reinitialise |
| `AcmeContactEmail` | The address the CA writes to about expiry. | now |
| `AcmeDomains` | The host names, separated by commas. Read in lower case, trimmed, each once, with a final dot taken off (`mail.example.com.` is `mail.example.com`). A name no certificate can carry is left out - see below. | now - and a save in the server's own process that adds a host the certificate lacks starts a check |
| `AcmeDirectoryUrl` | The CA's directory. `https://acme-v02.api.letsencrypt.org/directory` by default; Let's Encrypt's staging directory is `https://acme-staging-v02.api.letsencrypt.org/directory`. | now |
| `AcmeHttpPort` | Where the challenge is answered when the web services listener does not already hold that port. `80`. | now |
| `AcmeReuseKey` | `1` (the default) keeps the private key across renewals, so a published DANE TLSA `3 1 1` record stays valid. | now |
| `AcmeCertificateDirectory` | Where `fullchain.pem`, `privkey.pem` and `account.key` are kept. Empty is `Data\ACME`. | at a reinitialise |

A name in `AcmeDomains` that no certificate this server orders can carry is
**left out** of every check and every order, and the status names it in
`unusable_domains`; the application log says so at each check. That is a name
that is not a host name of letters, digits and hyphens - a wildcard
(`*.example.com`: http-01 cannot validate one), an IP address, a name with a
space, a semicolon or another character a host name cannot hold - and a name
past ASCII: write an internationalised name in its `xn--` form. Such a name would
otherwise be missing from every certificate, so that every check found the
certificate due and every order, which names every host, failed - the renewal
for the other hosts with it.

`AcmeDirectoryUrl` must be `https`. Plain `http` is taken only to an address
on this machine (`localhost`, `127.0.0.0/8`, `::1`), which is how the
regression suite stands a CA of its own; every URL the CA's directory and
orders hand on is held to the same rule.

## When a certificate is issued

A check decides, in this order. The certificate is due when:

1. **there is none** that can be read in the certificate directory;
2. **it does not name every host `AcmeDomains` lists** - due at once, whatever
   its dates. A name it carries that starts with `*.` covers one label in its
   place, as a mail client matches it. A host taken off the list does not make
   it due: the certificate still serves the rest, and the next renewal drops
   the name;
3. **the CA's renewal window has come**, when the CA publishes one (ACME
   Renewal Information, RFC 9773). The point inside the window is fixed per
   certificate, as the RFC asks;
4. **two thirds of its life have passed**, never later than a day before it
   expires.

Until GitLab work item 98 the second rule was not there: a host added to
`AcmeDomains` waited until the certificate in use was due for renewal - about
sixty days for a ninety-day certificate - before any certificate named it.

A check runs:

* when the server starts, and at every reinitialise;
* every hour;
* when `AcmeDomains` is saved with a host the certificate in use does not
  name, through a route that saves it in the running server: the Control Panel,
  COM (`Settings.SetIniSetting`) or REST (`PUT /api/v1/settings/ini/AcmeDomains`) -
  unless the spacing below holds it, when the log says so and the hourly check
  takes it;
* when it is asked for: `POST /api/v1/acme/run`, COM `Utilities.RunAcmeCheck`,
  or *Check now* on the Control Deck's *Certificates* page.

`hmailserver --set-setting AcmeDomains=...` on Linux is a process of its own: it
stores the value in the database, and the running server does not read it until
it is reinitialised (`systemctl reload hmailserver`) or started, as the command
says. The check at that start then finds the new host and issues for it; until
then every check, the hourly ones included, reads the list the server holds.

A check that finds nothing due asks the CA for its directory and its renewal
window and nothing else.

## Asking for a check

`POST /api/v1/acme/run` takes no body but `{}`. **It takes no names**: the
names are `AcmeDomains`, which only a server administrator can set, and whether
a certificate is issued is decided by the rules above, not by the caller. To
add a host, save it in `AcmeDomains` - which starts a check by itself - and
read the status; the run is for asking again after something outside the
server has been put right, a DNS record or a proxy.

It answers at once:

| Status | When | Body |
|---|---|---|
| 202 | A check is queued, or one was already queued or running | `accepted`, `already_running`, `attempt` (the queued or running check) and `status` (`/api/v1/acme`) |
| 400 | A body other than `{}` | `error` |
| 409 | ACME is off (`reason: disabled`), or `AcmeDomains` is empty (`no_domains`) | `error`, `reason` |
| 429 | Not yet (below): `reason` is `interval`, `cooldown` or `rate_limited` | `error`, `reason`, `retry_at`, `retry_after_seconds`, and a `Retry-After` header |
| 503 | The server is not running its services | `error` |

`Utilities.RunAcmeCheck` returns the 202's document and raises an error with
the sentence for the others.

## Reading how it went

`GET /api/v1/acme`, or `Utilities.GetAcmeStatus`, which returns the same
document. It is read from the certificate file and from this process; the CA is
not asked. Times are Unix seconds, and null when there is none.

| Member | What it says |
|---|---|
| `enabled`, `directory_url`, `domains`, `certificate_directory` | The settings, `domains` as the client reads them. |
| `unusable_domains` | The names of `AcmeDomains` left out because no certificate can carry them, as written. |
| `certificate` | The certificate in use, or null: `file`, `names` (its subject alternative names, or its common name when it has none), `subject`, `issuer`, `serial`, `not_before`, `not_after`, `expired`, `renews_at` (rule 4 above) and `deployed` (whether the ACME (automatic) record names it). |
| `covers_domains`, `missing_domains` | Whether it names every configured host, and the ones it lacks. |
| `due`, `due_reason` | Rules 1, 2 and 4, judged from the file: `missing`, `names` or `lifetime`. |
| `running` | The check queued or running now, or null. |
| `last_attempt` | The last check that finished, whatever it found. |
| `last_issuance` | The last check that tried to issue - an hourly check that found nothing due does not push it out of sight. |
| `last_issued_at` | When this process last installed a certificate. |
| `next_scheduled_check_at` | The hourly check, within the minute the scheduler wakes in. |
| `requests` | `allowed_now`, `waiting_for` and `next_allowed_at`, and the two spacings below in seconds. |
| `rate_limited_until` | The time the CA's last rate limit gave, while it lasts. |

A check, in `running`, `last_attempt` and `last_issuance`:

| Member | What it says |
|---|---|
| `id` | A number this process gives each check; a start begins again at 1. |
| `trigger` | `start`, `schedule`, `settings` (a save of `AcmeDomains`) or `request`. |
| `state`, `step` | `queued`, `running` or `finished`, and while it runs how far it has got: `deciding`, `directory`, `account`, `order`, `authorization`, `finalize`, `certificate`, `install`. |
| `queued_at`, `started_at`, `finished_at` | |
| `outcome` | `issued`, `failed`, `not_due`, `waiting` (due, and held by the spacing below) or `disabled`. |
| `reason` | Why it was due: `missing`, `names`, `ari` or `lifetime`. |
| `failed_step`, `error` | The step that failed and a sentence about it in the server's own words. |
| `ca_problem` | The CA's own explanation, when it sent one: the problem document's `type` (`urn:ietf:params:acme:error:rateLimited`, `...:connection`, `...:dns`, `...:unauthorized`) and its `detail`, cut to 300 characters. |

Nothing in either document is a secret: never the account key, the private
key, a nonce or the account's URL at the CA. The application log has the CA's
whole answer at each failing step, as it always had.

## How often, and why

Let's Encrypt counts what an account does: five failed validations per host per
hour, refilled one every twelve minutes; three hundred new orders per three
hours; five certificates per week for exactly the same set of names. A
provisioning script that asks for a check in a loop while a DNS record is
missing could spend the first of those in minutes, and a setup that cannot
validate would then fail its renewal too. So every check keeps three rules,
whatever started it:

* **One at a time.** A request while a check is queued or running is answered
  202 with that check, and starts no other.
* **A requested check at most once a minute** (`interval`, 60 seconds): one
  that finds nothing due still asks the CA for its directory and its renewal
  window. Read the status instead of asking again.
* **No order within twenty minutes of an attempt that failed** (`cooldown`,
  1,200 seconds) - requested, saved, hourly or at start alike. Attempts
  twenty minutes apart fail at most three times an hour, under the five Let's
  Encrypt allows. A check that is due inside those minutes ends `waiting` and
  orders nothing.
* **None before the time a CA's rate limit gave** (`rate_limited`): when the CA
  answers `urn:ietf:params:acme:error:rateLimited`, no check orders until its
  `Retry-After` has passed - an hour when it gives none, at most seven days -
  as RFC 8555 section 6.6 asks.

A certificate is issued only when one is due, so these do not need to guard the
weekly duplicate-certificate limit: after a certificate that names every host
is installed, the next check finds nothing due.

The cooldown and the rate limit belong to the CA they were learnt from: a change
of `AcmeDirectoryUrl` starts them afresh. They are kept in memory, not in the
database - they are what this process has seen, not configuration - so a
restart forgets them, and the first order after it learns a rate limit again
from the CA's answer to it.

## What a failed attempt leaves

The certificate in use, as it was. Nothing is written until the CA has issued,
and what it sends is installed only when it is a PEM certificate **for the key
the request was made with**, that **names every host asked for** and that **has
not expired**; otherwise the attempt fails at the `certificate` step and says
which. The new key and certificate are written under temporary names and moved
into place only when both are written, the key first.

A pair on disk that no ACME (automatic) record names - a certificate issued and
never deployed - is deployed by every check, whether or not a new one is due
and whether or not issuing it succeeds.

## Who may

| | Status | Run |
|---|---|---|
| REST | `view_configuration`: the administrator password, a full or read-only key with no domain list, a server administrator account, the auditor role | `manage_server`: not a read-only key, not the auditor |
| COM | a server administrator | a server administrator |

Both are refused to a key or a role confined to named domains: the certificate
names hosts of every domain, and a check of it orders a certificate for all of
them. [WebAuthorisationMatrix.md](WebAuthorisationMatrix.md) has the rows.

Every request that queues a check is on the audit trail, as `ssl certificate`,
`ACME (automatic)`, `check requested` (the Control Panel's audit trail page reads
it as *TLS certificate*, *Check requested*), with the names and the directory,
under the credential that asked. A refused request changes nothing
and is not recorded; the save of `AcmeDomains` is recorded by the settings
store as every setting is ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)).

## Provisioning a new mail host

For a hosting platform that adds a customer's `mail.example.net` beside
`mail.example.com`:

1. Publish `mail.example.net` in DNS, pointing at this server (or at the proxy
   in front of its port 80).
2. Add it to `AcmeDomains`. Over COM, as a server administrator:

   ```powershell
   $app = New-Object -ComObject hMailServer.Application
   $app.Authenticate("Administrator", $password) | Out-Null
   $app.Settings.SetIniSetting("AcmeDomains", "mail.example.com,mail.example.net")
   ```

   or over REST, `PUT /api/v1/settings/ini/AcmeDomains` with
   `{"value":"mail.example.com,mail.example.net"}`. The `[Settings]` routes
   answer to the administrator password alone, never to an API key, since the
   section holds the server's secrets and the commands it runs; an API key reads
   the status and asks for a check, and a full one with no domain list does both.
   The save starts a check: the certificate does not name the new host - unless
   an attempt failed within the last twenty minutes or the CA's rate limit
   holds, when the save starts none and the hourly check issues later.
3. Note the time of the save, and read the status -
   `$app.Utilities.GetAcmeStatus()`, or `GET /api/v1/acme` - until `running` is
   null and `last_issuance.finished_at` is at or after that time. Then
   `last_issuance.outcome` is `issued` and `covers_domains` true, or `failed`
   with `failed_step`, `error` and `ca_problem`. An earlier `last_issuance` is
   another attempt's - another host's, perhaps - and says nothing of this one.
   When `running` is null and no such `last_issuance` has come, the save
   started no check: `missing_domains` names the host still lacking,
   `requests.waiting_for` and `requests.next_allowed_at` say what held it and
   until when, and the first hourly check after that time
   (`next_scheduled_check_at`) issues for it.
4. If it failed - the record was not published yet, the proxy did not pass the
   challenge on - put that right and ask again (`Utilities.RunAcmeCheck`, or
   `POST /api/v1/acme/run`) once the time in `requests.next_allowed_at` has
   passed, or leave it to the hourly check. The answer's `attempt.id` is the
   check to wait for: read the status until `last_issuance.id` (or
   `last_attempt.id`, for a check that found nothing due) is at least that.

**A host that cannot be validated holds up every host.** There is one
certificate, and each order names every host in `AcmeDomains`: the first
authorization that fails ends the attempt, so while a host stays on the list that
does not resolve to this server, or whose port 80 does not reach it, no
certificate is issued - and the one the other hosts use is not renewed either,
until it expires (the error log's `HM5992` warns seven days before). Take a host
that cannot validate - a customer who has gone, a name not yet published - off
`AcmeDomains` promptly.

**Installing a certificate restarts the servers.** When a check installs a new
certificate it reinitialises the server, as it always has, so that the listeners
load it: for the few seconds that takes no SMTP, IMAP or POP3 listener accepts a
connection, and the sessions in flight are cut - unless `ShutdownDrainSeconds`
gives them a window to finish - for every host, not only the new one. Existing
hosts keep the certificate they had until then, and every host is served with
the new one after it. Maintenance mode (`POST /api/v1/server/drain`), if it is
on, ends with the reinitialise; entering it before asking for the check, and
asking once the sessions have finished, is the way to install a certificate with
no session cut.

## What it does not do

* **One certificate for every host.** There is no choice of certificate per
  host name (SNI); a host is added to the one certificate, which Let's Encrypt
  allows up to a hundred names. A certificate per customer is a separate
  request, not this.
* **http-01 only**: no dns-01, so no wildcard and no host that cannot be
  reached on port 80.
* **No operation history.** The status keeps the running check, the last one
  and the last that tried to issue; the application log and the audit trail
  keep the rest.

## In the log

* `ACME: The certificate in use does not name ...` - rule 2, and the check
  that follows.
* `ACME: AcmeDomains holds ..., which no certificate this server orders can
  name ...` - a name left out of the check and the order (`unusable_domains`).
* `ACME: AcmeDomains was saved with a host the certificate in use does not
  name; a check is queued now ...` - the save that started a check.
* `ACME: A certificate is due (...), and this check orders none: ...` - held by
  the cooldown or a rate limit.
* `ACME: The certificate the CA issued was not installed: ...` - the CA sent a
  certificate for another key, without a host asked for, or expired.
* `HM5992` (error log): an attempt failed and there is no usable certificate,
  or the one in use expires within seven days or has expired. `HM5993`: the new
  key was installed and the certificate could not be moved beside it. `HM6100`:
  a port could not be given the certificate.

## Where the code is

`hmailserver/source/Server/Common/Util/AcmeClient.cpp`: the client
(`AcmeClient`), the checks and their spacing (`AcmeChecks`), the scheduled task
(`AcmeRenewalTask`) and the rules' self-test (`AcmeChecksTester`, run by
`ClassTester` on Windows and `acme-tests` on Linux). The routes are in
`RestApiCertificates.cpp`; the COM methods in `COM/InterfaceUtilities.cpp`;
the subscription on `AcmeDomains` in `Application::SubscribeToSettings_`. The
regression fixtures are `API/RestApiAcme.cs`, `Security/TlsAcmeIssuance.cs`,
`Security/TlsAcmeDeployment.cs` and `Security/TlsAcmeRenewal.cs`, with the CA
of `Shared/FakeAcmeCa.cs`.
