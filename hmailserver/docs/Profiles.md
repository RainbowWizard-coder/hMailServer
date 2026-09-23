# Profiles, so a new server starts in the right shape

The shipped defaults suit one kind of deployment, and a new administrator was
left to find, one page at a time, the thirty settings that make a server the
kind it is. A **profile** is those thirty, named: a small office server, a
server behind a filtering gateway, an outbound relay, a high-volume sender. It
says exactly which settings it sets, to what and why, shows the difference from
this server before anything changes, saves what it replaces, and can be put
back.

Four ship. They are documents in the tree, not rows in a database:
`hmailserver/source/Server/Common/Application/Profiles/<name>.json`, embedded in
the server by `build/generate-profiles.py`, which also writes the tables at the
end of this page - so the tables are the data the server applies.

## A diff first

`GET /api/v1/settings/profiles/{name}` (COM `Settings.GetProfile`, the Deck's
checklist, the Control Panel's **Setting profiles** page, `hmctl settings profile NAME`)
answers every entry beside the value this server holds now:

| Field | What it says |
|---|---|
| `store`, `name` | `ini` for a `[Settings]` key, `database` for an `hm_settings` property |
| `value`, `current`, `changes` | what the profile sets, what is in force now - the stored value, or the shipped default where none is stored - and whether applying would move it |
| `why` | one clause: why a server of this kind wants it |
| `route`, `applies` | the REST settings route and key the value is written through, and when a change reaches the running server - `now`, `reinitialize` or `restart` |
| `levels` | `limit`, `gate` or `preference` for a setting a domain and an account may hold their own value of (below) |
| `caution`, `confirm` | `closes_listener`, `trusts_addresses` or `turns_filter_off`, and whether applying would do that here |
| `environment` | the variable, where the server's environment sets the setting |
| `refused` | the sentence the setting's own check refuses the value with |

`GET /api/v1/settings/profiles` lists the four with how many entries each would
change here, and which profile was last applied.

## Then a snapshot, then the writes

`POST /api/v1/settings/profiles/{name}/apply` (COM `Settings.ApplyProfile(name,
confirmed)`, `hmailserver --apply-profile NAME`, the Deck, the Control Panel,
`hmctl settings apply-profile NAME`):

1. **Every entry is checked first**, and the apply is all or nothing: an entry
   its setting would refuse - the REST row's own check, in its own sentence, or
   the settings store's for a `[Settings]` key - refuses the whole profile (409),
   and nothing is written.
2. **A listener or a trust is never taken silently.** An entry that closes a
   listener, trusts an address range or turns a filter off asks for an explicit
   confirmation wherever it would change something: without it the apply is
   refused (409, `confirmation: true`, and `confirmation_needed` names each such
   entry with the value it has and the value it would get). The confirmation is
   `{"confirm": true}` on the REST body, the `Confirmed` argument over COM,
   `--confirm` on the command line, a box the Deck and the Control Panel ask to
   be ticked, and `HM_PROFILE_CONFIRM=yes` for the container. Where such an
   entry would change nothing - the listener already closed, the filter already
   off - nothing is asked.
3. **The way back is saved before anything is written**: the value each changing
   entry has now, in the `hm_settings` row `ProfileSnapshot`. One snapshot, the
   latest: a second apply is refused (409) until the first has been reverted or
   its way back discarded, because it would replace the only record of the
   settings before the first.
4. **Each entry is written through the setter its REST settings row runs** - the
   setter `PUT /api/v1/settings/...` runs, which is the one `hmailserver
   --apply-environment` lends (`RestApiSettingsRows`): the value canonicalised
   and derived as every interface does it. A `[Settings]` key no row keeps is
   written through `IniFileSettings::WriteSettingsValue`, the settings store's
   locked write every save takes. Nothing is written straight into the property
   set: a property no REST row writes cannot be in a profile (the server refuses
   such an entry, and the regression test asks it about every shipped one).
5. **A setting the server's environment manages is skipped**, with the sentence
   that names its variable (`docs/EnvironmentConfiguration.md`): the environment
   wins at every start, and a value written here would be undone at the next.
6. Each value is read back. The answer says what each entry did - `set`,
   `unchanged`, `skipped` or `failed`, with the value before and after - and how
   many of the values set wait for a reinitialise (**Apply now**) or a restart.

An apply is one event on the audit trail - object `profile`, action `applied`,
with the counts - beside the row the settings chokepoint writes for every setting
it changed, under whoever asked: the administrator over REST or COM, or
`hmailserver --apply-profile` for the command line. The application log names each
setting changed, with its old and new value.

## The way back

`POST /api/v1/settings/profiles/revert` (COM `Settings.RevertProfile`) writes back
the value each setting had before the last apply, through the same setters - a
`[Settings]` key byte for byte, and one that was not stored before removed, so
that it returns to its default - and clears the snapshot. **A setting that no
longer holds what the profile wrote is left as it is**: an administrator changed
it since, and the way back does not undo their change. The answer says which were
put back and which were left, and why. Reverting can turn a filter back off that
the profile turned on - the Deck and the Control Panel say so before they revert.

`POST /api/v1/settings/profiles/discard` (COM `Settings.DiscardProfileSnapshot`)
forgets the way back and keeps the settings, so that another profile can be
applied.

`Settings.AppliedProfile` over COM, and `applied` in the list, say which profile
is in effect. The record is the `hm_settings` row `ProfileApplied`:
`{profile, applied, state, changed}`, state `applied`, `reverted` or `kept`.

Neither record is a setting: no default, in no create script, in no
changed-from-stock list, not nameable from the environment, and left out of the
XML backup, which carries a configuration and not this server's history with a
profile - a way back restored onto values it was not taken of would put back
values that were never there. Both are made, empty, at every start.

## What a profile never touches

`build/generate-profiles.py` refuses, before anything is written:

* a **port** - a listener's or a daemon's;
* a **directory, a file or a path**: a path is the installation's;
* a **database value**: the `[Database]` section is the bootstrap and not a
  setting, and the `[Settings]` keys about the database connection are refused
  as well;
* a **password**, a secret, a token, a pepper, a passphrase or a private key;
* a record the server keeps for itself (the environment's two, the profile's two,
  the sample domain's id, the VAPID public key);
* a name that is not a setting, or a value the setting would refuse - a whole
  number where the server reads one, text where it is text, and a number outside
  the range a REST row's check writes as `v.number < A || v.number > B`. The rest
  of a row's check is held by the server itself, when a diff is asked for.

Nor does a profile set what is not a setting at all: a listener's TLS (a port's
own), an IP range and what it may do, a DNS block list, a domain's own settings -
DKIM, its quarantine digest, its external tag - or the `[SendingLimits]` keys. A
profile that wants one of those says so in its **steps**, which every surface
shows under the diff.

## Levels, and limits

A profile sets the **server** level only (`docs/SettingLevels.md`). Where an entry
is the server level of a setting a domain and an account may hold their own value
of, the diff says which kind it is. For a **limit** - `office` sets the maximum
message size, the session idle and lifetime and the recover-deleted window - the
server's value is the ceiling every domain and account below it is held to:
raising it lets a domain's own value up to it, and lowering it caps a tenant's own
value when it is read.

## At setup

* **The Deck's checklist** has a first item, **A profile for this server**: action
  needed on a young server - fewer than ten accounts, the sample domain's not
  counted - that no profile has been applied to; done once one has been, with which
  and when; not needed on a server in service, where a profile is not something to
  suggest in a list of things left to do. Below the items the four profiles are
  offered, each with its diff, apply and the way back.
* **The installer** offers the four on its last page before it installs, for a new
  installation only - one that has no database yet - with one sentence each and
  "none" chosen until another is; the sentence names the listener or the filter a
  profile changes, and choosing it is the confirmation. After the database is
  created it runs `hMailServer.exe --apply-profile NAME --once --confirm`, before
  the service starts. A silent installation takes `/profile=NAME`, which is the
  same explicit choice. A profile that cannot be applied is said, and the
  installation still succeeds with the shipped settings.
* **The container** applies `HM_PROFILE` once (`--once`): see
  `platform/packaging/README.md`.
* **The Control Panel** has a **Setting profiles** page under Maintenance, beside
  the backup: the profile in effect, each profile's diff, apply - with a box to tick
  where an entry asks for the confirmation - put back and discard, over the COM
  members above. Its checklist on the External setup page has the same first item.
* **`hmctl`**: `settings profiles`, `settings profile NAME`, `settings
  apply-profile NAME [--confirm]`, `settings revert-profile`, `settings
  discard-profile`.

## The four profiles

<!-- The four profiles: generated by build/generate-profiles.py from Common/Application/Profiles/*.json. Edit the JSON, not this section. -->

### office - A small office server

One server that receives an office's mail from the Internet, holds its mailboxes and sends what its people write. Spam filtering, greylisting and the auto-ban are on at the shipped thresholds, spam a domain asks to hold is held in the quarantine, passwords are held to a policy and checked against known breaches, a signed-in person sends only as themselves, and a browser session lasts a working day.

33 entries.

| Store | Setting | Value | Why | Asks for a confirmation |
|---|---|---|---|---|
| database | `usespf` | `1` | a forged sender is the commonest spam, and SPF is the cheapest test of it |  |
| database | `usemxchecks` | `1` | a sender domain that cannot receive mail cannot be answered, so its mail is scored |  |
| database | `ASDKIMVerificationEnabled` | `1` | a verified signature is what DMARC, the reports and the brand logos build on |  |
| database | `ASDMARCEnabled` | `1` | the sender domain's own policy says what to do with a forgery of it |  |
| database | `spammarkthreshold` | `5` | the shipped score at which a message is spam, written down so the diff shows it |  |
| database | `spamdeletethreshold` | `20` | the shipped score at which a message is refused, well clear of the mark |  |
| database | `antispamaddheaderspam` | `1` | a mail client's own junk filter can act on the X-hMailServer-Spam header |  |
| database | `antispamprependsubject` | `1` | people see at a glance that a message was judged spam |  |
| database | `usegreylisting` | `1` | most spam software does not retry, and a real server retries within the hour |  |
| database | `BypassGreylistingOnSPFSuccess` | `1` | a sender whose SPF passes is not delayed by greylisting |  |
| database | `BypassGreylistingOnMailFromMX` | `1` | mail from the sender domain's own MX host is not delayed either |  |
| ini | `QuarantineEnabled` | `1` | spam a domain asks to hold is kept for people to release rather than delivered or lost |  |
| ini | `AuthenticationResultsEnabled` | `1` | the server's verdicts are written into each message, where a mail client and the brand logos read them |  |
| database | `AutoBanOnLogonFailureEnabled` | `1` | an address that guesses passwords is shut out |  |
| database | `MaxInvalidLogonAttempts` | `5` | five failures before a ban, so that one mistyped password is not one |  |
| database | `LogonAttemptsWithinMinutes` | `30` | the shipped window the failures are counted in |  |
| database | `AutoBanMinutes` | `60` | an hour's ban stops a guessing run and lets a person back in after lunch |  |
| ini | `AccountLockoutThreshold` | `10` | an account guessed at from many addresses is locked for a while as well |  |
| database | `authallowplaintext` | `0` | a password is never taken over a connection without TLS |  |
| ini | `SmtpAuthenticatedSenderCheck` | `1` | a signed-in person sends as their own address, an alias of it, or a mailbox that granted them Send-As |  |
| ini | `MaxSubmissionsPerIPPerMinute` | `60` | a stolen password cannot pour a thousand messages a minute through one address |  |
| ini | `PasswordPolicyMinimumLength` | `10` | a short password is the one guessed first |  |
| ini | `PasswordPolicyRejectCommon` | `1` | the passwords every guessing list starts with are refused |  |
| database | `BreachCheckEnabled` | `1` | a password already in a published breach is refused, and only five characters of its SHA-1 leave the server |  |
| database | `disconnectinvalidclients` | `1` | a client that sends nonsense is disconnected rather than answered for ever |  |
| database | `maximumincorrectcommands` | `20` | twenty wrong commands is a probe, not a confused mail client |  |
| database | `maxmessagesize` | `25600` | 25 MB, what most providers accept, so a message sent here can be sent on |  |
| database | `SessionIdleSeconds` | `1800` | a webmail or Deck session left open on a desk ends after half an hour unused |  |
| database | `SessionLifetimeSeconds` | `36000` | no browser session outlives a ten-hour working day |  |
| database | `CreateDefaultSpecialUseFolders` | `1` | a new account has Drafts, Sent, Junk and Trash before its first client connects |  |
| database | `enableattachmentblocking` | `1` | an attachment on the blocked list, such as a program, is taken off before anybody opens it |  |
| database | `RecoverDeletedDays` | `14` | a message somebody deleted by mistake can be put back for two weeks |  |
| database | `ReputationWatchEnabled` | `1` | the server says when its own address appears on a block list |  |

What it cannot do for you:

* Give the submission port (587) STARTTLS with a certificate, or add port 465 with TLS: a port's security is the port's, not a setting, and the checklist's submission item says which port still takes a password in the clear.
* Switch each domain's quarantine digest and external-sender tag on in the domain's own settings: both are a domain's choice, and the server has no value of them to set.
* Set quarantine_digest_base_url to the address the webmail answers on, or the digest has no links to send.

### gateway - Behind a filtering gateway

Inbound mail arrives from a filtering gateway - a hosted service or an appliance in front of this server - that has already judged it against the real sender. The tests that judge the connecting address are switched off, because that address is now the gateway's and every sender would fail them; the tests that judge the message itself stay on, and people who sign in here directly are still held to the auto-ban and the sender check.

19 entries.

| Store | Setting | Value | Why | Asks for a confirmation |
|---|---|---|---|---|
| database | `usespf` | `0` | the connecting address is the gateway's, so every sender's SPF would fail here; the gateway checked it against the real one | turns a filter off |
| database | `usegreylisting` | `0` | the gateway has filtered and retries on its own schedule, so greylisting it only delays mail | turns a filter off |
| database | `ascheckhostinhelo` | `0` | the HELO name is the gateway's, not the sender's | turns a filter off |
| database | `ascheckptr` | `0` | the PTR record is the gateway's, not the sender's | turns a filter off |
| database | `ASDMARCEnabled` | `0` | DMARC's SPF half fails behind a gateway; the gateway enforces DMARC against the real sender | turns a filter off |
| database | `ASDKIMVerificationEnabled` | `1` | a signature still verifies through a gateway that leaves the message as it was |  |
| database | `ASArcFilteringEnabled` | `1` | a gateway that rewrites a message seals what it saw, and the seal of a sealer named in arc_trusted_sealers is trusted | trusts an address range |
| ini | `SMTPXClientEnabled` | `1` | the gateway can pass the real client's address with XCLIENT, honoured only from the addresses in SMTPXClientTrustedIPs | trusts an address range |
| ini | `ReceivedSpfHeaderEnabled` | `0` | a Received-SPF header here would record the gateway's address as every sender's |  |
| ini | `MaxSubmissionsPerIPPerMinute` | `0` | every inbound message comes from the gateway's few addresses, and a per-address ceiling would throttle all of it |  |
| ini | `SmtpTarpitCount` | `0` | the gateway delivers messages with many recipients, which the recipient tarpit would slow |  |
| ini | `RejectFullMailboxAtRcpt` | `1` | the gateway is told at RCPT that a mailbox is full and returns the message itself, rather than this server bouncing to a sender it cannot check |  |
| database | `usemxchecks` | `1` | the sender's domain is part of the message, not of the connection, and is still checked |  |
| database | `antispamaddheaderreason` | `1` | the tests that still run say which of them scored, so a message the gateway passed can be traced |  |
| ini | `QuarantineEnabled` | `1` | what the content tests still judge spam is held for release rather than lost |  |
| database | `AutoBanOnLogonFailureEnabled` | `1` | people sign in here directly, not through the gateway, and a guessing address is still shut out |  |
| database | `authallowplaintext` | `0` | a password is never taken over a connection without TLS |  |
| ini | `SmtpAuthenticatedSenderCheck` | `1` | a signed-in person sends as their own address, an alias of it, or a mailbox that granted them Send-As |  |
| database | `enableattachmentblocking` | `1` | an attachment on the blocked list is taken off here too, whatever the gateway let through |  |

What it cannot do for you:

* Put the gateway's addresses in SMTPXClientTrustedIPs, so that the gateway can pass the real client's address with XCLIENT: until then XCLIENT is honoured from nobody.
* Add the gateway's addresses as an IP range that does not run the spam tests, and switch off each DNS block list on the anti-spam pages: a block list is a list entry, not a setting, and asking it about the gateway's address tells nothing.
* Name the ARC sealer the gateway signs as in arc_trusted_sealers, so that a message the gateway rewrote is judged by what the gateway saw.
* On the listener the gateway delivers to, leave TLS optional unless the gateway offers it: a port's security is the port's, not a setting.
* A backup-MX callout to the primary is a remote domain policy (callout), which a profile does not make.

### relay - An outbound relay

A server that sends mail for applications and other servers and holds no mailbox anybody reads. IMAP and POP3 are closed, an address that bounces hard is suppressed and the sender told at once, deliveries back off from a destination that asks them to and never hold more than ten sessions open to one, and every delivery attempt is traced and kept for three months, because a relay is asked about what it sent long after it sent it.

25 entries.

| Store | Setting | Value | Why | Asks for a confirmation |
|---|---|---|---|---|
| database | `protocolimap` | `0` | nobody reads mail on a relay, and a listener nobody uses is one more to attack | closes a listener |
| database | `protocolpop3` | `0` | the same for POP3 | closes a listener |
| database | `authallowplaintext` | `0` | a sender's password is never taken over a connection without TLS |  |
| database | `AutoBanOnLogonFailureEnabled` | `1` | an address that guesses a relay's passwords is shut out |  |
| database | `CreateDefaultSpecialUseFolders` | `0` | an account here is a sender's credential, not a mailbox |  |
| database | `DirectoryEnabled` | `0` | the accounts are applications, not a directory of people to list to each other |  |
| database | `BounceProcessingEnabled` | `1` | an address that bounces hard is not written to again |  |
| database | `BounceHardBounceThreshold` | `1` | one hard bounce is enough: the receiver has said the address does not exist |  |
| database | `BounceSuppressionAction` | `0` | refuse: a submission to a suppressed address is refused at RCPT with the reason, so the application learns it at once |  |
| database | `BounceSuppressionExpiryDays` | `180` | a suppressed address gets another chance after six months |  |
| database | `BounceReadDeliveryReports` | `1` | the reports receivers send back are matched to what was sent |  |
| database | `OutboundBackoffMaxMinutes` | `60` | the longest a destination that keeps asking to slow down is left alone, as shipped |  |
| database | `OutboundBackoffMinutes` | `2` | a destination that answers a slow-down reply is left alone for two minutes, doubling, as shipped |  |
| database | `MaxOutboundConnsPerDestination` | `10` | no destination is held more than ten sessions at once, which is what large providers tolerate |  |
| database | `maxdelivertythreads` | `25` | a relay's work is delivery, so more of it runs at once |  |
| database | `smtpnoofretries` | `12` | a destination that is down for an afternoon still gets its mail |  |
| database | `smtpminutesbetweenretries` | `60` | an hour between attempts, as shipped, so twelve of them span half a day |  |
| ini | `QuickRetries` | `2` | a delivery that failed for a moment is tried again within minutes before the hourly schedule |  |
| database | `SmtpDeliveryConnectionSecurity` | `2` | STARTTLS whenever the destination offers it, as shipped |  |
| database | `VerifyRemoteSslCertificate` | `1` | a destination's certificate is checked, so that a policy that requires TLS means it |  |
| ini | `MtaStsEnabled` | `1` | a recipient domain that publishes an MTA-STS policy gets TLS to a verified host, or no delivery |  |
| ini | `DaneEnforcementEnabled` | `1` | a recipient domain that publishes TLSA records is held to them |  |
| ini | `MessageTraceEnabled` | `1` | every delivery attempt is recorded, which is what answers whether a message went |  |
| ini | `MessageTraceRetentionDays` | `90` | three months of it rather than the thirty days shipped, because a relay is asked about old sends |  |
| database | `ReputationWatchEnabled` | `1` | a relay whose address is on a block list stops getting its mail through, and you are told |  |

What it cannot do for you:

* Decide who may relay: that is an IP range's question - require authentication on the range the senders connect from, or name their addresses - and a profile does not answer it.
* Switch DKIM signing and its key rotation on for each sending domain: both are the domain's, and the server has no value of them to set.
* Set the sending limits per account in [SendingLimits] if the senders are to be held to a volume: those keys are the settings file's, not the store's.

### sender - A high-volume sender

The relay's shape, for a server that sends a great deal - newsletters, notifications, receipts - and is judged by the large providers on its bounce and complaint rates. Everything the relay sets, and more deliveries at once, a ceiling per destination and per sending domain, the feedback loops read so that one complaint suppresses the address, a hard bounce suppressed for good, and the reports receivers send about this server kept for a year.

Everything `relay` sets, with the entries below replacing or adding to it - 33 entries in all.

| Store | Setting | Value | Why | Asks for a confirmation |
|---|---|---|---|---|
| database | `protocolimap` | `0` | nobody reads mail on a relay, and a listener nobody uses is one more to attack (from `relay`) | closes a listener |
| database | `protocolpop3` | `0` | the same for POP3 (from `relay`) | closes a listener |
| database | `authallowplaintext` | `0` | a sender's password is never taken over a connection without TLS (from `relay`) |  |
| database | `AutoBanOnLogonFailureEnabled` | `1` | an address that guesses a relay's passwords is shut out (from `relay`) |  |
| database | `CreateDefaultSpecialUseFolders` | `0` | an account here is a sender's credential, not a mailbox (from `relay`) |  |
| database | `DirectoryEnabled` | `0` | the accounts are applications, not a directory of people to list to each other (from `relay`) |  |
| database | `BounceProcessingEnabled` | `1` | an address that bounces hard is not written to again (from `relay`) |  |
| database | `BounceHardBounceThreshold` | `1` | one hard bounce is enough: the receiver has said the address does not exist (from `relay`) |  |
| database | `BounceSuppressionAction` | `0` | refuse: a submission to a suppressed address is refused at RCPT with the reason, so the application learns it at once (from `relay`) |  |
| database | `BounceSuppressionExpiryDays` | `0` | a hard-bounced address stays suppressed until somebody removes it, because a sender judged on its bounce rate never writes to it again |  |
| database | `BounceReadDeliveryReports` | `1` | the reports receivers send back are matched to what was sent (from `relay`) |  |
| database | `OutboundBackoffMaxMinutes` | `60` | the longest a destination that keeps asking to slow down is left alone, as shipped (from `relay`) |  |
| database | `OutboundBackoffMinutes` | `2` | a destination that answers a slow-down reply is left alone for two minutes, doubling, as shipped (from `relay`) |  |
| database | `MaxOutboundConnsPerDestination` | `20` | twenty sessions to one destination at most, spreading a send without looking like a flood |  |
| database | `maxdelivertythreads` | `50` | a high-volume sender's queue drains in minutes rather than hours |  |
| database | `smtpnoofretries` | `12` | a destination that is down for an afternoon still gets its mail (from `relay`) |  |
| database | `smtpminutesbetweenretries` | `60` | an hour between attempts, as shipped, so twelve of them span half a day (from `relay`) |  |
| ini | `QuickRetries` | `2` | a delivery that failed for a moment is tried again within minutes before the hourly schedule (from `relay`) |  |
| database | `SmtpDeliveryConnectionSecurity` | `2` | STARTTLS whenever the destination offers it, as shipped (from `relay`) |  |
| database | `VerifyRemoteSslCertificate` | `1` | a destination's certificate is checked, so that a policy that requires TLS means it (from `relay`) |  |
| ini | `MtaStsEnabled` | `1` | a recipient domain that publishes an MTA-STS policy gets TLS to a verified host, or no delivery (from `relay`) |  |
| ini | `DaneEnforcementEnabled` | `1` | a recipient domain that publishes TLSA records is held to them (from `relay`) |  |
| ini | `MessageTraceEnabled` | `1` | every delivery attempt is recorded, which is what answers whether a message went (from `relay`) |  |
| ini | `MessageTraceRetentionDays` | `180` | six months of delivery attempts, because a sender is asked about a campaign long after it went |  |
| database | `ReputationWatchEnabled` | `1` | a relay whose address is on a block list stops getting its mail through, and you are told (from `relay`) |  |
| ini | `MaxOutboundPerDestinationPerMinute` | `120` | at most two messages a second to one destination, so that a burst cannot undo an address's warm-up |  |
| database | `MaxOutboundConnsPerTenant` | `25` | one sending domain cannot take every delivery and hold up the others' mail |  |
| database | `ComplaintProcessingEnabled` | `1` | the abuse reports the providers send are read and counted per domain and per account |  |
| database | `ComplaintSuppressionThreshold` | `1` | one complaint suppresses the address: the reader pressed the spam button and did not want the mail |  |
| database | `ComplaintRetentionDays` | `180` | six months of complaints, as shipped, which is longer than any rate is measured over |  |
| database | `DomainReportRetentionDays` | `365` | the DMARC and TLS reports receivers send about this server are kept a year, as shipped |  |
| ini | `MtaStsHostingEnabled` | `1` | the web services serve each domain's MTA-STS policy, so receivers can hold this server's mail to TLS as well |  |
| database | `AlertsEnabled` | `1` | a listed address, a filling queue or a rising bounce rate is mailed to whoever is named to receive alerts |  |

What it cannot do for you:

* Warm a new sending address up: start with a few hundred messages a day to each large provider and double it every few days while the bounce and complaint rates stay low. No setting does this; the per-destination ceiling below only keeps a burst from undoing it.
* Register a feedback loop with each large provider, and name the mailbox the reports arrive at in the complaint settings (mailbox): until then no complaint is ever read.
* Set TlsRptFromAddress to a mailbox here, so that the TLS reports this server owes the domains that ask for them are sent, and publish an MTA-STS policy for each sending domain.
* Decide who may relay, and switch DKIM signing on per domain, as for the relay.

<!-- End of the generated section. -->

## Adding a profile

Write `Common/Application/Profiles/<name>.json` - `name` (its file's name, lower
case), `title`, `summary`, `steps`, optionally `includes` (another profile whose
entries come first, an entry of the same setting replacing the included one in
its place), and `entries`, each `{store, name, value, why}` with `caution` where
the entry closes a listener, trusts an address range or turns a filter off - and
run `python build/generate-profiles.py`. It refuses what a profile may not set,
and a caution that is missing or not earned, with the file and the entry; it
writes `ProfilesData.cpp` and the tables above; `--check` holds both on every
pull request. A profile is not added at run time: four, shipped, reviewed.

## What is not there

* **Four profiles, not user-defined ones.** A profile is a reviewed document in
  the tree; there is no route that makes one.
* **One snapshot.** The way back is the last apply's only; a second apply waits
  for it to be reverted or discarded.
* **A value only you know is a step, not an entry**: the gateway's addresses, the
  ARC sealer, the complaint mailbox, the TLS-RPT sender. A profile carries fixed
  values; the steps say what is left to fill in.
* **No parameter on a profile**, and no profile for a domain or an account.
* The installer page and the container's `HM_PROFILE` have been written and
  reviewed, and not run: the installer was not built for this change, and no
  container was started.

## Tests

`RegressionTests/API/RestApiProfiles.cs` (REST on 10030; every apply reverted in
the teardown, and the settings checked equal before and after each test): the
list; the diff for `office` with every entry's current value and `changes`; an
apply that changes exactly those entries, recorded as one audit event with the
count, and a second apply refused; a revert that puts every value back - the
changed-from-stock document compared before and after - and clears the snapshot,
and a revert with nothing to revert refused; a setting an administrator changed
after the apply left by the revert, the rest put back; an entry that turns a filter
off refused without the confirmation and applied with it, and asking for nothing
where the filter is already off; the relay's two listeners marked, its unconfirmed
apply refused with IMAP still on, and a confirmation sent as a string refused; a
setting the environment
manages skipped with its variable named; every entry of every shipped profile
answered by the server as written through a REST row, and none refused; the
generator's refusal of a port, a directory and an unknown name, as negative
controls on a temporary copy; the COM round trip; `--apply-profile --once`
applied once and not twice, the relay without `--confirm` exiting 3, and the
command line's apply recorded under its own name; the checklist's first item; and
the changed-from-stock document naming the profile as the source of a value it set.
The Deck harness (`build/deck-script-test.js`) checks the checklist item, the diff,
the environment's mark, the confirmation box, the apply with the confirmation, the
way back and the discard; `build/check-cli.py` the five `hmctl` verbs and the
refusal for want of `--confirm`.
