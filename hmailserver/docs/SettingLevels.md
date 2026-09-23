# One value, three levels

A handful of settings could always be set per domain - the maximum message size,
the quarantine digest's switch and hour, the external-sender tag, the session
policies, the quota shares - and a handful per account; most were the server's
because that is where they were written. From schema 6088 (23 September 2026) a
setting on the list below may be set **for the server, for a domain and for an
account**, each overriding the one above it, and every place that shows one shows
what is **in effect** and **where it came from**. That is the only honest answer
to "why is this account not doing what the domain says".

## The rule

For an account: **its own value, else its domain's, else the server's.** For a
domain (what its accounts that hold nothing of their own get): its own value,
else the server's. The deepest level that holds a value wins; a level that holds
none takes the level above.

One setting adds a ceiling to that: `max_message_size_kb`. The server's value,
when it is not 0, is a ceiling no level below exceeds - the rule the domain's
value has always had at `MAIL FROM` - so an account's 5000 KB under a server's
1000 KB is 1000 KB, and says it came from the server. A level below may tighten
the server's value and, between themselves, an account may hold more than its
domain; neither loosens the server's.

The rule lives in one place, `Common/Application/EffectiveSetting` (the resolver),
and every read of a setting on the list goes through it. `build/check-setting-levels.py`
holds the code to that (below).

**Nothing changes for a domain or an account that holds no value of its own.**
A domain field's value that has always meant "unlimited" or "off" - the maximum
message size's 0, a quota share's -1, a session switch's "server" - is read as
exactly that: the fields that were optional keep their "take the level above"
value, and the fields that never were (the digest switch and hour, the tag
switches, the impersonation action, plus addressing, the country policy) are
always the domain's, as they always were. Only a value somebody sets at a level
changes anything.

## Which settings have levels

Twenty-seven, every one of them to the account level; none stops at the domain.
`GET /api/v1/settings/levels` lists them with where each level is held, and the
Control Panel's list (`Services/SettingLevels.cs`) is held to the server's table
name for name.

| Name | Server level | Domain level held in | Account level held in | Read site that resolves it | Proved by |
|---|---|---|---|---|---|
| `max_message_size_kb` (ceiling; server administrator's) | property `maxmessagesize` | the domain's `MaxMessageSize` (0 takes the server's) | overlay | `SMTPConnection::GetMaxMessageSize_` at `MAIL FROM` (the envelope sender's domain and account); `IMAPCommandAppend` (the appending account) | `SettingLevels.TheMaximumMessageSizeIsTheAccountsThenTheDomainsUnderTheServersCeiling` |
| `quarantine_digest` | none (a domain is created with it off) | the domain's `QuarantineDigest` | overlay | `QuarantineDigestTask::RunPass_`, `HandleMeQuarantineDigest_` | `SettingLevels.TheDigestGoesAtTheAccountsOwnHourAndItsSiblingsAtTheDomains` (the domain's), `QuarantineDigests` |
| `quarantine_digest_hour` | none (a domain is created with 7) | the domain's `QuarantineDigestHour` | overlay | the same two | `SettingLevels.TheDigestGoesAtTheAccountsOwnHourAndItsSiblingsAtTheDomains` |
| `external_tag_subject`, `external_tag_header` | none (off) | the domain's `ExternalTagSubject`, `ExternalTagHeader` | overlay | `ExternalSenderTagger::Apply` (the recipient's) | `SettingLevels.TheExternalTagIsTheRecipientAccountsOwnWhereItHasOne` |
| `impersonation_action` | none (off) | the domain's `ImpersonationAction` | overlay | `ImpersonationCheck::Apply` (the recipient's) | `ImpersonationProtection` (the domain's) |
| `session_idle_seconds`, `session_lifetime_seconds` | property `SessionIdleSeconds`, `SessionLifetimeSeconds` (0 is the built-in 1800 and 43200) | the domain's session policy (0 takes the server's) | overlay | `SessionPolicies::Resolve_` (browser sessions) | `SettingLevels.TheIdleTimeoutIsTheAccountsOwnWhereItHasOne`; `RestApiDevices` |
| `session_one_per_device`, `session_bind_to_address` | property `SessionOnePerDevice`, `SessionBindToAddress` | the domain's session policy (0 takes the server's, 1 on, 2 off) | overlay | `SessionPolicies::Resolve_` | `RestApiDevices` (the server's and the domain's) |
| `quota_warning_percent` | settings-store key `QuotaWarningPercent` | the domain's field (-1 takes the server's) | overlay | `QuotaPolicy::WarningPercent` | `QuotaWarning` (the server's and the domain's) |
| `quota_grace_percent`, `quota_send_stop_percent` | property `QuotaGracePercent`, `QuotaSendStopPercent` | the domain's fields (-1 takes the server's) | overlay | `QuotaPolicy::GracePercent`, `QuotaPolicy::SendStopPercent` | `QuotaWarning` |
| `plus_addressing_enabled` (server administrator's) | none (off) | the domain's `PlusAddressingEnabled` | overlay | `PlusAddressing::ExtractAccountAddress` (the recipient's) | `SpamTraps`, `DomainServices` (the domain's) |
| `country_policy` | none (empty: no policy) | the domain's country policy | overlay | `CountryBlocking::IsLogonAllowed` | `CountryBlocking` (the domain's) |
| `message_retention_days` (server administrator's) | none (0: no policy) | the domain's `MessageRetentionDays` (0 takes none) | the account's `MessageRetentionDays` (0 takes the domain's; -1 keeps for ever) | `MailboxRetentionTask::Sweep` | `MessageRetentionTests` |
| `recall_days` | property `RecallDays` | overlay | overlay | `MessageRecall::Days`, `MessageRecall::Sweep` (the widest anywhere) | `SettingLevels.TheRecallRouteHonoursTheSendersOwnDays`, `SettingLevels.ComAndRestRoundTripAndRefuseAlike` |
| `recover_deleted_days` (server administrator's) | property `RecoverDeletedDays` | overlay | overlay | `RecoverableStore::Days`, `RecoverableStore::Sweep` | `RestApiRecoverDeleted` (the server's) |
| `sending_max_messages`, `sending_max_recipients` (server administrator's) | `[SendingLimits]` `MaxMessagesPerAccountPerPeriod`, `MaxRecipientsPerAccountPerPeriod` | overlay | overlay, else the address's `[SendingLimitsOverrides]` line | `RateLimiter::GetAccountLimits` | `AccountSendingLimits` (the server's and the file's) |
| `openpgp_portal_enabled` | property `OpenPGPPortalEnabled` | overlay | overlay | the `/api/v1/me/openpgp` routes, the protected-message routes, zero-access | none at a level below the server |
| `zero_access_allowed` | property `ZeroAccessAllowed` | overlay | overlay | `ZeroAccess::Allowed` | none at a level below the server |
| `reader_fetch_accounts_max`, `reader_sender_addresses_max`, `reader_masked_addresses_max`, `reader_offline_days_max` (server administrator's) | properties `ReaderFetchAccountsMax`, `ReaderSenderAddressesMax`, `ReaderMaskedAddressesMax`, `ReaderOfflineDaysMax` | overlay | overlay | the `/api/v1/me` fetch-account, sender-address, masked-address and offline routes | none at a level below the server |
| `directory_user_editable` | property `DirectoryUserEditable` | overlay | overlay | `DirectoryStore::UserEditable` | none at a level below the server |

Counted by where the server level comes from: **16 `hm_settings` properties**
(marked `account` in `build/settings-levels.txt`; the other 240 are `server`),
**1 key of the settings store** (`QuotaWarningPercent`, marked `account` in the
sixth field of `build/ini-settings-baseline.txt`; the other 266 are `server`),
**2 `[SendingLimits]` keys**, which the store does not hold, and **8 with no
server setting at all** - the value a domain is created with is their server
level. Seven of those eight are fields every domain always holds a value of, so
their server level is never what is in effect; the eighth, the retention, is 0 -
no policy - at the server, as it always was.

A setting not on the list is the server's alone: its REST name is refused on the
domain and account routes (400, `unknown field`), its name is refused by COM's
`SettingOverride` with a sentence naming `GET /api/v1/settings/levels`, and the
server's routes answer no `_levels` beside it.

## Where each level is held

* **A field the domain or the account already had** stays where it was, with the
  meaning it always had: its "take the level above" value (0 or -1, or a
  session switch's 0) is the level holding nothing. The domain route and COM's
  domain properties set it as they always did.
* **The overlay**, `hm_settingoverrides` (schema 6088): one row per (scope,
  owner, name) - scope 1 a domain, 2 an account - holding the value as text (a
  number, `1` or `0`, or the text). **No row is "take the level above"**. A name
  is at most thirty characters, lower case, the REST name.

A value is checked against the setting's own rule when it is written - the same
range and sentence the server's own value is refused with, less the value that
means "take the level above", which in the overlay is no row - and again when it
is read: a row somebody put in by hand that the interfaces would refuse is
treated as holding nothing and reported once per name (`HM7311`).

The rows are cached per owner for sixty seconds; every write through the
interfaces invalidates its owner at once. A row written behind the server's back
is seen within the minute.

## The sources

`server`, `domain`, `account`: where the value in effect came from. REST answers it
as `<name>_source`, COM as `EffectiveSettingSource(name)`, the Control Panel and
the Deck as "from the server", "from the domain", "from the account". Under
`max_message_size_kb`'s ceiling a level's value above the server's is answered as
the server's value with `server` as its source.

## The interfaces

**REST.** `GET /api/v1/domains/{domain}` and `GET /api/v1/accounts/{address}` answer,
for every setting on the list, `<name>_effective` (the value in effect, as a number,
a boolean or a string) and `<name>_source`; where the level is held in the overlay
they also answer `<name>`, the level's own value or **null when it takes the level
above**. A `PUT` sets such a `<name>` with a value and makes it inherit with
**null**; a field of its own keeps its contract. `<name>_effective` and
`<name>_source` are answered and never taken (400). The listing `GET /api/v1/domains`
leaves the level fields out: they are one query per setting per domain, and the
domain itself answers them. `GET /api/v1/settings/levels` lists the settings -
`{settings:[{name, type, levels, server, server_key, domain_storage,
account_storage, combine, server_administrators_only, server_value}]}` - and the
server's settings routes answer `<key>_levels` beside a key that has levels
(`GET /api/v1/settings`; `levels` on `GET /api/v1/settings/ini/{name}`; `x-levels`
in the OpenAPI document).

**COM.** `Domain.SettingOverride(name)` and `Account.SettingOverride(name)`, get and
put - the level's own value in its text form, **an empty string** for "take the
level above", written at once and not by `Save`; `EffectiveSetting(name)` and
`EffectiveSettingSource(name)` on both. A put is refused with a sentence for a name
not on the list, a value the setting refuses, a level held in a field of its own
(set that property instead), an object not saved yet, and - to a domain
administrator - a setting that is the server administrator's.
`AntiSpam.SendQuarantineDigestsAtHour(hour)` runs the digest pass as the scheduler
would at that hour, which is how an account's own hour is exercised.

**hmctl.** `hmctl domain set <domain> <name> <value|inherit>` and
`hmctl account set <address> <name> <value|inherit>`; the value is read as `--set`
reads one, and what is printed is what is in effect afterwards and where it came
from.

**The Control Panel** has a **Levels** tab on the domain and the account dialogs:
every setting on the list, ticked where this level holds a value of its own, with
"In effect: ..., from the ..." beside it; **Save levels now** writes what changed
over COM at once and reads every line back. A setting whose level at that dialog is
a field of its own is shown with what is in effect and edited on its own tab.
Against a server older than schema 6088 the tab says the server has no levels.

**The Deck** shows the same card in its domain and account editors, reading the
domain or the account itself (the listing does not carry the levels).

## Who may set what

A limit or a ceiling - `max_message_size_kb`, `plus_addressing_enabled`,
`message_retention_days`, `recover_deleted_days`, the two sending limits and the
four reader maximums (`server_administrators_only` in the list) - is the server
administrator's to set at a domain or an account: an API key restricted to named
domains is refused it with 403, and a domain administrator over COM with a
sentence, so nobody lifts their own limits. The rest a domain-restricted key may
set for its own domains and their accounts, and never another domain's (the
routes' own 403). `GET /api/v1/settings/levels` is server-wide and refused to a
domain-restricted key.

## A saved level applies at once

A resolved value is read when it is used, never latched: a level saved over REST,
COM, hmctl, the Control Panel or the Deck is in effect for the next message,
session or request, at every level - the same as a setting marked `now` in
[SettingsApply.md](SettingsApply.md).

## The cascade and the backup

Deleting an account deletes its rows, and deleting a domain deletes the domain's;
a row left behind would become the next owner's with the same id. A delete that
cannot remove them is reported - `HM7310` for a domain, the account's existing
orphan report (`HM6109`, "its own setting values") for an account.

The XML backup carries the rows under the domain and under each account
(`<SettingOverrides><SettingOverride Name=".." Value=".."/></SettingOverrides>`),
and a restore writes them back under the new ids through the same rule a write
goes through; a row the build refuses is left out and reported (`HM7312`), and the
restore carries on.

## What is thinner than the row asked

* **Not every server setting has levels.** Twenty-seven do; the list says which,
  and the rest of the ~500 are the server's. Among those that stay the server's:
  attachment blocking (it acts on the whole message before it is split between
  its recipients), web push, passkeys and account recovery.
* **The dedicated columns stay.** A domain's maximum message size, digest, tag,
  session, quota, plus-addressing and country fields, and an account's retention,
  are still held where they were, with their 0 or -1; only the new levels are in
  the overlay. The domain route's null for such a field leaves it unchanged, as it
  did.
* **The maximum message size is the sending account's**, at `MAIL FROM` and at
  IMAP `APPEND`, because the domain's has always been the sending domain's; the
  EHLO `SIZE` advertisement and the webmail's own send stay the server's.
* **The session policies are the browser sessions'** (the webmail, the portal, the
  Deck), as they always were; IMAP, POP3 and SMTP sessions have none.
* **`plus_addressing_character` stays the domain's**: there is no server value for
  it to override.
* **`country_policy` cannot be set to empty over COM** - an empty string is "take
  the level above" there; REST's `""` sets an empty policy of the level's own.
* **The sending limits' `[SendingLimitsOverrides]` line** is still read, as the
  account level, after an overlay row.
* **Five settings' behaviour is driven by a regression test at the account
  level** (the maximum message size, the digest hour, the external tag, the idle
  timeout, the recall window); the other twenty-two go through the same resolver
  and are pinned to it by the checker, and their existing tests prove the server
  and domain levels.
* **The fixture is Windows-only**: it uses COM members the Linux suite's REST
  shim does not answer.

## The checker

`build/check-setting-levels.py` reads the resolver's table and holds:

* every name to at most thirty characters, lower case, once;
* the marks - a server property or store key whose setting has levels is marked
  `account` (or `domain`) in `build/settings-levels.txt` or the baseline's sixth
  field, and nothing else is;
* every direct read of one level's value (a domain column's getter, the account's
  retention, the server's getter) to the functions allowed to read one level -
  where it is defined, stored or written, the interfaces that report a level's own
  value, the resolver, and a few reads that are the server's by design - by file
  and function;
* every pinned read site to calling the resolver for its name, and every name to
  having a pinned read site;
* the Control Panel's list to the table's.

It proves its own negative control on every run, in a temporary copy: a new
function reading a domain's digest hour straight, and the plus-addressing read site
with its resolver call swapped for the domain's column, are both refused.
`check-ini-coverage.py` and `check-settings-rows.py` refuse an unmarked line.

## Tests

`RegressionTests/Domains/SettingLevels.cs` (REST on 10040), each with its negative
control - a sibling account of the same domain with nothing of its own:

* the maximum message size at 1000/500/200 KB: `MAIL FROM` the account with
  `SIZE` 300 KB refused with its own 200, the sibling's taken and refused at
  600 KB with the domain's 500; an account value above the server's capped by it;
  cleared, the domain's again - with `_effective` and `_source` at every step;
* the digest hour: the domain at 7, the account at 9 - at nine only the account's
  digest goes, at seven only the sibling's though something new is held for the
  account, and cleared the account's goes at seven;
* the external tag: the domain on and the account off (untagged, the sibling
  tagged, cleared tagged), then the domain off and the account's header on;
* the idle timeout: the domain 600, the account 1 - the account's browser session
  refused after two idle seconds, the sibling's still answered, and cleared the
  account's next session announcing 600 and outliving the wait;
* the recall window at three levels: server 1, domain 30, account 2 against
  messages two and a bit days old;
* the cascade (a probe insert the unique index refuses while the row is there),
  the XML backup and restore, COM and REST round trips and refusals, a
  domain-restricted key, and the checker's own run.

`ControlPanel.Tests` (`SettingLevelsTests`) hold the words to the levels; the Deck
harness checks the card in both editors, a refused value sent nowhere, a value and
a null sent, and the answer read back; `check-cli.py` holds `hmctl domain set` and
`account set` to their bodies.
