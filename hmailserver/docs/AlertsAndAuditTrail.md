Alerts, and the audit trail
===========================

Two things an administrator asks for on the first day and, until 15 September
2026, could not have.

**Nothing told anybody anything.** Six conditions in this server already notice
that something is wrong — the disk filling, a work queue stalling, a certificate
a week from expiry, a run of auto-bans, a minidump written, a backup that failed
— and each of them wrote a line in a log file and stopped. A log file nobody is
reading is not a notification.

**Nothing recorded who changed what.** A setting was different from yesterday
and there was no way to find out who had changed it, from where, or what it had
been. The first thing a regulated customer asks for is the second thing on this
page.

---

The audit trail
---------------

### What is recorded

Every **administrative change** made over COM, REST or the Deck, with the
exceptions listed under *What this does not do* at the end of this page: a
configuration row created, changed or deleted; every server setting written
— both kinds, the settings in `hm_settings` and the `[Settings]` keys that moved
from `hMailServer.ini` into `hm_inisettings` at schema 6042; a lockout lifted
in any of its forms - an account unlocked, a POP3 mailbox lock cleared, the
logon-failure list cleared - which change no audited row and no setting, and so
are recorded where they are made; and a mailbox renamed or moved to another
domain, which is the opposite shape - so many rows, and so plainly one act, that
recording each would bury it. One row per change, in `hm_audit`:

| Column | What it holds |
|---|---|
| `audittime` | When, in seconds since the epoch, UTC. |
| `auditactor` | Who. `Administrator` over COM; `administrator` or `key:<id>` over REST — the same identity string the REST log line uses. An account signed in with its own password — a domain administrator, a help desk, an auditor — is its address over COM and `account:<address>` over REST and the Deck. |
| `auditactortype` | `administrator`, `account` or `apikey`. Read from the role the credential holds, never from `AdminLevel` alone: `administrator` is the administrator password and an account whose role is `serveradmin` or `domainadmin`; every other account is `account`, one holding a narrower role such as `helpdesk` included. See [Roles.md](Roles.md). |
| `auditinterface` | `COM`, `REST` or `Deck`. |
| `auditaddress` | Where from. The peer address for REST and the Deck; empty for COM (see below). |
| `auditobjecttype` | `domain`, `account`, `alias`, `rule`, `setting`, and so on; `logon failures` for the failure list cleared, `spam trap hits` for the record of trap hits cleared. |
| `auditobjectname` | The name a person would recognise — the domain name, the address, the setting's name. For a role it is the role's name, and for a role assignment the address of the account it is for. For a greylisting white address it is the address as it was typed (`10.78.0.?`), because the column holds the SQL `LIKE` pattern that was made of it (`10.78.0._`) and that is an address no other surface shows; the detail still carries the stored pattern. |
| `auditaction` | `created`, `updated` or `deleted`; `unlocked` for an account let back in or a mailbox lock cleared; `cleared` for the logon-failure list; `moved` for a mailbox renamed or moved to another domain, and `move failed` for one that failed and could not be put back. |
| `auditdetail` | The columns the change carried, or, for a setting, the value it had and the value it was given. When the credential acted under a role, the detail ends with `[role: <name>]` — `[role: helpdesk]`, `[role: serveradmin]` — which is inside the hashed field, so a role edited afterwards breaks the chain like anything else. A row of `hm_roleassignments` is nothing but ids, so a role assignment's detail is written by the code that made the change rather than read from the statement: the role and what it reaches, as they were and as they are - `role: helpdesk -> auditor; domains: the account's own domain -> every domain` - and a role's is its permissions and description, the same way. A row of `hm_spamtraps` is the same shape — a domain id and a local part going in, a row id coming out — so a trap declared or taken back is recorded with the address itself. |
| `auditprevhash`, `audithash` | The chain. See *Verifying the chain*. |

The configuration tables recorded, and the object type each is recorded as:

| Table | `auditobjecttype` |
|---|---|
| `hm_domains`, `hm_domain_aliases` | `domain`, `domain alias` |
| `hm_accounts`, `hm_aliases`, `hm_apppasswords` | `account`, `alias`, `application password` |
| `hm_distributionlists`, `hm_distributionlistsrecipients` | `distribution list`, `distribution list recipient` |
| `hm_groups`, `hm_group_members` | `group`, `group member` |
| `hm_acl` | `folder permission` |
| `hm_routes`, `hm_routeaddresses` | `route`, `route address` |
| `hm_rules`, `hm_rule_criterias`, `hm_rule_actions` | `rule`, `rule criterion`, `rule action` |
| `hm_securityranges`, `hm_tcpipports`, `hm_sslcertificates` | `ip range`, `tcp/ip port`, `ssl certificate` |
| `hm_incoming_relays`, `hm_remotedomainpolicies` | `incoming relay`, `remote domain policy` |
| `hm_warmup` | `warm-up schedule` - an IP warm-up schedule ([IpWarmup.md](IpWarmup.md)); its daily counts, `hm_warmupcounts`, are the queue's own writes and are not recorded |
| `hm_inboundhooks` | `inbound webhook` - an address or a domain whose mail is posted to a URL ([InboundWebhooks.md](InboundWebhooks.md)), under whoever made, changed or removed it, an API key among them, and named by its address; its secret and its URL - a receiver's URL often carries a token - are recorded as changed, never by their values. Its queue, `hm_inboundhookqueue`, is traffic and is not recorded, but a kept message downloaded, retried or removed is recorded as an event (`inbound webhook delivery`, `downloaded`, `retried`, `removed`) |
| `hm_dnsbl`, `hm_surblservers` | `dns blacklist`, `surbl server` |
| `hm_whitelist`, `hm_greylisting_whiteaddresses` | `white list address`, `greylisting white address` |
| `hm_blocked_attachments`, `hm_blocked_senders` | `blocked attachment`, `blocked sender` |
| `hm_servermessages`, `hm_fetchaccounts`, `hm_alertrules` | `server message`, `fetch account`, `alert rule` |
| `hm_archiveindex` | `archive entry` |
| `hm_senderlist` | `sender list entry` |
| `hm_spamtraps` | `spam trap` |
| `hm_roles`, `hm_roleassignments` | `role`, `role assignment` |
| `hm_settingoverrides` | `setting level` - a domain's or an account's own value of a setting with levels ([SettingLevels.md](SettingLevels.md)), named by the setting, with its scope (1 a domain, 2 an account), owner id and value in the detail |
| `hm_smimeca`, `hm_smimeissued` | `S/MIME authority`, `S/MIME certificate` - a domain's own S/MIME authority ([SmimeAuthority.md](SmimeAuthority.md)): a root created, rotated, retired or its key wiped, named by the domain, its private key recorded as `(changed)` and never by its value; a certificate issued, confirmed or revoked, named by its address and serial, under whoever did it - an administrator, the account itself, or whoever deleted the account; a deleted account's number taken off its certificates, named by its address; an erased certificate, named by its serial alone |
| `hm_migrations`, `hm_migrationjobs` | `migration`, `migration job` - a batch of mailboxes copied from another server and each mailbox of it ([Migration.md](Migration.md), *Five hundred mailboxes*), as an administrator creates, starts, pauses, resumes or deletes one, or adds, retries or cancels a job - a job named by its mailbox and its target, `anna@old.example -> anna@example.com`, and the cancel of a running job recorded as a `cancelled` event, since the job's own thread writes that row; the job's secret is recorded as changed, never by its value, and a job's own progress, written by its thread, is not recorded |

The archive index is mostly traffic, and is listed for the two writes of it
that only an administrator makes: placing or releasing a legal hold
(`POST`/`DELETE /api/v1/archive/{id}/hold`, COM `Utilities.SetArchiveHold`),
recorded as `archive entry` `updated`, named `archiveid <id>`, with `hold=1` or
`hold=0` in its detail; and an address erasure removing the index rows that
name the address. A copy being indexed as it is received, and the retention
pass removing an expired one, run with nobody behind them and record nothing.

Every other table the server creates is named in `build/check-audit-choke-point.py`
with the reason it is not recorded: messages and everything derived from them
are traffic, and calendars, contacts, webmail preferences, attached files,
scheduled sends and S/MIME keys are an account's own data rather than server
configuration. The quarantine and the folder table are the two that are not so
simple, and both are under *What this does not do*.

A setting's row reads `Name: old -> new`. For a `[Settings]` key, a side on which
there is no stored value is `(default)`: the first time a key is set it reads
`SmtpTarpitCount: (default) -> 5`, and returning it to its default — `DELETE
/api/v1/settings/ini/{name}` or COM `DeleteIniSetting` — is recorded with the
action `deleted` and reads `SmtpTarpitCount: 5 -> (default)`. A write of the
value a setting already has changes nothing and records nothing, for either
kind of setting, because a settings page that saves every field it shows would
otherwise bury the one that moved.

### What is never recorded

A password, a key, a token, a secret, a hash, a salt or a passphrase is recorded
as `(changed)` and never as a value. The decision is made by the NAME of the
column or the setting (`AuditTrail::IsSecretName`), matched as a substring, so a
column added later called `accountnewpassword` is covered without anybody
remembering to add it.

Among the `[Settings]` keys that means `BATVSecret`, `SRSSecret`,
`PasswordPepper`, `ServiceAccountPassword`, `GssapiServicePassword`,
`MetricsServerAuthPassword`, `MetricsServerAuthToken`, `OAuth2HmacSecret`,
`OAuth2IntrospectionClientSecret`, `OutboundOAuth2ClientSecret` and
`OutboundOAuth2FixedToken`; the private-key and key-file paths are withheld too.
Fifteen keys whose names only look like secrets are recorded with their values,
by exact name: the seven `PasswordPolicy*` keys, `PreferredHashAlgorithm`,
`MinimumAcceptedHashAlgorithm`, `ProtectStoredSecretsWithDPAPI`,
`OAuth2PublicKeyFile`, `UpdateLogPublicKeyFile`,
`IndexerFullTextMaxTokensPerMessage`, `IndexerFullTextMinTokenLength` and
`OutboundOAuth2TokenUrl`. A change-control reader is
owed "the minimum password length went from 12 to 4", not "it changed". The
list is exact rather than a pattern on purpose: a key added later that merely
resembles one of them is withheld until somebody decides it is not a secret.

An audit trail that copies every password an administrator sets into a table the
Deck renders is worse than no audit trail at all.

The server's own writes are not recorded either. Delivery, retention,
greylisting, the message index and the auto-ban expiry pass all change rows
every minute and none of them is an administrative change; a trail that said
"the mail server changed something" every minute would be one nobody reads.

### Why a COM row has no address

A COM call arrives through DCOM with no peer address this process can see.
Inventing one would be worse than saying nothing, so the field is empty and the
`auditinterface` column carries what can be known: the change was made by
something holding the administrator credential — the Control Panel,
`hmconfig.ps1`, DBSetup, or a script.

### Where it is written from

Three places in the whole server, and a check under `build/` that keeps it that
way:

* **Objects** — `DatabaseConnectionManager::Execute`, the single statement
  chokepoint every write in the server already runs through. The audit row is
  taken there, after the pooled connection has been released, for a statement
  against one of the tables listed above. The name is matched exactly, which is
  why the list is checked against the create scripts: until 15 September 2026
  five of its names (`hm_domainaliases`, `hm_incomingrelays`,
  `hm_whiteaddresses`, `hm_greyliststats`, `hm_blockedsenders`) were not tables
  at all, and domain aliases, incoming relays, both white lists and blocked
  senders were never recorded.
* **Settings** — `PropertySet::SetLong` / `SetBool` / `SetString`, which is the
  only place where the value the setting HAD is still readable. `hm_settings` is
  deliberately not one of the audited tables, so a setting is recorded once,
  with its old value and its new one, and not twice.
* **`[Settings]` keys** — `IniSettingStore::WriteSetting` and
  `::RemoveSetting`, which every route to those keys ends in: COM
  `SetIniSetting` and `DeleteIniSetting`, `PUT` and `DELETE
  /api/v1/settings/ini/{name}`, the settings-group fields kept in that store
  (`tarpit_count` and `tarpit_delay` on `/api/v1/settings/antispam`,
  `user_interface_language` and `rewrite_envelope_from_when_forwarding` on
  `/api/v1/settings`), hmctl, and the Control Panel pages built on them. The stored value is read before it is replaced.
  `PasswordPepper`, the one key kept in the file and never stored, is read from
  the file for the same purpose, so a new pepper — after which no hash made
  before it verifies — is a row like any other key's, reading `(changed)`.
  `hm_inisettings` is not an audited table either, for the same reason as
  `hm_settings`.

Both sit BELOW the interfaces, so COM and REST are covered by construction
rather than by two parallel implementations that can drift.

One kind of change passes neither: **a lockout lifted**. `POST
/api/v1/accounts/{address}/unlock` and `Application.UnlockAccount` clear the
failed-authentication lockout and the POP3 mailbox lock, both held in memory;
`Account.UnlockMailbox` clears the POP3 lock alone (and, since 15 September 2026,
asks `unlock_account` for anybody's mailbox but the caller's own - an account clearing
its own lock is not an administrative change, asks no question, installs no actor and is
deliberately not recorded, since a COM pool thread still carries the last caller's); and `POST
/api/v1/settings/logon-failures/clear` and `Settings.ClearLogonFailureList` delete the
rows of `hm_logon_failures`, which is traffic and not an audited table. `DELETE
/api/v1/spam-traps/hits` and `Utilities.ClearSpamTrapHits` are the same shape:
`hm_spamtraphits` is traffic too, and a clear whose whole effect is to remove the
evidence of what walked into the traps is the last thing that may go unrecorded. Each is
recorded explicitly, through `AuditTrail::RecordEvent`, at the call site that makes it -
object type `account`, the address, action `unlocked`; object type `logon failures` or
`spam trap hits`, action `cleared` - under the same actor and with the same
`[role: ...]` as any other row. A message held for a sandbox's verdict and
released without it, or discarded (`POST /api/v1/sandbox/held/{queue_id}/release`,
`DELETE /api/v1/sandbox/held/{queue_id}`), is recorded the same way - object type
`sandbox hold`, the queue id, action `released` or `discarded`, the sender, the
recipients and the files in the detail - because the hold's rows are traffic and a
release delivers mail the server was waiting to judge ([Sandbox.md](Sandbox.md)).
COM's `DeliveryQueue.Remove` of such a message is the same discard, recorded the
same way, its detail ending `(DeliveryQueue.Remove)`.

A mailbox renamed, or moved to another domain, is the other shape of the same
problem. It writes the account row, an alias, a row per distribution list
membership, a row per queued recipient and a row per quarantined message, and
every one of those is part of ONE thing an administrator did once. Left to the
statement chokepoint the trail would carry four or five entries, none of which
names both addresses. So the writing runs with no actor on the thread - an
`AuditScope` carrying an absent actor, exactly as a script's boundary does - and
the move is recorded once afterwards: object type `account`, the address it took,
action `moved`, and a detail naming the address it left, the messages and folders
it carried, what was re-pointed and whether the old address was kept. A move that
failed and could **not** be put back is recorded as `move failed`, with what was
left where; a move that failed and was put back is not recorded, because nothing
changed. [MailboxMove.md](MailboxMove.md) is the walk.

Who made the change is a thread-local actor, installed at one place per
interface:

* **REST and the Deck** — `RestApiServer::ProcessRequest_`, once, for the length
  of one authorised request.
* **COM and the Control Panel** — `COMAuthentication::InstallActor_`, which every
  authorisation question a COM write asks before it writes goes through:
  `GetIsServerAdmin`, `GetIsDomainAdmin`, and the `Allows` questions a narrow
  role's entry points ask (a help desk's `Application.ResetPassword`). It installs
  the audit trail's actor and RoleManager's together, from the one grant. The
  questions run on the thread that is about to do the writing, which matters:
  hMailServer's COM server is an out-of-process MTA and RPC dispatches each call
  on whichever pool thread is free, so an actor installed when the client
  authenticated would be sitting on a thread that never writes anything. The COM
  methods that write without asking — the settings property setters — carry an
  `AuditScope` of their own.

**An event script** runs in process, on the server thread that fired its event - an
SMTP, IMAP or POP3 worker, a delivery thread. A script that creates
`hMailServer.Application`, authenticates and asks it anything installs that credential
on that thread, as every COM authority question does. So every script runs inside a
boundary (`ScriptActorBoundary` in `ScriptServer.cpp`) that takes both actors off the
thread before the script starts and puts back exactly what was there when it has
finished: what the script changes through COM as an administrator is recorded as that
administrator, and what the server does on the thread afterwards - an auto-ban range
written after a failed logon the script refused, say - is recorded as nobody's, which
means it is not recorded.

`build/check-audit-choke-point.py` runs in GitLab CI's checkers job and fails
it if a caller outside those three appears, if any of the chokepoints goes away,
if `RecordEvent` is called anywhere but the five call sites where a lockout is
lifted or any of those goes away, if a COM method that persists something neither
asks the authorisation question nor opens an `AuditScope`, if a script engine is
started outside the boundary, if anything but `AuditTrail` writes `hm_audit`, if
the list of tables names one the create scripts do not define (or names
`hm_settings` or `hm_inisettings`, which would record every change twice), or if
a table the create scripts define is neither listed nor named in the checker
with the reason it is not recorded. That last rule is what makes a new
configuration table a decision rather than an omission: folder permissions
(`hm_acl`) and remote domain policies were simply never added, and the twelve
syslog setters on COM's `Logging` object wrote with no actor, until it existed.

### Verifying the chain

Each row carries the SHA-256 of the row before it and its own, computed over the
previous hash followed by this row's fields, newline-separated. Editing a row or
deleting one therefore breaks the chain at that point:

    GET /api/v1/audit/verify

    {"intact":false,"rows_checked":41,"first_broken_id":42,
     "reason":"the row's contents do not produce the hash stored with it, so the row has been edited"}

The two failures it distinguishes:

* **edited** — the row's own contents no longer produce the hash stored with it.
* **removed** — the row does not carry the hash of the row before it, so a row
  between them is gone.

This does not make the table tamper-**proof**. Anyone who can write the table
can recompute the whole chain. It makes tampering **detectable** without keeping
a second copy, which is what the question "has this record been edited" actually
needs.

A note on retention: `AuditRetentionDays` deletes rows older than the number of
days it names, and the oldest surviving row then still points back at a row that
is gone. It is left that way on purpose — rewriting its back-hash would mean
recomputing every hash after it, which is exactly the operation the chain exists
to make detectable. `verify` knows the difference: with retention in force, a
trail that does not begin at the empty hash is the administrator's own policy;
with retention off (the default, `0`, keep everything) it is a report of
tampering.

### Reading it

    GET /api/v1/audit?limit=100&offset=0&actor=&object_type=&action=&since=&until=

`actor` is a substring, so `key:` finds every change made with an API key.
`since` and `until` are seconds since the epoch. The answer is newest first,
with `total` for the whole filtered set.

The Deck has an **Audit trail** page that reads it, with the filter, paging and
a *Verify the chain* button. The Control Panel has the same page (Utilities >
Audit trail), over `Utilities.GetAuditTrail`, `Utilities.VerifyAuditTrail` and
`Utilities.RevertAuditedChange`, which answer what the three routes answer.

### Putting a change back

    POST /api/v1/audit/{id}/revert

A setting's row says what the setting had and what it was given (`name: old ->
new`), so the old value can be written back - through the same setter the
change came through, which means the put-back is itself a row in the trail,
made by whoever asked for it and never a quiet edit of history. The answer
names the setting and the value; a `[Settings]` key whose old side is
`(default)` is returned to its default. It needs `manage_settings` and is
refused to every API key, like the rest of the trail.

The trail is honest about what it does not hold, and so is the put-back: a row
that is not a setting's change (an account created, a rule deleted) is refused
with 409, because there is no old value to write; a secret's row is refused,
because the trail records `(changed)` and never the values; a value the trail
cut at 120 characters is refused, because what the trail holds is not the
value; and a setting this version of the server no longer has is refused. Each
refusal says which. The Deck's page has *Put back* on every row that qualifies,
and the Control Panel's page a *Put back* button for the selected row.

**Refused to every API key.** An API key of any scope is refused, and refused
with a 401 rather than a 403 so that it learns nothing from asking. A key that
could read who changed what could watch for its own footprints. The
administrator password reads it, and so does an account whose role holds
`view_audit` — reading it is what the `auditor` role is for — unless that role is
confined to named domains: the trail records every domain's changes, and a
confined role is refused it.

Switching recording off (`audit_trail_enabled`) is itself recorded, while
recording is off. Otherwise the way to change something unobserved would be to
turn the observer off first.

---

Alerts
------

### The shape

* A **condition** is a row in `hm_alertrules`: its name, whether it is on, its
  threshold, what to do (mail, a webhook, or both), how long to wait before
  saying it again, and whether it goes out at once or waits for the digest. The
  name is a free string, so a new condition is a new ROW and never a schema
  change.
* An **event** is a row in `hm_alertevents`: this condition became true (or
  stopped being true) at this time, with this summary.
* **Delivery** is `AlertTask`, once a minute. It is the only thing that sends.

That last point is not tidiness. Whatever noticed the condition writes a row and
returns — so a notification about the delivery queue is never submitted INTO the
delivery queue by the thread that is stuck on it. It is the same deferral the
TLS-RPT reporter makes, for the same reason.

### What ships

| Condition | On by default | What raises it |
|---|---|---|
| `backup.failed` | **yes** | `BackupManager::OnBackupFailed`, and a scheduled backup that could not run. |
| `certificate.expiring` | **yes** | `AlertTask`, which reads the notAfter of every configured certificate once a minute and compares it with the rule's threshold in days (7 by default). |
| `disk.low` | no | `DiskSpace::ReportBand_`, at the warning band and again below the floor. |
| `queue.stalled` | no | `WorkQueueHealthTask`, when every worker thread of a monitored queue has been in the same task past `AsyncQueueStallThreshold`. |
| `autoban.storm` | no | `AlertTask`, when auto-bans in the last hour reach the rule's threshold (25 by default). |
| `minidump.written` | no | `AlertTask`, when a dump newer than the last one recorded appears in the log directory. |
| `complaints.rate.high` | **yes** | `AlertTask`, when a locally hosted domain's complaint rate over the feedback loops' seven-day window reaches the rule's threshold in parts per million (1000 by default - one complaint in a thousand messages, which the large providers describe as too high). A domain that has sent fewer than 100 messages in the window is not judged. With complaint processing off it clears the condition and reads no complaint data at all - though the pass and the clear still read the rule and the newest event, as every seeded rule's do. See [Complaints.md](Complaints.md). |
| `dkim.rotation.stalled` | **yes** | `AlertTask`, from the DKIM key rotation's stored state: a next key whose record has not been seen for the rule's threshold in days (7 by default), or a running rotation (the schedule on, or a next or retiring selector in flight) whose last attempt to move failed. It never looks anything up. See [DkimKeyRotation.md](DkimKeyRotation.md). |
| `account.compromised` | **yes** | `CompromiseDetector::Apply`, from the submission thread, as an occurrence - one event per account per hour (every message after the shape completes scores again for the rest of its hour, and a surge is one alert, not one per message; a lock is always said), with the account, the client address, the score and one sentence per signal, and no cool-down (`0`) so that a second account in the same hour is said at once. In `watch` mode, the default, the summary says nothing was held or locked and the detail says what enforcing would have done. See [CompromisedAccounts.md](CompromisedAccounts.md). |
| `account.password-reset` | **yes** | `AccountRecovery::CompleteReset`, when a reset link sent to an account's confirmed recovery address is used to choose a new password - an occurrence, with the address in the summary and what the reset did in the detail, no cool-down (`0`) and no digest, because a self-service reset is the one change that turns a mailbox somebody could read into a mailbox somebody else can. It is not a failure and not a refusal: it is news. See [AccountRecovery.md](AccountRecovery.md). |
| `reputation.listed` | **yes** | `AlertTask`, from the reputation watch's stored rows: an outbound address of this server that a DNS block list listed at the last check that got an answer. It never looks anything up - the daily check does - and a check that was refused or timed out does not clear it. See [ReputationWatch.md](ReputationWatch.md). |

Four are off because they fire on a healthy server often enough to be noise
until somebody has decided they want them. The ones that are on are the ones
whose whole value is arriving unasked: a backup that failed is not noticed until
the day it is needed, which is the worst possible day to find out, and a DKIM key
rotation waiting on a record nobody published waits for ever. The rotation's
condition is raised only for a domain whose rotation is running, so on a server
that does not rotate keys it never fires. The reputation watch's is raised only
for an address somebody has told the server it sends from, so on a server that has
named none it never fires either.

Nothing is mailed until `AlertRecipient` and `AlertSenderAddress` are set, which
is what makes "on by default" safe in a stock install.

### Nothing may flap

An alerting system that mails every minute is switched off by its administrator
on the first day, and then nothing tells them anything. Four things stop that:

1. A **state** condition that is already raised is not raised again. The disk is
   not "low" once a minute for an hour; it became low once, and it will become
   not-low once. Clearing is recorded too, and only when the raise was sent.
2. An **occurrence** — a backup that failed, a minidump — has no "still true", so
   it is bounded by the rule's **cool-down** instead: another inside the
   cool-down is recorded but held for the digest rather than sent.
3. A **ceiling per hour** across every condition (`AlertMaxPerHour`, 20). Past
   it, everything goes to the digest.
4. The **digest** is the quiet default: one message at `AlertDigestHour` (07:00
   UTC) listing what fired, rather than one message per event. A digest with
   nothing in it is not sent — a message that says "nothing happened" every
   morning is the first thing an administrator filters into a folder they never
   open.

### Webhooks

A rule with the webhook action POSTs one JSON object per event:

```json
{"condition":"disk.low","state":"raised","severity":"high","time":1789000000,
 "time_utc":"2026-09-15 08:26:40Z","summary":"Free disk space is down to 412 MB.",
 "detail":"Mail is still being accepted. It will be refused below 100 MB.",
 "server":"mail.example.com"}
```

with three headers:

    X-hMailServer-Timestamp: 1789000042
    X-hMailServer-Signature: sha256=<hex>
    X-hMailServer-Event: disk.low

#### Verifying the signature

The signature is **HMAC-SHA256 over the timestamp, a full stop, and the raw
request body**, under the rule's shared secret, as lower-case hex. Verify it
against the bytes you received, before parsing them:

```python
import hmac, hashlib, time

def verify(secret, headers, raw_body):
    timestamp = headers["X-hMailServer-Timestamp"]
    signature = headers["X-hMailServer-Signature"]

    # Refuse anything older than five minutes, or the signature can be replayed.
    if abs(time.time() - int(timestamp)) > 300:
        return False

    expected = hmac.new(secret.encode(), (timestamp + ".").encode() + raw_body,
                        hashlib.sha256).hexdigest()

    return hmac.compare_digest("sha256=" + expected, signature)
```

```javascript
const crypto = require("crypto");

function verify(secret, headers, rawBody) {
   const timestamp = headers["x-hmailserver-timestamp"];
   const signature = headers["x-hmailserver-signature"];

   if (Math.abs(Date.now() / 1000 - Number(timestamp)) > 300) return false;

   const expected = "sha256=" + crypto.createHmac("sha256", secret)
      .update(timestamp + ".").update(rawBody).digest("hex");

   return crypto.timingSafeEqual(Buffer.from(expected), Buffer.from(signature));
}
```

Compare with a constant-time comparison, and check the timestamp: a signature
with no freshness check can be replayed for ever.

A rule that calls a webhook is refused without an address and refused without a
secret. An unsigned body arriving at a URL anybody can read out of a
configuration file is an alert anybody can forge.

This is the webhook that belongs to an operational ALERT. The other one - a
subscription that is told what this server's MAIL did, message by message - is
[Webhooks.md](Webhooks.md). It signs the same way and retries on the same
ladder, by calling this code rather than copying it; everything else about it
is different, because a stream of messages has no such thing as the same
delivery twice and no use for a daily digest.

#### Retries and the dead letter

An attempt that does not get a 2xx is retried on the next pass, then after 1, 5,
15 and 60 minutes, up to `AlertWebhookMaxAttempts` (5). After the last one the
event is **dead-lettered**: it stops being retried, the row stays with
everything it said, and HM6543 is recorded once so that an administrator learns
their endpoint is not answering. The events route shows the state:

    GET /api/v1/alerts/events
    ... "webhook":"dead-lettered","webhook_attempts":5 ...

Only https, or plain http to a loopback address — the rule `HttpsClient` applies
to every outbound request in this server.

### The routes

| Route | What it does |
|---|---|
| `GET /api/v1/alerts/rules` | Every condition and its rule. A webhook secret is never returned; `webhook_secret_set` says whether one is configured. |
| `PUT /api/v1/alerts/rules/<condition>` | Change one, or create one for a condition nothing has shipped. A field the body leaves out keeps the value it has. `webhook_secret` is write-only; `""` clears it. |
| `GET /api/v1/alerts/events` | What has fired, newest first, with the webhook delivery state. |
| `POST /api/v1/alerts/test` | Raise a condition on request, through exactly the path a real one takes. `{"condition":"disk.low","summary":"...","occurrence":false}`. |
| `POST /api/v1/alerts/run` | Run one alerting pass now, instead of waiting for the scheduled one. |

All five are refused to every API key, for the reason the audit routes are: the
rules decide where the alerting goes and whether it goes at all. The
administrator password reaches them, and so does an account whose role holds the
permission each needs — `view_configuration` to read the rules and the events,
`manage_settings` to change a rule or raise a test alert, `manage_server` to run
a pass — and reaches every domain.

### The settings

All nine are in `hm_settings` — the store the COM `Settings` object and the
Control Panel's classic pages use — and none of them is an `hMailServer.ini`
key. They are readable and writable at `GET`/`PUT /api/v1/settings`, and the
Deck's Settings page draws them from the server's own OpenAPI document, so they
appear there without the page being changed.

| Setting | Default | What it does |
|---|---|---|
| `audit_trail_enabled` | on | Whether administrative changes are recorded. |
| `audit_retention_days` | 0 | Days of trail to keep. 0 keeps everything. |
| `alerts_enabled` | on | The master switch. With no recipient, nothing is sent. |
| `alert_recipient` | empty | Where alerts and the digest go. |
| `alert_sender_address` | empty | What they are sent from. |
| `alert_digest_enabled` | on | Whether conditions marked for the digest wait for it. |
| `alert_digest_hour` | 7 | The hour, UTC, the digest goes out. |
| `alert_max_per_hour` | 20 | The ceiling before everything goes into the digest. |
| `alert_webhook_max_attempts` | 5 | Attempts before an event is dead-lettered. |

---

What this does not do
---------------------

* **There is no Control Panel page for the alert rules.** The nine settings
  appear on the Deck's Settings page and are writable over COM through
  `Settings`, so the desktop program can reach them; the rules themselves are
  reached over REST, the Deck, or `hmctl`. A Control Panel page is the next
  piece of work on this row.
* **A COM row records no address**, for the reason given above.
* **The audit trail records administrative changes, not reads.** Who looked at
  a mailbox is a different question and a much larger one.
* **One logical change can be more than one row** where saving an object writes
  its children as separate statements — a rule with its criteria, a distribution
  list with its recipients. Each row says what it was.
* **A change nobody made through an interface is not recorded.** An account
  sharing its own folder with IMAP `SETACL` has no administrator behind it, and
  neither has the start-up pass that reconciles `hMailServer.ini` with
  `hm_inisettings` or a `[SettingsOverride]` section edited into the file. The
  override announces itself in the error log at every start instead.
* **A restore is recorded as a request, and not as what it replaces.** One row
  is written the moment a restore is asked for — object type `backup`, the
  archive's path, action `restore started`, and a detail naming which parts are
  being put back and whether the archive is an encrypted one — by whoever asked,
  over COM (`Backup.StartRestore`) or over REST
  (`POST /api/v1/backup/restore`, which since it deletes every domain, account
  and public folder it is replacing is the most destructive thing in the API).
  Nothing the restore then does is recorded: the settings, the `[Settings]`
  keys, the domains and everything under them are replaced on the maintenance
  work queue, and the actor that says who made a change lives on the thread that
  asked the authorisation question, so the thread that does the restoring has
  nobody on it and the recorder writes nothing — the same filter that keeps
  delivery out of the trail. Recording *that* half needs a decision first: a row
  for every value it replaced, which for a large server is thousands, or a
  summary the restore composes as it goes. The check fails if a record call is
  put back into `IniSettingStore::XMLLoad` without that decision, because a call
  there would write rows attributed to nobody.
* **Creating or revoking a REST API key is not recorded.** The keys are kept in
  their own store file beside `hMailServer.ini`, not in a table, and neither
  `POST /api/v1/apikeys` nor `DELETE /api/v1/apikeys/{id}` reaches a recorder.
  Each is only a line in the application log, which is not chained.
* **Releasing or deleting a quarantined message is not recorded.** A release
  re-queues the message and then deletes its row with the same statement a
  deletion uses, so a listed table would record both as the same row naming an
  id that identifies nothing once the row is gone. It needs a record of its own
  in `QuarantineStore`, which is not built.
* **Creating, renaming or deleting a public folder is not recorded.** A public
  folder is a row in `hm_imapfolders`, the table every account's own folders
  are kept in too, and the webmail creates, renames and deletes those under
  the account's own name — so listing the table would record every folder a
  person makes in their own mailbox. Who may use a public folder is recorded:
  its permissions are `hm_acl` rows. Recording the folders themselves needs a
  record of their own that tells a public folder from an account's.
* **An erasure is recorded with the address it erased.** Among the stores
  `Utilities.EraseAddressTraces` sweeps are aliases, distribution-list
  recipients and (with the archive included) the archive index, all of them
  recorded tables, so each deletion there is a row that names the address.
  The trail is append-only by design; a retention period
  (`audit_retention_days`) is the way those rows age out.
* **The sections of `hMailServer.ini` that are not settings are not recorded.**
  Changing the administrator password or its second factor (`[Security]`), a
  folder in `[Directories]`, or the database connection in `[Database]` writes
  the file and nothing else. Those sections are
  how the server is reached before there is a database — the setup wizard writes
  them with none to record into — so recording them needs a decision about that
  case first.
* **A folder permission's row names the folder and the account by id**
  (`aclsharefolderid`, `aclpermissionaccountid`), because those are the columns
  the statement carries.
* **`hmctl` and the PowerShell module have no `audit` or `alerts` verbs yet.**
