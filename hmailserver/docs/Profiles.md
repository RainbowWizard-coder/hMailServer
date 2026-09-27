# Profiles, so a new server starts in the right shape

The shipped defaults suit one kind of deployment, and a new administrator was
left to find, one page at a time, the thirty settings that make a server the
kind it is. A **profile** is those settings, named: a small office server, a
server behind a filtering gateway, an outbound relay, a high-volume sender. It
says exactly which settings it sets, to what and why, shows the difference from
this server before anything changes, saves what it replaces, and can be put
back.

Four ship. They are documents in the tree, not rows in a database:
`hmailserver/source/Server/Common/Application/Profiles/<name>.json`, embedded in
the server by `build/generate-profiles.py`, which also writes the tables at the
end of this page - so the tables are the data the server applies. Every reason in
them is written from what the setting's reader does, and where the reader does
less than a reason would suggest - a switch that only scores, a policy enforced
only where a password is chosen - the reason says so.

## A diff first

`GET /api/v1/settings/profiles/{name}` (COM `Settings.GetProfile`, the Deck's
checklist, the Control Panel's **Setting profiles** page, `hmctl settings profile NAME`)
answers every entry beside the value this server holds now:

| Field | What it says |
|---|---|
| `store`, `name` | `ini` for a `[Settings]` key, `database` for an `hm_settings` property |
| `value`, `current`, `changes` | what the profile sets; the value the server holds - the stored one, or the shipped default where none is stored, and for a `[Settings]` key the one `[SettingsOverride]` forces - and whether applying would move it. For a limit whose 0 means the server's own ceiling (the session times), `current` is the stored 0, not the ceiling |
| `why` | one sentence: why a server of this kind wants it, as the setting's reader does it |
| `route`, `applies` | the REST settings route and key the value is written through, and when a change reaches the running server - `now`, `reinitialize` or `restart` |
| `levels` | `limit`, `gate` or `preference` for a setting a domain and an account may hold their own value of (below) |
| `caution`, `confirm`, `note` | `closes_listener`, `trusts_addresses`, `turns_filter_off` or `purges` (below), whether applying would ask for the confirmation here, and for a window it would shorten the note that a revert does not bring back what the sweep deletes |
| `environment` | the variable, where the server's environment sets the setting |
| `refused` | the sentence the setting's own check refuses the value with |

Beside the entries the diff answers `confirmation_needed` - each entry that asks
for the confirmation, as `{store, name, caution, current, value}`, the shape a
refused apply lists them in too - and `diff_token`, a token of this diff: the
profile, and for every entry the value in force, whether it would change, whether
it asks for the confirmation, and what would skip or refuse it.

`GET /api/v1/settings/profiles` lists the four with how many entries each would
change here and how many of those ask for a confirmation (`confirmations`), and
the record of the last choice.

## Then a snapshot, then the writes

`POST /api/v1/settings/profiles/{name}/apply` (COM `Settings.ApplyProfile(name,
confirmed, diffToken)`, `hmailserver --apply-profile NAME`, the Deck, the Control
Panel, `hmctl settings apply-profile NAME`):

1. **The diff is the one shown.** The body's `diff_token` names the diff the
   caller read; when the server would now make a different one - a setting moved,
   the environment took one, a confirmation became needed or stopped being - the
   apply is refused (409, `stale: true`) and nothing is written. Look at the diff
   again. The Deck, the Control Panel and `hmctl` send the token they drew; a
   script that sends none applies what the server would do now.
2. **Every entry is checked first**, and the apply is all or nothing: an entry
   its setting would refuse - the REST row's own check, in its own sentence, or
   the settings store's for a `[Settings]` key - refuses the whole profile (409),
   and nothing is written.
3. **A listener, a trust or data is never taken silently.** An entry that closes a
   listener, trusts an address range, turns a filter or a check off or weakens it
   (a throttle or a limit whose 0 is off set to 0, AUTH PLAIN offered, a spam
   threshold raised past the value this version ships), or shortens how long
   something is kept, asks for an explicit confirmation wherever it would change
   something: without it the apply is refused (409, `confirmation: true`, and
   `confirmation_needed` names each such entry with the value it has and the value
   it would get). The confirmation is `{"confirm": true, "diff_token": ...}` on the
   REST body - a `confirm` without the token is refused (400), because it would
   confirm whatever the server would do, which nobody has seen - the `Confirmed`
   argument with the token over COM, `--confirm` on the command line (where no diff
   is shown: the installer and the container, with the server stopped), a box the
   Deck and the Control Panel ask to be ticked, `--confirm TOKEN` for `hmctl`, and
   `HM_PROFILE_CONFIRM=yes` for the container. Where such an entry would change
   nothing - the listener already closed, the filter already off - nothing is
   asked.
4. **A window of days asks only where it shortens.** Every retention, expiry and
   the recover-deleted window carries the caution `purges`, and the server asks for
   the confirmation where the new value keeps less than the one in force - reading
   0 as the setting reads it: for ever for the trace, the domain reports, the
   complaints and the bounce suppressions, nothing for the recover-deleted window.
   The next sweep deletes what the shorter window no longer keeps, and **a revert
   puts the number back, not what was deleted**; the diff's `note` and the
   confirmation box both say so. A longer window deletes nothing and asks nothing.
5. **The way back is saved before anything is written**: the value each changing
   entry has now, in the `hm_settings` row `ProfileSnapshot`. When the database
   does not take it, nothing is applied (500, HM7301 in the error log). One
   snapshot, the latest: a second apply is refused (409) until the first has been
   reverted or its way back discarded, because it would replace the only record of
   the settings before the first. An apply whose way back would not fit the row's
   4000 characters is refused before anything is written; the store read that says
   which `[Settings]` keys are stored must succeed too, or nothing is applied.
6. **Each entry is written through the setter its REST settings row runs** - the
   setter `PUT /api/v1/settings/...` runs, which is the one `hmailserver
   --apply-environment` lends (`RestApiSettingsRows`): the value canonicalised
   and derived as every interface does it. A `[Settings]` key no row keeps is
   written through `IniFileSettings::WriteSettingsValue`, the settings store's
   locked write every save takes. Nothing is written straight into the property
   set: a property no REST row writes cannot be in a profile (the server refuses
   such an entry).
7. **A setting the server's environment manages is skipped**, with the sentence
   that names its variable (`docs/EnvironmentConfiguration.md`): the environment
   wins at every start, and a value written here would be undone at the next.
8. Each value is read back. The answer says what each entry did - `set`,
   `unchanged`, `skipped` or `failed`, with the value before and after, and for a
   value set that is not the profile's once read back (`[SettingsOverride]` forces
   the key, or the setter canonicalises it) a `reason` saying which value is in
   force - and how many of the values set wait for a reinitialise (**Apply now**)
   or a restart. The Deck and the Control Panel show those counts.
9. The record `ProfileApplied` is written after the writes: `applied`, or `failed`
   when an entry could not be written - which `--once` does not take for applied,
   so a container that stopped on it tries again at its next start and is told of
   the way back it holds. The checklist then asks for action (`profile_failed`).

An apply is one event on the audit trail - object `profile`, action `applied`,
with the counts - beside the row the settings chokepoint writes for every setting
it changed, under whoever asked: the administrator over REST or COM, or
`hmailserver --apply-profile` for the command line. The application log names each
setting changed, with its old and new value. Over COM, an apply or a revert that
failed part way raises an error naming what each setting did.

## The way back

`POST /api/v1/settings/profiles/revert` (COM `Settings.RevertProfile`,
`hmailserver --revert-profile`) writes back the value each setting had before the
last apply, through the same setters - a `[Settings]` key byte for byte, and one
that was not stored before removed, so that it returns to its default - and clears
the snapshot. **A setting that no longer holds what the profile wrote is left as it
is**: an administrator changed it since, and the way back does not undo their
change. The answer says which were put back and which were left, and why, and how
many of the values put back wait for a reinitialise or a restart - a listener the
profile closed opens again only at the next restart. Reverting can turn a filter
back off that the profile turned on - the Deck and the Control Panel say so before
they revert. A window the profile shortened gets its number back; what the sweep
deleted under the shorter one stays deleted.

`POST /api/v1/settings/profiles/discard` (COM `Settings.DiscardProfileSnapshot`)
forgets the way back and keeps the settings, so that another profile can be
applied. A way back that cannot be read cannot be put back (HM7300 in the error
log, and the revert refused with 500); the list answers it as
`{readable: false}`, and the Deck and the Control Panel offer only its discard.

`POST /api/v1/settings/profiles/decline` (COM `Settings.DeclineProfiles`,
`hmailserver --apply-profile none`, the installer's **None**, `HM_PROFILE=none`,
the Deck's and the Control Panel's **Keep the shipped settings**) records that the
server keeps the settings this version ships with: no setting is written, the
checklist's profile item is not needed (`profile_declined`), and any profile can
still be applied afterwards. It is refused while a way back is held or a profile is
in effect.

`Settings.AppliedProfile` over COM, and `applied` in the list, say which profile
is in effect. The record is the `hm_settings` row `ProfileApplied`:
`{profile, applied, state, changed, set}`, state `applied`, `reverted`, `kept`,
`failed` or `declined` (with `profile` empty), and `set` the settings the apply
wrote. **The changed-from-stock document names a profile as the source of a value
only when its apply wrote that value and the setting still holds it** - not a value
the apply found already there, not one the environment manages, whatever either
equals.

Both records are read from the database whenever they decide anything - the list,
the diff, an apply, a revert, a discard, the checklist and the changed document -
and not from the copy the server loaded, so that a record the command line wrote
beside a running server is the one that server reads, and a way back it saved is
never written over. Neither record is a setting: no default, in no create script,
in no changed-from-stock list, not nameable from the environment, and left out of
the XML backup, which carries a configuration and not this server's history with a
profile - a way back restored onto values it was not taken of would put back values
that were never there. Both are created, empty, at start-up where they are missing;
a record that is there is never emptied by a start.

## The command line

`hmailserver --apply-profile NAME [--confirm] [--once] [--if-new]` and
`hmailserver --revert-profile` open the configuration and the database as the
service does and print one line per setting. **On Windows they refuse while the
service runs (exit 4)**, as `--upgrade-database` does: a running server holds the
settings it loaded and would not see these until a reinitialise, and it is changed
through REST, COM, the Deck or the Control Panel, which show the diff first. On
Linux nothing says whether a server is running - systemd tracks it and no pid file
is written - so run them with the server stopped, as the container's entrypoint
does; a server running beside them reads the records at once and the settings at
its next reinitialise. `NAME` `none` records that no profile is wanted.
`--once` does nothing on a server something was recorded for before, except an
apply that failed. `--if-new` does nothing on a server in service - ten accounts or
more, the sample domain's not counted, the checklist's rule - and exits 5. Exit 0
done or nothing to do, 1 refused or failed, 2 no such profile or no database, 3 an
entry needs `--confirm`, 4 the service is running, 5 `--if-new` and the server is in
service.

## What a profile never touches

`build/generate-profiles.py` refuses, before anything is written:

* a **port** - a listener's or a daemon's;
* a **directory, a file or a path**: a path is the installation's;
* an **address the server binds to, a command it runs or an address it calls** -
  a `*BindAddress`, a `*Command`, a `*Url`: each is the installation's, and a
  command is refused by the ini route to anybody but the administrator password;
* a **database value**: the `[Database]` section is the bootstrap and not a
  setting, and the `[Settings]` keys about the database connection are refused
  as well;
* a **password**, a secret, a token, a pepper, a passphrase or a private key;
* a record the server keeps for itself (the environment's two, the profile's two,
  the sample domain's id, the VAPID public key);
* a name that is not a setting, or a value the setting would refuse - a whole
  number where the server reads one, text where it is text, and a number outside
  the range a REST row's check writes as `v.number < A || v.number > B` or
  `v.number < A`, for an `hm_settings` property and for a `[Settings]` key a row
  keeps. The rest of a row's check is held by the server itself, when a diff is
  asked for;
* a **caution** missing or not earned, and a window of days whose 0 the script
  does not know (it is refused until its sweep is read and it is listed);
* a **reason** that is more than one sentence - a full stop inside it or at its
  end - or not one line;
* an entry that opens a listener, trusts an address range or weakens a check in a
  profile whose **summary** does not name it: the paragraph an administrator
  chooses a profile by names what the profile takes away, and a profile that
  includes another names what the included one takes away too.

Nor does a profile set what is not a setting at all: a listener's TLS (a port's
own), an IP range and what it may do - "Require SSL/TLS when authenticating"
among it, which is what refuses a password sent in the clear - Incoming relays, a
DNS block list, a domain's own settings - DKIM, its quarantine digest, its external
tag - the `[Database]` connection pool, or the `[SendingLimits]` keys. A profile
that wants one of those says so in its **steps**, which every surface shows under
the diff.

A profile does not ask for a confirmation to switch a control **on** - attachment
blocking, the sender check - though such a control refuses or changes mail it let
through before: the diff shows the change, the revert switches it off again, and
nothing it did is lost with the way back. An attachment the blocking replaced with
a notice is not recovered by the revert.

## Levels, and limits

A profile sets the **server** level only (`docs/SettingLevels.md`). Where an entry
is the server level of a setting a domain and an account may hold their own value
of, the diff says which kind it is. For a **limit** - `office` sets the maximum
message size, the session lifetime and the recover-deleted window - the server's
value is the ceiling every domain and account below it is held to: raising it lets
a domain's own value up to it, and lowering it caps a tenant's own value when it is
read.

## At setup

* **The Deck's checklist** has a first item, **A profile for this server**: action
  needed on a young server - fewer than ten accounts, the sample domain's not
  counted - that no profile has been applied to; done once one has been, with which
  and when; action needed again when one could not be written in full; not needed
  on a server in service, where a profile is not something to suggest in a list of
  things left to do, or when the shipped settings were chosen. Below the items the
  four profiles are offered, each with its diff, apply and the way back, and
  **Keep the shipped settings**.
* **The installer** offers the four on its last page before it installs, for a new
  installation only - one that has no database yet - with one sentence each and
  "None" chosen until another is; the sentence names the listener, the filter or
  the trust a profile changes, and choosing it is the confirmation. After the
  database step - DBSetupQuick for the built-in database, DBSetup for an external
  one - it stops the service the database tools started over COM and runs
  `hMailServer.exe --apply-profile NAME --once --if-new --confirm`, before it starts
  the service. `--if-new` leaves alone a database DBSetup attached that is already
  in service: a profile is applied there from the Deck or the Control Panel, after
  its diff. "None" runs `--apply-profile none --once --if-new`, recording the
  choice. A silent installation takes `/profile=NAME` (or `none`), which is the same
  explicit choice. Every failure - the service would not stop, the verb did not
  start, a refusal - is said, naming the profile as not applied, and the
  installation still succeeds with the shipped settings.
* **The container** applies `HM_PROFILE` once (`--once`), before the server starts:
  see `platform/packaging/README.md`.
* **The Control Panel** has a **Setting profiles** page under *Set up this
  server*, beside the setup checklist: the profile in effect, each profile's diff, apply - with a box to tick
  where an entry asks for the confirmation - put back, discard and keep the shipped
  settings, over the COM members above. Its *Setup checklist* page
  has the same first item.
* **`hmctl`**: `settings profiles`, `settings profile NAME` (which prints the
  diff's token), `settings apply-profile NAME [--confirm TOKEN | --token TOKEN]`,
  `settings revert-profile`, `settings discard-profile`, `settings decline-profile`.

## The four profiles

<!-- The four profiles: generated by build/generate-profiles.py from Common/Application/Profiles/*.json. Edit the JSON, not this section. -->

### office - A small office server

One server that receives an office's mail from the Internet, holds its mailboxes and sends what its people write. SPF, the MX check, DKIM and DMARC score what they catch, greylisting defers a first delivery from an unknown sender, a message that reaches the delete threshold after DATA is held in the quarantine rather than refused, an address that keeps failing to sign in is banned and an account that keeps failing is locked, passwords are held to a policy and checked against known breaches, a signed-in person sends only as themselves, and a browser session lasts a working day.

31 entries.

| Store | Setting | Value | Why | Asks for a confirmation where it |
|---|---|---|---|---|
| database | `usespf` | `1` | a message from an address its sender's domain has not authorised in its SPF record is scored as a forgery |  |
| database | `usemxchecks` | `1` | a message whose envelope sender's domain has no MX record, and so cannot be answered, is scored |  |
| database | `ASDKIMVerificationEnabled` | `1` | a message whose DKIM signature fails to verify is scored, and each signer that verifies is recorded for Authentication-Results and the greylisting bypass list |  |
| database | `ASDMARCEnabled` | `1` | a message that fails its From domain's own DMARC policy of quarantine or reject is scored, as shipped, and recorded for the aggregate reports |  |
| database | `spammarkthreshold` | `5` | a message that scores 5 or more is marked as spam, as shipped, written down so the diff shows it |  |
| database | `spamdeletethreshold` | `20` | a message that scores 20 or more is refused, or held in the quarantine when it is judged after DATA, as shipped and well clear of the mark |  |
| database | `antispamaddheaderspam` | `1` | a message marked as spam carries X-hMailServer-Spam: YES, as shipped, which a mail client's own junk filter can act on |  |
| database | `antispamprependsubject` | `1` | the subject of a message marked as spam begins with the text set for it, [SPAM] as shipped, so people see it at a glance |  |
| database | `usegreylisting` | `1` | a first delivery from an unknown sender, recipient and address is deferred, which a real server retries and most spam software does not |  |
| database | `BypassGreylistingOnSPFSuccess` | `1` | a message whose SPF passes is not greylisted, as shipped |  |
| database | `BypassGreylistingOnMailFromMX` | `1` | a message from an address its sender domain's own MX or A record names is not greylisted either |  |
| ini | `QuarantineEnabled` | `1` | a message that reaches the delete threshold after DATA is held for release instead of refused, so a false positive can be put back |  |
| ini | `AuthenticationResultsEnabled` | `1` | the SPF, DKIM and DMARC verdicts are written into each message as Authentication-Results, which a mail client reads and the webmail's brand logos look for |  |
| database | `AutoBanOnLogonFailureEnabled` | `1` | an address that keeps failing to sign in is banned, as shipped |  |
| database | `MaxInvalidLogonAttempts` | `5` | an address is banned at its fifth failed sign-in within the window rather than its third, so one mistyped password is not a ban |  |
| database | `LogonAttemptsWithinMinutes` | `30` | the failures are counted over thirty minutes, as shipped |  |
| database | `AutoBanMinutes` | `60` | a ban lasts an hour, as shipped, which stops a guessing run and lets a person back in after lunch |  |
| ini | `AccountLockoutThreshold` | `10` | an account that fails ten sign-ins within AccountLockoutWindowMinutes, from any addresses, is locked for AccountLockoutMinutes, thirty minutes each as shipped |  |
| ini | `SmtpAuthenticatedSenderCheck` | `1` | a signed-in account may put in MAIL FROM only its own address, an alias of it, one it verified, or a mailbox that granted it the post right |  |
| ini | `MaxSubmissionsPerIPPerMinute` | `60` | no one address may start more than sixty messages a minute, signed in or not, so a stolen password cannot pour thousands through one |  |
| ini | `PasswordPolicyMinimumLength` | `10` | a password shorter than ten characters is refused wherever one is chosen, and one already set keeps working |  |
| ini | `PasswordPolicyRejectCommon` | `1` | a password on the server's list of the most commonly used ones is refused wherever one is chosen |  |
| database | `BreachCheckEnabled` | `1` | a password being chosen that is in the published breach corpus is refused, and only the first five characters of its SHA-1 leave the server |  |
| database | `disconnectinvalidclients` | `1` | an SMTP session that earns more error replies than the limit below is disconnected rather than answered for ever |  |
| database | `maximumincorrectcommands` | `20` | twenty error replies in one SMTP session, not the hundred shipped, is a probe rather than a confused mail client |  |
| database | `maxmessagesize` | `25600` | SMTP accepts a message of up to 25 MB rather than 20, which most providers accept, so a message sent here can be sent on |  |
| database | `SessionLifetimeSeconds` | `36000` | a webmail, portal or Deck session ends after ten hours however busy, rather than the twelve that apply while it is unset |  |
| database | `CreateDefaultSpecialUseFolders` | `1` | an account created from now on has Drafts, Sent, Junk and Trash before its first client connects |  |
| database | `enableattachmentblocking` | `1` | an attachment whose name is on the blocked list, which ships with programs and scripts on it, is replaced with a notice before the message is delivered |  |
| database | `RecoverDeletedDays` | `14` | a message a person deleted can be put back for two weeks, where nothing deleted is kept as shipped | shortens how long something is kept, where the window in force is longer |
| database | `ReputationWatchEnabled` | `1` | once a day the addresses this server knows it sends from are looked up in the block lists receivers use, and a listing is reported, as shipped |  |

What it cannot do for you:

* Check that "Require SSL/TLS when authenticating" (the Control Panel's Require TLS for auth) is on for the Internet IP range - a new installation turns it on, an upgraded one keeps what it had (docs/SecureDefaults.md) - or make the submission port STARTTLS-required: that is what refuses a password sent in the clear - SMTP AUTH LOGIN, IMAP LOGIN and POP3 alike - and an IP range is not a setting a profile can write.
* Give the submission port (587) STARTTLS with a certificate, or add port 465 with TLS: a port's security is the port's, not a setting, and the checklist's submission item says which port still takes a password in the clear.
* Switch each domain's quarantine digest and external-sender tag on in the domain's own settings: both are a domain's choice, and the server has no value of them to set.
* Set quarantine_digest_base_url to the address the webmail answers on, or the digest has no links to send.

### gateway - Behind a filtering gateway

Inbound mail arrives from a filtering gateway - a hosted service or an appliance in front of this server - that has already judged it. The gateway's addresses go in Incoming relays, so that SPF, HELO, PTR, DMARC and the DNS block lists judge the address the gateway received each message from rather than the gateway's own; greylisting is switched off, because it judges the connecting address, which is the gateway's, and would only delay mail the gateway has filtered; there is no per-address submission ceiling or recipient tarpit against the gateway's few addresses; XCLIENT is honoured, and an ARC seal trusted, only from the gateway and the sealer you name; and people who sign in here directly are held to the auto-ban and the sender check.

17 entries.

| Store | Setting | Value | Why | Asks for a confirmation where it |
|---|---|---|---|---|
| database | `usespf` | `1` | through Incoming relays SPF judges the address the gateway received the message from, which is the real sender's rather than the gateway's |  |
| database | `ascheckhostinhelo` | `1` | through Incoming relays the name the sender gave the gateway in HELO, read from the gateway's Received header, must resolve to the sender's own address |  |
| database | `ascheckptr` | `1` | through Incoming relays the sender's own address, not the gateway's, must have a PTR record whose name resolves back to it |  |
| database | `ASDMARCEnabled` | `1` | the From domain's DMARC policy judges the real sender through Incoming relays, as shipped, and the ARC entry below offsets its penalty only while it is on |  |
| database | `usegreylisting` | `0` | greylisting judges the connecting address, which is the gateway's even through Incoming relays, so it would only delay mail the gateway has filtered | turns a filter or a check off, or weakens it |
| database | `ASDKIMVerificationEnabled` | `1` | a signature the gateway left intact still verifies here, and a message whose signature fails is scored |  |
| database | `ASArcFilteringEnabled` | `1` | with DMARC on, a message the gateway changed so that DMARC fails here has the DMARC penalty offset when a sealer named in arc_trusted_sealers sealed a pass for it | trusts an address range |
| ini | `SMTPXClientEnabled` | `1` | the gateway can pass the real client's address with XCLIENT, honoured only from the addresses in SMTPXClientTrustedIPs and from nobody while that is empty | trusts an address range |
| ini | `MaxSubmissionsPerIPPerMinute` | `0` | every inbound message comes from the gateway's few addresses, and a ceiling on messages a minute per address would throttle all of it; 0 is off, as shipped | turns a filter or a check off, or weakens it |
| ini | `SmtpTarpitCount` | `0` | the gateway delivers messages with many recipients, which the recipient tarpit would slow; 0 is off, as shipped | turns a filter or a check off, or weakens it |
| ini | `RejectFullMailboxAtRcpt` | `1` | a mailbox already full is refused at RCPT, as shipped, so the gateway returns the message itself rather than this server bouncing to a sender it cannot check |  |
| database | `usemxchecks` | `1` | a message whose envelope sender's domain has no MX record is scored, which the gateway's address does not change |  |
| database | `antispamaddheaderreason` | `1` | each test that scored a message says so in the X-hMailServer-Reason headers, as shipped, so a message the gateway passed can be traced |  |
| ini | `QuarantineEnabled` | `1` | through Incoming relays the content tests run after DATA, so a message they bring to the delete threshold is held for release rather than refused back to the gateway |  |
| database | `AutoBanOnLogonFailureEnabled` | `1` | people sign in here directly, not through the gateway, and an address that keeps failing to sign in is banned, as shipped |  |
| ini | `SmtpAuthenticatedSenderCheck` | `1` | a signed-in account may put in MAIL FROM only its own address, an alias of it, one it verified, or a mailbox that granted it the post right |  |
| database | `enableattachmentblocking` | `1` | an attachment whose name is on the blocked list is replaced with a notice here too, whatever the gateway let through |  |

What it cannot do for you:

* Put the gateway's addresses in SMTPXClientTrustedIPs if it sends XCLIENT, so that the session takes the real client's address from the start: until then XCLIENT is honoured from nobody.
* Before applying the profile, add the gateway's addresses to Incoming relays and leave spam protection on for the IP range they connect from: Incoming relays run SPF, HELO, PTR, DMARC and the DNS block lists after DATA against the address the gateway received the message from, read from its Received header, and until the addresses are there those tests judge the gateway's own.
* Name the ARC sealer the gateway signs as in arc_trusted_sealers: until then no ARC seal is trusted, and the ARC entry offsets nothing.
* Check that "Require SSL/TLS when authenticating" is on for the Internet IP range - a new installation turns it on, an upgraded one keeps what it had (docs/SecureDefaults.md) - or make the submission port STARTTLS-required, for the people who sign in here directly: that is what refuses a password sent in the clear, and an IP range is not a setting a profile can write.
* On the listener the gateway delivers to, leave TLS optional unless the gateway offers it: a port's security is the port's, not a setting.
* A backup-MX callout to the primary is a remote domain policy (callout), which a profile does not make.

### relay - An outbound relay

A server that sends mail for applications and other servers and holds no mailbox anybody reads. IMAP and POP3 are closed at the next restart, an address that bounces hard is suppressed and a submission to it refused at once, deliveries back off from a destination that asks them to and never hold more than ten sessions open to one, and every message is traced and the trace kept for three months, because a relay is asked about what it sent long after it sent it.

24 entries.

| Store | Setting | Value | Why | Asks for a confirmation where it |
|---|---|---|---|---|
| database | `protocolimap` | `0` | nobody reads mail on a relay, and a listener nobody uses is one more to attack; it closes at the next restart | closes a listener |
| database | `protocolpop3` | `0` | the same for POP3, which also closes at the next restart | closes a listener |
| database | `AutoBanOnLogonFailureEnabled` | `1` | an address that keeps failing to sign in with a relay's credentials is banned, as shipped |  |
| database | `CreateDefaultSpecialUseFolders` | `0` | an account created here gets no Drafts, Sent, Junk or Trash, as shipped: it is a sender's credential, not a mailbox |  |
| database | `DirectoryEnabled` | `0` | the directory, which lists a domain's accounts to one another over REST and CardDAV, is not answered: the accounts are applications, not people |  |
| database | `BounceProcessingEnabled` | `1` | every bounce is read and counted, and an address that reaches the hard-bounce threshold is suppressed for its sending domain |  |
| database | `BounceHardBounceThreshold` | `1` | one hard bounce suppresses the address, as shipped: the receiver has refused it permanently |  |
| database | `BounceSuppressionAction` | `0` | a submission to a suppressed address is refused at RCPT, as shipped, so the application learns it at once rather than from a bounce |  |
| database | `BounceSuppressionExpiryDays` | `180` | an automatic suppression is removed after six months, giving the address another chance, where none is removed as shipped; one added by hand stays | shortens how long something is kept, where the window in force is longer |
| database | `BounceReadDeliveryReports` | `1` | a delivery report that arrives later counts as a bounce when it names a message this server sent, as shipped |  |
| database | `OutboundBackoffMaxMinutes` | `60` | a destination that keeps asking this server to slow down is left alone for at most an hour at a time, as shipped |  |
| database | `OutboundBackoffMinutes` | `2` | a destination that answers a slow-down reply is left alone for two minutes, doubling while it keeps asking, as shipped |  |
| database | `MaxOutboundConnsPerDestination` | `10` | no destination is held more than ten sessions from this server at once, where the delivery threads are the only limit as shipped |  |
| database | `maxdelivertythreads` | `25` | twenty-five deliveries run at once rather than the ten shipped, because a relay's work is delivery |  |
| database | `smtpnoofretries` | `12` | twelve retries rather than the four shipped, so a destination that is down for most of a working day still gets its mail |  |
| database | `smtpminutesbetweenretries` | `60` | an hour between the retries after the quick ones, as shipped |  |
| ini | `QuickRetries` | `2` | the first two retries come QuickRetriesMinutes apart, six minutes as shipped, for a receiver that greylists or failed for a moment |  |
| database | `SmtpDeliveryConnectionSecurity` | `2` | a delivery to a recipient's MX host uses STARTTLS whenever the host offers it, as shipped |  |
| database | `VerifyRemoteSslCertificate` | `1` | the certificate of a host this server delivers to is verified, as shipped: a failure ends a connection that requires TLS, and is forgiven on an opportunistic STARTTLS |  |
| ini | `MtaStsEnabled` | `1` | a recipient domain whose MTA-STS policy is in enforce mode is delivered to only over TLS to a host its policy names, as shipped |  |
| ini | `DaneEnforcementEnabled` | `1` | a recipient MX host with DNSSEC-signed TLSA records is held to them, as shipped |  |
| ini | `MessageTraceEnabled` | `1` | each message's acceptance, refusal, delivery and failure is recorded by queue id, which answers what happened to it; it is a record of who writes to whom |  |
| ini | `MessageTraceRetentionDays` | `90` | three months of the trace rather than the thirty days shipped, because a relay is asked about old sends | shortens how long something is kept, where the window in force is longer |
| database | `ReputationWatchEnabled` | `1` | once a day the addresses this server knows it sends from are looked up in the block lists receivers use, and a listing is reported, as shipped |  |

What it cannot do for you:

* Decide who may relay: that is an IP range's question - require authentication on the range the senders connect from, or name their addresses - and a profile does not answer it.
* Turn on "Require SSL/TLS when authenticating" on the IP range the senders connect from, or make the submission port STARTTLS-required: that is what refuses a password sent in the clear, and an IP range is not a setting a profile can write.
* Raise [Database] NumberOfConnections in hMailServer.ini to about the delivery thread count, 25 here: the deliveries, the sign-ins and the rest share that pool, five connections as shipped, and a wait for one longer than DBConnectionAcquireTimeout fails.
* Switch DKIM signing and its key rotation on for each sending domain: both are the domain's, and the server has no value of them to set.
* Set the sending limits per account in [SendingLimits] if the senders are to be held to a volume: those keys are the settings file's, not the store's.

### sender - A high-volume sender

The relay's shape, for a server that sends a great deal - newsletters, notifications, receipts - and is judged by the large providers on its bounce and complaint rates. Everything the relay sets, IMAP and POP3 closed at the next restart among it, and more deliveries at once, a ceiling per destination and per sending domain, the feedback loops read so that one complaint suppresses the address, a hard bounce suppressed for good, the trace kept for six months and the reports receivers send about this server kept for a year.

Everything `relay` sets, with the entries below replacing or adding to it - 32 entries in all.

| Store | Setting | Value | Why | Asks for a confirmation where it |
|---|---|---|---|---|
| database | `protocolimap` | `0` | nobody reads mail on a relay, and a listener nobody uses is one more to attack; it closes at the next restart (from `relay`) | closes a listener |
| database | `protocolpop3` | `0` | the same for POP3, which also closes at the next restart (from `relay`) | closes a listener |
| database | `AutoBanOnLogonFailureEnabled` | `1` | an address that keeps failing to sign in with a relay's credentials is banned, as shipped (from `relay`) |  |
| database | `CreateDefaultSpecialUseFolders` | `0` | an account created here gets no Drafts, Sent, Junk or Trash, as shipped: it is a sender's credential, not a mailbox (from `relay`) |  |
| database | `DirectoryEnabled` | `0` | the directory, which lists a domain's accounts to one another over REST and CardDAV, is not answered: the accounts are applications, not people (from `relay`) |  |
| database | `BounceProcessingEnabled` | `1` | every bounce is read and counted, and an address that reaches the hard-bounce threshold is suppressed for its sending domain (from `relay`) |  |
| database | `BounceHardBounceThreshold` | `1` | one hard bounce suppresses the address, as shipped: the receiver has refused it permanently (from `relay`) |  |
| database | `BounceSuppressionAction` | `0` | a submission to a suppressed address is refused at RCPT, as shipped, so the application learns it at once rather than from a bounce (from `relay`) |  |
| database | `BounceSuppressionExpiryDays` | `0` | a hard-bounced address stays suppressed until somebody removes it, because a sender judged on its bounce rate never writes to it again | shortens how long something is kept, where the window in force is longer |
| database | `BounceReadDeliveryReports` | `1` | a delivery report that arrives later counts as a bounce when it names a message this server sent, as shipped (from `relay`) |  |
| database | `OutboundBackoffMaxMinutes` | `60` | a destination that keeps asking this server to slow down is left alone for at most an hour at a time, as shipped (from `relay`) |  |
| database | `OutboundBackoffMinutes` | `2` | a destination that answers a slow-down reply is left alone for two minutes, doubling while it keeps asking, as shipped (from `relay`) |  |
| database | `MaxOutboundConnsPerDestination` | `20` | no destination is held more than twenty sessions from this server at once, which spreads a send without looking like a flood |  |
| database | `maxdelivertythreads` | `50` | fifty deliveries run at once, so a high-volume sender's queue drains in minutes rather than hours |  |
| database | `smtpnoofretries` | `12` | twelve retries rather than the four shipped, so a destination that is down for most of a working day still gets its mail (from `relay`) |  |
| database | `smtpminutesbetweenretries` | `60` | an hour between the retries after the quick ones, as shipped (from `relay`) |  |
| ini | `QuickRetries` | `2` | the first two retries come QuickRetriesMinutes apart, six minutes as shipped, for a receiver that greylists or failed for a moment (from `relay`) |  |
| database | `SmtpDeliveryConnectionSecurity` | `2` | a delivery to a recipient's MX host uses STARTTLS whenever the host offers it, as shipped (from `relay`) |  |
| database | `VerifyRemoteSslCertificate` | `1` | the certificate of a host this server delivers to is verified, as shipped: a failure ends a connection that requires TLS, and is forgiven on an opportunistic STARTTLS (from `relay`) |  |
| ini | `MtaStsEnabled` | `1` | a recipient domain whose MTA-STS policy is in enforce mode is delivered to only over TLS to a host its policy names, as shipped (from `relay`) |  |
| ini | `DaneEnforcementEnabled` | `1` | a recipient MX host with DNSSEC-signed TLSA records is held to them, as shipped (from `relay`) |  |
| ini | `MessageTraceEnabled` | `1` | each message's acceptance, refusal, delivery and failure is recorded by queue id, which answers what happened to it; it is a record of who writes to whom (from `relay`) |  |
| ini | `MessageTraceRetentionDays` | `180` | six months of the trace, because a sender is asked about a campaign long after it went | shortens how long something is kept, where the window in force is longer |
| database | `ReputationWatchEnabled` | `1` | once a day the addresses this server knows it sends from are looked up in the block lists receivers use, and a listing is reported, as shipped (from `relay`) |  |
| ini | `MaxOutboundPerDestinationPerMinute` | `120` | at most 120 messages a minute go to one destination and the rest wait, so that a burst cannot undo an address's warm-up |  |
| database | `MaxOutboundConnsPerTenant` | `25` | no one sending domain holds more than twenty-five sessions at once, so it cannot take every delivery and hold up the others' mail |  |
| database | `ComplaintProcessingEnabled` | `1` | the abuse reports the providers send to the complaint mailbox are read and counted per domain and per account |  |
| database | `ComplaintSuppressionThreshold` | `1` | one complaint suppresses the address: the reader pressed the spam button and did not want the mail |  |
| database | `ComplaintRetentionDays` | `180` | six months of complaints, as shipped, which is longer than the week the complaint rate is measured over | shortens how long something is kept, where the window in force is longer |
| database | `DomainReportRetentionDays` | `365` | the DMARC and TLS reports receivers send about this server's domains are kept a year, as shipped | shortens how long something is kept, where the window in force is longer |
| ini | `MtaStsHostingEnabled` | `1` | the web services listener serves each domain's MTA-STS policy, as shipped, once it has a port, so receivers can hold this server's mail to TLS as well |  |
| database | `AlertsEnabled` | `1` | the conditions switched on as shipped - an address on a block list, a complaint rate over its threshold, a failed backup, a certificate near expiry - are mailed to AlertRecipient |  |

What it cannot do for you:

* Warm a new sending address up: start with a few hundred messages a day to each large provider and double it every few days while the bounce and complaint rates stay low. No setting does this; the per-destination ceiling below only keeps a burst from undoing it.
* Register a feedback loop with each large provider, and name the mailbox the reports arrive at in the complaint settings (mailbox): until then no complaint is ever read.
* Give the web services listener a port (WebServicesHttpsPort) and a certificate, and publish each sending domain's _mta-sts record and mta-sts host: until then the MTA-STS policy this server hosts is served to nobody.
* Set TlsRptFromAddress to a mailbox here, so that the TLS reports this server owes the domains that ask for them are sent, and AlertRecipient to a mailbox somebody reads, so that the alerts below reach somebody.
* Raise [Database] NumberOfConnections in hMailServer.ini to about the delivery thread count, 50 here: the deliveries, the sign-ins and the rest share that pool, five connections as shipped, and a wait for one longer than DBConnectionAcquireTimeout fails.
* Decide who may relay, require SSL/TLS when authenticating on the senders' IP range, and switch DKIM signing on per domain, as for the relay.

<!-- End of the generated section. -->

## Adding a profile

Write `Common/Application/Profiles/<name>.json` - `name` (its file's name, lower
case), `title`, `summary`, `steps`, optionally `includes` (another profile whose
entries come first, an entry of the same setting replacing the included one in
its place), and `entries`, each `{store, name, value, why}` with `caution` where
the entry closes a listener, trusts an address range, turns a filter or a check
off or weakens it, or is a window of days - and run
`python build/generate-profiles.py`. Read each setting's reader before writing its
reason: the reason is shown to an administrator as what the setting does. The
script refuses what a profile may not set, a caution that is missing or not
earned, a reason of more than one sentence, and a summary that does not name what
the profile takes away, with the file and the entry; it writes `ProfilesData.cpp`
and the tables above; `--check` holds both on every pull request. A profile is not
added at run time: four, shipped, reviewed.

## What is not there

* **Four profiles, not user-defined ones.** A profile is a reviewed document in
  the tree; there is no route that makes one.
* **One snapshot.** The way back is the last apply's only; a second apply waits
  for it to be reverted or discarded.
* **A value only you know is a step, not an entry**: the gateway's addresses in
  Incoming relays and for XCLIENT, the ARC sealer, the complaint mailbox, the
  TLS-RPT sender, the alert recipient, the database pool. A profile carries fixed
  values; the steps say what is left to fill in.
* **No parameter on a profile**, and no profile for a domain or an account.
* **A caution is decided by the value an entry sets**, and for a window by the one
  it replaces: raising a submission ceiling or a spam threshold that is already
  weaker than the shipped value does not ask, and the diff shows both values.
* **No confirmation to switch a control on**, above.
* The installer page, the container's `HM_PROFILE` and the POSIX verbs have been
  written and read, and not run: the installer was not built for this change, no
  container was started, and the Linux server was not compiled.

## Tests

`RegressionTests/API/RestApiProfiles.cs` (REST on 10030; the teardown
reinitialises, reverts any way back held and puts back what a test changed by
hand, and the settings are checked equal before and after each test):

* the list, and every entry of every shipped profile answered as written through a
  REST row, none refused, with a diff token;
* the diff for `office`, with every entry's current value, `changes`, and a token
  that moves with the diff;
* an apply that changes exactly those entries - neither a setting already changed
  from stock nor one at its default moved - recorded as exactly one audit event
  since the test began, and a second apply refused;
* a revert that puts every value back - the changed-from-stock document compared
  before and after, and every `[Settings]` key the profile sets stored or absent,
  with the same text, as it was - says what waits, and clears the snapshot, and a
  revert with nothing to revert refused;
* a setting an administrator changed after the apply left by the revert, the rest
  put back;
* the relay and then the sender applied with their confirmation, every entry
  written and none failed, every entry answering `changes: false` afterwards, and
  an effect of each read from what reads the setting - the directory no longer
  answered to an account, the message trace on and kept ninety days, then a hundred
  and eighty - gone again after each revert; the office password policy refusing a
  short password where an account is made, and taking it after the revert; the
  gateway's quarantine answered on for an account;
* an entry that turns a filter off refused without the confirmation and applied
  with it, and asking for nothing where the filter is already off; XCLIENT marked
  `trusts_addresses` and asking only where it would change; the relay's trace
  window asking where it would shorten a year and not where it lengthens thirty
  days, with the note; the relay's two listeners marked, its unconfirmed apply
  refused with IMAP still on, and a confirmation sent as a string or without the
  diff's token refused;
* an apply from a stale diff refused, and the fresh token applying;
* a setting the environment manages skipped with its variable named, and not
  credited to the profile in the changed document though the variable holds the
  profile's value;
* an unreadable way back answered as such, an apply refused, the revert failing
  with HM7300 in the error log, and the discard over REST clearing it;
* keeping the shipped settings recorded, the checklist's item not needed, and the
  choice refused once a profile is in effect;
* the COM round trip, a confirmation without its token refused, and `kept` in the
  list and the checklist after a discard;
* the verbs: refused with exit 4 beside the running service; with it stopped, the
  relay without `--confirm` exiting 3, `--if-new` leaving a server of ten accounts
  with exit 5, `none` recorded, `--once` then applying nothing, the office profile
  applied and a second apply refused, `--revert-profile` putting it back, and each on
  the audit trail under the command line's name;
* the checklist's first item on a young server, applied, reverted, and not needed
  once the server holds ten accounts;
* the changed-from-stock document naming the profile as the source of a value its
  apply wrote, and not of one the apply found already there;
* the generator passing the tree and an unchanged copy, and refusing on a copy a
  port, a directory, a database value, a secret, a record, an unknown name, a value
  outside its row's range, a reason of two sentences, an unearned caution, a filter
  turned off without its caution, a bind address, and a weakened check the summary
  does not name.

Not tested: the way back longer than 4000 characters (every shipped entry's value
is a short number, so no shipped profile can produce one); an entry its setting
refuses (no shipped profile has one, and the generator refuses one first); a write
failing part way, and what COM says of it; `--once` taking an unreadable record for
applied; the records' absence from the XML backup; the installer page and the
container. No IMAP or SMTP client is used, so a listener closed at a restart is not
probed.

The Deck harness (`build/deck-script-test.js`) checks the checklist item and the
two new codes in the page's own words, the diff, the environment's mark, the
window's mark and note, the confirmation box, the token on the apply, the stale
apply redrawn, the apply and the revert with what waits, the way back and its
discard, the unreadable way back, and keeping the shipped settings;
`build/check-cli.py` the six `hmctl` verbs, the token, and the refusal for want of
`--confirm`.
