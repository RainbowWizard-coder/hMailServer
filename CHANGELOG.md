# Changelog

This file records what changed in each release of hMailServer that this project has published, newest first,
and what is in the tree after the latest release. It is a summary. The full notes of a release - the reasoning,
the tests and the verification - are on its GitLab release page,
`https://gitlab.com/Progressiverobot/hmailserver/-/releases/<tag>`, which every section below links to.

This project is a maintained fork of hMailServer 5.7 by Martin Knafve. Its own releases begin at 6.0.0; the
`hMailServer-5.7.0-bNNNN` tags in the repository are upstream builds, not releases of this project.

**The latest release is 6.3.3, published on 15 September 2026.** Anything that arrived after it is in the tree
after 6.3.3 and not yet released. It is listed under [Unreleased](#unreleased---in-the-tree-after-633-not-yet-released)
and will carry a version number when a release is cut.

## Where the releases are

The project's home is <https://gitlab.com/Progressiverobot/hmailserver>. Its releases were published on GitHub
until GitHub suspended the account on 18 September 2026, and every `github.com/Progressiverobot` address in older
text now answers 404. The releases are being re-created on GitLab from the GitHub originals, under their original
dates: <https://gitlab.com/Progressiverobot/hmailserver/-/releases>, with the latest always at
<https://gitlab.com/Progressiverobot/hmailserver/-/releases/permalink/latest>. Until a version's page exists, its
tag is at `https://gitlab.com/Progressiverobot/hmailserver/-/tags/<tag>`.

One release is not a version of the server and has no section here:
[`build-inputs-1`](https://gitlab.com/Progressiverobot/hmailserver/-/tags/build-inputs-1), of 11 September 2026,
holds the three third-party files the Windows installer build downloads instead of keeping them in git - the
7-Zip 19.00 console, the MariaDB Connector/C 3.4.9 client and the SQL Server Compact 4.0 SP1 x64 runtime. Each is
recorded by SHA-256 in `hmailserver/docs/third-party-binaries.json` and fetched by
`build/get-installer-binaries.ps1`. Since 25 September 2026 the 7-Zip console is 26.03, fetched from 7-zip.org itself, and
the release's 7-Zip 19.00 is no longer used.

## How to read a section

- **Dates** are UTC. Where the release's own publication time was not saved, the date is the tag's, and the
  heading says so.
- **Schema** is the database version the release requires: `REQUIRED_DB_VERSION` in
  `hmailserver/source/Server/Common/Application/Constants.h` at the tag. The installer applies the steps with
  DBUpdater on Windows; from 6.3.0, `hmailserver --upgrade-database` does it on Linux. The upgrade is one-way, so
  take a backup of the database and of the data directory before installing.
- **Pre-releases** (the 6.0.0 B builds, 6.2.22-pre1 to pre6 and 6.2.23 Alpha 1 and 2) are listed where they fall
  and marked as such. Three tags have no release and are listed in one line each: 6.0.0-B3, for which no
  release is known, and 6.2.22-pre4 and pre5, whose releases were deleted.

## Issue and pull-request numbers

Every issue, pull-request and discussion number written before 24 September 2026 is GitHub's, from the project's
former home, and this file writes it in words: "GitHub issue 263", "GitHub PR 12", "GitHub discussion 18". GitLab
numbers its own issues from 1 again and links a bare `#` and a number to its own issue of that number, so no
number is written that way here. Upstream hMailServer's numbers are written the same way, as "upstream PR 552" or
"upstream issue N". A person's handle on either forge is written in a code span, so that nobody is notified.

## How this file is kept

By hand. It is not generated: the project does not use GitLab's `Changelog:` commit trailers. The Unreleased
section is updated when a batch lands on `master`, never by the commits of individual roadmap rows, which would
make every fold of parallel work conflict on this file. A released version's section is written once, from its
notes, when the release is published. No CI runs on GitLab yet, so nothing checks this file automatically: after
editing it, run `python build/add-license-headers.py --check` and read the rendered page for a stray `#` number
or `@` name.

---

## Unreleased - in the tree after 6.3.3, not yet released

Everything in this section is on `master` after the `v6.3.3` tag and in no release; a server running 6.3.3 has
none of it ([compare `v6.3.3` with `master`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.3...master)).
It landed between 15 and 24 September 2026. The schema moves from **6040 to 6096 in 56 steps**. Most of what is
new is off until an administrator turns it on; what changes on upgrade without a switch comes first. The
operator documents named below are in `hmailserver/docs`, which has 71 more of them than it had at 6.3.3.

### What changes on upgrade without a switch

1. **The database is the settings store, and the `[Settings]` section of `hMailServer.ini` is its copy**
   (schema 6042). At the first start the section's keys are taken into the database, with nothing to run by
   hand; a line edited after the old server last started, including one edited to prepare the upgrade, is
   taken for an edit and put back, so make such a change after upgrading. From then on a line edited in the
   file is ignored in favour of the stored value, named in the error log as `HM5804`, and put back; deleting a
   line no longer returns a setting to its default. A setting is changed in the Control Panel, over COM
   (`Settings.SetIniSetting`), over REST (`PUT /api/v1/settings/ini/{name}`), or on Linux with
   `hmailserver --set-setting NAME=VALUE`. A `[SettingsOverride]` section in the file is the deliberate way
   round a database that cannot be read, and is named as `HM5807` at every start. No new key is added to
   `[Settings]` from now on: a new setting is a row in `hm_settings`.
2. **Most saved settings take effect at once.** Every `[Settings]` key now says when a save applies it - 211
   at once, 28 at a reinitialise, 2 at a restart - and the Control Panel, the Control Deck, COM and REST list
   the ones still waiting ([SettingsApply.md](hmailserver/docs/SettingsApply.md)).
3. **SPF is RFC 7208**, ported from upstream PR 645, in place of the RFC 4408 library the server had carried.
   127.0.0.0/8 and `::1` no longer pass unconditionally before any policy is read, so where the SPF test or
   DMARC runs for a loopback client, mail from a domain whose policy does not name the address is judged like
   any other; a content filter that re-injects mail should authenticate or connect from an IP range with
   anti-spam off. SPF lookups follow the server's own resolver settings on Windows, and work on Linux, where
   every check had been TempError. Every result is written under its own keyword; only a fail scores, as
   before.
4. **The audit trail is on** (schema 6043): an administrative change over COM or REST writes a row, kept
   until a retention is set, with secrets recorded as `(changed)`. **Alerts are on**, with seven of their
   eleven conditions enabled, `backup.failed` and `certificate.expiring` among them, but nothing is mailed
   until `AlertRecipient` and `AlertSenderAddress` are set
   ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md)).
5. **Compromised-account detection starts in watch mode** (schema 6071): it builds each account's baseline
   from authenticated submissions and raises `account.compromised` when a submission has the shape of a
   takeover, and holds nothing until `compromise_detection_mode` is set to `enforce`
   ([CompromisedAccounts.md](hmailserver/docs/CompromisedAccounts.md)).
6. **A destination that asks this server to slow down is given a pause.** A 421, or a 4xx with one of the
   providers' rate-limit codes or phrases, holds deliveries to that destination alone for
   `OutboundBackoffMinutes` (2 by default, doubling to `OutboundBackoffMaxMinutes`, 60); 0 turns the back-off
   off. A wait this server chose no longer spends a message's retries
   ([OutboundShaping.md](hmailserver/docs/OutboundShaping.md)).
7. **A backup carries everything an account has** - the address book, the webmail's preferences, scheduled
   sends, file links, S/MIME keys, calendars, password history, the first-contact memory and every message's
   keywords - and a restore puts them back, with each folder's access grants. An archive written by 6.3.3 or
   earlier restores exactly as it did, without what it never held.
8. **The out-of-office memory survives a restart** (schema 6060): who has been answered is kept in a table,
   so a restart no longer sends every correspondent the reply again.
9. **Addresses collected from what a person sends are suggestions, not contacts.** They appear under Other
   contacts in the webmail, and CardDAV serves only the contacts a person kept.
10. **In the container image, the environment wins.** `HM_SETTING_<key>` and `HM_PROPERTY_<name>` (with `_FILE`
    variants) are stored at every start, and a setting the environment manages is refused to the other
    interfaces until a start without the variable
    ([EnvironmentConfiguration.md](hmailserver/docs/EnvironmentConfiguration.md)).
11. **`GET /api/v1/me/search` goes on from where it stopped.** A search with more hits than `limit` answered
    `complete: true, more: true` and could not reach the rest; it now answers `complete: false, more: true`
    with `next_before_id`, which `before_id` continues from, and `complete` is false with a
    `next_before_id` whenever an answer is not the end. Hits are newest first across every folder by message
    id, where they were folder by folder ([Performance.md](hmailserver/docs/Performance.md)).
12. **IMAP answers as the RFCs say, where Dovecot's imaptest found it did not.** A client that relied on the
    old answers sees these: a mailbox that does not exist, or a right the account lacks, is refused `NO`
    rather than `BAD`, and a failed SELECT or EXAMINE leaves no mailbox selected; an unknown LIST selection
    or return option is `BAD`; every `BODY[<section>]` without `.PEEK` sets `\Seen`, `BODY[HEADER]` and
    `BODY[1.MIME]` included; SORT uses every criterion, REVERSE reverses only its own, ties go to sequence
    order and strings fold ASCII letters only, and THREAD ORDEREDSUBJECT and REFERENCES build the RFC's
    trees, so both can order messages differently; and an APPEND stores the literal byte for byte, where it
    used to add a CRLF, so RFC822.SIZE of a newly appended message is two octets smaller than the same
    message appended before.

### Delivery and SMTP

- **Remote-domain policies** (schema 6045): for one remote domain or a pattern, the outbound TLS it demands
  (encrypted, verified or DANE - a delivery that cannot meet it is deferred, never sent in the clear), TLS
  required of its senders, size, connection and rate limits, whether out-of-office replies, forwards and Sieve
  redirects may go to it, and a backup-MX recipient callout
  ([RemoteDomains.md](hmailserver/docs/RemoteDomains.md)).
- **Throttling per destination and per tenant**: besides the back-off above, `MaxOutboundConnectionsPerDestination`
  and `MaxOutboundConnectionsPerTenant` (both off) so that one busy destination or one hosted domain cannot
  hold every delivery thread; the state is at `GET /api/v1/queue/shaping`.
- **IP warm-up** (schema 6093): a schedule for a new outbound address holds it to a daily ceiling of recipients
  at each large provider, rising along a standard, gentle or fast plan; nothing is held until a schedule exists
  ([IpWarmup.md](hmailserver/docs/IpWarmup.md)).
- **A send API for applications** (schema 6089): an API key becomes a sender, with an address of its own and
  ceilings a minute and a day; `POST /api/v1/send` sends and `GET /api/v1/send/{queue_id}` says what became of
  each recipient ([SendApi.md](hmailserver/docs/SendApi.md)).
- **DKIM keys that rotate** (schema 6048): a policy per domain publishes a second selector, switches once the
  world has seen it and retires the old key, with an alert when a rotation stalls; off on every domain
  ([DkimKeyRotation.md](hmailserver/docs/DkimKeyRotation.md)).
- **What a domain does to a message** (schema 6044): an external-sender tag on the subject or as a header, a
  first-contact note, and a disclaimer on mail leaving the organisation, all off; the domain's signature and
  disclaimer now also go on mail sent from the webmail and its send-later
  ([DomainTransforms.md](hmailserver/docs/DomainTransforms.md), [Signatures.md](hmailserver/docs/Signatures.md)).
- **Restricted delivery** (schema 6084): accounts, or a whole domain, that may exchange mail only with named
  domains ([RestrictedDelivery.md](hmailserver/docs/RestrictedDelivery.md)).
- **Message recall within the server** (schema 6085): the sender withdraws an unread message delivered to
  accounts on this server ([MessageRecall.md](hmailserver/docs/MessageRecall.md)).
- **Journaling and dual delivery** (schema 6075): a copy of a domain's or one account's mail sent to an
  address, as a journal report or as the message itself ([Journaling.md](hmailserver/docs/Journaling.md)).
- **Quota policy** (schema 6061): a domain's own warning threshold and text, a grace past the limit for
  incoming mail, and a share of the limit at which the account can no longer send
  ([QuotaPolicy.md](hmailserver/docs/QuotaPolicy.md)).
- **Out-of-office policy** (schema 6060): a window of days before the same sender is answered again, a text
  for colleagues and replies to colleagues only ([OutOfOffice.md](hmailserver/docs/OutOfOffice.md)).
- Fixed: an outbound bind address of one address family no longer stops delivery to the other (GitHub issue
  276); `SMTPDeliveryBindToIP` may name one address of each family, separated by a comma.

### Anti-spam, anti-virus and mail flow rules

- **Greylisting hardened** (schema 6072; GitHub discussions 281, 282 and 284): the triplet key no longer
  truncates long addresses; a sliding wait on an early retry and an abuse lock, each off by default; and a
  bypass list whose entries may require SPF, DKIM or DMARC to pass
  ([Greylisting.md](hmailserver/docs/Greylisting.md)).
- **SMTP policy a score cannot express** (schema 6073; GitHub discussions 283, 285 and 286): scores of their own
  for SPF softfail, permerror and temperror, a temperror deferral, a ban or refusal for a null sender with many
  recipients, and a domain white and black list by envelope domain or HELO; all off
  ([SmtpPolicy.md](hmailserver/docs/SmtpPolicy.md)).
- **Allow and block lists and quarantine digests** (schema 6046): senders or domains allowed or blocked for the
  server, a domain or one reader, covering the From header's address as well as the envelope sender, and URL
  entries for the links in a message; held mail summarised daily to the person it was held for
  ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
- **Mail flow rules** (schema 6063): a domain's own rules, a condition on an attachment's name, and actions to
  add a disclaimer, reject with a text, require TLS, quarantine or warn the sender; and an **outbound content
  policy** - card numbers, IBANs, account numbers and attachment types, to external recipients - as conditions
  of the same rules, with no second engine ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- **Impersonation protection** (schema 6087): protected persons and partner domains per domain, judged on mail
  from outside; off on every domain ([ImpersonationProtection.md](hmailserver/docs/ImpersonationProtection.md)).
- **Attachments opened somewhere safe** (schema 6091): an outside message whose attachment is an executable, a
  script, an archive or an office document is held until a sandbox service the administrator names answers;
  off ([Sandbox.md](hmailserver/docs/Sandbox.md)).
- **Spam traps** (schema 6058): an address that was never real refuses like an unknown user, bans the sender's
  address and puts the sender on the block list ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Country blocking** (schema 6052): connections and sign-ins judged by country from a MaxMind database the
  administrator supplies, with loopback and private ranges exempt and a missing database meaning allow
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Sealed mail** handled honestly: a message the server cannot read is no longer gutted by the
  blocked-attachment list or given a clean verdict by a scanner that saw only ciphertext
  ([SealedMail.md](hmailserver/docs/SealedMail.md)).

### IMAP, POP3 and the webmail

- **Twelve webmail features**: installs as an app with the unread count on its icon, shares from other apps and
  `mailto:` links; contacts imported and exported as vCard or CSV; saved searches as folders; the reader's own
  time zone; Undo on three more actions; the tracker notice counts what it blocked; swipe actions and snooze
  presets; three stars (`$Star2`, `$Star3` on top of the flag); rows coloured by a rule; `@`-mentions completed
  from the address book; a formatted signature with an image; and Report phishing beside Junk.
- **Arabic, Hebrew and Persian**, with the layout mirrored: 23 catalogues besides English.
- **Recover deleted items** (schema 6068): a deleted message is kept aside for `recover_deleted_days` (off by
  default) and put back by its owner ([RecoverDeleted.md](hmailserver/docs/RecoverDeleted.md)).
- **A message that expires** (schema 6069): the recipient gets a link, not the message, with an optional
  passcode ([ConfidentialMessages.md](hmailserver/docs/ConfidentialMessages.md)).
- **Offline mail** for the last N days, kept in the browser under a ceiling the administrator sets
  (`ReaderOfflineDaysMax`, 30) ([OfflineMail.md](hmailserver/docs/OfflineMail.md)); **Web Push** for new mail
  with the page closed (schema 6054, off).
- **A second account in the same page** ([SecondAccount.md](hmailserver/docs/SecondAccount.md)); the reply card
  above the conversation as a choice (GitHub discussion 266); auto-delete of Trash and Junk and auto-archive of
  the inbox as the reader's own choices.
- **Important, and sections of the inbox**, and emoji, pictures and tables in compose
  ([ImportantMail.md](hmailserver/docs/ImportantMail.md), [FormattedMessages.md](hmailserver/docs/FormattedMessages.md)).
- **Masked addresses** (schema 6067), **sending as another verified address** (schema 6066), managing
  subscriptions, and a sender's BIMI logo as its avatar for mail that passed DMARC
  ([MaskedAddresses.md](hmailserver/docs/MaskedAddresses.md), [SendAs.md](hmailserver/docs/SendAs.md),
  [BrandLogos.md](hmailserver/docs/BrandLogos.md)).
- **Calendars**: CalDAV (RFC 4791) beside CardDAV, on an iCalendar (RFC 5545) reader and writer of this
  project's own with bounded recurrence expansion (schema 6041); the webmail's calendar and tasks pages, several
  calendars with colours and shares (schema 6064), invitations by mail (iMIP), and free-busy, group and
  delegate shares (schema 6086) ([Calendar.md](hmailserver/docs/Calendar.md)).
- **Contacts**: contact groups, the organisation's directory of a domain's people and lists (schema 6078), and
  shared address books handed to a colleague or a whole domain (schema 6079)
  ([Contacts.md](hmailserver/docs/Contacts.md), [Directory.md](hmailserver/docs/Directory.md),
  [SharedAddressBooks.md](hmailserver/docs/SharedAddressBooks.md)).
- **Delegated mailboxes named in the Autodiscover answer**, in Exchange's `AlternativeMailbox` element and only
  to a request signed in as the delegate. Outlook reads that element for an Exchange account and not for an IMAP
  one, so for IMAP clients the mechanism that works is still the `#Users` namespace, which Thunderbird and
  Outlook's IMAP code show by themselves.
- Fixed: a heap-corruption abort under concurrent IMAP `LIST` and folder changes on the same account; every
  loop over a shared collection now iterates a locked copy.
- **A folder of a hundred thousand in the webmail**: a page copies one page of the folder, never all of it;
  Oldest reaches the end in one request; a search is bounded by time as well as by files read, spared by
  the full-text index, and goes on from where it stopped across every folder; select-all hands the
  selection to the server (`POST /api/v1/me/folders/{id}/bulk`), which works through it in bounded steps.
  The seeding and timing scripts are in `build/perf/`; the numbers are to be measured on the bench
  ([Performance.md](hmailserver/docs/Performance.md)).

### Security and identity

- **Administrator roles** (schema 6047): `serveradmin`, `domainadmin`, `helpdesk` and `auditor` over 24
  permissions, custom roles, assigned per account and per domain ([Roles.md](hmailserver/docs/Roles.md)).
- **Passkeys and single sign-on** (schema 6077): WebAuthn, and OpenID Connect from the organisation's issuer,
  into the webmail and the Control Deck; **SAML 2.0** beside it (no schema step), with an XML canonicaliser and
  signature verifier of the project's own ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **SCIM 2.0 provisioning** (schema 6096): the identity provider makes, renames, suspends and removes accounts,
  aliases and lists at `/scim/v2` ([Scim.md](hmailserver/docs/Scim.md)).
- **A recovery address and one-time recovery codes**, and self-service password reset (schema 6076)
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Sessions and devices** (schema 6053): every client that signs in is a row its owner and the administrator
  can see and revoke; the session policies are off after an upgrade
  ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **A new password checked against published breaches** by k-anonymity, wherever a password is chosen; off by
  default ([PasswordBreachCheck.md](hmailserver/docs/PasswordBreachCheck.md)).
- **Compromised-account detection** (schema 6071), in watch mode as above, with a review queue.

### Encryption

- **The server publishes its users' OpenPGP keys** (schema 6050) through Web Key Directory; publishing is off
  by default, and a secret key is refused at every entry point.
- **OpenPGP in the webmail** (schema 6080): end-to-end encryption in the browser, with the keys under a
  passphrase this server never sees; a correspondent's key remembered at first sight and a changed key shown
  as a warning (schema 6081); and a password-protected message for a correspondent with no key (schema 6082)
  ([OpenPGP.md](hmailserver/docs/OpenPGP.md)).
- **Zero-access mailboxes** (schema 6083): opt-in per account, delivered mail is encrypted to the account's own
  key once the filters have run, and the administrator cannot read, search or recover it
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Gateway encryption to partner domains** (schema 6094): a remote-domain policy encrypts mail to a partner's
  OpenPGP key or S/MIME certificate at this server, and says whether to refuse or send unencrypted when it
  cannot.
- **An organisation CA for S/MIME** (schema 6095): a domain's own authority certifies keys its people generate
  in the browser. It never makes a person's key, and the private key stays wrapped where this server cannot
  open it, so nothing here can recover mail encrypted to a lost key
  ([SmimeAuthority.md](hmailserver/docs/SmimeAuthority.md)).
- **The webmail's S/MIME made honest**: signing out takes the private keys with it, an administrator's password
  change re-wraps them, and a message that was to be sealed is refused as a draft rather than stored in the
  clear.

### Administration

- **One value, three levels** (schema 6088): 27 settings may be set for the server, a domain or an account,
  with every interface showing what is in effect and where it came from
  ([SettingLevels.md](hmailserver/docs/SettingLevels.md)).
- **Profiles**: office, gateway, relay and sender, each a documented set of settings applied with a diff first
  and a way back ([Profiles.md](hmailserver/docs/Profiles.md)); **what this server has changed from stock**
  ([ChangedFromStock.md](hmailserver/docs/ChangedFromStock.md)).
- **Two command-line clients over REST**, `hmctl` (installed by the Linux packages) and the `HmailServer`
  PowerShell module, with the whole configuration as one document to export, diff and apply, and accounts
  imported from and exported to a spreadsheet ([CommandLine.md](hmailserver/docs/CommandLine.md)).
- **The audit trail** (schema 6043) as a hash chain that can be verified, a setting's change put back from its
  row, and a Control Panel page; its first gaps - misspelt table names, the settings store, the access lists -
  closed. **Alerts** now cover eleven conditions, by mail or signed webhook.
- **A setup checklist the server judges**, a sample domain that can be thrown away, **help beside every page**
  of the Control Panel, the Deck and the webmail from the documents built into the binary, and first-run tours
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md), [SampleDomain.md](hmailserver/docs/SampleDomain.md),
  [Help.md](hmailserver/docs/Help.md)).
- **The Control Panel on one design system**: a command bar, a sidebar that collapses to a rail and dialogs
  sized to their content ([ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)); 64 pages, nine more
  than at 6.3.3.
- **The Control Deck** in twenty languages besides English, redrawn on the webmail's design and usable without a pointer, with
  one connection's log followed live ([FollowConnection.md](hmailserver/docs/FollowConnection.md)). The census in
  [DeckParity.md](hmailserver/docs/DeckParity.md) counts 456 of the 464 properties the desktop Control Panel
  writes as writable over REST and reachable from a Deck view; the eight it lists as missing are an account's
  directory fields.
- **A person's data as one archive**, and an erasure that reaches the rest of it
  ([PersonalData.md](hmailserver/docs/PersonalData.md)); **mailbox rename and move** as one operation (schema 6056)
  ([MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- Fixed: 72 COM classes answered a refusal as a bare HRESULT; each now says why. The message trace's search
  answered nothing on SQL Server Compact (one parameter written twice and bound once).

### Migration and import

- **A migration that survives being interrupted** (schema 6090): mailboxes copied from another IMAP server,
  Google Workspace or Microsoft 365 as jobs, one per mailbox, throttled, resumed without copying anything twice,
  with contacts and calendars beside the mail ([Migration.md](hmailserver/docs/Migration.md)).
- **Other accounts in the webmail** (schema 6065): POP3 and IMAP accounts fetched from the page, and an import
  from Gmail or Outlook.com with a sign-in (OAuth with PKCE).

### Lists and bounces

- **Bounces read and a suppression list** (schema 6049): delivery reports classified and counted per address,
  hard-bounced addresses suppressed; off by default ([BounceSuppression.md](hmailserver/docs/BounceSuppression.md)).
- **Complaints read** (schema 6055): the providers' feedback-loop reports recorded and fed to the suppression
  list; off by default ([Complaints.md](hmailserver/docs/Complaints.md)).
- **A distribution list that defines itself** (schema 6074): its members given as a query over the server's
  accounts ([DistributionLists.md](hmailserver/docs/DistributionLists.md)).
- **A list manager and bounce handling for lists** (schema 6092): subscribe and unsubscribe by mail or page with
  confirmation, one-click unsubscribe, an archive, a digest, VERP and a footer, and members paused after hard or
  repeated soft bounces; nothing changes for a list until it is switched on
  ([MailingLists.md](hmailserver/docs/MailingLists.md)).

### Operations

- **The update feed is off GitHub.** The live update reads `https://updates.progressiverobot.com`, which GitLab
  Pages serves from the project's releases, in place of GitHub's Releases API, which has answered 404 since
  18 September 2026. It trusts two signers: the GitHub workflow that signed every release up to 6.3.3, for a
  certificate issued before the suspension and a release no later than 6.3.3, and this project's GitLab CI
  pipeline for a `v*` tag, which is to sign the releases after and has signed none yet. A file signed for one
  release tag is refused as any other release; on Windows the installer's own version resource must name the
  release it is installed as; and a version with a suffix is a pre-release whatever the feed says. On Windows
  the `Updates` folder admits changes from SYSTEM, the Administrators and the service alone, and the update
  helper runs the installer, and the rollback image, only with the SHA-256 the server read when it checked
  them, through a handle it holds until they have finished. **A server from 6.2.28 to 6.3.3 still looks at GitHub.** Under `[Settings]` in its `hMailServer.ini`,
  add `UpdateFeedUrl=https://updates.progressiverobot.com/releases.json`, then restart the service, and its
  update check works again. Its live update can then install the re-hosted releases up to 6.3.3. The first
  release signed on GitLab will be refused by that server's live update, which trusts only GitHub's signer:
  install that one release by hand, and until then keep `UpdateAutoDownload` off, or the server downloads the
  installer at every check and refuses it ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **Upgrades without a booked window**: a drain closes the listeners before it waits, the live update drains
  first, and with `UpgradeOnStart=1` in `[Database]` (a new bootstrap key, 0 by default) the service applies
  the schema steps itself as it starts ([Upgrading.md](hmailserver/docs/Upgrading.md)). A rolled-back update's
  outcome is reported as soon as the service is back.
- **Backups**: encrypted with AES-256, the passphrase never inside the archive (schema 6059); one mailbox or one
  message restored from an archive ([Backup.md](hmailserver/docs/Backup.md)).
- **Monitoring**: syslog (RFC 5424, or the journal under systemd; off) ([Syslog.md](hmailserver/docs/Syslog.md));
  per-domain reports ([Reports.md](hmailserver/docs/Reports.md)) with spam, virus and storage figures per domain
  (schema 6062); the DMARC and TLS-RPT reports a domain receives, read and charted (schema 6070)
  ([DmarcReports.md](hmailserver/docs/DmarcReports.md)); outbound webhooks for five mail events (schema 6051)
  ([Webhooks.md](hmailserver/docs/Webhooks.md)); a daily check of the server's own addresses against block lists
  (schema 6057), on in a new installation and off after an upgrade
  ([ReputationWatch.md](hmailserver/docs/ReputationWatch.md)).
- **Linux**: the live update applies on Linux through a root helper outside the server's unit; a Helm chart and a
  Compose file beside the container image; `hmailserver --set-setting`, `--sample-domain` and
  `--apply-environment`.
- **Windows packages**: `winget` and Chocolatey manifests rendered from the installer a release actually
  published ([PackageManagers.md](hmailserver/docs/PackageManagers.md)); they are not in the public repositories
  yet.
- **HTTP**: gzip and deflate on both listeners for the answers that hold no secret
  ([ResponseCompression.md](hmailserver/docs/ResponseCompression.md)).

### Build and tests

- The native server's line coverage was measured for the first time: 73.48% over the whole Windows regression
  suite. The three MIME fuzz harnesses gained a nightly job, and the Linux regression suite was split in two
  halves; both ran on GitHub Actions until 18 September 2026, and no CI runs on GitLab yet.
- Twenty-one new checkers under `build/` hold the rules the batches added, among them `check-account-stores.py`
  (what a backup must carry), `check-com-error-info.py` (COM refusals a caller can read),
  `check-collection-iteration.py` (locked iteration over shared collections), `check-com-dispids.py`,
  `check-openapi-blocks.py` and the audit and role choke-point checks.
- The three GnuPG proofs of the gateway-encryption and zero-access fixtures run on Windows for the first time:
  they had handed Git for Windows' gpg a home directory it could not read.

---

## [6.3.3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.3) - 2026-09-15

Large outbound messages sent with CHUNKING no longer stall, databases older than 6.3.2's schema upgrade again,
the webmail was rebuilt to the shape of Gmail and Outlook.com, the account's address book is served over CardDAV,
and the browser Control Deck was measured against the desktop Control Panel and brought close to it.

**Delivery**

- An outbound BDAT chunk larger than one 60,000-byte send buffer no longer stalls (GitHub issue 261): every
  message over 60,000 bytes to a server advertising CHUNKING had stalled since outbound BDAT arrived.
- The full-text indexer no longer reports an error for a message deleted before its terms were saved.

**The webmail**

- Rebuilt to the shape of Gmail and Outlook.com: one search box, a navigation column, a reading pane right, below
  or off, Undo after every move, a compose window that docks or pops out, and replies written under the message.
- Pin (`$Pinned`), mute (`$Muted`), block a sender, a right-click menu and the inbox in tabs, Gmail's or
  Outlook's way, decided by the server from the headers; follow-up flags with due dates, nudges and quick steps.
- The last ten searches offered again, contact names completing to `from:`, and fourteen more search operators.

**CardDAV**

- The account's address book on phones and desktop clients (RFC 6352) on the web-services HTTPS listener, with
  the card a client sends stored as sent (schema 6040) and the IMAP logon's lockout and auto-ban.

**The Control Deck and REST**

- `build/check-deck-parity.py` measures the desktop Control Panel against REST and the Deck; of 330 properties,
  328 became writable over REST and 322 reachable from a Deck view on 14 September.
- New write routes for the anti-virus settings, account and domain fields, lists, the anti-spam lists, incoming
  relays and, under an account's address, its app passwords, folder permissions and messages.
- A domain's relay password could not be saved on any Windows installation, because a DPAPI envelope is longer
  than the column; schema 6039 widens it.

**Security, from the release's own review**

- The REST routes for the `[Settings]` section of `hMailServer.ini` answered to any API key, so a read-only key
  could read its secrets; they answer to the administrator password only. A key restricted to a domain could
  lift that domain's limits; it is refused them with 403. CardDAV's request handling is bounded.

**Schema:** 6038 to 6040 (6039 widens a domain's relay-password column; 6040 adds the CardDAV tables).

**Upgrading**

- 6.3.2 could not upgrade a database older than its own schema (GitHub issue 263): every upgrade from 5.x or
  6.2.x stopped with "The server has not loaded its configuration". From anything older than 6.3.1, take 6.3.3;
  nothing in the database was changed by a failed 6.3.2 attempt.
- A live update started from an open Control Panel failed with the installer's exit code 5. It is fixed, but the
  helper that runs an update is the one already installed, so on 6.3.1 or 6.3.2 update by hand once: close the
  Control Panel and run this installer.
- On PostgreSQL a backslash is now stored as one backslash; values already stored doubled stay as they are, so
  edit and save them once.
- Until you upgrade, `OutboundChunking=0` is the workaround for the BDAT stall.

Full notes: [the 6.3.3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.3).

## [6.3.2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.2) - 2026-09-13

The webmail became a mail client, Kerberos single sign-on arrived on all three protocols, an auto-ban reaches the
operating system's firewall, the Linux packages install on the next distribution release, and thirty-seven
binaries left the repository.

**The webmail**

- Reading: conversations by thread; Archive, Junk and Trash on one key each, with a move into or out of Junk
  teaching the spam filter as an IMAP `MOVE` does; a search syntax with operators; labels as IMAP keywords
  (schema 6036) that every IMAP client sees; image attachments shown and PDFs previewed; warnings for a link
  whose text names another site and for a sender named like a contact.
- Writing: a formatting bar; a From picker offering exactly what SMTP submission would accept; signatures,
  templates, undo send, read receipts (RFC 8098) and one-click unsubscribe (RFC 8058) to public addresses only;
  send later and snooze (schema 6034); an address book collected from what the account sends (schema 6032);
  large files sent as links that expire (schema 6035).
- S/MIME in the browser with the Web Crypto API, the private key wrapped under a key derived from the account
  password (schema 6037), which the server cannot open.
- Settings, Away, Filters (a rules editor that writes Sieve), Contacts, Storage, Files and Security pages; the
  seventeen Control Panel languages plus Greek, Korean and European Portuguese; installs as an app and reads
  the inbox offline; branding per server or domain.

**Authentication**

- SASL GSSAPI (RFC 4752) on SMTP, IMAP and POP3 on Windows, off unless `GssapiEnabled=1`.
- `AutoBanFirewall=1` puts a banned address into Windows Defender Firewall; on Linux a packaged hook keeps an
  nftables set, with a fail2ban filter and jail; `AutoBanCommand` and `AutoBanNeverBan` beside it. All off.
- Making an app password requires the account's own password, and never accepts an app password for it.
- A SURBL server's answers are judged against an expected result (schema 6038); without one, the codes with
  which Spamhaus refuses a query are no longer taken as listings (GitHub discussion 167).

**Linux**

- Boost is linked statically, so the `.deb` installs on Ubuntu 26.04.
- A container image from every tag, whose entrypoint writes the database section, the REST listener and the
  administrator password from the environment and creates or upgrades the database.

**Database**

- The 6029 to 6030 step sweeps parents before children, fixing the orphan-sweep defect 6.3.1 named, in all four
  backends.
- Every statement in a MySQL script is followed by a blank line, and `build/check-sql-separators.py` keeps it so.

**Build**

- Thirty-seven of the forty committed binaries left git; `build/get-installer-binaries.ps1` fetches them from
  the `build-inputs-1` release against the SHA-256 in the manifest, and the COM wrapper is generated.
- A server holding a database whose schema it refused (HM5011) answers COM with the refusal instead of an access
  violation.

**Schema:** 6031 to 6038 (6032 to 6037: contacts, account preferences, scheduled sends and snoozes, files sent as
links, message keywords, S/MIME keys; 6038: the SURBL expected result).

**Known afterwards:** 6.3.2 could not upgrade a database whose schema was older than its own (GitHub issue 263),
and its live update from an open Control Panel failed; 6.3.3 fixes both.

Full notes: [the 6.3.2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.2).

## [6.3.1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.1) - 2026-09-11

The first release whose Windows installer carries an Authenticode signature. Nothing in the server changes: the
compiled server differs from 6.3.0 by its version stamp and one comment line.

**Signing**

- The installer is signed with Azure Artifact Signing against a certificate issued to Progressive Robot Ltd, so
  the elevation prompt names the publisher and a policy that refuses unsigned binaries no longer refuses it.
  SmartScreen still warns until reputation accumulates.
- Only the Windows installer is Authenticode-signed; the Linux packages have no such signature, and the
  Sigstore bundle beside every asset is unchanged.
- `UpdateRequireAuthenticode=1`, in the server's update settings since 6.2.28, becomes usable on Windows,
  because the installer it downloads now carries a signature.

**The release path**

- The release workflows were corrected before the certificate was first used: the signing gate checks all six
  of its settings, every attached asset is verified as it stands on the release, both SBOMs are required before
  anything is signed, an RFC 3161 countersignature is required, and runs for one tag are serialised.

**Documentation**

- The README offers the Linux packages on its download line; two files that named a version 6.2.29, which never
  existed, now say 6.3.0.

**Schema:** 6031, unchanged.

**Known, and fixed in 6.3.2:** the orphan sweep of the 6029 to 6030 step ran children before parents, so an
upgrade from below 6030 of a database that already held orphaned rows could refuse its own foreign keys. It
failed loudly and rolled back.

Full notes: [the 6.3.1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.1).

## [6.3.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.0) - 2026-09-10

hMailServer runs on Linux: a mail server that had been Windows-only since 2002 builds, installs, runs and delivers
mail on x86-64 and AArch64 Linux from the same source tree, with the Windows build unchanged in what it compiles
and how it behaves. Because a Linux server has no Control Panel and no COM, the REST API gained a large write
surface and the browser Control Deck drives most of it.

**Linux**

- Every core translation unit compiles under clang on x86-64 and AArch64, and the whole core builds and links
  with GCC on x86-64; `build/linux-tu-census.sh` counts them file by file.
- A `.deb` and an `.rpm` for both architectures, a `PKGBUILD` for Arch and an AppImage, each with a systemd unit
  that runs the server as its own user, a logrotate rule, the configuration under `/etc/hmailserver` and the
  Control Deck.
- `hmailserver --create-database`, `--upgrade-database`, `--set-admin-password` and `--check-config`.
- Proven against PostgreSQL 18 and MariaDB 11.8 from schema creation to a message delivered and read back over
  IMAP; directory authentication against slapd over StartTLS and LDAPS; stored secrets under AES-256-GCM with a
  key file the installation owns, where Windows uses DPAPI.
- The same commit built twice on the hosted runners gave byte-identical binaries.

**REST writes what only COM could write**

- `PUT /api/v1/settings` with its anti-spam and logging groups (108 settings, each through the Control Panel's
  own setter), global rules, SMTP routes, aliases, accounts with their password and administrator level,
  certificates and the listeners that bind them, and `POST /api/v1/server/reinitialize`.
- Refused to a read-only key and to a key issued for other domains; the OpenAPI document is valid JSON for the
  first time.
- Not yet written over REST: a domain's properties beyond its active flag and postmaster, DKIM signing, the
  per-domain size limits, the domain signature and a per-domain relay host.

**The Control Deck**

- Editors generated from the server's own OpenAPI document; rules, routes, certificates and listeners created,
  edited and deleted; accounts created and deleted; the services restarted in place.
- It no longer keeps the administrator password: `POST /api/v1/session` exchanges it once for an `HttpOnly`,
  `SameSite=Strict` session cookie that ends when the password changes, and a write must carry
  `X-Requested-With`.

**The webmail**

- The portal at `/portal` became a webmail: addresses for folders and messages, the Control Deck's look, new
  mail noticed without a reload, folders created, renamed and deleted, and inline images shown without loosening
  the frame's sandbox. `build/check-portal-script.py` runs the page's script in CI.

**Under the surface**

- Non-ASCII folder names and non-ASCII command lines are handled correctly on Linux.
- A TLS key-exchange group list OpenSSL rejects is reported once, not for every listener and delivery.
- The administrator's user name is no longer translated in the Control Panel's sign-in box (GitHub issues 156
  and 177); an installation-paths diagnostic lists every configured directory (GitHub issue 158).
- A Linux regression suite, `hmailserver/test/LinuxRegressionTests`, compiles 260 of the suite's own files in
  place and runs them against the Linux server.

**Schema:** 6031, unchanged.

**Upgrading**

- Windows: run the installer over the existing installation. Nothing new is on by default; the Control Deck and
  the portal need `RestApiPort`.
- Linux, first installation: install the `.deb` or `.rpm`, then `hmailserver --create-database`,
  `hmailserver --set-admin-password` and `systemctl enable --now hmailserver`.
- Moving a Windows installation to Linux is not a supported migration.

**Known and unfixed:** the installer is not Authenticode-signed (6.3.1 is the first that is); per-domain DKIM
cannot be configured on Linux; the Linux regression run skips what no route reaches yet.

Full notes: [the 6.3.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.0).

## [6.2.28](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.28) - 2026-09-08

The largest release of the fork so far: the server can update itself, an account can use its mailbox in a
browser, and the Control Panel speaks seventeen languages, with a real HTTP/1.1 server under the REST API and the
web services. Everything new is off until it is turned on.

**Live update**

- `UpdateCheckEnabled` (0 by default) reads the project's release feed every `UpdateCheckHours` (24); fetching
  and applying an installer are two further opt-in steps.
- What is applied is verified against the release's Sigstore bundle, which cannot be turned off; the settings
  `UpdateTrustRootsFile`, `UpdateSigningIdentity`, `UpdateSigningIssuer`, `UpdateSourceRepository` and
  `UpdateLogPublicKeyFile` point it at a private Sigstore instance.
- `hMailServer.Updater.exe` runs the installer with the service stopped, waits `UpdateServiceWaitSeconds` (180)
  and reinstalls the previous version if the service does not come back; `UpdateBackupBeforeApply=1` refuses an
  unattended apply without a successful backup.
- `HttpProxy=host:port` sends every web request the server makes as a client through a forward proxy.

**The mailbox in a browser**

- `/portal` on the REST listener, and `/api/v1/me` behind it, answer to an account's own credentials only: read,
  search, flag, move and delete; send with attachments, drafts, reply, forward and Bcc; the account's quarantine,
  forwarding, signature, vacation reply, Sieve script and password.

**Under it**

- The REST API and the web services run on `HttpServer`: HTTP/1.1 on Boost.Asio with keep-alive, chunked bodies
  and header and body limits.
- IMAP `COMPRESS=DEFLATE` (RFC 4978), behind `IMAPCompressionEnabled`.

**The Control Panel and the Control Deck**

- The Control Panel in Czech, Danish, German, Spanish, Finnish, French, Italian, Japanese, Norwegian bokmål,
  Dutch, Polish, Brazilian Portuguese, Russian, Swedish, Turkish, Ukrainian and Simplified Chinese, beside
  English: 3,376 texts, chosen in the connect card and remembered per user.
- The Control Deck reads the settings, the log files, the certificates and the global rules.

**Fixed**

- A masked password was typed backwards from the second character (GitHub issue 156).
- Saving the SSL/TLS page dropped the post-quantum key exchange groups, and saving the Scripting page took every
  COM object from every script.

**Schema:** 6031, unchanged.

**Upgrading:** this is the first release that carries the live update, so the upgrade to it is manual.
`UpdateCheckEnabled`, `RestApiPort`, `IMAPCompressionEnabled` and `HttpProxy` are all opt-in; a server upgraded
without touching its settings behaves as it did on 6.2.27.

**Known and unfixed:** the Control Deck reads but does not write, and holds the administrator password in
`sessionStorage` while it is open; the regression suite runs on Windows only.

Full notes: [the 6.2.28 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.28).

## [6.2.27](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.27) - 2026-09-07

Two roadmap items open since the roadmap was written: the validating resolver proves a missing DS record missing
rather than assuming it, and an external IMAP account can mirror every folder of a remote mailbox.

**Security**

- A DS query answered with nothing is no longer taken as an unsigned delegation: the resolver requires the
  parent's signed NSEC or NSEC3 proof (RFC 4035 section 5.2, RFC 5155 section 8), and a delegation without one
  under a signed parent is Bogus. `Diagnostics.DnssecChainStatus` reports a chain.

**Mail flow and migration**

- `FetchAccount.MirrorFolders` (schema 6031) mirrors every folder of a remote IMAP mailbox byte for byte, with
  flags and internal dates, without delivering anything through the filters; *days to keep messages* 0 makes it
  a move.
- The Import Tool reads a Maildir, with the INBOX, every Maildir++ folder and the flags in the file names.
- `hmailserver/docs/Migration.md` describes each route into this server and what it keeps.
- A message imported into a folder a client has open now appears at once.

**Schema:** 6030 to 6031 (one column on `hm_fetchaccounts`).

Full notes: [the 6.2.27 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.27).

## [6.2.26](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.26) - 2026-09-06

The release to take if you run the built-in SQL Server Compact database. Nothing else in the server changes.

**Fixed**

- Every SQL Server Compact upgrade through schema 6030 was reported as failed after it had succeeded, and the
  check that gave the false verdict took the service down with it (GitHub issue 114). The four verification
  probes for the foreign keys crashed the Compact Edition provider; they are rewritten so that they run on every
  backend.
- `build/check-db-scripts.ps1` now executes every probe against a freshly created database, with a negative
  control, and DBUpdater's message gives the backend's own words.

**Schema:** 6030, unchanged.

**Upgrading:** if the 6.2.25 installer reported a failed upgrade, the database is already at 6030 with its
foreign keys, and this installer finds nothing left to do.

Full notes: [the 6.2.26 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.26).

## [6.2.25](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.25) - 2026-09-06

The release to take if you use the built-in ACME client, and sixty commits of new work besides.

**The fix to upgrade for**

- ACME issuance and renewal ended the process (GitHub issue 93): two calls handed the OpenSSL DLL a C runtime
  `FILE*`, and OpenSSL answered the missing `OPENSSL_Applink` by terminating the process. Both calls use
  OpenSSL's own file I/O, the certificate is deployed before the DANE TLSA line is computed, and a certificate
  6.2.24 issued before it died is deployed at the first ACME check.
- Two restarts of the servers at once could end in an access violation; restarts run one after the other.

**Mail flow**

- External accounts over IMAP (`FetchAccount.ServerType` 1), the INBOX only.
- Message retention per domain and per account (schema 6027); an archive index with legal hold (schema 6029).
- Outbound delivery pipelines the envelope and sends the message as one BDAT chunk when the remote advertises
  PIPELINING and CHUNKING (`OutboundPipelining`, `OutboundChunking`, both on).
- Delivery hard links (`DeliveryHardLinks`, off); `SEARCH BODY` and `TEXT` read text attachments.
- A Message-ID is added only for submissions (upstream PR 552); a stranded fetch account is released
  (upstream PR 603).

**IMAP**

- Every session numbers its selected folder for itself (upstream PR 602); sessions stop sharing unguarded state
  (upstream PRs 580 and 566); default special-use folders at account creation (schema 6026, off); one
  authorisation choke point for folder access.

**Security and authentication**

- SASL EXTERNAL with client certificates; the AUTH line limit raised to 12,288 octets.
- OAuth2 keys from the provider's JWK Set and token introspection; scrypt (`PreferredHashAlgorithm=7`); the
  password-hash work factor as settings (upstream PR 554); tarpits on failed logons and stranger recipients.
- The administrator's second factor is enforced by the server; `SmtpAuthenticatedSenderCheck` and
  `ScriptAllowedObjects`; the Apple configuration profile served over HTTPS only.

**Administration and databases**

- The REST API reads what was COM-only; a week of metric history (schema 6028); every backup proves its mail
  can be restored (`BackupVerifyRestore`, on); Control Panel accessibility for High Contrast, mnemonics and
  screen readers.
- TLS to PostgreSQL and MS SQL as `[Database]` settings; a plaintext MySQL only by choice (upstream PR 559);
  seventeen foreign keys (schema 6030).
- OpenSSL 4.0.2 and Boost 1.92.0; twelve upstream ports in all.

**Schema:** 6025 to 6030 (6026 the special-use folders switch, 6027 retention, 6028 metric history, 6029 the
archive index, 6030 seventeen foreign keys, which first removes orphaned rows).

**Upgrading**

- The Apple `.mobileconfig` profile is served over HTTPS only; a Message-ID is no longer added to relayed mail;
  the four HTTPS clients require TLS 1.2 or later; AUTH lines may be 12,288 octets; IMAP sequence numbers are
  stable within a session; `Account.DeleteMessages` keeps a designated special-use folder.
- Every new setting is off or inert except `OutboundPipelining`, `OutboundChunking` and `BackupVerifyRestore`.
- COM clients compiled against the 6.2.24 type library keep working.

Full notes: [the 6.2.25 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.25).

## [6.2.24](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.24) - 2026-09-04

The first stable release since 6.2.21: the code of 6.2.23 Alpha 2, cut the same day, plus NuGet lock files for the
.NET tools and the CI changes that hold every restore to them. It brings everything the 6.2.22 and 6.2.23
pre-releases introduced to a 6.2.21 installation at once.

**Since 6.2.21, in short**

- Mail that was silently lost is not any more: a delegated `APPEND`, `COPY` or `MOVE`, Thunderbird's Sent copies
  with `UTF8=ACCEPT` (GitHub issue 53), a kept-open log never flushed (GitHub issue 33), and an ACME client that
  had never worked against real Let's Encrypt (GitHub issue 34).
- Anti-spam bypasses closed: a forged header steering a per-account override, a HELO literal choosing the
  address the IP tests ran against, and a transaction opened without a greeting.
- Shared and delegated mailboxes, full-text search, RFC 3464 bounces, BINARYMIME, per-account spam thresholds, a
  sender blacklist, domain-wide out-of-office, list moderation, the Windows Event Log, OTLP metrics and logs, the
  PROXY protocol, an HTTP filter hook, eleven IMAP extensions, Microsoft 365 XOAUTH2, app passwords, TOTP, a
  quarantine, password history, message trace, per-domain relay and a disk-space floor.
- Control Flow Guard, a reproducible build, signed release tags, SBOMs and Sigstore bundles.

**Schema:** 6025, unchanged since Alpha 1.

**Upgrading from 6.2.21**

- The schema moves from 6011 to 6025 in fourteen steps, one way. The 6024 to 6025 step rewrites
  `hm_messages.messageflags` on MS SQL, SQL Server Compact and MySQL, which can take long on a large database;
  PostgreSQL is unaffected.
- Without a switch: `MAIL FROM` or `AUTH` straight after STARTTLS without a fresh `EHLO` gets `503`; relayed and
  fetched mail is tested against the address the relay observed; mail is refused with a temporary error below
  100 MB free (`MinimumFreeDiskSpaceMB`); serious errors also go to the Windows Event Log; existing out-of-office
  replies honour the RFC 3834 suppressions; old `hm_imapexpunged` rows are pruned once at first start.
- Early-bound COM clients compiled against a 6.2.22 pre-release interop assembly must be recompiled; late-bound
  scripts and anything compiled against 6.2.21 are unaffected.

**Known limitations:** statement timeouts have been exercised only on SQL Server Compact; the weekly C++ CodeQL
analysis was not running; SCRAM-SHA-1 is not offered (GitHub issue 49).

Full notes: [the 6.2.24 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.24).

## [6.2.23-alpha2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha2) - 2026-09-04 - pre-release

A small, deliberate pre-release: one reported defect fixed, seven fixes taken from the original project, which had
become active again, and hardening that had been written down as missing.

**Fixed**

- Thunderbird's Sent copies were refused once `UTF8=ACCEPT` was enabled (GitHub issue 53): the RFC 6855
  `UTF8 (~{n}` wrapper on `APPEND` is now recognised.
- From upstream: the IP-based spam tests of relayed and fetched mail ran against the HELO literal the client
  chose rather than the address the receiving server observed; they use the observed address now.
- From upstream: `RSET` before `EHLO`, or STARTTLS without a fresh `EHLO`, opened a transaction with no greeting.
  A `MAIL FROM` straight after the TLS handshake now gets `503 Bad sequence of commands`, as RFC 3207 requires.
- A reverse-DNS answer with a trailing dot no longer empties the PTR result; null dereferences in the script host
  and the IMAP notification client.

**Hardening**

- Control Flow Guard on `hMailServer.exe` (GitHub issue 45); the first SSH-signed release tag (GitHub issue 46);
  an unattended install whose database step fails now fails; `IMAPFolder.Delete` reports a refused deletion.

**Repository**

- `hmailserver/docs/RegressionEnvironment.md` (GitHub issue 48), a DCO check on pull requests (GitHub issue 47),
  the C# tree `dotnet format` clean (GitHub issue 44), and SCRAM-SHA-1 closed as not planned (GitHub issue 49).

**Schema:** 6025, unchanged. Everything in Alpha 1's notes still applies to an installation coming from 6.2.21.

Full notes: [the 6.2.23 Alpha 2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha2).

## [6.2.23-alpha1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha1) - 2026-08-21 - pre-release

A large feature release cut as an alpha, not for production: 83 commits since 6.2.22-pre6. It also fixes one path
that was silently losing mail.

**New**

- Shared and delegated IMAP mailboxes under `#Users.owner@domain.folder`, reachable when the owner's folder
  carries an RFC 4314 ACL granting the caller rights.
- A full-text search index that narrows `SEARCH BODY` and `TEXT` without changing results
  (`IndexerFullText=1` with message indexing, both off).
- RFC 3464 machine-readable bounces; BINARYMIME (RFC 3030), with relay of a binary message refused rather than
  converted.
- PROXY protocol v1 and v2 and Postfix `XCLIENT`, trusted only from listed peers; an external HTTP filtering hook
  (`FilterHookUrl`) for an engine such as rspamd.
- A sender blacklist; per-account spam thresholds; a domain-wide out-of-office reply; distribution-list
  moderation and a per-list bounce address.
- Windows Event Log integration; OTLP metrics and logs; W3C trace context ingestion; IPv6 on the REST, metrics
  and web-services listeners; five REST routes and `GET /api/v1/openapi.json`; DMARC aggregate reports in the
  RFC 9990 form behind `DmarcRptSchemaVersion=2`.
- Two COM methods for data-subject requests, `Account.ExportMessages` and `Utilities.EraseAddressTraces`, and
  `docs/WarmStandby.md`.

**Fixed**

- Local delivery could lose a message silently when the account-level copy could not be written; the sender is
  now told.
- Two database backends could wait forever (`DatabaseStatementTimeout`); an unattended upgrade could hang at an
  invisible password dialog; the white-list cache never cached.

**Schema:** 6022 to 6025 (6023 the full-text index tables, 6024 the domain out-of-office columns and
`hm_blocked_senders`, 6025 `hm_messages.messageflags` widened, per-account spam settings and list moderation).

**Upgrading**

- From 6.2.21 the schema moves from 6011 to 6025 in fourteen steps. The upgrade is one-way; take a database and
  data-directory backup first. The 6024 to 6025 step rewrites the largest table.
- New defaults: mail refused below 100 MB free, errors written to the Windows Application log, statement timeouts
  of 30 seconds on PostgreSQL and MySQL, QRESYNC tombstones pruned to 5,000 per mailbox, existing out-of-office
  replies suppressed for lists and automatic mail, bounces as `multipart/report`, and `PreferredHashAlgorithm`
  below SHA-256 refused.
- With IMAP ACL enforcement on, which is the default, any account holder whose client speaks `SETACL` can
  grant another local account access to their own folders, with no administrator step.
- A COM vtable layout was repaired to match 6.2.21; clients early-bound against a 6.2.22 pre-release must be
  recompiled, and a hand-copied install must re-register with elevation.

Full notes: [the 6.2.23 Alpha 1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha1).

## [6.2.22-pre6](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre6) - 2026-08-20 - pre-release

A pre-alpha. The same build as pre5 (build 30, commit `bf0d38a`), republished with its SBOMs and a cosign bundle
for every asset, which pre5 could not carry. Its theme is standards catching up with reality.

**DMARC and algorithms**

- DMARCbis: the DNS tree walk (RFC 9989 section 4.10) decides organisational domains, with the Public Suffix List
  as the `DmarcTreeWalkEnabled=0` escape hatch; the `np=` tag for subdomains that do not exist; aggregate reports
  name the DKIM selector.
- DKIM `rsa-sha1` is refused and a domain configured to sign with it signs `rsa-sha256` (`DkimAcceptSha1=1`
  restores both); keys below 1,024 bits are refused; SPF void lookups are capped at two (`SpfVoidLookupLimit`).

**Certificates**

- The ACME renewal window is two thirds of the certificate's lifetime, ready for shorter lifetimes, and the CA's
  ARI window (RFC 9773) is consulted.

**Mail flow and operations**

- A full mailbox is refused at `RCPT` with `452 4.2.2` (`RejectFullMailboxAtRcpt=0` restores the old way); quota
  warnings at `QuotaWarningPercent` (90) (schema 6022).
- A relay host per sending domain (schema 6021; GitHub discussion 31).
- A queryable message trace (schema 6020, off); archive retention (`ArchiveRetentionDays`, 0); per-domain metric
  labels (`MetricsPerDomainEnabled`, off).

**Schema:** 6019 to 6022.

**Upgrading:** from pre3, DBUpdater moves the schema from 6019 to 6022. Not for a production server.

Full notes: [the 6.2.22-pre6 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre6).

## [6.2.22-pre5](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.2.22-pre5) - 2026-08-20 (the tag's date) - tag only

Superseded by 6.2.22-pre6, which is the same commit: pre5 went out carrying only the installer, its release was
deleted, and the tag remains.

## [6.2.22-pre4](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.2.22-pre4) - 2026-08-20 (the tag's date) - tag only

Withdrawn: its installer could not create its own database on SQL Server Compact. The release was deleted and
the tag remains; do not build from it. The 6.2.22-pre6 notes explain both.

## [6.2.22-pre3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre3) - 2026-08-19 - pre-release

The third pre-release of 6.2.22, whose theme is authentication: per-account two-factor authentication, and the
chain it needed first.

**Authentication**

- App passwords (schema 6016): a per-account credential, revocable on its own, for IMAP, POP3 and SMTP.
- Per-account TOTP (schema 6017): once enrolled, the account password stops being a mailbox credential and an
  app password becomes the only one.
- A password policy the server enforces where a password is chosen; password expiry and history (schema 6019).
  The server had no self-service password change then, so an expired password was reset by an administrator.
- ES256 OAuth2 tokens verify.

**Anti-spam, IMAP and POP3**

- An administrator-reviewable quarantine (schema 6018), off by default.
- `FETCH BINARY` on a composite section, a known limitation of pre2, is fixed; POP3 `EXPIRE` and `LOGIN-DELAY`
  (RFC 2449).

**Schema:** 6015 to 6019. **Upgrading:** from pre2, DBUpdater runs the four steps.

Full notes: [the 6.2.22-pre3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre3).

## [6.2.22-pre2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre2) - 2026-08-18 - pre-release

The second pre-release of 6.2.22: the server now sends the reports it has always asked others for.

**Reporting**

- DMARC aggregate reports (RFC 7489 `rua=`), inert until `DmarcRptFromAddress` is set, with the external
  destination check enforced; no forensic reports.
- TLS-RPT reporting made verifiable with `Utilities.SendTlsRptReports`.

**Authentication and anti-virus**

- Per-account lockout (`AccountLockoutThreshold`, off), which a distributed attack cannot spread around.
- POP3 `AUTH-RESP-CODE` (RFC 3206), `PIPELINING` and `IMPLEMENTATION` (RFC 2449).
- `AVFailAction`: a message that could not be scanned may be held and returned instead of delivered.

**IMAP**

- `BINARY` (RFC 3516) and `OBJECTID` (RFC 8474, schema 6015).

**Schema:** 6014 to 6015. **Known limitation:** `FETCH BINARY` on a composite section returned an empty literal
(fixed in pre3).

Full notes: [the 6.2.22-pre2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre2).

## [6.2.22-pre1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre1) - 2026-08-16 - pre-release

The first pre-release of 6.2.22: a Sieve wave, the Microsoft 365 OAuth switch-over, IMAP extensions, and two
reported bugs fixed. (The notes speak of two schema steps; from 6.2.21 there are three, 6012 to 6014.)

**Fixed**

- The built-in ACME client had never worked against a real Let's Encrypt (GitHub issue 34): the challenge
  locator missed the pretty-printed JSON. Failure messages now name the domain and quote the CA's response.
- With *Keep log files open*, log lines could stay invisible for forty minutes (GitHub issue 33); every line is
  now handed to the operating system as it is written.

**New**

- Microsoft 365 XOAUTH2 for outbound relay and POP3 collection, ahead of Basic authentication being switched
  off for SMTP relay.
- Sieve: variables, editheader, duplicate, reject and ereject, include, enotify, date and currentdate, spamtest,
  ihave and environment, mailboxexists and `:regex`; ManageSieve response codes and `RENAMESCRIPT`.
- Ten IMAP extensions - APPENDLIMIT, LITERAL-, LIST-STATUS, UNAUTHENTICATE, QUOTA, PREVIEW, MULTIAPPEND,
  REPLACE, SAVEDATE (schema 6013) and METADATA (schema 6014) - and SIZE on the SMTP client, which now opens every
  delivery with `EHLO`.
- An auto-reply start date and rule criteria up to 2,000 characters (schema 6012).

**Schema:** 6011 to 6014. **Upgrading:** databases upgrade automatically; take a backup before installing a
pre-release.

Full notes: [the 6.2.22-pre1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre1).

## [6.2.21](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.21) - 2026-08-15

The fix for inbound mail from a Postfix relay that could hang for ever, found by running a real Postfix.

**SMTP**

- GitHub discussion 18: Postfix sends the end of the body, the terminating dot and `QUIT` in one segment, and
  every end-of-data check had looked only at the end of the buffer. The standard terminator is now found
  anywhere, and what follows it is handed to the command parser; bare-LF terminators are still recognised only
  at the very end, the CVE-2023-51764 smuggling rule. The Postfix test rig is in `hmailserver/test/PostfixBench`.
- Dot transparency across buffer boundaries, in both directions, which could silently drop a dot or truncate a
  message.
- A malformed DNS answer is retried once over TCP.

**Sieve**

- Sieve scripts move with domain and account renames and are removed with deletes, so a recreated address no
  longer inherits the previous holder's filter; the Control Panel writes a script only after the save succeeds.
- The `body` test (RFC 5173) with `:text`, `:content` and `:raw`.

**IMAP**

- `SORT` with an empty criteria list spun a connection thread for ever; it is rejected. The message cache could
  never evict and only counted upwards; fixed.

**Control Panel**

- The MX query tool asks the server's own resolver through `Utilities.ResolveMXRecords` (GitHub issue 29).
- Seven things the retired Administrator could do are restored, among them pausing the server and renaming a
  domain or an account; the Welcome page's icons are centred (GitHub issue 30, reported by `@helptiger`).

**Schema:** 6011, unchanged.

**Known issues:** equal-preference MX records are not randomised; one-click unsubscribe (RFC 8058) is not
implemented.

Full notes: [the 6.2.21 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.21).

## [6.2.20](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.20) - 2026-08-15

The fix for fresh installations of 6.2.19, which failed or hung. Upgrades of existing installations were never
affected.

**Fixed**

- Database creation failed: statements added to the create scripts were separated by a single newline, and SQL
  Server Compact executes one statement per command. The create scripts of all backends are fixed.
- A silent install hung on a hidden dialog; every installer dialog now takes its default button under
  `/SUPPRESSMSGBOXES`, and the installer exits with a code a script can read.

**New**

- IMAP `THREAD` (RFC 5256) with `ORDEREDSUBJECT` and `REFERENCES`.

**Schema:** 6011, unchanged.

Full notes: [the 6.2.20 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.20).

## [6.2.19](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.19) - 2026-08-15

**Fresh installations of 6.2.19 fail; use 6.2.20.** Upgrades are unaffected. The release fixes the custom DNS
server that 6.2.16 to 6.2.18 broke, and adds directory provisioning.

**Fixed**

- With a custom DNS server configured, 6.2.16, 6.2.17 and 6.2.18 could resolve nothing through it (GitHub issue
  25): MX, DNSBL, SPF and SURBL lookups all failed. Pinned by a regression test.
- A UID could be handed out twice; a colon in an account address is refused; a session could outlive its
  `io_context`; a status card could report failing backups on a healthy server; a certificate handle leaked.

**New**

- An LDAP directory as an account source (`Settings.PreviewDirectorySync`, `ApplyDirectorySync`), with a Control
  Panel page: opt-in per domain, and nothing is ever deleted.
- Sieve `imap4flags` now reaches the stored message.
- The `[Settings]` values move into the database so they can be administered remotely, with the file still
  winning where both carry a value.

**Schema:** 6005 to 6011.

**Known issue:** GitHub issue 26, a partial `FETCH BODY[]<offset.length>`, does not reproduce here.

Full notes: [the 6.2.19 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.19).

## [6.2.18](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.18) - 2026-08-12

Finishes on the delivery side what 6.2.17 began, and stops a database outage bouncing mail.

**Fixed**

- A recipient lookup that fails because the database did not answer now returns `451` rather than `550`, so a
  database briefly locked by a backup defers mail instead of bouncing it. `DBConnectionAcquireTimeout` now
  defaults to 60 seconds.
- Bounded waits (GitHub issues 23 and 24): an absolute ceiling on outbound sessions (`ClientSessionCeiling`, 30
  minutes); ClamAV on the delivery path; `DNSQueryTimeout` (10 s), `ScriptTimeout` (60 s) and
  `ExternalProcessTimeout` (300 s); pre-authentication IMAP buffering capped at 11 MB; a backup restore validates
  the archive before deleting anything.
- The first `/analyze` pass: buffers sized in bytes where characters were counted, an overrun on long paths,
  and a log rotation that tested an uninitialised value.

**Documentation**

- `Roadmap.md` rewritten after a comparison with Postfix and Dovecot, Stalwart, iRedMail, Mail-in-a-Box and
  Exchange Online.

**Schema:** 6005, unchanged.

**Correction (12 August 2026):** ARC sealing is narrower than the notes first said: relayed third-party mail is
never sealed.

Full notes: [the 6.2.18 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.18).

## [6.2.17](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.17) - 2026-08-12

Reproduces and bounds a stall after `354` on mail from a trusted incoming relay, with the stall now logged by
stage. (6.2.21 later found the defect GitHub discussion 18's reporter actually had.)

**Fixed**

- SpamAssassin's wait is bounded (`SAMaxTimeout` plus 30 seconds), after which the message is accepted without a
  verdict.
- `FinalizationTimeout` (240 seconds, 0 disables it) answers `451 4.3.1` rather than leaving the sender waiting.
- Acceptance is timed per stage and each spam test individually, and a slow stage is logged.

**Also**

- The README rewritten around what the server does; per-version history moved to the release pages.
- Regression coverage for clients that vanish mid-operation, and an installer smoke test on a clean machine.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.17 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.17).

## [6.2.16](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.16) - 2026-08-11

A patch over 6.2.15 fixing one reported Control Panel defect. No server change.

**Fixed**

- Dismissing the Ctrl+K settings palette raised an error dialog (GitHub issue 21), on nearly every search in
  6.2.14 and 6.2.15.

**Schema:** 6005, unchanged. **Not fixed:** GitHub discussion 18, tracked in GitHub issue 20.

Full notes: [the 6.2.16 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.16).

## [6.2.15](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.15) - 2026-08-11

IMAP sequence sets behave as RFC 3501 defines them, and every file the server writes has an owner.

**IMAP**

- `*` is resolved on either side of a colon and descending ranges are normalised in all four sequence-set
  parsers. Before, `UID STORE *:* +FLAGS (\Deleted)` flagged every message and `UID EXPUNGE *` deleted every
  deleted message.
- `STATUS ... (RECENT)` no longer reports the selected folder's count for every folder.

**Data safety**

- A failed restore could empty the data directory; the restore now confirms the replacement store exists before
  deleting anything.
- An unreadable message file no longer hangs an outbound connection; several paths no longer leave orphaned
  files; the account cache has a size limit.

**From the forum**

- `BOOST_USE_WINAPI_VERSION` raised to Windows 10; upstream's end-of-input rule adopted, with a truncated-`BDAT`
  defect it exposed fixed before release; DKIM header-name case under `simple` canonicalisation (upstream PR 530).

**Also**

- ManageSieve limits authentication attempts; the SMTP `DATA` path gains debug logging.
- Control Panel pages for settings that had none: Authentication, Administrative access, DNS resolver, Web
  services and autoconfiguration, message archiving, and the consistency scan's findings in Diagnostics.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.15 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.15).

## [6.2.14](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.14) - 2026-08-11

Two community reports fixed, 21 defects found by an adversarial audit of the server fixed, and settings you can
search for.

**Reported**

- Backup would not start from the Control Panel (GitHub issue 19); the log-retention setting existed but could
  not be found (GitHub issue 16).

**Mail loss, corruption and crashes**

- A failed message copy crashed delivery; IMAP `APPEND` reported success for messages never written; string SQL
  parameters on MS SQL and SQL CE were bound from freed memory; a failed transaction start leaked a pooled
  connection; IMAP `CLOSE` expunged without telling other sessions; a refused POP3 session released another's
  lock.

**Security**

- DKIM test mode (`t=y`) no longer turns a failed signature into a pass; IMAP SASL credentials are no longer
  logged; every DKIM key record at a selector is examined; MTA-STS and MX failover hold for large recipient sets;
  REST requests are time-bounded; `ES256` tokens are refused with a clear message.

**Finding settings**

- Ctrl+K searches all 227 settings by label and key; settings moved to the pages where they are used; "Advanced
  hardening" became "Advanced INI settings".

**Schema:** 6005, unchanged.

Full notes: [the 6.2.14 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.14).

## [6.2.13](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.13) - 2026-08-11

A community-feedback bug-fix release, hardening the SMTP, IMAP and SpamAssassin paths against malformed input.

**Fixed**

- A `DATA` stall when relaying from Postfix or Proxmox Mail Gateway, caused by a reverse-DNS lookup on the network
  I/O thread; `BDAT` refusals, pipelined data and a wedged connection handled.
- A malformed IMAP partial-fetch range could crash the server or read heap memory; `OnClientLogon` fires from
  every `AUTHENTICATE` mechanism.
- A malformed or empty `spamd` response could hang a session, corrupt headers or lose the message.
- DBUpdater labels versions 6002 to 6005; a failed database step reaches the installer as a visible error; a
  customised `EventHandlers.vbs` survives reinstall; Control Panel diagnostics read the right properties.

**Schema:** 6005, unchanged. **Upgrading:** in place over any earlier release; the minimum OS is Windows 10 1607
or Windows Server 2016, 64-bit.

Full notes: [the 6.2.13 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.13).

## [6.2.12](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.12) - 2026-08-10

Every C# component in the product now targets .NET 8. No server-core changes.

**Changed**

- DBSetup, DBSetupQuick, DBUpdater, the Data Directory Synchronizer and the Shared library move from .NET
  Framework 4.8.1 to .NET 8, building against a checked-in COM interop wrapper.
- The VB6 migration wizards are replaced by an Import Tool: accounts from a text file, and messages from mbox files
  through the COM API, with the old tool's documented defects fixed.
- The .NET 8 Desktop Runtime is a server prerequisite and the supported floor is Windows 10 1607.
- CI builds every C# project; the regression suite runs on NUnit 4.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.12 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.12).

## [6.2.11](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.11) - 2026-08-07

A Control Panel accessibility fix, and the regression suite run in full for the first time. No server-core
changes.

**Fixed**

- The alias list on the Domains page and the Live logs list announced their CLR type name to screen readers
  instead of their content.
- Run on the release machine for the first time, the ClamAV and SpamAssassin tests failed twice, both times on
  a defect in the test: the EICAR test now sends a real attachment, and the `spamd` stop is awaited.
- The README documents a working SpamAssassin setup on Windows and the suite's need for elevation.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.11 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.11).

## [6.2.10](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.10) - 2026-08-07 (the tag's date)

A security and housekeeping release: an authorisation defect in the COM API, two unmaintained administration
front-ends removed, and the code-quality findings taken to zero.

**Security**

- Fifteen COM methods rejected an unauthorised caller with `return false`, which is `S_OK`, so each reported
  success for a call it had refused, and five handed back uninitialised memory. They return access denied now.
  Found by running CodeQL's C++ suite by hand.
- The PHP WebAdmin, which kept the administrator password in plaintext in a PHP session, is removed.

**Removals**

- The retired Administrator (`hMailAdmin.exe`) is gone from the repository; "Administrative tools" is renamed
  "Remote administration support (registers the COM API for scripts)", which is what it does.

**Quality**

- Code-quality findings in shipped, hand-written code from 958 to 0, with real defects fixed on the way.

**Schema:** 6005, unchanged.

**Upgrading:** a drop-in over 6.2.9. Anyone who ticked "Administrative tools" for the WebAdmin or the
Administrator should use the Control Panel, which connects to a remote server directly.

Full notes: [the 6.2.10 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.10).

## [6.2.9](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.9) - 2026-08-07 (the tag's date)

A follow-up to 6.2.8. No server-core changes.

**Fixed**

- LiveCharts 2.0.5 painted the dashboard charts as a white block in the dark theme (GitHub issue 11); the chart
  background is now set explicitly.

**Quality and repository**

- The Control Panel has automated tests for the first time (GitHub PR 12): `ControlPanel.Core`,
  `ControlPanel.Tests` and `ControlPanel.sln`.
- Release tags protected, secret scanning on, a Code of Conduct added.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.9 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.9).

## [6.2.8](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.8) - 2026-08-07 (the tag's date)

A Control Panel bug-fix release. Both defects made a healthy server look broken. No server-core changes.

**Fixed**

- Every data-driven list pane showed blank rows (GitHub issue 6), from domain aliases to account rules.
- The Control Panel died after a service restart with "The RPC server is unavailable" (GitHub issue 7); it now
  reconnects on its own, waits for the server to report Running, and no longer starts a service the
  administrator stopped.

**Dependencies**

- WPF-UI 4.3.0 and QRCoder 1.8.0 in the Control Panel; LiveCharts held at 2.0.0-rc2.

**Schema:** 6005 at the tag (the published notes said 6001, which the tree at the tag does not bear out); no
database change.

Full notes: [the 6.2.8 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.8).

## [6.2.7](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.7) - 2026-06-15 (the tag's date)

A Control Panel usability release. No server-core changes since 6.2.6.

**Changed**

- A browse button on every field that holds a path.
- One-click DKIM: an RSA-2048 key pair generated with the exact DNS TXT record to publish.
- Password reveal, a generator for strong passwords, and the external POP3 password masked.
- A date picker for the auto-reply expiry, number boxes, Enter and Esc in every dialog, Copy on MX query and
  Diagnostics output, and a window that remembers its size and position.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.7 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.7).

## [6.2.6](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.6) - 2026-06-15 (the tag's date)

A Control Panel and installer polish release, with one new opt-in protocol mode.

**Changed**

- IMAP4rev2 (RFC 9051) as an opt-in session mode through `ENABLE IMAP4rev2`; IMAP4rev1 behaviour is unchanged
  until a client asks.
- A Control Panel visual redesign on a theme-aware colour-token system, and a Welcome page of quick actions.
- Every configurable server setting has a Control Panel control.
- Two-factor setup shows a scannable QR code.
- The installer's database page scales with DPI, and the dead legacy dependency installers are gone.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.6 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.6).

## [6.2.5](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.5) - 2026-06-15 (the tag's date)

A critical fix release: the default fresh install, SQL Server Compact with DPAPI-protected secrets, could not
connect to its own database. Anyone running a default install should upgrade.

**Fixed**

- The DPAPI-protected database password was truncated by a 255-character INI read buffer, giving SQL CE error
  25028; the buffer is now 4,096 characters.
- The create scripts stamped schema 6004 while the server required 6005, so a new install reported its database
  as too old.

**Control Panel**

- Full settings parity with the classic Administrator, with the Server Status page and the per-account rule
  editor.

**Schema:** 6005.

Full notes: [the 6.2.5 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.5).

## [6.2.4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.4) - 2026-06-15 (the tag's date)

A Control Panel release that completes the last functional-parity item from the classic Administrator. The
server, the Control Panel and the installer all report 6.2.4.

**Changed**

- Active Directory account pickers: a read-only browser of the forest's domains and users, for an account's
  directory link and for distribution-list recipients.
- "Settings could not be read" now names the real cause.
- The MariaDB Connector/C client is staged into the build output, so MySQL and MariaDB back ends work out of the
  box.

**Schema:** 6005.

Full notes: [the 6.2.4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.4).

## [6.2.3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.3) - 2026-06-15 (the tag's date)

The first server-focused release since 6.2.2. Every new behaviour is additive and off by default. The installer
no longer ships the classic Administrator; the Control Panel is the only bundled GUI.

**Mail filtering**

- A Sieve (RFC 5228) interpreter run at local delivery, and a ManageSieve (RFC 5804) listener behind
  `ManageSieveServerPort`.

**Operability**

- `/livez`, `/readyz` and `/healthz` on the metrics listener; Prometheus metrics for the database, TLS, the queue,
  authentication, delivery outcomes and command latency.
- A slow-query log with secrets redacted (`SlowQueryLogMilliseconds`); a graceful shutdown drain
  (`ShutdownDrainSeconds`); `MessageStoreFsync`; a read-only store consistency check
  (`MessageStoreConsistencyCheck`); log retention (`LogDeleteDays`); the session id in the `Received` header;
  an active/passive runbook.

**SMTP**

- BATV backscatter protection (`BATVEnabled`, `BATVSecret`); CHUNKING and `BDAT` (RFC 3030) on receipt.

**Supply chain**

- SBOMs for every release, Dependabot and a dependency-review gate.

**Schema:** 6005.

Full notes: [the 6.2.3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.3).

## [6.2.2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.2) - 2026-06-14 (the tag's date)

The 6.2.2 server with a substantially upgraded Control Panel.

**Server**

- Full RFC 4013 SASLprep for non-ASCII SASL credentials; RS256 OAuth2 bearer tokens covered by regression tests.

**Control Panel**

- Search on the long list pages, confirmation before deletes, numeric validation, the OS theme followed by default,
  and the Advanced group split into Security, Network and Maintenance.
- Anti-virus test buttons and scanner presets, event-script templates, and an "Advanced hardening" page for
  settings that had no control.
- Screen-reader names and stable automation ids throughout.

**Schema:** 6005.

Full notes: [the 6.2.2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.2).

## [6.2.1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.1) - 2026-06-14 (the tag's date)

Deliverability and modern SMTP standards. Every new feature is off or backward compatible.

**New**

- `PIPELINING` (RFC 2920), `SMTPUTF8` (RFC 6531 and 6532), `ENHANCEDSTATUSCODES` (RFC 2034) and DSN (RFC 3461 and
  3464).
- SRS for forwarded mail (`SRSEnabled`, `SRSSecret`); per-IP and per-destination rate shaping
  (`MaxSubmissionsPerIPPerMinute`, `MaxOutboundPerDestinationPerMinute`).

**Fixed**

- A trailing NUL from `Unicode::WideToMultiByte` could corrupt DPAPI-protected stored secrets.

**Schema:** 6004 to 6005.

Full notes: [the 6.2.1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.1).

## [6.2.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.0) - 2026-06-14 (the tag's date)

The hMailServer Control Panel arrives: a new .NET 8 WPF administration app beside the full 6.x server. The
.NET 8 Desktop Runtime is bundled and installed when missing.

**New**

- The Control Panel: a live dashboard, domains and accounts, the delivery queue with live logs, a graphical rules
  editor, every settings area including DANE, MTA-STS, ARC, TLS-RPT, ACME and the REST API, and the utilities.
- The classic Administrator and the Web Control Deck remain included.

**Schema:** 6004.

Full notes: [the 6.2.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.0).

## [6.1.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.1.0) - 2026-06-12 (the tag's date)

A UI and UX modernisation across the desktop Administrator, a new web interface and the installer. The server core
is unchanged from 6.0.0 build 4 apart from the new web-page endpoint.

**New**

- The Web Control Deck, served by the REST API listener (`RestApiPort`): a live dashboard, domain and account
  management, queue retry and delete, and DANE TLSA records to publish.
- The Administrator gains light and dark themes, a Ctrl+K command palette and a modernised dashboard.
- The installer's wizard is rebranded and ships the WebAdmin payload.

**Schema:** 6001.

Full notes: [the 6.1.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.1.0).

## [6.0.0-B4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B4) - 2026-06-12 (the tag's date) - pre-release

Build 4, which fixes the service crash at startup that build 3 caused on fresh installations.

**Fixed**

- Build 3 bundled VS2019-era VC++ runtime DLLs, which the service loaded in place of the current ones; the
  installer now ships `Microsoft.VC145.CRT` 14.51 and no longer bundles the obsolete UCRT.
- A stdout and stderr pipe deadlock in `build/run-tests.ps1`.

**Schema:** 6001.

Full notes: [the 6.0.0-B4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B4).

## [6.0.0-B3](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.0.0-B3) - 2026-06-12 (the tag's date) - tag only

Build 3 exists as a tag only; nothing was saved of a release for it, and there is no evidence one was published.

## [6.0.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0) - 2026-06-12 (the tag's date)

The production release of hMailServer 6.0, 64-bit only. **The `v6.0.0` tag points at build 2 (`e4b716318`), while
the published release shipped the build-4 installer, `hMailServer-6.0.0-B4-x64.exe`, and its notes describe build
4.**

**Fixed in builds 3 and 4**

- Build 4: the service no longer crashes at startup on fresh installations from mismatched VC++ runtime DLLs.
- Build 3: MySQL and MariaDB database creation, and `libmysql.dll` bundled.

**Highlights of 6.0**

- OpenSSL 4 with TLS 1.2 and 1.3, MTA-STS, DANE with in-process DNSSEC validation, ARC sealing, Ed25519 DKIM, ACME
  (Let's Encrypt) built in, the REST admin API, Prometheus metrics, IMAP `MOVE`, `ID` and `SPECIAL-USE`, PBKDF2
  password hashing, and the Visual Studio 2026 toolchain.

**Schema:** 6001 (DBUpdater upgrades a 5.7 database).

Full notes: [the 6.0.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0).

## [6.0.0-B1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B1) - 2026-06-12 (approximately, when its installer was uploaded) - pre-release

The first regression-validated build of the modernised 6.0.

**New in 6.0**

- MTA-STS and DANE with DNSSEC for outbound TLS, ARC sealing, TLS-RPT reporting, ACME with automatic apply, the
  REST admin API, MTA-STS, autoconfig and autodiscover hosting, Prometheus metrics, JSON logging, PBKDF2 password
  hashing with transparent rehash, Ed25519 DKIM, DMARC, IMAP `MOVE`, `ID` and `SPECIAL-USE`, 8BITMIME, an
  Administrator dashboard with TOTP two-factor sign-in, and TLS 1.2 and 1.3 defaults.
- Built with Visual Studio 2026 (v145), OpenSSL 4.0.1, Boost 1.91, PostgreSQL 18.3 libpq and .NET Framework 4.8.1.

**Fixed**

- A latent `CStdStr::FormatV` buffer-count bug that crashed the service.

**Schema:** 6001 (upgrade from 5.7 through DBUpdater).

**Licence:** AGPLv3, based on hMailServer by Martin Knafve.

Full notes: [the 6.0.0-B1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B1).
