# The first-hour checklist

What a new server still needs before it is a mail server anybody can use, judged
by the server itself rather than by whoever is reading the documentation. Ten
items, in the order a new installation meets them:

| Key | Item | Done when |
|---|---|---|
| `profile` | A profile for this server | a setting profile has been applied ([Profiles.md](Profiles.md)); asked for on a young server - fewer than ten accounts, the sample domain's not counted - and not needed on one past that, where a profile that closes a listener or turns a filter off is not something to suggest in a list of things left to do, nor where the shipped settings were chosen instead; asked for again when a profile could not be written in full |
| `domain` | A domain | at least one domain exists and is active |
| `account` | An account | at least one account exists in any domain |
| `submission` | TLS on the submission port | every SMTP listener other than port 25 has STARTTLS or implicit TLS and a certificate that exists, other than the self-signed one a new installation made ([SecureDefaults.md](SecureDefaults.md)), which mail clients warn about |
| `sign_in_tls` | Passwords only over TLS | every IP range that lets a client connect from an address other than this computer's requires TLS for authentication, or reaches only listeners that start TLS before a password ([SecureDefaults.md](SecureDefaults.md)) |
| `spf` | An SPF record | every active domain has a TXT record beginning `v=spf1` |
| `dkim` | DKIM signing | every active domain signs, its selector and key file are set, the key file exists, and a TXT record with a `p=` tag is at `<selector>._domainkey.<domain>` |
| `dmarc` | A DMARC record | every active domain has a TXT record beginning `v=DMARC1` at `_dmarc.<domain>` |
| `backup` | A backup destination | the backup destination is set and the folder exists on the server |
| `second_factor` | A second factor for the administrator | the administrator password is enrolled with a one-time code |

Each item is in one of four states, and an item is as bad as its worst finding:

| State | Meaning |
|---|---|
| `done` | the server found the thing in place |
| `action_needed` | it is missing or wrong, and the finding says what |
| `cannot_tell` | the server could not find out: a DNS lookup failed, or the caller asked for none. This says nothing either way |
| `not_needed` | there is nothing to do, because nothing depends on it yet (the three record items, with no active domain; the passwords item, on a server only this computer may connect to) |

The judgement is made from what the server can see: its own configuration, the
files it would open, and DNS lookups made the way a receiving server makes them.
Port 25 is never the submission port, whatever its security setting: it is the
server-to-server port, and clients are not sent there. A plain submission port
beside a secured one still needs action, because the plain one is the one a
badly configured client will use. A listener whose certificate is the
self-signed one a new installation made needs action too: its TLS works, and
every mail client warns about it until a certificate authority's replaces it -
ACME's does, and takes the listener over when it arrives. The passwords item
judges every IP range that is not all loopback and lets a client connect over
SMTP, POP3 or IMAP: done when the range requires TLS for authentication, or when
every listener it may use - a listener bound to a loopback address only is not
one - has implicit TLS or requires STARTTLS; otherwise it
needs action and names the listeners - no TLS, or STARTTLS a client is free not
to start - on which a password from there would cross the network in the clear.
It is `sign_in_plain`, with the fix below, when every one of those listeners
offers STARTTLS with a certificate that is there, and `sign_in_no_tls`, with no
fix, when one of them has no TLS to start: requiring TLS for sign-in then would
refuse every client there on that listener, so the finding says to give those
listeners a certificate and STARTTLS first.
A server whose ranges are all on the loopback takes no password from a network,
and the item is not needed. The DKIM item checks that a record is
published; whether it carries the key the domain signs with is the DKIM
rotation check's business (`ReputationWatch.md`'s neighbour, `DkimRotation.h`),
which compares the record with the private key file.

## Where it is

**Over REST**, `GET /api/v1/setup/checklist`. The route is the server's rather
than a domain's - the submission port, the backup destination and the
administrator's second factor belong to no domain - so a credential confined to
named domains is refused it, and reading needs `view_configuration`. It is a
read: nothing changes, and the only traffic is the DNS lookups. `?dns=0` skips
the lookups; the three record items then say which host to look up
(`record_not_looked_up`) and are `cannot_tell`.

**Over COM**, `Utilities.GetSetupChecklist(LookUpDns)`, the same document, for
the server administrator.

**In the Control Deck**, the *Checklist* view, first in the navigation after the
dashboard, with a card per item and *Check again*.

**In the Control Panel**, the *External setup* page and the dashboard's
"needs attention" card carry the first-hour items ahead of the twelve external
prerequisites they already had. The panel asks the server for the list without
DNS lookups, so that its window is not held for the resolver's timeout, and
looks the SPF and DMARC records up itself off the window thread, as it already
did for DKIM; the server's DKIM item is left out there, because the panel's own
DKIM item verifies the record against the key file.

## The document

```json
{"generated": 1789000000, "dns_looked_up": true,
 "done": 3, "action_needed": 4, "cannot_tell": 0, "not_needed": 1,
 "items": [
   {"key": "domain", "state": "done",
    "findings": [{"state": "done", "code": "domain_count", "args": ["1", "1"],
                  "text": "Active domains: 1 of 1."}]},
   ...]}
```

Every finding carries a code and its arguments beside an English sentence, so
that the Deck and the Control Panel say the same thing in their own languages
while a script switches on the code. The arguments are strings whatever they
count.

A finding the server can put right in one step also carries **`fix`**: the REST
request that takes the step, as `{"method", "path", "body"}`. Only
`sign_in_plain` has one - `PUT /api/v1/ipranges/{id}` with
`{"require_tls_for_auth": true}`, offered only when every listener it names can
start TLS - and the Deck draws it as a button, *Require TLS for sign-in from this
range*, which asks first and then sends exactly that. For `sign_in_no_tls` the
Deck draws a button to its Ports view instead.
The route answers it as it answers any caller: `manage_settings` is needed, and
the Deck draws the button only for a sign-in that has it. A script may send it
too. The Control Panel shows the item and opens its IP ranges page.

```json
{"state": "action_needed", "code": "sign_in_plain", "args": ["Internet", "25, 110, 143, 587"],
 "text": "The IP range Internet takes a password without TLS on port 25, 110, 143, 587, ...",
 "fix": {"method": "PUT", "path": "/api/v1/ipranges/2", "body": {"require_tls_for_auth": true}}}
```

| Code | Arguments | Sentence |
|---|---|---|
| `profile_none` | | No profile has been applied. A profile sets about thirty settings for the kind of server this is - a small office, behind a filtering gateway, an outbound relay, a high-volume sender - and shows every change before it is made. |
| `profile_in_service` | accounts | This server holds {0} accounts, so it is in service: a profile is offered to a new server, and any of the four can still be compared with this one. (not_needed) |
| `profile_applied` | profile, date | The {0} profile was applied on {1}. |
| `profile_kept` | profile, date | The {0} profile was applied on {1}; its way back was discarded and its settings kept. |
| `profile_reverted` | profile, date | The {0} profile was applied and then reverted on {1}; the settings are the ones it replaced. (not_needed) |
| `profile_declined` | date | The settings this version ships with were chosen on {0}, and no profile applied; any of the four can still be compared with this server. (not_needed) |
| `profile_failed` | profile, date | The {0} profile could not be applied in full on {1}: some of its settings were not written. Put back what it wrote, or keep it, and apply it again. |
| `domain_none` | | No domain has been added yet. |
| `domain_none_active` | total | {0} domains, none of them active. |
| `domain_count` | active, total | Active domains: {0} of {1}. |
| `account_none` | | No account has been added in any domain. |
| `account_count` | count | Accounts: {0}. |
| `submission_none` | | No port takes message submission, so mail clients cannot send through this server. |
| `submission_plain` | port | Port {0} takes submission without TLS, so passwords and mail would cross the network in the clear. |
| `submission_no_certificate` | port | Port {0} is set to TLS but has no certificate, so TLS cannot start. |
| `submission_tls` | port, certificate | Port {0} offers TLS with the certificate {1}. |
| `submission_self_signed` | port, certificate | Port {0} offers TLS with {1}, the self-signed certificate this installation made: mail clients warn about it until it is replaced with one a certificate authority issued, which ACME can fetch from Let's Encrypt. (action_needed) |
| `sign_in_plain` | range, ports | The IP range {0} takes a password without TLS on port {1}, so a client there that signs in before STARTTLS sends it across the network in the clear. (action_needed, with `fix`) |
| `sign_in_no_tls` | range, ports | The IP range {0} takes a password without TLS, and port {1} has no TLS a client could start: give those ports a certificate and STARTTLS first, then require TLS for sign-in from the range. (action_needed, no `fix`) |
| `sign_in_tls_required` | range | The IP range {0} takes a password only over TLS. |
| `sign_in_tls_ports` | range | Every port the IP range {0} may use starts TLS before it takes a password. |
| `sign_in_local_only` | | Only this computer may connect, so no password crosses a network. (not_needed) |
| `no_active_domain` | | No active domain, so there is nothing to publish yet. |
| `record_published` | domain, kind, host | {0}: the {1} record at {2} is published. |
| `record_missing` | domain, kind, host | {0}: no {1} record at {2}. |
| `record_not_looked_up` | domain, kind, host | {0}: the {1} record at {2} was not looked up. |
| `lookup_failed` | domain, kind, host | {0}: the lookup for {2} failed, which says nothing about the {1} record. |
| `domains_not_looked_up` | count | {0} more active domains were not looked up. |
| `dkim_off` | domain | {0}: signing is off, so receivers cannot verify the mail as this domain's. |
| `dkim_no_selector` | domain | {0}: signing is on but no selector is set. |
| `dkim_key_missing` | domain, file | {0}: signing is on but the key file {1} was not found. |
| `backup_none` | | No backup destination is set. |
| `backup_folder_missing` | folder | The backup destination {0} does not exist on the server. |
| `backup_set` | folder | Backups go to {0}. |
| `second_factor_on` | | The administrator's password is backed by a one-time code. |
| `second_factor_off` | | The administrator signs in with a password alone. |

The three record items look the first twenty active domains up; the rest are
counted in `domains_not_looked_up`, and the Deck's DNS records view checks any
one of them on request. Each missing record costs the resolver's timeout, which
is why a server hosting hundreds of domains - a server past its first hour - is
not asked to wait for all of them.

## Where the code is

`Common/Util/SetupChecklist.cpp` builds the document; `RestApiSetupChecklist.cpp`
is the route; `InterfaceUtilities::GetSetupChecklist` the COM method; the Deck's
view is `renderChecklist` in `installation/WebAdmin/index.html`; the Control
Panel's items are `ExternalSetupChecks.FirstHourItems`. The regression fixture is
`test/RegressionTests/API/RestApiSetupChecklist.cs`: the ten items in order,
the records looked up in the suite's own zone for two domains, the skipped lookup
saying so, the submission port judged with a certificate that exists and with the
self-signed one a new installation makes, the passwords item's fix sent as the
finding gives it and withheld while a listener has no TLS, and the backup
destination judged on disk. The Deck's button is
`build/deck-script-test.js`'s.
