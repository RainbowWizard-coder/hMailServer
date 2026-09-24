# One value, three levels

A handful of settings could always be set per domain - the maximum message size,
the quarantine digest's switch and hour, the external-sender tag, the session
policies, the quota shares - and a handful per account; most were the server's
because that is where they were written. From schema 6088 (23 September 2026) a
setting on the list below may be set **for the server, for a domain and for an
account**, and every place that shows one shows what is **in effect** and
**where it came from**. That is the only honest answer to "why is this account
not doing what the domain says".

## The rule

For an account: **its own value, else its domain's, else the server's.** For a
domain (what its accounts that hold nothing of their own get): its own value,
else the server's. How a level below combines with the level above it depends on
what kind of setting it is, and every setting is exactly one kind:

* **A limit** protects the server or the other tenants: the message size, the
  recall window, the sending limits, the session lifetimes. **A level below may
  tighten the level above and never loosen it.** A looser value is refused when
  it is written - with a sentence that names the level above and what it holds -
  and a value the level above has since tightened past is capped by it when it
  is read, answering the level above's value and that level as its source.
  Anybody who administers the level may tighten it: a domain administrator, and a
  key restricted to the domain.
* **A gate** is a capability the server grants: zero-access mailboxes, the
  OpenPGP portal, a directory people edit themselves, plus addressing. **Only the
  server administrator sets it**, at any level and either way; the deepest level
  that holds a value wins, so the server administrator may open a gate for one
  domain or shut it for one account.
* **A preference** is the tenant's own: the digest's hour, the external tag, the
  retention of its own mail. **The deepest level that holds a value wins**, and
  the domain administrator sets it.

The rule lives in one place, `Common/Application/EffectiveSetting` (the resolver),
whose table gives each name its kind, and every read of a setting on the list
goes through it. `build/check-setting-levels.py` holds the code to that (below).

**Nothing changes for a domain or an account that holds no value of its own.**
A domain field's value that has always meant "unlimited" or "off" - the maximum
message size's 0, a quota share's -1, a session switch's "server" - is read as
exactly that: the fields that were optional keep their "take the level above"
value, and the fields that never were (the digest switch and hour, the tag
switches, the impersonation action, plus addressing, the country policy) are
always the domain's, as they always were. Two older contracts are kept for the
same reason:

* **A limit's domain field that was always free of the server's value stays
  free of it.** The domain's own session seconds and switches and its quota grace
  and send-stop shares could always go either way from the server's, and still
  can; the ceiling binds the account below them. The maximum message size's
  domain field was always capped by the server's value, and still is.
* **The sending limits' `[SendingLimitsOverrides]` line** for an address is that
  address's **server** level - the server administrator's own exception, above
  the `[SendingLimits]` value or below it, as it always was - which the domain's
  and the account's own values may tighten and not loosen.

## Which settings have levels, and what kind each is

Twenty-seven, every one of them to the account level. `GET /api/v1/settings/levels`
lists them with their kind and where each level is held, and the Control Panel's
list (`Services/SettingLevels.cs`) is held to the server's table name for name and
kind for kind.

| Name | Kind | Why | Server level | Domain level held in | Account level held in | Read site that resolves it | Proved by |
|---|---|---|---|---|---|---|---|
| `max_message_size_kb` | limit (0 is no limit) | what the server stores and carries, and every recipient's mailbox | property `maxmessagesize` | the domain's `MaxMessageSize` (0 takes the server's; capped by it, as always) | overlay | `SMTPConnection::GetMaxMessageSize_` at `MAIL FROM` (the envelope sender's domain and account, and the signed-in account's own value as well); `IMAPCommandAppend` (the session's account) | behaviour: `SettingLevels.TheMaximumMessageSizeIsALimitHeldAtEveryLevel` (SMTP), `SettingLevels.AnImapAppendIsHeldToTheAccountsOwnLimit` (IMAP) |
| `quarantine_digest` | preference | a message to the tenant's own people | none (a domain is created with it off) | the domain's `QuarantineDigest` | overlay | `QuarantineDigestTask::RunPass_`, `HandleMeQuarantineDigest_` | behaviour: `SettingLevels.AnAccountsOwnDigestSwitchSendsItUnderADomainThatHasItOff` |
| `quarantine_digest_hour` | preference | when the tenant's people are written to | none (a domain is created with 7) | the domain's `QuarantineDigestHour` | overlay | the same two | behaviour: `SettingLevels.TheDigestGoesAtTheAccountsOwnHourAndItsSiblingsAtTheDomains` |
| `external_tag_subject`, `external_tag_header` | preference | how the tenant's own readers see mail from outside | none (off) | the domain's `ExternalTagSubject`, `ExternalTagHeader` | overlay | `ExternalSenderTagger::Apply` (the recipient's) | behaviour: `SettingLevels.TheExternalTagIsTheRecipientAccountsOwnWhereItHasOne` |
| `impersonation_action` | preference | the tenant's own check of its own inbound mail | none (off) | the domain's `ImpersonationAction` | overlay | `ImpersonationCheck::Apply` (the recipient's) | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `ImpersonationProtection` |
| `session_idle_seconds`, `session_lifetime_seconds` | limit | how long a signed-in session the server issued stays usable | property `SessionIdleSeconds`, `SessionLifetimeSeconds` (0 is the built-in 1800 and 43200) | the domain's session policy (0 takes the server's; free of it, as always) | overlay | `SessionPolicies::Resolve_` (browser sessions) | behaviour (idle): `SettingLevels.TheIdleTimeoutIsTheAccountsOwnWhereItHasOne`; answer (lifetime): `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `RestApiDevices` |
| `session_one_per_device`, `session_bind_to_address` | limit (on is tighter) | how a session the server issued is held to one device and one address | property `SessionOnePerDevice`, `SessionBindToAddress` | the domain's session policy (0 takes the server's, 1 on, 2 off; free of it, as always) | overlay | `SessionPolicies::Resolve_` | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's bind switch in behaviour: `RestApiDevices` (its one-per-device switch is exercised there only by a refused value) |
| `quota_warning_percent` | preference | when the owner is told; it refuses nothing | settings-store key `QuotaWarningPercent` | the domain's field (-1 takes the server's) | overlay | `QuotaPolicy::WarningPercent` | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `QuotaWarning` |
| `quota_grace_percent` | limit | how far past its quota a mailbox still grows on the server's disk | property `QuotaGracePercent` | the domain's field (-1 takes the server's; free of it, as always) | overlay | `QuotaPolicy::GracePercent` | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `QuotaWarning` |
| `quota_send_stop_percent` | limit (0 is never) | the brake on an account that has filled its mailbox | property `QuotaSendStopPercent` | the domain's field (-1 takes the server's; free of it, as always) | overlay | `QuotaPolicy::SendStopPercent` | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `QuotaWarning` |
| `plus_addressing_enabled` | gate | it opens every `local+tag` address of every mailbox it reaches | none (off) | the domain's `PlusAddressingEnabled` | overlay | `PlusAddressing::ExtractAccountAddress` (the recipient's) | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `SpamTraps`, `DomainServices` |
| `country_policy` | preference | which countries may sign in to the tenant's own mailboxes | none (empty: no policy) | the domain's country policy | overlay | `CountryBlocking::IsLogonAllowed` | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; the domain's behaviour: `CountryBlocking` |
| `message_retention_days` | preference | how long the tenant's own mail is kept; it deletes nobody else's | none (0: no policy) | the domain's `MessageRetentionDays` (0 takes none) | the account's `MessageRetentionDays` (0 takes the domain's; -1 keeps for ever) | `MailboxRetentionTask::Sweep` | answer: `SettingLevels.TheFieldHeldLevelsAnswerWhatIsInEffect`; behaviour: `MessageRetentionTests` |
| `recall_days` | limit | a recall deletes unread copies in every hosted domain, not only the sender's | property `RecallDays` | overlay | overlay | `MessageRecall::Days`, `MessageRecall::Sweep` (the server's window, the widest anywhere) | behaviour: `SettingLevels.TheRecallWindowIsALimitAtEveryLevel` |
| `recover_deleted_days` | limit | deleted copies kept on the server's disk | property `RecoverDeletedDays` | overlay | overlay | `RecoverableStore::Days`, `RecoverableStore::Sweep` | behaviour (domain): `SettingLevels.TheOverlayLevelsReachTheirReadSites` |
| `sending_max_messages`, `sending_max_recipients` | limit (0 is no limit) | the brake on an account that sends too much, which every other tenant's reputation shares | `[SendingLimits]`, or the address's `[SendingLimitsOverrides]` line | overlay | overlay | `RateLimiter::GetAccountLimits`, consumed by an SMTP submission, the webmail's send and the send API (the last two since 24 September 2026, `429` with SMTP's sentence; a draft the webmail scheduled is not counted when its minute comes, which is a gap) | answer: `SettingLevels.TheOverlayLevelsReachTheirReadSites`; the server's and the file's behaviour: `AccountSendingLimits`; the account's `sending_max_messages` through the webmail's send and the send API: `RestApiSend.TheWebmailsSendConsumesTheAccountsSendingQuota`, `RestApiSend.TheDayCeilingAndTheAccountsQuotaCountOnlyAMessageThatGoes` |
| `openpgp_portal_enabled` | gate | a feature the server offers in the webmail | property `OpenPGPPortalEnabled` | overlay | overlay | the `/api/v1/me/openpgp` routes, the protected-message routes, zero-access | behaviour (domain and account): `SettingLevels.TheOverlayLevelsReachTheirReadSites` |
| `zero_access_allowed` | gate | a mailbox the server can no longer read, export or search | property `ZeroAccessAllowed` | overlay | overlay | `ZeroAccess::Allowed` | behaviour (domain): `SettingLevels.TheOverlayLevelsReachTheirReadSites` |
| `reader_fetch_accounts_max`, `reader_sender_addresses_max`, `reader_masked_addresses_max`, `reader_offline_days_max` | limit | resources each account takes of the server | properties `ReaderFetchAccountsMax`, `ReaderSenderAddressesMax`, `ReaderMaskedAddressesMax`, `ReaderOfflineDaysMax` | overlay | overlay | the `/api/v1/me` fetch-account, sender-address, masked-address and offline routes | behaviour (domain; the masked maximum at the account too): `SettingLevels.TheOverlayLevelsReachTheirReadSites` |
| `directory_user_editable` | gate | people editing what the directory shows of them | property `DirectoryUserEditable` | overlay | overlay | `DirectoryStore::UserEditable` | behaviour (domain): `SettingLevels.TheOverlayLevelsReachTheirReadSites` |

**Counted by kind:** 15 limits, 4 gates, 8 preferences. **By where the server
level comes from:** 16 `hm_settings` properties (marked `account` in
`build/settings-levels.txt`; the other 240 are `server`), 1 key of the settings
store (`QuotaWarningPercent`, marked `account` in the sixth field of
`build/ini-settings-baseline.txt`; the other 266 are `server`), 2 `[SendingLimits]`
keys, which the store does not hold, and 8 with no server setting at all - the
value a domain is created with is their server level. Seven of those eight are
fields every domain always holds a value of, so their server level is never what
is in effect; the eighth, the retention, is 0 - no policy - at the server, as it
always was. **By what proves them:** 15 names are driven by a regression test
through the read site that acts on them (behaviour); 12 are proved by what the
account and its domain answer (`_effective` and `_source`), because what acts on
them - SMTP, the retention sweep, a sign-in from a country - is driven at the
domain level by the fixture named beside them and was not driven again per
account.

**A server setting not on the list is set for the whole server only**: its REST
name is refused on the domain and account routes (400, `unknown field`), its name
is refused by COM's `SettingOverride` with a sentence naming
`GET /api/v1/settings/levels`, and the server's routes answer no `_levels` beside
it. The domain's and the account's own fields that have no server level at all -
the signature, the relay, DKIM, the automatic reply, the account and alias
limits, restricted delivery - stay where they are and have no levels.

## Where each level is held

* **A field the domain or the account already had** stays where it was, with the
  meaning and the authority it always had: its "take the level above" value (0
  or -1, or a session switch's 0) is the level holding nothing. The domain route
  and COM's domain properties set it as they always did.
* **The overlay**, `hm_settingoverrides` (schema 6088): one row per (scope,
  owner, name) - scope 1 a domain, 2 an account - holding the value as text (a
  number, `1` or `0`, or the text). **No row is "take the level above"**. A name
  is at most thirty characters, lower case, the REST name. A second index, on the
  name, serves the reads of one setting across every owner.

A value is checked against the setting's own rule when it is written - the same
range and sentence the server's own value is refused with, less the value that
means "take the level above", which in the overlay is no row - and, for a limit,
against the level above; and again when it is read: a row somebody put in by hand
that the interfaces would refuse is treated as holding nothing and reported
(`HM7311`), once per name until the caches are emptied.

An owner's rows are cached for sixty seconds, and so is every row of one name
(what the digest pass, the OpenPGP portal's module - served before anybody signs
in - plus addressing and the recover-deleted sweep ask); every write through the
interfaces drops them at once, and so does emptying the caches (REST's
`POST /api/v1/settings/cache/clear`, COM's `Cache.Clear`). A row written behind the
server's back is seen within the minute, or at once after the caches are emptied.

## The sources

`server`, `domain`, `account`: where the value in effect came from. REST answers it
as `<name>_source`, COM as `EffectiveSettingSource(name)`, the Control Panel and
the Deck as "from the server", "from the domain", "from the account". A limit's
value that the level above has since tightened past answers the level above's
value, with that level as its source.

## The interfaces

**REST.** `GET /api/v1/domains/{domain}` and `GET /api/v1/accounts/{address}` answer,
for every setting on the list, `<name>_effective` (the value in effect, as a number,
a boolean or a string) and `<name>_source`; where the level is held in the overlay
they also answer `<name>`, the level's own value or **null when it takes the level
above**. A `PUT` sets such a `<name>` with a value **of the setting's own JSON
type** - a whole number, `true` or `false`, a string; `true` is never a 1 KB limit
and `"30"` never thirty days - and makes it inherit with **null**; a member that
says what the level already holds changes nothing and needs nobody's authority; a
field of its own keeps its contract. `<name>_effective` and `<name>_source` are
answered and **ignored when sent back**, so a domain read and sent back as it is,
is taken. The listing `GET /api/v1/domains` leaves the level fields out: they are
one query per setting per domain, and the domain itself answers them.
`GET /api/v1/settings/levels` lists the settings -
`{settings:[{name, type, levels, server, server_key, domain_storage,
account_storage, class, tighter, combine, server_administrators_only,
server_per_address, domain_column_capped, server_value}]}` - and the server's
settings routes answer `<key>_levels` beside a key that has levels
(`GET /api/v1/settings`; `levels` on `GET /api/v1/settings/ini/{name}`; `x-levels`
in the OpenAPI document), which is answered and never taken, and which
`hmctl config export` and `Export-HmConfiguration` leave out of a document.

**COM.** `Domain.SettingOverride(name)` and `Account.SettingOverride(name)`, get and
put - the level's own value in its text form, **an empty string** for "take the
level above", written at once and not by `Save`; `EffectiveSetting(name)` and
`EffectiveSettingSource(name)` on both. A put is refused with a sentence for a name
not on the list, a level held in a field of its own (set that property instead),
a gate to anybody but the server administrator, a value the setting refuses, a
limit looser than the level above, and an object not saved yet (a set and a clear
alike). Because it is written at once, a put asks what `Save` asks: an account
that is a server administrator's is refused to anybody but a server administrator,
an account whose stored authority the credential does not take in is refused, and
a domain the credential's grant does not reach is refused.
`AntiSpam.SendQuarantineDigestsAtHour(hour)` runs the digest pass as the scheduler
would at that hour, which is how an account's own hour is exercised.

**hmctl.** `hmctl domain set <domain> <name> <value|inherit>` and
`hmctl account set <address> <name> <value|inherit>`; the value is read as `--set`
reads one. For a level held in a field of its own, `inherit` is sent as that
field's own "take the level above" (0 or -1), a domain's session switch as its 1 or
2, and `inherit` is refused with a sentence for a field that has none (the
digest's switch and hour, the tags, the impersonation action, plus addressing, the
country policy). What is printed is what is in effect afterwards and where it came
from.

**The Control Panel** has a **Levels** tab on the domain and the account dialogs:
every setting on the list, ticked where this level holds a value of its own, with
"In effect: ..., from the ..." beside it; **Save levels now** writes what changed
over COM at once and reads every line back, and shows the server's sentence when
it refuses one. A setting whose level at that dialog is a field of its own is shown
with what is in effect and edited on its own tab. Against a server older than
schema 6088 the tab says the server has no levels.

**The Deck** shows the same card in its domain and account editors, reading the
domain or the account itself (the listing does not carry the levels); saving the
levels redraws the card alone, so nothing typed elsewhere in the editor is lost.

## Who may set what

For the levels the overlay holds, the kind decides:

* **A limit** anybody who administers the level may tighten - the server
  administrator, a domain administrator over COM, a key restricted to the
  domain - and nobody may loosen past the level above (400, the sentence naming
  the level above and what it holds).
* **A gate** is the server administrator's: a key restricted to named domains is
  refused it with 403, and a domain administrator over COM with a sentence.
* **A preference** is the domain administrator's, and a domain-restricted key's.

**The fields the domain and the account already had keep the authority they
always had**, whatever the kind of the setting: the domain's maximum message size
and retention are the server administrator's (COM refuses a domain administrator,
and the domain route refuses a restricted key); plus addressing is the server
administrator's (the domain route refuses a restricted key, and COM from this row
refuses a domain administrator a change - it did not before - while taking the
same value written back); the domain's digest, hour, tags, impersonation action,
quota shares, country policy and session policy are the domain administrator's and
a restricted key's; and the account's own retention is the domain administrator's
and a restricted key's - so an account's `-1`, keep for ever, can switch the
domain's policy off for that account, as it always could.

A key restricted to named domains never reaches another domain's values (the
routes' own 403), and `GET /api/v1/settings/levels` is server-wide and refused to
it.

## A saved level applies at once

A resolved value is read when it is used, never latched: a level saved over REST,
COM, hmctl, the Control Panel or the Deck is in effect for the next message,
session or request, at every level - the same as a setting marked `now` in
[SettingsApply.md](SettingsApply.md). A browser session's policy is resolved
afresh at each of its requests, before the sessions' lock is taken, so a cache
reading the database never holds up anybody else's request.

## The cascade, the backup and a mailbox move

Deleting an account deletes its rows, and deleting a domain deletes the domain's;
a row left behind would become the next owner's with the same id. A delete that
cannot remove them is reported - `HM7310` for a domain, the account's existing
orphan report (`HM6109`, "its own setting values") for an account.

The XML backup carries the rows under the domain and under each account
(`<SettingOverrides><SettingOverride Name=".." Value=".."/></SettingOverrides>`),
and a restore writes them back under the new ids through the same rule a write
goes through, less the ceiling - the level above may not be restored yet, and the
ceiling caps the value when it is read; a row the build refuses is left out and
reported (`HM7312`), and the restore carries on.

A mailbox moved to another domain keeps its account id, and so its own values:
they follow it, including a limit set by the server administrator that the new
domain's administrator cannot loosen, and the new domain's level applies from then
on to everything the account holds nothing of - and caps the account's own limits.

## What is thinner than the row asked

* **Not every server setting has levels.** Twenty-seven do; the list says which,
  and the rest of the ~500 are the server's. Among those that stay the server's:
  attachment blocking (it acts on the whole message before it is split between
  its recipients), web push, passkeys and account recovery.
* **The dedicated fields stay, with their authority.** A domain's maximum message
  size, digest, tag, session, quota, plus-addressing, country and retention
  fields, and an account's retention, are still held where they were, with their
  0 or -1, and set by whoever could set them before (above). The domain route's
  null for such a field leaves it unchanged, as it did; hmctl sends the field's
  own "take the level above" instead.
* **Four limits' domain fields go either way from the server's.** The session
  seconds and switches and the quota grace and send-stop shares could always be
  set looser than the server's at the domain, and still can: tightening them into
  ceilings would change what existing domains resolve to. The ceiling binds the
  account below them.
* **The maximum message size is the sending account's**, at `MAIL FROM` - the
  envelope sender's domain's and account's, and the signed-in account's own value
  as well - and at IMAP `APPEND`, because the domain's has always been the sending
  domain's; the EHLO `SIZE` advertisement, the webmail's own send and the send
  API, which shares the webmail's check ([SendApi.md](SendApi.md)), stay the
  server's.
* **The session policies are the browser sessions'** (the webmail, the portal, the
  Deck), as they always were; IMAP, POP3 and SMTP sessions have none.
* **`plus_addressing_character` stays the domain's**: there is no server value for
  it to override.
* **`country_policy` cannot be set to empty over COM** - an empty string is "take
  the level above" there; REST's `""` sets an empty policy of the level's own.
* **Twelve names are proved by the answer, not by behaviour** (the table says
  which): the account level of the sending limits (whose `sending_max_messages`
  the send API's fixture also drives, through the webmail's send and the send
  API), impersonation, the country
  policy, the quota shares, the session lifetime and switches, plus addressing and
  the retention goes through the same resolver, pinned to it by the checker, and
  the fixtures beside them drive the server and domain levels.
* **The fixture is Windows-only**: it uses COM members the Linux suite's REST
  shim does not answer.

## The checker

`build/check-setting-levels.py` reads the resolver's table and holds:

* every name to at most thirty characters, lower case, once, and to exactly one
  kind - a limit with the way it tightens, a gate or a preference with none - and
  a domain field marked as capped by the server only where it is a limit's;
* the marks - a server property or store key whose setting has levels is marked
  `account` (or `domain`) in `build/settings-levels.txt` or the baseline's sixth
  field, and nothing else is;
* every direct read of one level's value (a domain column's getter, the account's
  retention, the server's getter) to the functions allowed to read one level -
  where it is defined, stored or written, the interfaces that report a level's own
  value, the resolver, and a few reads that are the server's by design - by file
  and function, in every branch of an `#if`;
* every pinned read site to calling the resolver for its name, and every name to
  having a pinned read site;
* the Control Panel's list to the table's, kind included.

It proves its own negative controls on every run, in temporary copies: a new
function reading a domain's digest hour straight, the plus-addressing read site
with its resolver call swapped for the domain's column, the same kind of reader in
the `#else` branch of an `#if`, and a gate given a way to tighten, are all
refused. `check-ini-coverage.py` and `check-settings-rows.py` refuse an unmarked
line.

## Tests

`RegressionTests/Domains/SettingLevels.cs` (REST on 10040). Behaviour, each with its
negative control - a sibling account of the same domain with nothing of its own,
or the step before the value:

* the maximum message size at 1000/500/200 KB: `MAIL FROM` the account with `SIZE`
  300 KB refused with its own 200, the sibling's taken and refused at 600 KB with
  the domain's 500; signed in as the account, the sibling's address does not lift
  its 200; 5000 refused as looser than the domain's; the domain tightened to 100
  capping the account's 200; cleared, the domain's again;
* the same limit at IMAP `APPEND`: `APPENDLIMIT` and a 300 KB literal refused for
  the account and asked for from its sibling;
* the digest hour and the digest switch, at hours the clock does not reach during
  the test;
* the external tag, both ways round;
* the idle timeout, 1 second of the account's own under the domain's 600, and
  1200 refused;
* the recall window at server 30, domain 3 and account 2 against messages two and
  a bit days old, and a domain and an account value past the level above refused;
* the ten settings whose domain level is new in the overlay, through the
  `/api/v1/me` routes that act on them, before and after the domain's values and
  an account's own.

Answer-proved: the column-held levels an account holds its own of, and the sending
limits. And the overlay's own rules: the cascade (a probe insert the unique index
refuses while the row is there), the backup and restore, COM and REST round trips
and refusals (types, a limit loosened, a name that is the server's alone, a field
held level over COM, an unsaved object, the answered fields sent back, a domain
read and sent back), a domain administrator over COM refused a server
administrator's mailbox, a gate and a looser limit while tightening an ordinary
account's, a domain-restricted key tightening and refused past the server's, a row
written by hand that the interfaces would refuse reported as `HM7311`, and the
checker's own run.

`ControlPanel.Tests` (`SettingLevelsTests`) hold the words to the levels; the Deck
harness checks the card in both editors, a refused value sent nowhere, a value and
a null sent, the answer read back, and something typed elsewhere in the editor kept
when the levels are saved; `check-cli.py` holds `hmctl domain set` and `account set`
to their bodies - a field's own inherit, a domain session switch, a refused
inherit - and an exported document to carrying no `_levels` mark.
