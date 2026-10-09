# Changelog

This file records what changed in each release of hMailServer that this project has published, newest first,
and what is in the tree after the latest release. It is a summary. The full notes of a release - the reasoning,
the tests and the verification - are on its GitLab release page,
`https://gitlab.com/hmailserver/hmailserver/-/releases/<tag>`, which every section below links to (the sections up
to 6.3.5 link the project's first path, `gitlab.com/Progressiverobot/hmailserver`, which GitLab redirects).

This project is a maintained fork of hMailServer 5.7 by Martin Knafve. Its own releases begin at 6.0.0; the
`hMailServer-5.7.0-bNNNN` tags in the repository are upstream builds, not releases of this project.

**The latest release is 6.3.6, published on 6 October 2026.** Anything that arrived after it is in the tree
after 6.3.6 and not yet released. It is listed under [Unreleased](#unreleased---in-the-tree-after-636-not-yet-released)
and will carry a version number when a release is cut.

## Where the releases are

The project's home is <https://gitlab.com/hmailserver/hmailserver>, in the group `hmailserver` since 29 September
2026 (its first path on GitLab, `Progressiverobot/hmailserver`, redirects there). Its releases were published on
GitHub until GitHub suspended the account on 18 September 2026, and every `github.com/Progressiverobot` address in
older text now answers 404. Every release up to 6.3.3 has been re-created on GitLab from the GitHub original, under
its original date and with its files byte for byte, but for 6.2.0 and 6.0.0-B4, whose files were not preserved:
<https://gitlab.com/hmailserver/hmailserver/-/releases>, with the latest always at
<https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest>. A tag with no release is at
`https://gitlab.com/hmailserver/hmailserver/-/tags/<tag>`.

One release is not a version of the server and has no section here:
[`build-inputs-1`](https://gitlab.com/hmailserver/hmailserver/-/tags/build-inputs-1), of 11 September 2026,
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
make every fold of parallel work conflict on this file. A released version's section is written once, before
its tag, in the commit the tag points at: from 6.3.5 the tag's pipeline makes the GitLab release's notes from
that section as the tag's own tree has it (`build/ci/release-chain.py`, `release-publish`), and refuses a version
without one, so the section cannot be added afterwards. GitLab CI's `checkers` job runs
`python build/add-license-headers.py --check` on every push; after editing, run it too and read the rendered page
for a stray `#` number or `@` name. A section's *What changes on upgrade* list is numbered and the section's text
names its items by number, so a fold of two branches that each added items leaves two with one number:
`python build/renumber-upgrade-items.py` numbers the list 1 to n again and rewrites every reference to an item inside
the section, saying which of two items of one number it took a reference to and why, or asking (`checkers` runs it
with `--check`).

---

## Unreleased - in the tree after 6.3.6, not yet released

### Security

Every user of a Windows computer on which hMailServer was installed in a folder at the root of a drive could replace
the programs its service runs. The installer left the program folder, `Bin` and `ControlPanel` with the permissions
the folder above them hands down. Under `C:\Program Files`, the default, that lets nobody but SYSTEM, the
Administrators and TrustedInstaller change them; but the root of a drive on a Windows client hands every folder made
below it `Authenticated Users` Modify, so any account that can sign in to the computer could replace `hMailServer.exe`,
or put a library in `Bin` that Windows loads into it before its own, and the service - LocalSystem by default and
after every upgrade that keeps it - ran it at its next start, as did the next administrator to run `DBSetup`, the
Control Panel's elevated steps or the uninstaller. On Windows Server the root lets `Users` make files in it, which is
enough for the library. Every release from 6.0.0 to 6.3.6 installs so in such a folder; the default folder was never
affected. 6.3.6's `/ProtectFolders` made the data folders private and left these three as they were, and
`/GrantServiceAccess` only said `CHECK`, and only under the virtual account. `hMailServer.exe /ProtectFolders`, which
setup runs at every installation and upgrade, now hardens the three: SYSTEM and the Administrators with full control,
`BUILTIN\Users` with Read & execute where every user could read them before (none in a folder only the administrators
may open), nothing inherited, and the Administrators as the owner of one somebody else owned, without the entries of its
own that let others change it, which that owner could have written.
It then holds the installation to the rule an administrator's install of an update checks, which such an installation
now passes, and says what it does not change - an entry of a file's own, a folder above the program folder that others
may rename - on a `CHECK` line with the `icacls` that removes it. Setup also gives a program folder it makes those
permissions before it copies anything into it, hardens in place the folder an installation of the Control Panel alone
finds there - no `/ProtectFolders` follows one, as it has no server - unless it is a drive's root, holds a server
installation or is a link, and says, without refusing it, when the folder chosen lets every user change what is in it.
Under Program Files the first run makes the three's entries their own, and nobody may do there anything they could not
before. An administrator who cannot upgrade yet runs the three `icacls` lines the documentation gives from an elevated
prompt. Found by the project's own review (work item 78; CVE requested).
[ServiceAccount.md](hmailserver/docs/ServiceAccount.md), "What the other users of the computer may read".

- **Fixed: any sender could lift the rule loop limit off a message, an account's own Sieve script or rule could
  reset it, and a Sieve `notify` did not consult it.** The server counts the generated messages a message has been
  through - a rule's forward, reply or copy, an account's forward, a Sieve `redirect`, `vacation` or `notify`, a
  bounce - in its `X-hMailServer-LoopCount` header, and makes no more once the count reaches the rule loop limit
  (`RuleLoopLimit`, 5 by default): that is what ends a loop between two accounts that forward to each other.
  - Nothing takes the header off mail that arrives from outside, and the server read it with `_ttoi`, so a negative
    count passed every test against the limit: a sender who wrote `-2147483648` set such a loop going round some two
    thousand million times, and on Linux, where `_ttoi` keeps the low 32 bits of a 64-bit `long`, so did one who
    wrote `2147483648`, which read as that same count, while `4294967295` read as -1 (in every version, and on Linux
    from 6.3.0). A Sieve `notify` went out whatever the count, carrying it + 1, so a count of `2147483647` made that
    a signed overflow, undefined behaviour that in practice gave the notification the most negative count there is
    (from 6.2.22). The header is now read in one place, `RuleGuard::ReadLoopCount`: as a count only when it is
    digits and nothing else, and never above the limit; anything else - a sign, a letter, an empty value - reads as
    the limit, so the message is delivered and none of the generated messages above is made from it; and a
    notification is no longer sent once the count has reached the limit, as a vacation reply is not. A message
    without the header, or with a count below the limit, is treated as before.
  - The count a forward or a redirect reads is the one the message carries when it runs, after the edits before it. An
    account's Sieve script could `deleteheader` the header, or `addheader` a 0 above it, ahead of a `redirect` (from
    6.2.22, which brought `editheader`), and a rule's *Set header value* action could set it ahead of a forward (in
    every version) - an account may change its own rules over COM - so that one such account and an ordinary forwarder
    looped between local accounts without bound. And a line break in a value either of them wrote went into the
    message as it stood, writing a second field: the count, reset, under a name nothing had checked - a sender's to
    write where a script copied a body test's `:matches` capture into a header. A script that edits the header, the
    name in any case, is now refused when it is stored or checked (ManageSieve `PUTSCRIPT` and `CHECKSCRIPT`, the
    self-service filter, COM's `CheckSieveSyntax`), as `Received` and `Return-Path` are, with the reason; the edit is
    not made at delivery either, of the name as the script's variables made it, and a script stored before this
    release still runs, without that edit, which the application log names. A *Set header value* action on the header,
    or on a name with a line break, a colon or a character outside ASCII in it, is refused by `RuleAction.Save`,
    `Rule.Save` and the REST API's rule routes, and one stored before is skipped, with a line in the application log.
    Event scripts, which are the administrator's own code, may still set the header. A script's edit of
    `Auto-Submitted` is held the same way, refused when stored and not made at delivery: the field says a program
    sent the message, and every automatic responder, this server's vacation, notification and out-of-office among
    them, reads the first one and does not answer (RFC 3834 section 2). RFC 5293 section 6 says a script MUST NOT be
    permitted to delete it, and RFC 3834 section 5.1 allows a message one, so an `addheader` of `no` above it is a
    deletion by another name; either, ahead of a `redirect`, handed an automatic reply to the next mailbox's
    responder as a person's message to answer (from 6.2.22).
  - A line break in a header value is now written as spaces unless it folds the line, a space for each CR and each LF,
    by the MIME writer itself, so that no value writes a field of its own. That was not the rules' and the scripts'
    edits alone: every message the server generates is built from values that are not its own, and the field a line
    break wrote sat above the ones the generator set, so it was what was read. A Sieve `notify` whose `:message` or
    `:from` held line breaks wrote `Auto-Submitted: no` and a count of 0 above the notification's own (from 6.2.22),
    so that a script notifying its own account notified about its own notifications for ever; a copy rule's name,
    which the copy carries in `X-CopyRule`, reset the count at every copy (in every version), so that an account
    copying its own mail made copies without end; and a rule's reply subject, an out-of-office subject, and the
    sender's own subject decoded into a vacation reply or a bounce reset it on the message they made. COM's
    `Message.Save` refuses a credential's message whose values hold a line break that does not fold them, and says
    why, rather than send it changed (6.3.6 sent a line break that began a field of its own as that field, and checked
    it as it checks the others). Nor does the writer fold a value before white space that ends it, which left a line
    of nothing but white space - the end of the header to some programs - and which `Message.Save` refused, from
    6.3.6, as a line break the program never gave: a subject of 78 letters and a space.
  - An out-of-office reply, an account's or a domain's, carried a count of 1 whatever the message it answered
    carried, where every other reply carries that count + 1: it now does too, and none is sent once the count has
    reached the limit, as a Sieve `vacation` reply is held. So a `RuleLoopLimit` of 0, which already stopped every
    forward, redirect, rule reply and copy and every Sieve `vacation` reply, now stops out-of-office replies too: a
    server set to 0 to make no generated mail sends none, where until now it still answered every message to an
    account that was out of the office. The setting's description in the REST API says what 0 does.

  `LoopCountTester` and `LoopCountEditTester` (`business-object-tests` on Linux) hold the reading, the edits and the
  writer to examples, `MimeTester` (`report-parser-tests` on Linux) the writer's folds, and `Rules.cs`,
  `API/RestApiRules.cs`, `API/ComCallingProcess.cs`, `Sieve/SieveEditheaderDelivery.cs`, `Sieve/ManageSieve.cs`,
  `Sieve/SieveNotifyDelivery.cs`, `Sieve/SieveVacationDelivery.cs` and `SMTP/OutOfOfficeScope.cs` send them through
  storing and delivery, four loops among them. Found by the project's own review.

- **Fixed: a Sieve `notify` was sent from whatever its `:from` said, so that any account with a Sieve script could
  send mail through this server as any address.** A `vacation` reply's `:from` is used only when it is an address
  that reaches the account - the account's own, or an active alias whose value is it - and otherwise the reply goes
  from the account's own address, with a line in the application log. A notification's `:from` was not checked at
  all (from 6.2.22, which brought `enotify`): a script of three lines sent a notification to any address, on this
  server or anywhere, from another account's address or anyone else's, and the server signed it with its DKIM key
  when it hosts the domain of the address it claimed. `notify`'s `:from` is now held to the rule `vacation`'s is, by
  the one function both use (`SieveFromAddress`), whatever the notification method. One that is not an address at
  all - none, a display name, an address followed by a line break - is refused when the script is stored or checked
  (ManageSieve `PUTSCRIPT` and `CHECKSCRIPT`, the self-service filter, COM's `CheckSieveSyntax`), as `vacation`'s is,
  with the reason; and at delivery one that does not reach the account is replaced by the account's own address,
  with a line in the application log naming the account and what its script asked for. A script stored before this
  release whose `notify` has a `:from` that is not an address still runs, and its notifications go from the
  account's own address. And the address the server finds for a `:from`, a `vacation` reply's too, must now be the
  one the script wrote, letters' case aside: on MySQL, whose collation ignores accents, a `:from` with an accent on
  a letter of the account's address or of an alias's was found as that address and used as the script wrote it.

  `SieveFromAddressTester` (`business-object-tests` on Linux) holds the rule, the check at storing and a stored
  script's run to examples, and `Sieve/SieveNotifyDelivery.cs` and `Sieve/ManageSieve.cs` send an alias, another
  account's address and a `:from` that is not an address through storing and delivery. Found by the project's own
  review.

- **Fixed: an account's Sieve script could redirect a message as from any address, and have this server sign it
  with that address's domain's DKIM key.** `editheader` let a script add or delete any field but `Received`,
  `Return-Path` and the loop count, and a `redirect` sends the message on as the script left it (RFC 5293 7). The
  server signs a message as the domain its `From` names wherever it signs for that domain and the message carries no
  signature of it, so a script that wrote a `From` - another account's address, or anyone's at a domain hosted here -
  had the redirected copy sent from this server in that name and signed as that domain; and one that deleted the
  `DKIM-Signature` a colleague's message came with, changed its `Subject` and redirected it had the server sign the
  changed message as the colleague's domain (from 6.2.22, which brought `editheader`). A script may no longer add or
  delete `From`, `Sender`, `Resent-From`, `Resent-Sender` or `DKIM-Signature`, the name in any case, which RFC 5293 6
  leaves to the server: one that does is refused when it is stored or checked (ManageSieve `PUTSCRIPT` and
  `CHECKSCRIPT`, the self-service filter, COM's `CheckSieveSyntax`), as one that edits `Received` is, with the
  reason; the edit is not made at delivery either, of the name as the script's variables made it, and the
  application log names it; and a script stored before this release still runs, without that edit. `Reply-To`,
  which names no sender, and every other field may still be edited.

  `SieveFromAddressTester` holds the refusal, a stored script's run and the edits at delivery to examples, and
  `Sieve/SieveEditheaderDelivery.cs` sends them through storing, delivery and a redirect between two accounts. Found
  by the project's own review.

- **Fixed: an account's own rule could reply as any address, or set the `From` of a message it forwarded, and have
  this server sign it with that address's domain's DKIM key.** An account may change its own rules over COM, signed
  in as itself, and a domain's administrator, or a REST key confined to the domain, the domain's. A rule's *Reply*
  went out from whatever address its action named, and its *Set header value* could set any header but the loop
  count, `From`, `Sender`, the `Resent-` pair and `DKIM-Signature` among them, ahead of a forward or a copy (in every
  version); the server signs a message as the domain its `From` names wherever it signs for that domain. Which
  collection a rule is in is recorded, and who saved it is not, so the two actions are now held to the rule's owner:
  - a *Set header value* of `From`, `Sender`, `Resent-From`, `Resent-Sender` or `DKIM-Signature`, the name in any case
    and with blanks round it, is for the server's rules alone, which only the server administrator saves - a domain's
    or an account's rule that sets one is refused, whoever saves it;
  - a *Reply* goes from an address the owner may send as: for an account's rule, one the account may send as - its
    own address, an alias of it, a mailbox that granted it the post right, an address it proved, a masked address of
    its own, the rule authenticated SMTP and the webmail's send apply - and for a domain's rule an address of the
    domain or of one of its domain aliases; one email address in either case. An account's rule the server
    administrator saves is held to the account's addresses too, so that what is saved is what is sent;
  - and the reply's name is written as one quoted name before its address: a name holding a comma and an address was
    written as it stood, and added that address to the reply's `From`. A reply from no address, the null sender, has
    its name before an empty `<>`, and a name with an `@` in it is left out: the reader the server picks a `From`'s
    address with takes a name that is one email address for the address, so a reply from no address under the name
    `ceo@` a domain hosted here was signed as that domain.

  COM's `Rule.Save` and `RuleAction.Save` and the REST API's domain and account rule routes refuse such an action,
  with the reason. One stored before this release, or restored from a backup since, is not taken at delivery - the
  header is not set, the reply not sent - and the application log names the rule and why; the rule's other actions
  are taken. To set `From` on a domain's mail, make the rule a server rule with a condition on the domain.

  `RuleOwnerTester` (`business-object-tests` on Linux) holds the two checks and the reply's `From` to examples, and
  `Rules/Rules.cs` and `API/RestApiRules.cs` save the rules over COM, signed in as the account and as the
  administrator, and over REST, and deliver a reply and a `From` stored before the check; `AntiSpam/DKIM/ArcSealing.cs`
  delivers a reply from no address under the name of a hosted domain's address, unsigned and naming no address.
  Found by the project's own review.

  A rule's *Warn sender* notice, and the notice a *Reject* met after acceptance sends, carry the rule's name and its
  owner's text, and came from `postmaster@` the sender's own domain (from 6.3.4, which brought them): an account's or
  a domain's rule wrote to a sender of another domain hosted here in that domain's name, and the server signed it with
  that domain's key. A notice now comes from the postmaster of the rule owner's domain - the account's domain for an
  account's rule, the domain for a domain's rule - and is signed as that domain, as mail the server makes is. A server
  rule's notice, the administrator's, comes from the postmaster of the server's default domain when one is set and
  hosted here, else of the hosted domain the message was addressed to, else of the sender's own.
  `Rules/OutboundContentPolicy.cs` has an account of one hosted domain warn a sender of another, and finds the notice
  from its own domain's postmaster and signed as it.

- **Fixed: mail from outside whose `From` named a domain hosted here was signed with that domain's DKIM key, and so
  was every forward of it.** The server signed a message as the domain its `From` names on its first delivery pass,
  whatever the message was (in every version that signs): a sender anywhere who wrote `From: ceo@` a domain hosted
  here had the message signed as that domain before local delivery, and an account's forward of it, a rule's
  forward or copy, a Sieve `redirect`, an alias or a distribution list to an address outside, the mirror copy or a
  journal rule's copy left the server as a forgery that passes DMARC. A message is now signed as its `From`'s domain only when the server is
  entitled to: a submission by a signed-in user, and every copy of it, unchanged; a message the server makes itself;
  and one an IP range lets in as one of this server's domains without signing in - the session the server is the
  submission server for, as `My computer`'s web sites and scripts are. Anything else came in from outside - an SMTP
  session that did not sign in and was not such a submission, a download from an external account, a message
  imported from a file - and is recorded so on the queued message, never in a header, and on every copy made of it,
  through a restart (`hm_messages.messageauthdomains` carries the mark ahead of the DMARC domain, with no schema
  change). It leaves with the signature it came with, and with `ArcSealingEnabled` on, the ARC set the relaying
  domain seals. A managed list does not sign its copies of such a posting as the list's domain when an address of
  the list domain's organisation is anywhere in its `From` fields, which DMARC would align. The quarantine keeps
  whether a message it holds came from outside in the name of the copy it stores, and a release reads it from there
  rather than from the message, whose `Received` field a domain's rule may have rewritten before holding it; a
  message held before the upgrade is from outside unless it has no `Received` field, or the one on top is this
  server's own for a session that signed in, or the send API's - so that a download from an external account, whose
  top field is the other server's, is released unsigned. Messages in the queue at the upgrade were recorded without
  the mark and are signed as before.

  `SigningOriginTester` (`business-object-tests` on Linux) holds which messages are signed, the mark on every copy,
  on the queue row and in a quarantined copy's name, a release's reading of a header, and the list's decision to
  examples; `AntiSpam/DKIM/ArcSealing.cs` sends a signed-in submission (signed) and mail from outside with a hosted
  `From` through an account's forward, an account rule's forward, a Sieve `redirect` and a distribution list (none
  signed, the forwards sealed) and a signed-in member's posting to the list (signed); `AntiSpam/Quarantine.cs`
  releases mail from outside that a domain's rule claimed a signed-in client for and then held, and finds its forward
  not signed. Found by the project's own review.

- **Changed: an authenticated SMTP submission is signed as another domain hosted here only when its account may send
  as the address its `From` names.** Authenticated SMTP does not hold the `From` header to the account
  (`SmtpAuthenticatedSenderCheck`, off by default, holds `MAIL FROM` alone), and the server signed a signed-in user's
  message as whatever domain hosted here its `From` named, so an account of one domain that wrote `From: ceo@` another
  had the message signed with that domain's key, which DMARC then passed (in every version that signs). The webmail's
  send and COM's `Message.Save` already hold a `From` to the account. Such a submission is now signed as the `From`'s
  domain when that is the account's own domain, or one of its domain aliases, whatever the address in it - an account
  writing as `info@` its own domain is signed as before - and as another hosted domain only when the account may send
  as that address: its own, an alias pointing at it, a mailbox that granted it the post right, an address it proved, a
  masked address of its own (`SendAs.md`). Otherwise the message is still accepted and sent, unsigned as that domain,
  and so is every copy of it, through a restart - it carries the mark mail from outside does - and the debug log says
  why. A session a trusted upstream signed in through `XCLIENT LOGIN` under a name that is no account here is held to
  the same rule with the name in the account's place: signed as the domain the name is at, and as another hosted
  domain only for an alias that points at the name. **Upgrading:** an installation where accounts send as another
  hosted domain's addresses without an alias or a grant should give them one, or the mail goes unsigned.
  `SigningOriginTester` holds the mark and the address the signer reads to examples, and `AntiSpam/DKIM/ArcSealing.cs`
  sends a signed-in account's message as another hosted domain's address (not signed, nor its forward), as an alias of
  its own and as its own domain's other address (both signed).

- **Fixed: an ARC seal attested whatever SPF result the first `Received-SPF` field of the message claimed.** With
  `ArcSealingEnabled` on, the `ARC-Authentication-Results` this server seals over mail it relays read `spf=` from
  that field (from 6.0.0, which brought ARC sealing), which is anybody's to write: the sender's own, on mail this
  server wrote no `Received-SPF` field for - it writes none unless `ReceivedSpfHeaderEnabled` is on - and on mail it
  did, a field a Sieve `addheader` or a rule's *Set header value* placed above it ahead of a `redirect` or a forward.
  The seal then told every receiver that trusts this server's ARC sets that SPF had passed for a message whose check
  never ran or failed. `spf=` is now the result this server's own check reached when the message arrived, recorded
  on the queued message beside where it came from and carried by every copy of it, through a restart - `none` when
  the check did not run - and no field of the message is read for it. The quarantine keeps it in the name of the
  copy it stores, beside whether the message came from outside (`<GUID>.spf-fail.outside.eml`), and a release puts
  it back, so that a released message and its forward are sealed with the result the message arrived with.
  Messages queued before the upgrade, and messages held before it, carry no result and are sealed with `spf=none`.

  `ArcAuthenticationResultsTester` (`business-object-tests` on Linux) holds the value, and the result on every copy,
  the queue row and a quarantined copy's name, to examples; `AntiSpam/DKIM/ArcSealing.cs` relays mail
  from outside with a `Received-SPF: pass` of the sender's through an alias, and with one a Sieve script added through
  a `redirect`, and finds neither attested, and `AntiSpam/Quarantine.cs` releases a message whose SPF check failed and
  finds the failure attested over its forward. Found by the project's own review.

- **Fixed: which hosted domain's key sealed an ARC set over relayed mail could be chosen by whoever wrote a header or
  the envelope.** With `ArcSealingEnabled` on, a message this server relays and does not sign as its author's domain
  is sealed by a domain hosted here, and the sealing domain was the first with a DKIM key among the envelope sender,
  the recipients' original addresses and the `X-Original-Rcpt-To` field (from 6.0.0, which brought ARC sealing): the
  field a sender writes when this server does not (`AddXOriginalRcptTo`, off by default), and a Sieve `addheader` or a
  rule's *Set header value* can write above this server's own; and an outside sender's envelope may name a hosted
  domain. Whoever wrote it chose which hosted domain's key vouched for the seal, though the results it attested stayed
  this server's. The sealing domain is now the one the server records as sending the message on, where it makes the
  message, on the message itself: the forwarding account's domain for an account's forward, its rule's forward or
  copy and its Sieve `redirect`; the domain of a domain's rule, of a journal rule and of a managed list; for a server
  rule, the domain the message was received for; the submitting account's domain for its own message; and the hosted
  domain an alias or a distribution list that expanded to an address outside belongs to. It is carried by every copy,
  kept on the queue row (`relay=` beside the outside mark) and in a quarantined copy's name (`.relay-<id>`), and a
  message with none recorded goes unsealed: no header and no envelope is read for it. Messages queued or held before
  the upgrade are sealed by an alias's domain, or a submitting account's, and otherwise not at all.

  `SigningOriginTester` holds the record on every copy, on the queue row and in a quarantined copy's name to examples,
  and `AntiSpam/DKIM/ArcSealing.cs` sends an account's forward of a message carrying a forged `X-Original-Rcpt-To`, and
  an alias's expansion of one whose envelope names another hosted domain, and finds each sealed by the domain that
  relayed it. Found by the project's own review.

- **Fixed: an ARC seal's results did not say whose they were.** The `ARC-Authentication-Results` this server seals
  said `dkim=pass` and `spf=pass` with no property (from 6.0.0), so a receiver could not tell a pass for the sender's
  own domain from one for the author's. Each now says, as RFC 8601 section 2.7 and RFC 8617 section 4.1.1 have it:
  a `dkim=pass header.d=` for each domain whose signature verified when the server sealed the message, each named once,
  and the domain the SPF check evaluated - the envelope sender's, the local part left out - as `smtp.mailfrom=`. That
  domain is recorded with the result when the message arrives, since a forward's envelope may be rewritten (SRS,
  `RewriteEnvelopeFromWhenForwarding`, a list's bounce address), and travels as the result does: on every copy, on the
  queue row (`mailfrom=`, left out when it does not fit beside the DMARC domain, which keeps its place) and, for a
  message held as it arrived, as a mark in the quarantined copy's name that it was the sender's domain. A check that
  did not run, or ran for a null envelope sender, names none, nor is a value that is not a domain's characters ever
  written. A value too long for one line is folded between its results. A message's record of where it came from
  costs it less than before: the two strings are kept apart from it, made only when set, so that every message an
  IMAP folder's cached listing holds is 24 bytes smaller. `ArcAuthenticationResultsTester` holds the value, its folds
  and the domain's travel to examples, and `AntiSpam/DKIM/ArcSealing.cs` and `AntiSpam/Quarantine.cs` find both
  named in a seal. Found by the project's own review.

- **Fixed: a mailbox key encrypted at rest stayed open, its private half in memory, past `MailboxKeyIdleMinutes` for
  as long as the server's other scheduled tasks held up their minute.** The sweep that closes an idle key was the first
  step of `RemoveExpiredRecords`, one of the Scheduler's tasks, and the Scheduler runs every task that is due one after
  another on one thread and sleeps its minute after the last of them (from 6.3.4, which brought encryption at rest).
  So the sweeps came a minute plus whatever every task of the minute took apart: a webhook pass may take a minute by
  design, ten seconds for each subscription whose endpoint does not answer, and the hourly message-store consistency
  check runs on the same thread as long as the store takes. The sweep now has a thread of its own and runs every ten
  seconds from the start of the servers to their stop, touching no database and waiting on nothing, so that a key
  closes within ten seconds of its idle minutes whatever the scheduled tasks are doing. `EncryptionAtRest.md` says
  so. `Infrastructure/EncryptionAtRest.cs` signs an account in at the start of a webhook pass that six subscriptions
  to an endpoint that never answers hold for a minute, and finds its key closed within ninety-five seconds of the
  sign-out, beside the endpoint reached six times ten seconds apart; with the sweep among the scheduled tasks the key
  closed at the turn after the pass, some two minutes on. Found by the batch's regression gate.

### Correctness

- **Fixed: a ManageSieve refusal that quoted a script's text with a line break in it was split in two.** RFC 5804's
  quoted string may hold no CR or LF, and `PUTSCRIPT` and `CHECKSCRIPT` quote the script's own text when they refuse
  it - an extension name, a string - which a Sieve string may write across lines. The server sent the text as it
  stood, so its `NO` ended at the break and the rest of the refusal reached the client as a line of its own, which it
  read as the start of the answer to its next command (from 6.2.3, which brought ManageSieve). A line break in a
  quoted string the server sends is now a space - a CR LF pair one space - and so is a NUL, and a script's name in
  `LISTSCRIPTS` is written the same way. `ManageSieveResponseTester` (`business-object-tests` on Linux) holds it to
  examples, and `Sieve/ManageSieve.cs` sends a script requiring an extension whose name holds a line break through
  `CHECKSCRIPT` and `PUTSCRIPT` and reads the `NOOP` after each. Found by the project's own review.
- **Fixed: a criterion or an action added over COM to a new rule after its first save was stored only when the rule
  was saved again.** A rule's criteria and actions are read when COM first asks for them, and a new rule's were bound
  to the rule as it then was, with no id: once the rule was saved, a criterion or an action added to a collection read
  before the save, through the same `Rule` object, was kept in memory by its own `Save` and stored by nothing but
  another `Rule.Save` (in every version). In the tree after 6.3.6 `Rule.Save` reads the rule's actions itself, to hold
  them to the rule's owner (*Security*, above), which bound the actions of every rule saved over COM so, and
  `RuleAction.Save` held an action saved that way to no owner: an account's rule took a *Set header value* of `From`,
  which it refuses, though it stored it nowhere and `Rule.Save` and delivery still refused it. A new rule's criteria
  and actions are now bound to the id its save gives it, and one made before that save and saved after it is stored
  under the rule. `Rules/Rules.cs` saves actions and criteria through a new rule after its first save, made before it
  and after it, and reads the rule back from the database. Found by the batch's regression gate.
- **Fixed: the REST API refused a rule's *Reply* with no `from_address`, which COM and the Control Panel save.** A
  reply with no address goes from the null sender, with the rule's name before an empty `<>` (*Security*, above), and
  `POST` and `PUT` on the server's, a domain's and an account's rules answered 400, "from_address is required for a
  reply action" (from 6.3.0, which brought the REST API's rule writes): a rule the Control Panel saved with one could
  not be saved back over REST as `GET` returned it. A reply with no `from_address`, or an empty one, is now saved as
  COM saves it, and one that is given is held to a domain's or an account's rule's owner as before.
  `API/RestApiRules.cs` saves one under a domain's and an account's rules, and the Linux suite delivers one through
  `AntiSpam/DKIM/ArcSealing.cs`. Found by the batch's regression gate.
- **Fixed: moving a rule or one of its actions over COM stored changes made to it and not saved, held to nothing.**
  `Rule.MoveUp`, `Rule.MoveDown`, `RuleAction.MoveUp` and `RuleAction.MoveDown` wrote each rule or action whose place
  changed whole, as it was in memory - a rule with its criteria and actions - so a change made through the object and
  not saved went into the database with the move (in every version): an account could turn a *Set header value* of
  its own rule to `From`, or a *Reply* to another's address, which `Rule.Save` and `RuleAction.Save` refuse it
  (*Security*, above), and have it stored by moving the action or the rule, or the one beside it, though delivery
  still skipped it. A move now writes the place alone, and the rules of the owner the rule's row names are read again
  in their new order. `Rules/Rules.cs` moves an account's action, and its rule, with an action turned to `From` or
  `Sender` and not saved, and reads both back from the database. Found by the re-check of the batch's fixes.
- **Fixed: a scheduled task due at a second was often found not due by a turn of the Scheduler within that second,
  and waited a whole turn, a minute, more; and when the clocks went back every scheduled task waited an hour.** A
  task's next run was a local date and time held as a count of days in a double, moved on after each run by a minute's
  span - or its own period's - added to it, and compared with the clock composed from the current second's fields. The
  sum came out a rounding error above the second it meant more often than not, from the second run after a start,
  and the Scheduler's turns, which come a minute and some 0.4 seconds apart, fall within the due second for a turn or
  two after a start early in a second, and again as they drift on: such a turn found the task not due, and the
  minute's tasks - the expiry of auto-bans and IP ranges, scheduled mail, alerts, webhook retries, the mailbox keys'
  idle sweep, which is now on a thread of its own (*Security*, above) - ran a minute late (in every version). And a
  local time came round again an hour after the clocks went back, so every task's next run was an hour away. The next
  run is now a count of seconds since 1970, as `time()` gives it, moved on and compared exactly. `ScheduledTaskTester`
  (`business-object-tests` on Linux) holds a task every minute, run at a turn within its due second for a week of
  minutes, to being due at each and never a second early, and one late by more than its period to resuming from the
  turn. Found by the batch's regression gate, where `EncryptionAtRest.AWebmailSessionWhoseKeyClosedIsEnded` waited
  out its 150 seconds for a key whose sweep had been put off a turn.
- **Fixed: a mailbox key encrypted at rest could be closed the moment it was opened or used, by an idle sweep that had
  read the clock a second before.** The sweep that closes idle keys read the clock before it took the lock the open
  keys are kept under, and a key whose last use was later than the time it had read counted as idle, as it does when
  the clock has been put back (from 6.3.4, which brought encryption at rest). A password sign-in that opened the key,
  a webmail request or the end of a session that came in the next second, while the sweep waited for the lock or had
  been put off the processor before taking it, was such a use: the key was closed at once, the session signing in
  found no key to hold, and a webmail session made under that opening was ended. The window is short - the time
  between reading the clock and taking the lock, microseconds unless the thread is put off the processor - but the
  sweep now runs every ten seconds where it ran once a minute (*Security*, above). It now reads the clock with the
  lock taken, as every use writes its time, so that only a clock that has been put back closes a key that way.
  `MailboxKeyIdleTester` (`business-object-tests` on Linux) starts a sweep while a use holds the lock, the use writing
  its time in the clock's next second before it lets the lock go, and finds the key open: with the clock read before
  the lock, as it was, the key was closed in 10 runs of 10. Found by the re-check of the batch's fixes.
- **Fixed: on Linux, an unexpected exception on one of the server's own threads ended the server.** The handler
  that runs a worker's body reports what it throws (HM4208) and passes it on, and on Windows the structured
  exception guard around it takes it there, writes the minidump and answers that the work failed: the work queue
  keeps its thread, the network's worker runs its context again, a singleton worker's thread ends and the server
  goes on. Linux has no such guard, and the exception left the handler - on the message indexer's thread, the SQL
  log device's or a migration job's, `std::terminate` and the end of the process; on a network worker, a pool one
  thread short (from 6.3.0, the first Linux release). The handler now takes what it passed on and answers as it
  does on Windows; a hardware fault still ends the process on Linux, as before. `business-object-tests` throws
  through the handler on a thread of its own and finds the handler answering false and itself still running; with
  the handler as it was it ended in `terminate called after throwing an instance of 'std::runtime_error'`, exit
  134. Found by the batch's Alpine suite, where the message indexer's thread ended the server two thirds of the
  way through (below).
- **Fixed: a reinitialise or a service stop while the message indexer was in the middle of a batch could end the
  server on Linux, and left the indexer stopped until its setting was saved again.** Stopping the indexer
  interrupted its thread and did not wait for it (in every version). The thread went on to its next interruption
  point, the next message of the batch, and a reinitialise closes the database as soon as the servers have stopped,
  so its next statement went through a database manager that was no longer there and threw ("boost unique_lock has
  no mutex", HM4208), or faulted, the close shutting the connection a statement was using too - which on Linux ended
  the process (the exception above; a fault ends it still) and on Windows wrote a minidump. And the start that
  followed found the thread still ending, took it for the running indexer and started none. The stop now waits for
  the thread to end, telling the service controller meanwhile. The thread ends at its next interruption point -
  before each message of a batch, between two slices of its sweeps for the rows of messages that are gone, in the
  pauses - which can be several statements away: a pass reads what it is to index with several before its first
  message. The sweep of the message metadata, which the indexer makes at every start, was one statement over the
  whole table; it is now cut into slices as the full-text terms' sweep is (*Operations*, below). A statement lasts
  as long as the database takes to answer it, or `DatabaseStatementTimeout` where the database applies that (not to
  a write on MySQL, and not at all when it is 0). A reinitialise waits for as long as that takes, since it closes
  the database straight after; a stop after which the process ends - the service's, or a signal's on Linux - waits a
  minute at most, says so in the log and goes on, so that a database that has stopped answering no longer holds the
  stop for ever while the service controller is told that it is progressing. A start after a stop begins a new
  thread, and no indexer is started while the servers are stopped, from a reinitialise's stop to its start, the
  start of the servers starting it from its setting - which is now saved before the indexer is started or stopped,
  where a save that came after the start of the servers had read it left the indexer stopped with the setting on,
  and a save refused for a setting the environment manages had started or stopped the indexer all the same; and the
  indexer is started or stopped again as the setting stands when another save has changed it meanwhile. A start made
  on the indexer's own thread, by an `OnError` script turning message indexing on while the indexer reported an
  error, asked the thread to join itself, which Boost refuses with an exception (in every version): the script met
  an error and the setting was not saved. Such a start now leaves the indexer as it is, and after a stop made on
  that thread too takes the stop back. And a stop no longer waits for the thread holding the lock that such a
  script's start or stop takes, which made the stop and the indexer wait for each other for ever.
  `MessageIndexerTester` (`TestInternals` on Windows, `business-object-tests` on Linux) runs the indexer's thread
  over a stand-in pass that holds no interruption point for a while: with the stop and the start as they were, the
  pass made a statement after the stop had returned, a start straight after a stop began no indexer, and a start on
  the indexer's own thread threw; with the lock held across the wait, the indexer's own stop and start while a stop
  waited for it waited for that stop, and it for them, until its limit; and a stop with a limit gives up on a pass
  that outlasts it, the start after it beginning a new indexer once the pass has ended. Found by the batch's Alpine
  suite: the teardown of `FullTextSearch.AMailboxLargerThanOneLookupIsSearchedAWindowAtATime` reinitialised while
  the indexer was in the second batch of the test's 450 messages, and every fixture after it was refused a
  connection. With the test's backfill made one batch, so that its teardown always comes in the middle of it, the
  server died there in three Alpine runs of three (twice a segmentation fault, once the exception) and lives through
  it with the fix. The limit, the lock, the order of the save and the start on the indexer's own thread were found
  by the re-check of the fix.

### Downloads and the release

- **The Helm chart's and the container image's repositories are on Artifact Hub, and the tree holds the IDs Artifact
  Hub gave them.** On 6 October 2026 the maintainers added both to the organization `hmailserver`: the chart's,
  `https://updates.progressiverobot.com/helm/`, with the ID `d8768ed9-b894-45c8-ba44-f7516bcf4206`, and the image's,
  `oci://registry.gitlab.com/hmailserver/hmailserver`, tracking its tags `6.3.6` and `latest`, with the ID
  `8cd87f71-a93b-4eb0-b3a0-a78ece5b5796`. `build/ci/helm-repo.py`'s `ARTIFACTHUB_REPOSITORY_ID` and
  `platform/packaging/docker/artifacthub-repo.yml` now carry them, so the next `package-repos` job writes the chart's
  into `helm/artifacthub-repo.yml` beside the index, and `container-image-artifacthub` - started by hand in `master`'s
  pipeline, and by itself in the next `v*` tag's - pushes the image's under the tag `artifacthub.io`: each gives its
  listing the Verified publisher label, which neither has yet. A new release's version tag is not tracked by itself:
  a maintainer adds it to the image repository's tags on Artifact Hub, ten at most.
- **A release's page says first what to download, and lists the Windows installers first.** 6.3.6's notes began with
  its `CHANGELOG.md` section, some 420,000 characters, and named no download at the top, and its two installers'
  links, made before the 22 `.deb` and `.rpm` links, were listed 24th and 25th of the 26 under *Assets*, *Packages*,
  among 160 links: GitLab lists a release's links newest first, and `release-publish` made them all in the order of
  their names. The owner could not find the Windows installer there. `build/ci/release-chain.py` now begins the notes
  with a `## Download` table made from the release's own files, above the section's heading: the x64 installer, and
  the ARM64 one when the release carries it, called a preview while `hmailserver/docs/WindowsOnArm.md` says, in
  bold, "This has not been run on ARM64 hardware.", each linked at its address on the release page with the SHA-256
  the job computed of the signed file; the Linux packages, naming the release's sums file; the container image, while
  the project's `HM_PUBLISH_IMAGE` is `1`; and how to check a download. It makes the two installers' links last, ARM64
  then x64, so that x64 and ARM64 head *Packages*. Every line of the table is a heading, a table row or blank, which
  the update feed's Atom summary passes over, so a release's summary is unchanged. The chain's self-test holds the
  table to the files (no ARM64 row without the ARM64 installer, no preview sentence once WindowsOnArm.md drops it, no
  image row while the variable is not `1`, each SHA-256 the file's), the summary to the notes without the table, and
  the order of the links. 6.3.6's release page was given the table and the order by hand on 6 October 2026.

### Delivery and SMTP

- **Fixed: with DANE enforcement on, mail to a DNSSEC-signed domain whose MX answer arrived with its exchange
  compressed - which is how every resolver measured sends it - was deferred, "the MX RRset for <domain> failed
  DNSSEC validation" and "No host was attempted", and TLS reporting recorded `dnssec-invalid`** (reported in work
  item 80, for live.com, on 6.3.4 and 6.3.6 under Windows Server 2022). A validating resolver checks an RRSIG over
  the records in the canonical form of RFC 4034 section 6.2, every name in them written out in full and in
  lowercase, and this one wrote out a CNAME's target alone: an MX record's exchange was checked as it arrived -
  live.com's `live-com.olc.protection.outlook.com` as four labels and a pointer to the "com" of the question - so
  the signature never matched. The chain itself validated throughout: the keys, the DS records, ECDSA P-256 and
  RSA/SHA-256 signatures alike. On 7 October 2026 live.com, outlook.com, cloudflare.com, ietf.org, isc.org,
  mailbox.org, posteo.de, debian.org, freebsd.org, gmx.de, web.de, tutanota.de, protonmail.com and nlnetlabs.nl
  were bogus before the fix and secure after it, through the system's resolvers on Windows and on Linux, and
  Cloudflare's, Google's and Quad9's resolvers send those exchanges compressed alike; a signed domain whose one
  exchange shares no label with it, such as sidn.nl, was secure before as well, and an unsigned one, such as
  hotmail.com, was never affected. The defect is as old as the validated MX lookup, 6.2.19, and
  `DaneEnforcementEnabled` has been on by default throughout. The names in a signed RRset's records - an MX record's
  exchange, a CNAME's target, and those of the other types RFC 4034 section 6.2 lists - are now written out and
  lowercased before its signature is checked, and the exchanges a validated MX lookup hands DANE are those names.
  Until the update, `DaneEnforcementEnabled=0` delivers to such a domain without DANE; MTA-STS, where the domain
  publishes a policy in enforce mode, still requires a verified TLS connection.
- **A DNSSEC validation that fails says where and why.** The debug log has a line for each bogus or indeterminate
  MX, TLSA or TXT lookup naming the RRset, the signing zone, the key tag and the check that failed: a signature that
  does not match the records, that has expired or is not yet valid (with this server's clock beside it), or that
  names a key the zone does not publish; a zone's DNSKEY records that match none of its DS records (with the DS
  records' key tags); a DS or DNSKEY RRset, or an answer, that came without its RRSIG records - the DNS server did
  not return what the DO bit asked for, or something on the way removed it; a missing proof of absence; or a lookup
  that did not complete. A zone whose keys do not validate is said once each time the verdict is reached, which the
  resolver then keeps for a minute (30 seconds for a lookup that did not complete). The application log lines that
  report a skipped host - "the MX RRset for ... failed DNSSEC validation", the TLSA one and the two indeterminate
  ones - end with the same reason in brackets ([DiagnosingStalledMail.md](hmailserver/docs/DiagnosingStalledMail.md)).

### Control Panel

- **Fixed: a list page whose read failed showed its empty state, the rows of its last visit or a count of zero as if
  they were current.** When the link to the service was lost, the delivery queue said "The delivery queue is empty."
  under its notice; the suppression list said there were no domains; the IP ranges, routes, rules, TCP/IP ports and
  incoming relays pages read with no catch at all and kept "No IP ranges defined yet." or their old rows, with only
  the shell's toast to say otherwise; the public folders page read its collection outside its catch; the audit trail
  said nothing had been recorded, the journaling page that there were no rules, every collection page's pill that
  there were "0 SURBL servers", the administrator roles page that nobody had been given a role, the domains page that
  there were no domains, with its account form live; the backup page showed ticked defaults with Save enabled; and the
  IP ranges page left Restore defaults live over a list that could not be seen. Every list page now shows a failed
  read the one way (`Services/ReadFailure`): the list is emptied and its empty state hidden, since neither can be
  trusted; a Critical notice above the list says what could not be read and why, with a Try again button that reads
  again; and the controls that write - Add, Restore defaults, the account form, the backup card - are disabled until a
  read works. The suppression list says there are no domains only after it has read that there are none, the
  collection pages' notice sits under the header rather than under three hundred pixels of blank card, and a role
  that could not be given is a Critical failure rather than an amber warning. The Try again button carries no Alt key
  in any language: it is on some twenty pages, and no letter is free of a collision on every one of them - in English
  T is also Require TLS for auth, Add action, Take the role away and Restore settings - and WPF, given two enabled
  targets for one key, moves the focus between them and presses neither; it is reached by Tab. The keyboard focus
  survives it: a read that fails again keeps the button it was pressed on, where a new one in its place sent the focus
  to the window and Tab back to the top of the page, and a read that works moves the focus to the first control that
  writes before the button goes. The domain white and black list reads again on a search while its read has failed,
  where filtering nothing brought "The list is empty, and an empty list changes nothing" back under the notice; the
  journaling page hides the card that held its list with the list, rather than leaving a bare strip under the
  notice; and the backup page clears its two hMailServer.ini boxes with the rest, where "Verify each backup…" stayed
  ticked - the file's copy, or the default - under "could not read the backup settings". `build/check-mnemonics.py`
  scans the shared helper and checks its captions against every page that calls it. `ReadFailurePageTests` holds
  every page to the pattern, the caption to no key in any catalogue, the helper to keeping the button and moving the
  focus, and the three pages to their part.
- **Fixed: the three read-only overviews drew defaults as findings when nothing could be read.** With the link to the
  service lost, the spam filtering overview said "1 value(s) could not be read" and then that nothing at all happens on
  score because both thresholds are 0, with every check Off at a score of 0; the virus scanning overview that no
  message is scanned and everything is delivered as it arrives, with a size limit of 0; and the transport encryption
  overview that no listeners, no certificates and no TLS version are configured - every one of them the type's default
  for a value the server never answered, drawn as a verdict about the server. When the settings object cannot be
  opened at all, each overview now draws no cards and shows one Critical notice with the reason and Try again. When
  some values cannot be read, each snapshot records which (`Unread` on `SpamPipelineConfig`, `VirusPipelineConfig` and
  `TlsPostureConfig`), the check or scanner they feed shows a neutral *Not read* pill with no score rather than Off, the
  verdict sentence and every note that follows from an unread value are left out and say so instead, and the warning
  above the cards reads "Some values could not be read, so the sections below may be incomplete. ..." - below, since the
  notice is above the sections it describes, where the virus and transport overviews pointed at "the rows above". The
  pipeline tests hold an unread check to neither on nor off, an unread threshold to no verdict and no threshold note,
  an unread scanner to no "cannot run" and no "nothing examines incoming mail", and unread listeners, versions and
  ranges to none of the notes that follow from them. With a check unread, the spam overview's verdict card says that
  the highest total reachable cannot be said, where it said the total was open-ended - a statement about a
  configuration it never saw. With the certificates unread but the ports read, the transport overview marks each TLS
  listener *Not read*, where it resolved every port's certificate against the empty collection and judged each one
  *Will not start* at Critical, with "N listeners configured for TLS but cannot serve it" and a Critical note per
  port, beside a certificates card saying they could not be read; its "Certificate: name" line is translated too.
- **Fixed: the quarantine page's empty state said quarantining was switched off whether it was or not.** With nothing
  held, the page always said "Nothing is held. Quarantining is switched off - turn it on under Anti-spam settings",
  under a warning glyph, and never read the switch: a server whose quarantine is on and merely empty was told it was
  off and sent to turn on what was on. The empty state now reads QuarantineEnabled, as the Anti-spam settings page
  does: while quarantining is on it says that nothing is held right now and what will appear there; while it is off
  it says so and offers *Open Anti-spam settings*, the page that turns it on; and its glyph is the neutral inbox, since
  an empty quarantine is not a warning. `QuarantinePageTests` holds the page to reading the switch and to saying "off"
  only when it is off.
- **Fixed: a Control Panel that could not read hMailServer.ini showed the directory pages as "LDAP off, nothing
  set".** From 6.3.6 the file belongs to SYSTEM, the Administrators and the service (`hMailServer.exe
  /ProtectFolders`), and the Control Panel asks for no elevation, so one started normally on such a server reads every
  `[LDAP]` value as its fallback. Directory authentication then showed the switch off and every field empty, with
  "Off. Directory-linked accounts are validated through Windows logon"; Directory synchronisation said "Not yet -
  directory access is switched off", the misstatement its own code says the card must never make; and a save failed
  with a message box. Both pages now ask whether the file can be read before they read anything, as the Live logs page
  does, and when it cannot they fill no editor, disable the editors and *Save changes*, and say so in a Critical notice
  above the cards: "hMailServer.ini could not be read: ... From 6.3.6 the file belongs to the administrators. Close the
  Control Panel and start it with Run as administrator to see and change these settings." The authentication page's
  state card says it cannot tell from here rather than "Off", the synchronisation page's takes the unread file through
  the branch that says it cannot tell, and its *Preview* and *Apply* stay available, since they run inside the server.
  `IniReadDeniedPageTests` holds both pages to asking before they read, and both state cards to never judging an
  unread file.
- **Fixed: in German the Server status page showed an open relay as a Warning and the auto-ban count as one too.**
  Each configuration warning carried its level as a word - "Critical", "High", "Medium", "Info" - passed through the
  catalogue and then matched back against the English, so in German "Kritisch" and "Mittel" matched nothing and fell
  to the mapping's default, Warning: the one Critical finding on the page was demoted and the one piece of information
  promoted, and Japanese escaped only because its catalogue left those words in English. Every warning now carries a
  `StatusLevel` - the open relay Critical; no host name, an empty sender denied and localhost banned Warnings; the
  auto-ban count and "could not evaluate" Information - and the word-to-level mapping and its vocabulary are gone,
  since a level cannot be translated. `StatusWarningLevelTests` holds every warning to a level and the file to no
  translated severity word.
- **Fixed: the Anti-virus settings page's "Delete infected attachments only" understated what the server deletes.**
  The strip action keeps one text part and removes every attachment, not only the infected one, and puts the server's
  "attachment removed" note before the text; an encrypted message, which has no part to keep, is delivered exactly as
  it arrived, with the log saying so. The two choices under *When a virus is found* now read *Delete the whole
  message* and *Remove all attachments and deliver the text*, as the virus scanning overview already described them,
  with a note under the row: "Every attachment is replaced by a short note, not only the infected one; an encrypted
  message is delivered unchanged."
- **Fixed: the Control Panel's Auto-ban page was blank on every second visit and after every Reload.** The never-ban
  list's editor kept its grid in a field built once with the page's definition, while the page builds every editor
  again on every visit; the second build added the same grid to a new panel, WPF refused ("Specified element is
  already the logical child of another element"), and the shell reported that as "Server unavailable" over an empty
  page, sending an administrator to restart a service that was fine. The editor makes its grid and status line anew
  on every build, and the shell titles a fault of a page's own "Could not build this page", keeping "Server
  unavailable" for a COM failure or a lost link. `NeverBanEditorTests` holds the editor to building afresh and the
  shell to telling the two apart.

- **Fixed: the Control Panel threw away unsaved edits without a word.** The window keeps a page once it has been opened,
  and every settings page reads the server again as it is opened, so an edit left behind by a click in the sidebar, a
  *Related* link, a palette result, closing the window or the page's own *Reload* was gone when the page came back -
  while its pill had said *Unsaved changes* the whole time. Leaving a page that holds an unsaved change now asks, *You
  have unsaved changes on* the page: *Save* saves it as the page's own Save does and goes on only when everything was
  saved, *Discard* goes on, and *Stay*, which Escape and Enter give, keeps the page and puts the sidebar back on it.
  The question is asked of every way to another page - the sidebar, a link, the palette, a tour - and of closing the
  window and a change of language, which restarts the Control Panel; *Reload* (and the event scripts page's *Reload
  from disk*) asks *Discard your unsaved changes and read the settings again?* first, Cancel being the answer Enter
  gives; and a reconnect after a service restart or *Apply now* no longer reads a page with unsaved changes again over
  them. The pages it covers are the two kinds of settings page (every page of hMailServer.ini settings and every page of
  the server's settings), directory authentication, directory synchronisation, the event scripts and the IP ranges
  page, whose permission boxes are asked about before another range's are put over them, and kept across an add.
- **Changed: the Control Panel changes the administrator password in a dialog that asks for the password in use now
  and the new one twice.** It was one masked box on the *Administrative access* page, *New administrator password
  (leave empty to keep the current one)*, written by the page's *Save changes* together with the mailbox password
  policy beside it, with no second box, no current password and nothing said afterwards: a typo locked the
  administrator out of the Control Panel, the REST API and every COM script, and the way back in is an elevated prompt
  on the server. The card has a *Change the administrator password…* button now, which opens *Set a new administrator
  password*: each box says on itself what is wrong with it - *The two passwords do not match* among them - the password
  in use now is put to the server before the new one is written (a one-time code is asked for when the server wants
  one), what the server refuses, such as a password found in a published breach, is said in its words, and the card
  says afterwards that the password was changed and to use the new one at the next sign-in, in scripts and in REST API
  clients. A Control Panel signed in as the administrator goes on with the new password, after a service restart and
  for the settings it reads over the REST API.
- **Changed: the Control Panel asks before it saves a new password pepper on a server with accounts.** The pepper was
  an ordinary secret among the hashing fields of the *Authentication* page, captioned *Password pepper — WARNING: set
  before creating accounts; changing it later invalidates ALL existing passwords*, and *Save changes* wrote whatever was
  typed into it. The server mixes the pepper into Argon2id and scrypt hashes only, so a new pepper - or one set for the
  first time - stops every password stored that way from working, while PBKDF2 hashes, the default, are not peppered.
  The field is captioned *Password pepper* now, with a warning under it that says just that, and a save that would
  write a new pepper on a server with accounts asks first, naming how many accounts the server has, with Cancel the
  answer Enter gives; a No writes nothing on the page.
- **Fixed: the Control Panel's Administrator roles page offered the whole server by default.** Its role box opened on
  the first role the server lists, `serveradmin` - *Everything, in every domain* - beside the primary *Give the role*,
  so an address typed and that button pressed handed the account full authority over the server, without a question.
  The box opens on no role now, saying *Choose a role*, and `serveradmin` - the role that holds `administer_server`,
  everything in every domain - is asked about before it is given, naming the account and the role, with Cancel the
  answer Enter gives.
- **Fixed: a SURBL server or DNS blacklist added in the Control Panel started switched off, and a DNS blacklist saved
  with no expected result was never a match.** *Add SURBL server* and *Add blacklist* opened on an unticked *Active*
  box, listed first, and on an empty *Expected result* with nothing to say what goes in it. The server expands that
  field into the answers that mean listed, so a DNS blacklist with none matches no answer at all: an administrator who
  added `zen.spamhaus.org` and pressed Save had added a list that was never asked, or never counted. A new entry is
  active now, its tick box after the fields that say what the entry is - on these two short dialogs and an account
  rule's, where a long dialog such as an external account's keeps its switch first, in sight; the DNS host and, on a
  DNS blacklist, the expected result are required, and refused empty on the field - both at once when both are
  empty, the keyboard put in the first; each field says under it what it wants - example
  hosts, the `127.0.0.2`, `127.0.0.2-11`, `127.0.0.*` and `|` forms the server reads, and that a SURBL server with no
  expected result takes any answer but the list's own error codes; and the dialog's button says *Add* when it adds and
  *Save* when it edits.

- **Fixed: on every tabbed page and dialog of the Control Panel, the selected tab's weight leaked into the whole
  tab.** The tab strip made the selected tab's header SemiBold by setting the weight on the tab item itself, and a
  tab's page is that item's child, so every description, field label, hint and editor value on the open tab that did
  not set a weight of its own came out SemiBold, as heavy as its card titles - on the 31 tabs of the settings pages,
  single-tab pages included, and in the domain, account, route and IP range dialogs - while a page without tabs showed
  the same text Regular. The 14-pixel size set on the tab item reached the tab's page the same way. Both are now set
  on the tab's header alone, so a tabbed page reads as the pages without tabs do.
- **Fixed: a check box or radio button caption longer than its card or dialog was cut off mid-word.** A caption given
  as text never wrapped: the setting profiles page's consent sentence ended at "...how long data is kept on" in every
  language and at every window size, so its consequence was never seen; the account's auto-reply "Reply to colleagues
  only..." and the API key dialog's "Read-only..." ended mid-word; the German backup page's verify caption was cut; and
  the folder permission entry's "Post..." was wider than its dialog. Every such caption now wraps under itself, keeps
  its Alt key and is still the box's name for a screen reader. And the four captions that needed more than a line say
  less, with what they meant in a note under the box that is also the box's help text, so a screen reader reading the
  box still hears it: the profiles page's box reads "I confirm the changes marked above." and its note says what the
  marked changes do and that what a shorter window deletes, putting the settings back does not bring back; the
  auto-reply's "Reply to colleagues only" has the outside senders' fate under it; the API key's read-only choice reads
  "can list and read; every change is refused"; and the permission entry's "Post" has the Send-As rule under it.

- **The Control Panel's Ctrl+K palette marks the result it will open the way every list marks its selected row.** The
  highlighted result was WPF's own solid accent band with the palette's text over it, near-black on blue in the light
  theme (3.6:1 for the title, 2.7:1 for the caption) and white on light blue in the dark (2.3:1 and 1.9:1) - the one
  solid accent band in the application, and the hardest text in it to read. The highlighted result now takes the
  lists' selection: a tint of the brand colour so near the page's own that the title and the caption keep their
  colours at 12:1 and 6:1 or more, and at the left edge the brand pill the sidebar marks the page it is on with. A
  test holds the theme's primary and secondary text to 4.5:1 on that tint over the page, a card and a dialog in both
  themes. Found by the project's own review.
- **A selected row looks selected in every list of the Control Panel, and looks the same in all of them.** A selected
  row of a grid - groups, administrator roles, IP ranges, routes, rules, the queue, every list of entries - was the
  same light grey as a row under the pointer, 1.08:1 against its neighbours, so with the pointer on another row two
  rows looked selected while Edit and Delete acted on one of them; the domains page's four lists drew a grey with an
  outline; the public folders and the member lists of the group, folder permissions and distribution list dialogs
  drew the solid accent band. All of them now draw one selection: the sidebar's brand pill at the row's left edge,
  with a tint of the brand colour behind the row - the sidebar and the rail keep their subtle grey behind the row
  they mark with the pill - hover kept to the grey alone, and under High Contrast no tint but an outline in the
  highlight. The tint is a design token (`AppSelectionFillBrush`, with `AppSelectionOutlineBrush` for High Contrast)
  that `docs/ControlPanelDesign.md` names, and the three dialogs' lists sit on a card as the public folders list
  does. A row the keyboard is on in a list wears the focus ring in the focus colour, as a sidebar row does. Found by
  the project's own review.
- **An inactive domain or account on the Control Panel's domains page says so legibly.** The whole row was dimmed to
  55 %, the word "inactive" in italics with it, which made the word the least legible text on the page (2.4:1 in the
  light theme) and the name 3.9:1. Only the glyph dims now; the name takes the secondary text, which clears 4.5:1;
  and the word is an *Inactive* pill after the name, in the neutral box the rest of the Control Panel uses for a state
  that is not a warning, at full strength on a plain, a hovered and a selected row alike. Found by the project's own
  review.
- **Every row delete on the Control Panel's domains page is the same destructive button.** A domain's trash glyph and
  an account's Delete painted the danger colour as a local colour, which measured 3.3 and 3.5:1 on the button in the
  dark theme and 1.8:1 on the selected domain, and outranked the button's disabled state; an alias's and a
  distribution list's trash glyphs had no danger colour at all - four looks for one action on one page. All four take
  the `DangerTextButton` style the other list pages use, whose shade of the danger colour clears 4.5:1 on the button
  in both themes and greys when the button cannot be pressed. Found by the project's own review.
- **The Control Panel's Browse Active Directory dialog lists the accounts it finds in the theme's own grid.** The
  results were the one stock Windows list view in the application: in the dark theme a white box with light column
  headers, separators and an empty filler column inside the dark dialog, in the light theme a dark outline and centred
  headers, unlike every other list of rows, and fixed at 300 pixels. They are a read-only grid on the Control Panel's
  grid styles now - the same headers, hairlines, hover and selection as the groups or routes pages - whose three
  columns share the width and end a long value in an ellipsis, seven rows at least and growing with the results
  before it scrolls. A double-click or Enter still chooses the row. Found by the project's own review.

- **Fixed: the Control Panel's sign-in card was cut off in a short window, and nothing scrolled.** The card is about
  560 pixels tall and the smallest window the Control Panel allows (760 by 520) leaves about 420 for it, so the card's
  bottom edge ran across the password box, and Connect, the failure notice, the two-factor link and the language
  picker were not drawn and could not be scrolled to; only Enter in the password box still signed in. The page scrolls
  now, the card stays centred in a window it fits in, a failed sign-in scrolls its notice into view, and the card is 24
  pixels shorter (a smaller logo, a smaller gap under the password), which is that much less to scroll.
- **Fixed: selecting a certificate on the Control Panel's SSL certificates page collapsed the list to a strip.** The
  list was a grid's star row between rows sized to their content, so when the details card appeared under it the list
  shrank to six pixels in a 1280 by 800 window: the selected row, the other certificates and the column headers all
  vanished while the arrow keys still changed the selection unseen, and the only way back to the list was a taller
  window; while the first inspection ran, its loading ring was never seen either. The page is laid out by `FillPanel`
  now, as the IP ranges page is: the list takes the height the details and add cards leave, down to four rows, below
  which the page scrolls.
- **Fixed: nine of the Control Panel's list pages lost their list in a short window.** Each put its list in a star
  row of the page's grid between rows sized to their content, the shape that gave the IP ranges page no list (work item
  79), so in the smallest window (760 by 520) the TCP/IP ports page had no grid at all between its toolbar and its
  listener warning, the setting profiles page lost its table and its "What it cannot do" list and nothing scrolled, the
  diagnostics page's results card had no height and its consistency footer was cut off, the greylisting bypass list and
  every other collection page (SURBL servers, DNS blacklists, the white lists, blocked senders and attachments, the
  allow and block list, remote domains, server messages, groups) showed its header row alone, the quarantine, domain
  white and black list and routes pages one row or half a row, the discovery page's lines had no room for a row, and
  the suppression list's bounce counts were a sliver. Every one of the nine is laid out by `FillPanel` now: the list
  takes the height the other parts leave, down to three rows (the discovery page's two lists share it equally, the
  suppression list and its bounce counts two to one), below which the page scrolls, and every other part - a toolbar,
  a notice, an add form, the diagnostics inputs and the consistency card - always keeps its own height. The ports,
  routes and domain white and black list pages' toolbar actions wrap in a narrow window, as the IP ranges page's do,
  rather than running off the right edge, and the bounce counts are a titled card, as the discovery page's lines are.
  `ControlPanel.Tests` holds every one of the eleven pages to the shape. A filling part's floor is on the device-pixel
  grid, so that a page whose parts fit the window shows no scroll bar: handed out as the part's share, a floor
  between two device pixels is asked for a fraction of a pixel over, and at 150 % scaling the bounce counts card's
  floor of 193, which is 289.5 of them, left the Suppression list page a third of a pixel taller than a 1280 by 800
  window it fit, which layout rounding made a whole pixel and a scroll bar the height of the page, over the right
  edge of the notice and both cards, in English and Japanese. The scrolling part of every page laid out by
  `FillPanel` - the IP ranges, domains and rules pages' too - keeps 12 pixels clear of its scroll bar on the right,
  which is drawn over the content: its thumb lay over the rounded corner of *Delete selected*, the right border of
  the list card and the end of the notices wherever a page scrolled.
- **Fixed: on the Control Panel's Domains & accounts page, the Add domain form and the alias and list forms were below
  the fold in a 1280 by 800 window.** The detail column's floors are taller than the page, so the page scrolls, the
  domains card stretched to the column's height, and its Domain box and Add domain sat at the foot of the card in the
  hidden bottom 90 pixels - on an empty server the card said "No domains yet. Add the first one to start receiving
  mail." with nothing to press on screen - and the alias form was cut at its caption and the list form was off screen.
  Every card's add form is at the top of the card now, under its search where it has one, with the list under it, so
  the thing to do is on screen whatever the window's height; and the empty domains state offers *Add a domain*, which
  puts the keyboard in the Domain box.
- **Fixed: the Control Panel's Suppression list cut its Source column short and kept Since and Expires off the card,
  and in the narrowest window lost its Add…, Remove and Refresh buttons.** The address column took two stars and about
  670 of a 1280-pixel window, so Source read "Refuse" and "An ad" and the two date columns were not drawn, and when an
  entry lapses could not be seen; the bounce counts' address was cut to "gone@old-provider.exampl" with no ellipsis
  while its date columns had room to spare; and the domain box sized itself to the longest domain name, so with one
  of sixty characters it filled the toolbar in a 760-pixel window, its own chevron cut off, left the three buttons
  no width, and in a 1280 one pushed Refresh onto a second line. The three buttons are in the page header now, as the
  delivery queue's are; the box is captioned *Sending domain*, with an Alt key, capped at 320 pixels, a name wider
  than it ending in an ellipsis and whole in its tool tip; and every column keeps a width - the address and the
  source share what is left between them, each over a floor and trimmed with an ellipsis, Why is 140 pixels and
  trimmed too, Status is 84 or its header's width (84 holds ステータス, which a floor of 64 cut to "ステー…" in the
  smallest window, where the header is not always counted once the grid scrolls sideways), the six counts 64 or
  theirs, Since, Expires and the dates 150 and Listed 70 - so a narrow window scrolls a grid sideways rather than
  squeezing a column out. No column of either grid is narrower than its own header, measured in the header's
  font, so that the counts' *Mailbox full* and *Last status* are whole in Japanese (メールボックス満杯 and
  最後のステータス), where a count's floor of 64 and Last status's 90 cut them to "メール…" and "最後のステ…" in
  the smallest window.
- **Fixed: the Control Panel's distribution lists card cut every address to about ten characters beside two
  10-pixel word buttons.** Properties and Members were words at 10 pixels, the only text in the Control Panel below
  the 12-pixel caption size, and with the trash button took most of a half-width card from the address, the row's
  one datum, which read "everyone@e..." and "board@exa..." in a 1280 by 800 window. The three are glyphs in 24-pixel
  boxes now, as the domain and account rows' buttons are, each saying which list it acts on in its tool tip and to a
  screen reader ("Properties of everyone@example.com"), so the address has about 170 pixels; the alias rows' trash
  button has the same box; and a test holds the page to the caption size.
- **Fixed: in the Control Panel's distribution list members dialog the address box shrank to about 30 pixels in
  German.** The add row was a star-width box and three buttons, and "Hinzufügen", "Aus AD hinzufügen…" and
  "Ausgewählte entfernen" left the box 30 pixels, Japanese 100 and English 155, too narrow to read an address in. The
  box has the dialog's width now but for Add beside it, and *Add from AD…* and *Remove selected* are under the list,
  where they act, wrapping onto two lines in a language where they do not fit on one.
- **Fixed: the App passwords tab of the Control Panel's account dialog cut Delete off and did not draw Refresh.** An
  inline "Name", a 220-pixel box, Create, Revoke / restore, Delete and Refresh sat in one horizontal row, so in the
  dialog's 592 pixels Delete lost its right edge and Refresh was off the tab; the panel also started four pixels
  further in than the other tabs. The name is a captioned field with its Alt key, with Create beside it; Revoke /
  restore, Delete and Refresh are under the list, where they act, the first two enabled only while a row is selected
  and Delete drawn as every list page's Delete is; a name left empty is said on the field, with the keyboard put in
  it; the panel starts where the other tabs do, and its list and its one-time password are on the component card.
- **Fixed: the Control Panel's collection grids were headed by the dialogs' sentences, cut until the columns could
  not be told apart.** The greylisting bypass list's SPF, DKIM and DMARC columns were all headed "Requires…" at 80
  pixels ("Requir…" in a 760-pixel window, "Erfordert…" three times in German); the allow and block list's "Block
  rather than allow" read "Block rath…" and its cells Yes and No, and the expiry date itself was kept off the grid,
  which showed only whether one was set; the domain dialog's Names tab carried an example in its header ("Alias
  domain name (e.g. example.net)"); and the account dialog's External tab left the server, its one star column,
  about 40 pixels ("Ser…") beside five fixed ones. A field can name its column's header apart from its caption now
  (`FieldSpec.GridLabel`), the caption staying the header's tool tip and help text, and a Bool's cells can be words
  of their own: the bypass list is headed SPF, DKIM and DMARC, reading *Required* or a dash; the allow and block list
  has an *Action* column reading Block or Allow and one *Expires* column showing the date, or *Never*; the Names tab
  is headed *Alias domain*; the External tab's server type is off the grid, so the server keeps its width, and its
  connection security is headed *Security*; and the remote domains grid is headed *Inbound TLS*, *Largest (KB)*,
  *Connections*, *Recipient check* and *Encryption* where the sentences were cut. Every column of a collection grid
  keeps its width now, as the IP ranges page's do, so that a grid wider than its card scrolls sideways rather than
  squeezing its columns - given less than its columns ask for, a grid took the difference from them, which cut the
  remote domains grid's headers at every width their fields named - and such a grid keeps its vertical bar, as the
  rules page's list does. A test holds every narrow column whose caption is longer than a few words to a header of
  its own, and every column to its width. The domain dialog's allow and block list fits its tab: its address
  column is 200 pixels, where 240 beside Action, Reason and Expires was 36 more than the 564 the tab gives the
  grid, and scrolled it sideways under a bar in every language at the only width the dialog has.
- **Fixed: the Control Panel's Create a REST API key dialog clipped the expiry's year, had no heading, and reported
  a refused value in a message box.** Its 200-pixel date picker showed the long date, "Wednesday, January 6, 2" (in
  German "Mittwoch, 6. Januar 202"), and it was the one dialog off the standard frame: no heading, notes at 65 %
  opacity, and a label, a date, a domain list or a source address the store would refuse was reported in a message
  box after Create key was pressed. It is on the frame now, with the "shown once" note as its description, each field
  a captioned row with its hint under it, the expiry in the short date beside quick picks of 30 days, 90 days and a
  year, and a refused value said on the field at fault, with the keyboard put in it.

- **Fixed: in a Control Panel window narrower than 1,000 pixels, the navigation rail led nowhere.** Below that width
  the sidebar is the rail - one glyph per group - whatever the administrator chose, and a glyph's click cleared the
  choice to collapse it, found the window still too narrow, and expanded the group inside the tree that was not
  shown: nothing happened on screen, while the glyph stayed marked as if its group held the page. The breadcrumb's
  group button did the same. In a maximised window narrower than that - 1280 by 800 at 150 % scaling is 853 pixels -
  the sidebar toggle could not widen the window either, and the only ways to another page were Ctrl+K and the
  *Related* links. While the rail shows, a glyph now opens the group's pages in a list beside it, and the breadcrumb's
  group button under itself: the arrow keys move through the list, Enter opens the page the keyboard is on and a click
  the one under the pointer, Escape or a click outside closes it, the keyboard goes back to the glyph or to the
  breadcrumb's group button - the one the breadcrumb was rebuilt with, where a page was chosen from it - and the rail's
  mark stays on the group of the page on screen. Above the threshold a glyph expands the sidebar on its group, as before.
- **Fixed: on the Control Panel's Domains & accounts page, a domain, an account, an alias or a distribution list could
  not be opened, deleted or managed from the keyboard.** The four lists put their verbs in buttons inside each row,
  which Tab never reaches - a list is entered on its selected row and left on the next Tab, and the arrow keys only
  move the selection - and nothing answered Enter, Delete, a double-click or the right mouse button. Each list now
  opens the selected row's properties on Enter and on a double-click, deletes it on Delete with the confirmation the
  button asks, and offers the same verbs - *Properties…*, *Members…* for a distribution list, *Delete domain*, *Delete
  account*, *Delete alias* or *Delete list* - on a context menu from Shift+F10, the Menu key or the right mouse button,
  under the selected row from the keyboard and at the pointer from the mouse; after each, the keyboard is back on the
  row that is selected, where the reload that follows used to leave it nowhere. The buttons stay for the mouse, and
  both roads lead to one method per verb.
- **Fixed: the Control Panel's notices were silent to screen readers.** Every notice, loading state, empty state,
  field error and result line declared itself a live region, which only promises an announcement: a reader says the
  region when the element raises the live-region event, and nothing in the Control Panel raised it, so "Released 3
  messages", "Deleted.", "Could not load…", a failed sign-in and every Save or Test outcome reached a sighted
  administrator and nobody else; and the result lines under *Clear logon-failure list* and the never-ban list's
  buttons carried a fixed accessible name, so a reader that was moved to one said the label in place of the outcome.
  A new `Announce` helper raises the event once per change, after the layout that shows the element and only while
  it is visible, and `InlineNotice`, `LoadingState`, `EmptyState` - now a polite live region like the loading state
  it follows - a `FieldRow`'s error and the two result lines call it; the result lines are named by their text.

- **Fixed: the count beside a Control Panel list's title put an English plural ending on a translated noun.** The
  pill on every list the generic editor draws - SURBL servers, DNS blacklists, the white list, blocked senders, the
  allow and block lists, the greylisting lists, blocked attachments, groups, remote domains, server messages, and the
  aliases, external accounts and rules in the domain and account dialogs - was the count, a space, the noun translated
  on its own and then an English `s`, `es` or `ies`: German read *3 Sicherheitsgruppes* and *2 Listeneintrags*,
  Japanese *5 ルールs*. The REST API keys page counted its keys the same way. Each count is now one text of the
  catalogue per number, *1 security group* and *{0} security groups*, translated whole in every language, so that
  Russian can put the noun first, *Групп безопасности: 3*, and Japanese its counter after the number. The
  localisation checker now refuses a count joined to a text with a space, which is what the two helpers did, so the
  shape cannot come back.
- **Fixed: the verdicts and notes of the Control Panel's transport encryption and virus scanning overviews were
  put together from translated fragments and read as nonsense in most languages.** A verdict was a counted noun
  with its verb (*client listener has* / *client listeners have*), a pronoun (*it cross* / *them cross*) and a
  template around them, each translated apart, so Polish read *przechodzą przez przez sieć*, German *kann es aber
  nicht* and Czech *z ten neběží*; a certificate note was *expires in {1}{2}{3}* with *day* or *days* and a trailing
  clause, which German rendered *vor 5 Tage* and Japanese with the clause before the verb. Every verdict and note
  is now one whole sentence per case - one listener or several, a certificate that expired earlier today, yesterday
  or days ago, expires today, tomorrow or in days, in use or not, TLS 1.0, 1.1 or both, one scanner or several, one
  fetch account or several with and without *at least*, one blocked pattern or several, and who is told when an
  infected message is deleted - translated whole in every language, and the Control Panel's tests pin the English of
  each. The day a certificate note names is read from the calendar date the certificate expires on, not from the
  count of whole days left, whose zero covers a certificate that expires at six tomorrow morning: that one *expires
  tomorrow*, one that expired at five this morning *expired earlier today* rather than *yesterday*, and the day count
  in the certificate's own row on the page is the same calendar count as its note's.

- **Fixed: the TCP/IP ports page's Listening column cut its word short in Japanese once the page kept clear of its
  scroll bar.** In a 1280 by 800 window the column read "待ち受けていませ" for 待ち受けていません (Not listening), the
  last glyph cut with nothing to say so: the grid's fixed columns shrink to their floor once the grid is narrower than
  they are and the certificate column has given up all it can, none of them had a floor of its own, and the 12 pixels
  the page now keeps clear of its scroll bar took three from the Listening column, which had held the word by that
  much. No column of the grid is narrower than its own header now, or than the widest of its words in the current
  language - the security and authentication names, the listening word with its mark, the widest IPv4 address, the
  highest port - measured as the window draws them, with the cell's margin and padding, so a narrow window scrolls the
  grid sideways rather than cutting a word, as the Suppression list's grids already do; a 1280-pixel window has room
  for every floor in every language.

- **Fixed: a notice's severity word could stay English in another language, and Japanese left three status words in
  English.** The word a notice or a status pill puts before its sentence - Warning, Critical, Information, OK - was
  translated once, when the first page touched the status vocabulary, so whatever language was in force at that
  moment was the language of every severity word until the Control Panel closed; the review's German renders read
  "Warning 11 brauchen eine Maßnahme" and "Critical 8 Einstellungen…" for that reason, and nothing in the start-up
  order guaranteed the product would not do the same. The word is now kept in English and translated each time a
  notice or a pill draws it, the level Good's "OK" included, which was the one word never translated. The Japanese
  catalogue says 重大 for Critical and 通常 for Normal where it said Critical and Normal, and アクション for the rule
  card's THEN beside 条件 for its IF; Russian and Ukrainian say Критично for Critical, where Russian had a plural
  adjective. `build/check-catalogues.py` reports a key of ordinary English words that a Japanese, Chinese, Russian or
  Ukrainian catalogue gives back unchanged, unless it is a name every language keeps, and its `--self-test` holds the
  rule to the three Japanese words; `SeverityWordTests` holds the word to the language in force when it is read.

- **Fixed: the Control Panel wrote "(s)" after a count, and counted in English grammar in every language.** Forty-three
  captions said "message(s)", "setting(s)", "item(s)" and the like, which reads as a form to fill in rather than a
  sentence and which no translation can carry, and twenty-six more chose between a singular and a plural sentence by
  asking whether the count was 1, which is English grammar: Czech, Polish, Russian and Ukrainian have a third form for
  counts such as 2 to 4, Russian and Ukrainian say 21 and 31 in the singular, French and Brazilian Portuguese say 0
  in the singular, and Japanese and Chinese have one form for every count. The Control Panel now counts through
  `Loc.Plural` and `Loc.PluralF`, which take the count and the English singular and plural and pick the form the
  language's CLDR plural rules give that count; the Czech, Polish, Russian and Ukrainian catalogues carry the third
  form of each, under the plural's key with "|few" after it. 119 places count that way and no "(s)" is left; where a
  sentence counted two things at once, each count is now a sentence or a labelled number of its own ("Messages: 4,
  files compressed: 2"), and where the English says "One ..." for exactly one, it still does. The same pass spells
  reinitialise the British way in each of the fifteen captions that named it (the COM method keeps its name,
  Reinitialize). Every new and changed sentence is translated in all seventeen languages. `build/check-localisation.py`
  reads each Plural call, refuses two forms with different placeholders, requires the "|few" form in the four
  languages that need it and keeps it on `--write`, and refuses "(s)" in a marked text, a plural ending in brackets
  in any translation - French "{0} expiré(s) et désormais refusé(s)" and Spanish "caducada(s)" in the API keys
  summary, and French "configuré(s)" in the spam overview, were the last, each now a Plural of its own - and an
  `== 1 ? L(...) : F(...)` choice; `build/check-catalogues.py` holds each "|few" form to the English of its plural;
  `PluralTests` holds the forms to the counts that tell them apart in each language.

- **Fixed: Control Panel titles and sentences were glued together from translated pieces.** A title put a translated
  fragment before a value - "MX — which server takes the mail for " and the domain, "Account - " and the address,
  "Add {0}" with a noun in it - and a paragraph added a sentence that began with a space to the one before it, so no
  translator could move the value or the verb: German read "... die Mail annimmt für example.com", Japanese "SURBL
  サーバー を追加" and put a space after its 。. Each is one whole text now, with the value in its place: the account,
  domain, distribution list, route, members, recipients and permissions dialogs' titles, the DNS records page's MX,
  SPF, MTA-STS and TLS-RPT cards (with one spaced hyphen in every card title) and each record's type with its host
  note ("Record type: MX. Host/Name - the domain itself ...:"), the DKIM and DMARC findings, the update row with and
  without its date, the cache line with and without its cap, the client certificate and HELO name warnings, a rule
  action's subject and route, and every Add and Edit dialog of the list pages, which says what it adds ("Add a SURBL
  server", "Edit the security group"). Sentences that follow one another are joined by `Loc.Sentences`, which puts no
  space after a full-width stop - among them the DNS records page's verdicts and what each found, its MTA-STS policy
  note, the setup checklist's summary, the Dashboard's note, the quarantine's partial result, the install refusals,
  the REST API's refusal of a credential and the settings pages' notices - and names of which any will do by
  `Loc.JoinOr`; a failure and its reason are joined by one colon ("Search failed: ..."). Every new text is translated
  in all seventeen languages. `build/check-localisation.py` refuses a marked text that begins with white space, or ends
  with it after anything but the colon of a label, and a translated text joined to another with a space literal;
  `WholeTextsTests` holds the catalogue to the same rule, no Japanese or Chinese text to a space after its 。, and
  every list page's dialogs to their whole titles.

- **Fixed: the overview pages promised rows that open a page, and the transport encryption overview showed 0.0.0.0 as
  an address.** The transport encryption, spam and virus overviews said that each row opens the page that owns the
  setting, but no row can be clicked: the button at the foot of each section is what opens a page, and the subtitles
  now say so. Under each listener the transport encryption overview read "0.0.0.0", the server's way of storing "every
  address", which it now shows as "All addresses" (and the same for :: and an empty binding). Both are translated in
  all seventeen languages; `OverviewHonestyTests` holds them.

- **Fixed: the rule card's Up button was English in every language, and Down wrapped onto a line of its own.** The
  THEN list's Up and Down buttons were captioned words, and Up's was never translated (Japanese read "アクションを削除(M)
  | Up | 下へ(N)"); at 1280 wide the card's five buttons did not fit, so Down sat alone below the rest. Up and Down are
  arrow glyphs now, named "Move action up" and "Move action down" for a screen reader and explained in their tool
  tips in all seventeen languages; Alt+Up and Alt+Down in the list move the selected action as well, and the moved
  action stays selected so that a second press moves it again. `build/check-localisation.py` now sees a one-word
  caption split by its access key, as "U_p" was, and its self-test holds it; `RulesActionOrderTests` holds the
  buttons, the keys and the translations.

- **Fixed: the SURBL servers and DNS blacklists pages counted entries the server does not ask, and named the refusal
  text as a command.** The pill beside each title counted every entry - "3 blacklists" - while the server asks only
  the active ones, and the spam overview said "2 active of 3" in its list rows and "2 of 3 active" in its pipeline
  rows for the same list; all three now say "Active: 2 of 3", the pill at Warning when none is on. The column and
  field holding what the sender is told were "Reject message", which German translated "Nachricht ablehnen" and
  Japanese as an instruction to refuse; they are "Refusal text" now, with the hint "What the sender is told when this
  list refuses the message", in all seventeen languages. Both pages show their columns in one order: Active, DNS host,
  Expected result, Score, Refusal text. `ActiveEntriesTests` holds the count the page takes from its rows (only a
  switch that reads true is on; an empty list, or one without a switch, takes none), its level, the overview's use of
  its words in every language, the order and the German and Japanese names.

- **Changed: the allow and block lists are named in one vocabulary in every language.** The lists had about five names
  each - white list, whitelist, white-list and allow list; blacklist, black list, block list, blocklist and real-time
  blackhole lists - so the domain list needed a notice to tell them apart, and the quarantine said "Whitelist the
  sender" while writing to the allow and block list. By the owner's decision the White list page is now **Spam
  exemptions**, the Greylisting white list **Greylisting exemptions**, the Domain white and black list **Allowed and
  blocked domains** (its entries allowed and blocked), and the DNS blacklists **DNS block lists (DNSBL)**, each page's
  heading the same as its name in the navigation; the quarantine's actions are "Allow the sender, then release" and
  "Block the sender, then delete"; the per-recipient Allow and block list keeps its name. Every old name stays a
  search alias, so a search for "White list" or "Domain white and black list" still finds the page. Every catalogue
  follows: German says Zulassungsliste and Sperrliste, zulassen and sperren, where it said Whitelist, Positivliste and
  Weißliste for the same thing; and in the eleven languages that called the greylisting bypass list a list of
  exceptions it is now named for bypassing, so that it reads apart from the greylisting exemptions beside it. A
  sentence that needs a list keeps its noun: the quarantine offers the sender's domain "on the list of allowed and
  blocked domains", beneath the address "on the server's allow and block list", and the notes speak of a sender "in
  the spam exemptions" and an address that "matches one of the greylisting exemptions", where the new names stood in
  for the list itself. `AllowBlockVocabularyTests` holds the names, the aliases, the headings, the English and German
  catalogues to the one vocabulary and every language to the four names, and no English text to putting a thing "on" a
  name that is not a list.

- **Fixed: the German Control Panel said one thing in several words, and some of them wrongly.** It said Domain and
  Domäne on one screen; Sperrliste for the suppression list as for the block lists; "Läuft ab 2026-10-20", which reads
  as "runs from"; Übersicht for the quarantine digest as for the overview pages; Aktualisieren, a verb, as the label
  of the Update row; Vertrauensanker-Ausnahme for the trust anchors one sets oneself; Auto-Bann beside Sperre for one
  ban; Punktzahl beside Bewertung for the score; Zugangsdatum for a credential; and "die ausgelieferte Vorgabe" for
  the shipped default, where ausliefern is what a mail server does to a message. The German catalogue now has one
  glossary: a mail domain is a Domain (only a Windows or Active Directory domain stays a Domäne), the suppression list
  is the Unterdrückungsliste, an expiry "Läuft am 20.10.2026 ab", the digest a Digest, the row label Aktualisierung,
  the trust anchors Eigene Vertrauensanker, an auto-ban a Bann (a name's lockout stays a Sperre), the score a
  Bewertung, a credential Anmeldedaten and whatever this version ships with werkseitig - the Setting profiles page's
  button is "Werkseitige Einstellungen behalten", where it said "Ausgelieferte Einstellungen behalten" above the line
  "Die werkseitigen Einstellungen bleiben" that the same click wrote; nearly four hundred texts follow.
  `build/check-catalogues.py` now holds a reference to any page in the navigation to that page's translated title,
  where it knew thirty: the German "on the Suppression list page" had kept the old name, and the Finnish, Japanese and
  Chinese named the Administrative access page otherwise than its title. `GermanGlossaryTests` holds each, and refuses
  ausliefern in the translation of any text that says shipped, its access key's underscore taken out first.

- **Fixed: the Control Panel's settings pages said in three ways whether what was on screen had reached the server,
  and the feature settings pages said it wrongly.** Transport security, Certificates (ACME), API & monitoring,
  Server limits & expert settings, Authentication, DNS resolver and Web services & autoconfiguration wore a grey
  *Editing* pill as they opened, with nothing changed, and never marked an edit: nothing but the computed warnings
  listened to their editors. After a save the pill turned a green *Saved*, though a setting that waits for a service
  restart was not running yet, and it stayed green through every edit after it. The server settings pages said
  *Saved* in green over a save whose settings waited for a reinitialise or a restart too. Every settings page now
  keeps the one model the server settings pages began: no pill while the page shows what it read; *Unsaved changes*,
  a warning, from the first edit - on the hMailServer.ini pages for as long as an editor differs from what was read
  or saved, so that an edit undone by hand takes the pill back to what the save had said; and after a save, until
  the next edit, *Saved* when every setting written is in effect, *Reinitialise to apply* when some wait for **Apply
  now**, or *Restart to apply* when some wait for a service restart or the server cannot say what waits. A save that
  could not be written says why in a Critical notice under the header. The per-address overrides box of the
  per-account sending limits now says when it changes as well, and a test holds every setting with a value to doing
  so.

- **Fixed: the Directory authentication (LDAP) and Directory synchronisation pages kept Reload and Save changes in a
  footer, where their result was grey caption text and a failed save a message box.** Every other settings page in
  the Sign-in & access group puts its actions at the top right of the page header; these two put them at the foot of
  the window, under a scroller whose last card simply stopped, beside the "Editing" sentence in a 12-pixel caption,
  and in a 760 by 520 window the sentence ran into Reload. Both pages now carry Reload and Save changes in the page
  header, with Save the primary and the same automation ids, the file they edit in a caption under the header, and
  the one save-state model of the settings pages: a pill beside the title that says *Unsaved changes* from the first
  edit, compared with what was read, and *Saved* after a save, its sentence in a notice under the header, and a save
  that could not be written, or that the page refused - Directory authentication's invalid port or timeout, or a new
  service account password typed with its removal ticked - in a Critical notice, brought into view with the header
  above it, where Directory synchronisation said it in the same grey caption as its success and Directory
  authentication in a message box. When hMailServer.INI is not on this computer, both say so in that notice beside
  an *Unavailable* pill, where Directory synchronisation said it in the footer's caption. The page scrolls whole, as
  the feature settings pages do, so the smallest window keeps the cards. Directory synchronisation says *Restart to
  apply* - after a save, and when the page is opened again before the service has restarted - while directory access
  is on and the schedule in hMailServer.ini runs at another interval than the one the server registered as it last
  started or reinitialised - none, when it started with directory access off - where it said after every save that a
  change to the schedule needs a restart. A schedule changed while directory access is off, or to minutes the server
  runs alike - 5 and 15, an empty value and 0 - waits for nothing. Directory authentication says the same, with the
  restart, when its save switches directory access on over a schedule the running server did not register, where it
  said that no service restart was needed. And Directory synchronisation now notices when hMailServer.ini refused a
  write, which it reported as saved.

- **Fixed: the lists on the domain and account dialogs' tabs saved as they were edited, under a dialog whose Cancel
  suggested nothing had been.** Adding, editing or deleting a domain alias or an entry of the domain's allow and
  block list, an external account or one of the account's rules writes it to the server at once, and pressing Cancel
  afterwards keeps it and discards the rest of the dialog; the Levels tab says its changes are written at once, not
  with the dialog's Save, and these four tabs said nothing. Each now says under its hint - the Rules tab where the
  page's header would be - "Changes here are saved as you make them, not with the dialog's Save, so Cancel does not
  undo them." A test holds every list built for a dialog's tab to saying so.

- **Fixed: the Control Panel's pending bar had nothing to press for the settings that wait for a service restart,
  and never said which settings waited.** "2 saved setting(s) wait for a reinitialize, and 1 for a service restart."
  had one button, Apply now, which takes only the first; afterwards the bar went on saying "1 for a service restart"
  with nothing to press, though the feature settings pages restart the service after a save themselves. The bar now
  says what each button takes - "2 saved settings wait for a reinitialise - Apply now takes them. 1 waits for a
  service restart." - in whole sentences per count in every language, and carries *Restart the service…* beside
  Apply now while a setting waits for a restart and the Control Panel is signed in to the server on its own machine,
  the only one it can restart. It asks first, naming how many settings wait and that every connected client is
  disconnected, with Cancel the answer Enter gives, and runs the restart the feature settings pages use, now one
  shared flow (`Services/ServiceRestart`). The bar's tool tip and help text name the waiting settings by their
  captions and pages, under what each waits for. Directory synchronisation offers the same restart beside a saved
  schedule that waits for one. Neither of the bar's buttons carries an Alt key: the bar stays under every page, in
  the same access-key scope, until what it counts is applied, and no letter of either caption is free on all of
  them - Apply now's Alt+A was taken on eighteen pages, by Add among others, and on such a page the key only moved
  the focus between the two buttons - so both are reached by Tab, Restart the service… first, in the order they are
  drawn.

- **Fixed: a secret stored on the Control Panel's feature settings pages could not be removed.** The shared HMAC
  secret and the introspection client secret on the Authentication page, the SRS and BATV signing secrets on
  Transport security, the password pepper and the other secrets said "A secret is configured - leave blank to keep
  it", and blank kept it while nothing cleared it; the Directory authentication page had always had a box for its
  service account password. While a secret is stored, a box under its field now reads *Remove the stored secret when
  saving*, and the page's save writes the empty value that is no secret, after which the field says none is stored.
  Ticking it is an edit like any other, as a value typed or filled in by **Generate** is - the pill says *Unsaved
  changes* and leaving the page asks - and the computed warnings that ask whether a secret is set follow it, where a
  value **Generate** filled in changed neither, as the box was watched for typing alone. A new value typed with the
  box ticked is refused before anything on the page is written, in the notice under the header, as the Directory
  authentication page refuses its own; and removing the password pepper is asked about as changing it is, since it
  stops the passwords peppered with it.

- **Fixed: the IP ranges page's permissions card said a save by adding "(saved)" to its title, and said nothing of a
  change.** The card now keeps the settings pages' model in a pill beside *Save permissions*: none while the boxes
  hold what the range has, *Unsaved changes* from the first box ticked or cleared, and *Saved* after a save until
  the next change.

- **Fixed: a lost link to the service was drawn at a different level on each page that said it, and on the Dashboard
  at none.** The Dashboard put "Connection to the server lost." in its grey subtitle with no mark, word or colour, and
  its setup summary added a caption counting the values it could not read; the setup checklist and DNS records called
  the same lost link a Warning, and Setting profiles called it Critical, as the shell's pill does. A lost link is now
  one event shown at one level. The Dashboard says a failed read of its statistics in a Critical notice under its
  header, with the reason and Try again, and keeps its subtitle; its setup summary says it cannot tell, rather than
  counting things that need attention from values that were never read, and values a server that answered could not
  give are a Warning notice in the card, as the setup checklist says them. The setup checklist and DNS records take
  their notice's level from the cause (`Services/ComErrorText`): Critical when the link went, Warning when the server
  answered and some values could not be read. The Dashboard's timer runs only while its statistics can be read; Try
  again starts it and checks the setup again. `ComErrorTextTests` and `FailureStateTests` hold the classification, the
  levels and the three pages to it.
- **Fixed: the Dashboard's KPI cards showed 0 as a count while the statistics could not be read.** Under a lost link the
  cards read "-", 0, 0, 0, 0 - the page's starting values - so "In queue 0" was a statement the page could not make,
  and after a loss in the middle of a session the last figures froze with nothing to flag them. Every card now starts
  as a dash. When the first read fails each stays a dash, named "not available" to a screen reader, with the queue's
  backlog badge hidden; when figures were read earlier they stay, in the secondary text colour, each named with the
  time it was read, and the subtitle says "Figures from 04:33:43, the last time the server could be read." A read that
  works puts the figures back in the primary colour. `FailureStateTests` holds the cards to their dash and the page
  to both cases.
- **Fixed: the Control Panel showed raw exception text and HRESULTs where other pages gave a sentence.** "Could not
  evaluate all warnings: The RPC server is unavailable. (Exception from HRESULT: 0x800706BA)" stood at the foot of
  Server status, the same text on the delivery queue, Diagnosing stalled mail, Backup & restore and Event scripts, a
  message box said "Could not save: " and whatever the runtime said, and Performance showed a JSON parser's position as
  a statistic. Every catch of a call to the server - eighty-eight of them, in thirty-two files - now goes through
  `ServerSession.DescribeComError`, which says a lost link in words, notices it and has the session repair it, and now
  also says what a failure that is not the server's own words is (`Services/ComErrorText`): an answer the Control Panel
  could not read - a number that was not one, a document that was not JSON, a value of another type - is "the server
  answered something this Control Panel could not read", and a member the server does not have is "the server has no
  such setting or action - it may be older than this Control Panel", each with the runtime's text in brackets. Each
  sentence follows a prefix that says what failed, so the places that had shown the runtime's text alone - Restore
  defaults on IP ranges and on TCP/IP ports, the administrator's second-factor dialog, the mailbox move and the sample
  domain on Welcome - now say first what could not be done, as in "Could not restore the default TCP/IP ports: " and the
  reason. The reason is also said last, since a sentence after the runtime's text in brackets ran on from it with no
  full stop between: the hMailServer.ini pages' notice read "... older than this Control Panel (Unknown name.) The page
  shows the values in hMailServer.ini ...", the settings pages put the server's environment after it, and DNS records
  and the spam, virus and transport overviews put that the cards or sections below may be incomplete after it; each now
  says what follows from the failure first and then "Why:" or "The first reason:" and the reason. The shared failure
  notice keeps the exception's type, HRESULT and text in its tool tip, and a statistic the server could not give is a
  Warning notice in place of the figure. The catches whose try is local - a file, the registry, a browser, an HTTPS
  fetch or an LDAP probe from this machine - keep their own text, and `FailureStateTests` names each with its reason, so
  that a new catch showing a failure's raw text fails it, and holds a message box or a dialog's notice to saying what
  failed before the reason, and every sentence the Control Panel formats a reason into to saying nothing after it.
- **Fixed: a settings card whose settings could not be read showed only its heading, and nothing said which card
  had lost what.** On the delivery, protocols, webmail, SSL/TLS, auto-ban, advanced, performance, anti-spam and
  anti-virus pages, a setting the server would not give was skipped - correctly, since no default is ever saved - but
  the only sign was "29 setting(s) could not be read" at the top of the page: the Rules tab showed only "Rules", the
  Thresholds & actions card was a bare heading, and on an older server the September greylisting settings simply
  vanished. Each such setting now keeps its caption and note on its card with a neutral *Not read* pill where its
  editor would be; the card says at its top, as a Warning, how many of its settings could not be read and that saving
  leaves them as they are on the server; and the page's sentence says the same of the page at the same level - one
  sentence per number - with the first reason last. A setting kept in hMailServer.ini whose read fell back to the file
  on this computer is one of them: the file's value may not be what the server runs, and under a lost link the
  delivery page's PIPELINING and CHUNKING boxes stood ticked and every idle timeout on Protocols was a live, editable
  number, on the same cards as rows marked *Not read* and left out of the page's count. When the link to the service
  went, the page says so once, at Critical, as every page says a lost link, and neither the cards nor a figure such as
  the message store's repeat it: their rows say *Not read*. `FailureStateTests` holds every road to an unread row - the
  REST key the server does not answer, the COM read that threw and the hMailServer.ini read that fell back - to the
  row, the count and whether the link went, and the page's sentence to its wording and its level.
- **Fixed: settings cards reached over the REST API said "Sign in to a server first." under a Connected pill, once
  per card.** Passkeys, OIDC, SAML, What people may do, OpenPGP, S/MIME, Web Push, Import, Reports, Managed mailing
  lists and Compromised accounts each carried that Warning - three times on the webmail page's Sign-in tab - when the
  shell showed a session and an administrator: the session simply held no credential the REST API takes. A session
  with no credential is now told that it holds no password the REST API can sign in with, and to sign in again with
  the administrator's user name and password; "Sign in to a server first." is left to a page with no session at all.
  When the REST API cannot be used at all - no credential, no listener, no answer, a session that could not be made -
  the reason is said once per tab, under the tab strip, as "These settings are read through the REST API, which this
  session cannot use." and the session's own sentence, with an *Open API & monitoring* button; each of the tab's REST
  settings is marked *Not read* on its card, so that no card is a bare heading. A refusal of one settings group alone
  stays on that group's card. `FailureStateTests` holds the sentence, the once-per-tab notice and its button, and the
  rows to it.
- **Fixed: the hMailServer.ini pages fell back to the file on this computer without a word, and claimed to edit it.**
  When the server could not be asked, Web services, DNS resolver, API & monitoring, Server limits & expert settings,
  Certificates (ACME), Transport security and Authentication read hMailServer.ini on this computer instead and looked
  as they always did - "Editing C:\...\hMailServer.ini", Save enabled - though since schema 6042 the database is the
  settings store and the file only its cache, which may not be what the server runs. `IniFeatureStore` now remembers a
  read that fell back, and why; such a page says that the server could not be asked, with the reason, that the values
  are the file's on this computer and may differ from what the server runs, and that nothing can be saved until the
  server answers, and its Save waits for the server: a save of values the page was built from the file with is
  refused and says why, the question before leaving the page included, and a read that falls back after a build that
  read cleanly - a warning's, on an editor change - no longer stops a save without a word, since the write itself then
  says why it failed. Its pill and its notice say so at the one level of the cause, as
  the Dashboard and the setup checklist do: *Connection lost* at Critical when the link to the service went, the
  shell's word and level for it, and *Read from file* at Warning when a server older than the call answered, which is
  connected. The pill's *Not connected*, at Warning, is gone from these pages: the service not coming back after a
  restart is *Connection lost* at Critical too. The line under the header says "Stored in the server's settings
  database." - naming the file only on Server limits & expert settings, whose sending limits a card says are kept in
  it - and the four subtitles that ended "(hMailServer.INI)" no longer do. `IniFallbackTests` holds the store to
  remembering a fall-back and its cause and the pages to the rest, and `SettingsPageStatusTests` the pill's state, word
  and level to the cause's.
- **Fixed: the domain and account dialogs' Levels tab showed its failures and its notes as plain bold text.** A read
  that failed was a bare line - on a lost link starting in lower case, and in the review's render "The input string
  'max_message_size_kb' was not in a correct format." - beside a greyed *Save levels now* and no way to read again,
  drawn exactly like the note about a server older than schema 6088 and the one asking for the dialog to be saved
  first. The tab now says each at its level, in a notice: a failed read at Critical, "Could not read the setting
  levels:" and the reason, with Try again, which keeps its button and the keyboard when the read fails again and gives
  *Save levels now* back when it works; the older server and the save-it-first notes as Information; a save as Good,
  or the settings it could not write as Critical, and a save that could not open the domain or account at all as
  "Could not save the setting levels:" and the reason. When the levels cannot be read again after a save, the save's
  outcome - what it could not write, or "The setting levels were saved." - stands before the read's failure and its Try
  again, where the read's failure was all the tab said: under a lost link the typed values went from the tab and
  nothing said that the save had not happened. `FailureStateTests` holds the tab to each.

- **Fixed: the Control Panel's confirmations came in several shapes, and Enter answered Yes to some that delete.**
  Fifty-six questions were asked with Yes and No, or OK and Cancel: Enter deleted a collection list's entry, removed a
  message from the delivery queue and revoked an API key, while the quarantine, the suppression list and the audit
  trail declined on Enter; a merge into a mailbox, which removes nothing, had the same red Yes as a replace that
  empties it; the restart offered after a settings save restarted the service on Enter, dropping every open session,
  as the window's *Apply now* and the TCP/IP ports page's offer after a save reinitialised the server on Enter, and
  the Status page's *Install the update* and *Pause the mail server* stopped it; and a collection list asked *Delete
  this security group?* without naming the group. Every question is now asked the one way, `Dialogs.Confirm`, as the
  few newer ones already were: the thing acted on is named in the question, the button says what it does - *Delete
  account*, *Revoke key*, *Replace mailbox*, *Merge*, *Pause the mail server*, *Restart now* - and the other button is
  Cancel or says what declining does (*Later*, *Keep working*, *Keep the next key*); the button that deletes, revokes
  or replaces something, or stops the server's services - a restart, a reinitialise, an update's install, a pause - is
  never the one Enter presses. Each collection list asks its own whole question with the entry's name, *Delete the
  SURBL server multi.surbl.org?* or *Delete the security group Sales?* with what goes with the group, where it put the
  list's noun into one template; the queue's question names the message's sender and recipients and says that it is
  deleted without a bounce; the restart after a save says that a restart drops every open session and takes *Later* on
  Enter; and the account dialog's recovery tab, which still raised the grey Win32 box, uses the themed one. The
  Control Panel's tests hold every page to the one shape, so that a Yes and No question cannot come back.
- **Fixed: the Domains & accounts and Public folders pages' deletes did not say what goes.** *Delete
  user00@example.com?* did not say that the mailbox and every message in it went with the account, and the five
  deletes were four sentence shapes, the domain's built from a translated fragment (*its 1 account*) put into a
  template. Each is now one whole question that says what goes and what does not: the account's mailbox and every
  message in it, with no undo; that mail to an alias or a distribution list is no longer delivered, and that the
  list's members' mailboxes are not affected; the domain's accounts, every message stored in them and its aliases and
  distribution lists, one sentence for a domain with no account, one and several; and the public folder with every
  message in it, named without quotes.
- **Fixed: deleting a TLS certificate or an IP range, and applying directory synchronisation, asked a question that
  named neither the act nor what followed.** *Delete certificate 'legacy.example.com'?* and *Delete IP range 'Branch
  office'?* were Yes and No boxes, and Directory synchronisation's Apply asked OK or Cancel without the counts its
  preview had just shown. The certificate's question now names the TCP/IP ports that present it, *SMTP 465, IMAP 993*,
  which do not start again until another certificate is chosen for them, or says that no port does; the range's names
  its addresses and says that they then fall under the next range that covers them, by priority; and Apply repeats the
  preview, the domain or every linked domain and how many mailboxes it creates and updates, how many accounts it marks
  inactive when that is ticked and how many directory entries it skips, before saying that no message is deleted and
  no account removed. Each is asked with its act on the button, *Delete certificate*, *Delete range*, *Apply*, as the
  never-ban list's *Remove* and the IP ranges page's *Replace all ranges* are.
- **Fixed: the Administrator roles page's Take the role away acted at once, and on an address typed for something
  else.** It removed the selected assignment without asking, and with nothing selected it took the role of whatever
  address was typed into the form for giving one; it was an ordinary button beside the primary *Give the role*, live
  at all times. It now acts on the assignment chosen in the list alone, can be pressed only while one is chosen, is
  drawn as the list pages' destructive action, red words on a secondary button, and asks first - *Take the role
  helpdesk away from helen@example.test? The account keeps only what its admin level gives it.* - with Cancel the
  answer Enter gives.
- **Fixed: several acts that cannot be undone ran at the first press.** A public folder's permission entry was removed
  at once (and a press with nothing chosen did nothing, silently); the quarantine released a whole group, or every
  checked message, at once; the quarantine's and the message trace's *Remove expired* deleted every message and every
  event past their retention windows at the first press, and both could be pressed at a window of 0, which keeps
  everything, to remove nothing; *Clear greylisting triplets* dropped every record, so every known sender is delayed
  again; and the server sendout asked to send to *all accounts matching '*'* without saying how many, and sent an
  empty message. Each now asks first, naming what it acts on, with Cancel the answer Enter gives: the entry's account,
  group or *anyone* and the folder; the group's sender or HELO domain, or the checked messages, and how many; the
  retention window in days, where *Remove expired* - on the quarantine and on the trace alike - cannot be pressed at a
  window of 0 and says why in its tool tip; and the sendout's subject, the wildcard and how many active accounts of
  active domains it reaches, counted as the server counts them, saying so and sending nothing where that is none. An
  empty message is refused on its field.
- **Fixed: the Control Panel drew destructive buttons too loud on lists and too quiet where they delete in bulk.** The
  group members' *Remove*, the folder permissions' *Delete*, the account dialog's *Delete folder*, *Empty mailbox* and
  *Erase traces...*, its two-factor *Remove* and recovery *Clear* buttons, the SSL certificates' *Delete selected* and
  every API key's *Revoke* wore the filled red of a confirmation's own answer, and the group members' and folder
  permissions' buttons were live with nothing chosen; while the journaling page's *Remove the rule*, both *Restore
  defaults*, both *Remove expired* and *Clear greylisting triplets* were plain buttons. Each is now drawn by its
  weight: a list's delete, remove or revoke and a button that deletes or replaces in bulk is red words on a secondary
  button, grey while it cannot be pressed, as the list pages' deletes already were, and the filled red is kept for the
  act itself - a confirmation's destructive answer and the backup page's restore. The buttons that act on a chosen
  entry can be pressed only while one is chosen. The folder permissions' *Delete* and the distribution list's *Remove
  selected* both read *Remove*, the word for taking an entry out of a list the thing outlives. Deleting an account's
  IMAP folder in any language but English no longer names and looks for the folder with "(not subscribed)" still after
  its name.
- **Fixed: an account's two-factor tab offered to enrol again, as its primary action, without saying that it replaces
  the secret.** With a factor enrolled the tab still showed the primary *Enrol a second factor* beside *Remove*, and
  pressing it made a new secret at once, so that the codes of the person's authenticator app stopped working; *Remove*
  took the factor away at once too. With a factor enrolled the button now reads *Enrol again (replaces the current
  secret)* as a secondary button, and asks first - the new secret replaces the current one and the app's codes stop
  working until it scans the new QR code - with *Replace the secret* and Cancel the answer Enter gives; *Remove* asks
  first as well, saying that the account's own password signs in to mail clients again.

- **Fixed: status words on the Control Panel's dark cards, and the live log's lines, were under the contrast a word
  needs.** The danger and information colours were chosen against the darkest surface the window has, and on the dark
  theme's card a certificate's verdict on the SSL certificates page, a scanner's problem on the virus scanning
  overview, a field's error, the result of a settings page's test button, the Auto-ban page's never-ban status and
  the account dialog's password strength line measured 4.1:1, and 3.5:1 on a hovered or selected row; the light
  theme's warning word measured 4.1:1 on a selected row and 4.4:1 in a dialog. Every status word on a surface is now
  drawn in the deeper or brighter shade of its level that the status pills and notices already use, which clears
  4.5:1 on the page, a card, a dialog and a hovered or selected row in both themes, and the status colours themselves
  are kept for marks, glyphs and charts; the dashboard's *Backlog* badge takes the same shade. The live log's lines
  are drawn as the rows of the Control Panel's other lists are, the grey under the pointer and the selection's tint
  and marker when selected, where they took Windows' own row, whose blue selection put every dark line at 3.8 to
  3.9:1 and whose grey one, once the list lost the keyboard, at 2.9 to 3.0:1; its error, IMAP and application lines
  are #FF7B72, #BC8CFF and #58A6FF in the dark theme, where they measured 4.1 to 4.3:1 on the log's card, and its
  SMTP, application and POP3 lines a shade deeper in the light theme, so that every line clears 4.5:1 on the card,
  under the pointer and selected. The log's palette is measured with the design tokens now, and the Control Panel's
  tests hold every pair to its floor, fail a page that colours a word with a status colour and fail a list whose rows
  are not the application's.

- **Fixed: the Control Panel's filled red buttons drew white words at 3.7:1 in the dark theme.** The destructive
  choice in every confirmation, *Start restore*, *Restore the mailbox* and every other button with the Danger
  appearance was filled with WPF-UI's own #F44336 in both themes, a colour none of the design's tokens named, and
  kept the theme's ordinary text colour on it: near-black in the light theme, white in the dark one. The Danger
  appearance now takes its colours from the design tokens, the palette's own danger red filled, with white words in
  the light theme and near-black ones in the dark theme, and a deeper or lighter shade under the pointer and pressed,
  every one 4.5:1 or more; under High Contrast it is the window text with the window colour on it. One style gives it
  to every such button, so a button needs only its appearance, and the Control Panel's tests measure every state.

- **Fixed: the Control Panel's text fields had no edge a reader could see at rest.** Fluent's own border drew a
  field's outline at 1.1:1 against its card and its bottom edge at 1.5 to 1.7:1, so that an empty field - the Note
  box of the suppression dialog, a route's host name - read as a faint band on the card, while the tick boxes beside
  it measured 5.8:1. Every text box, password box, number box and the date picker now has the theme's strong stroke
  along its bottom edge, the line Fluent itself finds a field by, which clears 3:1 against the card, the page, a
  dialog and the field's own fill in both themes; the outline round the top and sides stays as faint as it was, a
  focused field keeps its accent line and focus ring, and under High Contrast the theme's own border stands. The date
  picker, which had the buttons' border and no bottom edge at all, takes the text fields' border.

- **Fixed: links in the Control Panel were WPF's own blue, 2.5:1 on the dark theme's card, and turned red under the
  pointer, and the sign-in page's link followed the Windows accent.** The links on the About page, the help pane's
  list of documents and every link inside a help document set no colour of their own, so they took the system's
  #0066CC in both themes - legible on the light theme, under the floor on the dark one - and WPF's own red when
  pointed at, the colour the Control Panel keeps for failures; the sign-in page's "Set up two-factor
  authentication…", the first link an administrator sees, took a shade of the user's Windows accent instead, held to
  no contrast floor: #005494 on the light card and #A6D8FF on the dark one with Windows' default blue. Every link now
  takes the link colour of the design tokens, #0B5CB5 in the light theme and #6CB6FF in the dark, 4.5:1 or more on
  the page, a card, a dialog and a hovered or selected row, and a shade further from its surface under the pointer;
  the About page's and the help's links stay underlined and take the disabled text colour when they cannot be
  followed. Under High Contrast the links take the system's own colours.

- **Fixed: after a change of theme the Control Panel's quarantine page kept the old theme's green and red on its
  action buttons, and every refused field its old red ring.** The page gave its row template the shared success and
  danger brushes themselves, and a template freezes what it is given, so once a row had been shown the theme switch
  could no longer recolour those brushes: the 28 release, delete, allow and block glyphs on the rows, the groups and
  the column's header stayed the light theme's colours on the dark card, 2.5 to 2.7:1, and the ring a field draws
  when its value is refused, which read the same brush, stayed the old red in every window. The glyphs and the ring
  now take their colours by name, as every other colour in the Control Panel does, and follow the theme; the Control
  Panel's log says so if a shared colour is ever found frozen again, and a test refuses a page that holds one.

- **Fixed: the pages inside a group of the Control Panel's sidebar were drawn as WPF-UI's rows, not the sidebar's
  own.** The tree gave its row style to its own rows alone - Welcome, Dashboard, the groups and About - and the
  sixty-odd pages inside the groups fell back to WPF-UI's row: the selected page was marked by a bar in the Windows
  accent colour outside a highlight three pixels shorter, where the top-level rows have the brand pill inside theirs,
  the pages were drawn in the primary text at rest where the groups take the secondary, and neither the hover rule
  nor the border round the row the keyboard is on reached them. Every row of the sidebar is now the sidebar's own at
  every level: the brand pill on the selected page, the secondary text at rest, the same height and highlight, the
  keyboard's border, and each page's label lined up under its group's.

- **Changed: before sign-in the Control Panel shows the sign-in card alone, with no sidebar and no search.** The
  sidebar and the search field were disabled until somebody had signed in but were drawn exactly as they are when
  they work - the same dark grey on every page row, the hand pointer, the search's placeholder within a shade of its
  enabled colour - so a first-time administrator met sixty pages that looked ready to open and did nothing when
  clicked, with nothing to say why; only the help button dimmed. They are now not shown at all until sign-in: the
  sidebar, whether the tree or the rail, its footer and the button that collapses it, and the search field, with the
  sign-in card in the middle of the whole window. They appear with the pages the signed-in account may open, and go
  again if a second factor is refused. The help button stays, dimmed, beside the theme toggle and the administrator's
  menu.

- **New: the Control Panel answers F5, Ctrl+F, Ctrl+S and F6 on every page, and Escape closes the help pane.** Only
  Ctrl+K and F1 existed: a keyboard user tabbed through a page's header to refresh it, find its search box or save it,
  and tabbed through every control of the window to get from the navigation to the page or the help, which closed only
  by its own button or a second F1. F5 now presses the page's Refresh, or a settings page's Reload, which asks before
  it throws edits away, and reads again the pages that have no such button - domains, routes, rules, incoming relays,
  TCP/IP ports, SSL certificates, IP ranges (keeping the permissions not yet saved), live logs, stalled mail, the
  message trace's last search, the dashboard, the welcome and about pages - leaving the keyboard on the list's
  selected row; Ctrl+S presses its Save changes, IP ranges' Save permissions, and on Backup & restore the Save of the
  card the keyboard is in; Ctrl+F puts the keyboard in its search box - on the domains page the one beside the list
  the keyboard is in - and opens the palette on a page without one; F6 and Shift+F6 move round the command bar, the
  navigation, the page, the help pane and the pending bar, to the sidebar's row for the page on screen and back to the
  control of the page that last had the keyboard; and Escape in the help pane closes it and puts the keyboard on the
  Help button. Each key presses the page's own button as a click does - the keyboard moves to the button first, so
  that a number typed and not yet taken in by its box is what Save writes and what makes Reload ask, and then comes
  back to where it was - and nothing a disabled button would not. Refresh is Alt+R on every list page, where six pages
  said Re_fresh (Alt+F) because the R was taken - the domain white and black list's Remove is now Delete, the delivery
  queue's and the suppression list's Remove is Alt+M, and the diagnostics page's Run diagnostics is Alt+D - and the
  REST API keys, journaling and administrator roles pages, which are lists, say Refresh where they said Reload, the
  settings pages' verb (the owner's decision of 9 October 2026). The palette's new Keyboard shortcuts row lists every
  key in the reader's language - Strg+F and Entf in German. `KeyboardShortcutsTests` holds the table, the order F6
  moves in, the window's wiring, every re-read button to Refresh or Reload with its one access key, every page read as
  it opens to an F5 and every page that can hold an unsaved change to a Ctrl+S, and the keyboard's move to the button
  before it is pressed.

- **New: the Control Panel's lists answer Enter, F2, Delete and Insert, and Enter submits the forms on its pages.** No
  list answered a key: a keyboard user selected a row and tabbed to the page header's buttons to open or delete it, a
  double-click was the only other way to open a row - and on the collection pages a double-click on a column header's
  divider, to fit the column, opened Edit for whatever row was selected - each quarantine row was about seven tab
  stops with no key to release, delete or preview it, and Enter did nothing in the add forms of the routes, TCP/IP
  ports, rules, incoming relays, SSL certificates, IP ranges and domains pages or in the MX query's and the
  diagnostics' boxes. On every list page Enter and F2 now open the selected row, Delete deletes it with the page's own
  question, and Insert adds a row or goes to the page's add form, each pressing the page's button for it, so a key
  does nothing a disabled button would not, and the keyboard is back on the selected row afterwards; a double-click
  opens a row only on the row. The quarantine's rows are one tab stop, after the header's box and its four actions on
  the checked messages: Tab reaches the selected row, or selects the first, and leaves the grid from it; Up and Down
  move from row to row, groups included; and the selected row answers Space (check), R (release), Delete, P (preview)
  and H (headers), with the keys in the row icons' tool tips - its box and icons, a stop each before, are the mouse's,
  and a row's whitelist and blacklist are its box and the header's two actions. Enter in a form on a page presses its
  button as in a dialog, a box of several lines or an open list keeping its own, and Escape empties what was typed,
  leaving defaults such as a port's 25. The palette's Keyboard shortcuts list names the keys. `GridKeysTests` holds
  the helpers, every page's wiring, the quarantine's keys and the tool tips.

- **Fixed: a screen reader said other words than the Control Panel's buttons showed, and voice control could not press
  them by name.** Some thirty buttons and boxes had their accessible name replaced by a sentence that explained them:
  Refresh was "Re-check the external prerequisites" on the setup checklist and "Re-read the domains and server
  configuration" on DNS records, Preview and Apply on Directory synchronisation were "Show what a synchronisation
  would do, changing nothing" and "Create and update the accounts the preview listed", Reload, Save changes and Test
  connection on Directory authentication were sentences about hMailServer.INI, Members… on the groups page was "Edit
  the members of the selected group", Delete selected, Save passphrase and Remove stored passphrase on SSL
  certificates were sentences, and the Note box of the never-ban, domain policy and suppression dialogs was "Why it is
  listed" - so what was heard was not what was seen, and somebody who said "click Refresh" or "click Preview" reached
  nothing (WCAG 2.5.3, Label in Name). Each is named by its caption now, and the sentence is its help text, which a
  screen reader says after the name, and its tool tip (`Views/ControlHelp`). The links whose captions say less than
  where they go - ten "Settings…" and six "Open…" on the spam filtering overview, "Versions and ciphers…" on the
  transport encryption overview - are named by the caption and then the destination, "Settings: Open Blocked senders,
  which owns the Blocked senders settings", so that they are told apart and each starts with the words on it; the DNS
  records' per-record buttons are "Copy host of the MX record" and "Copy value of the MX record", and the dashboard
  charts' "Show table of delivery throughput". A box captioned by a field row is named by its caption alone, in every
  language - the row now names the box of a row of a box and a Browse… button, not the panel around them - where a
  name set beside the caption was a second string that some catalogues translated as another word: the journaling
  page's Note box was "Merknad" under Notat in Norwegian, and some 150 names in the seventeen catalogues, and four on
  the SSL certificates page in English, did not start with their captions. `LabelInNameTests` reads every page and
  dialog for a captioned control whose name does not start with its caption, in English and in each of the seventeen
  catalogues, so the defect cannot return unseen.

- **Fixed: arrowing through the Ctrl+K palette's results was silent to a screen reader.** The search box keeps the
  keyboard for the palette's whole life, so that typing never has to be resumed, and the list cannot take it: a
  reader that follows the keyboard heard the typed letters and nothing of the rows the arrow keys moved through,
  though each row's name was set. The palette now says the row the selection reaches - "3 of 12:" and the row's
  name, which is what it is and where it lives - as the arrow keys, Page Up and Down, Home and End move it and as
  typing changes the results, and the empty-results sentence when nothing matched, as a notification that replaces
  the last one rather than queueing every row passed (`Services/ScreenReaderNotice`); the search box's help text is
  the footer's key legend as a sentence, "Up and down arrows to move, Enter to open, Escape to close".
  `PaletteAnnouncementTests` holds the position, which counts the rows that can be chosen and not the section
  captions, the sentence and the wiring.

- **Fixed: the administrator roles page's two lists had no names, no titles, and rows a screen reader called by a
  class name.** Neither grid had an accessible name, the two cards no title - only the column headers told the roles
  from who holds one - and neither row class said anything of itself, so a reader announced each row as
  "hMailServer.ControlPanel.Views.RolesView+RoleRow". The cards are titled Roles and Who holds a role, the grids are
  named the same, and a row says its role and what it is for, or the account, its role and its domains
  (`Services/RowSpeech`). `RowSpeechTests` holds the sentences and the page.

- **Fixed: rows on the code-built list pages were announced by their class name or as unlabelled values.** A screen
  reader read a row of the audit trail as "hMailServer.ControlPanel.Views.AuditTrailView+RowView", and so the rows of
  the suppression list and its bounce counts, the message trace, journaling, eDiscovery's cases and their lines, the
  domain white and black list, setting profiles, the never-ban list, the message-store consistency list, the app
  passwords and the Active Directory picker; a collection page's row was its first three raw values with no column -
  "Yes, zen.spamhaus.org, 127.0.0.2", the first a tick box's. Each row now says itself as one statement: its cells
  that have something in them, a bare value with its column's name - "gone@old-provider.example, Hard bounce, 5.1.1,
  Refused while delivering, Since: 2026-10-07 03:10:43, Expires: Never"; "gone@old-provider.example, Hard: 3, Soft:
  0, …"; "zen.spamhaus.org, Active: Yes, Score: 3" - a collection page's cells as the grid shows them, a choice's
  word and not its number. `RowSpeechTests` reads every page for a class bound to a list's columns and holds each to
  a ToString of its own.

- **Fixed: four facts were only in row tool tips, out of reach of the keyboard.** Why a message was held in
  quarantine, what the receiving server answered for a suppressed address ("550 5.1.1 user unknown"), an audit row's
  whole detail and whether a traced event can be followed were each on screen only while the mouse rested on the
  row. Under each of those lists the selected row's fact is now in a compact card titled with what it is - Why it was
  held, What the receiving server answered (or Note for an address put there by hand), Detail - in text that can be
  selected and copied, collapsed while nothing is selected (`Views/RowDetailStrip`); the rows keep their tool tips. The
  audit trail and the message trace lay their toolbar, notice, list and detail out as the suppression list does, so
  that the list keeps three rows in the narrowest window and the page scrolls below them, where the detail would have
  taken the list's height. `RowDetailStripTests` holds the strip, each page's fact and its place after the list.

- **Fixed: the Control Panel's page forms had no Alt keys, one action had more than one, and keys collided.** Domain,
  Address, Password, Alias address, Delivers to and the list's address on the domains page, Account, Role and Domains
  on the administrator roles page, and some fifty more field captions - on the routes, TCP/IP ports, IP ranges, rules,
  SSL certificates, incoming relays, MX query, send-out, diagnostics, journaling, Directory synchronisation, Directory
  authentication and DNS records pages and in eight dialogs - carried no key while the dialog fields around them did, so
  Alt reached a page's buttons and none of its boxes. Each carries one now, and a field whose box has a button beside
  it - the backup file, the destination folder, a DKIM key file, a route's address to add - puts the keyboard in the
  box, where its key did nothing. A verb has one key on every page: Remove is Alt+M (the route and group member
  dialogs and the two-factor panel said Alt+R) on the button and in the question it asks, whose Remove was Alt+R in
  the group members, recipients, journaling, sample domain and sandbox token questions, Browse… Alt+B, Copy Alt+C,
  Search Alt+S, Remove expired Alt+E and Restore defaults Alt+F, and Resume is Alt+E, where on the server status page
  it was Alt+R beside Refresh; the route dialog's Add is Add address. The Backup & restore page's five cards hold some
  thirty-five captions, more than there are letters, and shared eight - Alt+P was the encryption passphrase, the
  archives to keep and Put the message back, and WPF, given two targets for one key, only moves the focus between
  them: the card that holds the keyboard now answers its own keys first, and a key it does not have goes to the page
  as before (`Views/AccessKeyScope`). `build/check-mnemonics.py` reads field captions in XAML and in code, captions
  set in code on a XAML page's buttons and the buttons a confirmation names, holds a verb to one key and each card of
  such a page on its own, and holds a dialog tab's keys against the Save button under every tab, which it meant to and
  did not: in 109 places, all in translations, a tab's key was its dialog's Save - Starkes Passwort erzeugen and
  Speichern were both Alt+S on a German account's General tab. Some 170 translated captions in fifteen languages took
  another letter to make room. `--self-test`, which CI runs, proves each rule against a page made to break it;
  `AccessKeysTests` holds the pages.

- **Fixed: the dashboard's chart range did not say which range was chosen.** Live, 24 hours, 7 days and 30 days were
  four buttons that marked the chosen range by its colour alone: a screen reader heard "Show Live" and three more
  buttons with none of them chosen, and pressing the chosen range read its history from the server again. They are a
  radio group named Chart range now - the caption beside them says Chart range where it said Show - with the chosen
  range still drawn as the primary button: one tab stop, the arrow keys move the choice, a reader hears "Live, radio
  button, selected, 1 of 4", and the chosen range pressed again does nothing. The note on a history range is under the
  choice, where it ran past the window's edge. `ChartRangeGroupTests` holds the group.

### Administration

- **Fixed: the Control Panel's IP ranges page could show no list of ranges at all (reported in work item 79), and the
  domains and rules pages cut parts of themselves off in a narrow window.** In 6.3.4 the IP ranges page's list was
  given the height the window left after the page's title, the toolbar, the permissions card and the add form, and the
  page did not scroll, so a window less than about 700 pixels high (875 at 125 % scaling) left it no room for a whole
  row, and one less than about 630 (790) no list at all: no range could be seen or selected, and so none could be
  edited or deleted. 6.3.5 kept 150 pixels of list by holding the page to the window over a fixed 560 pixels, which
  knew nothing of how tall the cards are: in the narrowest window (760 by 520), where the permissions wrap, the add
  form ran past those 560 pixels and could not be scrolled to. The page is now laid out by a new scaffold panel,
  `FillPanel`: the list takes the height the other parts leave, down to three rows, below which the whole page
  scrolls, and every other part always keeps its own height, so all of the page can be reached at every window size.
  In a narrow window the toolbar's buttons wrap where *Delete selected range* and *Restore defaults* ran off the right
  edge, the grid scrolls sideways where it squeezed the ranges' names to nothing and cut the upper address short, and
  the permissions take two columns, where a quarter of the card broke a word in two. The permissions card shows the
  selected range's permissions or none: with no range selected its boxes are clear and disabled, where they kept the
  last range's values under *Permissions (select a range)*; a save, an edit or an add keeps the range selected, where
  the reload dropped the selection and with it the card's *(saved)*; and Save, Properties and Delete find the selected
  range by its ID, where two ranges with one name and one lower address were both taken for the first of them. The
  domains and rules pages held a grid to the window over a fixed 640 and 620 pixels in the same way, and in the
  narrowest window the domains page's alias form ran past them and could not be scrolled to, its alias list had no
  height, *Create account* ran off its card and every domain's *Properties* and *Delete* off the list, and the rules
  page left a rule's actions no height and cut its match list off. Both pages are laid out by `FillPanel` now, which
  shares the window between several lists by weight: every list takes what the window leaves, down to three rows, and
  each card holds its toolbar, its list and its form whole, so all of either page can be reached at every window size.
  Below a page of 700 pixels the domains card goes above the accounts card, the alias and list cards go one above the
  other where they would be narrower than 280 pixels and the rules page's criteria and actions where they would be
  narrower than 340, *Generate* and *Create account* go under the password where they do not fit beside it, a list's
  rows are its width with a long name cut short and whole in its tool tip, and the rule grid's columns keep a width
  and scroll sideways.
- **Fixed: the Control Panel could stop answering on the rules page at some display scalings.** A list of rules,
  criteria or actions whose rows fitted only without a horizontal scroll bar, at a scaling where its columns came out a
  fraction of a pixel wider than the list, showed the horizontal bar, which left the rows too little room, so the
  vertical bar came, the columns narrowed to fit, the horizontal bar went, the rows fitted, the vertical bar went, and
  so on for ever: the page's layout never settled, and the Control Panel took no more input. In 6.3.6 four rules did it
  in a 1280 by 800 window at 175 %, and a rule with four criteria or actions in a shorter one. The criteria and actions,
  one column whose text ends in an ellipsis, no longer scroll sideways, and the list of rules, whose columns keep their
  widths and scroll sideways in a narrow window, keeps its vertical scroll bar, so that neither bar can turn the other
  on or off.
- **Fixed: the REST API's OpenAPI document described either the listing of a folder's messages or the import of a
  message into one, never both, and `GET /api/v1/status` twice.** `/api/v1/openapi.json` wrote
  `/api/v1/me/folders/{id}/messages` as two entries, one holding the `GET` and one the `POST`, since the import
  arrived on 11 September 2026, and `/api/v1/status` as two since 24 September, the second the description from
  before the event-script languages were listed in it. JSON leaves a reader to do as it likes with a member named
  twice in one object, and every reader keeps one of the two without a word: a browser's or Python's the last, so a
  client generated from the document had the import and not the listing, and the older status description; the
  server's own the first. Each path is now one entry carrying every method it answers, as the other paths with
  several were written. Nothing changed in what the server answers, only in what it says it answers.

### IMAP, POP3 and the webmail

- **Fixed: an answer of the HTTP listeners that its handler returned after the listener's request deadline was cut off
  after its first 64 KB.** The REST API, the webmail, the Control Deck, CalDAV and CardDAV, and the web services all
  answer through one HTTP server, whose request deadline - 30 seconds on the REST listener, 15 on the web services' -
  runs while a request is read and handled. When it ran out while a handler was still working, its expiry could no
  longer be cancelled, and it closed the connection as soon as the handler's answer had handed the socket its first
  64 KB: on server1 a webmail search that took longer than thirty seconds answered 65,413 bytes of its 67,996 and was
  closed, which Python's client reported as `IncompleteRead` and the page as a search that never drew. An answer under
  64 KB arrived whole, so nothing smaller showed it. A deadline that ran out while the handler worked no longer ends
  its answer: the answer is written a piece at a time, each piece given the request deadline to be taken, as a
  download already was; the deadline is for a peer that stalls, not for an answer slow to make. A connection whose own
  deadline (five minutes after it was opened) would come first is given the time its answer needs and closed after it.
  An answer the server does end part way - a reader that stopped reading for the request deadline, the server stopping
  - now ends with a reset rather than an orderly close, which a client that does not count the bytes against
  `Content-Length` could take for the end of the answer.
- **Fixed: with the full-text index on, a webmail search on SQL Server Compact took minutes, held the database
  throughout and stalled the indexer.** Both searches - `q` on a folder's listing and `GET /api/v1/me/search` - asked
  the account's whole index before they looked at a message: every message it had read whole, then every term holding
  the word, which `LIKE '%word%'` makes every database find by reading them all. On server1, with the 100,000-message
  folder of Performance.md, one search took 75 to 180 seconds, against 0.24 s with the index off; timed on SQL Server
  Compact holding the same folder's 14 million index rows, the word's query alone took 167 seconds and the whole
  messages' 89 seconds cold. All that while the server's one SQL Server Compact connection was held, so the indexer's
  batches failed (`HM5180` after 60 seconds, then `HM6260`), and the answer came after the listener's deadline
  (above). A search now asks the index about the messages it walks past, two hundred to a query - some 40 ms on
  Compact - before it reads any of them, stops asking after about two and a half seconds and answers where to go on
  from, as it already did when its reading ran out (`complete: false`, `next_before_uid` or `next_before_id`); what
  the index leaves is read under a clock of its own. Each answer now says how many message files it opened (`opened`)
  beside how many messages it walked past (`scanned`). IMAP `SEARCH` and the eDiscovery search, which asked the whole
  account the same way at their first word, ask a window of their own messages at a time too. A row counts only as the
  indexer wrote it: a term holds a word only where the scan would find it there, and a marker only spelt as the
  indexer spells it, whatever the database's collation matches besides. On server1, with the index caught up, the
  folder's search for a word in every thousandth body answered in 5 requests of 2.5 seconds, 12.4 seconds in all, and
  IMAP `SEARCH BODY` in 12.5 seconds, with no `HM5180` while the index was built and searched; with the index off they
  take 12.5 and 9.8 seconds. On SQL Server Compact a lookup costs about what reading a small message from a fast local
  disk does, so there the index spares reads without making a search faster; it pays where reading a message is slow.
  [Performance.md](hmailserver/docs/Performance.md), "Searching".

### Operations

- **Fixed: on Linux, a MySQL or PostgreSQL column the server reads as a `long` went through an `int` on its way
  there.** The two recordsets read such a column with `atoi`, whose `int` is narrower than Linux's 64-bit `long`, so a
  value beyond 2,147,483,647 would have come back wrong; they now read it with `strtol`, and a decimal column with
  `strtod`, the conversion `atof` made. The 64-bit ids, such as a message's, were and are read with `_atoi64`, and
  were not affected. A value in an `int`'s range reads as before, and Windows, whose `long` is an `int`'s width, reads
  every value as before. The REST API's complaints route reads its `account_id` filter with `_atoi64`, as the routes
  beside it read their ids.
- **On Linux the update check asks `rpm` whether it installed the server once in each process, through the server's
  own process launcher.** It ran `/usr/bin/rpm -q hmailserver` through `system()` and kept the answer where two
  threads asking at once could both run it; the first caller now runs it, with no shell, and the others take its
  answer. `rpm`'s standard error, which the shell line discarded, goes where the server's does.
- **Fixed: with the full-text index on, every start of the service held SQL Server Compact's one connection for
  minutes.** Before it indexed anything, the indexer removed the index rows of messages that no longer exist in one
  statement over the whole table, and again on the full-index cadence; on SQL Server Compact holding the 14 million
  rows of Performance.md's 100,000 messages that statement took 178 seconds, and on server1 the scheduled tasks that
  start with the service waited 60 seconds for the connection and failed (`HM5180`, then `HM6613` and `HM6624`), after
  the restart with the index on in both measuring runs of 7 October 2026. It now removes them a thousand message ids a
  statement, stepping over the ids the index names none of, with a pause of 50 ms between two: some 150 seconds in all
  there, no statement over 1.6 seconds. The sweep of the message metadata for the messages that are gone, which the
  indexer makes at every start whether the full-text index is on or not, is cut the same way: it was one statement
  over the whole table, which a stop of the servers had to wait for (*Correctness*, above).

### Build and tests

- **Every change is checked before it lands, a maintainer's batch as well as a merge request** (OpenSSF Baseline
  OSPS-VM-05.03 and OSPS-VM-06.02). `semgrep-sast`, `semgrep-sast-gate` and `mr-dependency-check` ran in a merge
  request's pipeline alone, and a maintainer's `batch*` branch, whose merge request runs no pipeline of its own, landed
  without them - 6.3.5's and 6.3.6's batches among them. They now run in a protected `batch*` branch's pipeline too, the
  one GitLab holds its merge request to, judged against where the batch leaves `master` (`change_base()` in
  `build/ci/security-gate.py`; `--merge-request auto`, `--base auto`): blocking, with no `rules:changes` list, no rule
  that reads `SAST_DISABLED`, and tried twice more when an input cannot be read (exit 2) or GitLab itself fails.
  `semgrep-sast` runs GitLab's analyzer from a copy in the project's registry that keeps its digest
  (`mirror/semgrep`, made by `build/ci/mirror-semgrep.sh`): on 5 October 2026 GitLab's registry stopped serving the
  digest pinned on 26 September, which would now have stopped every change. On a batch, `static-analysis`, `semgrep`,
  `ci-image-packages` and `dependency-scan` compare the branch with `master` rather than with the push before, so a
  push of documentation on top of code still runs them. `build/land-via-mr.py` lands a batch only when each gate in
  its pipeline succeeded itself - the three change checks always, the two source scans unless the batch changes
  Markdown alone at the root or under a dot-directory - since GitLab calls a pipeline a success with a job a rule left
  out or one that failed while allowed to, and it writes their statuses into the landing record.
  `build/check-release-chain.py` holds all of it, each rule with a case of its self-test that must fail
  (SECURITY.md, "Every change"; ContinuousIntegration.md, "Change checks"). And `.own-runners`' paths name the
  files called `d` and `md`, which their patterns left out, so that a change to one runs the project's runners'
  jobs as a change to any file that is not a document does.
- **A change cannot silence its own finding.** `semgrep-sast-gate` holds a change that adds an entry to
  `build/ci/static-analysis-baseline.txt` or `build/ci/semgrep-baseline.txt`, or takes a reason from one, and one that
  raises, in any file a scan reads, the count of the inline comments by which Semgrep, clang-tidy or cppcheck passes
  over a finding; a change may delete an entry, or give one its reason. A change that alters the scan's configuration
  answers for every finding in the tree, as before, and now so does one that removes or moves one of its files, which
  the gate read as touching nothing. When it re-pins the analyzer, and only then, its own copy of the exceptions file
  may accept a finding in a file it does not touch, never in one it does, so that re-pinning a vanished analyzer need
  not wait on an exception no gate could judge.
- **A narrowing of what a scan reads lands alone, as a declaration.** Whole-tree mode widens what counts and cannot
  see a finding a change keeps out of the report: a `.semgrepignore` (GitLab's analyzer includes the root's), an entry
  of `SAST_EXCLUDED_PATHS` or `SAST_SEMGREP_EXCLUDED_PATHS` in any CI file, the analyzer's options, a custom ruleset,
  the source scans' ignore and suppression lists - a line gained, or a `!` line Semgrep's list loses or puts before the
  line it takes back from - clang-tidy's checks, or the lines of `static-analysis.sh` and `semgrep.sh` that choose what
  cppcheck, clang-tidy and Semgrep read and report (the files, options and checks, the rules each pack brings, and
  what of a failed run stands as the report), now marked `# scan-scope begin` and `# scan-scope end`, or one of
  those tools' options put outside them, could hide the finding the same change brought. `semgrep-sast-gate`
  holds a change that narrows what a scan reads while it changes anything else a scan reads; a narrowing that lands
  by itself, with its documents, passes. `static-analysis.sh` refuses `HM_TIDY_CHECKS` in a GitLab job, so that no
  project variable chooses its checks, and `semgrep.sh` refuses to run without its ignore list, without which Semgrep's
  default would leave out `build/` and every test directory. `semgrep-sast`'s exclusions are held to the root -
  Semgrep's `--exclude` and GitLab's report filter read `tests` as any directory of that name, as the analyzer's
  Semgrep 1.174 showed on 6 October 2026 - and the job sets `SAST_SEMGREP_EXCLUDED_PATHS` to nothing itself. A source
  file moved to where the scan does not read it - as git pairs a move, or deleted while a file of its name is added
  there, a move rewritten past what git pairs - or added under a `vendor/`, `dist/` or `build/` below the root, which
  the analyzer's own template leaves out, is held too. SECURITY.md says that the scripts under `build/`, the gates'
  own among them, are read by the `semgrep` job alone, in a batch, and that an edit of the vendored zlib or QuickJS
  sources is read by no gate.
- **A report the scan did not write in this pipeline stands for nothing.** `semgrep-sast` was `allow_failure`, so a
  scan that failed - an image the registry no longer served, a timeout - let `semgrep-sast-gate` run on whatever
  `gl-sast-report.json` the checkout held, a committed one among them. `semgrep-sast` is no longer `allow_failure`, and
  removes `gl-sast-report.json` and `semgrep.sarif` before its analyzer runs; the gate, and the release job, refuse a
  tree that tracks a scan's report, and a `semgrep-sast` report that GitLab's analyzer did not write or that began
  before the pipeline was created; `.gitignore` keeps the scans' outputs out of a `git add`. `semgrep-sast` keeps
  Semgrep's SARIF beside its report, and the gate holds a change to a file one of Semgrep's rules gave up on, which the
  analyzer leaves at a warning. It reads the SARIF in the words of the analyzer it was checked against (6.26.1,
  Semgrep 1.174.0, kept word for word in its self-test): a notification of a kind it does not know, a time limit that
  names no file of the checkout as the analyzer names one (from the root: a `./` or an absolute path would match no
  changed file), or a SARIF with no run stops it as an input it cannot read (exit 2), never a pass, and its
  self-test fails while `semgrep-sast` pins another analyzer, until the new one's words are read
  (`build/ci/mirror-semgrep.sh`'s header; ContinuousIntegration.md, "Re-pinning `semgrep-sast`'s analyzer").
- **A fork's merge request runs a pipeline of its own, and does not land on it.** The workflow's first rule compared a
  merge request's source project with `CI_PROJECT_ID`, which in a fork's pipeline is the fork's, so a merge request from
  a fork's protected branch ran no merge request pipeline and was held to a branch pipeline that runs no gate. The rule
  now leaves a merge request without a pipeline of its own only when it comes from a protected `batch*` branch of this
  project, and `build/check-release-chain.py` holds it to the branches the gates run for. A fork's pipeline runs on
  runners its owner chooses, so SECURITY.md says a fork's change lands through a batch, or on a pipeline run here, and
  `build/land-via-mr.py` refuses a pipeline another project ran.
- **The gates' base is GitLab's, and the landing reads it back.** A variable a CI file defines stands in for GitLab's
  own, so a change could name its own base. `change_base()` takes `CI_MERGE_REQUEST_DIFF_BASE_SHA` only in a merge
  request's pipeline, and only when it is an ancestor of the head and of the target branch; a CI file that defines a
  `CI_*` or `GITLAB_*` variable holds the change, and `check-release-chain.py` refuses one; and `land-via-mr.py` reads
  "The base:" from each gate's log and refuses a landing whose gates judged it against anything but `master`'s tip.
- **A change to the gates' own code is judged by the base's copy too.** The gates ran the change's own scripts, so an
  edit of them judged the change that made it. When a change alters `security-gate.py`, `security-findings.py`,
  `mr-dependency-check.py`, `dependency-scan.py` or `osv-offline-db.py`, each gate also runs the base's copies over a
  worktree of the change's head, and a hold from either holds it. Whether the base's copy runs is read from the code
  that runs, against the base's, never from a variable, so neither a CI file nor a project variable can stop it; each
  gate says in its log how the copy ended, and `land-via-mr.py` lands a batch that changes the gates' code only when
  both logs say it passed. Each script of the gates and of the landing takes its own directory off Python's module
  path before it imports anything else, so that a `build/ci/json.py` a change adds does not run as the gate's `json`.
  `land-via-mr.py` lands a batch that changes the gates' own files - their code, `.gitlab-ci.yml`, the CI files the
  gate jobs are in or take a part from (`mr-checks.yml`, `gitlab-security.yml`, `no-github.yml`), itself,
  `check-release-chain.py`, `sign-off.py` and `scan-secrets.py`, which it runs, and the source scans' scripts,
  `static-analysis.sh` and `semgrep.sh` - only with `--gates-read "<reason>"`, a person's word that the change was
  read, which the landing record keeps (exit 9 without it), and only when `checkers` succeeded itself;
  `check-release-chain.py` holds that list to the files the gate jobs reach with `extends` and `!reference`, refuses a
  second definition, in another CI file, of a job that holds a change or of a template one reaches - GitLab merges
  the two, a later include's keys over an earlier one's - and a CI file but `.gitlab-ci.yml` that sets `variables`,
  `default` or `workflow` or includes anything but GitLab's templates, and holds each Python script of the list to
  the line that keeps its directory off the module path. SECURITY.md says the gates stand against mistakes, not
  against a change that edits the jobs themselves.
- **`mr-dependency-check` holds a requirement that pins no version, and does not take OSV's silence for an answer.** A
  requirements line it could not read as `name==version` - a range, `name @ <url>`, another index - was skipped; one a
  change adds now holds it, and so does a `-r` or `-c` naming a requirements file the tree does not hold (a URL, an
  absolute path) and a `${...}` pip takes from the environment. A control that OSV is known to name (OpenSSL's, Boost's, zlib's, Connector/C's, 7-Zip's)
  that draws no advisory is OSV's data not read, exit 2, and `dependency-scan` fails its scan for it; a version older
  than its control that OSV names nothing for, while it names the control's, is held by the control's advisories as
  ones that may affect it (OpenSSL 0.9.6 draws none, 4.0.0 43). A base whose lock file or exceptions file cannot be
  read, or whose exceptions file has an entry reviewed after today, holds no change by that alone - the file names
  or accepts nothing - so that the change that mends it can land.
- **`static-analysis` and `semgrep` pass a finding that `master`'s exceptions file accepts, and their baselines only
  shrink.** A finding new to a baseline failed the job unless the baseline took it, which needed no reason, reviewer or
  expiry and was read from the change itself. Each job now reads `master`'s `.gitlab/security-exceptions.yml` -
  `static-analysis` with git, which it installs with its tools, `semgrep` from the project's API - through
  `security-gate.py`'s own reader, passes a new finding that an exception in force accepts, and fails (exit 2) when
  the file cannot be read; an exception within 30 days of its expiry is named in the log. `--write-baseline` keeps the
  entries still found, drops those gone and adds none, and `--reasons` gives a reason only to an entry that has none.
  The artifacts gain `accepted-findings.txt` and `master-security-exceptions.yml`.
- **The eight High baseline entries that had no reason have one.** Six of Semgrep's parse the project's own
  catalogues, a nuspec rendered from its template or a TRX the suite wrote, or start node with an argument list;
  `hmctl`'s unverified TLS context is made only for `--insecure` to a loopback address; and `CrashSimulation.cpp`'s
  null write is the crash handler's deliberate test, reached only through a setting a server administrator sets. None
  was fixed: the XML rule accepts only `defusedxml`, which the checkers job does not install, and the crash tests need
  the fault. 475 of `static-analysis`'s 691 entries and 62 of `semgrep`'s 125 have no reason yet; they are triaged by
  31 March 2027.
- **`mr-dependency-check` asks about the native and vendored libraries, and honours the VEX document.** A change that
  moved the pin of OpenSSL, Boost, libpq, zlib, QuickJS, MariaDB Connector/C, 7-Zip or SQL Server Compact was listed
  as not checked; each new version is now asked of `api.osv.dev` by its upstream tag, with a control version beside it
  as `dependency-scan` asks, and held on a Critical or High advisory. One older than its repository's first tag is
  held by that tag's advisories, and a library OSV has no usable data for is listed as UNKNOWN. A Critical or High
  advisory now passes when the base's VEX document declares the package at that version not affected, matched by
  purl, as well as when the base's exceptions accept it. Its self-test holds zlib 1.2.11 on CVE-2022-37434.
- **`dependency-scan` rates an advisory that carries only a CVSS 4.0 vector, and dismisses one only for the version a
  VEX statement names.** It read CVSS 3.x vectors and database ratings alone, so CVE-2026-76844, in zlib's
  `gz_write`, which its CVSS 4.0 vector rates 8.3, High, with a fixed version, counted as Unknown; it is now scored by
  `mr-dependency-check.py`'s calculator, and would hold the release. The VEX document says zlib 1.3.2 is not affected:
  `gzwrite.c` is not vendored. A VEX statement dismissed an advisory for any version of the library; it now needs the
  library at the version scanned among its products, by purl. Its new `--self-test` runs in `checkers`.
- **`dependency-scan`'s line for the CI image counts the kernel headers apart.** Of the image's 3,608 advisories on 6
  October 2026, 3,535 were against `linux-libc-dev`, the kernel's user-space headers, which hold no kernel code:
  Trivy files every advisory against the kernel's source package under it. The CI image's test case now counts those
  apart from the 73 against the image's other packages, each by severity. Every advisory stays in the reports and
  `findings.txt` (and, since the next entry, one against the headers that has a fixed version is not affected).
- **`dependency-scan` and the release gate treat `linux-libc-dev` as a header-only package, whose advisories are
  not affected.** Ubuntu published `linux-libc-dev` 7.0.0-38.38, and `batch44`'s pipeline 2924313539 (8 October
  2026) counted 60 kernel advisories against the CI image's 7.0.0-34.34 headers as High or Critical with a fixed
  version (3,610 in all, 15 dismissed by the VEX document), on which the release gate would have held the next
  release: headers that hold no kernel code, in an image that holds no kernel, under which Trivy files every advisory
  against the kernel's source package. The new rule `HEADER_ONLY` in `build/ci/dependency-scan.py` names the image's
  header-only packages, that one today, with the reason: an advisory against one of them in the CI image's scan is
  listed in every report with the status "not affected: header-only package, the advisory is against the kernel it
  was built from" - the GitLab report's record carries it in its `details`, under the record's own id, so the
  dashboard shows it - and counted apart in the image's test case, and it is never among the high or critical with a
  fixed version, by the scan or by `build/ci/security-gate.py`, which reads the same rule off the record's package and
  location and says in the release job's log what it spared. The rule reaches that name alone, and the image's scan
  alone: a High with a fixed version against any other package, or against the same name in a lock file, counts as
  before; both self-tests hold it ([SECURITY.md](SECURITY.md), "Dependencies").
- **`build/ci/mirror-semgrep.sh` and `build/ci/mirror-tumbleweed.sh` handle no token.** `mirror-tumbleweed.sh` read a
  registry token from its standard input for `skopeo login`, and `repo-hygiene`'s Token-Permissions check failed on it,
  in `batch43`'s pipeline 2916285454, as a script that reads a token where no job runs it. Both now take a
  `docker login registry.gitlab.com` made before they run, best in a `DOCKER_CONFIG` of its own, and hand the file it
  wrote to skopeo, read-only and for the copy's destination alone; they refuse a login kept in a credential helper,
  which skopeo's container cannot reach. Token-Permissions no longer fails on either.
- **The DNSSEC validator is tested against answers captured from the public DNS** (work item 80).
  `DnssecResolverTester`, which the service's class tests run on Windows and `dane-tests` on Linux, now runs the
  resolver on its own thread against replies captured on 7 October 2026 (`DnssecCapturedAnswers.h`), with its clock
  at their capture and the built-in root anchors: live.com's and cloudflare.com's MX RRsets validate from the root's
  keys down, debian.org's under both its RSA/SHA-256 and ECDSA P-256 signatures, and the same answers expired,
  altered - the exchange, the signature, the key tag it names, the key-signing key the DS record names, an MX record
  added - and stripped of their signatures fail with the reason the log gives, while the signed record repeated in
  another spelling is counted once - nothing is sent, and the tester's answers reach no other thread and no cache. It
  also holds the canonical form itself: an exchange compressed and in mixed case is written out and lowercased, and
  a pointer that points at itself or forwards, or a name that runs past its RDATA, is refused. The suite's DNS server
  now sends an MX record's exchange compressed against the question, as resolvers do, so every signed delivery in
  `SMTP/DaneIndeterminateLookups` goes through it, with a test of an exchange in mixed case besides;
  `Security/DnssecCompressedMx` (Windows) holds the verdicts and the debug log's reason.
- **The static-analysis job's baseline holds 691 findings, down from 887.** The 191 that the security dashboard's
  triage of 5 October 2026 marked to harden or fix are changed in the code, and five more went with the same edits:
  the Linux command line's and `openpgp-writer`'s unchecked writes to the terminal discard their results with
  `(void)`, members and locals take their value where they are declared, the REST handlers' assignments leave their
  `if` conditions, branches with one body are joined, `StdString.h`'s SSMIN and SSMAX return by value, its no-op
  statements are cast to `(void)` and FmtArg's assignment is deleted, `IMAPResult`'s assignment checks for
  self-assignment, the ini file's and the file-backed settings' whole-file read loops stop at the first short read,
  two static-only classes' private destructors are no longer virtual, the LDAP client's option pointer and the
  recipient callout's read buffer say in so many words what they hold, the PST reader's child sets its signal mask
  with `pthread_sigmask`, three widening casts widen before the arithmetic, and six of the seven sites cppcheck rated
  High read as its model follows them (the seventh, CrashSimulation.cpp's null pointer, is deliberate and stays).
  Apart from the two entries under Operations, none of them changes what the server does. `openpgp-writer`, a test
  tool, now ends with 1, saying why, when its message could not be written to standard output, where it ended with 0.
- **The testers' four timing checks of computation that 6.3.6 left out take `HM_TEST_TIME_BOUND` as well**, ten times
  their margin where the aarch64 RPM builds run them under qemu-user: ImipTester's three fixed margins (a quarter of a
  second, 0.02 s, a quarter of a second) over the expansions they are measured against, and XmlDocument's 3000 ms for
  its worst-case document. The 6.3.6 tag's `rpm-fedora43-aarch64` failed once on the 0.02 s margin (0.1547 s against
  0.1424 s). The checks that race a real sleep or a deadline - NeverBanNameList's two and `network-deadline-tests`' -
  keep their bounds: emulation does not slow a sleep, and ten times theirs would pass a deadline that was not kept.
- **The Tumbleweed RPM jobs take their images from the project's own registry.** openSUSE's registry drops a rolling
  snapshot within weeks: the x86_64 image 6.3.6 was built in (`bc34ab33…`) answered 404 on 6 October 2026, so a
  pipeline on a runner without it cached could not have started those jobs. The 6 October snapshot's linux/amd64 and
  linux/arm64 images are copied, digests preserved, to
  `registry.gitlab.com/hmailserver/hmailserver/mirror/opensuse-tumbleweed`, which `.gitlab/ci/packages-rpm.yml` now
  pins; `build/ci/mirror-tumbleweed.sh` takes a new snapshot when one is wanted, and Renovate leaves the pins alone.
  Both packages build in it (clang 23.1.2, the rest as before), every tester passing - the aarch64 one's under
  qemu-user - and each installs in its architecture's image and answers `--version`.
- **The Linux jobs run on the CI image as the project's registry holds it, named by the digest of its manifest, and
  each starts by checking that it was started so.** `.linix` named `hmailserver-ci/linux:26.04-52e9976e73bf`, which
  each runner with the tag `linix` found in its own image store and never pulled: a tag is only a name, and a job that
  is root on linix in effect - the privileged `linix-systemd` entry's - could have given it other bytes, which every
  later job, a release's packages among them, would then have built in (the second security review of 6.3.6,
  SUP2-1). On 6 October 2026 the image was pushed with `build/ci/build-linux-image.sh --push` to
  `registry.gitlab.com/hmailserver/hmailserver/ci-linux`, which anyone can pull from, and `.linix` now names it
  `ci-linux:26.04-52e9976e73bf@sha256:4eead001…`: Docker takes an image named so only as the registry holds that
  digest, and linix and server2 both pull it from there. `.linix`'s `before_script` is
  `python3 build/check-ci-image.py --job`, and `container-image-sbom`, `container-image-start` and
  `container-image-start-arm64`, which take its image without extending it, start with the same step: it stops the
  job before it builds anything unless the image it was started in is named so, with the tag of the commit's own
  Dockerfile. The registry's cleanup policy, still switched off, keeps this image's tags and the Tumbleweed mirror's
  by name since the same day.
- **The CI image was rebuilt on 8 October 2026, from the unchanged Dockerfile, for Ubuntu's kernel-header and libc
  updates.** The image pushed on 6 October carried `linux-libc-dev` 7.0.0-34.34; Ubuntu then published 7.0.0-38.38,
  and `dependency-scan` in `batch44`'s pipeline counted 60 kernel advisories against the headers as High or Critical
  with a fixed version, which would have held the next release. The rebuild carries `linux-libc-dev` 7.0.0-38.38,
  `libc6` 2.43-2ubuntu2.4 and `openssl` 3.5.5-1ubuntu3.7, and `.linix` names it
  `ci-linux:26.04-52e9976e73bf@sha256:35048402…`. The rule for a rebuild at the same Dockerfile: it builds to the same
  tag, `26.04-<hash>`, and `build/ci/build-linux-image.sh --push` moves that tag in the registry to the new image's
  digest; the pin in `.linix` follows the digest, in a commit of its own, with `build/check-ci-image.py` holding the
  tag beside it to the Dockerfile's; and the old digest, which would be left without a tag - GitLab.com's registry
  removes an untagged manifest about 24 hours after nothing references it (its online garbage collection; the cleanup
  policy governs tags alone) - is kept under a dated tag, `26.04-<hash>-<yyyymmdd>`, for the commits that name it.
  The 6 October digest, `sha256:4eead001…`, was kept so by hand on 8 October, as
  `ci-linux:26.04-52e9976e73bf-20261006`, within the hour after the tag moved off it, by a copy inside the registry
  that preserves the digest; `master` at 9b2001ce9 and `batch44`, which name it, pull it as before. `--push` now does
  that itself, before the push: it reads the digest the tag names (`skopeo inspect --raw`, without credentials,
  hashed), gives it the dated tag by `skopeo copy --all --preserve-digests` with the login's file mounted read-only
  for the destination alone, as `mirror-semgrep.sh` does, reads it back and holds it to the digest, then pushes and
  prints the pin line with the dated tag; a second rebuild the same day takes `-2` on when the dated tag names another
  digest and does nothing more when it names this one; and it stops before the push when the registry cannot be read.
  `build/check-ci-image.py` holds the script to those steps in their order, and its self-test runs the script with a
  docker of its own. The registry's cleanup policy keeps the dated tags by name, its ninth pattern,
  `^[0-9]{2}\.[0-9]{2}-[0-9a-f]{12}-[0-9]{8}$`
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "The image").
- **DBSetup.Tests runs on xUnit.net v3** (`xunit.v3.mtp-off`), as ControlPanel.Tests and ImportTool.Tests have since
  6.3.6; it is new in 6.3.6 and had kept the deprecated `xunit` 2.9.3.
- **The HTTP listeners' deadlines are tested without the service.** `HttpServerTester`, which `ClassTester` runs on
  Windows and the new `http-server-tests` program runs on Linux, holds an HttpServer of its own on 127.0.0.1 with a
  one-second request deadline: an answer of 300,000 bytes its handler returns after the deadline, or after the
  connection's own, arrives whole; a reader that stops part way through a 16 MB answer is reset; a kept-alive
  connection answers the request after a late one. Each of the first three fails with its part of the fix taken out -
  the reset's on Linux, where the reader was given an orderly end after 3,563,520 of the 16,777,216 bytes; Windows
  resets such a connection of itself. `http-server-tests` needs the loopback, so the source packages' builds leave it
  out, as they leave out `network-deadline-tests`.
- **The full-text index's lookups are tested without the service, and the searches over several of its windows with
  it.** `PersistentMessageIndexTester`, which `ClassTester` runs, holds which messages a lookup's rows rule out - read
  whole, not given up on, a needle in none of their terms as the scan finds it - and that a message's own terms never
  rule it out; each rule taken out fails it. Two regression tests search 450 messages, three windows of the index,
  with the index off and on: `FullTextSearch.AMailboxLargerThanOneLookupIsSearchedAWindowAtATime` over IMAP, under a
  1 MB examine ceiling the scan alone cannot keep to, and
  `RestApiSelfService.TheFullTextIndexNarrowsTheWebmailSearchAWindowAtATime` through both of the webmail's searches,
  which with the index caught up open the five messages that hold the word and no other.
  `build/perf/webmail-timing.py` records the files a search opened.
- **`build/perf/webmail-timing.py` reaches the end of a run on a folder of a hundred thousand messages, and its times
  hold the server's work alone.** The REST API refuses a credential's 201st request in ten seconds with 429, and the
  HTTP pass's walk of 500 pages met it, which ended the run: the tool now asks again after `Retry-After`, keeps the
  refused request and the wait out of every time, a wall time included, says each wait on standard error and counts them
  in the JSON's `waits` and under its table. A request whose session has ended signs in again, once, untimed. A search
  whose request fails - an answer cut short, a timeout, a status other than 200 - is recorded in its cells and the run
  goes on to the next measure, the tool ending with 1 once it has written everything; the browser loads the page again,
  and signs in again if it asks, before its select-all. A request went out a second time after any failure before; now
  only when a kept-alive connection the server had closed fails before an answer, so a search cut short is not asked
  for, and timed, twice. The folder tree's first request, which after a restart is the one that reads the folder from
  the database, has a row of its own, *folder tree, first time*, where it was in the JSON alone and *first page, first
  time* read as the cold open. `--self-test`, which `checkers` runs, puts HTTP mode against a REST listener of its own
  that refuses one request with 429, ends the session once and cuts a search's answer short, and each of those three,
  taken out of the tool, fails it.
- **The load generator's `prepare-hmailserver` gives the generator's IP range a priority above every range holding its
  address**, at least 20 as before: a connection takes the range of the highest priority, and every installation has *My
  computer*, 127.0.0.1 at priority 30 with spam and virus protection on, which passed over the range for a generator on
  the server's own machine - the Windows bench. **`imap-sessions` takes its idle point once the server's memory has
  stopped falling** (`--idle-wait`, 120 seconds at most by default; 0 takes it at once, as before), and its table says
  the memory per session is not valid when the idle point is still above the first step's memory: on server1 two runs
  begun ten seconds after the one before took 526 MiB of private bytes for idle against 238 and 271 MiB at 25 sessions.
  A run that passes its last step says so, the limit not reached. `smtp-out` warns on Windows when asked for more than
  12,000 messages with the sink on the server's own machine. `loadgen.py self-test` holds the range's priority, the idle
  point, the last step and what a sink on the loopback is.

- **The REST API's OpenAPI document is tested without the service, against the router that answers it.**
  `RestApiOpenApiTester`, which `ClassTester` runs on Windows and the new `openapi-tests` program runs on Linux, in
  `linux-build` and in the source packages' builds, reads the document `GET /api/v1/openapi.json` serves - built by
  the same function, `RestApiServer::OpenApiDocument_`, without a listener - with a reader of its own that refuses a
  member named twice in one object, which the server's reader and a browser's take without a word; holds every path
  item to the fields OpenAPI 3.0 gives one and every operation to its summary and responses; gives every documented
  operation, its templates made concrete, to `ParseRoute_` and fails on one the router does not know, the operations
  `ProcessRequest_` answers before it parses a route excepted; and fails on a route kind the router can name that no
  documented operation reaches, the two refusals' own kinds excepted. The reader is first proved on documents
  written to fool it. With the two paths written twice put back, it fails naming `paths./api/v1/status` and
  `paths./api/v1/me/folders/{id}/messages`; with a documented path misspelt, naming the operation the router does
  not know.

- **The test simulators' `TcpServer` stops its listener once, and before it says it has finished.**
  `SMTP.BounceReports.OurOwnNonDeliveryReportIsNotCountedTwice` failed on `batch44`'s Windows gate (pipeline
  2928270199, 9 October 2026) with a NullReferenceException inside `TcpListener.Stop`, called from
  `TcpServer.Dispose`. The listen thread said it had finished and only then stopped its listener, so a test that
  disposed the server as soon as `WaitForCompletion` returned, as most of the suite's tests that use one do, stopped
  the same listener at the same moment, and .NET Framework's `Stop` tests its socket, loads it again to close it and
  then clears it: a thread held up between the test and the load while the other clears it calls `Close` on nothing.
  The listen thread now stops the listener before it signals, under a lock that lets whichever of it and `Dispose`
  comes first stop it, once. A listener whose construction threw (a port out of range) had left the listen thread a
  null to stop, a NullReferenceException on a thread nothing catches - which ends the Linux suite's test host, and
  which NUnit's agent on Windows, run with the legacy unhandled-exception policy, lets end the thread without a word -
  and is now nothing to stop. `Dispose` and the thread-pool thread that served the client, which both put the
  connection away when a test disposes the server mid-conversation, each take it out of its field in one step, where
  one could test it, the other clear it and the first then use it; and a TLS handshake that fails, or that `Dispose`
  cuts short, is kept with the conversation's other failures instead of being thrown out of a thread-pool thread. The
  new server-free fixture `ServerFreeTests.TcpServerLifetime`, in both suites, holds the listener's part: fifty servers
  in a row have stopped listening when `WaitForCompletion` returns, a port the listener refuses fails `StartListen`
  and lets nothing escape the listen thread, and a server disposed before any client came stops once. With the class
  as it was, the first failed on server1's bench within three servers, twice, and on Linux at the fourteenth; the
  second failed on the bench on the NullReferenceException that escaped the listen thread, and on Linux ended the
  test host and the run. A stress harness on server1 built the class as it was and as it is: before, 1,827 of 2,000
  servers in a row were still listening when `WaitForCompletion` returned, after none. With another process
  suspending its threads at random instructions, as a busy virtual machine's host does to its guest, two
  `TcpListener.Stop` calls at once threw that NullReferenceException eleven times in 448,998 tries; the class as it
  was, its servers disposed mid-conversation by the tests' own steps, threw it from `TcpServer.Dispose` - the gate's
  very stack - eleven times in 3,184,997 servers, and once from `DisposeSocket` on a thread-pool thread, and the
  class as it is threw nothing in 3,289,402.

- **A simulated server disposed with its accept still pending no longer ends the Linux suite's test host.** .NET's
  `TcpListener.Stop` disposes its socket and only then marks itself stopped, and `EndAcceptTcpClient` turns the
  aborted accept's SocketException into an ObjectDisposedException only once it is marked; the thread-pool thread
  that ends the accept can get there first, and `TcpServer.OnAcceptSocket`, which caught ObjectDisposedException and
  ArgumentException, let the SocketException ("Operation canceled", 125) out of a thread where nothing catches it -
  the test host crashed. It can happen to any test that disposes a simulator before its last expected client came,
  `ServerFreeTests.TcpServerLifetime`'s own server disposed before any client among them, and the entry above made it
  far likelier: two threads stopping the listener at once had cleared that mark sooner than the one that stops it now.
  `OnAcceptSocket` now ends such an accept as the stopped listener's other answers end it, and keeps a failed accept
  while the server is not being stopped for `WaitForCompletion` to report. A new test in that fixture disposes five
  thousand servers in a row with their accept pending: with the class as the entry above left it, it crashed the Linux
  test host in each of four runs on server2 in the CI image; with the class as it is it passed in each of three. A
  harness on .NET 10.0.12 disposed such servers one after another: the class as it was crashed after 25,398 to
  128,602 of them in each of four runs, as the entry above left it after 116 to 1,857 in each of eight, and as it is
  it ran 1,094,254 in a minute, and 1,934,356 more in three minutes from sixteen threads on two processors, without
  one. .NET Framework's `Stop` marks its socket closed before it closes it, so on Windows the test passed before as it
  passes now.
- **`secret_detection` runs GitLab's secrets analyzer from a copy in the project's registry**
  (`mirror/secrets:7.40.0-1f54871d97a7`, made by the new `build/ci/mirror-secrets.sh` as `mirror-semgrep.sh` makes
  `semgrep-sast`'s). GitLab's registry had stopped serving the digest pinned on 5 October 2026, and the job failed
  before it ran in `master`'s pipelines of 7 and 9 October; `build/scan-secrets.py`, the landing's and the hooks' scan,
  which takes the same image, could run only where an earlier pull had left it cached. The copy is 7.40.0 as GitLab
  rebuilt it on 9 October: the analyzer, gitleaks 8.30.1 and GitLab's rules byte for byte the ones before, Alpine's zlib
  1.3.2-r1 where it had r0; over `master`'s whole history the two images' gitleaks found the same.
  `build/check-release-chain.py` fails when the job names another image, or names it over two lines, which
  `scan-secrets.py` would not read, or by a tag that is not the digest's (ContinuousIntegration.md, "Change checks").
  Neither scan reads a path under a directory named `build`, `bin`, `obj`, `target` or `packages`, which GitLab's rules
  leave out, in the old image as in the new: not this tree's `build/` (ContinuousIntegration.md, "Before a push: the
  secret scan").
- **Every job of a pipeline for `master`, a `batch*` branch, a `v*` tag or a schedule runs on the project's own
  runners, and none on GitLab's shared runners.** The namespace's compute minutes ran out on 9 October 2026, and
  `batch46`'s pipeline could not start `semgrep-sast-gate` (`stuck_pending_no_matching_runners`, then
  `ci_quota_exceeded`): a change gate that cannot run holds every landing. Forty jobs named no runner and ran there -
  the checks, the three change gates, `secret_detection`, `pages` and `package-repos`, the release's `release`,
  `release-sbom`, `release-sign` and `release-publish` and the rehearsal's two, the container image's jobs in cosign's
  and regctl's images and `container_scanning`, the command-line packages' jobs and the release channels' but
  `snap-upload` and `chocolatey-publish`, `helm-chart-package` and `installer-smoke`. `.gitlab-ci.yml`'s `default:`
  now gives every job that names no runner of its own, GitLab's templates' jobs among them, the tags `[$HM_RUNNER]`:
  `linix`, which linix and server2 carry, in such a pipeline. A merge request's pipeline and an unprotected branch's
  still run their checks on GitLab's hosted runner (`saas-linux-small-amd64`, which the workflow's rules set for them)
  and never on the project's machines, which are registered for protected refs alone and take no code nobody here has
  reviewed: a fork's in the fork, on the fork's minutes; Renovate's, one a maintainer runs here for a fork's merge
  request, and a fix by an unprotected branch on the project's, so that while those are spent they fail before they
  start - a batch, the pipeline every change lands on, needs none. `build/check-release-chain.py` fails when a job
  that names its own tags names none, a variable other than `HM_RUNNER`, tags no runner of the project's carries, or
  none of `linix`, `linix-systemd` and `bench150`, when `default:` or `HM_RUNNER` changes or anything but a workflow
  rule sets it, and when the workflow's rules, tried for every kind of pipeline, give a protected ref's pipeline
  GitLab's runner, give any other the project's, or make a job tagged for the project's runners where they take
  nothing; its self-test breaks each in `.gitlab-ci.yml` and in an included file. `build/ci/repo-hygiene.py`'s
  Token-Permissions judges a job on `default:`'s tags by the workflow's rules. Under linix's limits `semgrep-sast`
  took 188 s over the batch and found what it finds on server2. The Sigstore certificate of a release's signature now
  names the runner self-hosted, an extension nothing that verifies a release reads (ContinuousIntegration.md, "Every
  job on the project's own runners"). And the ID token a release tag's signing jobs are given is now held on linix or
  server2 while they run, where a VM of GitLab's made for the one job held it: whoever is root there, or in the
  `docker` group, could have Fulcio certify a key as that release's pipeline while the token is valid. Those machines
  build the release's Linux packages already, and only a protected ref's pipeline reaches them (the same section,
  "What it trusts").
- **The message indexer's stop and start, as fixed above (*Correctness*), leave `static-analysis` nothing new to
  say.** Run over them as the job runs it, cppcheck and clang-tidy found four findings in `MessageIndexer.cpp` that
  no baseline holds, none of them a defect in what the code does, and each is now written so that the tools have
  nothing to say: the destructor called a Stop that may throw from a destructor that may not (cppcheck's
  `throwInNoexceptFunction`, High, and `bugprone-exception-escape`), and now catches what it throws and ends the
  process, as the exception leaving the destructor did, since the thread would otherwise run on over the indexer
  gone; the catch by which a Start on the indexer's own thread takes back a Stop made there was empty
  (`bugprone-empty-catch`) and now notes that it took the stop back, which the test after it reads; and the
  tester's two asks that do the same are one branch (`bugprone-branch-clone`). What the server does is unchanged.

### Documentation

- **The 6.3.6 section names the CVE identifiers GitLab's CNA has assigned since the tag**, six so far, and says what
  the correction notes posted on the CVE requests on release day establish: the string replacement and the header
  unfolding were reached in the default configuration by the DMARC test while a message was received, the replacement
  from 6.0.0; the SPF expansion by the DMARC test as well; the Sieve match since 6.2.22; the JSON reader's large
  bodies from 6.3.2; on Linux, the error reports of a command run as root went into the service account's log
  directory from 6.3.0, and in 6.3.0 to 6.3.2 the server's DNS resolver waited without a timeout. It also says that
  such a command now writes no report but the stored-secrets code's `HM7496` and `HM7497`, which go to standard
  error, where it said every report went there.
- **SECURITY.md's vulnerability management policy says what every change must pass, and what declares a finding not
  exploitable.** A new first section, "Every change", names the route to `master`, each gate, what it evaluates, what
  is a violation and what is not, and what no gate reads; "Declaring a finding not exploitable" names the two
  documents, read from the target branch and never from the change, who reviews an entry, which lands first, and that
  a fix waiting upstream is no reason, a baseline entry or an inline comment no declaration. ContinuousIntegration.md's
  "Merge request checks" is "Change checks", with how the base is found, the retries, the analyzer's copy and what the
  checks cannot hold, and "Landing on master" names the four batches that have landed through it and the two
  settings the owner makes once this batch has; `.gitlab/security-exceptions.yml`'s header counts its 500 entries by
  scan and by group, and says how each group was read.
- **Performance.md's index-on probe passes when the search for `quokka`, followed to its end, opened its hundred hits
  and no other message**, which is the full-text index having read every other one. It asked for one search answered
  in one request before, which a request no longer gives on SQL Server Compact however far the index has got, since it
  asks the index two hundred messages at a time under its own clock; the margin after the probe goes with it.
  "Searching" says how a search asks the index now and what that costs on SQL Server Compact.
- **Performance.md and PublishedNumbers.md carry the first numbers measured on the Windows bench**: server1, 6.3.6 and
  SQL Server Compact, 7 October 2026, each run's JSON and table kept in `build/perf/runs/2026-10-07-server1/`.
  Performance.md's column with the full-text index off is filled - the folder's cold read 1.5 s, a page in 8 to 37 ms
  wherever it lies, a search to the end in 50 requests and some 12 s, a keyword set over the hundred thousand in 38 s -
  and its column with the index on is not, with the reasons: on 6.3.6 every search with the index on failed (fixed
  above), and the probe that shows the index caught up never passed. Six of the seven cells of PublishedNumbers.md's
  Windows table are filled, SMTP out at 12,000 messages a run, since 20,000 cannot complete on Windows with the sink on
  the same machine - each relay holds one of the loopback's 16,384 dynamic ports in TIME_WAIT for two minutes, and some
  4.5 % bounced - and IMAP's 50 sessions a core is the last of the default steps; memory per session is left to be
  measured, its runs having taken a busy server for idle; the linix comparison is not yet run. The recipes change with
  what the run found: Playwright's browsers go under `C:\perf` (`PLAYWRIGHT_BROWSERS_PATH`), since the install removes
  from the folder it installs into - otherwise the administrator's own profile - the browsers of every other Playwright
  version, and on server1 it removed the machine's owner's; the index probe runs every ten minutes, since on SQL Server
  Compact it shares the one connection with the indexer; the Windows load runs set `MaxDeliveryThreads` to the shipped
  10, where a gate leaves 50, and put back the bench's value and its `MessageStoreFsync` as they were, not deleted; SMTP
  out waits 130 seconds before each run for the ports; and an empty folder of the load domain, which the bench's
  pre-flight takes for an orphan, is removed. Rule 5 says the generator's range now outranks *My computer*, and the
  Windows table says that its "fsync on" has a database that flushes every ten seconds.

---

## [6.3.6](https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.6) - 2026-10-06

Everything that landed on `master` after the `v6.3.5` tag, from 29 September 2026 ([compare `v6.3.5` with
`v6.3.6`](https://gitlab.com/hmailserver/hmailserver/-/compare/v6.3.5...v6.3.6)). It is the last release for the
time being: after it the project makes security releases only, and the 6.3 line receives security fixes until 30
September 2027 (below). 6.3.6 fixes the security flaws the project's own reviews of the code found, CVE-2026-103011
among them, and takes OpenSSL 4.0.3 into the Windows installers (both below), fixes what 6.3.5's release and the first
live updates from 6.3.4 found, replaces with the project's own code the last code in the server whose licence a
distribution would have had to settle, and carries every file the release pipeline makes, the ARM64 Windows installer
among them. The schema moves to **6108** in three steps: 6106, which changes no column, gives every external account's
*days to keep messages* the one meaning both fetchers now read, keeping each account doing what it did or stopping it
deleting (What changes on upgrade without a switch, item 22); 6107 finishes the greylisting key schema 6072 began
(item 23); and 6108 adds the never-ban list by reverse name and, for each SMTP listener, what it does with `AUTH`, On
for every listener an upgrade finds (item 25). The security notes come first, then what changes on upgrade without a
switch. The operator documents named below are in `hmailserver/docs`, which has twelve more of them than it had at
6.3.5.

**Releases pause after this one.** 6.3.6 is the last release for the time being, by the owner's decision of 28
September 2026, and no feature release follows it until the maintainers say otherwise in [SECURITY.md](SECURITY.md),
"While releases pause". A confirmed vulnerability still ships, in a release of its own that is 6.3.6 with fixes -
6.3.7, 6.3.8 and so on - on the targets SECURITY.md gives: a critical or high one as soon as its fix has passed the
gates, and none held past 90 days. Other defects are fixed on `master` and ship with the next security release, or
when releases resume. The 6.3 line receives security fixes until 30 September 2027; before that date the
maintainers write in SECURITY.md whether support continues, and if nothing is written, it ends then. There are
still no maintenance branches: an installation of 6.3.5, 6.2 or anything older is upgraded in place to the newest
release.

**What the release carries**: 62 files and 160 links (ContinuousIntegration.md, "What a release carries"). Beside
the x64 installer is `hMailServer-6.3.6-arm64.exe`, for Windows on ARM, Authenticode-signed like it. It is a
preview: the ARM64 build is cross-compiled on the project's x64 Windows runner and has never run on ARM64 hardware,
so read it as built to work ([WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md)). If you install it on an ARM64
machine, please say how it went, working or not, in [an issue](https://gitlab.com/hmailserver/hmailserver/-/issues):
the project has no ARM64 machine of its own. SQL Server Compact has no ARM64 build, so a new ARM64 installation is
set up against SQL Server or PostgreSQL, and a silent one is given its database on the command line. For Linux the
release carries the generic `.deb` and `.rpm` for x86-64 and, for the first time since 6.3.3, for AArch64 -
cross-compiled and tested under emulation, since no AArch64 machine has run them - and each Debian 12 and 13, Ubuntu
24.04, Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed release's own package for both
architectures. It also carries both AppImages, with the source of the libraries they carry and of those their
runtime links; the source tarball, `hmailserver-6.3.6.tar.gz`, reproducible and with nothing non-free in it, from
which the Debian source package and the RPM spec in the tree build the server as a distribution does
([SourceTarball.md](hmailserver/docs/SourceTarball.md), [SourcePackages.md](hmailserver/docs/SourcePackages.md));
and the Helm chart. Every file has a Sigstore bundle beside it, every compiled file and the tarball an SBOM, and
SLSA build provenance names every file by its SHA-256 ([SECURITY.md](SECURITY.md), "Verifying a release" and
"Build provenance"). The apt and dnf repositories and the other channels publish from `master` once their keys and
accounts exist; none is live as this release is made ([Channels.md](hmailserver/docs/Channels.md)).

**The first container image**: `registry.gitlab.com/hmailserver/hmailserver:6.3.6`, also tagged `6.3` and `latest`,
one index over linux/amd64 and linux/arm64, each platform's image built from this release's `.deb` of its
architecture. It is pushed from this release's own pipeline once the release is published, by a job started by
hand, signed with cosign as that pipeline, each platform's image with its SBOM attested to it; until then the
registry holds no image. In the container the update check names the image to pull and installs nothing
([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

**A Nix flake and Alpine Linux**: a Nix flake of the project's own at the repository's root, with a NixOS module
(`services.hmailserver`), and an APKBUILD and an OpenRC service for Alpine Linux, on whose musl the server now
builds and runs; both are built without the live update and name the command that updates them, and neither is in
a distribution's archive yet ([Nix.md](hmailserver/docs/Nix.md), [Alpine.md](hmailserver/docs/Alpine.md)).

**Signing, and who installs this release by the live update.** Every file is signed with Sigstore as this
release's own pipeline at the group's path,
`https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.6`, issuer `https://gitlab.com`, with the
project's id, 86730559, in the certificate: the first release signed there since the project moved into the group
`hmailserver` on 29 September 2026. Every server from 6.3.4 on has that signer compiled in and requires the id for
it, so a server on 6.3.4 or 6.3.5 installed from the x64 installer or the release's x86-64 `.deb` or `.rpm` is built
to install this release by its live update: from 6.3.5 with 6.3.5's own file as its rollback image, and from 6.3.4,
which carries no Sigstore bundle, without one. No live update had installed a release signed at the group's path
when these notes were written. The update is applied by the version already installed, so three things of 6.3.4
and 6.3.5 still hold for it:

- a Windows service running as `NT SERVICE\hMailServer` downloads and verifies the installer, then leaves the
  install to an administrator (`HM7463`), whose *Install update* on 6.3.4 and 6.3.5 cannot do it: install this
  release by hand, once, as the next paragraph says, and every release after it with one click;
- on Linux, a backup destination inside `/var/lib/hmailserver` fails the backup taken before an unattended apply
  (`HM5014`), and the apply is refused until the destination is moved or the release installed by hand
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "From 6.3.4 on, by the live update");
- a server whose `UpdateSigningIdentity`, `UpdateSigningIssuer` and `UpdateSourceRepository` still name the path
  the project began at - set so that a server from 6.2.28 to 6.3.3 could install 6.3.5 - trusts that path alone and
  refuses this release. Clear the three first - in the Control Panel, with `DELETE /api/v1/settings/ini/<name>`, or
  on Linux with `hmailserver --set-setting <name>=` - and not in the file, which 6.3.4 and 6.3.5 put back.

A server from 6.2.28 to 6.3.3 trusts only the GitHub workflow that signed the releases up to 6.3.3, and is upgraded
to this release by hand, once, after the backup [Upgrading.md](hmailserver/docs/Upgrading.md) describes; so is a
server installed from an AArch64 package or an AppImage, whose last release was 6.3.3. One pointed at this
project's feed with `UpdateFeedUrl` downloads the installer at every check and refuses it, so keep
`UpdateAutoDownload` off there until then. The three settings that let such a server install 6.3.5 do not reach
6.3.6, and must never name the group's path ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "A server from
6.2.28 to 6.3.3"). No release before this one carried the ARM64 installer, so no ARM64 Windows server has a live
update to take it; from 6.3.6 one finds its installer by name, and an x64 server is never offered it. A server
installed from one of this release's per-distribution packages is offered the next release's generic package by its
live update, which apt refuses on Debian 12 and dnf on EL 9, so there it is upgraded with its package manager and
its own release's package of the next version, not by the live update
([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "The download links").

**One click installs an update where the service cannot.** A Windows service running as `NT SERVICE\hMailServer`,
the account a new installation has had since 6.3.4, is not an administrator and cannot run an installer, so up
to 6.3.5 *Install update* told the administrator to download the release and run it by hand. From 6.3.6 the
Control Panel on the server installs it: *Install update* on the Status page, one Windows prompt, and everything
else happens by itself - the installer is checked, the backup taken, the version to go back to fetched, the
server drained, the release installed, the service started, the previous version put back if it does not start,
and the Control Panel opened again, signed in, on the outcome. What runs elevated trusts nothing the service
wrote: `hMailServer.exe /InstallUpdate` checks that only administrators can change the installation, copies the
installer out of the service's `Updates` folder into `AdminUpdate`, which only administrators can change, and
checks the copy itself - the Authenticode signature with the publisher pinned to Progressive Robot Ltd and the
Microsoft chain it is issued under, revocation online, the version and the architecture, a release newer than
the running one, and the Sigstore bundle, in a child process with the administrator's rights taken away.
`SeDebugPrivilege`, which the stage needs to read a Control Panel of another user's and the Updater to end the
programs running from the installation folder, is switched on for that alone and off again before anything else
runs, the installer included. The Control Deck, the REST API and a Control Panel connected to another computer say
where to click. The service
never makes an administrator or SYSTEM run anything ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md),
"The live update"). On LocalSystem the live update is as it was.

A server on 6.3.4 or 6.3.5 under `NT SERVICE\hMailServer` still says to install this release by hand, since that
is compiled into it; nothing in the feed or the release can change what it does. Download
`hMailServer-6.3.6-x64.exe` from this release, check that `Get-AuthenticodeSignature` reports it `Valid` and
signed by `CN=Progressive Robot Ltd`, and run it as an administrator, keeping the service account on the
installer's *Service account* page. On a machine that has
not seen the file before, Windows SmartScreen may say it "prevented an unrecognized app from starting": it
judges a file by how many machines have run it as well as by its signature, and a new release is new to it.
*More info* names the publisher, Progressive Robot Ltd; with the signature checked, *Run anyway* installs it.
The one-click install never meets SmartScreen: the installer is not downloaded by a browser, and the Control
Panel's updater starts it itself.

### Security

The code of the legacy Blowfish scheme of stored secrets that the project inherited from the original hMailServer,
which this release replaces with its own (under Operations), had two heap buffer overflows. The one in its
encryption is in every version up to 6.3.5, and 6.3.6 fixes it. The one in its decryption is in every
version up to 6.3.3, and was fixed in 6.3.4, whose notes did not say so. Both were found by the project's own
review. The same release makes the helpers of COM `Utilities` that do work in the server answer only a caller that
has signed in, or an event script running in the service (What changes on upgrade, item 1). GitLab, the CVE
Numbering Authority for projects on GitLab.com, has assigned them CVE-2026-103011 (the encryption, fixed in 6.3.6)
and CVE-2026-103010 (the decryption, fixed in 6.3.4);
[work item #49](https://gitlab.com/hmailserver/hmailserver/-/work_items/49) gives the details from the day of the
release. Upgrade every server running 6.3.5 or earlier.

Calendar messages delivered by mail (iMIP) - a cancellation, an attendee's answer, an updated invitation - changed
the event stored in the recipient's calendar without the server checking who had sent them, in 6.3.4 and 6.3.5;
6.3.6 applies one only when the server verified that it came from the event's organiser, or from the answering
attendee or a delegate of theirs, and delivers anything else as an ordinary message; an invitation filed from a
sender it could not verify no longer keeps the organiser's own out of the calendar (What changes on upgrade,
item 17). A room or a piece of equipment took a DMARC pass for an outside organiser's domain as the organiser's
own, whichever sender at that domain had written the message, in 6.3.4 and 6.3.5; 6.3.6 believes that organiser
only when the message's From is the organiser's address. A room or a piece of equipment also took an invitation
an application sent through the send API, with a key allowed to send as the organiser's address, as the
organiser's own, in 6.3.4 and 6.3.5, where its documentation said such a message is not believed; in 6.3.6
neither a room nor a calendar takes a message sent through the send API as an account's own.

The Control Deck is now served under a script policy that lets its own script run and nothing else - the SHA-256 of
its one inline script, which the server takes from the page it serves - with Trusted Types, whose one policy refuses
markup the Deck never builds, and with X-Frame-Options and Cross-Origin-Opener-Policy, where 6.2.28 to 6.3.5 allowed
any inline script on the page and earlier versions sent it with no policy at all; a proxy in front of the server
must now pass the page through unchanged ([ResponseCompression.md](hmailserver/docs/ResponseCompression.md), Behind
a reverse proxy).

The webmail's pages are now served with Trusted Types, whose one policy takes out of what a page puts into itself
the elements the page never builds and admits only the page's own two script addresses, and with X-Frame-Options and
Cross-Origin-Opener-Policy, none of which any earlier version sent.

An account's formatted signature is now given to the webmail only as the subset the webmail's own editor admits -
basic formatting, links to http(s) and mailto, inline images, a simple table, and nothing else - whatever is stored,
and the account holder's own save is stored that way. The webmail puts a signature on the screen as markup,
including in an administrator's support session as the account, and earlier versions cleaned it only in the browser
that saved it, so a signature written another way could carry script into the browser that later rendered it. The
signature an administrator sets in the Control Panel, over COM or over the account route is stored as it is written
and appended to the account's outgoing mail with its styles and hosted images, as before; a domain's signature,
which no browser renders as markup, is unchanged.

A message attachment is now served only under a media type from a list of those no browser runs, applies or renders
as a page - pictures, sound, video, PDF, text, the office formats and the archives - and as `application/octet-stream`
otherwise. Earlier versions kept any declared type that was not one of a few named ones, which left the
executable-JavaScript media-type aliases a browser also runs; the attachment already carried
`Content-Disposition: attachment`, `nosniff` and a sandbox policy.

The REST administration listener's plain loopback port now answers only a request whose `Host` it expects and refuses
any other with 421 before the request is authenticated, so a web page that points a name of its own at this machine
can no longer reach the console through a browser on it (CVE-2026-104659). It answers `localhost`, an IP address, the
server's host name and the host of each address configured as the one the webmail is reached at from outside. A reverse
proxy that forwards the browser's own `Host` needs that name to be one of these: upgrading behind such a proxy, set the
webmail's outside address (for example `confidential_base_url`) or the server's host name to it first
([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md), Which Host the listener answers). A listener with its
own certificate is unchanged.

A wrong administrator password over REST now costs a growing delay, capped at ten seconds, before the next one from
the same source is looked at - on every route that takes it, the password asked again at an administrator passkey's
registration among them, and from the loopback address too, where the brute-force protection of the mail ports does
not reach and where every browser behind a reverse proxy on the same machine arrives. Earlier versions looked at
every guess at once from there. A source is an IPv4 address, `::1`, or any other IPv6 address's `/64`, and an
address that carries an IPv4 client's own - `::ffff:a.b.c.d`, the NAT64 and SIIT prefix `64:ff9b::/96`, Teredo's
`2001::/32` - counts as that IPv4 address, since one `/64` of those is every client of the translator; the
listener's own short refusal after the auto-ban's limit covers the `/64` of an IPv6 address as well, and such an
address as its IPv4 address, as `::ffff:a.b.c.d` always was (What changes on upgrade, item 35). It is a delay,
cleared by the right password: a source that keeps guessing keeps it waiting while it does. Behind a reverse proxy
on the same machine every browser shares one source, so one client's wrong guesses keep the password sign-in waiting
for everyone while they last; the administrator's API keys, passkey and single sign-on are not touched by it
([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md), The administrator password's delay). Found by the
project's own review and its second review of the release (CVE-2026-104659).

A response sent to a browser that came over HTTPS now carries `Strict-Transport-Security` for a year, which no earlier
version sent, so the browser does not go back over plain HTTP where the credentials could be read; it is never sent on
the plain loopback path, and it does not include the names under the one the browser used.

Creating or revoking a REST API key, and enrolling or disabling the administrator's second factor over REST or COM,
are now recorded on the hash-chained audit trail the Deck's Audit view reads, where earlier versions wrote them only
to the rolling application log or not at all; the key's token and the second-factor secret are never recorded.

The Windows installers, x64 and ARM64, carry OpenSSL 4.0.3, published on 29 September 2026 with fourteen advisories
against 4.0.x, where 6.3.5's x64 installer carried 4.0.2. Of the two rated above Low, CVE-2026-84782 (High) is in
DTLS, which hMailServer does not use, and CVE-2026-84783 (Moderate, in 4.0.0 to 4.0.2 only) applies to the Windows
server: a use-after-free when several threads first use the same certificate at once, which a remote peer can turn
into a crash when connections build their first chains to the same trusted CA certificate together. In the Windows
server those are the connections to a port that asks for client certificates, whose certificates are all checked
against that port's CA file. Its outgoing connections share none: the one context its SMTP, POP3 and SpamAssassin
clients share holds no trust anchor, since a remote server's certificate is checked by Windows' own chain
verification or against DANE records, and each of its other TLS clients loads its own for each connection. Of the
twelve Low ones, nine are in QUIC, DTLS 1.2, the CMP client and SM2, which the server does not use, and one in
`SSL_set_SSL_CTX` during a handshake, which it never calls. The other two are fixed with the rest: CVE-2026-35189,
the memory a certificate with many relative CRL distribution points makes OpenSSL allocate, which a server that
hMailServer connects to can send, as can a client on a port that asks for client certificates; and CVE-2026-54872, a
timing leak in ECDSA signing on curves other than NIST's. Every Windows installer from 6.0.0-B1 to 6.3.5 carried a
4.0.x that CVE-2026-84783 affects: 4.0.1 up to 6.2.24, and 4.0.2 from 6.2.25. The Linux packages and the AppImages
use the OpenSSL of the distribution they run on, and the container image its base distribution's, and take OpenSSL's
fixes from there.

The webmail now hands the browser an attachment it decrypted itself, of an S/MIME message (since 6.3.2) or an
OpenPGP one (since 6.3.4), as a file of no type (`application/octet-stream`) rather than under the type the message
named, so that the browser only ever saves it; every version from 6.3.2 to 6.3.5 is affected (CVE-2026-103647). Such a
part that names no file is saved as `part` with the extension of the type it declares when that type is on a short
list - plain text, CSV, a calendar, a contact card, PDF, ZIP, a key, a forwarded message, PNG, JPEG, GIF, WebP, MP3, MP4
and the three Office documents - and as `part` alone otherwise; no extension a browser renders as a page or a desktop
runs is ever given, and the file's type stays `application/octet-stream`.

In the same way, a file the webmail saves for a second account shown beside the first is now always of no type, and
a photo it shows for that account, or an attachment its offline store answers without a connection, keeps the type
the server named only when that is an image type the page shows (or, offline, a PDF); 6.3.4 and 6.3.5 used the type
as it came.

One more hardening change, found by the project's own review: a mailbox key encrypted at rest - an account's or a
domain's recovery key, opened, made or sealed again - and the keys derived from it are written over when the code
holding them is done, however it ends: a return, and now an exception thrown through it as well (from the database,
an allocation, the log), which left such a key in memory freed without being written over, in 6.3.4 and 6.3.5.
The lines of a stored-secrets key file that `--import-stored-secrets-key` reads, the key's own line among them, are
written over the same way, the key's base64 is no longer copied into a string that was freed as it stood, and the line
being read no longer grows a character at a time into larger buffers, freeing each smaller one - most of the key in
base64 - as it stood. And a key put into base64 - the stored-secrets key `--export-stored-secrets-key` writes, an
account's mailbox key made when its encryption is switched on and held under the machine's own key until its first
sign-in - no longer leaves the whole of it in base64 in the buffer it was encoded in, freed as it stood, as it did in
6.3.4 and 6.3.5. Nor does the stored-secrets key read from its line in `hMailServer.ini`, as the service reads it at
its start and each key verb when it runs, leave the whole of it in hex in a copy freed as it stood, as it did in 6.3.4
and 6.3.5.

The Control Deck is held tighter to what the server hands it, found by the project's own review. It draws an
address - the release page the update feed names, the documentation wiki's, an eDiscovery export's - as a link only
when it is an http or https address, where 6.3.4 and 6.3.5 drew the address as it came. The update check keeps a
release's page only as an https address, where every version from 6.2.28 to 6.3.5 kept whatever the feed gave. The
sign-in card shows a single sign-on error carried in the page's address only in a tab that started a sign-in at the
identity provider in the last hour and has not loaded the Deck since, inside a sentence of its own, where 6.3.4 and
6.3.5 showed the text of any such address; the webmail's sign-in page is held to the same rule, where 6.3.4 and
6.3.5 showed any such text there too. A translation that is not text, or carries a character HTML reads, is left
out when the Deck loads a language, as the build already refused it, where 6.3.4 and 6.3.5 used the server's copy
of the catalogue as it was. And the ids and the counter three of its views take from the server are drawn as
numbers whatever the answer holds.

A file sent from the webmail as a link (`/files/{token}`) is now served under the type its sender declared only
when that type is on a list of types no browser runs, applies or renders as a page - pictures other than SVG, sound,
video, PDF, plain text and CSV, the office documents and the archives - and as `application/octet-stream`
otherwise, the files links already hold included; every version from 6.3.2 to 6.3.5 refused a few named types and
served any other as declared. A contact's photo is served under its card's type only when that is one of the same
list's pictures, and as `application/octet-stream` otherwise, where 6.3.4 and 6.3.5 served it under the type the
card declared. Found by the project's own review.

The size an account declares for a file sent as a link (since 6.3.2) or for a password-protected message (since
6.3.4) is now checked against the most either may be before it is made a whole number, so that every declared size
counts against that ceiling and against the account's own; found by the project's own review.

A read of another account's mail through the REST API by the server's administrator, a domain administrator or a
role holding `view_mailbox`, `view_archive` or `view_discovery`, and a support session opened as an account, are now
recorded on the audit trail; 6.3.4 and 6.3.5, the versions with the trail, recorded none of them (Administration,
below).

When the server declines to reach an address that is not public for somebody who named it - a list's one-click
unsubscribe and a correspondent's key from their domain's Web Key Directory in the webmail, the server of another
account a reader adds there, the delivery of an inbound webhook route an API key made - the reason it gives now names
the host as it was given and never the address the server resolved it to, and for another account's server says the
same whether the name does not resolve or leads inside the network. From 6.3.2 the unsubscribe's answer, and from
6.3.4 the others, named the private address a host led to. A refusal by `HttpProxy`, which these requests go
through from 6.3.6 (What changes on upgrade, item 20), is reported to such a person as the setting's, by its status
code alone, and neither by the proxy's address nor in the words of its answer. Found by the project's own review.

Expanding a recurring event now looks each occurrence up among the event's excluded dates (`EXDATE`) by halving a
sorted list, where it went through every one of them in turn. An event can carry tens of thousands of excluded
dates in the megabyte an invitation may have, and an invitation is filed in the calendar on delivery, so such an
event cost about a third of a second each time a calendar was read over a span it covers (the webmail, CalDAV, a
room's booking check) - and, before this release, several seconds of a delivery thread for a cancellation of some
of its occurrences (What changes on upgrade, item 18), which also asks of the event a few expansions fewer than it
did. Found by the project's own review.

The Linux live update's apply step, `hmailserver-update`, which runs as root on a request the server's service account
writes, took the unit it stops from that request, and for an AppImage request the image - the program that checks the
new file's signature - as well, in 6.3.4 and 6.3.5. A service account that had been taken over could therefore have
root run a program of its choosing, on a `.deb` or `.rpm` installation with the live update's path unit started (the
packages' default) as on an AppImage. 6.3.6 takes the unit and the image from the helper's own command line alone,
refuses an AppImage request where that names no image, checks after the stop that the server's unit is down and that
nothing of the service account still runs, and verifies, runs and installs only copies it has made after the stop,
which nobody else can read until they have verified (What changes on upgrade, item 24). Windows is not affected. An
AppImage run under the path unit keeps the script copied to `/usr/lib/hmailserver/` when its update units were
installed, which the AppImage's own update does not replace: copy 6.3.6's there and give it `--image`. Until a Linux
server's helper is 6.3.6's, `systemctl disable --now hmailserver-update.path` turns the apply off, and the check and
the download go on; install 6.3.6 with the package manager. Found by the project's own review (CVE-2026-104658).

The same helper went on writing into the service account's Updates directory by name after it had started the server
again, in 6.3.4 and 6.3.5: it appended each line of its log to the log's file, gave the log and the outcome to the
service account with `chown` and `chmod`, which follow a symbolic link, and renamed both onto names through which a
symbolic link to a directory would have led them into that directory. A service account that had been taken over,
and was running again by then, could therefore have root append the log's lines to a file of its choosing, give that
file to the service account or change its mode, or put root's file into a directory of its choosing. 6.3.6 opens the
directory, and makes and opens both files, while the server is confirmed stopped; gives them to the service account
through the open files before it starts the server; writes them through the open files alone; and renames each into
place through the open directory, which replaces a symbolic link and never follows it. Windows is not affected. Found
by the project's own review (CVE-2026-104658, with the helper's first flaw above).

Some COM objects that carry no credential were not refused what only a signed-in caller may do. A message, its
attachments and a fetch account that a program creates for itself carry no credential, and neither do the objects an
`Application` handed out once a later `Authenticate` on it fails. With an administrator password set, a local Windows
user with no hMailServer credential could through them queue mail, read and write files as the service's account, and
attach a fetch account to any account. This is in every version up to 6.3.5, as in the original hMailServer. Outside
the server's own event scripts these are now refused, unless the program's own process holds an `Application` that
signed in, whose credential they then take; reading or writing a file on the server's disk through a message is the
server administrator's alone, and mail a signed-in account queues claims to be from only an address it may send as
(What changes on upgrade, item 26). Found by the project's own review (CVE-2026-104660).

An IMAP session that has turned compression on (`COMPRESS=DEFLATE`, RFC 4978) no longer has the server inflate
whatever one read of its compressed input expands to. Every version from 6.2.28, the release that brought the
extension, inflated all of it at once, and from 6.3.4 kept what did not fit the receive buffer and took each command
line off the front of it, copying the rest - and the full receive buffer - for every command. One read is at most 512
compressed octets (Boost.Asio's read into the emptied buffer the connection used), so what waited was at most about
half a megabyte, about what 6.3.6 itself lets wait, and the copying cost a few microseconds a command: at deflate's
highest ratio, 0.18 s for the 66,000 eight-octet commands of one read, beside what serving them costs (measured on
linix, 3 October 2026). The server now inflates no more than twice its receive buffer ahead of the commands it has
read, keeps the rest compressed until those are taken, and takes each line at a cost of its own length. Found by the
project's own review.

Outbound DANE (RFC 7672) used TLSA records of usage DANE-EE (3) alone. A recipient's MX host whose DNSSEC-validated
records were all of another kind - DANE-TA (2), the usual record for a certificate from an authority, or records this
server cannot use - was delivered to as if it published none: over TLS when it was offered, and in the clear when it
was not seen. The server now verifies DANE-TA as well as DANE-EE, through OpenSSL's own DANE support, and treats a
validated set none of whose records it can use as section 2.2 of the RFC asks: TLS is required to that host, a
delivery that cannot have it is deferred, and TLS reports count the host under the policy type `tlsa`. Such a set asks
for no authentication, so the host's certificate is not judged for it; it is still verified where something else asks
for that - an MTA-STS policy in enforce mode, a remote-domain policy or a rule, or this server's own outbound security,
`SmtpDeliveryConnectionSecurity` at SSL or STARTTLS (required), or a route's, with `VerifyRemoteSslCertificate` on
(What changes on upgrade, item 28). In every version up to 6.3.5; found by the project's own review and its second
review of the release (CVE-2026-104704).

Outbound DANE did not hold against anybody who could disturb a recipient domain's DNS: on the path between this
server and its resolver, a resolver that answered falsely, or on the path between the resolver and the domain's own
servers. A lookup of the MX or TLSA records that did not complete - no answer, `SERVFAIL`, a reply that would not
parse - and an answer from a signed zone that came without its signatures, or with no records and no proof that none
exist, were all read as "not signed", and the destination was delivered to as one that publishes no DANE: over TLS
with any certificate when it was offered, and in the clear when the offer was taken away. And the hosts a delivery
used came from the plain, unvalidated MX lookup even when the validated one named others. The validating resolver now
walks from the root to the name and reads an answer as unsigned only below a delegation a signed zone proves
unsigned; under a signed zone such a lookup is indeterminate and the host is not used, as RFC 7672 section 2.1.1
asks, a delivery with no host left is deferred, a validated MX RRset decides which hosts are used, and an empty
answer counts as no records only with the zone's NSEC or NSEC3 proof of it (What changes on upgrade, item 36). In
every version up to 6.3.5; left so, it would have undone the DANE-TA and unusable-record changes above as well.
Found by the project's second review of the release (CVE requested).

The ManageSieve service (off by default, and on the loopback when it is turned on) served one connection at a time,
and each read on a connection had a limit of its own with nothing bounding a command line, the TLS handshake after
`STARTTLS` or the time before authentication as a whole, so one client that had not signed in could hold the service
for as long as it kept its connection going, and a signed-in client sending `NOOP` every 29 seconds held it for ever.
Since 6.2.3, the release that brought ManageSieve. Every wait now runs to a deadline (What changes on upgrade, item
29); connections are served side by side, an address has one served at a time before it signs in, a connection that
runs out of time before signing in counts towards the auto-ban, and a signed-in session and an account are bounded
(item 37). Found by the project's own review and its second review of the release.

The URI blocklists (SURBL, URIBL, the Spamhaus DBL) were asked about the first 16 names a message's links gave, the
plain-text part's before the HTML part's, so that links ahead of a listed one could keep it from every list. The
SURBL test now reads every link of both parts and asks each list about every registrable domain they name, up to 200
for a message, up to sixteen lookups at a time within the list's ten seconds, so that links beside a listed domain
do not make it less likely to be asked about. A message whose links name more than 200 domains is not asked about
whole, and scores `SurblTooManyDomainsScore` instead, so that hiding a listed link among made-up domains does not
come free: 5 by default, the stock mark threshold and more than one list's stock score of 3 (What changes on
upgrade, item 30). A list that lists a host name and not its domain is asked about the host names there is room left
for, two drawn under a domain and in an order the links do not set, so made-up host names can still make a listed
one less likely to be asked about. In every version up to 6.3.5; found by the project's own review.

The REST listener's tables of sign-in state no longer let one person push other people out of them. A full table
of browser sessions dropped its least recently used session whoever held it, so any account holder signing in over
and over with their own password could sign out every webmail tab and every other browser on the server that sat
idle for a minute, from 6.2.28 to 6.3.5, and the administrator's Control Deck with them from 6.3.0. The pending
halves of a single sign-on and of a passkey sign-in, which anybody may start, were held the same way, so a client
asking for sign-in starts a few times a second - a hundred a second for passkeys - made every other person's OpenID
Connect, SAML or passkey sign-in fail when their browser came back, in 6.3.4 and 6.3.5. In 6.3.6 a holder of browser
sessions at its cap gives up its own oldest and a full table the oldest of whoever holds the most, among holders tied,
one of the domain holding the most: an account or the administrator holds at most fifty sessions, and the
administrator's are never taken for anybody else's sign-in (What changes on upgrade, item 32). The pending sign-ins are
no longer held on the server at all: the browser carries each one in its cookie, sealed with AES-256-GCM under a key
of the server process's, and only a finished sign-in is recorded, to make it good once, so no number of starts can
push anybody's out; a passkey registration's challenge, which only a signed-in credential asks for, is held by that
credential, fifty at most (item 33). Found by the project's own review and its second review of the
release (CVE requested).

An account holding a role with `manage_settings` could put a key of the `[Settings]` section back to an earlier value
through the audit trail's put-back, in 6.3.4 and 6.3.5, where that section answers to the administrator password
alone everywhere else; and the breach check's endpoint test, asked by an API key or such an account, reached
addresses on the server's own network and reported whether and how fast something answered there, in 6.3.4 and
6.3.5. Both are now the administrator password's: the put-back refuses such a row to anybody else, and their test
reaches public addresses only (What changes on upgrade, item 34). `manage_settings` remains the server
administrator's equal - it names the hosts the server connects to and the commands it runs - and is documented so
([Roles.md](hmailserver/docs/Roles.md), What manage_settings reaches). Found by the project's own review.

A webmail or Control Deck page loaded before the upgrade to 6.3.6, and still open after it, could have mail deleted
on the other server that its reader had asked to leave there: the restart ends the page's session and it signs in
again in place, with its old script, and 6.3.5's webmail sends -1 for *Leave the mail there* and its Deck's hint says
-1 keeps every message, where 6.3.6 reads -1 as delete each message there once it has been collected, for IMAP as for
POP3 (What changes on upgrade, item 22). The 6.3.6 webmail and Deck now say what they mean by the number
(`days_to_keep_meaning`, 6106), and a negative `days_to_keep_messages` from a browser session without it is refused
with 409 and nothing saved - on `/api/v1/me/fetch-accounts`, its import's start and the administrator's
`/api/v1/accounts/{address}/fetch-accounts` - so such a page says to reload. An API key, a password and COM, which send
the documented meaning, are not asked for it. Found by the project's second review of this release; no released
version reads -1 as 6.3.6 does.

The webmail kept the S/MIME certificate of a signed message it opened as a correspondent's certificate, to encrypt
replies with, whether or not the server found its chain trusted, under the first address the certificate named rather
than the message's From address, and beside any different certificate already kept for that address without asking.
Every later message the account encrypted to that address was then encrypted to such a certificate as well, so whoever
held its key could read it. This is in 6.3.2 to 6.3.5. In 6.3.6 a certificate is kept only when its chain is trusted,
only under the From address the signature was checked against, and a different one for an address that has one only
when the reader chooses **Keep this certificate** (What changes on upgrade, item 38). Certificates kept before the
upgrade stay where they are: the Security page lists each with its address and fingerprint, and **Remove** takes one
out ([SmimeAuthority.md](hmailserver/docs/SmimeAuthority.md)). Found by the project's own review (CVE requested).

The webmail's view of a message's HTML (`GET /api/v1/me/messages/{id}/html`) put the images the message embeds back in
place with work that grew with the square of the number of references to them, and wrote an image out again for every
reference while counting it against the inline budget once, so one message could hold one of the REST listener's four
workers for minutes and have it build a document of gigabytes; with a few of them the webmail, the Control Deck and
the REST API stopped answering anybody. This is in 6.3.2 to 6.3.5. In 6.3.6 the route reads the HTML twice, once for
the images it names and once as it writes them, in a time that grows with the HTML's size alone, and writes 24 MB of
images at most in all, every reference counted (What changes on upgrade, item 39). Found by the project's own review
(CVE requested).

The webmail inflated a compressed OpenPGP message in the reader's browser tab with no limit on its size, so a small
message could take the tab's memory each time it was opened, in 6.3.4 and 6.3.5; the server was not affected. In 6.3.6
each compressed layer is inflated to 32 MB at most and a message past that is refused with a sentence that says so
(What changes on upgrade, item 40). Found by the project's own review.

Where a message has an HTML part and no text part, the server reads its text with the tags dropped and the entities
decoded: for the snippet the webmail's message list shows, for an IMAP client's `PREVIEW`, and for the page of a message
that expires. The decoding replaced each entity in place and moved the whole rest of the text along every time, so its
time grew with the square of their number, and one message of a few megabytes of entities held the REST worker that
listed the folder for minutes - no click needed, since the list is the first thing the webmail asks for - and held an
IMAP thread the same way; with a few of them the webmail, the Control Deck and the REST API stopped answering anybody.
This is in the message list from 6.3.3, the page of a message that expires from 6.3.4 and `PREVIEW` from 6.2.22, up to
6.3.5. The same replacement was reached by other readers of a stranger's text from 6.0.0: the DMARC anti-spam test, on
by default, verifies the DKIM signatures of each message an unauthenticated sender delivers from an address in a range
with spam protection (the shipped Internet range has it), up to the anti-spam scan size (1 MB by default), on the task
thread that accepts the message, before the server answers it, and the reader of a signature removes the spaces of its
`h=` and `b=` tags with that replacement, so a signature holding a great many spaces held that thread for seconds to
tens of seconds with nobody opening anything, and a stream of such messages held every thread the queue gives those
tasks, so that the server answered no received message while it lasted; and IMAP `FETCH ENVELOPE` and `SORT` by date
read a message's Date field with it. So in every version from 6.0.0 up to 6.3.5. In 6.3.6 the server's string
replacement, which these and the rest of the server use, writes a result of another length in one pass, and its removal
of a character takes one pass too; what they make is as it was. Found by the project's own review (CVE requested).

An IP range's, an incoming relay's or a white-list entry's address set over COM - the Control Panel's IP ranges,
incoming relays and white list pages, or a script - that the server could not read as an address was stored as
0.0.0.0 without a word, in every version up to 6.3.5: a range whose lower address was mistyped began at 0.0.0.0,
with whatever it allows - relaying, delivery without authentication, no spam tests - and a relay or a white-list
entry so written covered every address below its upper one. 6.3.6 refuses such an address with the reason, and the
object keeps the address it had, as the REST API answers one with a 400. On Windows it matters more in this
release, which reads more texts as no address there (What changes on upgrade, item 42): `192.168.001.001`, which
Windows read as 192.168.1.1, would otherwise have become 0.0.0.0. Found by the project's own review.

Parsing one IMAP `FETCH` command from a signed-in account could hold an IMAP worker thread - shared with SMTP and
POP3 - for minutes, and far longer. Reading the sections of a `BODY[]` item ran the server's case-ignoring string
search and replacement, which copied and lower-cased the whole item once for each occurrence, so the time grew with
the square of the number of `BODY.PEEK[` in the item; and splitting the item list cut each item off the front of the
rest by copying the rest every time, so it grew with the square of the number of items. One command line carries
about a quarter of a megabyte - some twenty thousand `BODY.PEEK[`, or over a hundred thousand items - which is already
seconds of one thread, and a `FETCH` is not held to one line: the text after each literal on its line joins the
command, which can so reach the eleven megabytes the command buffer takes, where the time grows by the square (40,000
occurrences took three minutes, 200,000 items two and a half). A few such commands from a few sessions stopped the
IMAP, SMTP and POP3 ports answering anybody. This is in every version from 6.0.0 up to 6.3.5. In 6.3.6 the
case-ignoring search, replace and contains take one pass, in a time that grows with the string's length alone however
long what is looked for (a `SEARCH TEXT` key sent as a literal of a megabyte, over a message of a few in the client's
own mailbox, was compared afresh at every position); the `FETCH` item parser walks the command by offsets and finds
each space once; and the other parsers of a client's text that had the same shape were made to take one pass as well:
the message set of `FETCH`, `STORE`, `COPY`, `MOVE` and `UID EXPUNGE` and its session's UID lookups, the `THREAD` and
`SORT` base subject, a `STORE`'s keyword list, the per-line scan of the command buffer that masks a password in the
log, the sets of `SEARCH`'s `UID` and sequence-set keys and of `UID FETCH`'s `VANISHED`, read once per command rather
than once per message, and a `HEADER.FIELDS` list, read once per command rather than once per message and name, each
section then reading the message's header once, field by field; and a `FETCH`'s part numbers, each part list read and
each encapsulated message parsed once for all of a message's sections, where each section listed the parts from the
first to the one it named and parsed a forwarded message afresh on its way to a part in it.
What each produces is as it was. Commands that named something over and over now cost what naming it once costs: `$`
(the saved search result) in a message set or as a `SEARCH` key stands in once, where each `$` copied the saved
result; a `SORT` criterion named again is left out, where each made a key for every message; and a `FETCH` whose
sections of one message add up to more than sixteen times the message is answered `NO [LIMIT]`, where all of them
were read and held before any was sent - each section counted by what it reads or makes, not only by what it keeps:
`BODY.PEEK[HEADER]<0.1>` read the message whole for its one byte, and `HEADER.FIELDS`, `HEADER.FIELDS.NOT`, `MIME`,
`TEXT` and a part's content cut short read or made theirs whole, as often as a command named them. A `$` written into
an element with anything else - `1:$` - is answered `BAD`, as no set. Found by the project's own review (CVE
requested).

The commands an administrator runs no longer go where the service's account can lead them. Since 6.3.4 a new Windows
installation runs the service as `NT SERVICE\hMailServer`, which has Modify on the program
folder's `Logs`, `Data`, `Temp` and `Database` folders, and the commands Setup, DBSetup and the Control Panel run
elevated - `/GrantServiceAccess`, `/Register` (on a registration that fails), `--apply-new-install-defaults`,
`--apply-profile` and `--create-stored-secrets-key` - and those an administrator runs from an elevated prompt -
`--upgrade-database`, the stored-secrets and store commands and the rest - wrote their errors to the error log in
`Logs`, and armed the crash oracle, whose record and dump go there too. `--apply-new-install-defaults` wrote the
self-signed certificate and its private key, and set the key's permissions, in `Data\Certificates` by path, and the
store commands that rewrite, remove or add messages (`--compress-store`, `--decompress`, `--encrypt-store`,
`--decrypt-store`, `--recount-store`) and the copies to and from an object store worked by path in `Data`, a copy
to the store handing what it read to a store the service's account reads. A program running as the service's
account - one that had already taken it over, through some other flaw - could put a junction or a mount point where
such a write was going and have an administrator's process create, overwrite, delete or change the permissions of
a file it chose, or read one, and so act with the administrator's rights. In 6.3.6
every command line but the service's own (`RunAsService`) and the two a developer runs (`/Debug`, `/Test`) writes
nothing to the error log, the Application log or an event script, has no crash oracle, and, where Windows has it
(Windows 11 22H2, Windows Server 2025), follows no junction a user who is not an administrator made; each command
says what it did and why it failed on its own output, which Setup, DBSetup and the Control Panel keep in their logs.
The certificate and its key are made below the `Certificates` folder held open from the drive's root down - no folder
of it a link - and each file is checked to be in it before a byte is written; and every store command, `--measure-store`
included, and every copy to or from an object store, first walks the data folder and stops, naming it, on a junction,
a mount point or a symbolic link in it that the administrators do not alone control. A link they alone control - owned
by `BUILTIN\Administrators`, `SYSTEM` or `TrustedInstaller`, and changeable or removable by no one else - is gone
through, and the path it holds is held from that volume's own root, every link on it - the folder it leads to
itself included - asked about in its turn, and checked to be where the link led; so a data folder, a folder above it, or a
domain's folder kept on another drive through a junction an administrator made and gave permissions only the
administrators may change is served, while one the service's account could have made or could turn elsewhere - a
link made in the data folder inherits the service's write rights until it is given others (docs/ServiceAccount.md) -
is refused with the reason. A folder no administrator may list, such as `System Volume Information` at a drive's
root, is skipped when the administrators alone control it rather than refused. `--measure-store`, which only reads and
reports to the administrator, is held to the same rule because it reads every file with an administrator's rights.
The store commands also reached each file by the name the database gives it, which the service's account may write:
a message row naming a file outside the data folder - a full path, or a part that is `..` - had an administrator's
`--compress-store` or `--encrypt-store` rewrite that file, or `--copy-store-from-object-storage` create it, and a held
part's name in `hm_storeblobs` could send a recount's removal anywhere. Now such a row stops every store command,
naming the message, before it reads or changes a file; an object store's key whose name would lead out of the
folder is not listed; and a held part's name is its SHA-256 in hex or names no file. A full path below the folder a
junction of the data folder leads to is in it; a legacy full path elsewhere is moved with the Data Directory
Synchronizer, which the message names.
On Linux the same commands run as root were confined to the data folder by path but followed a symbolic link the
service's account (the unit's `User=`, which owns the data and log folders) planted in it - `sudo hmailserver --compress-store`
would then read, rewrite or remove whatever the link led to, as root. The store commands, the object-store copies,
`--sample-domain`, the profiles and `--apply-environment` now drop to the service account - the data directory's owner,
with its groups - before they open anything when run as root, so each reaches only what the service reaches through any
link and leaves no file the service cannot open; a data directory whose way is not root's and its owner's alone - a
folder above it, or above where a link on the way points, that another account or group may write - is refused, exit
2, since a link put there could choose which owner, and so which account, that is; over a data directory root owns - a
service run as root - they run on as root only where that directory and everything under it, the log, temporary, event
and database folders and everything under them, the configuration that names them, and the ways to them are root's
alone, no link in them another account's and no default access control list giving another account a write, and make
what they make with every other account's write taken away whatever the umask, and are refused, naming the folder,
file or link, otherwise; and every other command line run
as root writes nothing to the log directory (it says what it did, and why it failed, on its own output; a report
that would have gone to the error log is not written, but for the stored-secrets code's `HM7496` and `HM7497`,
which go to standard error), as the Windows commands do.
`--create-database` makes its `.new-installation` marker without following a link of that name.
`build/check-elevated-verbs.py` holds all of it. The Windows commands were affected in 6.3.4 and 6.3.5, and so was the
Linux root store commands' symlink following; and in every release with Linux packages, 6.3.0 to 6.3.5, every command
run as root - `--set-admin-password`, which the packages have said to run with `sudo` since 6.3.0, and from 6.3.4
`--set-setting` and the stored-secrets commands among them - wrote its error reports into the service account's log
directory, `/var/log/hmailserver`, opening the day's error log by name and so through a link planted there; it needs a
program already running as the service's account and an administrator who then runs one of them (CVE requested).
`/GrantServiceAccess` also no longer leaves a Critical HM5028, "Check your database settings in hMailServer.ini", in the
Application log when the administrator running it cannot open a SQL Server database reached with Windows authentication:
the installer smoke VM showed one on every such move on 3 October 2026. Found by the review of the installer's fixes and
the sweep after it. [ServiceAccount.md](hmailserver/docs/ServiceAccount.md), "What an administrator's command writes".

Every user of a Windows computer that runs hMailServer could read its mail. The installer makes the `Data`, `Logs`,
`Temp`, `Database` and `Events` folders in the program folder, `C:\Program Files\hMailServer` by default, and left
them and `Bin\hMailServer.INI` with the permissions Program Files hands down, which give `BUILTIN\Users` Read. Any
account that can sign in to the computer - a Remote Desktop server's users, a shared machine's, a service running as
an ordinary account - could read every message; the logs, with their addresses, subjects and SMTP, IMAP and POP3
conversations; the built-in database, with the accounts' password hashes and the stored secrets' sealed values,
whenever the service that keeps its file open is stopped; a key file an administrator put in `Data\Certificates`; the
event scripts; and `hMailServer.INI`, with the database's password - sealed with the machine's DPAPI key, which any
account on the machine may use to unseal it - and the administrator password's hash and second factor. A data folder
an administrator moved under a folder at a drive's root, behind a junction, took that folder's permissions instead
once anything rewrote its own: on Windows Server, Users' Read and the right to make files and folders in it; on a
Windows client, Authenticated Users' Modify. Every release of this project, 6.0.0 to 6.3.5, installs so, as the
original hMailServer's installer did. 6.3.6's installer makes the six private to SYSTEM, the Administrators and the
service at every installation and upgrade, whatever account the service runs as (What changes on upgrade, item 45).
On Linux the AppImage made its folders under the user's umask, 0755 under the usual 022, in a home other users may
enter on many distributions; it now makes them 0700. The packages' folders were 0750 and their configuration 0640
already. Found by the project's own review (CVE requested). [ServiceAccount.md](hmailserver/docs/ServiceAccount.md),
"What the other users of the computer may read".

With bounce processing or complaint processing on, or a mailing list the list manager runs (none is by default),
reading an incoming delivery status notification (RFC 3464) or abuse feedback report (RFC 5965) could hold a delivery
thread for a minute and more. The reader of the returned message's headers part dropped the blank lines some senders
leave at its front two bytes at a time, copying the rest of the part on each, so the time grew with the square of
their number: 400,000 of them took eleven seconds, and the reader's 2 MB cap holds a million. A message shaped like a
bounce to a local address or to a list's `-bounces` address, or like a feedback report to the complaint mailbox, is
read this way without anyone opening it. The blank lines are now counted and cut off once. This is in every version
that reads these reports, 6.3.4 and 6.3.5. What is read is as it was. Found by the project's own review (CVE requested).

A header field read as text - the Subject and From of the webmail's message list, the header keys of IMAP `SEARCH`,
`SORT` and `THREAD`, a rule that tests a header, the spam tag put in front of a Subject, the abuse report reader, the
From the DMARC anti-spam test reads while a message is received - was unfolded by finding each line break from the end
of the value and moving everything after it, so the time grew with the square of the number of line breaks, which are
the sender's: an RFC 2047 encoded word may decode to as many as it likes. A value of 1.6 MB of short lines took half a
minute, each time it was read, and the default largest message holds over ten times as many, so a stranger's message
held the REST worker that listed its folder, or the IMAP thread that searched, sorted or threaded it, for minutes and
more, and a few such messages stopped the webmail, or the IMAP, SMTP and POP3 ports, answering anybody. The DMARC test,
on by default, reads the From of each message an unauthenticated sender delivers from an address in a range with spam
protection, up to the anti-spam scan size (1 MB by default), on the task thread that accepts the message, before the
server answers it, so a sender holds that thread for seconds with each such message and needs nobody to open anything.
This is in every version from 6.0.0 up to 6.3.5. In 6.3.6 the value is unfolded in one pass, to the same result. The
reading of a message's header from its file had the same shape at a larger scale: after each 4,000 bytes it searched all
it had read for the header's end, some five seconds for a header of 20 MB, the default largest message, at each of the
several reads of a message; it now searches only what each read adds. Found by the project's own review (CVE requested).

A review of the whole server for the same shape - input a remote party controls, read in time quadratic in its size -
found more of it, all fixed in one pass with the result unchanged, each proven against the previous code over generated
input and held to a single pass's time (`InputCostTester`, the Windows `ClassTester` and the Linux `input-cost-tests`).
Removing a MIME header parameter whose value is empty and directly followed by a `;` - a Content-Disposition such as
`filename=a.bat; filename=;` - **did not terminate at all**: it found the `;` after the empty value as the parameter's
own start, removed nothing and found the same parameter again, holding a worker thread at full load until the service
was restarted. It is reached when the attachment blocker renames a blocked attachment (not on by default) or a filename
is set over the REST API, from one received message; ten such messages could hold all ten default delivery threads for
good. The parameter's start is now taken as the last `;` before its value. In every version from 6.0.0. The same
removal was also quadratic in a Content-Disposition of many `filename*<n>` RFC 2231 continuations, now one pass.
Decoding a header field holding many RFC 2047 encoded words of an encoding other than base64 or quoted-printable
searched each to the end of the value, though such a word is copied as text and the search's result is never used
(a 1.1 MB header took 101 seconds); it is now searched only for a word that is decoded. Deleting a header field by
name - the spam classification removed on delivery, the list manager stripping a posting's fields, the external-sender
and impersonation taggers - took the fields one at a time, each a scan of the header and a move of its tail, so a header
of many fields of one name cost their square (80,000 took 18.6 seconds); they are removed in one pass. All in every
version from 6.0.0 up to 6.3.5 (CVE requested).

Verifying a received message's DKIM or ARC signatures built the canonical header and chose the fields the signature's
`h=` names in time growing with the square of the message's header: the "simple" canonicalisation put each continuation
line of a folded field in front of the ones already gathered, and both canonicalisations searched the gathered fields
from the bottom for each `h=` name. A message whose header holds very many fields, or a field folded over very many
lines, with a signature the sender signs for a domain they own so it reaches verification, held a worker thread for
tens of seconds, and up to ten signatures are verified per message by each of the DKIM and DMARC tests, on the threads
that serve delivery and SMTP. Each field is now built once and each `h=` name found in an index by name; what is hashed
is unchanged byte for byte. In every version from 6.0.0 up to 6.3.5 (CVE requested). Expanding an SPF mechanism's domain-spec cut a name
too long to look up (RFC 7208 7.1) one label at a time, copying the rest each time, so a record the sender publishes
with a macro that expands to a very long name made the SPF check - the SPF test's, or the DMARC test's, which is on by
default and evaluates the sender's SPF itself - take several seconds (a domain-spec of 2,000 `%{s}` took 4.9 seconds);
the cut is now made once. In 6.3.4 and 6.3.5, where that SPF implementation arrived (CVE requested). The JSON reader
kept the first of each member name by searching the members already kept, so an object of many members cost their square
(64,000 took 2.4 seconds); it reads the REST API's bodies and, where domain reports are on, a TLS report (RFC 8460)
mailed in by anyone, of up to 16 MB, on the delivery queue, where a 4 MB report took about 300 seconds - a few such
reports stopped all delivery for over an hour each. It now keeps a hash set. The reader is in the server from 6.2.28;
the REST API's largest bodies, the webmail's send and draft of 16 MB, go through it whole from 6.3.2 (in 6.3.0 and 6.3.1
what reached it was held to 64 KB), and the reports from 6.3.4 (CVE requested). Two more of the shape are fixed that
need the attacker's own account or script rather than a stranger's message: a Sieve `:matches` test backtracked over
every length of every `*` (`*a*a*a*b` over 800 characters took 44 seconds) and `deleteheader` erased its chosen fields
one at a time, both over a sender's header but driven by the account holder's own filter script, on the delivery threads
every account shares (since 6.2.22; CVE requested); and
an IMAP `LIST` de-duplicated its result lines by searching a list
for each (40,000 folders took 1.4 seconds a command), over the signed-in account's own folders (since 6.2.0). The Sieve match is now linear in the value times the
pattern, and `LIST` keeps a hash set. The calendar's handling of an organiser's `CANCEL` of a series from an occurrence on
took the stored event's `RDATE` values, rule parts, overrides and the server's own marks out one at a time, each a copy
or a move of the rest; a stranger's invitation is filed on delivery, so its `RDATE` can be as long as the 1 MB a message
is read for a calendar within allows, some 60,000 dates, in time growing with the square of their number (200,000 dates,
more than that limit allows, took 21.6 seconds). Each is now one pass. The `RDATE`, rule-part, override and mark loops
of the calendar are batch42's own and were never released; the `METHOD` and attendee `SENT-BY` loops beside them are in
6.3.4 and 6.3.5. All found by the project's own review.

The same review, checked again, found the rest of that shape. A date of a calendar object in a time zone the object
carries was placed by looking for the zone among the object's components and building it from its observances - and an
observance's own `RDATE` list was read to its end though only 500 of its values are kept - once for every date placed:
once for each `RDATE` value and each override's `RECURRENCE-ID` an organiser's `CANCEL` reads, and once for each
occurrence it names. Under the 1 MB limit, 20,000 dates and 1,500 overrides in a zone of 25,000 transitions took 20
seconds, and a `CANCEL` of 1,990 occurrences of a series in a zone of 60,000 transitions 4.2 seconds. Each zone is now
built once for all the dates of one reading (`ICalTimeReader`) and its `RDATE` list read no further than the 500 values
kept; what each date is placed at is unchanged, and the same two readings take 0.1 seconds. Those `CANCEL` paths are
batch42's own and were never released. A room's or equipment's booking, in 6.3.4 and 6.3.5, took a `CANCEL` of many
occurrences one occurrence at a time, reading the `RECURRENCE-ID` of every override the booking holds, its zone built
afresh, for each occurrence, and again for each to compare their `SEQUENCE`s; each override is now read once, with its
zone built once, and the overrides named removed in one pass, with the same result - the booking's own organiser's
`CANCEL`, so a lesser matter. The list manager removed a posting's own `List-*`, `Precedence`,
`X-hMailServer-List-Member`, `Return-Path` and `DKIM-Signature` fields one at a time and stopped after 64 of a name, so
that many could not cost their square: a posting that brought a 65th kept it in every member's copy, beside the list's
own field and under the list's signature (6.3.4 and 6.3.5). Every one is now removed, in one pass. Found by the
project's own review.

### What changes on upgrade without a switch

1. **COM `Utilities`: the helpers that do work answer a caller that has signed in.** `BlowfishEncrypt`,
   `BlowfishDecrypt`, `ResolveMXRecords`, `GetMailServer`, `IsLocalHost`, `CheckSieveSyntax`, `EvaluateSieveScript`
   and `CriteriaMatch` refuse a COM client that has not called `Authenticate`, with "You do not have access to this
   property / method", whether it reached them through `Application.Utilities` or created `hMailServer.Utilities`
   itself. They answer, as before, an account of any level or the administrator that has signed in, and an event
   script running in the service, which needs no change. `MD5`, `SHA256`, `GenerateGUID`, `IsStrongPassword`,
   `IsValidEmailAddress`, `IsValidIPAddress` and `IsValidDomainName` stay open to any caller, and a server with no
   administrator password and no second factor answers every caller as it did. A program or script outside the
   service - a scheduled VBScript, a PowerShell script, a tool of your own - that calls one of the eight creates
   `hMailServer.Application`, calls `Authenticate` with an account or the administrator, and uses that object's
   `Utilities`: a `Utilities` created on its own has no way to sign in. The Control Panel and the shipped
   `DecryptBlowfish.vbs` already sign in. In `Utilities` the type library changes help text only; no member or
   dispid of it is added, removed or moved.
2. **The live update's trust is read from `hMailServer.ini` alone.** `UpdateSigningIdentity`,
   `UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` are read
   from the file's `[Settings]` section on every platform, and never from the database, where 6.3.4 and 6.3.5 kept
   them. At the first start a value those versions stored in the database is taken out; where it said something the
   file does not, the error log names it once as `HM5809`, with both values, and from then on the file's value
   applies. A line for one of the five in `[SettingsOverride]` is ignored and named as `HM5809` at every start. COM
   and the REST API (409) refuse a change to them, and so the Control Panel, the Deck, a settings profile and the
   audit trail's put-back; a write of the value the file already holds is taken. Most servers set none of the five
   and see nothing. One that set a signer of its own over REST, the Deck or the Control Panel sets it in the file:
   on Windows with an editor run as an administrator, on Linux with `sudo hmailserver --set-setting NAME=VALUE`,
   then restarts the service. Where several servers share a database, the first 6.3.6 to start takes the rows out
   for all of them, so put the value in every server's file before the first is upgraded. `hmctl config export` and
   `Export-HmConfiguration` leave the five out, and `diff` and `apply` of an earlier export skip them with a line
   naming them; a backup does not carry them either ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "Where the
   trust is read from").
3. **The MX-record test falls back by the Public Suffix List.** When a sender's domain has no MX, the test judges it
   by its registrable domain's, which the server now finds with the Public Suffix List it already compiles in for
   DMARC (the list of 25 July 2026, its PRIVATE section included), where it cut the name to two labels, or three
   under a name in SURBL's list of 2006: `mail.example.act.edu.au` falls back to `example.act.edu.au`, not the
   suffix `act.edu.au`. A sender at a public suffix itself that has no MX - `github.io`, `co.uk` - now fails the
   test, since there is no registrant's domain above it to fall back to; under the old list `github.io` failed and
   `co.uk` passed. A name of one label is not judged, as before. Nothing to set.
4. **The SURBL test reads a link as a browser does, and a list's error answer is never a listing.** On a server
   with URI block lists (SURBL servers):
   - A link's host is the one after its user name - `http://anything@evil.example/` is asked about as
     `evil.example`, where it was `anything` - percent-decoded as UTF-8 and without its port, with a soft hyphen,
     a zero-width space and the other characters a browser deletes from a host deleted, and an IPv4 address in any
     form a browser takes is that address, an IPv4-mapped IPv6 address (`[::ffff:192.0.2.1]`) among them. A link
     with no scheme is read where a browser or a redirector follows one: an HTML attribute's value that begins
     with two slashes (`href="//evil.example/"`), and a query parameter's (`?u=//evil.example`). The host is asked
     about whole and cut to its registrable domain by the Public Suffix List, as above, and a host under a
     shared-hosting suffix also as the domain the list's ICANN section registers (`github.io` for
     `tenant.github.io`), as 6.3.5 asked. Messages whose links escaped every list in any of these ways can now be
     tagged.
   - A link to an address is asked about reversed, as a DNS list expects, and only so: it was also asked about in
     its own order.
   - The lists' error codes - `127.255.255.x`, `127.0.0.1`, `127.0.1.255` and `127.0.2.255` - never tag a message,
     whatever the expected result. A server that asked the Spamhaus DBL or ZRD with no expected result, or with
     `127.0.1.*` or `127.0.2.*`, no longer tags every message holding a link to an address. A URI list of your own
     that answers `127.0.0.1` for a listing no longer tags anything: have it answer another address, such as
     `127.0.0.2`. A list that refuses the server's queries, or answers `127.0.0.1` to every name of a message, is
     named in the application log at most every 15 minutes.
   - A host DNS cannot carry - a link that ends in a full stop, a label of 64 characters - is not asked about, where
     its failed lookup ended the list's check for the message, and the links are asked about in the order they come
     in the message, not sorted.
   - The sender lists' URL entries are matched against the same host, an address in its own order: an entry for the
     name before an `@` no longer matches, and nor does an address entry written reversed (`1.2.0.192` for a link to
     `192.0.2.1`). Write such an entry as the address itself. They are matched against every link of a message
     within the anti-spam size limit (item 21), where only the first 16 hosts were, so a message with an entry's link
     after sixteen others is now held or marked as the entry says.
5. **Backups leave out the live update's folder, and a destination inside the data directory works.** A backup no
   longer carries `Updates` - the installer or package waiting to be applied, the rollback image, the apply token
   and the last apply's outcome, most of the size of a backup taken before an apply - and a restore leaves this
   machine's own folder as it is and puts back no older archive's copy. A destination, the temp directory or the
   run's staging folder inside the data directory is left out of the copy, recognised by the directory itself and
   not by its spelling, and a restore leaves them, and the folder of the archive it restores from, where they are;
   the backup log names each. Up to 6.3.5 such a destination failed every backup (`HM5014`), and on Linux every
   unattended update with it; the nested copy a failed run left in the staging folder is now removed rather than
   archived. The same change removes two faults of the Linux packages, whose temp directory is inside the data
   directory, found by reading the code: an encrypted backup with messages failed the same way, and a restore of
   messages deleted the store it had just extracted. Nothing to set; a destination on another disk is still the
   better place ([Backup.md](hmailserver/docs/Backup.md), "A destination inside the data directory").
6. **`tlds.txt` is removed.** The Windows installer deletes `Bin\tlds.txt`, and the Linux packages no longer carry
   `/usr/bin/tlds.txt`, so apt and dnf remove it. Nothing reads it: a copy edited by hand has no effect any more,
   and the Public Suffix List compiled in decides. Error 4335, the list not read, is retired.
7. **The project's address.** The project is at `gitlab.com/hmailserver/hmailserver` since 29 September 2026, with
   its id, and the old addresses redirect. The Control Panel's links, the policy link of the server's
   `/.well-known/security.txt`, the systemd units' `Documentation=` and the packages' home page name it. A new
   database gets `HelpWikiBaseUrl` `https://gitlab.com/hmailserver/hmailserver/-/wikis/`; a database set up before
   keeps the value it has, as it keeps any setting - `https://gitlab.com/Progressiverobot/hmailserver/-/wikis/`
   unless an administrator changed it, which GitLab redirects, so the help panes' *Open on the wiki* still works. It
   is changed on the Control Panel's Advanced page under General, on the Deck's Settings page, as
   `help_wiki_base_url` over REST or as `Settings.HelpWikiBaseUrl` over COM ([Help.md](hmailserver/docs/Help.md),
   "The wiki link"). This release and every one after it are signed at the group's path (above).
8. **The live update keeps its outcome, tries a failed check again once, and keeps a verified installer.**
   - After an update, `Status.UpdateApplyOutcome`, `apply` in `GET /api/v1/update` and the Control Panel keep the
     outcome until the service stops or the next outcome replaces it; on 6.3.4 and 6.3.5 the check a second after
     the start erased it. The update that installs 6.3.6 is the first whose outcome stays.
   - A scheduled check that fails, when the one before it did not, is due again ten minutes later and read at the
     quarter-hourly task's next run, then every `UpdateCheckHours` as before; `GET /api/v1/update` gains
     `nextCheck`, when the scheduled check is next due.
   - An answer that ends before its headers is reported with who ended the connection, after how many bytes and
     seconds, the TLS handshake's share and how the read ended, where it said "The response carried no HTTP
     headers.", for every request of the server's HTTP client but the installer's and the bundle's downloads. A
     monitor that matches the old sentence needs the new one.
   - With `UpdateAutoDownload=1`, an installer and bundle already in the `Updates` folder are verified again where
     they are and not fetched again, and a copy that no longer passes is deleted and fetched; a service that cannot
     install an update itself (`NT SERVICE\hMailServer`, `HM7463`) no longer downloads the whole installer, 88 MB
     for 6.3.5, at every start ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
9. **On Linux the backup and event logs are UTF-8**, as the application and error logs are; they were UTF-16LE,
   which `grep` and `tail` read as every character followed by a NUL. The server converts `hmailserver_backup.log`
   and `hmailserver_events.log` in place the first time it opens each; the copies logrotate made before stay as they
   were. Windows keeps UTF-16LE.
10. **The Linux packages.** The `.deb` and the `.rpm` require `ca-certificates`, which apt and dnf install where it
    is missing. Every Linux install puts hMailServer's AGPL-3.0 text at
    `/usr/share/hmailserver/licenses/AGPL-3.0.txt` beside `QuickJS.txt`, and a copyright file at
    `/usr/share/doc/hmailserver/copyright` from the `.deb` or `/usr/share/licenses/hmailserver/copyright` from the
    `.rpm`. The `.rpm`'s `License` names every licence of what is compiled in, where it named the AGPL alone, and
    its `%changelog` is this release's entry, where it was CPack's placeholder of 2010. The update helper now runs
    `apt-get` quietly, so the journal no longer holds dpkg's progress as "[614B blob data]"; the helper already
    installed is the one that runs an update, so this shows from the update after 6.3.6.
11. **Text in the Control Panel can be selected and copied** (work item 48): on every page and dialogue by dragging,
    double-clicking, Ctrl+C or a right-click Copy, and every plain list copies its selected rows, one row per line.
    The words on the controls - a check box's or a button's caption, a drop-down's value, a tab's or a page's name,
    a label - are copied whole from their own right-click Copy or Ctrl+C; a drop-down's right-click menu, which
    offered Copy, Cut and Paste and could run none of them, now offers Copy. Live logs selects several lines,
    follows new lines only while its end is in view, and no longer splits a line the server is still writing into
    two rows. Keyboard navigation and screen-reader output are unchanged.
12. **Scripting off runs no script engine.** With scripting off the server no longer compiles the event script at
    every start, reinitialise and reload; switching scripting on compiles it first, and a script that does not
    compile is reported then rather than at each start while scripting is off.
13. **Install update under `NT SERVICE\hMailServer`, and what a script sees of it.** Where the service is not an
    administrator, the Control Panel's *Install update* installs (above), and:
    - `Status.InstallUpdate` returns false and leaves `UpdateState` at 3, with `UpdateLastError` saying who
      installs the update and where; it made the state 5, a failure, which took *Install update* away and
      offered the download again. `POST /api/v1/update/install` answers 409 with code
      `update_install_by_administrator` and the same sentence, and the state stays 3. On LocalSystem both are
      as they were.
    - `UpdateState` has two new values, 6 while the service prepares an administrator's install and 7 when it is
      ready, and COM `Status` six new members, `UpdateNeedsAdministrator`, `UpdateInstallAdvice`,
      `PrepareUpdateForAdministrator`, `UpdatePreparation`, `BeginUpdateForAdministrator` and
      `CancelUpdateForAdministrator` (dispids 20 to 25; nothing existing moves). The three that act answer only
      the server's administrator, and only from this computer. `GET /api/v1/update` adds `installedBy`
      (`service`, `administrator` or `elsewhere`), `installAdvice`, `serviceAccount`, `serverName` and
      `preparation`, and the Deck's Update page shows the sentence where its Install button stood.
    - The checklist's `update_apply_by_administrator` ends "installed by an administrator with Install update in
      the Control Panel on this server", and `HM7463` says that an administrator installs it with Install update in
      the Control Panel on this server, where they said an administrator installs them.
      `HM7465` to `HM7469` are new, and only an administrator's install raises them; its outcome is an
      application-log line, `HM5480` when it did not succeed, and an audit event of object type `update` under
      the Windows account that approved it, interface `Windows`
      ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md)).
    - The program folder gains `AdminUpdate`, made by the first administrator's install, holding each install's
      copies, log and outcome, which only administrators can change; anyone may read the folder's own
      permissions, which the Control Panel checks before every later install's prompt. The uninstaller removes it.
      `hMailServer.exe` loads the libraries it links from `Bin` and System32 only.
14. **Smaller things a script, a monitor or a log reader may notice.**
    - `GET /api/v1/update` also answers `installKind`, `liveUpdate`, `updateCommand` and `updateAdvice`, which the
      Control Deck shows ([LiveUpdateKinds.md](hmailserver/docs/LiveUpdateKinds.md)).
    - The REST API writes to the application log which method and path it answered 500 after an exception, and the
      crash oracle's record adds, after the fields it always had, what an access violation read, wrote or executed,
      the image and offset of the fault and up to twelve frames of the faulting thread.
    - Putting a switch back from the audit trail (`POST /api/v1/audit/{id}/revert`, `Utilities.RevertAuditedChange`,
      *Put back*) sets the switch; on 6.3.4 and 6.3.5 it wrote the setting's text column, which the server does not
      read, and changed nothing.
    - Over COM, `Recipients.Item` and `DiagnosticResults.Item` refuse a negative index, a look-up that finds nothing
      no longer leaves an object behind, and a date made from a NaN reads as no date.
    - `HM4229` reports the same codes in new words, and `HM7277` names `LegacyBlowfish::EncryptToString` as its
      source.
    - A TLS scanner sees three things change (Operations, *every TLS context switched on every OpenSSL option from
      bit 32 up*): the Windows server's outgoing TLS 1.3 handshakes no longer carry an Encrypted Client Hello GREASE
      extension; TLS 1.2 handshakes in both directions offer only the uncompressed elliptic-curve point format, on
      Windows and wherever the Linux server's OpenSSL is 3.6 or later; and on Linux with OpenSSL 3.2 or later the
      MTA-STS policy fetch and the OAuth2 token request no longer offer certificate compression.
    - The error log says six failures it did not (Operations, *six failures that stopped a pass or a step*), two of
      them with new codes, `HM7554` and `HM7555`.
    - A device's count of sign-ins (`sign_ins`) stops at 2,147,483,647, the most its column holds; the next sign-in
      would have made it negative.
    - A file sent as a link whose declared type is not on the list under Security goes out as
      `application/octet-stream`, which a browser saves under the file's own name, and its record's `type` in
      `/api/v1/me/files` says so, for a link recorded by an earlier version too; one declared `text/plain` with a
      charset, which went out as `application/octet-stream`, goes out as `text/plain`. A contact's photo whose card
      declares a type that is not one of the list's pictures goes out as `application/octet-stream`, and the webmail
      no longer shows a photo in SVG.
15. **Event scripts on Linux run in QuickJS 2026-06-04**, Fabrice Bellard's release, in place of 2025-04-26. It fixes
    memory-safety flaws in the engine that a script can reach: CVE-2025-62494 (a type confusion when a string is
    added to a value whose conversion changes it, High), CVE-2025-62493 (a BigInt written as a string, Medium) and
    CVE-2025-62492 (`indexOf` and `lastIndexOf` of a typed array with a tiny negative start, Medium). It fixes two
    more, both High, that a script could not reach in the server, because each needs more memory than the 64 MiB
    ceiling allows it: CVE-2025-62495 (an integer overflow compiling a regular expression of over 2 GB) and
    CVE-2025-62496 (a BigInt made from a string of some 67 million digits or more). Each needs the script to do the
    thing; message text a script only reads and compares does not. Two advisories a scanner may list against QuickJS
    do not apply: CVE-2025-12745 (`ArrayBuffer.prototype.slice` and a buffer resized under it) is in the resizable
    ArrayBuffers that 2026-06-04 adds, and fixed there, and 2025-04-26 had none; CVE-2026-88378 (rated Critical,
    published on 24 September 2026 and fixed in no release yet) is in the reader of compiled bytecode, which the
    server never calls, and the VEX document says so. The release adds newer language features -
    iterator helpers, the Set methods, RegExp's `v` flag, modifiers and `RegExp.escape`, Float16Array, `Promise.try`
    and `Error.isError` among them - and runs scripts faster (42% on bench-v8 over the release before it, by its
    author's measure). The objects the server gives a script, the time limit and the memory ceiling are unchanged.
    Windows' VBScript and JScript are not affected.
16. **Three more web requests go through `HttpProxy`, the MTA-STS policy fetch reaches only public addresses, and on
    Windows the listeners keep their ports to themselves.** The OAuth2 token request (`OutboundOAuth2TokenUrl`), the
    MTA-STS policy fetch and the ACME client now use the server's one HTTPS client (Operations, *on Linux, some of
    the server's network waits had no timeout*), so when `HttpProxy` is set they go through it, as the update check,
    the JWKS fetch and the server's other web requests do; they went direct before. The policy fetch, whose host the
    recipient's domain names, is made only to a public address, judged by the addresses `mta-sts.<domain>` resolves
    to: a policy host at a loopback, private, link-local or otherwise reserved address is not asked, and the domain
    is treated as one with no policy, as RFC 8461 treats a policy host that cannot be reached. Through `HttpProxy`
    the proxy is asked for a tunnel to one of those addresses and not to the name, so that what it reaches is what
    was judged; a proxy that allows a tunnel only to a name refuses that, with the same outcome. The policy host is
    taken as a host name and nothing more, so a domain cannot name a port or a path for it. With it a port in
    `OutboundOAuth2TokenUrl` is used, where it was taken for part of the host name and the request failed; an MTA-STS
    policy answer longer than 64 KB is no policy, where what had been read by then was parsed; and an ACME request
    names itself `hMailServer`, not `hMailServer-ACME`, and accepts any type of answer. The token request and the
    policy fetch also take the server's TLS settings - the protocol versions, the cipher lists and the key-exchange
    groups, with TLS 1.2 still the floor - as the ACME client and the other web requests already did, and on Windows
    too each of the three is given up once its deadline has passed for the whole request (15, 20 and 30 seconds),
    where there each read had that long. On Windows the metrics, ManageSieve and ACME http-01 listeners and the two
    web listeners (the REST API with the webmail it serves, and web services) no longer set `SO_REUSEADDR`, as the
    SMTP, POP3 and IMAP listeners have not for years: given a port that another program listens on with that option,
    such a listener now fails to bind and says so in the application log, where it bound the port and reported itself
    started while the connections went to the other program. Linux keeps the option, which there only lets a listener
    start again while its last connections linger. On Windows the SMTP, POP3 and IMAP listeners, ManageSieve, the
    metrics listener, the two web listeners and the SNMP agent also claim their ports with `SO_EXCLUSIVEADDRUSE`: no
    other program can then bind one address of a port such a listener holds on every address and take the connections
    made to that address, which Windows otherwise allows a program of the same account; and a listener that binds
    every address of a port another program already holds on one address now fails to bind and says so, where it
    started and served the other addresses. A listener started again on its own address - a service restart, a
    reinitialise, a drain that is left while sessions go on - binds as before, whatever its last connections are
    doing (measured on Windows 11). It is left off in two places: a port number listed twice among the TCP/IP ports
    in one address family (on 0.0.0.0 and again on 127.0.0.1, say), both of which still bind, and the ACME http-01
    responder, which still takes every address of its port that the REST listener or another web server does not
    hold.
17. **A calendar message delivered by mail changes a stored event only from a sender the server verified.** A
    `CANCEL` is applied only from the organiser the stored event names, an updated `REQUEST` replaces the stored
    copy only from that organiser, and a `REPLY` sets only the answer of an attendee the event already lists who
    sent it - or whose delegate did, kept as `SENT-BY` in the organiser's copy. A sender at this server's own
    domains is verified by a signed-in session (SMTP `AUTH`, the webmail, a room answering by itself) as that
    account or an address it may send as; a sender from outside by a DMARC pass for exactly the domain of a `From:`
    that is the sender's address, written as that one mailbox - a `From:` that another mail program could read as
    another address, with a second `<address>`, text after the `>` or an address in a comment or in the display
    name, verifies nobody. With DMARC off, from an address range spam protection skips, through the send API -
    whatever address its key may send as - or from an external account, nothing is verified. A `REPLY` from the
    account an alias the event lists points at is set on that alias; a `REPLY` adds nobody, to the event or to an
    occurrence the organiser left the guest out of, but a member of a distribution list of this server's that the
    event lists, answering themselves, is added beside the list with `MEMBER` naming it. A new invitation is filed
    as before, marked `X-HMAILSERVER-IMIP-UNVERIFIED` when its organiser was not verified, and such a copy does not
    hold off the organiser's own invitation by its `SEQUENCE`; anything else is delivered as an ordinary message,
    with a line in the application log, and the webmail's card for it says that the calendar still has the event as
    it was instead of saying it changed ([Calendar.md](hmailserver/docs/Calendar.md), "Who may change a stored
    event"). A room or a piece of equipment believes an organiser from outside only when the DMARC pass is for a
    `From:` that is the organiser's address, and no organiser for a message sent through the send API; its own
    answers are recorded as its submission, so that an organiser's calendar on this server files them
    ([RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md), "Who is asking").
18. **A cancellation of occurrences takes those occurrences out of a guest's calendar and leaves the rest of the
    series.** A `CANCEL` whose every event carries a `RECURRENCE-ID` (RFC 5546 section 3.2.5), from the organiser
    the stored event names, takes those occurrences out of the guest's copy: each one's override is dropped and an
    `EXDATE` written on the series, and from an occurrence named with `RANGE=THISANDFUTURE` on the series ends, its
    later overrides and `RDATE`s dropped and its `RRULE` given an `UNTIL` the moment before it in place of any
    `COUNT`. The copy is marked cancelled only when those occurrences are everything it holds, as for a `CANCEL` of
    the whole event. In 6.3.4 and 6.3.5 such a `CANCEL` marked the guest's whole series cancelled. The webmail's
    card for the message says the event is no longer in the calendar once its occurrences are out, and its "Remove
    from calendar" - `POST /api/v1/me/messages/{id}/invitation/reply` with `remove`, which then answers
    `occurrences: true` - takes out those occurrences alone, where it removed the whole series
    ([Calendar.md](hmailserver/docs/Calendar.md), "Filed on delivery"). A room or a piece of equipment took
    occurrences out of a booking already.
19. **The answer from an invitation's card replaces a copy of its UID that nobody verified, whatever organiser that
    copy names.** A copy filed from an invitation whose organiser the server could not verify - marked, item 17 -
    that names another organiser than the invitation no longer makes the reader's Accept, Maybe or Decline on the
    invitation's card fail with `422`: the answer replaces it with the invitation's event, marked as the card's
    answers mark what they write. A copy planted under the UID of an invitation before it arrived, naming somebody
    else, so keeps the invitation out of the calendar only until the reader answers it. Delivery still replaces no
    copy that names another organiser, verified or not, since a sender verified as the organiser it names is not
    the owner of the UID; and a copy the server did not mark - the organiser's own, verified, one the reader or a
    phone put in, and every copy filed before 6.3.6 - still refuses an answer that names another organiser
    ([Calendar.md](hmailserver/docs/Calendar.md), "Who may change a stored event").
20. **Five more web requests go through `HttpProxy`.** A sender's brand logo, a correspondent's OpenPGP key from
    their domain's Web Key Directory, a Web Push notification, a one-click unsubscribe and the post of an inbound
    webhook route held to public addresses - one that anybody but the server's administrator made - go through
    `HttpProxy` when it is set, where until 6.3.6 they went directly whatever it said, so that a server that
    reaches the web only through its proxy could make none of them. Each is still made only to a public address,
    judged by the addresses its host's name resolves to on this server, and the proxy is asked for a tunnel to one
    of those addresses and never to the name, as for the MTA-STS policy fetch (item 16): a proxy that allows a
    tunnel only to a name refuses it, and the request fails as one with no way out. A route the server's
    administrator made, which may reach this server's own network, still posts directly.
21. **The sender lists' URL entries read a large message only as far as the anti-spam size limit.** A message
    larger than the limit past which the SURBL test and the other tests after `DATA` do not look at a message
    (`maximum_message_size_kb` of `/api/v1/settings/antispam`, COM `AntiSpam.MaximumMessageSize`, 1024 KB unless
    changed; 0 is the MIME parser's 80 MB) is judged by the links in the first part of its text and of its HTML,
    as far as that limit into each, where every character up to the parser's 80 MB was read - some 8 seconds of a
    delivery thread for a 20 MB message. A block or allow entry for a link after the limit no longer decides such
    a message, and a link the limit cuts through is left out rather than read as the part of its host before the
    cut ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
22. **An external account's *days to keep messages* means one thing for POP3 and IMAP, and the upgrade starts no
    account deleting remote mail it was not deleting.** The setting has always meant, to the POP3 fetcher, the COM API
    and the `OnExternalAccountDownload` event: -1 deletes each message on the remote server as soon as it has been
    collected, 0 leaves every message there, and a number of days N deletes a message N days after it was collected.
    From 6.2.25 to 6.3.5 the IMAP fetcher read 0 as "delete" and -1 as "keep"; the Control Panel (from 6.2.0), the
    Deck, the webmail's *Other accounts* and the REST API described that reversed meaning for both kinds of account;
    and the webmail sent -1 for *Leave the mail there*, its default. Who was affected:
    - an **IMAP** external account at 0 - the default of COM, the Control Panel and the REST API - had its remote
      INBOX emptied as it was collected, and every mirrored folder when it mirrors folders, where the setting's own
      meaning is to leave everything; one at -1, the webmail's *Leave the mail there*, kept what -1 deletes;
    - a **POP3** external account added, moved or changed in the webmail (6.3.4 and 6.3.5) with *Leave the mail
      there*, or given -1 through the REST API or the Deck, which called -1 "keeps every message", had each message
      deleted on the remote server once collected; one given *Delete it there* kept everything;
    - an event script that answered 3 ("never delete") for a message of an IMAP account had it deleted, and one that
      answered 1 ("delete now") had it kept.

    A message deleted on a remote server had been collected first and is in the local mailbox; the upgrade cannot put
    it back on the remote server. Both fetchers now ask one rule (`FetchAccount.h`), and the upgrade to schema 6106
    translates what is stored so that every account goes on doing what it did or stops deleting, and none starts: an
    IMAP account's 0 becomes -1 and its negative number 0; a POP3 account made or moved by somebody other than a
    server administrator - in the webmail, by a reader, by a key restricted to named domains or by a domain
    administrator - has a negative number made 0, so that it stops deleting what that person asked to leave; every
    other value is as it was. A backup made before 6.3.6 is restored through the same translation, and one made by
    6.3.6 as it is. What the upgrade does not change is what an event script answers: an `OnExternalAccountDownload`
    script that answers 1 (*delete now*) for a message of an IMAP account had it kept, and from 6.3.6 has it deleted,
    as the event has always said - look at such a script before upgrading. **After upgrading, look at your IMAP
    external accounts:** one that was emptying its remote INBOX against your intention now shows -1, *delete after
    download*, and goes on doing it until you set it to 0. **Look at your POP3 external accounts at -1 too:** the
    upgrade cannot tell one a server administrator set to -1 through the REST API or the Deck to keep its mail, or one
    a reader set to *Leave the mail there* in the webmail on an account a server administrator made, from one set to
    -1 through COM or the old Administrator to delete it, so it goes on deleting until you set it to 0; one a domain
    administrator made over COM with -1 to delete it, as COM has always meant, now keeps its mail until it is set to
    -1 again; and one set to 0 in the Control Panel to delete after download has kept everything, and goes on keeping
    it until you set it to -1. **A program or script that writes the value** - over COM, the REST API or the Deck -
    now has -1 delete and 0 keep for an IMAP account too; one written to the IMAP fetcher's earlier meaning is to be
    changed before it next writes it. A webmail or Deck page loaded before the upgrade says the earlier meaning until
    it is reloaded, and the webmail's sends -1 for *Leave the mail there*: a change such a page sends with a negative
    number is refused with 409, *reload the page*, and nothing is saved, since the 6.3.6 pages say what they mean by
    the number (`days_to_keep_meaning`) and a browser session that does not is taken to be one of them; an API key, a
    password and COM are not asked. The Control Panel (*Days to keep on server
    (0 = never delete, -1 = delete after download)*), the Deck, the webmail, the COM help text and the REST API's
    description now all say the same thing; the webmail shows an account's number of days that its list does not offer
    as that number, where it showed and saved back 30; and an account the webmail imports with a sign-in leaves the
    mail on the other server unless asked otherwise ([Migration.md](hmailserver/docs/Migration.md), step 3). Tests:
    `ExternalAccounts/RemoteRetention` (both fetchers at -1 and 0, and the event's 1 and 3 on IMAP and POP3, each IMAP
    case failing against 6.3.5's fetcher), `FetchAccountTester` (the rule, the 6106 translation and a backup from
    before and after it), `BackupRestore`, the 6106 step's rows on PostgreSQL and MariaDB in
    `build/db-upgrade-gate.sh`, and the webmail's and the Deck's script tests.
23. **The upgrade to schema 6107 finishes the greylisting key, and a greylisting race is never a pass** (work item
    [#50](https://gitlab.com/hmailserver/hmailserver/-/work_items/50), the follow-up to GitHub discussion 281). Schema
    6072 left the triplets written before it without a key, to be found by their first 200 characters until they were
    matched once or expired - a second query for every new triplet for as long as one remained. The upgrade now gives
    them their key in the server, since no database can compute it, with the function new rows are written with;
    deletes the rows in which an address had been cut to the old columns' 200 characters, since such a row may stand
    for several senders and its key would match nobody; and leaves a triplet one row, the one that passed most. Then
    both key columns are `NOT NULL` and the key is unique - on MySQL and MariaDB over a stored generated column,
    `glipaddress2key`, and on PostgreSQL as two partial indexes, because both count one NULL `glipaddress2`, an IPv4
    triplet's, as different from another - and the lookup asks for the key and nothing else. A sender whose row was
    deleted is greylisted once more; the upgrade's output says how many rows were keyed and deleted, and why (with
    DBUpdater and `UpgradeOnStart`, in the application log when application logging is on). On a large
    `hm_greylisting_triplets` the keying runs before the step's transaction, each statement committed as it goes, so
    an upgrade stopped part-way keeps what it did and the next run continues from there. And when two connections
    present the same new triplet at once, the unique key refuses the second insert, which is read back and answered as
    the triplet that is there: deferred and counted while it is pending, let through once it has passed - never let
    through for the refusal ([Greylisting.md](hmailserver/docs/Greylisting.md), "The triplet key").
24. **The Linux live update's helper takes the unit and the AppImage from its own command line.**
    `hmailserver-update` stops `hmailserver.service`, or the unit its `--unit` option names, and refuses a request
    whose `unit=` line names another. It takes an AppImage request only when its `--image` option names the image,
    which is then the program that verifies the new one, and refuses a request whose `image=` line names another
    file, and a `.deb` or `.rpm` request where `--image` is given. After the stop it checks that the unit is
    inactive and that no process runs as the owner of the Updates directory or of a directory above it, and
    otherwise starts the unit again and does nothing else. It verifies, reads and installs copies of the package and
    the rollback image, made once the service has stopped and only of plain files still inside the Updates
    directory, under `/var/tmp` (an AppImage's beside the image), which needs room there for both. A machine whose
    package database, or image, does not say which version is installed is no longer updated "over whatever is
    here": the outcome is `failed`. A server installed from the `.deb` or the `.rpm` under the packaged unit needs
    nothing. One under a unit of another name, through the path unit, needs `--unit` in a drop-in for
    `hmailserver-update.service`; an AppImage under the path unit needs `--image` there, and 6.3.6's script in
    `/usr/lib/hmailserver/`, which the AppImage's swap does not replace; an AppImage that runs as root under a unit
    of its own passes both itself. The update to 6.3.6 is applied by the helper of the version it replaces, and
    6.3.6's own applies the next one
    ([packaging/README.md](hmailserver/source/Server/platform/packaging/README.md), An AppImage).
25. **Port 25 still takes AUTH, and the setup checklist says so** (GitLab work item #54). The schema step 6107 to
    6108 gives every TCP/IP port an SMTP authentication and writes On into every row, so no listener of an upgraded
    server answers `AUTH` differently, and nobody is banned on the morning of the upgrade: clients that still sign in
    on 25 go on doing so. The setup checklist has a thirteenth item, *No password on port 25*
    (`delivery_port_auth`), action needed with no one-click fix while an SMTP listener on 25 is On - setting it to
    Honeypot or Off is what arms the ban, so move such clients to 587 first
    ([SmtpAuthOnDeliveryPorts.md](hmailserver/docs/SmtpAuthOnDeliveryPorts.md)). `GET /api/v1/ports` gives each port
    `smtp_authentication`, and a `PUT` of a port that leaves it out writes `on`, as it writes the default of every field
    it leaves out. `TCPIPPorts.SetDefault` - *Restore defaults* - writes both SMTP ports On, and says so. A backup
    taken before 6.3.6 restores every port On. The type library adds `TCPIPPort.SMTPAuthentication` (dispid 12). The
    error HM6099, a ban's range that could not be saved, no longer says the address reached the failed-logon limit,
    which a spam trap's or a null sender's ban never did.
26. **COM: a message or fetch account a program creates itself answers with the credential of an `Application` the
    program signed in on, and is refused outside an event script when there is none; a signed-in account queues mail
    only as itself.** Most scripts need no change: one that creates `hMailServer.Application`, calls `Authenticate` and
    then makes its message with `CreateObject("hMailServer.Message")` works as before, as long as it keeps the
    `Application` until the message is saved (a .NET program with `GC.KeepAlive`). With an administrator password set,
    `Message.Save` and `Copy`, `FetchAccount.AccountID` and `Save`, `Account.Save` and the other writes to an account's
    rules, folders and folder permissions refuse an object that carries no credential. A `hMailServer.Message` or
    `hMailServer.FetchAccount` a program creates with `CreateObject` or `new` carries none of its own: it takes the
    credential of an `Application` the same program holds and has signed in on - the most privileged, where it holds
    several - and has none in a program that never signed in, or once that `Application` is released or a later
    `Authenticate` on it fails. The objects an `Application` handed out before a later `Authenticate` on it failed are
    refused too. `Attachments.Add`, which reads a file from the server's disk, and `Attachment.SaveAs`, which writes
    one, answer the server administrator alone: a program signed in as a domain administrator or an account is refused
    them. Mail that a program signed in as an account queues, or saves again while it waits in the queue, claims to be
    from only an address the account may send as - its own, an alias of it, one granted to it or verified
    ([SendAs.md](hmailserver/docs/SendAs.md)) - in its envelope and in its `From` and `Sender` fields, with no line
    break inside a value of its header, and a domain administrator's from an address of a domain this server holds and
    it administers, other than an account there with authority it does not hold; to send as anybody, a program signs in
    as the server administrator, as it must to attach a file. The server's event scripts, which create these objects
    without signing in, work as before, and a server with no administrator password and no second factor answers every
    caller as it did. The new `Application.CreateMessage()` gives a program a message that carries the credential of
    the `Application` it is called on; a .NET program built against an earlier type library reaches it once rebuilt
    against 6.3.6's, or late-bound ([Roles.md](hmailserver/docs/Roles.md)).
27. **The packaging Compose file's database is PostgreSQL 18.** `docker-compose.yml` beside the Dockerfile runs
    `postgres:18` with its `hm-db` volume at `/var/lib/postgresql`, where it ran `postgres:16` with the volume at
    `/var/lib/postgresql/data`. PostgreSQL 18 does not open 16's data: a deployment made from an earlier copy of the
    file that takes this one finds `db` stopping at each start with "Error: in 18+, these Docker images are
    configured to store database data in a format which is compatible with "pg_ctlcluster"" and the server not
    started, 16's data unchanged, until the database is moved once - a dump taken on 16 and loaded into 18, four steps with a way
    back ([ContainerImage.md](hmailserver/docs/ContainerImage.md), "PostgreSQL 16 to 18, in the Compose file").
    A deployment that keeps its own copy of the file keeps PostgreSQL 16, on which 6.3.6 runs as before.
28. **DANE-TA records authenticate a recipient's MX host, and a validated set of none this server can use requires
    TLS.** A DNSSEC-validated TLSA record of usage DANE-TA (2) - selector 0 or 1, matching type 0, 1 or 2 - is now
    used, through OpenSSL's own DANE support: the host's certificate must chain to the anchor the record names (which
    the host sends in its chain when the record holds a digest), name the MX host and be in date. A DANE-EE (3) record
    is matched as before, against the certificate's key or the certificate alone. A delivery to a host that publishes
    only DANE-TA records, and whose certificate does not meet them, is now deferred where until 6.3.6 it went ahead as
    if no record were published; and such a host that does not offer `STARTTLS` is deferred, as a host with DANE-EE
    records now is too (item 31). A validated set none of whose records is usable here - PKIX-TA (0) and PKIX-EE (1),
    which SMTP does not use, and records of values or lengths no record has - requires TLS to that host without
    judging its certificate: a host that does not offer `STARTTLS`, or whose handshake fails, is deferred, never
    delivered to in the clear, and TLS reports (RFC 8460) count the host under the policy type `tlsa`. An MTA-STS
    policy in enforce mode, a remote-domain policy or a rule that asks for a certificate that verifies still has it
    verified, and so does this server's own outbound security, or a route's, at SSL or STARTTLS (required) with
    `VerifyRemoteSslCertificate` on: there the certificate of a host with only unusable records must verify, and a
    delivery to one whose certificate does not is deferred. Error 5510 is logged when a connection's records could not
    be given to OpenSSL; that delivery is deferred rather than made without them.
29. **A ManageSieve connection is held to deadlines.** A command line must arrive whole within 30 seconds of the
    server waiting for it, which is also how long a session may sit idle between commands, as before; the TLS
    handshake after `STARTTLS` has 30 seconds; a literal (a script, a SASL response) and the writing of each response
    have 300 seconds; and a connection must complete `AUTHENTICATE` within 60 seconds of being accepted, or of
    `UNAUTHENTICATE`. A connection that runs past one is closed, with a debug log line naming the client. The figures
    are constants, far above what a client needs, and there is no setting for them.
30. **The URI blocklists are asked about every domain a message links to, and a message that links to too many is
    scored for it.** The SURBL test reads every link of the text part and of the HTML part of a message within the
    anti-spam size limit, takes the hosts of one registrable domain (by the Public Suffix List) as one domain, and asks
    each active list about every registrable domain and address the links name - and, for a host under a shared-hosting
    suffix of the list's private section, the domain registered under its ICANN section, `github.io` for
    `tenant.github.io` - up to 200 for a message, the HTML part's before those only the text part names; then up to two
    host names under each domain, drawn at random when a domain has more, until 256 names in all, the domains taken in
    a random order when their host names do not all fit. Each list is asked up to sixteen names at a time, where it was
    one - on the thread testing the message and on up to fifteen of 64 threads the whole server has for the URI
    lists, which the messages being tested at once take in turn - and still stops beginning lookups after 10 seconds. A
    message whose links name more than 200 domains has 200 of them asked about - the HTML part's first, drawn at random
    from the part that does not fit - and scores the new setting `SurblTooManyDomainsScore` once, however many lists
    are active: 5 by default, the stock mark threshold, so such a message is marked as spam at the stock thresholds; 0
    scores nothing, and nothing is scored while no URI list is active. It is on the Control Panel's Anti-spam page
    (General), in `PUT /api/v1/settings/antispam` as `surbl_too_many_domains_score`, in the Deck, and over COM as
    `AntiSpam.SurblTooManyDomainsScore`; a database gets its row at the first start, with no schema step. A message
    with many links makes up to 256 DNS queries per active list, where it was 16, at most sixteen of them at once;
    however many messages are tested at once, their URI lists' lookups take at most 64 threads beyond the ones testing
    them.
31. **A host that the destination's TLSA records or MTA-STS policy require TLS of, and that offers no `STARTTLS`, is
    deferred; and TLS reports count a session by what became of its TLS.** Such a host was refused with the permanent
    500 5.7.0 "Server does not support STARTTLS.", and the message bounced at the first attempt; RFC 7672 section 2.2
    and RFC 8461 section 5 ask for the delivery to be retried, as it already was for TLSA records none of which is
    usable. A host that refuses `EHLO`, and so offers no `STARTTLS` either, was refused with the permanent 500 5.5.1
    "Server does not support EHLO command." - there, also where a remote domain policy or a rule required TLS. Each is
    now 450 4.7.0 with a sentence naming the records, the policy or the rule, and the message waits for the retries; a
    certificate that does not match the records, or does not verify for MTA-STS, is deferred with the same sentence
    where it was "Remote server closed connection". A route's own `STARTTLS (required)`, and the server's outbound
    connection security toward a destination that publishes neither, still refuse permanently. In the TLS-RPT store
    (RFC 8460) a session counted as successful whenever its connection security asked for TLS - a host that offered no
    `STARTTLS`, one whose certificate did not match its TLSA records or verify for MTA-STS, a connection that never
    got that far - so the reports a domain receives hid the failures they are published to show. A session is now a
    success only when its handshake completed, a failure of type `starttls-not-supported` when there was no
    `STARTTLS`, it was refused or `EHLO` was, a failure of type `validation-failure` when the handshake or the
    certificate's check failed, and not counted when it ended before TLS was asked for; each is a debug log line. This
    holds for every destination reported to, opportunistic deliveries included, which therefore report the hosts that
    offered no `STARTTLS`.
32. **An account, or the administrator, holds at most fifty REST browser sessions, and a full table takes from whoever
    holds the most.** A fifty-first sign-in of the same account - webmail tabs, bearer sessions of a second account,
    a script that signs in and never signs out - ends that account's least recently used session, where it used to
    end the least recently used session on the server; a support session counts against the credential that opened
    it. A table of a thousand that is full gives up a session of the holder with the most - among holders tied, one
    of the domain holding the most - and opening a support session on a full table makes room as a sign-in does where
    it was answered 503. The administrator's sessions are held by the administrator's own fifty alone: they do not
    count towards the thousand and nobody else's sign-in takes one
    ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md), How many sessions, and who makes room).
33. **A pending single sign-on or passkey sign-in is carried by the browser, sealed, and is good once.** An OpenID
    Connect start, a SAML start and a passkey sign-in's challenge leave nothing on the server: the `hmoidc` cookie
    holds the sign-in's state, PKCE verifier, nonce, page and end, `hmsaml` its request's ID, and `hmpasskey` its
    challenge and end, each sealed with AES-256-GCM under a key the server process makes for itself, holds in memory
    and makes again whenever the REST listener stops; a SAML request's ID, which comes back through the identity
    provider, carries its page and its end under a tag of that key, and the RelayState is the same ID. Where 6.3.5
    held tables of 200, 200 and 1,000 that dropped their oldest whoever held them, no number of starts asked for can
    now push anybody's sign-in out. A sign-in is spent once it finishes - the issuer's `id_token`, a signed SAML
    Response or a passkey's assertion verified - and a second answer to it is refused. A sign-in started before the
    listener stopped is not finished after it ("this sign-in was not started here"), as before; two servers behind
    one name each seal with their own key, as each held its own table. `X-Forwarded-For` is not read for any of this
    ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md), What a stranger can make the server
    hold). A passkey registration's challenge counts against the credential that asked for it, fifty a credential,
    where the table of a thousand dropped its oldest.
34. **A `[Settings]` key's audit row is put back by the administrator password alone, and the breach check's test
    reaches public addresses only for anybody else.** `POST /api/v1/audit/{id}/revert` answers 403 to an account
    holding a role for a row of a `[Settings]` key, with the sentence `/api/v1/settings/ini` refuses it with; a
    database setting's row is the role's as before. `POST /api/v1/settings/breach-check/test` asked by an API key or
    an account holding a role tests an endpoint at a public address only, over https and through `HttpProxy` when
    one is set, and answers `reachable: false` with the reason for one on the server's own network; the
    administrator password, the Control Panel and COM test a mirror there as before
    ([Roles.md](hmailserver/docs/Roles.md), What manage_settings reaches).
35. **A wrong administrator password over REST costs a growing delay, which an IPv6 `/64` shares and a passkey's
    registration is held to.** After a wrong administrator password the next from the same source is looked at only
    once a delay has passed, growing to ten seconds at most and cleared by the right password, on every route that
    takes the password and from the loopback address too (Security, above); a guess inside it is answered 429 with
    `Retry-After` before it is looked at. Wrong administrator passwords from any address of one IPv6 `/64` share one
    delay, and the password asked again when the administrator registers a passkey from a session is held to the
    delay too. When the auto-ban's limit is reached at the REST
    listener, an IPv6 address's whole `/64` is refused there for the five minutes, and the application log names the
    `/64`; the auto-ban's IP range stays the one address. Everybody on one `/64` - an office network, conference Wi-Fi,
    devices tethered to one phone - therefore shares the delay and the refusal, as everybody behind one IPv4 NAT
    address always has. An address that carries an IPv4 client's own - `::ffff:a.b.c.d`, the NAT64 and SIIT
    well-known prefix `64:ff9b::/96`, and Teredo's `2001::/32` - is counted and refused as that IPv4 address instead
    ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md), The administrator password's delay).
36. **DANE: an MX or TLSA lookup that cannot tell defers the delivery, and the validated MX records choose the
    hosts.** With `DaneEnforcementEnabled` and `DnssecValidationEnabled` on, as they are by default, a recipient
    domain's MX lookup and its hosts' TLSA lookups are made with DNSSEC as before, and an answer without signatures,
    without records, or not at all - no answer, `SERVFAIL`, a reply that does not parse - is read as unsigned only
    when the resolver can prove the name lies below an unsigned delegation, which it now asks for: one more DS query
    per unsigned domain, remembered for five minutes, which a validating or forwarding resolver answers. Under a
    signed zone, or when nothing can be proven, the lookup is indeterminate (RFC 7672 section 2.1.1): an indeterminate
    MX lookup leaves every host of the domain unused, an indeterminate TLSA lookup that host, and a delivery with no
    host left is deferred with `4.7.0` and a reason naming the lookup - "could not be completed, ... The message has
    NOT been sent" - where until 6.3.6 it went ahead as to a domain without DANE. An empty TLSA or MX answer from a
    signed zone counts as no records only with its NSEC or NSEC3 proof (NODATA, or NXDOMAIN with the wildcard's; an
    NSEC3 Opt-Out span reads as unsigned, as RFC 5155 asks). A validated MX RRset decides the hosts: a host the plain
    lookup found and the validated records do not name is not used, and the domain's own name is the host of a signed
    domain proven to have no MX records. A zone whose validated DS records name only algorithms or digests the
    resolver does not verify is unsigned (RFC 4035 section 5.2), where it was bogus and its mail held. A domain under
    a top-level name the root proves it does not have (`company.local`) is outside the signed tree and unsigned too; a
    private name under a signed public domain, answered by an internal DNS server, is indeterminate - deliver to it
    through a route. **A server whose resolver cannot answer DNSSEC queries** - one that strips signatures, or a
    firewall that drops DNS answers longer than 512 octets - now defers its mail to every domain it cannot prove
    unsigned, which is most of them, and says so in each deferral, and in the application log once in ten minutes
    with the cause ("DNSSEC: the root zone's keys did not validate", or could not be fetched): make the resolver
    answer them (any current public or ISP resolver does), or set `DaneEnforcementEnabled` to 0, which leaves DANE
    off. The validating resolver asks the server in `DNSServer`, or else the system's first two IPv4 DNS servers, as
    it always has, and when the system lists no IPv4 DNS server, its first two IPv6 ones (passing over the site-local
    `fec0:0:0:ffff::1` to `::3` Windows lists for an adapter given none), which until 6.3.6 it never asked: a system
    whose DNS servers are IPv6 alone had none it could ask, which would have made it such a server too, and on Linux,
    where every DNS lookup of the server goes the same way, had no DNS at all. TXT lookups (SPF, DKIM, DMARC) are read
    as before. `Diagnostics.DnssecChainStatus` answers 3 for an indeterminate MX or TLSA lookup.
37. **ManageSieve serves its connections side by side, one at a time per address before signing in, and bounds a
    signed-in session.** Up to 16 connections are served at once, and up to 64 wait for a thread; before it has signed
    in an address - an IPv6 one by its `/64`, an IPv4 client of NAT64 or Teredo by its own IPv4 address - has one
    connection served at a time, its others wait their turn - four at most, a fifth refused with `BYE (TRYLATER)` -
    and a connection that runs out of time before signing in (a line, the TLS handshake or the 60 seconds before
    `AUTHENTICATE`) counts as a failed logon from its address for the auto-ban, which with the shipped
    `MaxInvalidLogonAttempts` of 3 bans an address after three - but not from this computer's loopback address
    (`127.0.0.0/8`, `::1`), a webmail beside the server, which three such timeouts would otherwise have banned from
    SMTP, IMAP and POP3 for everyone it serves; SMTP's ban for `AUTH` on a delivery port spares the loopback the same
    way, a wrong password from it still counts, and `AutoBanNeverBan` still spares any other address. A signed-in session ends with `BYE` after ten lines in
    a row that do nothing (`NOOP`, `CAPABILITY`, an empty or unknown command) or once it has waited two minutes in all
    for its commands, and an account has at most four sessions at once, a fifth sign-in answered `NO (TRYLATER)`.
    `UNAUTHENTICATE` takes the address's one turn back, and is answered `NO (TRYLATER)`, the session still signed in,
    while another connection from the address is signing in. A reinitialise or a stop ends each session at its next
    command with `BYE`, where a client that kept sending commands held it until that client stopped. A client that
    keeps a session open to edit in - which the 30 seconds between commands already cut - signs in again. The figures
    are constants.

38. **The webmail keeps a signer's S/MIME certificate only when its chain is trusted, under the From address, and a
    different one for the same address only when asked.** A signed message whose certificate the server does not trust
    no longer leaves the account a certificate to encrypt replies with; the message says it was not kept (unless it is
    one the account holds already, imported on the Security page), and a correspondent whose certificate is
    self-signed or from an authority this machine does not trust is imported on the Security page once it has been
    checked with them. A trusted certificate is kept under the message's From address, which it must name, where it
    was kept under the first address it listed. A different certificate for an address that has one is offered behind
    **Keep this certificate** with its fingerprint, where it was kept beside the old one at once: a correspondent's
    renewed certificate takes that one click on the first signed message after the renewal. Certificates kept before
    the upgrade stay; review them on the Security page ([SmimeAuthority.md](hmailserver/docs/SmimeAuthority.md)).
    Tests: the webmail's script test, eight checks that fail against 6.3.5's page; the five new texts are in all 23
    catalogues.
39. **A message's HTML in the webmail shows its embedded images until 24 MB of them are written.** The route
    `GET /api/v1/me/messages/{id}/html` writes at most 24 MB of `data:` URLs in all, every reference to an image
    counted, where it wrote one for every reference; a reference past that stays the `cid:` URL it was and shows no
    image. The 64 images, 4 MB each and 12 MB together, are as they were, so a message that showed each of its images
    once still shows them all. A `cid:` URL is matched whole, up to the quote, space, bracket, angle bracket,
    ampersand or backslash that ends it, where any text that began with `cid:` and a part's Content-ID was replaced,
    the rest of a longer id left behind ([Signatures.md](hmailserver/docs/Signatures.md), In the webmail). Tests:
    `RestApiInlineImageBudget`, both failing against 6.3.5's server.
40. **The webmail refuses an OpenPGP message that inflates past 32 MB.** Each compressed layer of a message the
    webmail opens is inflated to 32 MB at most, the size of message the server reads whole; a larger one is refused
    with *the message is larger than 32 MB once decompressed, more than this page opens; open it in a mail program*,
    where the tab inflated it whole ([OpenPGP.md](hmailserver/docs/OpenPGP.md)). The compressed data goes to the
    browser's decompressor 16 KB at a time, since Chromium's inflates all of what it is given before the page reads
    any. Test: `build/check-portal-openpgp.js`, four checks that fail against 6.3.5's module, one of them with a
    decompressor that inflates the way Chromium's does.
41. **What the auto-ban put below the server is lifted after a stop too.** A ban that ended while the service was
    stopped, or an `unban` a hook still owed when it stopped, stayed blocked below the server for good with the Linux
    hook's iptables variant or with `AutoBanCommand`: what they had been told lived in the server's memory only. At
    every start and reinitialise, before the first listener opens, the packaged Linux hook is now told `reset` before
    every ban in force is told to it again - wherever it is installed, whether `AutoBanFirewall` is on or off (with the
    switch off and no `CAP_NET_ADMIN` it finds nothing) - and again when `AutoBanFirewall` is switched off.
    `AutoBanCommand`'s account is kept in `autoban-command.ledger` in the temp folder, under the SHA-256 of the command
    line and never its text, and at a start the command is told `unban` for each address it was left holding that is not
    banned now; the ledger begins at the first start of 6.3.6, so what 6.3.5 left a command holding is not in it, while
    the packaged hook's `reset` takes away whatever it held. A command that starts the server short of its listeners -
    `--sample-domain`, `--apply-environment`, `--apply-profile` and the rest - does none of this, and no longer tells
    the firewall every ban in force again at its start either: run beside a running server, a `reset` would lift every
    ban below that server. A command replaced or cleared, while the service runs or while it was stopped, is never run
    again: the addresses it was told to ban and not to un-ban are named in the error log (`HM6523`), and the new command
    is told every ban in force, where it was told none of them. The hook's `iptables` calls wait for the xtables lock
    instead of failing while fail2ban holds it, and Windows' uninstaller removes the `hMailServer auto-ban` rules, which
    Windows keeps across restarts. A reconciliation that cannot read the IP ranges now changes nothing below the server:
    it took a failed read for no bans at all, told every hook to un-ban every address and removed every Windows rule,
    until a later pass put them back. A command of your own is still best made to let a block lapse after the
    `<minutes>` it is handed ([AutoBan.md](hmailserver/docs/AutoBan.md), Below the server across a stop and a start).

42. **An IP address in text is read the same way on Windows as on Linux.** Wherever the server reads an address from
    text - an IP range's bounds, an incoming relay, a listener's address, the bind address of the REST and
    web-services listeners and of the SNMP agent, `AutoBanNeverBan`, XCLIENT's `ADDR` and the PROXY protocol's
    addresses, a route's, a callout's or a URL's host, and COM's `Utilities.IsValidIPAddress` - an IPv4 address is
    four decimal parts and an IPv6 address is written as RFC 4291 writes one, with a zone after a `%` if it has one,
    and nothing else. On Windows these were read by the system's own parser, which also takes the forms inet_aton has
    taken since 4.2BSD and more: `192.168.1` as 192.168.0.1, `3232235777` as 192.168.1.1, `0x7f.1` as 127.0.0.1, a
    part with a leading zero as octal (`010.0.0.1` as 8.0.0.1, `192.168.001.010` as 192.168.1.8), an address with a
    port after it (`192.0.2.7:25`) and an IPv6 address in brackets; Linux took none of them. Such a value is now
    refused where it is entered, on Windows as on Linux: by the REST API; by COM, whose IP range, incoming relay and
    white-list addresses stored 0.0.0.0 for a text they could not read until 6.3.6 (Security, above); and so by the
    Control Panel's pages that write through COM, and by its API keys page for a source restriction, which it checked
    with .NET's reader, as liberal as Windows' own. An `AutoBanNeverBan` entry written that way is reported (HM6524)
    as an entry that protects nothing, where on Windows `192.168.1/24` used to protect 192.168.0.0/24 without a word.
    Ranges, relays, white-list entries and listeners already saved are stored as numbers and are not affected. A value
    kept as text is read the new way from the first start of 6.3.6: besides `AutoBanNeverBan`, an API key's source
    restriction so written is one the server cannot read, so the key is refused from every address; a REST,
    web-services or SNMP bind address so written leaves that listener stopped, which the application log says (the
    SNMP agent's, the ERROR log: HM7420); and a route's, the relayer's or an external account's host so written is no
    longer taken for an address. The refusal of HELO and sender names that are not names (GitLab issue 51) judged
    `192.0.2` "an IP address" on Windows and "numeric where its top-level domain should be" on Linux; both now give
    the second, and refuse it as before. `DNSServer` is read the same way by every lookup: the Windows resolver read
    it with `inet_addr`, which takes the older forms on every platform, while the DNSSEC resolver and the TLS policy's
    lookups read it with `inet_pton`, so `DNSServer=8.8.8` sent the one to 8.8.0.8 and the other to the system's
    servers; now the Windows resolver too reports it invalid (HM4401) and asks the system's servers. Measured on
    2 October 2026 against Windows 11's `WSAStringToAddressW` and glibc 2.43's `inet_pton`
    ([SmtpPolicy.md](hmailserver/docs/SmtpPolicy.md)).
43. **`hMailServer.exe`'s commands write nothing to the error log and say it all on their own output, and on Linux a
    store command run as root runs as the service's account.** Every command line but `RunAsService`, `/Debug` and
    `/Test` - `/Register`, `/Unregister`, `/RegisterTypeLib`,
    `--upgrade-database`, `--apply-new-install-defaults`, `--apply-profile`, the stored-secrets, store and object-store
    commands and the rest - leaves the error log, the Application log and the `OnError` event script alone and arms no
    crash oracle (Security, above); what it did and why it failed is on its standard output and standard error, where
    each already said it - `--upgrade-database` now into output its caller redirected, where a console the caller had
    took its lines. `/Register` now says why a registration failed there, where it said nothing and wrote the
    error log; Setup writes what `/Register`, `--apply-new-install-defaults`, `--apply-profile` and
    `--create-stored-secrets-key` say into its own log and its messages, which no longer send anybody to the error log
    for them. A script that read the error log after one of these commands reads its output instead:
    `--forget-sealed-secrets`' `HM7497` and a command's `HM7496` are said on its standard error, as a Linux command run
    as root says them. Every store command - `--measure-store` included - and every copy to or from an object store
    stops, exit 1, before it reads or changes anything when the data folder holds a junction, a mount point or a symbolic
    link that the administrators do not alone control, and names it. A link owned by `BUILTIN\Administrators`, `SYSTEM`
    or `TrustedInstaller` that no one else may change or remove - a junction an administrator made to keep the data, or a
    domain's folder, on another drive - is gone through, and the path it holds is held, every link on it asked about,
    and checked; a folder only the
    administrators may list, such as `System Volume Information` at a drive's root, is skipped rather than refused. So a
    data folder below a mount point or a junction an administrator made, or at a drive's root, works once the link is the
    administrators' alone: a link made in the data folder inherits the service's write rights, and one made at a drive's
    root those of `Users`, and is refused, naming who may change it, until it is given permissions only the
    administrators may change (`icacls <link> /inheritance:r /grant:r *S-1-5-32-544:(F) *S-1-5-18:(F) *S-1-5-32-545:(RX) /L`,
    and `/setowner *S-1-5-32-544 /L` where its owner is an administrator's own account; docs/ServiceAccount.md). A store command on
    Windows, or run as root on Linux, also stops, before it reads or changes a file, when a message row names a file
    outside the data folder - a full path elsewhere, which an old row may hold, or a part that is `..` - and names the
    message: a full path below where a junction of the data folder leads is in it, a legacy full path elsewhere is moved
    with the Data Directory Synchronizer, otherwise delete the message.
    On Linux a store command, an object-store copy, `--sample-domain`, a profile or `--apply-environment` run as root now
    runs as the service's account first: these work in the data and log folders, which that account (the unit's `User=`)
    owns and can plant a symbolic link in, so `sudo hmailserver --compress-store` becomes that account - the data
    directory's owner, with its groups - before opening anything, follows a planted link as that account and not as root,
    and leaves behind no file the service cannot open; over a data directory that cannot be examined, or whose way is
    not root's and its owner's alone (a folder above it that another account or group may write), it refuses (exit 2),
    naming why. Over a data directory that belongs to root - a service run as root, as a source build or a container
    without `User=` has it - it runs on as root, and says so, where the data directory and every folder under it, the
    log, temporary, event and database folders and every folder under them, every folder a link in them leads to, the
    configuration that names them and the ways to all of them are root's alone (each folder and the configuration
    root's and written by no other account, no folder's default access control list giving another account a write,
    each link root's), and then makes what it makes with at least every other account's write taken away by its
    umask; otherwise it refuses (exit 2), naming the first folder, file or link another account owns or may write - a
    log directory below Ubuntu's `/var/log`, which the `syslog` group may write, among them. Every other Linux
    command line run as root (`--set-setting`, the stored-secrets verbs,
    `--create-database`, `--upgrade-database` and the rest) writes nothing to the log directory - what it did and why
    it failed are on its own output, and a report that would have gone to the error log is not written, but for the
    stored-secrets code's `HM7496` and `HM7497`, which go to standard error - as the Windows commands do; and
    `--create-database` makes its `.new-installation` marker without following a link of that name.
44. **`/GrantServiceAccess` that cannot read the database ends in 7.** It names the certificate and key files, the
    backup folder and the PST import folder from the database, to say which the service would not reach once moved;
    when the account running it could not open the database - SQL Server with Windows authentication, where LocalSystem
    is a SQL Server administrator and that person is not - it said so in a note and ended in 0, and the installer,
    DBSetup and the Control Panel moved the service without knowing. It now says `NOT READ` and ends in **7** unless an
    administrator confirmed: with `--confirm`, as Setup in a window, DBSetup's wizard and the Control Panel run it, the
    question it asks names the database it could not read, and Yes moves the service as before. A silent move then
    waits, as it does for a folder nobody approved, and Setup says what to run; an upgrade of a service already moved
    goes on. A database not set up yet names nothing, and is no 7.
45. **The data, log, temp, database and event folders and `hMailServer.INI` are the administrators' and the service's
    alone.** Setup runs `hMailServer.exe /ProtectFolders` at every installation and upgrade of the server, before the
    database step and whatever account the service runs as: the program folder's own `Data`, `Logs`, `Temp`, `Database`
    and `Events` and `Bin\hMailServer.INI` stop inheriting what the folder above them hands down - `BUILTIN\Users`' Read,
    `ALL APPLICATION PACKAGES`, `CREATOR OWNER`, and on a data folder moved behind an administrator's junction the drive
    root's entries - and keep SYSTEM and the Administrators, with full control, the service's grant, every deny entry
    and every entry of their own; the service is given Read on `hMailServer.INI`. On a large message store the first
    run takes minutes, while Windows hands the change down; after that it finds them private and changes nothing. A
    folder the configuration names outside the program folder, a folder approved for the service, the backup and PST
    import folders, and a key file in `Data\Certificates`, `Data\DKIM` or `Data\ACME` with permissions of its own, are
    checked and not changed: a `CHECK` line in the setup log names who may read each and the `icacls` that closes it. A
    run that does not end in 0 does not fail the installation; setup says so. What an administrator who relied on the
    old permissions does: an account that is not an administrator - a script, a backup program or a monitoring agent
    that read the logs or the message store, or an administrator's own program not run as administrator - is given an
    entry of its own (`icacls "<folder>" /grant "<account>:(OI)(CI)RX"`), which later runs keep; Explorer's *Continue*
    on such a folder adds the administrator's own account the same way. The Control Panel not run as administrator
    says *Could not read the file* on its live log page and its message-store check, where it found no log folder;
    run it as administrator to see them. An editor that saves `hMailServer.INI` by replacing the file gives it
    `Bin`'s permissions again; run `hMailServer.exe /ProtectFolders` after it.
46. **A full restore keeps this installation's own self-signed certificate and key, and the server's own files at the
    data directory's root.** When the archive's `Certificates\self-signed-key.pem` is not this installation's byte for
    byte - an archive taken on another computer, or before this installation made its pair - the restore leaves this
    installation's pair as it is and says so in the backup log; up to 6.3.5 a service running as LocalSystem put the
    archive's pair in its place, and one running as `NT SERVICE\hMailServer` failed the restore. A mail client that
    had accepted the other server's self-signed certificate asks again, and a TLSA record published for the other
    key no longer matches; to have the archive's pair instead, stop the service and copy both files from the
    archive's `Certificates` folder over this installation's. A file in the data folder the service's account may not
    replace or remove is left as it is and named (`HM5705`), where the restore used to stop. And of the files at the
    data directory's root a restore removes only the delivery queue's message files, where it removed every one.

### Downloads and the release

- **The AArch64 `.deb` and `.rpm` are in the release again**. The releases up to 6.3.3
  carried them; 6.3.4 and 6.3.5 did not, so an arm64 Debian, Ubuntu or RPM server has had no newer file than
  6.3.3. The release now collects `hmailserver_<v>_arm64.deb`, cross-compiled by `deb-ubuntu26.04-arm64` in
  Ubuntu 26.04's own container, and `hmailserver-<v>-1.aarch64.rpm`, `rpm-fedora44-aarch64`'s copy of its Fedora
  44 build - the names an installed arm64 server's update check asks for - each with its own CycloneDX SBOM
  (`<file>.cdx.json`), a line in `hmailserver-linux-<v>-SHA256SUMS.txt` and a Sigstore bundle, checked by
  `release-verify` with the server's own verifier like every other file.
  `release` waits for each package's build, its SBOM job and its install job, which installs and runs it in an
  AArch64 container of its distribution under qemu-user; no AArch64 machine has run them. A package job's
  container has neither PowerShell nor Python, so every package build now records what it was built against
  (`built-against/`: what dpkg, or the aarch64 sysroot's rpm database, held, and Boost's header), and the new
  `deb-ubuntu26.04-arm64-sbom` and `rpm-fedora44-aarch64-sbom` jobs make the SBOM from that in the CI image
  (`build/ci/packages/package-sbom.sh`; `build/asset-sbom.py make --dpkg-list` or `--rpm-list`). In a cross
  build the C and C++ libraries are named as the cross toolchain's, and every other library as the target
  architecture's own `-dev` package, never the build container's. The `release` job's `collect` step
  (`build/ci/release-chain.py collect`) holds each AArch64 package to the sums its build job wrote, and
  `release-publish` checks that the feed gives Debian and RPM servers on AArch64 their file and bundle, as it does for
  x86-64. The x86-64 files, their SBOMs and `linux-build`'s two lines of the sums are unchanged; `linux-repro` still
  compares the x86-64 packages alone, and the AArch64 ones are built once and not compared (ReproducibleBuilds.md).
  `build/check-release-chain.py` now holds `release` to those six package jobs, none of which may be allowed to fail or
  wait for a press in a tag's pipeline, and `collect` to where they leave the files; the update feed's
  `/download/hmailserver_arm64.deb` and `/download/hmailserver.aarch64.rpm` follow the first stable release that carries
  them, and its `--serve-check` fails a stable release that carries any server's file without its bundle. The generic
  arm64 `.deb` goes to the apt suite `ubuntu26.04`, as the amd64 one does; the generic `.rpm`s still go to no dnf
  repository, and the pages job no longer calls the aarch64 one built on Ubuntu (ContinuousIntegration.md, "What a
  release carries"; PackagesDeb.md; PackagesRpm.md).

- **The container image is pushed from the release's own pipeline, by hand, and started before it is left there**
  (6.3.6's own pipeline is the first to run these jobs, which no branch's pipeline has). The image jobs are in
  every protected `v*` tag's pipeline. `container-image-deb`,
  started by hand after `release-publish` once the project variable `HM_PUBLISH_IMAGE` is 1, takes the `.deb` and its
  SBOM from the published release and verifies both with cosign as the tag's pipeline. The variable is read as the
  jobs run, not by a rule, so a release tagged before it is set has its image pushed from its own pipeline
  afterwards; 6.3.5's own pipeline has no image jobs, so the first image is 6.3.6's. The image is pushed only at
  `registry.gitlab.com/hmailserver/hmailserver`, which the Compose file and the chart now name (`linux-image.sh`
  and `container-image-push.sh` refuse the old path), then signed, scanned and started beside PostgreSQL by
  `container-image-start`, which checks SMTP and the REST API. The image sets `HM_INSTALL_KIND=container` and
  carries OCI and Artifact Hub labels ([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

- **The container image for linux/arm64 as well as linux/amd64, one index over both**. Each platform's image is built
  from the release's `.deb` of its own architecture: the Dockerfile takes the `.deb` of `TARGETARCH` and runs
  `hmailserver --version` at the end of its build, so an image whose server cannot run on its platform is never made.
  Each platform has its own kaniko job on linix, the arm64 one under qemu-user: `linux-image-arm64` beside
  `linux-image` in a branch's pipeline, and `container-image-amd64` and `container-image-arm64` in a tag's.
  `container-image` joins the two under one Docker manifest list with regctl, the tool the channels already take from
  Docker Hub, and pushes nothing until both images and the index have passed their checks; no tag names anything but
  the index. `:latest` and `:<major>.<minor>` only move forward: a release whose image jobs run after a newer
  release's still pushes `:<version>`, and leaves either tag where it is when the image it names says it is a newer
  version, or its version cannot be read, which the job's log says. The index and each platform's image are signed,
  each image has its own SBOM attested to it, and each is scanned and started beside PostgreSQL, the arm64 one under
  emulation. By hand on linix, an arm64 image of 6.3.5, from batch41's arm64 `.deb` (the 6.3.5 release carries none),
  built in about three minutes and passed the start test ([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

- **The Helm chart as a file of each release, served from `https://updates.progressiverobot.com/helm/`**.
  The release carries it, signed, with its `.sigstore.json` copy. `helm-chart-package` packages `hmailserver-chart-<version>.tgz` at the release's version,
  naming the release's image, and `build/ci/helm-repo.py` writes the repository's index and Artifact Hub's metadata
  from the update feed in `package-repos`. The chart's `artifacthub.io/images` annotation names the image's two
  platforms, `linux/amd64` and `linux/arm64`, and `helm-chart-package.sh` refuses a tree's chart, or a package, that
  lists any other set.

- **Artifact Hub's metadata for the container image's repository, pushed under the tag `artifacthub.io`**. The new
  job `container-image-artifacthub` pushes the tree's `platform/packaging/docker/artifacthub-repo.yml` with regctl, in
  the shape Artifact Hub documents (an empty configuration and one layer, each of Artifact Hub's media type), and
  reads it back byte for byte: after `container-image` in each release's pipeline, and by hand in the default
  branch's, so that the image's listing gets its Verified publisher label without waiting for a release. It pushes
  nothing until a maintainer has added the repository on Artifact Hub and committed the repository ID it gives,
  refuses a file with anything else in it, and pushes only at the group's path while `HM_PUBLISH_IMAGE` is 1. Nothing
  waits for it. Against a stand-in registry on linix it pushed what an independent reader, following Artifact Hub's
  documentation, read the ID from ([ContainerImage.md](hmailserver/docs/ContainerImage.md), "Artifact Hub").

- **`hmctl` as a PyPI package, `hmailserver-cli`, and the PowerShell module with a Gallery manifest** (neither
  published yet: the two accounts do not exist). `build/ci/cli-packages.py` builds `hmailserver-cli`'s sdist and wheel from
  `hmailserver/source/Tools/Cli/python/` with the same `hmctl` file the `.deb` and the `.rpm` install, copied into it
  byte for byte, at `Version.h`'s version, with no dependency and no code that runs when it is installed; it runs
  on Python 3.8 and later. `HmailServer.psd1`, the module's new manifest, lists its 95 functions for Windows
  PowerShell 5.1 and PowerShell 7 at the server's version, which the release's version stamp now includes. The
  manual `master` jobs `pypi-publish` (a PyPI trusted publisher, with PyPI attestations) and `psgallery-publish`
  (`PSGALLERY_API_KEY`) say "not configured" until each account is set up. They publish only a release the update
  feed lists, rebuilt from its owner-signed tag, and never a version twice: `pypi-publish` finishes an upload that
  stopped halfway and fails when a PyPI publisher that once worked stops matching, and `psgallery-publish` sends
  the module only as the release signed it (below, "The PowerShell module is Authenticode-signed in the release")
  ([CommandLinePackages.md](hmailserver/docs/CommandLinePackages.md)).

- **A source tarball with nothing non-free in it, which the Linux build and suite run from** (the release
  carries it from 6.3.6, below). `build/ci/source-tarball.sh` writes `hmailserver-<version>.tar.gz`
  from a commit with `git archive`: the commit's tree less every path of `build/source-exclusions.txt` - SQL Server
  Compact 3.5's headers and both copies of its licence, which are not free, and what only the Windows build and its
  installer use (the rest of `libraries/` with the Fat Cow icons, the COM API, the Windows service host and
  helpers, the vendored zlib, the Control Panel and the database tools, the VBScript add-ons, the .NET Framework
  developer tools and the installer's sources, keeping the Control Deck and `dh2048.pem`, which the packages
  install). It is reproducible - every entry carries the commit's time, uid and gid 0 and a 0644 or 0755
  mode, line endings are `.gitattributes`' alone (no system attributes file is read, and a clone whose
  `.git/info/attributes` sets any is refused), gzip writes no name or time - and git 2.39.5, 2.47.3, 2.50.1 and
  2.53.0 with gzip 1.10 to 1.14 made the same bytes. QuickJS stays and is declared with the other third-party
  notices of the code the Linux build compiles; `docs/SourceTarball.md` also lists the third-party test data kept
  (Mail::DKIM's corpus, and captured messages with no licence stated, for the Debian package to decide on).
  `build/check-source-tarball.py` holds the list to the tree in the `checkers` job (nothing the CMake build,
  `linux-build.sh`, `linux-suite.sh`, the other Linux jobs' scripts - packages, fuzzing, cross, sanitizer,
  coverage, repro - the suite's project and its directory, what the build and the suite read at run time, or a
  POSIX `#include` reads may be left out, and every file of the Windows-only trees must be) and a tarball to the
  list, its licences and its format; `--files-excluded` prints Debian's `Files-Excluded` field from the same list.
  The `source-tarball` job makes it twice and compares, and `linux-build` and `linux-suite` take it with
  `HM_SOURCE_TARBALL`; `build/native-dependencies.ps1 -LinuxOnly` gives the packages' SBOMs their components
  without the Windows build's inputs. See [SourceTarball.md](hmailserver/docs/SourceTarball.md).

- **The release carries every file the pipeline makes for it**. From 6.3.6 a release is 62 files and 160 links: the
  x64 and ARM64 installers (the ARM64 one below); the generic `.deb` and `.rpm` for x86-64 and AArch64; each Debian 12
  and 13, Ubuntu 24.04, Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed release's own package for both
  architectures (`hmailserver_<v>-1~<release>_<arch>.deb`, `hmailserver-<v>-1.<release>.<arch>.rpm`); both AppImages,
  with the project's own runtime (work item 36), the source of the libraries linked into it and the source of the
  libraries they carry in `usr/lib` (below); the source tarball; the PowerShell module's `HmailServer.psd1` and
  `HmailServer.psm1`, Authenticode-signed (below); the Helm chart; the sums of every Linux file, the
  two AppImage sources files and the chart; an SBOM for each of the 26 compiled files and the tarball, and the tree's
  two; a Sigstore bundle beside each, a byte-identical `<file>.sigstore.json` of each primary file's bundle, and
  `hmailserver-<v>.intoto.sigstore.json`, SLSA v1 build provenance naming every file by its SHA-256, signed as the
  pipeline with `cosign attest-blob` and checked with `cosign verify-blob-attestation` (SECURITY.md, "Build
  provenance"). The new `release-packages` job gathers the per-distribution packages, the AppImages, the two AppImage
  sources files and the tarball, holds each to its build's sums and makes the SBOM of each but the sources files
  (`build/asset-sbom.py` gains the kinds `appimage`, which lists what the image bundles and what its runtime links,
  and `source`), so that `release` stays under GitLab's fifty needs. Before anything is signed, and again before
  anything is uploaded, the release counts what GitLab and Pages take: every link's path (the `~` of a `.deb`'s
  version, which GitLab refuses there, is written `-`), `_redirects` under 1,000 rules and 64 KiB with the apt and
  dnf rules, and the feed under the 4 MiB a server reads (with the repositories as `package-repos`' `--keep` keeps
  them). The apt and dnf suites will serve the new packages once the repository key exists; openSUSE's go through
  OBS. A protected `batch*` branch rehearses the chain (`.gitlab/ci/release-rehearsal.yml`): collected with
  `windows-packages-x64`'s installer, signed as the branch, which no server trusts, and checked with the server's own
  verifier, which must refuse every bundle at its identity; nothing is published (ContinuousIntegration.md, "What a
  release carries", "The release rehearsal").

- **The source of the AppImages' runtime's libraries goes out with them**. Each
  AppImage's runtime is linked statically on Alpine, and two of the libraries it takes in, libfuse3 (LGPL-2.1) and
  lzo (GPL-2.0-or-later), ask for their own source to go with the binary, not only the runtime's; with lzo's GPL, so
  does every other library linked in but the compiler's own. From 6.3.6 the release carries
  `hmailserver-<v>-appimage-runtime-sources.tar.gz`: for libfuse3, lzo, squashfuse, zstd, lz4, xz, zlib and musl, at
  the version and aports commit each runtime job recorded from the linker's map and apk's database, Alpine's APKBUILD
  and patches from gitlab.alpinelinux.org, fetched by the commit's id, and the upstream archive from Alpine's
  distfiles mirror - never the GitHub releases several APKBUILDs name - each source held to the SHA-512 its APKBUILD
  gives, with a README.txt saying where each file came from, what each licence asks and how to link the runtime again
  with a changed library. The new `appimage-runtime-sources` job packs it reproducibly, twice, and compares; the
  release gathers it with a line in the sums, a bundle and a `.sigstore.json` copy, and no SBOM, being source. Each
  image's `usr/share/hmailserver/licenses/appimage-runtime.txt` lists what its runtime links and names the file, and
  the image carries each library's licence files in `usr/share/hmailserver/licenses/appimage-runtime/`, with the
  GPL-3.0's text, under which the runtime takes lzo (runtime.c being AGPL-3.0-or-later). GCC's start-up files and
  libgcc stay out, under the GCC Runtime Library Exception
  ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md), "Licences"; work item 36).

- **The AppImages carry the licences of the libraries in their `usr/lib`, and the release their source**.
  Each AppImage carries in `usr/lib` the shared libraries the server needs that are not left to the
  host - libpq, libldap and what they need, nineteen files from fourteen Ubuntu 24.04 source packages - and seven of
  them are under the LGPL (GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils), which asks for the
  library's source to be offered with the binary. From 6.3.6 each image carries, in
  `usr/share/hmailserver/licenses/`, the copyright file of every package a library came from, every licence text
  those name (a list in braces included), `bundled.txt`, which lists each library with its package, version and
  source package and names the release file with their source, and hMailServer's own AGPL-3.0 text (`AGPL-3.0.txt`).
  The release carries `hmailserver-<v>-appimage-bundled-sources.tar.gz`: all fourteen source packages (`.dsc`,
  upstream archives, Ubuntu's changes) at the versions the images took, from Ubuntu's archive or, for a version the
  archive has superseded since - as a package the pinned build image already had can be, which the build does not
  upgrade - from its snapshot service at the time the image's dpkg installed it, each file held to the `Sources`
  index of an `InRelease` verified against Ubuntu's archive keys and again to its `.dsc`, with a README.txt saying
  what each licence asks, where each package was taken from and how to run an AppImage with a changed library; about
  75 MB, one file for both architectures. The new `appimage-bundled-sources` job packs it reproducibly, twice, and
  compares; the release gathers it like the runtime's sources, with no SBOM. Each AppImage's SBOM now lists every
  Alpine package its runtime links statically, with its aports commit and licence, and each bundled library's source
  package; a library an image starts to carry fails the image's own job until its licence is named in
  `build/ci/packages/appimage-bundled-sources.sh` ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md),
  "Licences"; work item 36).

- **The release channels, each published by hand from `master`, dormant until its account exists** (none live as
  6.3.6 is released).
  `.gitlab/ci/channels.yml` adds `registry-mirror` (the image by digest, with its cosign signature and SBOM
  attestation, to Docker Hub, Quay.io and optionally Amazon ECR Public, with `regctl`), `aur-publish` (the AUR's
  `hmailserver`), `ppa-upload` (the Debian source package to `ppa:hmailserver/hmailserver` for Ubuntu 24.04 and
  26.04), `copr-build` (Fedora Copr through its webhook; `.copr/Makefile` makes the source RPM in Copr's builder),
  `obs-publish` (the openSUSE Build Service, from `platform/packaging/obs/_service`), `chocolatey-release` with
  `chocolatey-publish` (packed on bench150), `snap-upload` (edge and candidate) and `channels-verify`, the dry run.
  Each publishes the release `HM_CHANNEL_VERSION` names or the update feed's latest (Copr and OBS the latest alone),
  verifies every release file it takes against its Sigstore bundle as the tag's pipeline - the project's id, read
  from the certificate's extensions, required at the group's path - before it logs in or sends anything, and never
  moves a channel back to an older release. The snap, not a release file, is held to the verified tarball or the tag;
  OBS fetches the tarball itself once the job has checked the permanent link serves the verified bytes. cosign and
  regctl come out of their images pinned by digest; nothing from GitHub, and no credential on a command line
  ([Channels.md](hmailserver/docs/Channels.md)).

- **The Arch package builds from the release's source tarball**, not a git tag, as a distribution's build
  (`-DHM_LIVE_UPDATE=OFF`, naming `makepkg -si` from the AUR as its update) and without LTO (Arch's default full-LTO
  link of the server passed 6 GB and was killed); `aur-publish` writes the verified tarball's SHA-256 in.

- **The Chocolatey package's id comes from `make-package-manifests.ps1 -ChocolateyId`**: `hmailserver`, or
  `hmailserver-progressiverobot` if upstream's maintainers do not add the project. Its addresses are the group's.

- **The release checks read the signing certificate's fields from its extensions, never by searching its bytes, and
  only from the certificate cosign verified.** release-verify.sh and container-image-deb.sh took the project id
  (Fulcio's 1.3.6.1.4.1.57264.1.15) from wherever its bytes first appeared, and a certificate's public key is bytes
  its holder chooses; registry-mirror read the id from whichever signature certificate held the identity's bytes, and
  check-provenance matched the identity the same way. One reader, build/ci/sigstore-certificate.py, now walks the
  certificate's DER to its extensions, with a shell twin for cosign's image held to it case by case. The shell reader
  reads a v0.3 bundle only - cosign verifies just the first certificate of a v0.1 or v0.2 chain, and accepts field
  names spelled in ways a count of `rawBytes` misses - container-image-deb.sh refuses a cosign without
  `--new-bundle-format`, and registry-mirror refuses a signature manifest Go's JSON reader would read otherwise (a
  key in another case or with a long s, a key twice, an index at the `.sig` tag); the Python reader refuses a
  duplicated key. build/check-release-chain.py holds every script to the readers and refuses the Sigstore OID written
  as bytes, decimal numbers or base64 anywhere else. The container image's mirror has cosign verify its signature and
  SBOM attestation from one copy of their manifests, each fetched by its digest, reads the project's id from those
  same manifests - the attestation's certificate now held to it as the signature's is - and puts exactly those
  manifests at each registry, replacing whatever signature was there; the release's SLSA provenance is held to the
  project's id too, as its files are; and neither certificate reader relies any longer on cosign refusing a malformed
  bundle to know which certificate it read. The server's own verifier reads the extension through OpenSSL and was
  never affected.
- **An Atom feed of releases** at https://updates.progressiverobot.com/releases.atom: every release, newest first,
  the first paragraph of its notes as the summary, ids that do not change when the project moves into the group;
  build/check-atom-feed.py holds it to RFC 4287 and to releases.json in the pages job
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "The Atom feed"). The files a server reads are unchanged, byte
  for byte, and a release whose name XML cannot carry is entered under its tag rather than stopping the feed.
- **Releases pause after 6.3.6; security fixes do not.** The 6.3 line receives security fixes until 30 September
  2027, each in a release of its own ([SECURITY.md](SECURITY.md), "While releases pause"; [RELEASE.md](RELEASE.md),
  "A security release during the pause").
- **The container image's mirror checks each registry as anyone would see it.** `registry-mirror` verifies the image
  again at each destination, which Channels.md calls anonymous; it read with the regctl configuration it had logged
  in with and ignored whether the logout worked, so a failed logout left the check logged in, where an image or a
  signature only the account can see would pass. The check now reads with a configuration of its own that no login is
  written to, and a failed logout is said in the log; Channels.md says a registry's earlier signature or attestation
  for the image is replaced by the verified one.
- **A Debian source package and an RPM spec, built from the release's source tarball as a distribution builds them.**
  `hmailserver/source/Server/platform/packaging/debian-src/debian/` builds `hmailserver` and `hmailserver-cli`
  (debhelper 13) from `hmailserver_<version>+ds.orig.tar.gz`: the release tarball repacked by `Files-Excluded`, which
  is the project's exclusion list plus five test files that state no licence and the Debian build does not read.
  `platform/packaging/rpm/hmailserver.spec` builds the same two packages for Fedora, EL and openSUSE from the tarball
  as it is. Both are built without the live update, naming `apt upgrade hmailserver`, `dnf upgrade hmailserver` or
  `zypper update hmailserver` instead, and link the distribution's shared Boost (`-DHM_BOOST_STATIC=OFF`, new; the
  project's own packages keep it static). The account comes from `sysusers.d`, the unit is installed and not started,
  `hmctl` is a package of its own that depends on `python3`, and the server's `--version` and the tester programs run
  in the build, and neither links with LTO. The Debian package has a DEP-5 copyright over every third-party notice; a
  watch file on GitLab's releases API; and an autopkgtest that starts the server against a throwaway PostgreSQL and
  asks SMTP and IMAP for their greetings. The spec declares `Provides: bundled(quickjs) = 2026.06.04`. New CI jobs
  (`.gitlab/ci/source-packages.yml`, `batch*` and `master` only) build both for Debian 13, Ubuntu 24.04 and 26.04,
  Fedora 44, EL 10, Leap 16 and Tumbleweed with the network refused, fail on a lintian or rpmlint error, and install
  and start the result in a clean container. On the laptop the spec built on Fedora 44 and EL 10 and each server
  installed and started against PostgreSQL, and the Debian package built on Ubuntu 26.04 and passed its autopkgtest;
  a Fedora 44 server that died before `main` there was a file the laptop's WSL file system had corrupted, not a GCC
  16 or code defect. See [SourcePackages.md](hmailserver/docs/SourcePackages.md).
- **A Nix flake of the project's own: `nix run gitlab:hmailserver/hmailserver`**.
  `flake.nix` at the top of the repository builds the Linux server as a Nix package, `packages.<system>.hmailserver`
  for x86_64-linux and aarch64-linux, with nixpkgs' Boost, OpenSSL, libpq, OpenLDAP and zlib, the live update off
  naming `nix flake update hmailserver`, the MariaDB connector on the server's RUNPATH for the `dlopen` a MySQL
  database needs, and the server's `--version` and every tester program in `checkPhase`; `hmctl` is its `cli` output.
  nixpkgs is NixOS 26.05's release tarball from releases.nixos.org, pinned by its NAR hash in `flake.lock` and
  imported as a plain tree, never `github:NixOS/nixpkgs`, and the flake registry is not read. `nixosModules.default`
  is `services.hmailserver`: the server under systemd with the packaged unit's sandbox;
  `/etc/hmailserver/hMailServer.ini` with its directories and database set from the options at each start and every
  other line kept, the stored-secrets key among them, and the database password too when no password file gives
  one; a local PostgreSQL reached over its socket by peer authentication, root mapped to the same role so that the
  packaging guide's `sudo hmailserver` commands work, and the server started after `postgresql.target`, once
  PostgreSQL's setup has made that role; the database made and upgraded before each start and the first start's
  secure defaults; and options for another database, an administrator password file, settings, the firewall, and
  the auto-ban's reaching the firewall (`autoBanFirewall`, below). `checks.<system>.nixos` starts that machine and
  asks SMTP (with STARTTLS) and IMAP for their greetings, as the Debian package's autopkgtest does, and the REST API
  for its status, has root make the stored-secrets key, holds the service to no restart before that, and asks again
  after a restart, which must keep the key and a database password typed into the file; then it places an auto-ban
  through the REST API and requires the address in the hook's nftables set, and out of it once the ban is deleted.
  The new `nix` job (`.gitlab/ci/nix.yml`, `build/ci/nix.sh`; master and `batch*`) holds the lock to
  releases.nixos.org, evaluates every output for both systems, builds the package and runs the NixOS test under
  QEMU's TCG, since linix has no KVM. On 29 September 2026 the test passed with KVM and under TCG, and the job's own
  script passed end to end on linix, without KVM, in 27 minutes. See [Nix.md](hmailserver/docs/Nix.md).
- **Alpine Linux: the server on musl, an OpenRC service and an APKBUILD**. The server had
  never been built on musl; on Alpine 3.24 four things were musl's, each fixed where it was. `File.cpp` took
  `strerror_r`'s answer as glibc's GNU one gives it, and musl's is the POSIX one. The server named US-ASCII to iconv
  as `ANSI_X3.4-1968`, glibc's name, which musl's iconv does not know, so a `us-ascii` part did not convert and a
  script's text added to an empty body was lost; it is now `US-ASCII`, which every iconv knows. And musl's iconv has
  no UTF-7, in which IMAP folder names are written: `CMakeLists.txt` now asks the iconv it found for UTF-7 and
  US-ASCII and refuses one without them, naming GNU libiconv, which the Alpine package builds with. And the tree
  writes a `size_t` as MSVC's `%Iu`, which the POSIX build handed to the C library as it was: glibc read the `I` as a
  flag of its own and printed the size, cut to 32 bits, and musl refused the format and wrote nothing, so every IMAP
  FETCH item sent as a literal (`BODY[...] {n}`, `BINARY`, `PREVIEW`) lost its name and size; `%I` is now rewritten
  as C's `%z`, which also mends a size above 4 GB on glibc. `platform/packaging/alpine/APKBUILD` (with
  `hmailserver.pre-install`, which makes the account, and `hmailserver.post-upgrade`, which upgrades the schema)
  builds `hmailserver`, `hmailserver-openrc` and `hmailserver-cli` from the release's source tarball with Alpine's
  libraries, the live update off naming `apk upgrade hmailserver`, depends on `ca-certificates-bundle` as the other
  packages depend on `ca-certificates`, and runs the server's `--version` and every tester in `check()`;
  `apkbuild-lint` has nothing to say about it, and `build/check-linux-version-stamp.sh` holds its `pkgver` to
  `Version.h`, so stamping a release now edits it too (RELEASE.md).
  `platform/packaging/openrc/hmailserver.initd` runs the server as `hmailserver` with `CAP_NET_BIND_SERVICE` ambient,
  no new privileges and umask 0027, applies the first start's secure defaults, reloads on SIGHUP and gives a stop
  five minutes to drain; `hmailserver --create-database` says, where OpenRC and that service are installed, that the
  service's next start secures the new database, where it said only the systemd unit's first start would. New CI
  jobs (`.gitlab/ci/alpine.yml`, `build/ci/alpine.sh`; master and `batch*`):
  `alpine-package` runs the APKBUILD with abuild on the source tarball, `alpine-install` installs the packages in a
  clean container, starts the server with `rc-service` against PostgreSQL and asks SMTP (with STARTTLS) and IMAP, and
  `alpine-suite` runs the whole Linux suite against the installed server in Alpine, with Alpine's .NET SDK. On linix
  on 29 September 2026 the package built and passed `check()`, the install and start passed, and the whole Linux
  suite passed against the installed server with the counts batch42's glibc suite had that day: 1897 passed, 331
  skipped, none failed, of 2228. The aports merge request is a named maintainer's (Roadmap3, "Alpine Linux"). See
  [Alpine.md](hmailserver/docs/Alpine.md).
- **The `.deb` and the `.rpm` carry hMailServer's licence and a copyright file**. Until
  now the release's `.deb` and `.rpm`, and every per-release package built with them, carried QuickJS's licence and
  nothing of the project's own: no AGPL-3.0 text, which the AGPL's sections 4 and 6 ask to go with every copy, and no
  copyright file, which Debian Policy 12.5 asks of every package; only the Arch package installed `LICENSE`. From
  6.3.6 every Linux install puts hMailServer's AGPL-3.0 text, the FSF's file byte for byte, at
  `/usr/share/hmailserver/licenses/AGPL-3.0.txt` beside `QuickJS.txt`, as the AppImages carry it; the `.deb` carries
  `/usr/share/doc/hmailserver/copyright` and the `.rpm` `/usr/share/licenses/hmailserver/copyright` - the Debian
  source package's machine-readable copyright file, with the AGPL in full and the terms of what is compiled into the
  server - and the `.rpm` marks all three `%license`, so that `rpm -qL` lists them. Its `License` now names the
  licences an identifier can name,
  `AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND LicenseRef-Fedora-UltraPermissive AND BSL-1.0`, where
  it named the AGPL alone. `build/ci/packages/check-licenses.sh` holds the three files to the tree by SHA-256, mode
  and owner: `linux-build`, `deb-build.sh` and `rpm-build.sh` refuse a package without them, and every install test
  checks them as installed ([PackagesDeb.md](hmailserver/docs/PackagesDeb.md),
  [PackagesRpm.md](hmailserver/docs/PackagesRpm.md), "The licences").
- **The backup index's XML is the project's own code; XMLite is gone.** The code that writes and reads
  `hMailServerBackup.xml`, writes a person's `account.xml` and reads the class tests' `test_config.xml` was XMLite
  (Cho Kyung Min, The Code Project, 2002-2004), which carried only its author's notice - one a distribution's legal
  review would have had to settle, and which the project could not relicense. `Common/Util/XmlTree.cpp` replaces it,
  written anew to a description of what XMLite wrote and read and keeping the callers' names; the description was
  taken from XMLite's code by the same hand that wrote the replacement, so it is not a clean-room reimplementation,
  and none of XMLite's code is in it. It writes an index byte for byte as XMLite did, and reads and refuses exactly
  what 6.3.5's XMLite did, so every archive 6.3.5 restores still restores; four indexes written by 6.2.13 and 6.3.3,
  read and written back through the server's own file code, come back byte for byte. A differential harness compared
  old and new over about 17 million cases each with GCC 15, clang 21, and clang with AddressSanitizer and
  UndefinedBehaviorSanitizer, and 7 million with MSVC, and found no difference. `XmlTreeTester` runs in ClassTester
  and, on Linux, as the new `xml-tree-tests` program, which the Debian and RPM source-package builds also run. HM4229
  reports the same codes in new words. `debian/copyright`, the RPM spec and the source-package documents no longer
  declare XMLite, and `check-source-tarball.py` now refuses a `debian/copyright` paragraph whose files are gone.
- **The date class is the project's own.** `HM::DateTime`, which every stored date, expiry, IMAP UIDVALIDITY and MIME
  date passes through, was a class of 1999 from CodeGuru that stated no licence and kept the names of MFC's
  `COleDateTime` in its helpers, compiled into both servers: a file no distribution's archive could take.
  `Common/Util/DateTime.cpp` replaces it, written from the calendar and the OLE Automation date's definition to the
  old class's behaviour. A differential test held the two to the same answers bit for bit (a NaN's sign aside) under
  MSVC, GCC 15 and clang 21, about 84 million comparisons a run, and, on Linux, ran the server's own MIME, IMAP and
  system-date parsers over both; a second check held the new class to Python's calendar on every day of the years 1
  to 9999 and to the C library's local time in twelve time zones. Nothing an administrator or a client sees changes,
  except that a date made from a NaN (which only a COM client could pass) now reads as no date instead of undefined
  behaviour. With it, no file compiled into the server is left whose licence a distribution has to settle:
  `debian/copyright`, the RPM spec and SourcePackages.md no longer list it. A new `datetime-tests` program runs the
  date class's and the date parsers' tests on Linux, where ClassTester does not run, in `linux-build` and in the
  Debian and RPM package builds.
- **`ScriptSite.h` opens with VisionTech's notice again, and says it is altered.** The ATL Active Script host that
  only `hMailServer.exe` compiles (`Common/Scripting/ScriptSite.h`) is VisionTech Limited's of 2001, under a notice
  that may be neither removed nor changed in any source distribution and asks an altered version to say so. Since
  August 2026 `build/add-license-headers.py` had put the project's header above it - its copyright and
  `SPDX-License-Identifier: AGPL-3.0-or-later` - which claimed the file as the project's, and nothing said it was
  altered. The notice is first again, byte for byte as hMailServer's own source had it; below it the file says it
  is altered, by whom, and that the changes are under the project's licence, ending with the project's address and
  `SPDX-License-Identifier: LicenseRef-ScriptSite AND AGPL-3.0-or-later`. The header script leaves the file alone
  (`OWN_NOTICE_FIRST`), and its `--check`, which the checkers job runs, fails when the file opens with the project's
  header again, when its first lines are not the notice word for word (held by the notice's SHA-256), or when it
  loses its own SPDX line. `debian/copyright`'s paragraph for the file names the alterations' holders and licence
  beside VisionTech's ([SourceTarball.md](hmailserver/docs/SourceTarball.md)).
- **A release's download links work, and each file's bundle is beside it at GitLab's permanent address.** 6.3.5's 18
  links were given the path `/downloads/<name>`, and GitLab, which serves a link's path below
  `/-/releases/<tag>/downloads` itself, served each at `.../downloads/downloads/<name>`: a 404, until the links were
  corrected by hand through the API on 29 September 2026. `build/ci/release-chain.py` now gives each link the file's
  name without the version after one slash, and refuses a path under `/downloads/` before anything is signed or
  uploaded. From 6.3.6 every file's path follows that rule - a bundle's and a bundle's `.sigstore.json` copy's too,
  whose paths kept the version up to 6.3.5 (6.3.5's bundles, 6.3.3's copies), as 6.3.4's SBOMs' did - so
  `https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/hMailServer-x64.exe` and
  `.../hMailServer-x64.exe.cosign.bundle` are the latest installer and the bundle that verifies it, under the names
  the update site's `/download/` gives them. A link's name is still the file's own, which servers and the feed read.
  `release-chain.py --self-test` holds the paths, where GitLab serves them and the pairing, with 6.3.5's paths as a
  negative control; `build/check-release-chain.py` holds them and every `permalink/latest` address the tree writes
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "The links' names and paths").
- **The `.deb` and the `.rpm` require `ca-certificates`**, which every HTTPS request the server makes needs, the
  update check's among them; in the first Linux live update PostgreSQL had pulled it in. So do the source packages
  and the Arch package.
- **The `.rpm` asks for glibc's character-set converters, and the RPM spec requires them**, `glibc-gconv-extra` on
  Fedora and EL and `glibc-gconv-modules-extra` on openSUSE. glibc only recommends them, so every container image of
  those distributions, and any installation made without recommended packages, goes without, and there, with every
  `.rpm` up to 6.3.5, the server read and wrote no text in a character set but Unicode's, US-ASCII, UTF-7, ISO-8859-1,
  ISO-8859-15 and Windows-1252: a header or a body in BIG5, ISO-2022-JP, Windows-1251 or ISO-8859-2 came out empty,
  and a PST's text in code page 932, 936, 949 or KOI8 was imported as its ASCII, each byte outside ASCII a replacement
  character, and an RTF body's read as Windows-1252 (the pinned Fedora 44, UBI 9 and 10, Leap 16.0 and Tumbleweed
  images, 1 October 2026). Every `.rpm` the project makes, the generic ones and each release's own, now requires
  `((glibc-gconv-extra(<arch>) or glibc-gconv-modules-extra(<arch>)) if (dnf and system-release))` and recommends
  `(glibc-gconv-extra(<arch>) or glibc-gconv-modules-extra(<arch>))`, which each distribution meets with its own
  package, built for the server's architecture: an i686 build of it beside an x86-64 glibc would meet the bare name
  and convert nothing. Required where `dnf` is installed on Fedora or EL, whose release package provides
  `system-release` - there `dnf` installs them with the package, the live update's `dnf install` included; only
  recommended elsewhere, because on openSUSE the live update installs with `rpm -Uvh`, which installs nothing beside
  the package and refuses one whose requirements are not met, or, where `dnf` has been installed beside zypper, with
  that `dnf`, which has no repositories there. With the requirement everywhere, 6.3.5's helper refused the update to
  6.3.6 on every openSUSE server without the converters, and the server stayed on 6.3.5; now it is updated, zypper
  installs the recommendation unless `solver.onlyRequires` is on, and the server names what it cannot convert at its
  start. A recommendation alone would not do on Fedora and EL: `dnf` does not install one that glibc's own
  recommendation of the same package already left out (`exclude_from_weak_autodetect`). So on Fedora and EL `dnf`
  needs a repository that has them: a server without them whose `dnf` has none enabled, an unregistered RHEL say, has
  the update refused, and stays on 6.3.5, until `glibc-gconv-extra` is installed. Measured on Fedora 44 and 43,
  UBI 9 and 10 and their minimal images, Leap 16.0 and Tumbleweed, with and without `dnf` beside zypper, on 2 and
  3 October 2026 ([PackagesRpm.md](hmailserver/docs/PackagesRpm.md), "The character sets"). The spec, whose packages
  have no live update, requires its distribution's one by name. `rpm-install-test.sh` holds each package to the
  requirement and the recommendation and, in its release's clean image, which lacks the converters, requires that the
  installed server finds every character set it names and that BIG5 and ISO-2022-JP text converts, then takes the
  converters away again and requires that neither holds, and that the live update's install goes through without
  them on openSUSE - `rpm -Uvh`, and `dnf install -y` with `dnf` installed beside zypper - and that `rpm -Uvh` is
  refused for them on Fedora and EL; `rpm-build.sh` writes the recommendation into the spec where a
  release's CPack leaves it out, as it does the suggestions; the RPM source packages' build reads the requirement and
  their install test requires the first through `server-starts`, and `linux-package-rpm` reads the requirement and the
  recommendation in the generic x86-64 `.rpm`. An AppImage runs on its
  host's glibc and cannot require a package: on Fedora, EL or openSUSE that package is installed by hand
  ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md)), and the server says when it is missing (Operations,
  below).
- **The `.rpm`'s `%changelog` is its own**: one entry, from the version's heading in `CHANGELOG.md` and dated by
  `SOURCE_DATE_EPOCH`, in place of CPack's placeholder of 2010.
- **The Linux update helper runs `apt-get` quietly** (`-q`, `Dpkg::Use-Pty=0`), so the journal no longer holds
  dpkg's progress as "[614B blob data]".
- **The release's own pipeline deploys the update feed with the release in it.** 6.3.5's tag pipeline ended in
  `update-feed`, a trigger of a pipeline on `master` running `pages` alone. GitLab refused it the moment it started,
  once `release-publish` had passed (`invalid_bridge_trigger`, "downstream pipeline trigger definition is invalid"):
  it drops a multi-project trigger naming the project that runs it. The tag's pipeline therefore reads "failed", and
  the feed was deployed by a `master` pipeline started by hand. `update-feed` is gone; in a `v*` tag's pipeline
  `pages` now needs `release-publish` and runs once it has passed, and `package-repos` deploys the site from that
  pipeline. On `master` and in a schedule's pipeline `pages` runs at once, as before. No token, variable or project
  path is involved, so the project's move into the group on 29 September changed nothing here.
  `build/check-release-chain.py` fails on a `pages` that does not wait for `release-publish` and on any `trigger:
  project` in a CI file.
- **The project is at `gitlab.com/hmailserver/hmailserver`.** It moved from the owner's namespace into the group
  `hmailserver` on 29 September 2026, keeping its id, 86730559, and the old addresses redirect. The documents, the
  packages' metadata, the Control Panel's links, the policy link of the server's `/.well-known/security.txt` and the
  Service Desk address name the group's path, and a new database gets `HelpWikiBaseUrl`
  `https://gitlab.com/hmailserver/hmailserver/-/wikis/`; one set up before keeps its value, which GitLab redirects.
  From 6.3.6 every release is signed as
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>` with the project's id, which
  every server from 6.3.4 on trusts. The release scripts no longer sign at the first path, which signed 6.3.5 and
  which servers still trust for it ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **The ARM64 Windows installer is in the release.** From 6.3.6 a release carries `hMailServer-<v>-arm64.exe`, the
  installer for Windows on ARM, beside the x64 one: `release` collects it from `windows-packages-arm64`, held to the
  SHA-256 that job wrote; `release-authenticode` signs it with Authenticode by a second call, and stops if the x64
  installer was not signed; `release-sbom` makes its `.cdx.json` from the signed bytes and `linux-sbom`'s ARM64
  component list; `release-sign` signs it with Sigstore, with its `.sigstore.json` copy, and the provenance names it.
  A release is 62 files and 160 links. A server's update check asks for its own processor's installer by name - as
  6.3.4 and 6.3.5 do - so an x64 server is never offered the ARM64 one, and `/download/hMailServer-arm64.exe` on the
  update site leads to it. The rehearsal on a `batch*` branch collects it too. `build/check-release-chain.py` holds
  every one of those places, and refuses a job the release needs that is allowed to fail, in either of the ways
  GitLab lets it. The ARM64 build is cross-compiled on the x64 Windows runner from libraries built into that runner's
  cache, and has never run on ARM64 hardware; the release notes say so
  ([PackagesWindows.md](hmailserver/docs/PackagesWindows.md), [WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md)).
- **The release notes and the documents check the signer's project id, not only its path.** The commands the
  release's verification section, SECURITY.md, ContainerImage.md and the chart's README gave checked the identity
  cosign verifies, which is a path - one that could pass to another project were the group or the project renamed
  or deleted - and not the project's own id, `86730559`, which the certificate carries and a server from 6.3.4 on
  requires at the group's path. Each now reads it as well: OpenSSL prints the certificate's extension
  `1.3.6.1.4.1.57264.1.15` from a file's bundle, or from each signature of the container image
  (`cosign download signature`), and from the source tarball `build/ci/sigstore-certificate.py repository-id`
  reads it as the server does. `container-image-sign` keeps the certificate Fulcio issued for each of the image's
  signatures (`cosign sign --output-certificate`, `container-image-signed/certificate-<name>.pem`) and fails unless
  `build/ci/sigstore-certificate.sh` reads `86730559` from its extensions; an attestation's certificate, which
  `cosign attest` does not write out, is held to the id by the channels' registry mirror, as before.
  `build/check-release-chain.py` holds the job to the check, with three negative controls.
- **The SURBL lookups and the MX-record test cut host names by the Public Suffix List, and `tlds.txt` is gone.** To
  ask a URI blocklist about a link's host, and to judge a sender's host that has no MX by its domain's, the server
  cut the name to two labels, or to three under a name in `tlds.txt` - SURBL's list of 2006, which stated no licence
  and was installed beside the binary on every platform. It now cuts to the registrable domain by the Public Suffix
  List it already compiles in for DMARC, PRIVATE section included, as SURBL's own list of today does:
  `mail.example.act.edu.au` is asked about as `example.act.edu.au`, not the suffix `act.edu.au`; `a.b.c.kawasaki.jp`
  as `b.c.kawasaki.jp`; `tenant.github.io` as itself, where the old cut gave `github.io` alone; and `mail.gov.je` as
  `gov.je`, one of the 134 names of the old list that are not public suffixes today. SURBL is still asked about the
  whole host too, and a host under a PRIVATE suffix about the domain the ICANN section alone gives as well
  (`github.io`: see "A host under a shared-hosting suffix", under Anti-spam). A sender
  at a public suffix itself that has no MX, `github.io` or `co.uk`, fails the MX-record test, since there is no
  registrant's domain above it to fall back to; the old cut failed the first and passed the second. Error 4335, the
  list not read, is retired. The installer deletes `Bin\tlds.txt` on upgrade, and the Linux packages no longer carry
  it. The cut's tests run in ClassTester and, on Linux, as the new `public-suffix-tests` program, in `linux-build`
  and in the Debian and RPM package builds.

- **The PowerShell module is Authenticode-signed in the release, and only that signed module goes to the
  PowerShell Gallery.** Each release from 6.3.6 carries `HmailServer.psd1` and `HmailServer.psm1`, which `release`
  collects from the tagged tree, unsigned, and `release-authenticode` signs by a third call of
  `build/ci/windows-authenticode.ps1 -Module`, made only when both installers signed, through the same Azure
  Artifact Signing profile: each signature must be valid, timestamped and by the publisher, chain and code-signing
  use the installers are held to, and each file the tree's bytes with one signature block appended and nothing
  else, which `release-sbom` checks again before the provenance names the signed bytes and `release-sign` signs
  them with Sigstore (sixty-two files, 160 links). `psgallery-publish` no longer reads the module from the tag's
  tree: it downloads both files and their bundles from the release's package, verifies each bundle with
  `sigstore` (hashed, in a virtual environment of the job's own) as the tag's pipeline, for this repository at the
  tag's commit, and requires each file to be the tag's bytes with one signature block, refusing anything else
  before the Gallery is sent a version it would keep for good. `windows-authenticode.ps1 -Check <file>` holds a
  downloaded installer or module file to the same pin
  ([CommandLinePackages.md](hmailserver/docs/CommandLinePackages.md), "Publishing").

- **The build inputs' Authenticode signatures are checked again, on bench150, before any build uses them.** The
  new protected job `build-inputs-authenticode` (`.gitlab/ci/packages-windows.yml`), which
  `windows-packages-x64`, `windows-packages-arm64` and `windows-installer` need, downloads every build input
  afresh - `build-inputs-1`'s files from the package registry, 7-Zip's archive from 7-zip.org - holds each to its
  pinned SHA-256 and each file the installers take from one to its own, and requires each of those files that
  `third-party-binaries.json` records as signed (one, the SQL Server Compact runtime) to carry a valid, timestamped
  signature by exactly the signer recorded, each recorded unsigned to carry none, and each to have one of those two
  records (`build/ci/build-inputs-authenticode.ps1`). What the GitHub workflow's Windows job did until 18 September
  2026. It then runs `build/ci/authenticode-selftest.ps1`: that check refusing a wrong signer, a wrong or missing
  record and a wrong hash, and `windows-authenticode.ps1`'s pin refusing a throwaway certificate's signature,
  another publisher's and a module file changed in or after signing, the throwaway key deleted after it is used
  ([ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md), "What checks it").

### Anti-spam, anti-virus and mail flow rules

- **A provider can be spared from every auto-ban by its reverse name** (GitLab work item #56; schema 6108,
  `hm_neverbannames`). `AutoBanNeverBan` lists addresses, and a provider whose outbound hosts share a reverse name and
  whose addresses move had nowhere to say "never ban this name". The never-ban list by reverse name holds patterns in
  the domain list's form - `mail.example.net` that name alone, `.example.net` the name and every name beneath it - and
  refuses a public suffix, an address, a network and a `*`. An address is spared from all three auto-bans (failed
  logons, a spam trap, a multi-recipient null sender) - no IP range, no firewall rule, no ban line for fail2ban - when
  one of its reverse names is covered and resolves back to the address (A or AAAA): forward-confirmed, because anybody
  who holds an address can write its reverse name. Nothing is looked up on a session's thread: the SMTP session's own
  reverse lookup feeds the list, and a ban placed before the answer is in is written, resolved on a queue of its own
  with every lookup given up after five seconds, and lifted if the address is spared, before the log line, the alert
  count or the firewall is told; until then every firewall reconciliation leaves its range out. The waiting bans are
  taken by network in turn, an IPv4 `/24` or an IPv6 `/48`, so that bans earned from addresses whose reverse lookups
  hang delay a listed provider's by one lookup for each such network. A waiting ban is announced unverified after
  four attempts that were not answered, a minute apart, or ten minutes in all - a minute it waits without a turn
  counting as an attempt - and is forgotten unannounced when its range goes first. At most 16 of one network wait, a
  seventeenth being announced at once, and at most 1,024 in all: a new ban past that takes the place of the oldest
  waiting ban of the network with the most, which is announced unverified. The failed logons counted before that
  answer - over IMAP, POP3, ManageSieve or REST, which look nothing up until the logon
  that bans - still write their `logon failure` lines, which the packaged fail2ban jail counts on its own: with
  `MaxInvalidLogonAttempts` at five or more, the jail's `maxretry`, fail2ban can ban a listed provider below the
  server. Nothing
  else is skipped, and the list is empty by default. COM (`Settings.AutoBanNeverBanNames`, `AddAutoBanNeverBanName`,
  `RemoveAutoBanNeverBanName`, `CheckAutoBanNeverBanName`), REST (`/api/v1/never-ban-names` and
  `/api/v1/never-ban-names/check`), the Control Panel's Auto-ban page with a check of what the list would decide about
  an address, and the Deck's Lists view; [AutoBan.md](hmailserver/docs/AutoBan.md). The REST check of an address
  that is not public - private, loopback, link-local, carrier NAT, reserved or documentation - is the administrator
  password's alone, since its reverse names are the server's own view of its network: an API key and an account
  holding a role get 403 for one and check a public address, as the breach check's endpoint test reaches one
  ([Roles.md](hmailserver/docs/Roles.md), What manage_settings reaches; the project's second review of this release).
- **Port 25 takes no password: an SMTP listener's AUTH is On, Off or Honeypot** (GitLab work item #54; schema 6108,
  `hm_tcpipports.portsmtpauth`). Port 25 is where other servers deliver mail, and up to 6.3.5 every SMTP listener
  offered `AUTH` there after `EHLO` and checked the password - a spray target on the port every scanner reaches. Each
  SMTP listener now has an SMTP authentication: **On**, as before; **Off**, which does not offer `AUTH`; and
  **Honeypot**, which offers it exactly as On does. On Off and Honeypot an `AUTH` is answered `502 5.5.1 AUTH not
  permitted on this port.` without the account being looked up or the password evaluated - the same answer in the same
  time for every mechanism, user name and password - and the first `AUTH` of a session bans the address for
  `AutoBanMinutes` through the auto-ban's own writer, mirrored to the firewall when `AutoBanFirewall` or
  `AutoBanCommand` is on: one range per address while it lasts, named `Auto-ban: AUTH on a delivery port <address>`,
  however many `AUTH` lines and sessions it sends. Never this computer, an address `AutoBanNeverBan` lists (where an
  incoming relay that sends `AUTH` is named), one the never-ban list by reverse name spares or an authenticated
  session; nothing with `AutoBanMinutes` at 0. One line in the application log names the address, the port, the
  mechanism (as a SASL mechanism name, never what followed it) and the listener's mode; the packaged fail2ban filter
  matches the ban's. Unauthenticated `MAIL FROM` stays inbound delivery. A new installation's port 25 is a Honeypot and
  587 On; an upgraded server's listeners stay On (below). POP3 and IMAP listeners are always On. COM
  (`TCPIPPort.SMTPAuthentication`), REST (`smtp_authentication` on `/api/v1/ports`), the Control Panel's TCP/IP port
  dialog and ports list, the Deck's Ports view, and the setup checklist's *No password on port 25*;
  [SmtpAuthOnDeliveryPorts.md](hmailserver/docs/SmtpAuthOnDeliveryPorts.md).
- **An auto-ban can be widened to its reverse-name neighbours** (GitLab work item #55; `AutoBanWidenByReverseName`,
  off by default). A scanner working through `203.0.113.10`, `.11`, `.12` under one reverse domain earned a ban, a
  firewall rule and a sweep for each. With the switch on, the ban is still written at once for the one address, and is
  then widened in the background, on the name lookup queue, to the contiguous run of addresses around it inside its
  IPv4 `/24` whose reverse names share its registrable domain (the public suffix list's, so
  `mail5.outbound.example.net` is `example.net`): one IP range and one Windows Defender Firewall rule for the run, each
  address of it told to `AutoBanCommand` and the Linux hook, and the before and after in the application log. Only
  when one of the banned address's own reverse names resolves back to it, so a scanner cannot name itself after its
  neighbours; at most 32 lookups each way; never across an address on `AutoBanNeverBan`, one the never-ban list by
  reverse name covers, or one an administrator's IP range tells apart from it or lets in at the ban's priority or
  above; never for IPv6, nor for a network's own address (RFC 1918, RFC 6598, loopback, link-local), whose
  neighbours are its own machines; a lookup not answered leaves the single address. A hook command that runs past
  `ExternalProcessTimeout` is not run again in the same reconciliation, so a run costs one timeout of a hook that hangs,
  not one per address. A ban it was not run for is not asked again, as a failed run never was; an un-ban it was not
  run for, or that the timeout stopped, is owed - with iptables or a command of your own nothing else would lift that
  block - and asked for again by the next reconciliation, then after a wait of a minute doubling to an hour for as
  long as the hook still runs past the timeout. COM
  (`Settings.AutoBanWidenByReverseName`), REST (`auto_ban_widen_by_reverse_name`), the Control Panel's Auto-ban page and
  the Deck's Settings; [AutoBan.md](hmailserver/docs/AutoBan.md).
- **A link's host is the one after its user name.** The SURBL test asked the URI lists about the characters after `://`
  that can be in a name, so `http://anything@evil.example/` was asked about as `anything`, and a spammer escaped every
  list by writing a name and an `@` before the host; a URL entry of the sender lists read the same `anything`. A link is
  now read as RFC 3986 and the browsers read it: the host follows the last `@` before the path, without its port, and is
  percent-decoded (`%65vil.example`); `%40` in the user name is not its end; a bracketed IPv6 address is not asked
  about, since no URI list publishes one, unless it carries an IPv4 address (below); and an IPv4 address in any form a
  browser takes (`0xC0.0.2.1`, `3221225985`) is that address. `http:`, `http:\\` and `http:///` before a host count as
  `http://` does, a backslash ends the host as a slash does, and a link written into another's query
  (`?u=http%3A%2F%2Fx%40evil.example`, `?u=http%3A%5C%5Cevil.example`) is read the same way, however long its run of
  `%2F`, and so is one whose slashes were left as they are (`?u=http%3A//evil.example`, as Python's `quote` and
  JavaScript's `escape` write it). In a text part, where the mail program that shows a link decides where it ends, a
  link it may end before the `@` - Thunderbird ends one at `)`, `"`, `` ` ``, `{` or `}` - is also asked about as the
  name before that character: `http://evil.example)@innocent.example/` is asked about as `innocent.example` and
  `evil.example`. In an HTML part the character references are read as a browser reads them (`x&#64;evil.example`,
  `http&colon;//`), and a quoted attribute's value runs to its closing quote, spaces and a `'` or a `>` in a user name
  included. Punctuation after a link still ends its host; a host with another character in it (`a!b.evil.example`, a
  label in Unicode) is asked about as the name before that character and the labels after its label. Not done: an
  internationalised name is not turned into its `xn--` form, and a host written in fullwidth or other characters a
  browser maps to ASCII is not mapped ([Roadmap3.md](Roadmap3.md), "Internationalised link hosts"); a soft hyphen and
  the other characters a browser deletes are deleted (below). The reading's tests run in ClassTester and, on Linux, in
  `public-suffix-tests`.
- **Characters a browser deletes from a host no longer hide it.** Before it looks a name up, a browser deletes from the
  host the code points UTS 46 ignores - a soft hyphen, a zero-width space, a word joiner, a zero-width no-break space,
  the variation selectors and the rest of the 17 ranges of Unicode's `IdnaMappingTable.txt` 18.0 - and reads its
  percent-encoding as UTF-8, so `http://ev%C2%ADil.example/`, `http://evil<U+200B>.example/` and, in an HTML part,
  `http://ev&shy;il.example/` all go to `evil.example`. The server read a percent-encoded byte past ASCII as U+FFFD and
  deleted nothing, so such a link was no name at all, and a listed name hid behind one character nobody sees. The host
  is now decoded and cleaned as a browser does, in the SURBL test and for the sender lists' URL entries, which are
  stored the same way (`http://sh%C2%ADop.example/` is `shop.example/`); HTML's named references for those characters
  (`&shy;`, `&ZeroWidthSpace;` and twelve more) are read, and a numeric one past U+FFFF (`&#xE0100;`) is the character
  it names, where it was U+FFFD. A zero-width joiner is not one of them: a browser refuses one except after a virama
  (RFC 5892's CONTEXTJ rule, which UTS 46 applies), and no name is read from a host that holds one. In a text part,
  where the mail program that shows a link decides where it ends, a link with a zero-width no-break space in its host is
  also asked about as the name before it: a webmail's script that ends a link at white space by its `\s` ends it there,
  and a browser given the whole link deletes it. So are U+180E and U+200B, which older engines count as white space.
- **A link with no scheme is read.** A browser resolves `<a href="//evil.example/">`, `<img src="\\evil.example/x">` and
  `href="/\evil.example"` against the page they are on, so from a webmail page they go to `evil.example`, and a
  redirector's `?u=//evil.example` (or `?u=%2F%2Fevil.example`) sends its visitor there; none was read. Two slashes or
  backslashes or more are now a link where they open an HTML attribute's value, quoted or not, after the spaces the URL
  parser drops, and where they follow a query parameter's `=`, in a text part as in an HTML part; elsewhere they are a
  path's or a comment's, and nothing is read from them. And in a quoted value the tabs and line breaks the URL parser
  drops are dropped before the slashes after `http:` are skipped: `<a href="http:&#10;//evil.example/">` goes to
  `evil.example`, where the server stopped at the line break and read nothing. A quoted value with a tab or a line break
  anywhere in it is also read without them, as the URL parser reads it, so a link written into its query with one in it
  (`?u=http%3A%2F%2Fev&#9;il.example`) is read as the redirector receives it.
- **A link to an IPv4-mapped IPv6 address is asked about as its IPv4 address.** `http://[::ffff:192.0.2.1]/`, in any
  of the forms the WHATWG IPv6 parser takes (`[::FFFF:C000:201]`), is 192.0.2.1 to a dual-stack system, and
  `http://[64:ff9b::192.0.2.1]/`, under the NAT64 well-known prefix of RFC 6052, is 192.0.2.1 through a translator;
  no bracketed host was asked about, so a listed address escaped every list in brackets. Both are now asked about as
  the IPv4 address, reversed, and a sender list's URL entry for one of them is stored as that address. Any other IPv6
  address, the IPv4-compatible `[::192.0.2.1]` among them, is still not asked about.
- **A host under a shared-hosting suffix is also asked about as the domain the ICANN section registers.** The Public
  Suffix List's cut (under Downloads and the release) asks about `tenant.github.io` as itself, since `github.io` is a
  suffix of the list's PRIVATE section. A list that is wildcarded, as SURBL's multi and the Spamhaus DBL are, answers
  for `github.io` when it is asked about the host; one that is not, as URIBL is not, answers only for the names it
  lists, and a whole hosting domain it lists - `duckdns.org`, say - is listed under that name alone. So a link to
  `evil.duckdns.org` escaped such a listing, which 6.3.5, cutting at two labels, caught. A host under a PRIVATE suffix
  is now also asked about as its registrable domain by the list's ICANN section alone - `duckdns.org`, `blogspot.com`
  for `a.b.blogspot.com` - which the generated `PublicSuffixListData.h` makes possible with the PRIVATE section's rules
  in arrays of their own; DMARC and the MX-record test read both sections as before. The header was regenerated from
  Debian's copy of the list of 25 July 2026, whose rules are byte for byte those compiled in until now.
- **The sender lists' URL entries see every link, and a message is read for them once.** The URL entries were matched
  against the first 16 hosts of a message, the SURBL test's cap, so a message that put the link an entry blocks after
  sixteen others got past it; and the message was parsed and its text read for each recipient with a URL entry, up to
  the MIME parser's 80 MB each time, about 30 seconds per recipient for a crafted message. Every link is now matched,
  and the text is read once for the message against the entries of every scope, however many recipients have one:
  each link is looked up by the names an entry for it could have, so the reading costs one pass over the text and no
  more for more entries, and it keeps one record for each entry host a link is under, whatever the message holds. A
  recipient reached after the list was read again from the database has the links read again. A message larger than
  the anti-spam size limit is read only as far as that limit into its text and into its HTML, so a link after it is
  not matched (What changes on upgrade, item 21; [SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
- **A link to an address is asked about reversed, and only so.** It was asked about in its own order too, which is
  the address the other way round: a link to `192.0.2.1` was tagged when a list had `1.2.0.192`, and a sender
  list's URL entry for `1.2.0.192` blocked or allowed it.
- **A host DNS cannot carry no longer ends the SURBL test.** A host went to the resolver as the message wrote it,
  and the names were asked about in sorted order, so a link at the end of a sentence (`http://example.com.`),
  `http://../` or a label of 64 characters made a name the resolver refuses, and the failed lookup abandoned the
  list for the message with every link sorted after it unchecked. Such a host is not asked about, a name that with
  the list's zone passes 253 characters is not asked, and the links are asked about in the order they come.
- **The URI lists' error codes are never a listing, whatever the expected result.** The Spamhaus DBL answers every
  address with `127.0.1.255`, "IP queries prohibited", and the Spamhaus ZRD with `127.0.2.255`, "IP queries not
  supported", so a server that asked either with no expected result, or with `127.0.1.*` or `127.0.2.*`, tagged
  every message holding a link to an address. SURBL and URIBL answer `127.0.0.1` to a query they block, which tagged
  every message holding a link on a server they block. Those, and Spamhaus's `127.255.255.0/24`, which counted only
  when an expected result took it in, are now errors under any expected result, logged at debug level once for their
  lookup. A list that refuses the server's queries with `127.255.255.x` is not asked again for that message, and the
  application log says so at most every 15 minutes, as for a lookup that fails. `127.0.0.1` does not end the list
  for the message, since Hostkarma answers it for a name on its whitelist and a link to one would switch the list
  off for the links after it; the application log says so, at most every 15 minutes as well, when a list answered it
  to every name of a message.

- **Fixed: a URL entry of the sender lists with an `@` or a `\` in it could name another host.** An entry typed as a
  link was cut at its first `@`, after a leading `www.` had been looked for, so `http://a@b@bad.example/` was stored
  as `b@bad.example/` and `http://user@www.bad.example/` as `www.bad.example/`, neither of which is any link's host,
  and the backslash of `http://good.example\@evil.example/`, which a browser reads as a slash, was not seen, so the
  entry named `evil.example` where the link goes to `good.example`. An entry's host is now read as the SURBL test
  reads a link's (above): the part after the last `@`, up to a port, `/`, `?`, `#` or `\`, percent-decoded, each
  leading `www.` dropped while two labels or more are left (`http://www.www.example.com/` is `example.com/`), and an
  address in any form a browser takes (`0xC0.0.2.61`) written in dotted decimal. An entry whose host no link can have
  - neither an IPv4 address nor a name of two labels or more, of letters, digits, `-` and `_`, within the lengths DNS
  allows and not ending in a number - is refused: an IPv6 address and a name of one label were refused already, and
  the rest (a name with a `!` in it, an empty label, a label of 64 characters) were stored and matched nothing.
  Entries stored before are read the new way when the server loads them, which mends the first two forms; one stored
  from the third names `evil.example` and has to be entered again, and one of a host now refused stays in its list,
  matching nothing as it did. The cases run in `SURBLTester`, in ClassTester and, on Linux, in `public-suffix-tests`.
- **Fixed: a filtering engine's score that is no number could count as the kindest verdict.** The filter hook
  (`FilterHookUrl`) rounds the score an engine answers with to the whole number the spam framework counts in, and for
  a score that is not a number, an infinity or past the range of that number - `NaN`, `Infinity` and `-Infinity` as
  Python's JSON writer spells them, `1e999`, `1e10` - the rounding was undefined: on x64 it gave the lowest score
  there is, so a message the engine had scored as far past every threshold as a score goes was delivered as clean.
  Since 6.2.23 Alpha 1, which brought the filter hook. A score from -1,000,000 to 1,000,000 is taken as it is; any
  other that reads as a number now counts as 1,000,000, the most spam there is, and text that is no number at all is
  still no score. `AntiSpam/FilterHook.cs` holds it to that, on Windows and in the Linux suite.
- **Fixed: a Sieve `size` test whose number and quantifier came to more than 8 EiB answered the wrong way round.** The
  number was multiplied by its `K`, `M` or `G` in 64 bits and wrapped below zero, so `size :over 9999999999G` was
  true of every message and `size :under 9999999999G` of none. The product is now held at the largest 64-bit number,
  as a number written without a quantifier already was. Since 6.2.3, which brought the Sieve interpreter.
  `Sieve/SieveEvaluation.cs` holds it to that.
- **A HELO name or a sender's domain that is not a name can be refused**
  ([work item #51](https://gitlab.com/hmailserver/hmailserver/-/work_items/51)). Two settings, both off by default, so
  nothing changes until one is switched on: `RefuseHeloNotFqdn` answers `550 5.7.1` at `MAIL FROM` to a session whose
  HELO/EHLO name is not a fully qualified domain name, and `RefuseMailFromDomainNotFqdn` to one whose envelope sender's
  domain is not - an address with or without brackets (`192.0.2.7`, `[192.0.2.7]`, an IPv6 literal), a single label
  (`localhost`), a public suffix (`co.uk`, `com`) or anything that is not a host name, while `mail.co.uk` is a name.
  No DNS is asked; the suffixes are the Public Suffix List's, the list the quarantine review groups by. At `MAIL FROM`
  rather than at `HELO`, so AUTH has had its chance; `MAIL FROM:<>` has no domain to judge and is no excuse for a bad
  HELO. Never judged: an authenticated session, an IP range that does not run the spam tests, a sender on the
  anti-spam white list, and an incoming relay, whose HELO is the gateway's own. One log line names the HELO, the
  envelope and the peer. On `Settings.AntiSpam` over COM, `PUT /api/v1/settings/antispam` (`refuse_helo_not_fqdn`,
  `refuse_mail_from_domain_not_fqdn`), the Control Panel's Anti-spam page under Host checks in every language, and
  the Deck's settings; rows of the settings table, so no schema step. docs/SmtpPolicy.md says what a name is here;
  `AntiSpam/FqdnRefusal.cs` and `API/RestApiFqdnRefusal.cs` hold the sessions and the settings to it, and
  `FqdnCheckTester` every rule, in the class tests and in the Linux `public-suffix-tests`.

### Administration

- **Text and values in the Control Panel can be selected and copied (work item 48).** Every page and dialogue: drag
  over text, double-click a word, Ctrl+C, or right-click for Copy / Select all; with nothing selected, Copy takes the
  whole text, and a selection dragged out into another program carries plain text only. Text inside buttons, lists,
  tabs, menus, grids and editors is left to its control, so a press there stays a press, and keyboard navigation and
  screen-reader output are unchanged. The words of the controls themselves are copied whole: a right-click on a check
  box, a radio button, a button or a toggle, a drop-down, a tab, a page in the navigation tree or a label offers Copy,
  which copies the caption as it is shown - a drop-down's value as it shows it, without an access key's underscore or
  an icon - and Ctrl+C copies the same on the one that has the keyboard, unless a data grid or a list around it would
  copy its chosen rows, which it still does. A left click and every key those controls take do what they did, an
  icon-only button offers no menu, and a control with a menu of its own keeps it; a drop-down's right-click menu,
  which offered Copy, Cut and Paste and could run none of them, now offers Copy. Most of Logging's and API &
  monitoring's settings are check boxes, whose captions could not be copied before. Every plain list - domains,
  accounts, aliases and distribution lists, public folders, incoming relays, an account's IMAP folders, folder
  permissions, group members, recipients, a route's addresses and the Active Directory picker - copies its selected
  rows with Ctrl+C or its right-click Copy, one row per line in the list's order, and offers Select all where several
  rows can be chosen; the Ctrl+K search's results copy from their right-click Copy (its search box keeps the
  keyboard); data grids copy their rows as before. Live logs selects several lines (click, Shift+click, Ctrl+A) and
  copies them exactly, in the log's order; it follows new lines only while the end is in view and no longer splits a
  line the server is still writing into two rows. On REST API keys, a key's label, id, state and scope can be copied.
  Every Copy button now writes the clipboard the same way: a program that reads the clipboard the moment it changes
  (Windows' clipboard history, a remote-desktop client) no longer makes a copy fail after a second's freeze (a chart's
  Copy then showed no confirmation, and the new API key's and app password's Copy an error), and text copied just
  before the Control Panel is closed still pastes after it has gone. Text copied from the help pane - by Ctrl+C, the
  menu's Copy or a selection dragged out - is plain text, written the same way as every other Copy, where it put RTF
  and XAML on the clipboard beside the text, in the theme's colours; and its right-click menu is Copy / Select all in
  the Control Panel's language, where it was the document viewer's, in English (measured in German and Swedish).
- **Where the keyboard is shows in one violet, in the Control Panel, the Control Deck and the webmail.** A text field
  that has the keyboard - clicked or tabbed into alike - takes a focus colour for its border and a soft glow of it
  outside, the look of Firefox's address bar: in the Control Panel every text box, password box, number box, combo box
  and date picker with a border round it, the search boxes among them and the fields of every dialog; in the Control
  Deck and the webmail every input, drop-down and text area with a box of its own and the search boxes, and in the
  pages the server serves on its own - the password-protected message, account recovery, a confidential message, a
  shared file's password, a list's subscription page. A field drawn as a line - the Control Panel's search palette,
  the webmail's compose window and command palette - takes the colour in its line. The ring the keyboard leaves on a
  button, a link or a row takes the same colour in the Deck and the webmail, as the navigation's keyboard border does
  in the Control Panel; on the compose window's dark header it is the dark theme's violet, and inside a calendar event
  filled with its calendar's colour it keeps a band of the page between the two. The violet is #7542E5 on the light
  themes and #B89CFF on the dark ones, and each clears 3:1 against every surface a field sits on and beside its glow
  at the glow's strongest (WCAG 2.2, non-text contrast). The glow is drawn outside the field and moves nothing; under
  Windows High Contrast and a browser's forced colours the system's own colours stay, the web pages giving a field an
  outline in the system's highlight; the brief fade-in stops when animations are turned off. Tested by
  `DesignTokensTests` (the contrast, High Contrast, the web pages' copies of the colours) and by `deck-script-test.js`
  and `portal-script-test.js` (each page's tokens and rules).
- **Fixed: the webmail's Sign in button was wider than its fields.** On the sign-in card the button ran 32 pixels
  past the right edge of the E-mail address and Password fields, and *Send the link* past its field on the reset card,
  in every browser and both themes: the buttons' `all: unset` put back `box-sizing: content-box`, so a width of 100%
  had their padding added to it. They are as wide as the fields now (measured in headless Chromium and Firefox, light
  and dark), and `build/check-portal-script.py` refuses a control reset with `all: unset` that is given a width of
  100% without saying `box-sizing: border-box` again. The Deck's sign-in card and the password-protected message's
  page were not affected.
- **Fixed: words of the Control Panel that stayed English, or lost a space, in its seventeen other languages.** The
  app password's "Copied.", a list editor's "Deleted.", directory synchronisation's "Applying...", the "in" before
  each page's place in the Ctrl+K search's results, a dashboard chart's time axis ("last 3 minutes") and what its
  legend says of each line to a screen reader and in a tool tip, the LDAP page's "Could not write" and the anti-virus
  overview's "and" between the two notices were English in every language: the localisation check took one word
  ending a sentence for a host name and did not look at a piece of a sentence joined to others, and it now reports
  both. 290 translations in sixteen languages had lost the space at their end that the program joins the next
  words to, so a Danish copy that failed said "Kunne ikke kopiere:" with the reason run on, and a status line ran
  two sentences together; the spaces are back, and the check holds every translation to the white space at the
  edges of its English (Japanese and Chinese may drop it after a character of their own script). Two Finnish
  counts had a space too many, and four sentences were split around a value with their end left in the source,
  where no translator could move it: an account's renamed address told German "Die Adresse muss ein Name gefolgt
  von @ seinexample.com.", and a group's name opened with « in French and Russian and closed with an English
  quote. The four are whole sentences now, and the check reports a space a translation adds and a sentence
  split that way. A dashboard chart's "collecting" said "fetching" in six languages. And the words WPF puts on
  the screen itself - Copy, Cut and Paste on a text box's right-click menu, a scroll bar's right-click menu - were
  English in Danish, Finnish, Norwegian, Dutch, Swedish and Ukrainian, which .NET has no translation for, and
  WPF's German scroll bar menu named both Top and Page Up "Nach oben"; in the Control Panel's seventeen languages
  they are now its own words, and in English and in a language it has no catalogue for they stay WPF's. The two
  buttons that page a date picker's calendar were "Previous" and "Next" to a screen reader in every language and
  every view, the theme's English; they now say what they page as the calendar shows months, years or decades
  ("Previous month", "Next decade"), in the Control Panel's language.
- **Fixed: the Control Panel window's own buttons had no name for a screen reader.** Minimise, Maximise (Restore
  down while the window is maximised) and Close in the title bar were each read as just "button", in every
  language; they now carry their names, in the Control Panel's language. So do the chevron that folds each of the
  navigation's nine groups ("Collapse Maintenance", "Expand Maintenance") and a password box's eye ("Show password",
  "Hide password"), each as its state changes; both were nameless in every version of the Control Panel. A chart's
  time axis says "last second" where it said "last 1 seconds".
- **Fixed: a switch put back from the audit trail stayed as it was.** Putting a setting back from its audit row
  (`POST /api/v1/audit/{id}/revert`, `Utilities.RevertAuditedChange`, *Put back* in the Deck and the Control Panel)
  wrote a switch's old value, `true` or `false`, into the text column of `hm_settings`, which the server does not
  read, and answered that it was put back. A setting now goes back into the column the server reads it from - a
  switch as a switch, recorded as `false -> true` - and a text whose values hold ` -> ` is put back whole, where it
  was cut at its first arrow, while its row is the setting's latest change in the trail, or refused when the row does
  not say where the old value ends. A switch put back by 6.3.4 or 6.3.5 keeps a `true` or `false` in its unread text
  column, which does nothing; one put back twice by those versions left a row `true -> false` or `false -> true`
  describing that text column, and putting that row back now sets the switch. `Utilities.RevertAuditedChange`'s help
  text in the type library says so too.

- **Fixed: `Utilities.IsStrongPassword` knew three of its special characters wrongly on Windows.** A password of
  five or six characters counts as strong only with a digit or one of a set of special characters, and three of
  them - the currency sign, the diaeresis and the acute accent (U+00A4, U+00A8, U+00B4), upstream's, from a Swedish
  keyboard - had been written over with U+FFFD when the source was re-encoded in 2014. The Windows build, which
  reads that source in the ANSI code page, took each U+FFFD for the three characters its UTF-8 bytes are in
  Windows-1252 (U+00EF, U+00BF, U+00BD), so a five-character password ending in a currency sign was weak and one
  ending in an i diaeresis strong; the Linux build took U+FFFD itself. The three are back, written as `\u` escapes,
  which every compiler reads alike. Nothing in the server calls the method, which answers COM scripts only; a
  password an administrator chooses is judged by the password policy. The passwords the server generates, twelve
  hexadecimal digits, are longer than the rule looks at.
- **The audit trail's put-back names every refusal.** `POST /api/v1/audit/{id}/revert`'s description in the OpenAPI
  document, `Utilities.RevertAuditedChange`'s comment in the type library's source and the audit trail's help page
  listed the refusals of 29 September. They now also name, where each was missing, a setting's row whose detail does
  not hold the two values, a `[Settings]` value the store cannot hold, a write that fails, and the two refused with
  the sentence every other way of changing them is refused with: one of the five `[Settings]` keys of the live
  update's trust, which only `hMailServer.INI` itself changes since 6.3.6, and a setting the server's environment
  sets - each unless the value put back is the one it has already. The help string in the type library is
  unchanged: it says that the call raises why it cannot, within the 260 characters MIDL keeps.
- **Fixed: an account saved through the object COM's `Authenticate` hands out could lose its retention.**
  `Application.Authenticate` and `AuthenticateWithCode` hand the caller a copy of the account signed in, and a
  webmail's password or out-of-office plugin changes that copy and calls `Save()`. From 6.2.25, which brought the
  account's `MessageRetentionDays`, to 6.3.5 the copy was made without it: the copy's `MessageRetentionDays` held
  whatever its memory happened to hold, and the save wrote that over the account's own. So an account's retention -
  a number of days, -1 to keep its mail for ever, or 0 to follow its domain - could become another number once its
  holder changed a password or an out-of-office reply through such a plugin, and the retention sweep, every six
  hours, then deleted the account's delivered mail older than the number the save left, or kept it, or followed
  the domain, where the account had said otherwise. Windows only: the Linux server has no COM. The copy now
  carries every field of the account, the mailbox key wrap a password change prepares on it among them. After
  upgrading, check the retention of the accounts whose holders use such a plugin (the account's page in the Control
  Panel, COM `Account.MessageRetentionDays`, REST `message_retention_days`). `AccountTester`, which the self-test
  and the Linux program `business-object-tests` run, copies an account with every field set and compares each
  one, and `MessageRetentionTests.TheAccountAuthenticateHandsOutKeepsItsRetentionThroughASave` saves the account
  COM hands out and reads the retention back.
- **Fixed: `POST /api/v1/ipranges` took priorities its update refuses.** Since 6.2.25 the create read `priority` as
  any run of digits, with `atol`, where `PUT /api/v1/ipranges/{id}` takes a whole number from -2,000,000,000 to
  2,000,000,000: a number beyond that was stored as given while a 32-bit integer holds it, and past that Windows
  stored the integer's bound and Linux handed the database a value its column cannot hold; a fraction was cut at its
  point; a priority written as a string (`"5"`) was passed over, and the range made with priority 0; and the digits
  were looked for after the first `"priority"` anywhere in the body, so a range named `priority` could be made with 0
  whatever its priority said. The create now finds the member by name, as it finds the range's name and addresses,
  takes what the update takes, and answers anything else with 400 "priority must be a whole number between
  -2000000000 and 2000000000", making no range; a body without a priority, or with `null` there, still makes one of
  priority 0. The OpenAPI document says so. Tested by `RestApiCoverage.IpRangesRoundTrip` and
  `business-object-tests`.
- **The audit trail records somebody's mail read through an administrator's route.** An account's folder tree, a
  folder's permissions, every message, a folder's messages, a message's header fields and its source under
  `/api/v1/accounts/{address}`, a backup's listing of a mailbox, a mailbox exported to the server's disk or downloaded
  as `export.zip`, a held message's headers and text, the archive index searched or one of its entries read, and a
  page of an eDiscovery search's hits, are each a row - object type `account` and action `mailbox-read`,
  `quarantined message` and `read`, `archive` and `searched`, `archive entry` and `read`, or `discovery search` and
  `read` - under the credential that read them and the address it came from; a support session opened as an account
  is a row, `support-session-opened`. A row names a message or a folder by its id, a held message or an archive entry
  by its sender and recipients, an archive search by what was asked for and an eDiscovery search by its id and its
  case, never by a subject or a folder's name. The same read by the same credential from the same address is one row
  a minute, so that the Deck's *Shared folders* page and a client paging a listing do not bury the trail; another
  message, folder or credential is a row at once, and an export or a support session is a row every time - an export
  that stopped part of the way too, with how far it got. A read the server refuses is not recorded. A change made to
  the mailbox through the same routes is a row too, action `mailbox-changed`, every time it is made: a message added,
  its flags set (by what the request set them to), copied or deleted, by their ids, and every message deleted; only a
  request that found the flags already as it asked, and so changed nothing, is held to one row a minute. The rows of
  `hm_messages` are traffic, so nothing recorded those either. And a mailbox the IMAP master user opens - `AUTHENTICATE
  PLAIN` naming the account, with the master user's own password - is a `mailbox-read` row under the master user,
  named `account:<its address>` as REST names an account, interface `IMAP`, once a minute for the same master, mailbox
  and address; the master user signing in as itself is not. A read or a change over COM is not recorded, nor what a
  support session or the master user's session does once open, which the application log holds request by request
  ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md), "Mail read through an administrator's route").

- **`-Insecure` works on Windows PowerShell 5.1, and `hmctl --version` says the release's version.** The
  `HmailServer` module's `-Insecure`, which accepts the self-signed certificate of a server on the loopback, had no
  effect on Windows PowerShell 5.1, which has no `-SkipCertificateCheck`. There it now puts a certificate check in the
  process's place for that call alone, one that accepts the certificate of the session's loopback address and port
  and hands every other connection to the check it replaced, and puts that back when the call ends; on PowerShell 7 an
  `-Insecure` call follows no redirect, since the skipped check would follow it. `Invoke-HmApi` refuses `-Insecure`
  for a host other than the loopback as `Connect-HmServer` does. Against a loopback server with a self-signed
  certificate, on 5.1 and 7.5, 14 of 14 cases pass, where the module as it was failed three on 5.1 and two on 7.
  `hmctl --version` said `hmctl 1.0`, and its requests `hmctl/1.0`, whatever the release; it says the server's
  version now, stamped with the release (RELEASE.md, step 7), and `build/check-cli-packages.py` fails `checkers` while
  `hmctl`'s `VERSION` is not `Version.h`'s, with a `--self-test` case the check as it was passed.
- **Fixed: the Control Panel's API keys page took a source restriction the server cannot read.** It checked each
  address with .NET's `IPAddress.TryParse`, which reads `10.1`, `010.0.0.1` (octal, as 8.0.0.1) and `[::1]` as
  addresses; the server does not, on Windows or Linux, since 6.3.6 (What changes on upgrade, item 42), and refuses a
  key whose restriction it cannot read from every address. The page reads an address as the server does now
  (`Services/ServerAddressText.cs`, the server's `IPAddress::Parse` line for line) and refuses such a restriction
  with the sentence it gives any other.

### Operations

- **Fixed: upgrading a server whose data folder is a junction to another drive no longer fails granting the service
  account.** This affects an installation of 6.3.5 (or a 6.3.4 one already moved to `NT SERVICE\hMailServer`) whose
  `Data`, `Logs` or `Database` folder, or a folder above them, an administrator had moved to another folder or drive
  behind a junction or a volume mount point - an ordinary layout. Setup runs `hMailServer.exe /GrantServiceAccess`
  again on every upgrade of such a service, and the grant refused any junction, symbolic link or mount point on the
  path it granted on (exit 1), so Setup failed the upgrade. The grant now goes through a link in the path when the
  administrators alone control it - owned by `BUILTIN\Administrators`, `SYSTEM` or `TrustedInstaller`, and nobody else
  may change it or remove it from the folder it is in - as the store commands have gone through such a link since
  6.3.6 (`OnlyAdministratorsControl`, [ServiceAccount.md](hmailserver/docs/ServiceAccount.md), "Never granted"):
  the path the link holds is held from that volume's root - every link on it asked about in its turn, so a junction
  the service's account put where an administrator's link leads is refused, naming that account - must be on a fixed
  local drive, and is held to the same refusals as a folder named directly, and the grant is written on the folder
  the link leads to by its volume's name, so that what it inherits is its own folder's.
  Any other link is still refused, the message now naming who may change it or who owns it. `icacls <link>
  /setowner *S-1-5-32-544 /L` and `icacls <link> /inheritance:r /grant:r *S-1-5-32-544:(F) *S-1-5-18:(F)
  *S-1-5-32-545:(RX) /L` give a link to the administrators alone.

- **Fixed: on MySQL and MariaDB an insert refused as a duplicate could be read as one that wrote a row.** A caller
  that ignores a duplicate - two nodes recording one complaint report or DMARC or TLS report, a greylisting triplet
  written by two connections at once - tells a written row from a refused one by the id it is handed back. After a
  refused statement `MySQLConnection` asked the client library for the insert id anyway, which MySQL documents as
  undefined after an error and which libmariadb answers with the id of the last row the pooled connection wrote, in
  whatever table (measured against MariaDB 11: a duplicate refused with 1062 after an insert into another table read
  as that table's id), so a duplicate report was recorded as new. It hands back none after a failure it was told to
  ignore, as the other three backends do.
- **Fixed: the lines `[Database] UpgradeOnStart` writes at the service's start were dropped.** Upgrading.md promises
  a line in the application log for each step; they were written before the service had read its settings, when its
  logger writes nothing. They are kept and written once the settings are loaded, and so are the counts schema 6107's
  prerequisite reports when DBUpdater runs it over COM, which DBUpdater's reinitialise at the end of the upgrade now
  writes ([Greylisting.md](hmailserver/docs/Greylisting.md), "What the upgrade to 6107 does").
- **Fixed: on Linux, some of the server's network waits had no timeout.** The MTA-STS policy fetch, the DANE TLSA
  query, the OAuth2 token request (`OutboundOAuth2TokenUrl`), the ACME client and its http-01 responder, and the
  ManageSieve and metrics listeners set their socket timeouts in the form Windows takes, which Linux refuses, and the
  three HTTPS requests among them read with calls that on Linux wait without a timeout in any case. So on Linux a
  wait on a peer that stopped answering lasted for as long as the connection stayed open. This was so in every
  release with Linux packages, 6.3.0 to 6.3.5 (CVE-2026-103649); Windows honoured the timeouts, each of which bounded
  one read. In 6.3.0 to 6.3.2 the server's own DNS resolver, through which the Linux build's lookups go, set its query
  timeouts in the same form, so a query a name server did not answer held the lookup thread for ever; 6.3.3 corrected
  that, and its notes did not say so. The three HTTPS requests now go through the server's one HTTPS client, whose
  deadline bounds each request whole on both platforms - 15 seconds for the MTA-STS policy, 20 for the token, 30 for an
  ACME request - and the others take their timeouts in the form each platform takes: 5 seconds for the TLSA query, the
  http-01 responder and the metrics listener, 30 for ManageSieve. What this changes besides is in What changes on
  upgrade, item 16. A new tester program the Linux build runs, `network-deadline-tests`, holds the token and ACME
  requests, the TLSA query and the http-01 responder to their deadlines against a peer on 127.0.0.1 that never answers,
  and the policy fetch to its own through a proxy of its own that never answers, and fails against the code as it was;
  `HealthProbes`, `ManageSieve` and `OutboundXOAuth2` in the regression suite have a case each.

- **The ACME client reads a header that comes last.** `AcmeClient` looked a response header up in the block cut
  at the blank line and then wanted a line break after its value, so a `Replay-Nonce` or `Location` that a CA sent
  as the last header was read as absent. HTTP gives the order of fields no meaning (RFC 9110, section 5.3). A
  `Replay-Nonce` sent last left the client without a nonce, and every certificate request stopped at its first
  step; a `Location` sent last left the account's and the order's URLs empty. The last header now runs to the end
  of the block, as the migration client's lookup already had it, and the self-test and `network-deadline-tests`
  pin it. Found in the SAST triage of 6.3.6, in the function of a cppcheck finding (AcmeClient.cpp:208) that was
  itself a false positive.

- **Why the container image waits, said truly.** `.gitlab/ci/container-image.yml`,
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md) ("What the owner sets on the registry", 3),
  Roadmap2's release-chain row, the packaging README and a comment of `build/check-release-chain.py` said, and
  6.3.4's notes below say, that the image waits because GitLab.com cannot transfer a project whose container
  registry holds an image. GitLab's documentation does not say that: on GitLab.com a project whose registry is
  enabled is transferred only within its top-level group, one with more than 1,000 container repositories not at
  all, and a project must hold no image only on GitLab Self-Managed. The image waits because the maintainers decided
  on 27 September 2026 to move the project into the group `hmailserver` before the first image is pushed, so that
  its address, `registry.gitlab.com/hmailserver/hmailserver`, is the lasting one - a container repository's path is
  always its project's - and 6.3.4's `SigstoreVerifier` already trusts the group's path with the project's id,
  86730559. The texts that said the image appears at `registry.gitlab.com/progressiverobot/hmailserver` - the
  comments of the Compose file, the chart's values and the Dockerfile, the packaging README, EvidencePack.md,
  ContinuousIntegration.md's `cosign verify` example and SECURITY.md's `cosign verify-attestation` example, and
  `renovate.json`, which also said the registry was not switched on - now name the group's address; the Compose file
  and the chart pull from it since the move. Nothing the pipeline does changes.

- **`HM_DB_WAIT_SECONDS`** (0 to 999999, default 0): the image's entrypoint waits for the `[Database]` server to
  accept a TCP connection before opening it, and does not start (exit 69) when it has not. The entrypoint reads the
  ini's `[Database]` section as the server does (spaces trimmed, names in any case), and `HM_INSTALL_KIND`, which
  the image sets for the update check, is not a reason for it to open the database.

- **A build without the live update, and a live update that knows a snap and a container**.
  CMake's new `-DHM_LIVE_UPDATE=OFF` builds the server with no apply step and installs neither
  `hmailserver-update` nor its two units; an AppImage made from an OFF tree carries no `hmailserver-update`
  either. Its update check still names a newer release and the distribution's own command
  (`-DHM_UPDATE_COMMAND="apt upgrade hmailserver"`). A server in a snap (`SNAP` naming the directory it runs from)
  or in the container image (`HM_INSTALL_KIND=container`) names `snap refresh hmailserver` or the image to pull,
  and never downloads or installs a release file. `GET /api/v1/update` reports `installKind`, `liveUpdate`,
  `updateCommand` and `updateAdvice`, and the Control Deck shows them. What the live update did in images before
  6.3.6 is written down in [LiveUpdateKinds.md](hmailserver/docs/LiveUpdateKinds.md): it installed nothing,
  downloaded two packages and reported HM6890. Tested by `build/test-install-kind.sh` (CI job
  `linux-live-update`).

- **A new Windows installation can be set up against SQL Server, PostgreSQL or MySQL with no window** (not yet
  run against a server: that is the installer smoke test's `--database` run, made by hand, not by a pipeline).
  Both installers take the database on the command line - `/dbtype=`, `/dbhost=`,
  `/dbport=`, `/dbname=`, `/dbuser=`, `/dbpasswordfile=`, `/createdatabase=`, `/dbservice=` - or in the
  container's own `HM_DB_*` variables, the password only in `HM_DB_PASSWORD` or a file, never on a command line.
  `DBSetup.exe /unattended` then creates the database, or uses one that exists and upgrades it with DBUpdater if
  it is older, and ends only at the schema the build requires, its lines in the setup log. Settings it cannot use
  are refused before anything changes, among them a password file that is not UTF-8, a control character, and a host
  or user wrapped in quotation marks, which `hMailServer.ini` would take off; a quote, a backslash or a semicolon in
  a password is passed on as it is (below, "Database passwords"). This is how a silent installation of the ARM64
  build, which has no built-in database, finishes; its installer now refuses a silent new installation given no
  database before anything is installed, rather than open DBSetup's wizard where nobody can answer it. On x64 it is
  an option beside the built-in database, which stays the silent default unless `HM_DB_TYPE` is set in the machine's
  or the user's environment, which makes a new installation an external one; the setup log says whether the type came
  from `/dbtype` or the environment. The x64 installer still does not run on ARM64 under emulation (`x64compatible`),
  by the maintainers' decision of 28 September 2026, which WindowsOnArm.md records with its reasons. The installer
  smoke test checks the path with `-DatabaseSettings` (`installer-smoke-run.sh --database`).

- **A new installation on SQL Server with Windows authentication serves mail as `NT SERVICE\hMailServer`, or says what
  to run.** The database step creates the database as LocalSystem, and the service is then moved to the virtual
  account, which had no login on SQL Server and no user in the new database: on the installer smoke VM on 2 October
  2026, with SQL Server 2025 Express, the server could not open its database (HM5028, HM4354), listened on no port,
  and Setup exited 0 without a word. Before the move the installer now runs `hMailServer.exe --grant-database-login`,
  a new verb that, as the administrator running Setup, gives `NT SERVICE\hMailServer` a login, a user in the database
  and `db_owner` on a SQL Server on this computer, leaves whatever is there, and leaves alone a SQL Server on another
  computer, where the virtual account is this computer's account as LocalSystem is. It makes no user and changes no
  role in a database with DDL triggers of its own, which would run with the administrator's rights on SQL Server,
  server-wide, whoever left them there. Where it cannot, on a database Setup has just created, the service is not
  moved: it stays on LocalSystem and serves mail, the message gives the four statements a SQL Server administrator
  runs and the `sqlcmd` line that runs them, and Setup exits **10**. Every service Setup moves is then held to opening
  its database as the virtual account - its version read through it - and one that does not start or does not open it
  is moved back to LocalSystem and started again, with the same message and exit 10 (11 once the database step changed
  the database). DBSetup, finishing an installation (below), does the same.
  [ServiceAccount.md](hmailserver/docs/ServiceAccount.md), "Its login on SQL Server", and
  [WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md), "SQL Server with Windows authentication".

- **A new installation whose database step did not complete is finished as a new one, by Setup run again or by
  DBSetup.** The message after a refused or failed database step offers both, and until 6.3.6 neither ended where a
  new installation does (the installer smoke VM, 2 October 2026): Setup run again kept the LocalSystem service it had
  registered for the step, and `DBSetup.exe` set the database up and nothing else - no self-signed certificate (the
  database kept its `NewInstallDefaultsPending` mark), no stored-secrets key, LocalSystem. Setup now leaves a mark,
  `NewInstallationPending` (the account chosen) and `NewInstallationProfile` under
  `HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer`, and takes a LocalSystem service beside a settings file that names no
  database - or the mark - for a new installation's: run again, it offers and by default gives
  `NT SERVICE\hMailServer`, and the secure defaults, the profile and the stored-secrets key follow the database as for
  any new installation. `DBSetup.exe`, its wizard or `/unattended`, run as an administrator with the mark there, runs
  those steps itself after the database, in the installer's order and with its verbs, the move and its check included;
  the wizard asks Windows for elevation, `/unattended` without it refuses before changing anything (exit 2), and a
  step it could not finish is exit 4. Setup run again takes the account and the profile the mark holds as its
  defaults, as DBSetup does. Setup clears the mark before its own database step and the uninstaller removes it; an
  update `hMailServer.Updater` runs leaves it alone and upgrades the installation as it is, keeping its account, and a
  database the settings file names is held to its version through the step whatever the mark says, so that a step that
  changed it ends in 11, not 10. [ServiceAccount.md](hmailserver/docs/ServiceAccount.md), "A new installation whose
  database step did not complete"; DBSetup's `NewInstallationTests` hold the steps and their order.

- **A new installation whose administrator cannot open the database gets its secure defaults and its stored-secrets
  key all the same.** On SQL Server with Windows authentication, where LocalSystem is a SQL Server administrator and
  the person running Setup is not, `--apply-new-install-defaults` ended in 2 and `--create-stored-secrets-key` in 1,
  both run as that person, and the recovery the message gives - the statements run by a SQL Server administrator, then
  Setup again - ran as an upgrade and applied neither: the server had no certificate, no STARTTLS and no key (the
  installer smoke VM, 3 October 2026). The service, which can open its database, now applies them itself at a start
  asked for with the argument `--apply-new-install-defaults` - `sc start hMailServer --apply-new-install-defaults` -
  before its listeners start, with the verb's own rules (the database's mark required, the shape of a new database,
  the mark cleared once decided), and for that new installation makes the key when it runs as LocalSystem, which may write `hMailServer.ini`;
  each step a line in the application log when that is on, and what it could not do an error (HM7498 for the key,
  HM7510 and HM7511 for the defaults), with the setup checklist showing the outcome. Setup and DBSetup start the
  service so whenever either verb could not open the database. And Setup's message after `--create-stored-secrets-key`
  no longer says it made a key that the command only found already held - Setup run again to finish an installation
  whose key an earlier run made - and DBSetup's says the same. [ServiceAccount.md](hmailserver/docs/ServiceAccount.md),
  "A new installation whose administrator cannot open the database"; DBSetup's `NewInstallationTests` and
  `InstallerPackaging` hold it.

- **The installer's last error after a new installation's external database names what retries it.** It said to
  run `DBSetupQuick.exe` to retry the database, the line after it said `DBSetup.exe`, and followed after a wrong SQL
  Server password `DBSetupQuick.exe` exited 1 with "CoCreateInstance failed. Result: -2147221164", since an
  external-database installation has no SQL Server Compact. It now names Setup run again or `DBSetup.exe` as an
  administrator, either of which finishes the installation. After a new installation's built-in database step the
  messages name Setup run again too, which finishes it, where `DBSetupQuick.exe` sets the database up and nothing
  after it; `DBSetupQuick.exe` stays the advice for an upgrade. The .NET runtime's failure, which said "then run
  DBSetupQuick.exe" on either branch, now says to run Setup again.

- **An older database named with `/createdatabase=0` is no longer reported as "too old" while it is upgraded.**
  DBSetup names it (COM `Database.SetDefaultDatabase`), restarts the server on it and upgrades it with DBUpdater;
  the restart's refusal of the older schema was a Critical HM5011 in the error log and the Application log on every
  such installation that then succeeded (6105 to 6108 on the installer smoke VM, 2 October 2026). `SetDefaultDatabase`
  now marks that restart as one that expects an older database, as the installer's start for an upgrade is marked
  (`--for-database-upgrade`): the refusal stands, and is what DBSetup and its wizard read and show, and is not
  reported; any later start or reinitialise reports it, and so does a database newer than the build.

- **Scripting off runs no script engine.** With scripting off the server no longer compiles the event script and runs
  it once per handler at every start, reinitialise and reload; switching scripting on - by COM, REST or an
  environment - loads it, compiled and searched, before the switch takes effect, and reports then a script that does
  not compile, whatever runs beside the switch - a reload, a reinitialise, a switch-off, another switch-on: a reload
  that read the switch as off is made again once it is on, and of two loads that overlap, the script of the one
  started last stays in force, unless it does not compile, which is reported. A rule's Run function action against a
  script loaded while scripting was off, which never compiled, is refused (HM5018) instead of being handed to the
  engine. An empty script runs no engine.

- **Crash records that place the fault.** The crash oracle's record adds, after the fields it always had, what an
  access violation read, wrote or executed and where, the faulting address's image base and offset, and up to twelve
  frames of the faulting thread; the REST API writes to the application log which method and path it answered 500
  after an exception.

- **Fixed: settings read while they were being reloaded.** hMailServer.ini's database and directory values, the
  administrator's second factor, and each value of the settings store and its map are read and written under locks,
  each value whole; a reinitialise or a COM Settings.Directories call could hand another thread an empty or freed
  value. A reinitialise now builds the settings store, the SMTP, POP3, IMAP and cache configurations, the server
  messages, blocked attachments, SSL certificates, DNS blacklists, SURBL servers and the local addresses whole
  before another thread can see them, and hands each over under a lock: a COM call made during one - the Control
  Panel's or a script's - could copy a pointer while it was being replaced, or find the settings not yet read and
  take every one as missing (0 or empty, and HM5015 in the ERROR log). The logger's level, device and format, and
  whether the live log takes entries, are read and written whole (`live-log-tests`, which `linux-build` runs, and
  under ThreadSanitizer), and `[Database] UpgradeOnStart` reads as off until the file is first read. Several values read
  one after another are still not read as one set, and a setting changed during a reinitialise, after the new
  settings were read, is stored but not in force until the next reinitialise or start.
- **Fixed: an error reported where the settings held no scripting switch exhausted the reporting thread's stack.**
  Every error reported asks whether the event script is on, to fire `OnError`. Where the settings store held no
  `UseScriptServer` - a database without that row, say - reading it reported the setting missing (HM5015), and that
  report asked the same question again, without end, until the stack ran out and took the service with it, at the
  first error reported. In every version. A report made while the switch is being read is now logged and fires no
  event, and the read answers the default, off. Found by `network-deadline-tests` on Debian 12 and Ubuntu 24.04,
  whose OpenSSL 3.0 makes the first TLS context report the key-exchange groups it lacks (HM5720); the program now
  reports an error with no switch in its settings before anything else, on every build.
- **Fixed: a list or a manager COM handed out before a reinitialise was not the server's after it.** Each reinitialise
  made new lists - the incoming relays, routes and remote domain policies, the public folders and groups, the server
  messages, blocked attachments and SSL certificates, the DNS blacklists and SURBL servers - so a COM object for one
  of them taken before it, as the Control Panel keeps one while its page is open, held the old list: a route added, a
  relay deleted or a message changed through it reached the database and the old list, and the server went on without
  the route, trusting the deleted relay and sending the old text until the next reinitialise or start. The lists are
  now made at the start and read again in place by every reinitialise, each read whole under its own lock, so every
  COM object for one holds the list the server reads. `Database.IsConnected` and `EnsurePrerequisites` answer for the
  database the server is connected to, where an object made before a reinitialise asked the connection manager the
  reinitialise had closed, and a `BackupManager` object asks for the backup manager in force at each call, so that a
  backup started through an old one no longer runs beside one started through the new; the two managers are replaced
  under a lock, where COM, the SQL log device's worker and the other threads a reinitialise does not stop could copy
  the pointer while it was being replaced, and a restore asked for through a `Backup` object in the moment between the
  old manager and the new is refused with a sentence, where it reached no manager at all and was answered as an
  unknown error. Tested in the Windows suite (`SettingsDuringReinitialize`: lists and a Database object held across a
  reinitialise).
- **Fixed: a profile verb an event script asked for while a profile was being applied or reverted.** An apply or a
  revert of a setting profile writes settings while it holds the profiles' lock, and a write can run an event script
  on the same thread: an error reported fires `OnError`, and a revert whose way back holds the scripting switch runs
  the script's top level. A script that then asked over COM to apply, revert, discard or decline a profile locked the
  mutex its own thread held, which the Windows build answered with an unknown error and a POSIX build would never
  have finished. Such a call is now refused at once, with the reason, and the apply or the revert goes on
  (`profiles-tests`, and `RestApiProfiles` in the Windows suite, where a revert that switches scripting back on runs
  a script that asks for a discard).
- **Active-active nodes: the design, not built** ([ActiveActive.md](hmailserver/docs/ActiveActive.md)). Until it is,
  one service per database.
- **Fixed: languages read while they were being reloaded.** A reinitialise rebuilt, in place, the translations COM's
  `Languages` and `Language` objects read - a script's or another COM client's; the Control Panel has translations of
  its own - and a language's first `String` call read its file into a table other calls were reading at the same
  time, which could crash the service on Windows (on Linux nothing reads them). Both are now built whole and handed
  over under a lock, as the settings are since the reinitialise fix. A language reads its translation from the
  directory its English was read from, where one taken before a reinitialise that changed `ProgramFolder` read the new
  directory's file through the old directory's English, whose numbered strings need not agree (`language-tests`, and
  the class tests on Windows).
- **Fixed: a part of the server that two threads asked for first at the same moment could be made twice.** Each of
  the server's 61 kinds of shared part made on first use (the logger, the caches, the rate limiter, the recipient
  callout's cache, the outgoing OAuth2 token client and the others) was checked for outside its lock and not again
  inside it, so two threads arriving together - two deliveries, or a COM call beside a REST request - could each make
  one, and what the first was given was lost; a thread could also be handed one before it was whole. Each is now made
  exactly once, and, as before, never taken down at exit; `singleton-tests`, which `linux-build` runs, holds it to
  that.
- **Fixed, on Linux: a rewrite of `hMailServer.ini` as root could give the file an owner nobody chose.** A rewrite
  goes to a temporary file that is given the owner, group and mode of the file it replaces, and the owner was taken
  from a look at that file whether or not the look had found it. When the file had gone between being read and being
  rewritten - removed or renamed by something else at that moment - the new one written as root, as
  `sudo hmailserver --set-admin-password` writes it, was given an owner and group no one had set, and the service
  could lose its own configuration. Since 6.3.0, the first Linux release. Such a file is now made as a file that was
  not there is, the writer's own and readable by it alone (0600); a file that is there keeps its owner, group and mode
  as before. `ini-file-tests`, which `linux-build` runs as root, holds it to that, with a race against another user's
  process that removes the file and makes it again while root rewrites it.
- **Fixed: a calendar edit at the largest SEQUENCE made the event older than it was.** The REST routes of the
  calendar and the tasks (the webmail's) write SEQUENCE one higher when an event with attendees or a task is changed,
  and at 2147483647, the largest an int holds, adding one wrapped to -2147483648, so a calendar that held the event
  took the change for an older version than its own. It now stays at 2147483647, and a SEQUENCE past that reads as
  2147483647 on Linux as on Windows: Linux read its low 32 bits, 4294967297 as 1. A task's PRIORITY that another
  client wrote outside 0 to 9 is listed as 0, none, since 0 to 9 is all the routes take; and a `priority` or an
  `alarm_minutes` past an int's range sent to them is refused with 400 rather than converted first, which was
  undefined - an alarm of `1e20` minutes was taken as no alarm. An alarm whose TRIGGER, in an event a calendar
  client stored or in an invitation the webmail shows, comes to more minutes than an int holds is listed as
  2147483647, held to its end as SEQUENCE is, where its digits and their sum overflowed. Since 6.3.4, which brought
  the routes. `API/RestApiCalendar.cs` holds it to that.
- **SCRAM holds a stored password hash's iteration count to the bound a password logon holds it to.** SCRAM-SHA-256
  read a stored PBKDF2 hash's count with no upper bound - one past an int as 2147483647 on Windows and as its low 32
  bits on Linux - and asked the client for that many rounds; the server never writes such a hash. One that says more
  than 10,000,000 is now refused, as a password logon refuses it, and the exchange fails at the proof, as for an
  unknown account. Since 6.2.0, which brought SCRAM. A password logon on Linux read the same count as its low 32
  bits too, and so took 4294967297 as one round; it now reads it as SCRAM does, and the two refuse the same hashes
  (since 6.3.0, the first Linux release). `IMAP/Basics.cs` holds SCRAM to that on Windows, and the tester program
  `scram-tests` both on Linux.
- **Fixed: a number past the range of what it was read into.** A REST body's number with a fraction or an exponent was
  converted to an integer from its double with no range check - undefined behaviour for `1e20` - and several routes
  then kept an int's low 32 bits of it, so `4294967301` was taken as 5. Such a number is now held at the end of the
  range it is read into: a message's `ids`, `max_uid` and `before_uid` in a webmail bulk call, a recall's `sent_id`
  and a mailbox restore's `message` that are not whole numbers an id can be are refused with their 400 (a fraction of
  an id was read as the id below it); an alert rule's `threshold`, `actions` and `cooldown_minutes`, a Web Push
  device's choices, a zero-access key's Argon2 parameters, an account's `restricted_delivery` and `kind`, an
  impersonation action, a webhook subscription's `max_per_minute` (COM and REST) and an eDiscovery hold's `search`
  reach their own checks as the large numbers they are. An iMIP invitation's SEQUENCE past an int's range was read as
  its low 32 bits on Linux, 4294967297 as 1, so the organiser's invitation at that SEQUENCE was taken for an older one
  than a copy at 2; it is now held at 2147483647 on both platforms, as the calendar routes hold it, by a room's
  bookings too. A Sieve `:index` past an int's range named an instance by its low 32 bits, so `:index 4294967297`
  selected the first; it now selects none. `update-feed-tests` (JsonValue's integers), `imip-tests`,
  `API/RestApiNumberRanges.cs`, `API/RestApiWebhooks.cs`, `API/RestApiSelfService.cs` and `Sieve/SieveDateDelivery.cs`
  hold it to that.
- **Fixed: another node's row of a domain's DKIM key rotation could keep a pass's state from being written.** When
  the row was made by another node between this pass's look for it and its insert, the insert was refused by the
  unique index and the refusal, which is ignored there, was taken for the write; the pass's state was lost until the
  next one. A refused insert is told from a written one by its id now, and the row that is there is updated. Since
  6.3.4, which brought the rotation. The class tests hold it to that on the server's own database.
- **Fixed: a Linux live update that the helper refused left the server refusing connections for ten minutes.** The
  helper refuses a request before it stops anything - an AppImage under the path unit with no `--image`, a line that
  fails a check - and writes its reason to its journal and nowhere else, so the server waited the ten minutes out in
  the drain the apply entered. It now asks systemd about the helper's unit every five seconds, and one that has
  ended with no outcome written has the drain left at once, HM7151 naming the unit and its exit status. Since
  6.3.4, which brought the drain. `update-feed-tests` holds the reading of systemd's answer to its cases.
- **Fixed: a COM look-up that found nothing left an object behind.** Asking a COM collection for an index, id or name
  it does not hold - `Languages.Item`, `Domains.ItemByName`, `Accounts.ItemByAddress` and 78 other look-ups, among
  them `Aliases.ItemByDBID` and `ItemByName`, `DistributionListRecipients.Item` and `ItemByDBID`,
  `IMAPFolderPermissions.ItemByDBID` and `ItemByName`, `IncomingRelays.Item`, `ItemByDBID` and `ItemByName`,
  `Messages.Item` and `ItemByDBID`, `RouteAddresses.ItemByDBID`, `Routes.Item`, and `SecurityRanges.Item`,
  `ItemByDBID` and `ItemByName`; `IMAPFolders.Add` when the name was taken or the folder could not be saved; and
  `Application.BackupManager` when no backup manager was available - made the object it would have answered with
  before looking, and kept it, never released, when the answer was no. Each now looks first, and a sweep of every COM
  method that makes an object, for a return that neither hands it over nor releases it, finds none left.
  `Recipients.Item` and `DiagnosticResults.Item` also refuse a negative index, which read outside their list.
- **The legacy Blowfish scheme of stored secrets is the project's own code.** `Common/Util/LegacyBlowfish.cpp`,
  written from Bruce Schneier's published description of the cipher, replaces `BlowFish.cpp`/`.h`/`.h2` (Schneier's
  reference code made a C++ class by Jim Conger, whose files stated no licence, which neither Debian nor Fedora
  ships). Every value the scheme holds - an `hMailServer.ini` database password with `PasswordEncryption=1`, a secret
  stored while `ProtectStoredSecretsWithDPAPI` is 0, the passwords and tokens a backup or a person's export carries,
  and COM `Utilities.BlowfishEncrypt` and `BlowfishDecrypt` - is written and read byte for byte as 6.3.4 did: about
  31 million comparisons with the old code and 60,000 answers from a 6.3.4 service found no difference. HM7277's
  source is now `LegacyBlowfish::EncryptToString`.
- **COM `Utilities`'s helpers that do work answer an authenticated caller or an event script.** Through
  `Application.Utilities` or created as `hMailServer.Utilities`, `BlowfishEncrypt`, `BlowfishDecrypt`,
  `ResolveMXRecords`, `GetMailServer`, `IsLocalHost`, `CheckSieveSyntax`, `EvaluateSieveScript` and `CriteriaMatch`
  answer a caller that has called `Authenticate` (an account of any level, or the administrator) or an event script
  running in the service, as the rest of the COM API and the equivalent REST routes do; a server with no
  administrator password answers as it did. `MD5`, `SHA256`, `GenerateGUID`, `IsStrongPassword` and the three syntax
  validators stay open.
- **The last update's outcome survives the check that follows the start.** The service reads the update helper's
  outcome at start-up, and on 6.3.4 and 6.3.5 the start-up check, a second later, replaced the whole update status:
  after an update `Status.UpdateApplyOutcome`, `apply` in `GET /api/v1/update` and the Control Panel said nothing,
  and only the log had it (issue #10's test, 29 September 2026). A check now keeps it; it stays until the service
  stops or the next outcome replaces it. Tested by `UpdateSchedule.TheLastApplysOutcomeOutlivesTheStartUpCheck`.
- **An update check that gets no answer says why, and a failed scheduled check is tried again soon after, once.** A
  new server's first check failed with "The response carried no HTTP headers." after 10.8 seconds, and the next with
  the scheduled check alone was a day away. An answer that ends before its headers is now reported with who ended the
  connection, after how many bytes and how many seconds after the connection was made, the TLS handshake's share and
  how the read ended - for every request of the server's HTTP client, not only the update check (not the installer's
  and bundle's downloads) - and a scheduled check that fails when the one before it did not is due again ten minutes
  later and read at the quarter-hourly task's next run, a quarter of an hour after a failed scheduled check, then
  every `UpdateCheckHours` as before. `GET /api/v1/update` gains `nextCheck`, when the scheduled check is next due.
  Tested by `UpdateCheck.AFeedThatEndsTheConnectionWithoutAnAnswerIsReportedAsSuch` and
  `UpdateCheck.AScheduledCheckThatFailedIsTriedAgainOnceTenMinutesLater`.
- **A verified installer is kept across a restart.** With `UpdateAutoDownload=1`, a service that cannot install an
  update itself (`NT SERVICE\hMailServer`, `HM7463`) downloaded the whole installer, 88 MB for 6.3.5, every time it
  started. A download now finds the installer and bundle an earlier one left in the `Updates` folder, holds them to
  everything a fresh download is held to, and fetches nothing when they pass; a copy that no longer passes is deleted
  and fetched again, and a start whose first check fails no longer sweeps the kept copy away. Tested by
  `UpdateSchedule.AVerifiedInstallerIsKeptAcrossARestartAndNotFetchedAgain` and
  `UpdateSchedule.AKeptInstallerOutlivesAStartWhoseFirstCheckFails`.
- **Fixed: a backup destination inside the data directory failed every backup, and so every unattended update.**
  The copy of the data directory walked into its own staging folder until the path was too long (`HM5014`): the
  first Linux live update, whose destination was under `/var/lib/hmailserver`, was refused for an hour and a half.
  A backup now leaves out, and a restore leaves in place, the destination, the temp directory and the run's staging
  folder - and a restore the folder of the archive it restores from - wherever they are inside the data directory,
  recognised by the directory itself rather than its spelling; the backup log names each. Such a destination works,
  and the backup is then on the disk it backs up ([Backup.md](hmailserver/docs/Backup.md)). The same change removes
  two faults of the Linux packages, whose temp directory is inside the data directory, found by reading the code and
  not reproduced: an encrypted backup with messages failed the same way, and a restore of messages deleted the store
  it had just extracted. Tested by `backup-tests`, `BackupExecuterTester` and
  `BackupRestore.ADestinationInsideTheDataDirectoryIsBackedUpToAndRestoredFrom`.
- **A backup no longer carries the live update's folder.** `Updates` - the package or installer waiting to be
  applied, the rollback image, the apply token and the last apply's outcome - was in every archive, 15 MB of the
  first Linux live update's 15.4 MB, and a restored copy of the outcome would have a server report an apply it
  never made. It is left out as the recoverable store is, and a restore leaves this machine's own where it is.
- **Fixed: a full restore of messages failed on every installation made new with 6.3.4 or 6.3.5, and left the mail
  in the temp folder alone.** Such an installation's service runs as `NT SERVICE\hMailServer`, and the self-signed key
  it made for itself gives that account Read and nothing more; the restore emptied the data directory before it
  copied the archive's store back: it stopped at `Data\Certificates`
  (`HM5700`), failed the copy 30 s later on the archive's copy of the same key (`HM5014`), and ended with no domain on
  the server and the backup's messages only in `Temp\{GUID}\DataBackup` - on the smoke VM with the signed 6.3.4 and
  6.3.5 installers and with 6.3.6 before this change, every time. A key or certificate file an administrator moved
  into the data folder, which keeps the permissions it had, did the same wherever those permissions did not let the
  service's account delete it. On Linux, from 6.3.0, the emptying also deleted the data directory's
  `.hmailserver-secret-key`, which every `LINUX1:` secret is sealed under, and no archive carries it: from 6.3.4
  `[Security] StoredSecretsKey` then no longer opened, so from the restore on no account's second factor and no
  `[Settings]` secret did (`HM7495` at each start), and neither did the other `LINUX1:` lines of `hMailServer.ini`.
  The restore now copies the archive's store in first, over what is there: a file the data directory holds byte for
  byte is left, one that differs is renamed aside and the archive's put in its place, and one the service's account
  may not replace is left as it is and named - in the backup log, and once as `HM5705`. This installation's own
  self-signed certificate and key are left as they are when the archive's key is not the same, and no file at the
  root of the archive's store, which no archive the server makes holds, is put over the server's own. A copy that
  fails is undone, the part of a file a full disk left included, so the data directory is what it was. Only then is
  what the archive does not hold removed, and at the data directory's root only the delivery queue's message files.
  **If a restore failed this way**, the archive is as it was: upgrade and restore again from it, then delete the
  staged copy its `HM5014` line names; on Linux import the stored-secrets key from its export
  ([Backup.md](hmailserver/docs/Backup.md), "How a restore replaces the data directory"). Tested by
  `BackupExecuterTester` (`backup-tests` on Linux),
  `BackupRestore.AFullRestoreLeavesInPlaceTheFilesTheServiceMayNotChange`, a restore end to end on Linux - a full
  disk among them - and on the installer smoke VM.
- **Fixed: a message deleted while a backup copied the data directory failed the backup.** From 6.2.19 the copy is
  meant to count a file that a POP3 `DELE` or an IMAP `EXPUNGE` removed between the listing and the copy as gone,
  and go on. It asked Boost whether the file was still there and took "no error, and not there" for gone, and Boost
  answers a file that is not there with an error as well: so such a file was retried for 30 s and then failed the
  backup (`HM5014`, "could not be copied: No such file or directory"), or failed it at once when it went between
  the listing and the question what it was - and with it the update a backup was taken for
  (`UpdateBackupBeforeApply`). An encrypted backup's measure of the data directory failed the same way, and a restore
  whose staged copy lost a file, a scanner taking it from the temp folder, failed instead of going on without it. A
  file is now gone when the system answers that there is no such file - not when a share or a drive cannot be
  reached, which Boost answers alike - and is counted and said as it was meant to be. Found by `BackupExecuterTester`,
  whose case of a file gone from a restore's store failed against the old test.
- **The live update's trust is read from `hMailServer.ini` alone.** The server read `UpdateSigningIdentity`,
  `UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` from the
  database, where the REST API and the Deck write, and root's `hmailserver --verify-update` on Linux from the file,
  so a signer set over REST was used by the one and not the other. The process an update is checked for must not
  choose what it is checked against: on every platform the server now reads them from the file, and COM and the REST
  API (409) - and so the Control Panel, the Deck, a profile and the audit trail's put-back - refuse a change. **On
  upgrade** a value 6.3.4 or 6.3.5 stored in the database no longer applies: the first start takes it out and names
  it once as `HM5809`. Set them with `sudo hmailserver --set-setting` on Linux, and in the file on Windows
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "Where the trust is read from"). `hmctl config export` and
  `Export-HmConfiguration` leave the five out, as they leave out what the server's environment sets, and `diff` and
  `apply` of a document an earlier version exported skip them with a line naming them rather than fail the apply
  ([CommandLine.md](hmailserver/docs/CommandLine.md)). Tested by `trust-anchor-tests`, by `IniSettingsOverCom`,
  `RestApiSettings` and `IniSettingsMirror`, and by `check-cli.py`.
- **The database upgrade's token is in no log of the live update.** Where the service installs its own updates
  (LocalSystem), `hMailServer.Updater` has passed the single-use token on the installer's command line since 6.2.28,
  and so written it into `Updates\apply-<version>.log`, and the installer's own log, `Updates\install-<version>.log`,
  began with the same command line, as Inno Setup's logs do. A 6.3.6 or later installer is now given the token in
  its environment (`HM_UPGRADE_TOKEN`), as the administrator's install gives it, and the helper's log names it as
  `/upgradetoken=(the token)` for an older one. The folder is readable by SYSTEM, the Administrators and the
  service alone, and the token is spent once the installer's database step has run. Tested by
  `UpdateApply.AVerifiedInstallerIsHandedToTheHelperWhichRunsItSilently` and
  `UpdateSchedule.AnUpdateIsAppliedUnattendedInsideTheWindow`.
- **An update that did not complete is put back only while the database is as the earlier version left it, and the
  installer's exit code says which.** When a step after the installer's files did not complete - the service
  account's folders, the administrator password, the database or the service - the Windows installer showed and
  logged the error and then exited 0, though its own comment and [Upgrading.md](hmailserver/docs/Upgrading.md)
  said it failed the install; the update helper, finding the service running (the installer starts it for the
  database step, and the server in it refuses the database), reported the update installed. On the installer smoke
  VM an update whose database upgrade could not run was reported as "the installer succeeded and the service is
  running", with no mail served. The installer now exits with **10** while the database is as the earlier version
  left it, and `hMailServer.Updater` - the live update's and the administrator's one-click install's - runs the
  rollback image on it as it does when the service does not come back, without first waiting for the service, and
  does not call a rollback whose own installer exits 10 or 11 a success. Once its database step has changed the
  database - its version, read through the service as DBUpdater reads it, before the installation and after the
  step - or cannot show that it left it unchanged, the installer exits with **11**, and the Updater puts nothing
  back: the earlier version may refuse that database, and a rollback would leave a server that serves no mail. A
  database already at the version the new build needs is known unchanged even when its version cannot be read after
  the step, so an update that changes no schema and whose new service does not start is put back. Nor does the
  Updater roll back where the installer's log - its own stamped lines, after the step began - says its database
  step began and not that it left the database unchanged, which covers a service that stops after exit code 0 and an
  installer that never finishes. That outcome is `upgraded-incomplete`: what did not complete, how the database's
  version moved, and, for a database upgrade that stopped part-way, to finish it with DBSetupQuick or restore the
  backup the update took (`UpdateBackupBeforeApply`), naming where from a note the service writes, which the
  elevated Updater reads only as a plain file with one name; the service reports it as `HM5481`, and the
  Control Panel says it in English and its seventeen other languages ("its database step changed the database, or
  may have"), its sign-in
  banner cut at the first sentence in Japanese and Chinese too; its four texts that promised the previous version back
  whenever the new one did not come up now add "unless the new version has already changed the database". Run by the
  Updater with a version to put back - which
  it says only when it holds that version's installer, opened and held to its SHA-256 from before the installer runs
  until it has run - the installer stops before its database step when a step before it - the .NET runtime, SQL
  Server Compact, the service's registration, the grant of its folders, the administrator password - did not
  complete, so that the rollback meets the database as the earlier version left it; run by hand, it carries on to the
  database step as before. The outcome quotes the installer's own message about the step, which the lines it took
  from the installer's log had left out for two that only name an error (an imported `WSAGetLastError`, a grant with
  "0 failed") and for "did not start", no longer the last file copied, and no carriage return of a message box's
  text. Tested by `UpdateApply.AnInstallerWhoseStepsAfterItsFilesDidNotCompleteIsRolledBack`,
  `AnInstallerThatUpgradedTheDatabaseIsNeverRolledBack`, `AServiceThatDoesNotComeBackOnAnUpgradedDatabaseIsNotRolledBack`
  and `UpdateHelperDigest.TheRollbackImageIsHeldWhileTheInstallerRuns`, by `AdministratorUpdateTests`, and end to end
  on the smoke VM with the administrator's one-click install from a 6.3.9 to a 6.3.10 whose database step moves the
  schema from 6105 to 6106: with the grant of the service's folders made to fail, the install stopped before its
  database step and 6.3.9 was put back on its 6105 database and served mail, the rollback beginning the second the
  installer ended (built before this change, the same update upgraded the database, put 6.3.9 back on 6106, whose
  installer then failed - rollback-failed - and the server answered nothing; and the Updater had waited three
  minutes before the rollback); with the step's script made to fail after a CREATE TABLE, DBUpdater's transaction
  took the table back, the installer ended with 10 and 6.3.9 served mail on 6105; with the service's start made to
  fail after the upgrade, it ended with 11 and nothing was put back, and 6.3.10 served mail on 6106 once its service
  could start (built before this change, the same update put 6.3.9 back on 6106, its installer failed and 6.3.9
  refused the database as too new: no mail); and a 6.3.10 that changes no schema, whose service could not start, was
  put back and 6.3.9 served mail (built before this change: 11, nothing put back, no mail) - the same again as the
  service's own unattended apply on a server whose service runs as LocalSystem.
- **The Linux live update puts the earlier version back only onto the database it left, too.** `hmailserver-update`
  reinstalled the previous `.deb`, `.rpm` or AppImage whenever the new version's server did not come back, without
  looking at the database; but the package's own post-install step runs `hmailserver --upgrade-database`, and a
  server with `[Database] UpgradeOnStart=1` runs the chain at its own start, so the earlier version could be put back
  onto a database its schema step had already moved, which that version refuses as too new: no mail. The helper now
  reads the database's version before it installs and again when the new version is not back, with the new
  **`hmailserver --schema-version`** (`required <n>`, the version the program needs, said before it reads anything,
  then `database <n>`, its `hm_dbversion`; exit 1 when that cannot be read, 2 with no `[Database]`), run as the
  unit's `User=` - for an AppImage with the unit's `HMAILSERVER_HOME` and `XDG_DATA_HOME` - and in the unit's own
  sandbox: systemd runs it as a transient service with the unit's sandboxing properties, as `systemctl show` reports
  them, and where systemd will not, the version is not read, never asked outside the sandbox. It puts the earlier
  version back only while it is unchanged, or, where it cannot be read afterwards, was already at or past what the new
  version needs. Otherwise nothing is put back, with a rollback image or without one, and the outcome is
  `upgraded-incomplete` (HM5481): how the database moved, or that it may have, and what to do - start the service once
  what keeps it down is put right, finish an upgrade that stopped part-way with `--upgrade-database`, or restore the
  backup the update took, which the server now names in its request (`backup=`), and reinstall the earlier version.
  A version that cannot be read before the install leaves nothing to hold the database to, and nothing is put back.
  The server's own HM5481 sentence now says the database step changed the database "or may have", on both platforms,
  and `hmailserver-update check` says whether the version can be read. The update to 6.3.6 is made by 6.3.5's helper,
  so the rule holds from the update after it. Tested by `build/test-hmailserver-update.sh` (new cases: a version the
  package's schema step moved all the way and part-way, one a new AppImage's start moved, one that did not move, one
  that cannot be read after the install - short of what the new version needs, and at it - and one that cannot be
  read before, with and without a rollback image, the backup named and one with a control character left out, a unit
  user that is no user name; the helper before this change fails 32 of the script's 276 checks), by
  `build/db-upgrade-gate.sh`'s `--schema-version` on PostgreSQL and MySQL before and after the real chain from 6029,
  with no database to reach and with none configured, and end to end on the lab VM, in containers with PostgreSQL 18,
  this tree's `.deb` and a 6.3.7 made of it whose server needs schema 6110: a 6.3.7 whose post-install step moved the
  database to 6109 and stopped, and whose server then could not start, was not put back - the outcome said 6108 to
  6109, short of 6110, and named the backup - and once the step was put right and finished, 6.3.7 started, reported
  HM5481 and served mail; one whose step moved nothing was put back, and 6.3.6 served mail again. The helper before
  this change put 6.3.6 back onto the database at 6109, which 6.3.6 refused: rollback-failed, no mail.
- **A server that refuses its database for its version says why to whoever started it.** A variable of the same
  name hid the reason from the caller of the start: on Linux the start printed "hMailServer could not start: " and
  nothing after it, and COM's `Application.Reinitialize` onto such a database answered success with nothing loaded
  (the REST API's reinitialise logged it completed, SIGHUP's nothing). The reason ("The database is too new for this
  version of hMailServer ... Database version: 6109, Required database version: 6108") now reaches all of them; the
  error log's HM5011 is as it was. Tested by `DatabaseRefused.TheUpdatersPathStaysOpenOnARefusedDatabase`.
- **DBSetupQuick says why it could not run, and the installer reads it as a database step that changed nothing.**
  When the hMailServer service could not be reached through COM, the installer's database tool died of the
  unhandled COMException (exit code -532462766) and wrote the reason nowhere; the installer took that for a database
  step that may have changed the database. It now exits with **4**, having started no DBUpdater, and writes why - the
  COM error with its HRESULT, and that nothing was changed - to standard error and into a report the installer names
  on its command line (`report:PATH`) and copies into its log, as it now does whatever DBSetupQuick says about a
  failure; with `[Database] UpgradeOnStart` off, the installer then knows the database unchanged ("Database step left
  the database unchanged ... DBSetupQuick.exe could not reach the hMailServer service (exit code 4)"), and on an update
  ends with 10, so that the earlier version is put back. A failure of the sign-in or of the question of which database
  on a new installation is said the same way and ends with 1 rather than the crash. Tested by
  `DBSetup.Tests.QuickServiceNotReachedTests` (eight).
- **DBUpdater's and the server's comments, and Greylisting.md and Upgrading.md, no longer say that SQL Server Compact
  commits each `ALTER` as it runs.** It does not: inside the upgrade's transaction its DDL is rolled back with the rest,
  through DBUpdater's COM transaction as through the server's own (`SchemaUpgrader`), both a `SQLCEConnection`'s ADO
  transaction - shown on the installer smoke VM, where a CREATE TABLE and an ALTER TABLE ADD were gone after the
  rollback. DBUpdater still commits the steps before 6107 on Compact when a chain starts below 6072, for the size of
  the transaction its greylisting rewrite would need in Compact's temporary file; the cost is the earlier steps'
  rollback, so a failure from 6107 on leaves 6106, which the installer reads as changed (exit code 11). The method is
  now `SplitsAtLongPrerequisites`, and nothing it does has changed.
- **The one-click install (above): what refuses it, and what its window says.** The elevated stage refuses an
  installer whose certificates' revocation could not be checked: Windows' Authenticode check takes a revocation server
  that does not answer as good, so the stage reads the revocation status of each certificate in the chain Windows
  built, the root aside, and refuses where one is unknown or offline, naming the revocation server. A computer that
  cannot reach that server - a firewall, a proxy, somebody on the network between - so stops the one-click install
  with nothing changed; the installer can still be run by hand. The install window says what stopped the install
  once; says a refused copy (a link or a junction on the way to the installer) was kept for inspection only where it
  was; and, where Windows would not start the install as an administrator - a standard user whose requests to
  elevate are denied by policy - says that nothing was changed, beside Windows' own sentence. It waits for the answer
  to its own question ("Stop the update?", or whether to install without a backup): nothing moves on, towards the
  installer or a cancel, until the question is answered. *Install update* clicked while the service prepares, holds
  ready or installs an administrator's install - from a second administrator's Control Panel, or a second click -
  says that another administrator's install of an update is under way on this server, as the stage says when it
  finds one. And `hMailServer.Updater` starts the Control Panel it opens again with the security the desktop's shell
  gives the programs it starts, so that the user it runs as, unelevated, can end it. Tested by `AdminUpdateTester`
  and `AdministratorUpdateTests`, and on the smoke VM, where the real 6.3.5 with outbound HTTP blocked was answered 0
  by Windows and refused, and passed with it open and nothing cached.
- **The backup and event logs are UTF-8 on Linux**, as the other logs are; they were UTF-16LE. A file an earlier
  version wrote is converted when the server first opens it. Windows keeps UTF-16LE.
- **Fixed: a message whose last bytes the disk refused was kept short, and taken for whole.** A write to a file goes
  into a buffer first, and closing the file writes the last of it - 4 KB on most systems - so the close is the only
  report of whether a file's end arrived. The server did not check it, in every version up to 6.3.5, so when a disk
  filled or a quota was reached just as a message's last bytes were written, the message was kept without them and
  nothing said so: SMTP answered 250 to DATA and to BDAT, IMAP APPEND answered OK, and a message collected from an
  external POP3 or IMAP account was delivered short and then deleted from the remote server, where the account
  deletes what it collects. A header the server writes into a message after accepting it - the `Return-Path` and
  `Delivered-To` of local delivery, a DKIM signature, an ARC seal, the `Return-Path` SpamAssassin is shown, the
  sandbox's verdict - could put a short copy in place of the whole message, and so could the message store's own
  writes (a compressed or encrypted message, a copy of one, an import), a journaling copy, a copy encrypted at the
  gateway, an IMAP migration's copy and every file the server writes whole through `FileUtilities`.
  `MessageStoreFsync`, off by default, had the spool files of SMTP and of the fetchers and the store's own writes
  checked before their close; nothing else was. Every writer that takes a file for whole now takes the close's answer
  too: SMTP answers `451 4.3.0` and the sender tries again, with `HM5862` in the error log once per message; APPEND
  answers NO (`HM5211`); a collected message stays on the remote server for the next collection (`HM5862`, then
  `HM5864` for POP3 or `HM5865` for IMAP); a header rewrite leaves the message as it was, and the delivery goes on as
  it did when the rewrite could not be written; an IMAP migration tries the run again. Tested by
  `file-utilities-tests`, against `/dev/full`, a quota and a full disk, and by `SpoolWriteFailure` for DATA, BDAT,
  APPEND and both kinds of collection.
- **Fixed: a SpamAssassin result the disk refused part-way cut the message short.** With `SAMoveVsCopy` at its
  default, the server copied SpamAssassin's result onto the message file, and a copy onto a file empties it before it
  writes it: when the disk filled during the copy, the message was left short or empty, in every version up to 6.3.5,
  and the server went on with it as if it were whole. The result is now copied beside the message and renamed over it
  once it is whole; a copy that fails leaves the message as it came, delivered without SpamAssassin's headers, as
  when spamd cannot be reached (`HM5861`). A result whose file did not close cleanly is not applied either. Tested by
  `file-utilities-tests` (`FileUtilities::CopyOver`, against a quota and a full disk). With `SAMoveVsCopy=1` the
  result was moved onto the message instead, which is a copy onto it when the temporary folder and the data folder
  are on two volumes; it is now moved beside the message and renamed over it in the same way
  (`FileUtilities::MoveOver`), and a move whose copy fails leaves the message as it came (`HM5860`).
- **Fixed: two more files whose last bytes went unchecked.** The list of members a backup restore hands 7-Zip, and a
  file the live update downloads (the installer and its Sigstore bundle), were taken for whole when their close
  failed, in every version that has them: a restore could extract fewer
  members than it asked for and go on, and the live update could take a short installer or Sigstore bundle for
  whole until its check refused it. Both now take the close's answer:
  the restore stops with the list's file removed, and the download fails, says that the disk may be full and leaves
  nothing behind. Tested by `file-utilities-tests`, with a loopback server for the download, against a quota and a
  full disk, and each beside its control where the disk takes it.
- **The object store's committer says what ends a pass.** Since 6.3.4, which brought the object store, a pass that
  met an unexpected error ended without a word. Nothing was lost, and nothing is: what it had not put in waits on the
  local disk, is read from there, and the next pass takes it up. It now says so in the error log as `HM7473`, once an
  hour at most ([ObjectStorage.md](hmailserver/docs/ObjectStorage.md)).
- **On Linux the server says which character sets the machine cannot convert.** It converts mail's and IMAP's text
  with the C library's iconv, which loads a character set's converter only when one is asked for, and a converter
  that was not installed made the text empty and said nothing. `hmailserver --check-config` now ends with a line
  `Character sets:` that says every character set the server converts text in - the code page table's 24 and the
  PST import's - converts here, or names those that do not, with the package that has them on Fedora, EL and
  openSUSE; and a start with any missing writes the same to standard error, which the journal, OpenRC's console log
  or an AppImage's terminal keeps, and to the application log. A warning, not a refusal: mail in the other
  character sets is served, and `--check-config` still exits 0, as the OpenRC service's start needs. The checks of
  the `.rpm` above read that line (`Unicode::CodePageCharacterSets`, `Unicode::ConvertsCharacterSet`,
  `platform/main.cpp`; the packaging guide, [step 5](hmailserver/source/Server/platform/packaging/README.md)).
- **The auto-ban hook is run from where the build installed it, and on NixOS the auto-ban reaches the firewall.**
  With `AutoBanFirewall=1` the Linux server runs `autoban-hook`, which keeps an nftables set of the banned
  addresses. It ran `/usr/lib/hmailserver/autoban-hook` whatever prefix it was built for, while the build installs
  the hook below its own prefix: the packages', `/usr`, had it, and NixOS, where the flake's package is in the Nix
  store, and a build installed under `/usr/local` did not, so there the setting found no hook (`HM6523`).
  `CMakeLists.txt` now compiles the hook's installed path into the server (`HM_AUTOBAN_HOOK`,
  `AutoBanFirewall::PackagedHook`), and refuses a prefix the server could not run as one command; every package's
  path is the same as before. The NixOS module's new `autoBanFirewall` stores the setting and gives the service
  `CAP_NET_ADMIN` and `nft`, which the hook needs - the packaged unit leaves that capability to a drop-in, and the
  option is that deliberate step - and does so too when `settings` sets `AutoBanFirewall` to 1; every program the
  service starts then has the capability, whatever `AutoBanCommand` names among them. Neither a link at
  `/usr/lib/hmailserver` on NixOS nor the hook's path as a setting: the first writes outside the store, and the
  second would be one more program the REST API could name for the server to run with that capability. The NixOS
  test places a ban through the REST API and reads it in the set, and the ban deleted, out of it; with KVM it
  passed, and with the server as it was it failed there ([Nix.md](hmailserver/docs/Nix.md), "The auto-ban below
  the server").

- **The compromised-account detection and the rooms' booking records say what an unexpected error stopped.** Since
  6.3.4, which brought both, seven catch-alls around a whole pass or operation dropped every unexpected error without
  a word - on Windows, where the server is built with `/EHa`, an access violation among them: the detection's
  handling of a verdict (the baseline, the lock and the alert), its unlock, its discarding of an account's held
  messages when the account is deleted, its reading of the locks at start and its sweep; and a room's forgetting of
  one booking's record and of the records whose calendars are gone. Each still stops there, as it did, and now
  writes one line to the error log saying what was not done and what happens next: `HM6811` for the verdict,
  `HM6812` for the sweep, `HM6813` for the locks, `HM7561` for the unlock, `HM7562` for the discarding with a
  deleted account, and `HM7434` for the booking records
  ([CompromisedAccounts.md](hmailserver/docs/CompromisedAccounts.md), "In the error log";
  [RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md)).

- **Fixed: every TLS context switched on every OpenSSL option from bit 32 up.** The mask of options the server sets
  on each TLS context (`SslContextInitializer`) was held in an `int`. OpenSSL's `SSL_OP_ALL` carries bit 31, so the
  value was negative, and OpenSSL's 64-bit option word received it with bits 32 to 63 set. Every listener (SMTP,
  POP3, IMAP, ManageSieve, the metrics and REST listeners, web services) and every outgoing connection set up by the
  same code (SMTP delivery, the external POP3 accounts' fetch, the SpamAssassin client, the HTTPS client, ACME, the
  IMAP migration client, the syslog TLS sink) had them, in every release linking an OpenSSL that names options
  there: on Windows from 6.0.0-B1 to 6.3.5 (OpenSSL 4.0), on Linux from 6.3.0 wherever the distribution's OpenSSL is
  3.2 or later. What could be seen on the wire: on Windows every outgoing TLS 1.3 handshake carried an Encrypted
  Client Hello GREASE extension, and TLS 1.2 handshakes in both directions offered the compressed elliptic-curve
  point formats RFC 8422 deprecates, as they did on Linux with OpenSSL 3.6 or later; on Linux with OpenSSL 3.2 or
  later certificate compression (RFC 8879) was off in both directions. None of it lowered the protection of a
  connection as the server sets it up. `SSL_OP_PREFER_NO_DHE_KEX`, among the bits, acts only with
  `SSL_OP_ALLOW_NO_DHE_KEX`, which the server does not set, so TLS 1.3 resumption kept its (EC)DHE exchange - except
  on a host whose OpenSSL configuration file allows resumption without one (`Options = AllowNoDHEKEX`, which no
  distribution sets by default), where the server preferred that for a client offering both, and a session resumed
  so had no forward secrecy. The other options there act only on what the server does not use (kernel TLS,
  Encrypted Client Hello keys), and a bit no OpenSSL names yet would have switched on whatever option a later OpenSSL
  gave it. The mask is a `uint64_t` now and holds only what the server asks for. Certificate compression stays off
  in both directions, on purpose: it keeps a decompressor that the peer feeds out of the handshake, and saves a mail
  server nothing it needs. The two HTTPS clients this code does not set up, the MTA-STS policy fetch and the OAuth2
  token request, keep it off now too: on Linux with OpenSSL 3.2 or later they offered to take the web server's
  certificate compressed. A new tester program the Linux build and the source packages run, `tls-options-tests`,
  sets up a listener's and a client's context for every combination of the TLS versions and the cipher-order
  settings, and the two HTTPS clients' context, and reads back with `SSL_CTX_get_options` what OpenSSL holds; it
  fails against the code as it was. `SslTlsVersionTests` has a case that reads the EC point formats a listener
  offers.

- **Fixed: six failures that stopped a pass or a step were said nowhere.** Each is still caught where it was, so
  nothing else stops with it, and is now written to the error log, once an hour at most: a pass of the migration
  scheduler that starts and resumes the jobs (`HM7240`); the filing of a delivered calendar reply, cancellation or
  invitation into the recipient's calendar (`HM7555`, new; the message is delivered all the same); the opening of an
  encrypted mailbox's key at a sign-in (`HM7366`, once an hour an account; the sign-in stands); the recording of a
  message trace event (`HM7554`, new; the delivery goes on and the trace has a gap); keeping an account's list of
  devices to its fifty (`HM6991`); and the SNMP agent's refresh of the delivery queue's two figures (`HM7422`).
  Until now a migration that did not start, replies that were not filed, a key that did not open, a trace with
  gaps, a device list past its limit or two SNMP figures that stopped moving left no cause in any log - the trace
  since it came, in 6.2.22's pre-releases, and the other five since 6.3.4. Migration.md, Calendar.md,
  EncryptionAtRest.md, Reports.md and Snmp.md name the codes. A seventh, the sandbox poller's reading of its own
  interval, is said too (`HM7560`, new, Sandbox.md; the poller goes on every 30 seconds until the interval can be
  read): it was swallowed without a word since 6.3.4.
- **Database passwords are passed on as they are.** The server wrote the host, the user, the password and the database
  into its connection string as they were: for PostgreSQL between single quotes, so a quote or a backslash in a
  password ended its value early; for SQL Server ending at a semicolon, and a value that began with a quote was read
  as a quoted one or did not parse ("Format of the initialization string does not conform to the OLE DB
  specification"). PostgreSQL's values are now quoted and escaped as libpq reads them, with the root certificate's
  path in the code page libpq opens it by. SQL Server's user and password are handed to ADO's `Open` beside the
  string, not in it, and its server and database are in the quotes ADO reads, the server as `Data Source`, OLE DB's
  own name for it, where it was the provider's `Server`; a name with a double quote beside a single quote or a
  semicolon, which no string ADO reads can carry, and a failover partner with a quote, a semicolon, a brace or an
  equals sign, are refused with the reason. MySQL takes each value as an argument of its own, but the server named the
  database bare in two statements of every connection, so a database whose name MySQL takes only in quotes - with a
  hyphen in it, say - could not be used; it is quoted now. A PostgreSQL or MySQL value with a character outside ASCII
  reached the database server as nothing on Linux, and on Windows so did one with a character beyond U+00FF, the
  client then connecting with its own defaults: such a value now goes in UTF-8. On Windows a value whose characters
  are all U+00FF or below still goes in Latin-1, as it always did there, so that a password with such a letter that
  connects today, set on a PostgreSQL server whose encoding is WIN1252 or LATIN1, goes on connecting; such a password
  on a server in UTF8 is still refused there, as it was (PostgreSQL 17 initialised each way takes only the bytes of
  its own encoding). So DBSetup no longer refuses a quote, a backslash or a semicolon
  ([WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md), "What the server passes on"; `DBSetup.Tests`), and the
  container's entrypoint refuses (exit 64), before `hMailServer.ini` is written, an `HM_DB_*` value the file would not
  give back: a line break or another control character, white space at either end, or quotation marks around it; a
  quote or a backslash inside is written as it is. Tested by `connection-string-tests` (every value libpq reads back
  as given, nothing else moved; the bytes each value is handed over in, and MySQL's quoted database name), by the
  class tests on Windows (the same, and ADO's own reading of SQL Server's string), by
  `build/test-docker-entrypoint.sh` in the `update-helper` job, and against PostgreSQL 17, MariaDB 11.4, MySQL 8.4 and
  SQL Server 2022 with such values on 1 October 2026.
- **Fixed: `CREATE DATABASE` named the database as it stood.** DBSetup's (COM `CreateExternalDatabase`) and
  `hmailserver --create-database`'s statement had the name bare on SQL Server and MySQL and in double quotes it could
  close on PostgreSQL, so a name those take only quoted - with a hyphen in it - could not be created, and a quote
  in it ended the name early. Each now quotes the name as its dialect quotes an identifier,
  with that quote doubled inside. And the SQL Server Compact connection string had the database file's path and
  password bare: a data folder with a semicolon in its name ended the path there, and every start failed with
  "Multiple-step OLE DB operation generated errors". Both are quoted as ADO reads them back now. `connection-string-tests`
  and the class tests hold both to that.
- **Fixed: a SQL Server Compact 3.5 database the engine could not upgrade was moved aside all the same.** When a server
  upgraded from hMailServer 5 found its `.sdf` at version 3.5, it asked SQL Server Compact 4.0's engine to upgrade it
  and read the HRESULT of creating the engine instead of the engine's answer: a file the engine could not upgrade - a
  damaged one, say - was moved to `.backup_v52` with nothing put in its place, and the service could not find its
  database at the next start; where a stopped attempt had left its copy in the way, that copy was put in the live file's
  place; and a second upgrade wrote over the first one's backup. The upgraded copy is now made beside the file, removed
  first when an earlier attempt left one, and checked - the engine reads every page and the provider opens it - before
  the 3.5 file is moved aside, to `.backup_v52` or the first `.backup_v52.<n>` free; a copy that cannot take its place
  puts the 3.5 file back. Any failure says why in the error log (`HM5122` to `HM5125`, with what the engine recorded)
  and in the start's own `HM4354`, which carried no message; every one but one leaves the live file as it was, for the
  next start to try again, and that one, a copy that cannot take its place when the 3.5 file cannot be put back either,
  names both files and the rename that finishes the upgrade. The class tests hold it with a damaged file, a 4.0 file, a
  file held open by another process, a leftover copy, an unsound copy and a backup name already taken.
- **The live update requires the project's id at the path the project began at too.** A certificate for
  `https://gitlab.com/Progressiverobot/hmailserver`, where 6.3.5 was signed, is now refused without the project's id,
  86730559, as one for the group's path always was, and no setting takes one without it: servers 6.3.4 and 6.3.5
  checked the id there only when the certificate carried one, because no GitLab certificate had been seen when the
  entry was written. 6.3.5's records it, so it installs, and rolls back to, as before; what is refused is what a
  project that is not this one could sign at the path the project has left, which a personal namespace hands to
  someone else when its owner is renamed. The release chain's own checks of a release's files, provenance and images
  (`release-files.py`, `release-chain.py`, `registry-mirror.py`, `container-image-deb.sh`) hold both paths to the id
  likewise ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "What a server built after 6.3.3 checks").
- **Fixed: a hook that hung held up every ban decision while it ran.** `AutoBanNeverBan` - which a spam trap's `RCPT
  TO`, a failed logon, a null sender's second recipient and AUTH on a delivery port all ask before they ban, and a
  widening asks of every neighbour - was read under the lock a reconciliation holds while it runs `AutoBanCommand` or
  the packaged hook, up to `ExternalProcessTimeout` (300 seconds as shipped) a run. So while a hook hung, a spam
  trap's refusal was answered only when the hook had been stopped - a difference in time from an unknown address's,
  which the trap's ban is placed off the session's thread to prevent - and every failed logon of the server waited as
  long. It is read under a lock of its own. On bench150 a trap addressed while a hook hung for another ban was refused
  after 7.7 seconds before, and at once after ([AutoBan.md](hmailserver/docs/AutoBan.md), "When"). And the report of
  an entry of a new `AutoBanNeverBan` value that is no address (HM6524), which runs the event script's `OnError` on
  the thread that read the value, is made once that lock is let go: under it, a slow handler held up every ban
  decision of the server as a hung hook did.

### Build and tests

- **The tests that sign in over SMTP with `AUTH PLAIN` turn it on themselves.** `API/RoomsAndEquipment.cs` signs its
  organisers in over SMTP with `AUTH PLAIN`, which the server offers only with `AllowSMTPAuthPlain` on - off in the
  shipped configuration - and relied on `Security.PasswordMasking` having turned it on earlier in the same run: on a
  bench that had not run that fixture first, 18 of its 24 tests failed with "504 Authentication mechanism not
  supported". `AntiSpam/FqdnRefusal.cs`, `AntiSpam/SpamTraps.cs`, `API/RestApiCalendar.cs`,
  `API/RestApiCalendarSharing.cs` and `Security/Tarpits.cs` had the same gap: with the switch off, 13 of their 62
  tests failed, two of them timing out on a 235 that never came - one only after more than twenty minutes, when the
  server dropped the idle connection. Each fixture now turns it on in its own `SetUp` and puts back what it found in
  `TearDown`, as `IMAP/MasterUserImpersonation.cs` does with `IMAPSASLPlainEnabled`.

- **The installer smoke test's line for DBSetup quotes DBSetup.** `installer-smoke.ps1`'s evidence for a
  `--database` run took every line of Inno's log that began "Creating the", and printed Inno's seven "Creating the
  icon." lines instead of DBSetup's; it now takes the unstamped lines Inno logged after "DBSetup.exe /unattended
  returned", and fails when there are none. Pass and fail were not affected.

- **The 6.3.6 SAST triage: what the static analysis found is fixed, not dismissed.** The `static-analysis` job's
  findings were read in seven parts - the network code, the server logic, the core's types, platform layer and dead
  code, the TLS and error-reporting code, numbers held to their range, a written file's close, and members left
  unset - and each part's fixes took its lines out of `build/ci/static-analysis-baseline.txt`. Where a part's fixes
  are described under another heading, the part names them.
  - **The network code: twenty-two findings** are gone from the baseline, two of them on GitLab's Security
    dashboard: the seventeen `dangerousTypeCast` findings on the sockets' timeouts and `SO_REUSEADDR`, which were
    the two defects under Operations and What changes on upgrade, item 16 - the socket timeouts are now set through
    `AcceptLoop::SetTimeouts` or in each platform's own form, and `SO_REUSEADDR` through
    `AcceptLoop::AllowRebindPastTimeWait`, on POSIX only; the two `danglingTemporaryLifetime` findings (High) in
    `AcmeClient.cpp` and `OutboundOAuth2TokenClient.cpp`, whose code the fixes replaced, though the lifetime they
    named was sound, since the string class copies; a `cert-err34-c` `atoi` in the ACME client's own status-line
    parser, which went with it; the empty catch around the start of a connection the REST and web-services listeners
    have accepted (`HttpServer::Accept_`), which now says in the application log that a connection was dropped, for
    the first and then once per thousand with the count; and a `bugprone-unused-return-value` in `HTTPClient.cpp`, a
    plain-HTTP client that nothing in either build called, deleted with the finding. Nothing new is added to the
    baseline. The new tester program, `network-deadline-tests`, runs in `linux-build` and in the Alpine and Nix
    package builds, which run every program whose name ends in `-tests`; the source packages' builds, which refuse
    every socket, leave it out.
  - **The server logic: 30 lines.** Besides the fixes described above (an account's copy, an IP range's priority,
    the seven catch-alls): a message's copy now copies its base, which holds its id, as an account's does;
    `ChangeNotification`, `WorkQueueTaskInfo`, the work queue's running and pending tasks, the firewall's ban,
    `OidcSignIn::Mapping`, the list manager's taken command and the protected messages' lapsed row give every scalar
    member a first value; `DiagnosticResult`'s empty constructor sets its result to false, which a
    `hMailServer.DiagnosticResult` made directly over COM had left unset; and a script object is made with its type,
    which the compiler now requires. None of these was found read unset in the server's own use. The domain-sample
    prune's `struct tm`, whose fill it did not check, now starts zeroed, so that a day that cannot be converted
    prunes nothing rather than read what the memory held. `business-object-tests`, a new tester program that
    `linux-build.sh` and the source packages' builds (`dh_auto_test`, `%check`) run, makes an account's and a
    message's copies and four of these types over memory filled with a pattern, holds every member to what it
    should be, and holds the IP range's priority to what the update takes; with the fixes undone, the account's, the
    four types' and the priority's checks each fail, and the message's fails when a member is dropped from its copy.
    The 30 lines of the baseline whose findings these fix are gone.
  - **The core's types, its platform layer and its dead code: 71 findings.** None of it changes what the server
    does. The constructors of `Singleton<T>` and `BusinessObject<T>` are private and `T` is their friend, so a class
    that named another type as its `T` - a singleton that would be handed that type's `Instance()` - no longer
    compiles; `singleton-tests` holds both, and does not compile against the old headers. `TCPConnection`,
    `IMAPCommand`, `IMacroExpander`, `TCPConnectionFactory`, `NotificationClient` and `IPrerequisite` have virtual
    destructors: every object of theirs was owned by a `shared_ptr` made from its own class, which destroyed it
    whole, and one deleted through the base now would be too. `MySQLInterface` and `GssapiAcceptor`, and four
    classes of the same shape that the analysis did not flag - the database layer's connections and recordsets
    (`DALConnection`, `DALRecordset`), `ByteBuffer` and `FolderManipulationLock` - release a handle, a buffer or a
    lock in their destructors, and cannot be copied; nothing copied one. The unused `StringTreeParser` is deleted:
    nothing included it, its `Parse` never returned a node, and an unmatched quote gave `Mid` a negative length. A
    redundant test in the MIME parameter parser goes, and so do five C-style casts - two in `Unicode.cpp`, three in
    the POSIX build's `BSTR` functions (`hm_platform.h`) - and the code compiled from them is the same as before;
    the `ETRN` reply, whose two `else`s were indented as neither of their `if`s was, and four more places are
    indented as their braces are; and the `timeval`, `tm` and `timespec` of the POSIX `GetLocalTime`,
    `GetSystemTime` and `GetTickCount64`, and two more `struct tm` locals whose fill was not checked
    (`DomainSamples.cpp`, the prune's above, and `ZipWriter.cpp`), start zeroed. 71 findings leave the baseline,
    and the job, run over the tree, finds none that is new. A sweep of the rest of the tree for the same shape - a
    class whose destructor frees, closes, ends or gives back what it holds and that could still be copied - found
    sixteen more, and they cannot be copied now either: the external fetcher's manager, IMAP `APPEND`'s handler, the
    web services listener, the rooms' drain task, the event-script site, the live update's drain guard and the
    Updater's shutdown block, the indexer's second look, a test's key-file guard and seven handle, SID and
    security-descriptor holders of the elevated update stage and the service account's grants; nothing copied one.
    Twenty-two files that no build compiled are deleted: the eleven `CMakeLists.txt` listed as in no build
    (`LoggerFile`, `ServerThreads`, `AccountCache`, `OutboundPortConnection`, `Mutex`, `ISMTPCommand` and
    `SMTPCommandHelp` with their headers, and the empty `Collection.cpp`, `Cache.cpp`,
    `CacheReaderWithDbFallback.cpp` and `TestConnectionResult.cpp`), `IThread.h` and the DKIM COM class's three
    files, which no project named; the list goes from `CMakeLists.txt`, `build/linux-tu-census.sh` and
    `build/check-source-tarball.py`. The Debug build compiles again (`AutoBanFirewall.cpp` used `DEBUG_NEW` without
    defining it).
  - **The TLS and error-reporting code: twenty findings.** Beside the two Operations entries above (the TLS options
    mask; six failures said), the listeners' and the TLS initialiser's seven catches of `boost::system::system_error`
    take it by reference rather than copy it; `CipherInfo` throws its `std::logic_error` by value where it threw a
    pointer that no `std::exception` handler would catch and that leaked (no caller reads an empty `CipherInfo`, so
    it was never thrown); the DNSSEC resolver's socket timeouts and the journal socket's address are passed with C++
    casts; MailboxKeys' reseal record starts with its account id and flag set; and a device's sign-in count
    saturates rather than overflows (What changes on upgrade, item 14). Their lines are gone from the baseline.
  - **Numbers held to their range: ten findings.** The fixes are the filter hook's score and the Sieve quantifier
    (Anti-spam, above) and the configuration file's owner, the calendar's SEQUENCE and PRIORITY and the iteration
    count SCRAM and a password logon read (Operations, above); with them, a POP3 `LIST` sizes its buffer in
    `size_t`, and the POP3 session's destructor calls its own `OnDisconnect` by name, which is the call it made.
    Their ten lines are gone from the baseline. `ini-file-tests` (`platform/ini_file_tests_main.cpp`) is a tester
    program `linux-build.sh` runs with the others: a file another user owns keeps its owner, group and mode when root
    rewrites it; a file read and gone before it is written - the server reads a FIFO whose writer removes it before
    closing it, so the moment is not left to chance - is made as the writer's own, 0600; one made again by another
    user in that moment gives the replacement that user's owner and mode; and two thousand rewrites while another
    user's process removes and remakes the file each leave it root's own, the other user's or absent. The cases that
    need root say they are skipped without it. `scram-tests` (`platform/scram_tests_main.cpp`) holds the rounds
    SCRAM asks a client for to a stored hash's count from 1 to 10,000,000 and the unknown account's 210000 past it,
    as the regression suite's case does on Windows through COM's `Database`, which the Linux suite cannot reach, and
    a password logon to refusing the same hashes.
  - **A written file's close: `file-utilities-tests`, and a third mode of `SimulateSpoolWriteFailure`.** The report's
    `cert-err33-c` at `File.cpp` and the committer's empty catch are fixed, and their lines leave the baseline. The
    tester program holds `File::Close`, `FileUtilities::WriteToFile`, `TraceHeaderWriter`, the message store's
    `WriteAtomically` and the spool a received message is written to (`TransparentTransmissionBuffer`, which must
    count the message as not written, and report it once) to a file that cannot take its last bytes: `/dev/full`, a
    quota of the program's own (`RLIMIT_FSIZE`) and, where it may mount one, a 64 KB tmpfs filled to its last byte,
    in a mount namespace of its own; a container that may not mount says the last case did not run.
    `FileUtilitiesTester`, which the class tests run on Windows, closes a named pipe whose reader has gone, which
    refuses a write as a full disk does. The regression suite's switch `[Settings] SimulateSpoolWriteFailure=3` lets
    every write of a received message land and has its close fail, for SMTP, the two fetchers and APPEND, off unless
    set, like modes 1 and 2.
  - **Members left unset, dead code and tests already implied: forty-one findings.** None of it changes what the
    server does. Twenty-seven structures that are filled field by field - the archive index's and the ZIP writer's
    entries, the SPF macro reader's result, the S/MIME key rows and twenty-three more - and the logger's copies of
    `LogLevel` and `MaxLogLineLen` and the SMTP session's message start time, which their constructors left unset and
    which are written before they are read, start at zero, `false`, a null pointer or an enumeration's first value,
    so a field that a later change forgets to fill holds a known value rather than whatever the memory held; on
    Linux, the `timespec` that the crash record and an OpenTelemetry span read with an unchecked `clock_gettime`
    starts zeroed. A MIME body's part iterator starts at the end of its empty part list, where `Clear()` puts it,
    rather than unset, and `MimeHeader` and `MimeBody` call their own `Clear()` by name in their destructors, the
    call a destructor makes anyway. Dead code goes: `CachedObject`'s default constructor, which nothing called and
    which left its id, creation time and size unset; the three `*_MEDIATYPE` macros, which nothing used and the
    first of which could not have compiled; and two tests already implied where they stand - a public-folder test
    inside the public-folder branch of IMAP `CREATE`, and `== -2` beside `< 0` in the update verifier's
    certificate-expiry check. Their 41 lines are gone from the baseline.

- **[Roadmap3.md](Roadmap3.md): the plan for everything the project had on GitHub.** A new file at the
  repository root maps, row by row, what it takes to bring back on GitLab what a user could download, verify and
  install before 18 September 2026 - the AArch64 `.deb` and `.rpm`, the AppImages, the container image, the
  Sigstore bundles and the build provenance - and to add what the owner asked for beside them: the ARM64 Windows
  installer, the apt and dnf repositories, winget and Chocolatey, and every other channel people install mail
  servers from. It opens with a counter line like Roadmap2.md's, and `build/check-roadmap2.py` now holds both
  files: it fails when either counter disagrees with its rows or either file is missing, checks one file alone
  when given its name, and has a `--self-test`, run by the `checkers` job, whose controls include a wrong
  Roadmap3.md counter beside a right Roadmap2.md one, which the check as it was let through. README.md,
  Roadmap.md, CONTRIBUTING.md, SUPPORT.md, the documentation index and the Feature and merge request templates
  link it. The `.deb` and `.rpm` help pages, which said no Roadmap2 row carried choosing a package by
  distribution, now name the Roadmap3.md row that does.

- **The packages started under their own systemd unit, with systemd as PID 1.** Two new jobs,
  `package-systemd-ubuntu26.04` and `package-systemd-fedora44` (`.gitlab/ci/package-systemd.yml`,
  `build/ci/package-systemd-test.sh`). They take `linux-build`'s `.deb` and `rpm-fedora44-x86_64`'s `.rpm` and run
  each in the distribution's own image with systemd as PID 1. There they check the package scripts' systemd
  branches, `systemd-analyze verify`, and the unit's first start, which must make the STARTTLS certificate. The
  unit's journal must not say that systemd ignored part of its sandbox, and the jobs read the sandbox from the
  running server: `hmailserver`'s uid, `CAP_NET_BIND_SERVICE` alone, no new
  privileges, a seccomp filter, and a read-only system with its two directories writable, a private `/tmp` and a
  `/dev` of its own, another tmpfs than the container's. SMTP on 25 and 587, POP3, IMAP and the REST API must
  answer. Then the server is reloaded, which must replace its listening sockets in the same process, stopped,
  started again and removed while running. Until now no job had started the unit itself, since the install jobs'
  containers have no systemd. The jobs run on linix's privileged runner entry, through podman. The container
  systemd runs in is not privileged: it gets `CAP_SYS_ADMIN` only, and its image masks `systemd-binfmt`, after a
  privileged systemd container removed the host's binfmt handlers on 29 September. The job counts those handlers
  before and after, and fails if any went. Both passed by hand on linix with batch42's packages. Nine negative
  controls each failed at the check they target: an extra capability, `ProtectSystem=no`, `PrivateDevices=no`, no
  first-start step, a reload that does nothing, a stop that cannot end cleanly, systemd's "ignoring" line in the
  journal, no `CAP_SYS_ADMIN` and an unprivileged job container; and an image whose `systemd-binfmt` is not
  masked is told apart from one whose is (`hmailserver/docs/PackagesDeb.md`, "Started under systemd").

- **A clock flake in the two-factor tests, not a server fault.** The Windows gate of pipeline 2887524317 failed
  `TwoFactorAuthentication.ACodeComputedElsewhereIsAccepted` with "The previous step must be accepted": the test
  began at 23:37:29.5 UTC, half a second before a thirty-second step ended, computed the previous step's code in
  step s and had it judged in step s+1, two steps behind the server and outside its window of one step either side.
  `OneTimeCodes.WaitForRoomInTheStep` now returns at once when ten seconds or more of the step remain and otherwise
  sleeps into the next step, and the two tests that judge codes against the server's step
  (`ACodeComputedElsewhereIsAccepted` and `ACodeIsTakenOnceOverCom`) call it after enrolling. The suite's other
  codes come from `OneTimeCodes.Fresh`, which never hands out a step behind the clock, or are refused whatever the
  step (a wrong code, one five steps out, a wrong password or a spent budget), so they needed nothing. The new
  fixture `ServerFreeTests.OneTimeCodesStep` replays the failed run against a clock of its own, without a server,
  in both suites: without the wait the previous step's code is two steps behind (its negative control), with it
  the code is judged in its own step.

- **The ARM64 toolchain check could not find the ARM64 C++ runtime on any machine.**
  `build/ci/packages/check-arm64-toolchain.ps1` looked for it with `Get-ChildItem -Path '...\VC\Redist\MSVC\*\arm64'
  -Filter 'Microsoft.VC*.CRT'`, which with a wildcard in the path returns the `arm64` folders themselves and filters
  their names. It reported the runtime missing on the laptop, whose Build Tools 18.9 has it, and on bench150 in
  pipeline 2887115157, where `windows-packages-arm64` stopped on that report with the ARM64 tools and ATL for ARM64
  present, as 6.3.5's notes below say. The check now asks for `...\arm64\Microsoft.VC*.CRT` as one path. It also no
  longer tells the owner to add components the instance already has: when every component is installed and a file is
  not there, it prints the Installer's `repair` of the instance, or, for the runtime, the `modify --add` of the
  component that the instance's own catalog says installs `Microsoft.VisualCpp.CRT.Redist.ARM64` when the instance
  lacks it. `check-arm64-toolchain.ps1 -SelfTest` holds both to instance trees and catalogs made to order, with the
  old path and the old command as its negative controls; on the laptop the check now exits 0.

- **The command-line packages are checked wherever they change.** `build/check-cli-packages.py`, in the
  `checkers` job, fails when the manifest's version or function list drifts from `Version.h` or the module, or the
  Python package gains a dependency, a version of its own, an install hook, a second copy of `hmctl` or a
  standard-library module newer than Python 3.8, or `hmctl` runs code when it is imported; its `--self-test` breaks
  each rule in a copy. The new `cli-packages` and `cli-powershell` jobs (`.gitlab/ci/cli-packages.yml`; merge
  requests and branches, never a tag) build the sdist and the wheel twice and compare them, run `twine check
  --strict`, install the wheel alone, and run `Test-ModuleManifest` and PSScriptAnalyzer - from the PowerShell
  Gallery, held to its published SHA-512 - under pwsh. PSScriptAnalyzer's 25 findings in `HmailServer.psm1` are
  fixed or justified on the one function, with no command, parameter or confirmation prompt changed;
  `Export-HmConfiguration -Verbose` now says why the ini section is empty when the server does not answer for it.

- **The Linux build includes the distribution's `zlib.h`.** Six core sources included the zlib header vendored
  for `hMailServer.exe` on every platform, so the Linux server was compiled against zlib 1.3.2's header and
  linked against the distribution's library (1.3.1 on Ubuntu 26.04). They include `<zlib.h>` under
  `HM_PLATFORM_POSIX` now; the Windows build compiles the same text as before.

- **An ARM64 publish of the .NET tools no longer takes the x64 COM wrapper from an earlier x64 publish.** MSBuild
  searched each tool's own `publish` folder before the reference's hint path, so `build-tools.ps1 -Platform ARM64`
  after an x64 publish compiled all five tools against the x64 wrapper (warning CS8012) and shipped it, in the
  add-ons' `Addons` folders where nothing replaced it; `check-pe-machine.ps1` found five. The publish folders are
  kept out of the projects' items (`Tools\Directory.Build.props`). CI builds in a clean checkout and never met it.

- **`native-dependencies.ps1 -LibrariesRoot`** holds each linked library's version in an installer's component
  list to the tree the build used, which records its own (OpenSSL's `VERSION.dat`, Boost's `version.hpp`,
  PostgreSQL's `meson.build`); the ARM64 list was checked that way against the ARM64 libraries of 26 September.
  The lists themselves are unchanged.

- **OpenSSL 4.0.3 in the Windows builds, x64 and ARM64** (Security, above). `hMailServer.vcxproj`'s include and
  library paths, `post-build.bat`, `build.ps1`'s ARM64 copy and both installer scripts name `openssl-4.0.3`, and
  `libraries/build-openssl.ps1` pins its archive's SHA-256, `325b5c80...16d9`: the value in Debian's signed
  `openssl_4.0.3-1.dsc`, the archive's own OpenSSL signature checked against the key openssl-library.org names, since
  OpenSSL publishes its `.sha256` files on GitHub only. libpq moves from PostgreSQL 18.3 to 18.6 with it, built
  against 4.0.3 (`postgresql-18.6`, `-arm64` on ARM64, in the same paths and the same installer lines), its archive held
  to `983ee554...ff3f`, the `.sha256` ftp.postgresql.org publishes beside it. Of the fixes PostgreSQL made from 18.4 to
  18.6, the one in libpq itself, CVE-2026-6477 (18.4), is in the large-object functions, which the server never calls;
  the others are in the server, its contrib modules and its command-line programs, none of which the installer
  carries. The DLL keeps its name, `libpq.dll`.
  `Get-LibraryVersions.ps1`, `native-dependencies.ps1` and `dependency-scan` read the version from the project file,
  so the SBOMs and the scan name 4.0.3 with no change of their own.

- **A merge request answers for what it brings.** On GitLab's shared runners, with no secret:
  `build/check-binary-manifest.py`, run by `checkers` in every pipeline, fails on a committed binary that
  `hmailserver/docs/third-party-binaries.json` does not list at its SHA-256 (what `verify-binary-provenance.yml` did
  until 18 September); `semgrep-sast-gate` holds a merge request on a Critical or High `semgrep-sast` finding in a
  file it changes that `.gitlab/security-exceptions.yml`, as the merge request's base has it, does not accept
  (`build/ci/security-gate.py --merge-request`, the release's own rule; OpenSSF Baseline OSPS-VM-06.02); and
  `mr-dependency-check` holds one that adds a dependency version with a Critical or High advisory in OSV's own
  export of NuGet's or PyPI's advisories (`build/ci/mr-dependency-check.py`), what GitHub's dependency review did
  (`.gitlab/ci/mr-checks.yml`; ContinuousIntegration.md, "Merge request checks").

- **The server is built and tested with the newest GCC as well as clang.** `linux-build-gcc` (`.gitlab/ci/gcc.yml`;
  linix; `master` and `batch*`, never a tag) builds the server and its tester programs with the CI image's own GCC,
  Ubuntu 26.04's GCC 15, and runs them and the OpenPGP round trip; `build/ci/linux-build.sh` takes
  `HM_COMPILER=gcc`, and a GCC build makes no packages. The Debian and Ubuntu package jobs already built with GCC
  12, 13 and 14; none built natively with GCC 15. The CI image already carried it through `build-essential`, and is
  unchanged.

- **OpenSSF Scorecard, self-run.** `scorecard` (`.gitlab/ci/scorecard.yml`, the schedule described `scorecard`) runs
  Scorecard 5.5.0 taken from cache.nixos.org by its Nix store path, with every path of its closure pinned by NAR
  hash, not from GitHub, and `build/ci/scorecard-pages.py` publishes its result and a badge at
  https://updates.progressiverobot.com/scorecard/; a result it cannot fetch or read never stops the site's
  deployment. Until the owner's `GITLAB_AUTH_TOKEN` exists it says "not configured" and passes.

- **SonarQube Cloud's scope, and GitHub kept out of a job.** `.sonarcloud.properties` scopes SonarQube Cloud's
  automatic analysis to the project's own code and tests. `.gitlab/ci/no-github.yml`'s `.no-github` puts GitHub's
  six hosts at 0.0.0.0 and `::` inside a job, has the hosts file read first where an image has no
  `/etc/nsswitch.conf`, and refuses a job it cannot protect. ContinuousIntegration.md says what GitLab's free tier
  and Ultimate give and why the Code Quality widget is not used.

- **The channels are checked where they change.** `channels-check` runs every channel script's self-test and proves
  the verification with the pinned cosign itself against `build/ci/channels/fake-sigstore.py`, a Sigstore of the
  jobs' own (a Fulcio-shaped authority with an embedded SCT, a Rekor-shaped log, a trusted root).
  `build/check-channels.py`, in `checkers`, holds the jobs to protected master, by hand, allowed to fail, no ID
  tokens and `.no-github` first, and the PKGBUILD, the AUR's pinned host keys, OBS's `_service` and the Copr Makefile
  to shape.

- **The Windows gate is not failed by an earlier run's crash record, and keeps what places a fault.**
  build/ci/windows-gate.ps1 moves aside and prints the crash-oracle.log the binary before it left, and on a failure
  saves the service's module list with every recorded address placed in it (server-modules.txt) and the Application
  log's crash events (crash-events.txt). On 28 September a gate failed its first test on the record a killed earlier
  gate had left, with every other test passed.

- **Boost for the package builds comes from the job's cache, archives.boost.io or SourceForge.**
  build/ci/packages/fetch-boost.sh makes six attempts at each source and holds every copy to the pinned SHA-256; the
  EL, openSUSE and Debian 12 build jobs cache the tarball under its SHA-256. On 28 September three package jobs of
  6.3.5's pipeline failed because some of archives.boost.io's addresses did not answer the runner.

- **The CI's Python is 3.14, and Renovate no longer proposes a locked requirements file it cannot lock.** Every
  Python job runs in python:3.14-slim-trixie; the locked requirement files are remade for 3.14 (the next entry
  says which versions moved). Renovate's Python merge request of 28 September 2026 had moved twelve versions inside
  build/ci/requirements.txt without resolving or hashing them - click kept its old hashes, and Semgrep 1.178.0
  accepts none of the new versions - so the semgrep job, and with it the release's security gate, would have
  failed. Its pip manager now reads the .in files only, and its merge request says how to remake the locked files.
  Microsoft.NET.Test.Sdk is 18.10.1.

- **The rest of what Renovate proposed, taken where it holds, and `renovate.json` held to what is chosen.**
  Taken: every job image that is not a build target at its tag's digest of 28 September 2026, those of the seven
  jobs added after Renovate's merge request among them (`postgres:16`, since moved to 18, and Microsoft's .NET SDK
  image), and GitLab's Container Scanning analyzer at its tag's digest of 30 September, since GitLab had stopped
  serving the one pinned; uvicorn 0.54.0 and Playwright 1.63.0, the locked files remade with uv; the sixteen updates of
  the `packages.config` test projects (Azure.Core 1.63.0, MSAL 4.90.1, OpenTelemetry 1.19.1, the Microsoft.Testing
  packages 2.4.1, NLog 6.2.1 and the rest); NUnit 5.0.0 in RegressionTests, StressTest and LinuxRegressionTests, which
  nunit3-console 3.22.0 and NUnit3TestAdapter 6.3.0 discover and run as they did NUnit 4.6.1; xUnit.net v3
  (`xunit.v3.mtp-off` 4.0.1) in place of the deprecated `xunit` 2.9.3 in ControlPanel.Tests and ImportTool.Tests;
  Node.js 24 for the `harnesses` job, whose four harnesses give the same results as on Node.js 22; and QuickJS
  2026-06-04 (What changes on upgrade, item 15). Left, each for its reason: `global.json`'s SDK floor of 10.0.303, since
  `rollForward: latestFeature` builds with the newest .NET 10 SDK installed anyway and 10.0.401 would refuse a machine
  with 10.0.400 and Alpine's `dotnet10-sdk` 10.0.303; every Python version that Semgrep 1.178.0 (the newest release),
  OpenTelemetry 1.37.0, pydantic 2.13.5 or Playwright holds where it is - click 8.5, jsonschema 4.26, mcp 1.30 and 2,
  OpenTelemetry 1.45, PyJWT 2.15, pydantic-core 2.49, boltons 26, importlib-metadata 9, peewee 4, protobuf 7, wcmatch
  11, wrapt 2 and pyee 14 - so PyJWT 2.13.0's advisories stay dismissed in the VEX document; sse-starlette 3.5.0,
  googleapis-common-protos 1.75.5 and coverlet.collector 10.1.0, younger than the three days a release waits; the lab's
  MariaDB 11.8, the CI's PostgreSQL 18 and MariaDB 13, the packaging compose file's PostgreSQL 18 and GitLab's Semgrep
  analyzer 7, each a decision and a run of its own, which took all but the last (below, "The CI's databases"); and the
  package builds' distribution releases. `renovate.json` now moves a distribution's image and the lab's images by digest
  only, keeps the CI's databases, the compose file's PostgreSQL and GitLab's analyzers on their major version, no longer
  proposes `global.json`'s SDK, brings an OpenSSL patch release on any day in a merge request of its own, and asks
  Fedora's registry, which lost a lookup on each of three days, one request at a time.

- **Renovate moves the images a job names through a variable: cosign, regctl and the container start test's
  PostgreSQL.** `image: $HM_COSIGN_IMAGE` is nothing Renovate's `gitlabci` manager can read, so the cosign every
  signing job runs in (`HM_COSIGN_IMAGE`) and the regctl the channels' jobs take (`HM_REGCTL_IMAGE`) were pinned
  once and never proposed again - while `container-image.yml`'s `regctl:<version>-alpine`, which
  `build/check-release-chain.py` holds to `HM_REGCTL_IMAGE`'s release, was read, so a new regctl would have
  arrived as a merge request the checkers fail. A regex manager in `renovate.json` now reads
  `HM_<NAME>_IMAGE: "<image>:<tag>@sha256:<digest>"` in the CI files and a script's `HM_<NAME>_IMAGE:-` default
  (`build/ci/container-image-start-test.sh`), and the three move with the other container images, regctl's two
  in one merge request; cosign stays on 2, which the release scripts were written for. The self-tests of
  `check-release-chain.py` and `check-channels.py` read cosign's and regctl's pins from the files instead of
  naming them, so a moved pin no longer fails them. Renovate 44.115.13's lookup dry run found the three at their
  pins; given cosign 2.6.4-dev it proposed 2.6.5-dev's digest and held 3.1.3-dev back; with the old
  `renovate.json` it found none of them.

- **The CI's databases are PostgreSQL 18 and MariaDB 13, the packaging Compose file's is PostgreSQL 18, and the
  test lab's MariaDB is 11.8 LTS.** `linux-suite`, `linux-coverage`, `alpine-suite`, the PostgreSQL upgrade gate,
  `linux-package-deb`, `linux-live-update` and the two container start tests run beside `postgres:18` (18.6)
  instead of `postgres:16`, and the MySQL upgrade gate beside `mariadb:13` (13.0.2, MariaDB's newest stable
  release, a rolling one; its long-term ones are 11.8 and 12.3) instead of `mariadb:11`, which is 11.8 LTS - the version Ubuntu 26.04 ships and the one the lab now
  runs: its MariaDB moves from 11.4 LTS to 11.8 LTS, with `MARIADB_AUTO_UPGRADE` set so that the lab's volume has
  its system tables upgraded at the first start. The Compose file's database moves too (What changes on upgrade,
  item 27). GitLab's Semgrep analyzer stays on 6: the `semgrep:7` Renovate offers is 7.0.0, released for GitLab
  19.0 and withdrawn by GitLab the next day, older inside than 6.26.1 (Semgrep 1.145.0 against 1.174.0), and over
  this tree the two found the same 316 findings. Run on linix in the CI image beside the new servers, 2 October
  2026: the whole Linux suite on PostgreSQL 18.6, 1922 passed, 338 skipped, none failed, of 2260; both upgrade
  gates, 6029 to 6105 and the same schema as a new database, on 18.6 and 13.0.2; the `.deb`'s install test; the
  live-update check, 156 passed; the container start test; and the lab's database-sensitive fixtures on a copy of
  the lab's own volume upgraded to 11.8.9, 227 passed, 100 skipped, none failed. `alpine-suite` and
  `linux-coverage` run the same suite beside the same service, and `alpine-install` already makes its database on
  Alpine's PostgreSQL 18; their first run beside `postgres:18` is a pipeline's.

- **The semgrep job counts a file Semgrep could not parse, or parsed leaving more than ten of its lines unread, as a
  hole in the scan**, as it did a rule that timed out: Semgrep reports both only as a warning, and the job had taken
  `build/ci/semgrep.sh` (not parsed at all), `container-image-sign.sh` (97 of 138 lines unread, every command that
  signs and attests the image) and `scorecard.sh` (117 of 243) as scanned; the three are now written so Semgrep's
  bash parser reads every line.

- **The regression tests that read a mailbox by the order its messages arrived in no longer depend on which delivery
  thread wins.** Each message is in the mailbox before the next is sent (FullTextSearch, which failed 6.3.5's gate
  once with "1 3" for "1 2"; RestApiRecoverDeleted, Discovery, ObjectStorage, and RestApiSelfService's subscriptions
  and keywords tests), and the quota warning, which is queued before the message that crosses the threshold is
  stored, is found by being the one message not sent rather than as the last one. About 290 other tests that deliver
  several messages were read and left as they are: they count, compare sets, find by content, or already wait.
- **`build/check-changelog.py` holds a release section's "Full notes" line to the section's end.** The line that sends
  the reader to the release page must be its section's last paragraph, once, linking that section's own release;
  release-publish ends the GitLab release's notes with the section's text, and a fold of parallel work on 1 October
  2026 had left 6.3.6's in the middle of it. Four self-test cases: the line mid-section, twice, linking another
  release, and not in its form.
- **`build/check-release-chain.py` holds the AppImages' two sources jobs to `.no-github`.**
  `appimage-runtime-sources` and `appimage-bundled-sources` fetch sources whose recipes name GitHub for five Alpine
  libraries; the checker now fails when either stops running `.no-github`'s `before_script` - taken out of its
  `extends`, or replaced by a `before_script` of its own or of a nearer template - and its self-test breaks it six
  ways.
- **A build step can be run with the network refused, on a runner that cannot `unshare`.**
  `build/ci/packages/offline.c` installs a seccomp filter under `no_new_privs` that makes `socket(2)` fail for every
  address family but `AF_UNIX` and `AF_NETLINK`, and `io_uring_setup(2)` fail, then runs the command; every child
  inherits it. The source packages' builds run under it, since linix's unprivileged containers are refused
  `unshare(2)` by Docker's default seccomp profile. It refuses making a socket, not using one: a socket open before
  it starts stays usable, and the jobs' shells hold none.
- **The version stamps and the exclusion list reach the source packages.** `build/check-linux-version-stamp.sh` holds
  the Debian changelog's one entry (`<version>+ds-<revision>`) and the spec's `Version:` and one `%changelog` entry
  to `Version.h`, so stamping a release now changes seventeen files, the APKBUILD among them (RELEASE.md, step 7);
  `build/check-source-tarball.py` holds
  `debian/copyright`'s `Files-Excluded` to the exclusion list plus test data the Debian build does not read, and the
  new build scripts to the list; `build/add-license-headers.py` treats only maintainer-script hooks and `rules` in a
  `debian` or `rpm` directory as scripts, since a changelog or copyright file cannot begin with a comment.
- **`legacy-blowfish-tests` and `check-blowfish-constants.py`.** LegacyBlowfishTester - ClassTester on Windows, the
  new `legacy-blowfish-tests` on Linux, which `linux-build.sh` and the source packages' builds run with the other
  `*-tests` - holds the cipher to Eric Young's 58 published vectors and the scheme to 18 values 6.3.4 wrote and 11 it
  read; `build/check-blowfish-constants.py` recomputes pi's first 8,336 hexadecimal digits and compares them with the
  cipher's 1,042 table words, with a `--self-test` in the `checkers` job; `API/Basics.cs` holds the COM API to the
  same values. The source packages' builds also run `singleton-tests`.
- **The documents say what 6.3.5's release did.** RELEASE.md, SECURITY.md, CONTRIBUTING.md,
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
  [UpdateFeed.md](hmailserver/docs/UpdateFeed.md), [WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md) and
  [EnvironmentConfiguration.md](hmailserver/docs/EnvironmentConfiguration.md) said the release chain had never run,
  dated 6.3.5 the 28th, or described the update-feed trigger. They now give 29 September, what 6.3.5's tag pipeline
  showed and measured, and, in UpdateFeed.md's new "From 6.3.4 on, by the live update", issue #10's test: a 6.3.4
  service running as LocalSystem installed 6.3.5 unattended, without a rollback image since 6.3.4 carries no Sigstore
  bundle; one running as `NT SERVICE\hMailServer` verified it and left it to an administrator (`HM7463`); and the
  Linux live update from 6.3.4, tried the same day (below). RELEASE.md step 3 compares the `hMailServer.exe` line
  `windows-installer` prints with the hash the version's CHANGELOG section gives, since a pipeline release's
  verification section names the installer and not the executable in it.
- **The installer smoke test holds Windows Update off for its run.** Its VM resumes from a snapshot of 25 September
  2026 at every run, and on 29 September Windows Update installed an update and restarted it in the middle of a test.
  `build/ci/installer-smoke-run.sh` now stops and disables the Windows Update and Update Orchestrator services over
  SSH before it copies anything; the snapshot is left alone, so the next revert brings them back, and a failure to
  stop them is reported and does not stop the run.
- **Both Windows package jobs count, and the ARM64 one builds.** `windows-packages-x64` and `windows-packages-arm64`
  may no longer fail without failing the pipeline; a tag's release waits for the ARM64 one, and for
  `windows-installer`, which builds a tag's x64 installer (below, "A tag builds the x64 installer once"). The ARM64
  job's own script ran
  whole on the Windows runner by hand, as LocalSystem, on 29 September 2026: the ARM64 OpenSSL, Boost and libpq built
  into the runner's cache (17, 2 and under 1 minutes the first time), then the server, the tools and the installer in
  under six minutes, and every native file the installer packs checked to be an ARM64 image, but Microsoft's ARM64EC
  `vcruntime140_1.dll`, which an ARM64 process never loads. The runner's `PATH` is its service's own, so libpq's
  build tools are fetched by the job: `libraries/get-libpq-tools.ps1` takes Python 3.14.7 from nuget.org, Meson
  1.11.1 and Ninja 1.13.2 from PyPI and winflexbison 2.5.24 from SourceForge, each held to a pinned SHA-256, into the
  cache, installing nothing. `windows-packages.ps1` named a variable of its Boost step `$tag`, which PowerShell reads
  as its `-Tag` parameter, so every ARM64 build's `build-info.txt` named the tag `vc145`; fixed.
- **The Windows runner refuses GitHub.** bench150's hosts file sends `github.com`, `api.github.com`,
  `codeload.github.com`, `objects.githubusercontent.com`, `raw.githubusercontent.com` and `ghcr.io` to `0.0.0.0` and
  `::`, so that anything a job runs that reaches for GitHub fails by name; both installers built afterwards
  (ContinuousIntegration.md, "bench150").
- **The documents say what the first Linux live update showed.** On 29 September 2026 a 6.3.4 server from the
  `.deb` on Ubuntu 26.04 installed 6.3.5 unattended, and one from the `.rpm` on Fedora 44 by hand over REST, both
  without a rollback image. [UpdateFeed.md](hmailserver/docs/UpdateFeed.md), PackagesDeb.md, PackagesRpm.md, the
  packaging README and [Roadmap3.md](Roadmap3.md) say so where they said it was untried.
- **The documents say what the pipelines have run.** [PackagesDeb.md](hmailserver/docs/PackagesDeb.md),
  [PackagesRpm.md](hmailserver/docs/PackagesRpm.md), [PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md) and
  [PackagesWindows.md](hmailserver/docs/PackagesWindows.md) said no package job had run in a pipeline, and now say which
  have, job by job, PackagesDeb.md and PackagesRpm.md with each job's times. `batch41`'s pipelines of 27 September 2026
  ran the package jobs for the first time: every `.deb` build job passed in 2886955494 and 2887115157, and every `.rpm`
  build job in 2887115157, in which `windows-packages-x64` built the unsigned x64 installer. The nineteen install jobs
  first ran, and passed, in `batch41`'s 2887524317 on 28 September, and every build and install job passed again in
  6.3.5's tag pipeline and in `batch42`'s 2896082335 and 2897966924, which built 6.3.6 and hold each package to its
  licence files. The AppImage jobs first passed in 6.3.5's tag pipeline and, with both sources tarballs, in both of
  `batch42`'s. SourcePackages.md said its seven targets were still to run, of which five have passed and openSUSE Leap
  16.0 and Tumbleweed have not; ContainerImage.md, that `linux-image-arm64` had not run;
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), that no GitLab job had run, and that the
  AArch64 packages' SBOM jobs and the three scans had not; and Nix.md, that the `nix` job had not, which passed in
  2896082335, as Alpine.md now says its three jobs did. The three package documents also said linix's qemu-aarch64
  registration had not been seen to come back after a reboot; it came back at linix's reboot of 28 September, and the
  arm64 jobs ran under it. [Roadmap3.md](Roadmap3.md) says the same in fourteen rows - the chart's package, the
  `.sigstore.json` copies and the provenance among them, all three in 2897966924's rehearsal of the release - and two
  are done: the package install and test jobs pass in a pipeline, and the texts that said what was no longer true now
  say what the tree and GitLab show. RELEASE.md's security release, step 10, said a server from 6.2.28 to 6.3.3 needs
  one `UpdateFeedUrl` line; it is upgraded by hand, once, as these notes say. ContinuousIntegration.md also says the
  GitHub workflows stayed unchanged from `faca023d4` until `6593259e0` removed them, and gives the registry's cleanup
  policy as the API does: it kept two of the five patterns the document listed, and the maintainers added the other
  three through the API on 28 September 2026, before the policy is ever switched on; it and README.md say that the fuzz
  schedule and Renovate's schedule and token exist since 27 September. README.md no longer says the project has no ARM
  machine to build the AArch64 packages on (the pipeline cross-compiles them), `build/ci/release-chain.py` no longer
  says the pipeline does not build them, and Roadmap2's Windows on ARM row names the CI job it said there was not.
  [PackageManagers.md](hmailserver/docs/PackageManagers.md) no longer says the GitHub workflow built the manifests for
  the releases up to 6.3.3 - it arrived after the `v6.3.3` tag, and no release ran it - nor that the AppImage is
  attached to each release, nor that the Chocolatey identifier `hmailserver` is free: it holds upstream's hMailServer
  5.6.8.2574, unlisted, and publishing under it needs the project to become one of its maintainers; its table no longer
  says the workflow ran on every release, and its walk for the first Chocolatey push says the push waits for that. The
  documents that sent a reader to `6399e08a5` for the removed GitHub workflows now give `faca023d4`, the same commit as
  `master`'s history holds it: `6399e08a5` is its copy from before its branch was rebased, which `master`'s history does
  not hold.
- **`backup-tests` and `trust-anchor-tests`**, two more tester programs `linux-build.sh` runs, and the source
  packages' builds too (`dh_auto_test` and `%check`, [SourcePackages.md](hmailserver/docs/SourcePackages.md)); both
  run in the class tests on Windows as well.
- **`connection-string-tests`, `language-tests`, `live-log-tests` and `profiles-tests`**, four more, run the same
  ways: the PostgreSQL connection string read back by libpq and the bytes MySQL's values are handed over in, with its
  database's name quoted, COM's languages across a `ProgramFolder` change, the live log switched on and off beside
  four logging threads - the harness a ThreadSanitizer build runs, whose header says how - and a profile verb asked
  for on the thread that holds the profiles' lock. All but `live-log-tests` run in the class tests on Windows too,
  beside the SQL Server connection string read back by ADO.
- **`DBSetup.Tests`**, run by the `windows-dotnet` job beside the Import Tool's: how `DBSetup /unattended` reads its
  `HM_DB_` variables - the quotes, backslashes and semicolons it takes and passes on as given, and the values
  `hMailServer.ini` or SQL Server's connection string cannot carry, refused before anything is changed.

- **No CI script reads a long or still-running output through `head` or `grep -q` under pipefail.** A reader that
  stops at what it wants closes the pipe, and a command still writing then dies of SIGPIPE, which `set -o pipefail`
  makes the pipeline's status, 141 (linux-sanitize failed so in pipeline 2887524317). Measured in WSL, 20 runs each:
  a command that writes a line at a time piped into `head -n 2`, as `syft version` is in container-image-sbom and
  linux-sbom, failed 20 times; the build scripts' failure summaries, `grep ... | sort -u | head -n 80` over a long
  list, failed 20 times, and under `set -e` ended the AppImage and Nix scripts before their own message. Those, and
  `ldconfig -p | grep -q` in `build/lab/db-suite.sh`, `rpm -qp ... | grep -q` in `srcpkg-rpm.sh`, a shell `printf`
  of a variable into `grep -q` or `awk` in `container-image-start-test.sh`, `appimage-build.sh`, the packaging's
  `build-appimage.sh`, `test-install-kind.sh` and `test-hmailserver-update.sh`, and `helm template ... | grep -q` in
  the chart-and-compose job, which could pass a chart that wrote the Secret it is there to catch, now read a file
  themselves (`grep -m`), read their input to the end (`sed -n '1,Np'`) or search a file or a here-string. The
  scripts run under `sh` without pipefail, and pipes whose command writes a line or two at once, are unchanged.

- **The performance tools run as their pages say**, for the measurements after this release
  ([Performance.md](hmailserver/docs/Performance.md), [PublishedNumbers.md](hmailserver/docs/PublishedNumbers.md)).
  `webmail-timing.py --mode browser` looked for *Search older messages* inside the message list, where 6.3.5 no
  longer draws it, so its search stopped at the first answer and reported no continuation; and in an inbox shown in
  tabs, the default, its select-all and *Mark as read* changed the Primary tab's messages alone. It now presses the
  button under the list, and runs with the inbox in one list, putting the account's choice back afterwards - by a
  new sign-in when its own session has ended while the browser ran, and saying so when it cannot.
  `build/readme-shots/bench.ps1` stopped at its first line under Windows PowerShell 5.1, which the pages' `powershell
  -File` starts. `seed-folder.py` and `webmail-timing.py` take the account's password from `HMPERF_PASSWORD`, as the
  load generator takes its own from the environment. The recipes now take a pinned Python outside the bench's PATH,
  where a `python.exe` would change which tests the next gate runs; turn the logging off, which a gate leaves on; run
  the browser's select-all once, last, with the index on, since it leaves the folder read; wait five minutes after
  each restart; check the index has caught up by a search, not by its count, and leave it a margin after the search
  first says so; measure SMTP out with fsync off, and the Windows bench with fsync off at all; run a warm-up and
  three counted runs on Windows, each with `--server`; give the commands that put back what `prepare-hmailserver
  --cleanup` leaves; and give the seed's bytes for the page's own account, 1,322,787,827. `lab.sh numbers` records
  the served server's logging beside its fsync setting. The seeding, the timing and the load generator were run with
  the pages' command lines - the load generator's with the Linux process's name and data folder - at a pilot's size,
  against a server built from this tree on linix with PostgreSQL: 5,000 messages seeded at 50 a second; each search
  three requests with the index off and one with it on; the browser's search followed twice where it had stopped, and
  its *Mark as read* changed all 5,000 messages where it had changed the Primary tab's 2,999; and
  `prepare-hmailserver`, `smtp-in`, `smtp-out` and `imap-sessions` with the `/proc` sampler, fsync on and off.
  `bench.ps1 -Up` and `-Down` ran under Windows PowerShell 5.1 and PowerShell 7 against a stand-in for the COM
  object, and the pinned Python and Playwright installed as the page says under 5.1. The lab's comparison, imaptest
  and anything on the Windows bench were not run.

- **ControlPanel.Tests hold Live logs and the Server status page.** Live logs' reading of the server's log and its
  following of the end moved out of the page into `Services/LiveLogTail.cs`, unchanged, so that tests hold them:
  whole lines only, a tail begun in the middle of a line, a rotated log, following only while the end is in view,
  and the line being read held in place as the oldest are dropped (13 tests; the real page, run in-process before
  and after, follows, holds and trims the same). The Server status page's captions and values are held to be text
  that can be selected and copied (4 tests). `build/check-localisation.py` has a `--self-test`, which the
  `checkers` job runs: lines and translations made to pass and to fail its rules, among them each shape it missed
  before 6.3.6.

- **`build/check-db-scripts.ps1` refuses a SQL Server upgrade statement marked `[IGNORE-ERRORS]` whose failure ends
  the transaction.** On SQL Server a failed `ALTER TABLE ... DROP CONSTRAINT` or `CREATE INDEX` rolls back the
  walker's transaction - DBUpdater's, which holds the whole upgrade, among them - whatever the marker and
  `XACT_ABORT` say, and the statements after it then commit one by one, the version row among them; a failed
  `DROP INDEX` does not (measured on SQL Server 2022). The 6107 step's script began with such a drop, of a
  constraint step 6072 had already removed, and no longer does; step 6072's own, which never failed, is the one
  exception the check knows by name.
- **`channels-check` runs when `.gitlab-ci.yml` changes.** The job that runs every release channel script's
  self-test and proves their verification with the pinned cosign ran only when a channel's own files changed;
  `.gitlab-ci.yml`, which pins that cosign (`HM_COSIGN_IMAGE`), and `.gitlab/ci/no-github.yml`, whose `.no-github`
  the job takes first, now count among them.

- **One Linux suite at a time on linix.** `linux-suite` and `alpine-suite` (`.gitlab/ci/alpine.yml`) share the
  resource group `linix-suite`, so that the two, each hours long, no longer run side by side on linix's runner,
  which takes more than one job at once, and two pipelines' suites no longer share it either.

- **The AppImage runtime's sources outlast a short outage of Alpine's hosts.** `appimage-runtime-sources.sh` tried
  a download three times, and only after a timeout or a 408, 429 or 5xx answer; a connection refused or dropped
  failed the job at once, and the aports fetch and `apk add` were tried once. A failed, timed-out or dropped
  download is now tried again with a growing wait for up to ten minutes, as `appimage-bundled-sources.sh` already
  did (a 404 is not, and still sends it to `edge/`'s mirror at once), and the aports fetch and `apk add` three
  times over a minute and a half; every file is still held to the SHA-512 its `APKBUILD` gives and every aports
  directory to its commit. `appimage-bundled-sources.sh`'s `apt-get update` retries as its install did. In the
  job's `alpine:3.24` against a loopback server, a download dropped twice and then served arrives, which the old
  function gave up on at the first drop, and a 404 is asked for once.

- **`check-release-chain.py` refuses a third platform's scan or start.** Its rule that the container image is built
  for linux/amd64 and linux/arm64 alone skipped every job named `container-image-start*` and never read the
  `container_scanning_*` jobs, so a `container-image-start-riscv64` or a `container_scanning_riscv64` passed it; both
  are refused now, and the push script's check that each image is for the architecture its job is named for is held
  to the file too. Three new `--self-test` cases, each passed by the checker as it was.

- **`repo-hygiene`'s Pinned-Dependencies reads the included CI files.** It read the images of `.gitlab-ci.yml` alone,
  27 of them, and the installs of `build/*.sh` and `build/ci/*.sh`; most of the jobs are in `.gitlab/ci/*.yml`. It
  now reads the images and installs of every file `.gitlab-ci.yml` includes and of every shell script under
  `build/`: 91 images in 23 files, every one pinned by digest or, for the CI image, by its Dockerfile's hash, and no
  piped download. A `pip install --no-index` of a local wheel, which no index can change, is not taken for an
  unpinned install (`cli-packages` installs its own wheel so). Its `--self-test`, which the `checkers` job runs,
  has four new cases - an image, a service and an unhashed `pip install` in an included file, each caught, which the
  check as it was passed, and a `--no-index` install, passed.

- **Both halves of `build/check-cli.py` run in CI.** The PowerShell module's half - its commands against the recorded
  server, 502 checks - never ran in a pipeline: the `checkers` image has no pwsh, so the run there skipped it.
  `cli-powershell` (`.gitlab/ci/cli-packages.yml`), in the .NET SDK image whose pwsh `psgallery-publish` uses, now runs
  `build/check-cli.py --powershell` after the manifest and analyzer checks, and runs when the checker changes too;
  `checkers` runs `hmctl`'s half (`--python`) by name. Asked for alone, the PowerShell half that cannot
  run now fails rather than passing as skipped. In the job's image on linix the module's 502 checks passed; a route
  planted wrong in `HmailServer.psm1` failed three of them.

- **The arm64 jobs ask whether linix still runs aarch64 programs, and say so when it does not.** On 29 September
  a privileged container running systemd unregistered every binfmt_misc handler on linix as it stopped, and every
  arm64 job after it failed with "exec format error", most before their scripts started. The arm64 and aarch64
  builds of the `.deb` and `.rpm` packages, `appimage-aarch64-compile` and `linux-image.sh` for arm64 now run
  `build/ci/arm64-emulation.sh` first: it writes a 132-byte aarch64 program that exits with 42, runs it, and refuses
  the job by name, with what to do on linix, when anything else comes back. The install and AppImage jobs that run
  in arm64 containers cannot ask for themselves - their shells are aarch64 programs - so the build before each asks;
  `appimage-runtime-aarch64`, which follows nothing, still meets the error itself. Proven on linix in the images of
  the jobs that run it (busybox, dash and bash; kaniko without `/tmp`), and refused in a user namespace with an empty
  binfmt_misc of its own, the host's 31 handlers untouched.

- **A tag builds the x64 installer once.** A `v*` tag's pipeline built it twice on bench150: in `windows-installer`,
  whose file the release carries and `release-authenticode` signs, and in `windows-packages-x64`, whose bytes nothing
  took, at the cost of another build under the gate lock before the ARM64 build, which `release` waits for. The
  maintainers decided on 1 October 2026 that a tag builds it once, in `windows-installer`: `windows-packages-x64`
  refuses a tag, and `windows-packages-arm64` follows `windows-installer` there instead of it (an optional need, which
  a branch still meets). `build/check-release-chain.py` holds both, each with a negative control; the batch and
  master pipelines, and the release rehearsal, still build and collect `windows-packages-x64`'s installer
  (PackagesWindows.md, "One x64 installer for a tag").

- **A cancelled job's compilers no longer break the next checkout on bench150.** In pipeline 2897966924 a cancelled
  build left `cl.exe` and `mspdbsrv.exe` running in the runner's checkout, holding a precompiled header open, and the
  next job's checkout failed until they were killed by hand. Every job on bench150 now runs
  `.bench150-leftovers`' `pre_get_sources_script` before anything is fetched: it stops each process of the runner's
  account named as a compiler, MSBuild, its tracker, the .NET build server or test host or Inno Setup's compiler whose
  working directory - read from the process itself - is the job's own directory or under it, or whose command line
  names it, and logs which and why; never another account's process, the runner, another job's directory or
  `hMailServer.exe`. `build/check-release-chain.py` holds every bench150 job to it. On the laptop, five planted
  leftovers (64- and 32-bit, by directory and by command line, one holding a file open) were stopped and four that
  must stay were left; without the step all five stayed and the open file could not be deleted.

- **`build-openssl.ps1` finds a version Debian's pool has dropped, and cleans up its download.** It takes OpenSSL's
  release archive from Debian's archive, which ships it unchanged, and stopped with "find it on snapshot.debian.org"
  once a version had left the pool; and the download it made stayed in the library folder. It now looks the version up
  through snapshot.debian.org's API and downloads the `.orig.tar.gz` by the SHA-1 snapshot gives
  (`Get-DebianOrigArchive` in `libraries/build-common.ps1`, which tries a failed download again three times and never
  follows a redirect), holds it to the same pinned SHA-256 as before - a file that does not match is deleted and
  refused - and deletes its download once the tree is extracted; a copy given with `-Archive` is still the caller's.
  Under PowerShell 7 and 5.1: 4.0.3 from the pool (x64 and ARM64) and 4.0.1, gone from the pool, from snapshot, each
  extracted with nothing left behind; a wrong digest and a version in neither place refused. The script as it was
  failed all five.

- **The merge request checks, tightened.** `semgrep-sast-gate` printed up to twenty of the tree's unaccepted
  Critical and High findings outside a merge request's changed files in its public log; it counts them now and
  describes none, the release's gate holding them as before. A merge request that changes `.gitlab-ci.yml`, whose
  variables for every job reach the analyzer, or `.gitlab/ci/mr-checks.yml`, which runs the gate, answers for every
  finding in the tree, as one that changes the scan's own file did. `mr-dependency-check` rated an advisory by its
  GitHub label or CVSS 3.x vector alone, so one carrying only a CVSS 4.0 vector held nothing; it now scores CVSS 4.0
  vectors by FIRST's method - the same as Red Hat's `cvss` 3.6 on all 419,904 base vectors with each Exploit
  Maturity and on random ones with environmental metrics - and reads a malformed one, such as `E:AP` or an
  empty value, as no vector, as it does a malformed 3.x one. And
  an advisory it could not decide - a version, or a range's bound, that it cannot order as NuGet or PEP 440 does -
  passed; Critical or High, it now holds the merge request as one that may affect the version. Each with
  `--self-test` cases the checks as they were failed.

- **The container image jobs, hardened.** `container-image-deb` verified each `.deb` and SBOM it took from the
  published release as signed by the release's pipeline, but a signature binds bytes, not a name: another file the
  pipeline signed, served under a `.deb`'s name, verified too. It now also takes the release's signed
  `hmailserver-linux-<version>-SHA256SUMS.txt` and holds each `.deb` to the SHA-256 it gives that name, and each SBOM
  to naming its `.deb`'s SHA-256. Against a mirror of 6.3.5's published files, the release as it is passed, and its
  `.rpm` under the `.deb`'s name and the `.rpm`'s SBOM under the `.deb`'s were refused - both of which the job as it was
  took. `container-image-sign` waits for both start tests, so an image that does not start is never signed. And a
  plain `docker build` of the Dockerfile no longer labels the image with Ubuntu's `24.04` as its version: it says
  `local`, or the version given with `--build-arg HM_IMAGE_VERSION`, which `linux-image.sh` now passes as well as its
  label. `build/check-release-chain.py` holds the three, each with a negative control.

- **The build provenance keeps its one name, and `repo-hygiene` counts it.**
  `hmailserver-<version>.intoto.sigstore.json` is a Sigstore bundle holding the provenance's DSSE envelope, and is
  named as one; OpenSSF Scorecard counts only `*.intoto.jsonl` as provenance, the SLSA generator's name for JSON
  Lines of bare DSSE envelopes, so it counts this file as a signature. Decided: no copy under that name, which would
  misdescribe the file's format to raise a score (SECURITY.md, "Build provenance"). `repo-hygiene`'s Signed-Releases
  check, which read the same name alone, counts it as provenance now, and says why Scorecard does not.

- **Each PPA upload stands alone.** `ppa-upload` sent the release's orig tarball with its first Ubuntu release's
  upload alone, and the next upload only named it: had Launchpad taken the second first, or refused the first, the
  second would have been refused too. While the PPA holds no orig of the version, every release's upload now carries
  it, the same bytes each time, which Launchpad takes again as it is ("Troubleshoot package upload errors": it refuses
  a file it holds only when the bytes differ); once the PPA holds one, none does (`series_flags`, with a
  `--self-test` case for each).
- **`static-analysis` and `semgrep` fail the pipeline on a new finding.** The two source scans, cppcheck and
  clang-tidy and Semgrep's open rules, held their findings to committed baselines but were `allow_failure`, so a new
  finding was a warning in a branch's pipeline and only the release's gate read it. They are not `allow_failure`
  now: a finding new to its baseline, or a scan with a hole, fails a `batch*` branch's pipeline and a tag's, and
  `build/check-release-chain.py` fails while either is `allow_failure` again (a `--self-test` case for each, which
  the check as it was passed). Each passed first over `batch42` with the change folded in, on linix in its own
  image, once what it found new since its last pipeline was read: a cast in the vendored QuickJS header that the
  QuickJS update brought in, upstream's code, which cppcheck now leaves out with the rest of the engine as
  clang-tidy did; a `subprocess.run` with an argument list in `build/check-deck-script.py`, marked where it is; and
  the MD5 of the greylisting key the database-upgrade gate recomputes, in the baseline with its reason. And
  cppcheck is given `_T` as the POSIX build defines it, since it does not read the header the build forces into
  every file from the compile database: two literals joined, `_T("a") _T("b")`, made it give up on a test program's
  file, a hole in the scan that fails the job. A cppcheck finding planted in the report fails the job. `static-analysis` has two hours where it had 90 minutes, since a
  run cut off now fails the pipeline: it took 72 minutes on a busy linix. GitLab's `semgrep-sast` over the same
  tree had 22 High findings that no exception accepted; each was read against the code - a buffer sized and given
  in characters, copies bounded by their arrays, string literals, the system's environment block, and two programs
  started by full path with arguments that cannot be split - none is a defect, and each is accepted in
  `.gitlab/security-exceptions.yml` with its reason, for a year.
- **The three source scans pass over the tree of the tag.** `static-analysis`, `semgrep` and GitLab's `semgrep-sast`
  were run as their jobs run, in their own images, over `batch42` with its last fold, and what each found new since
  was read. Fixed: the Windows update helper's installer-log reader compares a line's beginning without `wcslen`
  (flawfinder's High `strlen` rule; the same answer in all 2,112 cases tried); `build/ci/snap-install-test.sh`,
  which Semgrep had not read at all because of a `case` written on one line, is read whole again; and the semgrep
  job takes a rules pack that semgrep.dev serves as JSON as well as YAML - it served `p/javascript` as JSON three
  times running on 3 October 2026, which failed the job before it scanned anything - and refuses a pack with no
  rule, which it took before. Not defects, each with its reason: two `cert-err33-c` findings in `--schema-version`'s
  output (a diagnostic to stderr and a flush of stdout) and eighteen of Semgrep's unquoted-expansion findings on the
  auto-ban hook's iptables calls, which now wait for the xtables lock, and in its test - a fixed word or a split made
  on purpose - in the baselines; eight more in `hmailserver-update`'s schema reads and in `snap-install-test.sh`,
  words split on purpose and an IFS set to a newline and put back, marked where they are; and six High
  `semgrep-sast` findings, the copies of a name server's socket address in the rewritten DNSSEC lookup, each held
  to its destination's size and family, accepted in `.gitlab/security-exceptions.yml` for a year. The baselines'
  lines for findings fixed in 6.3.6 or moved past recognition are gone. Over the result: 888 static-analysis
  findings and 125 Semgrep findings, none new, and no `semgrep-sast` finding that holds the release.
- **Two more tester programs: `deflate-tests` and `dane-tests`.** `DeflateStreamsTester` holds IMAP compression's
  inflation to the limit the connection asks for - a round trip, a stream of command lines hundreds of times its
  compressed size inflated in pieces no larger than the limit and put back whole, what zlib still holds once the input
  is all taken, a limit of nothing and a corrupt stream refused. `DaneTester` holds outbound DANE to RFC 7672: which
  TLSA records are usable, what a validated set of none of them means, how many records a connection is armed with,
  and TLS handshakes in memory against certificates it makes - DANE-TA authenticating a chain that carries its anchor
  and refusing another anchor, a missing one, another name and an expired certificate, DANE-EE authenticating its key
  alone. Both run in ClassTester and, on Linux, in `linux-build` and in the Debian and RPM source packages' own
  builds; `SURBLTester` (in `public-suffix-tests`) gains the choice of names the URI blocklists are asked about.
  `DaneTester` also holds what a validated MX lookup leaves of each host and whose TLS waives the certificate check,
  and runs `DnssecResolverTester`, which name servers the validating resolver asks of the ones a system lists (an
  IPv6 server when there is no IPv4 one); and `sign-in-budget-tests` runs `SignInStateTester` beside the budget's
  rule: the sealed state of a pending sign-in, its refusals and its one use.
- **The regression suite's DNS zone has a DNSSEC root of its own.** `SuiteDns` signs a root for the run, points
  `DnssecTrustAnchors` at it, and its `FakeDnsServer` answers the root's key and proves every top-level name an
  unsigned delegation unless a fixture publishes a DS for it (`FakeDnsServer.RootSigner`), so that the MX and TLSA
  lookups of every delivery in the suite are proven unsigned as most of the internet's are. `DaneIndeterminateLookups`
  delivers to signed zones whose TLSA lookup answers `SERVFAIL`, nothing and no proof, or records without their
  signatures, and whose MX records come without theirs - each deferred, nothing sent - beside its controls: a zone
  proving no TLSA records by NSEC and by NSEC3, a domain under an unsigned top-level name, a domain under a top-level
  name the root does not have, a zone signed only with an algorithm the resolver does not follow, and unusable
  records under opportunistic and required STARTTLS. `DnssecRootKeysUnverifiable` (Windows) anchors a root whose
  keys come back unsigned: an unsigned MX answer is then indeterminate, not insecure and not bogus, and the
  application log says why.
- **End-to-end regression fixtures for the DANE-TA and SURBL fixes.** `DaneTaDnssecRequiresTls` sends to a recipient
  whose MX RRset and TLSA RRset are DNSSEC-validated from the suite's own root, under a top-level name of its own, the
  TLSA records DANE-TA (`2 1 1`) only, and the host offering no STARTTLS: the message is deferred naming the records and
  the session is a TLS-RPT failure, not delivered in the clear (it runs on the Linux suite too). `AntiSpam.Basics`'s two
  SURBL fixtures put sixteen links to innocent domains before a listed one - in a multipart message's text part, and as
  hidden links in its HTML part - and require the listed link to be asked about and the message tagged, which the
  pre-6.3.6 sixteen-name, text-first budget did not do. IMAP COMPRESS's own bound is held by `DeflateStreamsTester`
  (above), a unit test with a negative control, and by no end-to-end fixture, since none can tell 6.3.5 from 6.3.6:
  6.3.5 read at most 512 compressed octets at a time, so its backlog never passed about half a megabyte, and its
  copying, a few microseconds a command, is below what serving a command costs - a flood of 200,037 commands took 209 s
  on 6.3.5's code and 271 s on 6.3.6's in the Linux suite.
- **The snap: `hmailserver`, strictly confined, on core24, for amd64 and arm64**
  ([Snap.md](hmailserver/docs/Snap.md)). `snap/snapcraft.yaml` makes it from the release's own Ubuntu 24.04
  `.deb`, unchanged, with libpq, libldap, the MariaDB client and 7-Zip from Ubuntu 24.04's archive. The service
  (`hmailserver.daemon`) is installed but not started; `snap set hmailserver db.type=... db.host=...` and the other
  keys of the container image's environment, checked by a configure hook, become that environment, and the image's
  entrypoint, unchanged, makes the configuration, creates or upgrades the database and applies the settings at each
  start. No secret is a `snap set` value, since snapd gives a snap's configuration to every command of the snap and
  `snap get` to every user: the hook refuses a key that names a password or a secret, and a `setting.*` or
  `property.*` the server itself counts as a secret by its name (`IniSettingStore::IsSecretName`, its lists compared
  with the server's source at each build), which lets the password policy and a hash's work factors through; the
  database password goes in through `hmailserver.set-database-password` into a root-only file, and the
  administrator's through `sudo hmailserver --set-admin-password`. snapd starts the service as root; it makes the
  three directories directly under `/var/snap/hmailserver/common` and gives them to `snap_daemon`
  (`system-usernames`) - nothing inside them, since snapd's AppArmor profile gives a snap's root no
  `CAP_DAC_OVERRIDE`, `CAP_DAC_READ_SEARCH` or `CAP_FOWNER` - and then runs the rest through `setpriv` as
  `snap_daemon`, with `CAP_NET_BIND_SERVICE` as its one capability and `no_new_privs`, as the `.deb`'s unit runs it
  as its own user; the account makes what is inside its directories, and the first start applies a new
  installation's defaults as the unit's does. The snap carries Ubuntu 24.04's `timeout`, which the entrypoint runs
  while it waits for the database (`db.wait-seconds`) and which the profile does not let it run from core24, and
  sets `PGGSSENCMODE=disable`, so that libpq does not look for a Kerberos credential the snap cannot have. `sudo hmailserver <verb>` runs the
  server's own verbs as `snap_daemon` against the snap's configuration. The new `snap-build-amd64` and
  `snap-build-arm64` jobs (`.gitlab/ci/snap.yml`) build it on linix after `deb-ubuntu24.04-<arch>`, in `batch*`
  pipelines and on `master`, never for a tag: snapcraft and core24 come from the Snap Store at a pinned revision,
  held to the SHA3-384 of the store's signed assertion and unpacked without snapd (`build/ci/snap-tools.sh`), and
  snapcraft packs in destructive mode in the job's Ubuntu 24.04 container - no LXD, no remote build, no
  privilege, nothing from GitHub. `snap-upload` now takes both jobs' snaps and runs on linix with the same snapcraft
  instead of on the privileged runner. `build/ci/snap-install-test.sh`, run by hand on linix in a privileged Ubuntu
  24.04 container with systemd and snapd (binfmt and AppArmor kept out of it, the host's handlers and profiles
  counted before and after), installed the amd64 snap and passed 62 checks: the refused `snap set`s, a database
  named only in the configuration file, SMTP, POP3 and IMAP with STARTTLS, the server as `snap_daemon` with that one
  capability, REST, the update check's `snap` kind, and a clean `snap remove --purge`; four copies of the snap, each
  with one thing taken out, failed it. With `--remote` the same test runs on a machine of its own and requires
  AppArmor there: in a throwaway Ubuntu 24.04 VM under QEMU on linix, so that no profile reached linix's kernel, it
  passed 63 checks, the server under its enforced profile and no AppArmor denial of the snap in the kernel's log.
  The arm64 snap installed, a MySQL database under AppArmor and the store's review have not run. The Snap Store
  waits for the owner's account and the name (Snap.md, "The Snap Store").
- **`linux-build` fails when its packaging fails.** `build/ci/linux-build.sh` printed `PACKAGING FAILED` and exited 0
  when the version stamps or CPack refused, so the job was green with no `.deb` or `.rpm` in its artifact and only the
  jobs that needed them said so. It now runs its tester programs to the end as before and then exits 4, its last line
  saying the packaging failed (1, with that said as well, when a tester failed too), and the pipeline fails there.
- **The PowerShell module is held to ASCII.** `build/check-cli-packages.py` fails `checkers` on any byte of
  `HmailServer.psm1` or `HmailServer.psd1` that is not ASCII, a byte order mark included, naming the line and column:
  a release signs both with Authenticode, and Windows PowerShell 5.1 reads a script without a byte order mark in the
  machine's own code page, so a character beyond ASCII would leave the signature valid on one machine and not on
  another. Both files are ASCII today; `--self-test` breaks each and the rule reports it.
- **The internal suite runs to its end on Windows again.** `ClassTester`, which the Windows gate runs whole
  (`MainOperations.TestInternals`), stopped at its 18th part of 130 on Windows from the fold of the FQDN refusal on:
  `FqdnCheckTester` expected `192.0.2` to be judged as Linux judges it, and Windows' parser read it as an address
  (What changes on upgrade, item 42). Past that it stopped again at `SignatureHtmlSanitizerTester`, whose expected
  text held raw UTF-8 copyright signs in a file with no byte-order mark, which MSVC reads in the ANSI code page: two
  characters each on Windows, one on Linux. The expected text is written with escapes now, and so are the three narrow
  literals of `CharsetTester`, which a code page other than 1252 would have changed; no other literal of the server's
  C++ was written so. On bench150, under a copy of `ClassTester` that runs every part whatever the one before it did,
  127 of 130 parts passed at batch42's d4410eae4 and all 130 with these fixes; the third failure, `AdminUpdateTester`,
  was the bench's own, which had not been given the release installers it checks (`build/fetch-installer-fixtures.ps1`
  has been run there). Two checkers keep it so, each with a self-test and each failing on the tree as it was:
  `build/check-address-parsing.py` (an address in text is read by `IPAddress::Parse`, never by Boost's `make_address`)
  and `build/check-source-literals.py` (no character beyond ASCII written raw in a literal that MSVC would read
  otherwise). `IPAddressTester` holds both platforms to one answer for every form - each older IPv4 form, brackets,
  ports, zones, a NUL inside the text - and runs in `public-suffix-tests` on Linux too.
- **`Security/AutoBanWidenCom` passes on Windows.** `AHookThatHangsCostsOneTimeoutNotOnePerAddress` failed on bench150
  at every run: its first half took back, with its ban, the spam trap and the IP range the fixture's set-up had made,
  so its second half's trap hit was refused as an unknown user and never banned, and there was no ban to widen. Each
  half now takes back only its own ban. The fixture's new `AHookThatHangsHoldsUpNoBanDecision` holds the fix under
  Operations: against the server without it, it fails at 7.7 seconds.
- **The whole Windows gate ran on bench150, and each of its failures is answered.** On this branch's 57a86f59f, before
  it took in batch42's later folds, it ran 4,018 tests: 3,939 passed, 75 were skipped as on every bench run, and 4
  failed, `TestInternals` and `AutoBanWidenCom` among those that passed. Two were `RestApiHelp`'s, whose
  `HelpPagesData.cpp` had not been made again from the changed AutoBan.md and SmtpPolicy.md, which the release does.
  `AutoBanNeverBanNamesCom` expected the refusal of `*.nbn-com.test` to name `.nbn-com.test`, and every star was
  answered with the one example `.example.net`: the refusal now names the pattern that was meant when the check would
  take it, and the example otherwise. `RestApiProfiles.AnEventScriptRunByARevertCannotReenterTheProfiles` failed
  whatever the server answered: its script reported through `EventLog`, which the load that runs a script's top
  level when scripting is switched on does not give it, so every write failed under `On Error Resume Next`; the
  script writes to a file of its own now, and its sixteen runs on the reverting thread are each refused with the
  sentence (with the refusal taken out, each was told "An error occurred processing the request."). After the
  merge, `build/check-address-parsing.py` found `SignInBudget.cpp`, folded since, reading addresses with Boost's
  `make_address`; its two readings of an address read as `IPAddress::Parse` does (What changes on upgrade, item 42).
  And `IniFileSettingsTester`'s swap under load, which runs on Windows alone, has five minutes rather than one for
  its thousand publishes: beside the laptop's builds bench150 made 915 and 813 of them in the minute in two runs and
  1,001 in 44 seconds in a third. On batch42 merged with this branch, the copy of `ClassTester` that runs every part
  passed all 133 parts, and `TestInternals`, the 16 fixtures these changes touch (119 tests; one skipped where the
  bench has no `python.exe`) and each change's negative control did what they should.

- **The Linux jobs can name their image by digest, and the snap's credential stops short of `stable`.** Two
  hardening steps from the project's second review of this release, neither a flaw any outsider could reach. Since
  the privileged `linix-systemd` runner entry, a job of a protected pipeline can be root on linix, and so give the
  CI image's tag other bytes, in which the release's x86-64 `.deb` and `.rpm` would then be built, reproduced and
  verified. `build/ci/build-linux-image.sh --push` now pushes the image to the project's container registry
  (`registry.gitlab.com/hmailserver/hmailserver/ci-linux`) and prints its name with its manifest's digest;
  `build/check-ci-image.py` holds such a name, in `.gitlab-ci.yml` and the files it includes, to the Dockerfile's tag
  and a whole `sha256` digest, refuses both kinds of name at once, holds every job that runs in the image to start
  with its `--job` step once the image is named so, and with `--job` stops a job whose image (`CI_JOB_IMAGE`) is not
  named by digest - Docker takes an image named so only as the registry holds that digest, so a tag given to other
  bytes on linix is not run; root on linix rewriting Docker's store underneath is beyond any such check.
  `.gitlab-ci.yml` takes the name, and `.linix` and the three container-image jobs that borrow its image the `--job`
  step, once the owner has pushed the image
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), The image). And the Snap Store
  credential is made with `snapcraft export-login --channels=edge,candidate` and an expiry
  ([Snap.md](hmailserver/docs/Snap.md), The Snap Store): `snap-upload` asks it what it may do (`snapcraft whoami`)
  and refuses, uploading nothing, one that could release to `stable` or of which it is told nothing, and refuses a
  release list of its own that names `stable`, so that `stable` stays a person's step with their own login.
- **The re-check of reading an address the same way on both platforms.** `IPAddress::Parse`, taken out of
  IPAddress.cpp by its own text, read 6,000,000 generated texts as Boost's `make_address_v4` and `make_address_v6`
  read them on Linux before it (glibc 2.43's `inet_pton` and Boost's zone rule), under AddressSanitizer and
  UndefinedBehaviorSanitizer: 490,972 taken, no address read otherwise, and 45,505 differences in the zone alone, each
  Parse's own rule for a zone - stricter than Boost's (an empty zone, a space in one, a number past 32 bits), or a
  name looked up where Boost looked one up for a link-local address only; with leading zeros let through, 489 of
  300,000 differed. The re-check found four things the change had left, each fixed with a test that fails without it:
  COM's address setters for IP ranges, incoming relays and the white list, which stored 0.0.0.0 for a text they could
  not read (Security); `DNSServer`, which the Windows resolver read with `inet_addr` (What changes on upgrade,
  item 42) - `build/check-address-parsing.py` now refuses `inet_addr` and `inet_aton` too, with
  `Utilities::IsLocalHost` marked as the one call that keeps it; the Control Panel's API keys page (Administration);
  and the report of a never-ban entry that is no address, made under the never-ban list's lock (Operations). On
  bench150, with the builder's server files laid over this branch, the five new tests of `Persistence.Limitations`,
  `CustomDnsServer` and `AutoBanWidenCom` failed and their fixtures' 29 others passed (34 tests); on batch42 0840ce7e3
  merged with this branch, 24 fixtures - `TestInternals`, the whole internal suite, among them - ran 238 tests: 237
  passed and one was skipped where the bench has no `python.exe`. `ControlPanel.Tests` passed 1,260 of 1,260; with the
  API keys page's check as it was, ten of its twelve new cases failed. The Linux build of 41f563b3f, this branch
  before it took in 0840ce7e3, made the .deb and the .rpm with their licences checked, and its 33 tester programs
  passed, `public-suffix-tests`' "what is an address" among them. Merged with batch42 0689fab9c,
  `build/check-address-parsing.py` found the reading of `AutoBanCommand`'s ledger that b42-sweep4 brought
  (`CanonicalAddress`, through Boost's `make_address`); it is `IPAddress::Parse` now, and a ledger line is still taken
  only when the address read from it writes back as the line itself. On that tree, on bench150, the seven auto-ban
  fixtures, `MainOperations` (`TestInternals` in 123 seconds), `Persistence.Limitations`, `CustomDnsServer`,
  `WhiteListing` and `RestApiProfiles` ran 113 tests: 109 passed, and four were skipped - three `AutoBanHookRetry`
  cases that are the Linux suite's, and the one where the bench has no `python.exe`. Merged with batch42 76b58148e
  as well, the same fixtures ran 114 tests (`AutoBanNeverBanNames` has one more): 110 passed and the same four were
  skipped, `TestInternals` in 127 seconds. On that tree the Windows build had no warning; the Linux build made the
  .deb and the .rpm with their licences checked and its 33 tester programs passed; the Linux suite's auto-ban
  fixtures passed 38 of 38 (37 of 37 at 0689fab9c); `ControlPanel.Tests` passed 1,260 of 1,260; and every check the
  checkers job runs passed but `generate-help-pages.py --check`, whose `HelpPagesData.cpp` the release makes again.

Full notes: [the 6.3.6 release](https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.6).

---

## [6.3.5](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.5) - 2026-09-29

Everything that landed on `master` after the `v6.3.4` tag, from 27 September 2026 ([compare `v6.3.4` with
`v6.3.5`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.4...v6.3.5)). The schema moves from **6104
to 6105 in one step**, which adds a column to the quarantine's table, `hm_quarantine.quarantinehelo`. What changes
on upgrade without a switch comes first. The operator documents named below are in `hmailserver/docs`, which has
five more of them than it had at 6.3.4.


This is the first release built to be made by the project's GitLab CI from its tag ([RELEASE.md](RELEASE.md), *The
release on GitLab* and *On GitLab, from 6.3.5*). The tag's pipeline builds the Windows installer on the project's
Windows runner after the whole Windows suite has passed there; once the owner has started the `release` job, it
Authenticode-signs the installer through Azure Artifact Signing with an RFC 3161 timestamp, signs every file of the
release with Sigstore as that pipeline -
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`, issuer `https://gitlab.com` -
into a `<file>.cosign.bundle` beside it, and checks every bundle with the server's own `--verify-update` before
anything is published. That identity is the one a 6.3.4 server trusts (`SigstoreVerifier.cpp`, unchanged since
6.3.4, and the verifier that check runs), so the live update of a 6.3.4 server is built to install this release, on
Windows and on Linux. A server from 6.2.28 to 6.3.3 trusts only the GitHub workflow that signed the releases up to
6.3.3, and as it stands its live update refuses this one: install 6.3.4 or this release by hand, and keep
`UpdateAutoDownload` off until then, or on Windows give it the GitLab signer as
[UpdateFeed.md](hmailserver/docs/UpdateFeed.md) describes. What the release carries, and what the new package jobs
leave in CI only, is under Operations.

### What changes on upgrade without a switch

1. **The Control Panel's sidebar is in the order a new administrator needs.** Welcome and Dashboard come first,
   then a new group, **Set up this server**: the **Setup checklist**, which was *External setup* under Monitoring &
   troubleshooting, then Setting profiles and DNS records. Two groups are renamed - *TLS & certificates* is
   **Certificates & encryption** and *Access & abuse protection* is **Sign-in & access** - and Connections &
   protocols gains a **Webmail** page (Administration, below). The palette (Ctrl+K) still finds the checklist as
   External setup. A page's `nav-<key>` UI Automation id is unchanged; the `navgroup-` and `navrail-` ids of the
   new and the renamed groups follow their new headings, so a UI test that finds a group by its old id must be
   changed.
2. **The quarantine shows the HELO name, and groups.** The Control Panel's Quarantine page and the Control Deck's
   Quarantine view gain a HELO column and group the held messages by the sender's domain unless told otherwise.
   Messages held before the upgrade have no HELO name recorded and are grouped with those that have no HELO
   domain. The Deck now asks before a delete with a Yes or No of its own whose default is No, as the Control Panel
   already did; it used the browser's `confirm()`, whose default is OK. It offers whitelist and blacklist only to a
   credential holding `manage_settings`, and a held message's headers and preview only to one holding
   `view_mailbox`, as the server requires (below).
3. **The webmail's keyboard.** The message list is one Tab stop, where each row put its box, star and marker in
   the Tab order, and the list's keys reach what a row holds. Enter on a button in the mail view presses the
   button; it used to open the message under the list's cursor. A letter typed into the formatted signature is
   text, where `c` opened a new message. Escape closes every modal dialogue as its Cancel does and gives the focus
   back to where it was, the page under a dialogue cannot be reached until it closes, and menus close on Escape.
   The event and task editors close when the page changes. The compose window's *Plain* is **Remove formatting**,
   its tooltip's words. On a phone a row's importance marker is shown and not pressed; the message's More menu
   marks it.
4. **An invitation sent the way Outlook sends one is shown as an invitation.** `GET /api/v1/me/messages/{id}`
   answers `"invitation": true` for a `text/calendar` part whose `Content-Type` carries parameters and which has no
   `.ics` name, where it answered `false`, and the webmail shows the invitation's card with Accept, Maybe and
   Decline (the fix is under IMAP, POP3 and the webmail).
5. **What a script sees.** `GET /api/v1/quarantine` gives each message three more fields - `helo`,
   `sender_domain` and `helo_domain` - and four routes are new. Over COM, `Quarantine` gains `Bulk` (dispid 9)
   and `QuarantinedMessage` gains `HeloName`, `SenderDomain`, `HeloDomain`, `ReadHeaders` and `ReadPreview`
   (dispids 11 to 15), added to the existing interfaces; no existing member or dispid changes. A program built
   against 6.3.5's type library that calls one of them needs a 6.3.5 server.

### Anti-spam, anti-virus and mail flow rules

- **Reviewing the quarantine** (schema 6105; [GitLab issue #1](https://gitlab.com/Progressiverobot/hmailserver/-/issues/1)):
  - **The HELO name** the client gave is stored with each held message. A message held during the SMTP
    conversation records the session's own; one held later, at delivery - by a rule, a block list, an account's
    own threshold, the sandbox or impersonation protection - takes it from the `Received` header this server wrote
    when it accepted the message, and only when that header names this server, so a `Received` line the sender
    wrote is never read. Empty for a client that gave none, for a message that did not arrive over SMTP and for
    every message held before the upgrade.
  - **Grouping**: by the sender's domain, by the HELO name's domain or not at all, each group folding, the group
    with the newest message first, and every column sorting the messages inside the groups. The domain is the
    organization's - the registrable domain by the Public Suffix List - which the server works out, so that every
    page groups a message the same way.
  - **Many at once**: a box on every message, every group and the header, and four actions on a message, a group
    or the checked messages - release; delete; whitelist, then release; blacklist, then delete. Whitelist and
    blacklist write the lists that already exist and no third one: the sender's address on the server's allow
    and block list (the default), or the organization's domain as `.example.com` on the domain white and black
    list, matched on the envelope sender, or on the HELO name when the page is grouped by HELO domain. Several
    messages from one sender or domain make one entry, and an entry already there is changed rather than
    doubled. The dialogue first shows what the server would write (a dry run); a domain that is itself a public
    suffix (`co.uk`, a HELO of `localhost`) is written only after a box is ticked, and without it the server
    refuses with 409 and changes nothing. The entries are written before any message is acted on, so a message
    whose entry could not be written - the null sender, no HELO recorded, a full list - is left in the
    quarantine with the reason.
  - **The headers and a plain-text preview** of a held message, read without releasing it: the headers up to
    65,536 characters, and the preview up to 16,384, the text part or the HTML part with its markup, style sheets
    and scripts taken out. Nothing is rendered as HTML.
  - **The Control Deck** is level with the Control Panel: the same groups, columns, boxes, icons and dialogues, one
    request for the checked messages, a delete dialogue of its own with No as the default, and Remove expired.
    Against a server from before this release, the Control Panel releases and deletes one message at a time, and
    its whitelist and blacklist say the server needs upgrading.
  - **REST**: `POST /api/v1/quarantine/bulk` (1 to 1,000 ids and one action, with `dry_run` and
    `confirm_public_suffix`), `POST /api/v1/quarantine/expire`, and `GET /api/v1/quarantine/{id}/headers` and
    `/preview`. The list, the headers and the preview need `view_quarantine`, and the headers and the preview
    `view_mailbox` beside it, since they are the mail itself; `bulk` and `expire` need `manage_quarantine`, and a
    `bulk` that allows or blocks needs `manage_settings` beside it, since it changes who is let in for everyone.
    Every one of them is refused to a credential confined to named domains, as the quarantine's other routes are.
    **COM**: `Quarantine.Bulk` takes and answers the same JSON, and each `QuarantinedMessage` has `HeloName`,
    `SenderDomain`, `HeloDomain`, `ReadHeaders()` and `ReadPreview()`; `Bulk`, `ReadHeaders()` and
    `ReadPreview()` are the server administrator's, as the rest of the quarantine is over COM
    ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md#reviewing-the-quarantine)).
- **A HELO that is an address has no domain.** A client that says `HELO 192.0.2.7`, bracketed or not, and a sender
  `someone@192.0.2.7`, have no organization's domain: `helo_domain` and `sender_domain` over REST, and `HeloDomain`
  and `SenderDomain` over COM, are empty, the message is grouped with those that have none, and whitelisting or
  blacklisting it by domain leaves it where it is and says why: "The HELO name is an address, which the domain list
  does not name." for the HELO, and "The sender's address has no domain to list." for the sender. Handed to the
  Public Suffix List, whose implicit rule takes the last label for a top-level domain, `192.0.2.7` would have been
  the domain `2.7`, and blacklisting it would have refused at `MAIL FROM` every client whose HELO is an address
  ending in `.2.7`.
- Fixed: a rule with a **Readable text** or **Sensitive content found** condition, which 6.3.4 added, read a
  message's HTML part in time that could rise with the square of its size. The part's entities were unwrapped by
  a replacement that moved the rest of the text once per occurrence, so 400,000 `&amp;` - two million characters -
  held the delivery for about a minute, and on a server with such a rule anybody could send one. Each entity is
  now unwrapped in one pass into a new string, with the same result (`ContentPolicy::StripHtml`). The quarantine's
  preview ends in the same function, and takes the style sheets, scripts and head out of an HTML part in one pass
  too, so a held message's preview costs time in proportion to its size.

### IMAP, POP3 and the webmail

- **The webmail, audited on a phone and a desktop, light and dark, by keyboard and through the accessibility
  tree**, with a seeded mailbox, and what it found fixed. On a phone: the search box has room to type and its
  options are a sheet the width of the screen; the tour skips a step whose target a phone does not draw and keeps
  its card on the screen; help covers the page instead of squeezing it; the settings tabs are one line that
  scrolls; and everything a finger presses is at least 44 pixels each way. By keyboard: the changes above, a focus
  ring on about twenty kinds of control that had lost it, and the earlier messages of a conversation and the
  Storage page's rows opened by keyboard as well as by mouse. For a screen reader: an unread row says Unread, a
  narrow list's tabs are named with their unread count and are a tab list, the row under the list's cursor is
  said, and the hidden page headings are translated. Contrast is raised where it fell short: the initials on the
  avatar colours, white on the dark theme's pale accent and danger colours (dark text now), the placeholders, and
  the light theme's warning colour (5.5:1, from 4.3:1); and the page declares its colour scheme, so that the
  browser draws its own parts of a control for the dark theme. Also: a long attachment name is cut with an
  ellipsis, and the week and agenda views name their month.
- **The webmail heard through a screen reader.** Every view was walked with NVDA 2026.2 in Chromium, in English
  and German, and what it said fixed. The message list is one control that reads each message as one line, its
  state first ("unread, starred, Grace Hopper, Re: The migration plan, Sep 26, has attachments, 3 of 51"), where
  Tab used to read all fifty rows and each row's controls; Home, End and the list's own keys work in it, Enter or
  `o` opens a message at its subject and Escape comes back to the same row, and Shift+F10 opens a message's menu
  from the keyboard. The star says whether it is pressed. What happens without the focus moving - a toast, once,
  not its countdown; a search's count; an attachment added; a new calendar month; a send that failed after the
  undo delay, which now also reopens the message and says why - is said in one place, and `z` presses a toast's
  Undo. Address suggestions, the command palette and mentions say the suggestion Enter will take; folders say
  their unread count in words and a subfolder its parent; the current folder, page, tab and view are said;
  dialogs and help open at their headings; the logo, the avatars' initials and a duplicate heading are hidden
  from a screen reader. 82 texts that were English on a translated page are in all 23 languages, and the server
  has a browser check the webmail's language file on each use instead of keeping it for a day, so new texts are
  not left in English after an upgrade. A dialog's name is said twice as the focus enters it, and a colour
  picker's name is not said; both are NVDA's reading of what Chromium exposes. Roadmap2 row 43 is done.
- Fixed: an invitation whose calendar part carries parameters in its `Content-Type` and no `.ics` name - the shape
  Outlook sends - was never marked as an invitation, so the webmail showed no card for it. The part's media type is
  now read without its parameters, as the server's iMIP code already read it. 6.3.4 had it too.

### Administration

- **Every database setting that should have an editor has one in the Control Panel.** 146 of the 314 settings the
  server keeps in its database had no editor in the Control Panel, and 137 of them needed one: the webmail's
  sign-in (passkeys, OpenID Connect, SAML, account recovery), its sessions and limits, OpenPGP, Web Push, the
  alerts, the SNMP agent, the audit trail, outbound webhooks, the spam traps, the complaint feedback loops, the
  reputation watch, country blocking, compromised-account detection, the object store, the managed lists and the
  migrations. They are now on the pages an administrator looks for them on - new tabs on Delivery of e-mail,
  Anti-spam, Auto-ban, Logging, Performance and Advanced - and a new **Webmail** page, under Connections &
  protocols, holds the webmail's own: Sign-in, Browser sessions, What people may do, Encryption, Notifications and
  Import. The 34 a COM property carries are edited over COM. The other 103, which the server reaches over its REST
  API alone, are read and saved over that API, which is off as shipped (`RestApiPort` 0): with it on, the Control
  Panel signs in to it once with the credentials it signed in with, asks for the administrator's one-time code once
  when there is a second factor, and sends only the keys that changed; with it off or out of reach, each such tab
  says so, and why. The domain dialog gains Country blocking, OpenPGP and Webhooks tabs, the account dialog an
  Identity tab (a person, a room or a piece of equipment, and the external ID SCIM matches on), an external account
  the options for a mailbox several people share, and a managed list's member how they receive the list's mail.
  `build/check-db-settings-coverage.py` now fails any database setting with no editor and no recorded reason for
  having none; the other nine have such a reason.
- **The Control Panel, audited page by page** in both themes at two window sizes, with each colour measured. The
  server status at the top right is a filled box with no outline - Connected is green on a soft green fill,
  6.0:1 in the light theme and 5.9:1 in the dark - and every status and notice is at 5.5:1 or better; High
  Contrast keeps a one-pixel outline. The light theme's field hints and captions are at 5.3:1, where they were
  3.3:1. Toasts are on the theme's neutral surface, with their level in the title and a glyph, and a failure - a
  page that could not load, a reinitialise that failed, a page partly saved - has a style of its own and stays up
  for eight seconds. A header or a cell too narrow for its text ends in an ellipsis and shows the whole of it as a
  tooltip. A list's Delete is one style, red words while it can be pressed and grey while it cannot; Incoming
  relays' and IP ranges' Properties and Delete need a selection, as every other list's do. The Domains page's
  alias and list boxes have captions. The **Setup checklist** numbers its items in the order a new installation
  meets them and marks the first one that needs action as the next step, and each item's button names the page it
  opens; the dashboard's attention card numbers and marks its items the same way
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md),
  [ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)).
- **The Control Deck's census** ([DeckParity.md](hmailserver/docs/DeckParity.md)) counts 498 of the 508
  properties the Control Panel writes as writable over REST and reachable from a Deck view; the ten it lists as
  missing are an account's directory fields and its external ID, and the server's country policy. The census had
  missed 16 COM members whose help string holds a `]`.

### Operations

- **Packages for Debian, Ubuntu, Fedora, RHEL and openSUSE, the AppImages and both Windows installers, built on
  GitLab CI** on the project's own runners, never for a merge request, and with no tool from GitHub:
  - **Debian and Ubuntu**: Debian 12 and 13 and Ubuntu 24.04 for amd64 and arm64, and Ubuntu 26.04 for arm64, as
    `hmailserver_<v>-1~<distro>_<arch>.deb` - Ubuntu 26.04's as `hmailserver_<v>_arm64.deb`; the amd64 package for
    Ubuntu 26.04 is the release's own, below ([PackagesDeb.md](hmailserver/docs/PackagesDeb.md)).
  - **RPM**: Fedora 44 and 43, RHEL 10 and 9, and openSUSE Leap 16.0 and Tumbleweed, for x86_64 and aarch64, as
    `hmailserver-<v>-1.<disttag>.<arch>.rpm`, with Boost linked in and every other `Requires` resolved from the
    release's own repositories; the executables are PIE with full RELRO, the stack protector, stack-clash
    protection and `_FORTIFY_SOURCE=2` ([PackagesRpm.md](hmailserver/docs/PackagesRpm.md)).
  - Each `.deb` and `.rpm` is built in its distribution release's own container with that release's compiler and
    libraries (the arm64 and aarch64 ones cross-compiled), then installed in a clean container of the same release
    and architecture, run and removed. They are built for every `v*` tag and `batch*` branch, and on `master` when
    started by hand.
  - **AppImages** for x86_64 and aarch64, built on Ubuntu 24.04, so for a host with glibc 2.39 or later and
    OpenSSL 3, with a runtime of the project's own (`platform/packaging/appimage/runtime.c`) in place of the
    AppImage project's, which is published only on GitHub, and without linuxdeploy; each is run again on
    Fedora 44. They are built for a `v*` tag, and elsewhere when started by hand
    ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md)).
  - **Windows**: the x64 and the ARM64 installer, unsigned, in every pipeline of `master`, a `batch*` branch or a
    `v*` tag, after the Windows suite. The ARM64 job may fail without failing the pipeline, and in this release it
    does: its toolchain check looks for Visual Studio's ARM64 C++ runtime by a path that finds it in no
    installation, so it stops on the Windows runner, which has the ARM64 tools, ATL and SDK libraries, and prints a
    command that changes nothing. The check is corrected for the next release
    ([PackagesWindows.md](hmailserver/docs/PackagesWindows.md)).


- **What the release carries.** The release collects the x64 installer built by its own `windows-installer` job,
  the amd64 `.deb` and the x86-64 `.rpm` that `linux-build` makes (clang, in the CI's Ubuntu 26.04 image), the
  `hmailserver-linux-<v>-SHA256SUMS.txt` of those two, the server's SPDX and CycloneDX SBOMs and each compiled
  file's own `<file>.cdx.json`, every file with its Sigstore bundle (`build/ci/release-chain.py`). The packages
  above - every other `.deb` and `.rpm`, the AArch64 ones, the AppImages and the ARM64 installer - are CI job
  artifacts only, kept for 7 to 30 days: they are not signed, not in the release and not offered by the update
  feed. A server installed from one of the other x86-64 `.deb` or `.rpm` packages is offered the release's by its
  live update, whose dependencies its distribution may not meet; apt or dnf then refuses it, and the server stays
  as it was. One installed from an AArch64 package, an AppImage or the ARM64 installer is told that the release
  carries nothing for it to download.

### Build and tests

- **Reproducible builds on both platforms** ([ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)).
  `hMailServer.exe` builds to the same bytes from a checkout at any path: the Release configuration now trims the
  checkout's root and the libraries' root from every path the compiler writes (`/d1trimfile`), where it trimmed
  only the project's own directory, so that sixteen of the server's source files and 31 Boost headers left their
  absolute paths in the executable. 6.3.4 and the releases before it reproduce, if at all, only from a checkout and
  libraries at the paths they were built at. On Linux a new `linux-repro` job builds the server, the `.deb` and the `.rpm` a second time - from a copy of
  the tree at another path, without `.git` and with other file modes, in another build directory, under another
  umask and time zone - and fails unless all three are byte for byte `linux-build`'s; the release waits for it.
  For that, the packages now have the same modes whoever makes them - their directories 0755, the `.deb`'s
  maintainer scripts 0755 and its `conffiles` 0644 - and the `.rpm` records the commit's time as its build time
  and `reproducible` as its build host. Not claimed: the installer, the Updater and Minidump helpers, the .NET
  programs and the signatures. The release's runbook builds the executable once more from a second checkout
  (RELEASE.md, step 8).
- **Coverage on Linux, with branches.** `build/ci/linux-coverage.sh` builds the server and its tester programs with
  clang's source-based coverage (`-DHM_COVERAGE=ON`, a new CMake option), runs the testers and the whole Linux
  regression suite against that server, and writes llvm-cov's line, region and branch figures; a run of the suite
  that did not reach its end is void. On 26 September 2026 on linix they were 49.01%, 47.23% and 38.87% of the
  Linux build (43.35%, 41.38% and 33.82% before the fixtures below were linked, which were measured in a run of
  their own and the counters merged) - a floor, since the Linux suite runs what its REST shims can drive and
  llvm-cov counts every line of a function. The `linux-coverage` job runs it weekly once the owner creates the
  schedule described `coverage` ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), *The
  coverage schedule*); it has not run on GitLab yet. The Windows measurement of 15 September is in the tree as
  `build/coverage-windows.ps1`, not yet run from it; it stores the suite's resolver over COM before the console
  server starts, as the suite asks since the settings moved into the database. Seventeen class constants the
  server passes to `Format` by reference are defined now (`static constexpr`): six of them kept the instrumented
  build from linking, and the other eleven had the same defect and linked only because the optimiser folded them.
- **Twenty-nine more fixtures run on Linux**, among them DKIM signing, ARC sealing, the OpenPGP keys, message
  recall, shared address books, the IMAP mirror, journaling, per-domain relay and the webmail's footers: each
  already compiled against the REST shims and ran clean on linix, and none was linked. `API/RestApiRouteSweep` is
  new, in both suites: every administrative operation under `/api/v1/` that the server's OpenAPI document
  describes is refused, with 403, to an account without a role, and every read is answered to an auditor without
  a server error. The Linux shims give `Domains.ItemByName[...]` and `DistributionLists.ItemByAddress[...]` as
  indexers, and `Paths.Combine` reads a relative path written the Windows way as one on Linux. The `linux-suite`
  job, which the fixtures lengthen by about 20 minutes, has the runners' four hours now rather than three, and its
  test filter is a variable of its own, `HM_LINUX_SUITE_FILTER`, which the job refuses, so a variable meant for
  another job cannot make the gate run part of the suite.
- **The maintainers' tools**:
  - `build/scan-secrets.py`, which the git hooks and the landing script run: the directory its scanner's container
    writes to is inside a private (0700) directory, locally and over ssh. 6.3.4 made the whole directory 0777, so
    that on a POSIX machine another account could, while a scan ran, read the lines being committed, plant a link
    the writes followed, or put an empty report in place of the scanner's (a Semgrep finding accepted for 6.3.4 as
    a real defect of low severity). And a scan that did not run to its end no longer passes: gitleaks 8.30.1
    writes an empty report and exits 0 when the `git log` it reads fails, so a `--range` naming a commit the
    repository does not have scanned nothing and said nothing was found. Such a range is now refused before any
    scanner is asked, and an empty report is clean only when gitleaks also exited 0 and logged no error.
  - `build/ci/repo-hygiene.py`'s Token-Permissions reads CI rules as GitLab does - every alternative of a
    condition, a rules list at any indentation, tags and `extends` in every form, templates and includes, and a
    job's rules from the last of its `extends` parents, which GitLab merges last over first - and refuses what it
    cannot read; its `--self-test` holds it to 33 cases. Its note on each PostgreSQL password written in
    `.gitlab-ci.yml` names the job whose service container it is.
  - New checkers: `build/check-idl-case.py` fails a COM name that `hMailServer.idl` spells two ways - a type
    library keeps one spelling of a name whatever its case, so a new member spelled like an existing one would
    rename it for every early-bound client - with the seven that shipped that way listed and why;
    `build/check-db-settings-coverage.py` (under Administration). `build/check-deck-parity.py` reads a member
    whose help string holds a `]`, and `build/ci/security-findings.py --reasons` writes the reason each finding
    is known rather than fixed into the baseline.
  - The installer smoke test takes an upgrade that writes `[Security] AdministratorPassword` again as a PBKDF2
    hash, which is the documented upgrade of an older hash, and still requires every other `[Security]` key as it
    was. And it now makes the checks [SecureDefaults.md](hmailserver/docs/SecureDefaults.md) lists for an
    installation on a clean machine: after a new installation, the certificate made at installation (its record,
    its two files, a key only SYSTEM, the administrators and the service can read, RSA 2048 for 825 days naming
    the host), STARTTLS on ports 25, 587, 110 and 143, authentication refused without TLS from the *Internet*
    range and not from *My computer*, the audit rows and the setup checklist; after an upgrade, the ports and
    ranges exactly as they were. Against the signed 6.3.4 installer on a clean Windows Server 2025: a new
    installation passed 59 checks and an upgrade from 6.3.3 73, none failed. The run script follows the VM to the
    address its DHCP lease moved to after a snapshot revert.
- **The CI**: `secret_detection` failed `master`'s first pipeline after 6.3.4 on the 24 vetted test keys and
  fixtures, because a pipeline started by a push scans the checkout as files and names a finding by its path, not
  its commit; `.gitleaksignore` now names each of them that way too. The `static-analysis` and `semgrep`
  baselines are written again from runs over the release's tree in the jobs' own images - 1,165 and 122 known
  findings, 264 and 37 of them with the reason each is known rather than fixed, and every finding no scan reports
  any more removed - and a clang-tidy that did not finish on a file (an abort, a kill) is run again alone and
  fails the job if it still does not finish, where before the file's findings read as fixed and the job passed. The package builds no longer read `$HM_PIPELINE`, which
  in a pipeline where the variable was defined would have refused every build and left GitLab making no pipeline
  for `master`, a batch or a tag; `build/check-release-chain.py` now reads the four package files and fails such a
  rule.
- **The documents, corrected**: 6.3.4's own notes - it was published on 27 September, not 26; the security
  exceptions file accepted 455 findings at the release, not 169; and `ScriptAllowedObjects` first exists in
  6.2.25, so before it an event script could create any COM object. `ServiceAccountName` and
  `ServiceAccountPassword` are no longer used, though still loaded
  ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md)); the update feed's
  site has served over HTTPS since 26 September 2026 ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)); an
  inbound webhook's URL writes an international domain name in its `xn--` form
  ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)), and before 6.3.4's fix the check let a URL outside
  printable ASCII through on Linux and Windows alike, where 6.3.4's notes said Linux refused it; and
  CONTRIBUTING.md's schema-upgrade gate and ContinuousIntegration.md's list of what is still done by hand say what
  the jobs do. Two of these, ServiceAccount.md's and UpdateFeed.md's, were found by the check of the wiki's copies
  ([GitLab issue #25](https://gitlab.com/Progressiverobot/hmailserver/-/issues/25)).

Full notes: [the 6.3.5 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.5).

---

## [6.3.4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.4) - 2026-09-27

Everything that landed on `master` after the `v6.3.3` tag, between 15 and 26 September 2026 ([compare `v6.3.3` with
`v6.3.4`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.3...v6.3.4)). The schema moves from **6040
to 6104 in 64 steps**. Most of what is new is off until an administrator turns it on; what changes on upgrade
without a switch comes first. The operator documents named below are in `hmailserver/docs`, which has 90 more of
them than it had at 6.3.3. This release carries an Authenticode signature and no Sigstore bundle, so every server
installs it by hand: a live update of any version that is offered it says it cannot be verified and downloads
nothing ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)). Its `hmailserver-6.3.4-SHA256SUMS.txt` names every
file of the release but itself and its signature, and is signed with the release tag's SSH key, `hmailserver-6.3.4-SHA256SUMS.txt.sig`;
SECURITY.md, "Verifying a release", has the two commands that check a download against it.

### What changes on upgrade without a switch

1. **The database is the settings store, and the `[Settings]` section of `hMailServer.ini` is its copy** (schema
   6042). At the first start the section's keys are taken into the database, with nothing to run by hand; a line
   edited after the old server last started, including one edited to prepare the upgrade, is taken for an edit and
   put back, so make such a change after upgrading. From then on a line edited in the file is ignored in favour of
   the stored value, named in the error log as `HM5804`, and put back; deleting a line no longer returns a setting
   to its default. Under a service account that cannot write `hMailServer.ini` - `NT SERVICE\hMailServer`, a new
   installation's - the copy is not refreshed and the database's values are used, said once a start in the
   application log. A setting is changed in the Control Panel, over COM (`Settings.SetIniSetting`), over REST (`PUT
   /api/v1/settings/ini/{name}`), or on Linux with `hmailserver --set-setting NAME=VALUE`. A `[SettingsOverride]`
   section in the file is the deliberate way round a database that cannot be read, and is named as `HM5807` at
   every start. No new key is added to `[Settings]` from now on: a new setting is a row in `hm_settings`.
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
   off. A wait this server chose no longer spends a message's retries until the message has been queued
   for longer than its retry schedule spans, so a destination that never stops asking still ends in a bounce
   ([OutboundShaping.md](hmailserver/docs/OutboundShaping.md)).
7. **A backup carries what an account keeps beyond its mail** - the address book, the webmail's preferences,
   scheduled sends, file links, S/MIME keys, calendars, password history, the first-contact memory and every
   message's keywords - and a restore puts them back, with each folder's access grants. Passkeys, calendar and
   address-book shares, deleted mail waiting to be recovered, messages that expire, IMAP annotations and the
   out-of-office memory are not carried, each for the reason `AccountStores.cpp` gives. An archive written by
   6.3.3 or earlier restores exactly as it did, without what it never held.
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
12. **IMAP answers as the RFCs say, where Dovecot's imaptest found it did not.** A client that relied on the old
    answers sees these: a mailbox that does not exist, or a right the account lacks, is refused `NO` rather than
    `BAD`, and a failed SELECT or EXAMINE leaves no mailbox selected; an unknown LIST selection or return option is
    `BAD`; every `BODY[<section>]` without `.PEEK` sets `\Seen`, `BODY[HEADER]` and `BODY[1.MIME]` included; SORT
    uses every criterion, REVERSE reverses only its own, ties go to sequence order and an ASCII string folds ASCII
    letters only (a string beyond ASCII is compared as before), and THREAD ORDEREDSUBJECT and REFERENCES build the
    RFC's trees, so both can order messages differently; a session that is the first to be told of new messages
    counts them as `\Recent` (RFC 3501 2.3.2), where it said `* 0 RECENT`; and an APPEND stores the literal byte
    for byte, where it used to add a CRLF, so RFC822.SIZE of a newly appended message is two octets smaller than
    the same message appended before.
13. **MySQL and MariaDB tables become utf8mb4** (schema 6102). Every table is converted to utf8mb4 under
    `utf8mb4_general_ci`, InnoDB and `ROW_FORMAT=DYNAMIC`, and so is the database's default. Before it, 106 of the
    123 tables were utf8, three bytes a character, so an emoji or a CJK Extension B character in a contact, a
    subject in the message index, a signature or a log line was refused with error 1366 or made a question mark.
    The step rebuilds every table, which takes time on a large database; it needs MySQL 5.7.9 or MariaDB 10.2.2 or
    later and the ALTER privilege on the database, and a step stopped part of the way is finished by running it
    again. So do steps 6041 and 6101 before it, and each now opens with a probe table that needs the same key size,
    so on an older server the upgrade stops at 6041 with error 1071 having changed nothing. Fixed: those two steps
    made their first tables before the index that failed, and MySQL commits DDL at once, so they stopped half done
    and every later run failed on a table already there; a database an earlier build left that way, at 6040 (or,
    after a change of database server between the two steps, at 6100), is put back by hand with the statements
    Upgrading.md gives. Two rows the old collation told apart and the new one does not - on MariaDB 11.8, `straße` and `strase`
    in one address book; on MySQL 8.4, two names that differ only in an emoji - stop it with error 1062 until one
    of them is removed. SQL Server, SQL Server Compact and PostgreSQL only move the version
    ([Upgrading.md](hmailserver/docs/Upgrading.md),
    [MigratingDatabaseBackend.md](hmailserver/docs/MigratingDatabaseBackend.md)).
14. **The upgrade alters the message table.** Schema 6101 adds two columns to `hm_messages`, the table that has a
    row for every stored message: the account an authenticated session submitted a message as, and the domains that
    passed DMARC and SPF when it arrived, recorded as properties of the message and never as headers. On a large
    store the step is not instant on every backend.
15. **The database connection is encrypted when the database server offers TLS.** MySQL and MariaDB through MariaDB
    Connector/C - the client bundled on Windows and the one Ubuntu packages - had never used TLS, whatever the
    documents said; with nothing configured they now use it whenever the server offers it, without checking the
    certificate, and connect without it when it offers none. SQL Server through MSOLEDBSQL 18 and SQL Native Client
    now encrypts the whole session, and tries once more without encryption when the failure reads as TLS; through
    SQLOLEDB, which the server picks when MSOLEDBSQL 18 or later is not installed and no failover partner is set,
    only the login is encrypted unless `TlsMode` is `require` or `verify-full`. PostgreSQL is unchanged. Two
    `[Database]` keys choose otherwise: `TlsMode` (`prefer`, the default, `require`, `verify-full` or `disable`)
    and `TlsCaFile`. The application log says at every start what the connection got, and `--check-database`
    makes one connection and says the same
    ([DatabaseTls.md](hmailserver/docs/DatabaseTls.md)).
16. **The administrator password is hashed again at sign-in.** The Windows installer wrote it as an unsalted MD5
    digest that nothing ever replaced. The first successful administrator sign-in - the Control Panel, COM, REST,
    `hmctl` or the Control Deck - writes an MD5 or salted SHA-256 value, or a PBKDF2 hash with fewer iterations
    than `PasswordHashIterations`, again as PBKDF2; a browser session made under the old hash ends once. A file the
    service cannot write keeps the old value and says so once (`HM7494`). An upgrade seals nothing else: see
    Encryption ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md)).
17. **A COM change to a shared configuration object takes effect at `Save()`.** For routes and their addresses,
    incoming relays, DNS black lists, SURBL servers, remote-domain policies, server messages, blocked attachments,
    SSL certificates and groups, a `put_*` now writes a copy held by the COM object, not the item every session is
    reading, so it is not in effect until `Save()`, and one never saved changes nothing; an object read from the
    collection before the save keeps what it read. A script that set one of these and relied on it without saving
    must now save. The old way let a session read a string while it was being freed, the use-after-free under the
    heap-corruption abort below; a save whose re-read fails is reported as error 7390
    ([Upgrading.md](hmailserver/docs/Upgrading.md)).
18. **Mail labelled ISO-8859-1 is decoded as windows-1252**, as the WHATWG Encoding standard and every mail client
    read it, and ISO-8859-2 to -9, -13 and -15 by their own code pages. There was no ISO-8859 name in the server's
    table at all: a Western Windows server decoded Latin-1 right by accident and decodes it as before, one with
    another ANSI code page now decodes it right too, and on Linux Latin-1 had become replacement characters.
19. **A restore lists the archive before it extracts anything, and refuses an unsafe one whole**: an entry with an
    absolute path, a `..` segment or a link with somewhere to point is named in the backup log and as `HM7540`, and
    nothing is extracted. No backup this server writes has such an entry. The Windows installer's 7-Zip is 26.03,
    fetched from 7-zip.org, in place of 19.00, which predates the fixes for CVE-2023-31102 and CVE-2025-55188
    ([Backup.md](hmailserver/docs/Backup.md)).
20. **Event scripts: a rule's Run function action names a plain function.** A name that is not ASCII letters,
    digits, `_` and `$`, not starting with a digit, is refused as `HM5018` each time the rule fires, in either
    Windows language, VBScript or JScript, where it used to be written into the script's call as it was. On Linux,
    whose JavaScript engine looks a handler up by name instead of building a call, such a name is never evaluated
    and finds no function: nothing is run, and the ERROR log has a Script Error line, not `HM5018`
    ([EventScripts.md](hmailserver/docs/EventScripts.md)). On a JScript server every value a
    handler is given - a password, a remote UID, an error text - now arrives exactly as it was, backslashes
    included (see the fix under Security and identity).
21. **A domain can no longer be named as a directory the server keeps its own files in.** A domain's mail is kept
    in the directory named after it under the data directory, beside the server's stores, so a single-label name
    such as `protected`, `confidential`, `recoverable`, `discovery`, `quarantine`, `files`, `lists` or `blobs` (in
    any case), or `temp`, `events` and `database`, which the Linux packages put there, is refused for a new domain
    and for a rename over COM and REST. A domain that already has one is still saved, and is named as `HM7597` at
    every start and as `domain_name_reserved` on the first-hour checklist: rename it
    ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md)).
22. **A one-time code is taken once.** Over COM and over REST, a TOTP code the server has accepted is refused if
    it comes again, for the administrator and for an account alike (see the fix under Security and identity). A
    script that sent the administrator's password and a code with every REST request signs in once instead -
    `POST /api/v1/session`, and the `hmailsession` cookie from there - or uses an API key; otherwise only its first
    request in each half minute is accepted. The others are refused with 401 and are not counted as wrong codes, so
    such a script does not lock the administrator out. Somebody asked for a code twice within half a minute - to sign in to
    the Control Deck and then to register a passkey, say - waits for the authenticator's next one
    ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
23. **The outbound relay sends its OAuth2 token over TLS only.** A route or the SMTP relay whose host is on
    `OutboundOAuth2Hosts` and whose connection security is none - or STARTTLS optional against a server that does
    not offer it - sent `AUTH XOAUTH2` with the server's own token in the clear. Such a delivery now sends
    nothing in its place, fails with a temporary `450 4.7.0`, and the error log says to set the security to TLS
    or STARTTLS required (`HM5912`); the message waits for the retries the route allows. Exchange Online offers
    STARTTLS on 587, so a route set up as Microsoft documents it is not affected.
24. **A password hash stored before 6.3.4 of a password with a character above U+00FF is refused**, to its own
    password too, and the application log names the account: that hash was of NUL bytes or question marks, and
    any other password of that form opened it (see the fix under Security and identity). An administrator sets
    such a password again, and soon: until then SCRAM-SHA-256, whose client derives its key itself, still opens
    such a PBKDF2 hash to the NUL bytes (Windows) or question marks (Linux) it was made over, which the server
    cannot tell from the password. Every ASCII and Latin-1 password signs in as before.

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
- **Mail in, to a webhook** (schema 6100): an address, or a whole domain this server hosts, is routed to a URL, and
  the mail that arrives for it is posted there parsed - as `multipart/form-data` in SendGrid's inbound parse field
  names or as one JSON document - signed with the outbound webhooks' HMAC-SHA256 recipe and retried on their
  schedule; when the attempts run out a route keeps the message to be downloaded or retried, or returns it to its
  sender. A route may deliver to the mailbox as well. Link-local and cloud metadata addresses are refused to
  everybody, an API key may name only a public address, and plain `http` is taken only to this machine. A URL is
  printable ASCII, and anything outside it is refused on both platforms - fixed before release, when Linux and
  Windows alike put such a URL into the request line as it was. Routes are
  made over REST (`/api/v1/inbound-webhooks`) and on the Control Deck, under a new permission,
  `manage_inbound_webhooks`, that only `serveradmin` holds; there is no COM member and no Control Panel page.
  Nothing is on until a route is made ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)).
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
  entries for the links in a message; held mail summarised daily to the person it was
  held for, once `QuarantineDigestBaseUrl` is set and a domain or an account turns the digest on (off by default)
  ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
- **Mail flow rules** (schema 6063): a domain's own rules, a condition on an attachment's name, and actions to
  add a disclaimer, reject with a text, require TLS, quarantine or warn the sender; and an **outbound content
  policy** - card numbers, IBANs, account numbers and attachment types, to external recipients - as conditions
  of the same rules, with no second engine ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- **Impersonation protection** (schema 6087): protected persons and partner domains per domain, judged on mail
  from outside; off on every domain ([ImpersonationProtection.md](hmailserver/docs/ImpersonationProtection.md)).
- **Attachments opened somewhere safe** (schema 6091): an outside message whose attachment is an executable, a
  script, a shortcut, an archive, a disk image, an office document or a PDF is held until a sandbox service the
  administrator names answers;
  off ([Sandbox.md](hmailserver/docs/Sandbox.md)).
- **Spam traps** (schema 6058): an address that was never real refuses like an unknown user, bans the connecting
  address and, when its domain's SPF passes for that address, puts the envelope sender on the block list
  ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Country blocking** (schema 6052): connections and sign-ins judged by country from a MaxMind database the
  administrator supplies, with loopback and private ranges exempt and a missing database meaning allow
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Sealed mail** handled honestly: a message the server cannot read is no longer gutted by the
  blocked-attachment list or given a clean verdict by a scanner that saw only ciphertext
  ([SealedMail.md](hmailserver/docs/SealedMail.md)). "Cannot read" is judged from the body, never from the
  `Content-Type` label: only a message that really is S/MIME enveloped or signed-data, or PGP/MIME of exactly a
  version part and armoured ciphertext under a session key, named as clients name them or not named at all, is
  spared the blocked-attachment list, the anti-virus strip action, eDiscovery's body search and the rules' body, `content`
  and `sensitive_content` conditions. Fixed before release: as first written, a `multipart/encrypted` header any
  sender could add switched all of those off for the ordinary parts under it, `invoice.exe` included. The
  detached signature of a signed message is likewise left alone only when it is one - the second of the two
  parts of a `multipart/signed`, called and made as a signature is; as first written, any part anywhere that
  typed itself `application/pkcs7-signature` or `application/pgp-signature` was spared the list, whatever it
  was called.
- Fixed: a message held by this server's own outbound shaping - a destination's back-off, a rate or connection
  ceiling, the tenant share, the IP warm-up - no longer sends a server or domain rule's forward, copy or reply,
  or calls its script function, once per held pass. Each pass re-ran the rules, and each queued the new message
  again, so a message held every few minutes for as long as its retry schedule spans sent one copy per pass;
  those actions now run once per counted attempt. An account's own rules, which run once for their recipient,
  are not held back by this, and an account rule's warn_sender now goes when its recipient is reached on a later
  attempt too - it was skipped, as the server's once-per-message notice is
  ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md), [OutboundShaping.md](hmailserver/docs/OutboundShaping.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- Fixed: a mail flow rule's forward, copy, reply or notice to the sender, and an account's own forwarding, no
  longer wait up to a minute in the queue. Each is queued from inside the delivery of the message that caused
  it, after the delivery manager last read the queue, and the manager was not told of it, so it sat there until
  something else woke the manager, a minute later at the latest.
- Fixed: one domain's mail flow rules no longer act on another domain's recipients. A domain's rules ran on the
  whole of every message with a recipient in the domain, and on every message whose envelope sender claimed the
  domain, authenticated or not, so one tenant's delete or reject removed a message for another tenant's
  recipients, and its header, route or bound address applied to their copies. The sending domain's rules - the
  submitting account's domain, and the envelope sender's where its account submitted the message, DMARC passed
  for it, or the message reaches no other domain's mailboxes - still act on the whole message; a recipient
  domain's act on the whole message only when every recipient is its own, and otherwise take its own recipients
  off (a quarantine keeps a copy for them, and a reject mails a hosted sender a notice naming them), copy to them
  alone and leave the message's headers, body and routing alone
  ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md)).
- Fixed: a custom virus scanner whose command line holds a bare `%FILE%` was handed the message file's path
  cut at its first space - and the default data directory, under `C:\Program Files`, has one - so it scanned no
  file, answered with an exit code other than the virus one, and the message was delivered as clean. A `%FILE%`
  outside double quotes is now replaced by the path in quotes; one already inside them, `"%FILE%"` as the
  documented examples write it, is replaced as before. ClamWin's database and temporary folders lose a trailing
  backslash, which escaped the closing quote of `--database="..."` and took the file to scan with it.

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
- **Rooms and equipment** (schema 6101): an account can be a room or a piece of equipment (`kind` over REST, the
  Deck's account editor, `Account.Kind` over COM), which answers the invitations it is sent by itself, at delivery,
  by its booking policy - who may book, a booking window (180 days as shipped), a longest booking, working hours,
  conflicts and recurring meetings - and leaves what it does not decide to its delegates. It believes an organiser
  only from what the server recorded when the message arrived - the account that submitted it, or a DMARC or SPF
  pass for an outside domain the policy names - never from a header. A resource cannot be signed into. The
  directory lists rooms with their capacity, and CalDAV's room finder searches them. The policy is at
  `/api/v1/accounts/{address}/booking-policy`, with no COM or Control Panel page; nothing changes until an account
  is given a kind ([RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md)).
- Fixed: on SQL Server and SQL Server Compact a room's record of whom it accepted a booking from was keyed on the
  UID under a collation that ignores case and width, so a booking whose UID differed from another's only that way
  replaced the other's record, and a delegate's later decline of the first booking told its organiser nothing. The
  UID is now compared exactly on every database, so the first booking keeps its record; the second, which the
  unique index there takes for the same UID, goes unrecorded, and the application log says that a delegate's later
  answer about it will tell nobody.
- **Delegated mailboxes named in the Autodiscover answer**, in Exchange's `AlternativeMailbox` element and only
  to a request signed in as the delegate. Outlook reads that element for an Exchange account and not for an IMAP
  one, so for IMAP clients the mechanism that works is still the `#Users` namespace, which Thunderbird and
  Outlook's IMAP code show by themselves.
- Fixed: a heap-corruption abort under concurrent IMAP `LIST` and folder changes on the same account; every
  loop over a shared collection now iterates a locked copy.
- Fixed: an IMAP command pipelined behind an `APPEND` with a non-synchronising literal was never answered, which
  stopped `mbsync` after one message.
- Fixed: a heap-corruption abort when two IMAP sessions on one mailbox met on the same message's keywords - a
  `STORE` of `$Forwarded`, `Junk` or a label beside a `FETCH`, a `SELECT` or another `STORE`, which is what two
  of Thunderbird's connections, or a phone and a desktop, do. Every session shares the one message object, and its
  keywords were rewritten with no lock; they are now read and changed under a lock of their own, so the abort
  cannot happen. 6.3.3 had it too.
- Fixed: two changes to one message's flags at the same moment - two IMAP `STORE`s from two sessions, a `STORE`
  beside a `FETCH` that sets `\Seen`, or beside the webmail's flag and label routes - could each write back the
  flags and keywords it had read before the other's change, so one change was lost, in memory and in the
  database. The read, the change and the write are now one step under a lock of the message's own, and each flag
  is set and cleared on its own bit. `STORE ... (UNCHANGEDSINCE n)` is tested in the same step; a `STORE` refused
  for too many keywords leaves the message as it was rather than half changed in memory and never written; and a
  flag change no longer writes back a `\Recent` another session has just claimed. As old as `STORE`.
- Fixed: an IMAP `APPEND` whose client went away after sending a message's octets and before the command's
  final line break left that message's file in the data directory for good, with no row, counted by no quota,
  and - with `MessageStoreCompression` on - holding its references to shared parts. An authenticated user could
  fill the volume by repeating it until the server refused mail for everyone. The file is now removed with the
  connection, and one left by a server that stopped in the middle of such an `APPEND` - a crash, a kill, a power
  cut - is removed when the server next starts: every such file has a marker in the temp folder's `IMAPAppend`
  directory until its row is saved, and the file of a marker no row answers goes
  ([MessageStore.md](hmailserver/docs/MessageStore.md)). 6.3.3 had it too.
- Fixed: an IMAP `APPEND` into a folder shared from another mailbox (`#Users`) was measured against the quota
  of the user appending, not of the mailbox that stores it, so a delegate with the insert right could push the
  owner past the limit, and a delegate over their own limit was refused an append to a mailbox with room. It is
  now charged to the owner, as `COPY` and `MOVE` already were. 6.3.3 had it too.
- Fixed: an IMAP `FETCH` of a message the server could not read - one in a mailbox encrypted at rest while its
  owner's key is closed, as a delegate meets it, or a damaged one (`HM7350`) - was answered `OK` with an
  `ENVELOPE` of `NIL`s, an empty structure and empty sections, which clients keep by UID for good, and a
  non-`PEEK` section set `\Seen`. It is now answered `NO [UNAVAILABLE]` for a closed key, with nothing of the
  message sent, and said in the debug log only, not the error log: a closed key is not damage. A damaged one is
  left out and the rest of the range fetched, and the command ends `NO`, as for a message lost from an object
  store, so one damaged message does not stop a whole folder's sync at itself. Every part is read before any is
  sent, so a failure can no longer leave an untagged `FETCH` unfinished
  ([EncryptionAtRest.md](hmailserver/docs/EncryptionAtRest.md)).
- Fixed: nothing bounded an IMAP `LIST` or `LSUB` pattern but the 11 MB one command may hold - a quoted pattern
  goes on over the lines a literal joins - and matching it against each folder cost at least its length on the
  threads every protocol shares: 6.3.3 copied the whole pattern twice for each, and this release's matcher, which
  finds a wildcard anywhere in a pattern, cost its length times the folder name's. So one authenticated client's
  pattern of `*` megabytes long could hold those threads. A run of wildcards is now matched as one and a pattern
  that cannot match stops at once, and a pattern longer than 8,192 characters with its reference - longer than
  any name a folder can have - or more than 32 patterns in one `LIST` is answered `BAD`. `LIST` and `LSUB` also
  read a reference or pattern sent as a literal (`{n}`), as RFC 3501 allows, and the same bound holds for it: they
  matched the literal's length as the pattern, so such a command did not list what it asked for. 6.3.3 had that
  too.
- Fixed: a message whose `Date` header held a backslash - one at its end, say - broke the IMAP `ENVELOPE` it was
  sent in: the backslash escaped the date's closing quote, so a client read the subject and the addresses the
  sender wrote as part of the date and then as the response's own structure. The date's backslashes are left out,
  as its quotes already were. 6.3.3 had it too.
- Fixed: the same for IMAP `BODYSTRUCTURE`: a part's type, charset, file name, boundary, `Content-ID` and
  disposition were put between quotes as the sender wrote them, so a file name ending in a backslash or a
  `Content-ID` holding a quote broke the structure every client parsed, and a folded `Content-ID` ended the
  response line. Their quotes and backslashes are now escaped and a control character is sent as a space; a value
  without any of these is sent as before. 6.3.3 had it too.
- Fixed: on an IMAP connection compressed with `COMPRESS DEFLATE`, what one read inflated past the connection's
  250,000-octet receive buffer was dropped without a word. A few hundred compressed octets of a repeated byte
  inflate to more than that, so such an `APPEND` stored its message cut short, or never finished, and the session
  lost step with the client. The rest now waits and is taken before the connection reads again. 6.3.3 had it too.
- Fixed: a later message of an IMAP `MULTIAPPEND` refused under a `{n+}` literal, or any literal after the command
  had failed, was read and thrown away whatever size it declared, past the 2 GB ceiling too, so a client could
  keep a connection reading for as long as it liked. Declared larger than the maximum message size (and 1 MB),
  it now closes the connection with `BAD [TOOBIG]`, as the first literal already did. 6.3.3 had it too.
- IMAP `COMPRESS DEFLATE` is taken, and `COMPRESS=DEFLATE` advertised, only once the client has signed in; before
  that it is answered `NO`. It was taken in any state, which put the inflater within reach of anyone who could
  open a connection, for no client that compresses before it authenticates.
- Fixed: IMAP `STARTTLS` was taken, and offered, after `LOGIN` and inside a TLS session it had already started,
  where it began a second handshake on the running one. It is a command of the not authenticated state only
  (RFC 9051); it is now answered `BAD` and left out of `CAPABILITY` once TLS is active or the client has signed
  in, as SMTP's `STARTTLS` and POP3's `STLS` already were. 6.3.3 had it too.
- **A folder of a hundred thousand in the webmail**: a page copies one page of the folder, never all of it;
  Oldest reaches the end in one request; a search is bounded by time as well as by files read, spared by
  the full-text index, and goes on from where it stopped across every folder; select-all hands the
  selection to the server (`POST /api/v1/me/folders/{id}/bulk`), which works through it in bounded steps.
  The seeding and timing scripts are in `build/perf/`; the numbers are to be measured on the bench
  ([Performance.md](hmailserver/docs/Performance.md)).

### Security and identity

- **Administrator roles** (schema 6047): `serveradmin`, `domainadmin`, `helpdesk` and `auditor` over 27
  permissions, custom roles, assigned per account and per domain ([Roles.md](hmailserver/docs/Roles.md)).
- **Passkeys and single sign-on** (schema 6077): WebAuthn, and OpenID Connect from the organisation's issuer,
  into the webmail and the Control Deck; **SAML 2.0** beside it (no schema step), with an XML canonicaliser and
  signature verifier of the project's own ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **SCIM 2.0 provisioning** (schema 6096): the identity provider makes, renames, suspends and removes accounts,
  aliases and lists at `/scim/v2` ([Scim.md](hmailserver/docs/Scim.md)).
- **A recovery address and one-time recovery codes**, and self-service password reset (schema 6076)
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Sessions and devices** (schema 6053): every client that signs in is a row its owner and the administrator
  can see and revoke; the session policies start unset, after an upgrade as in a new database, so the
  ceilings the server always had (thirty minutes idle, twelve hours in all) apply until they are set
  ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **A new password checked against published breaches** by k-anonymity, wherever a password is chosen; off by
  default ([PasswordBreachCheck.md](hmailserver/docs/PasswordBreachCheck.md)).
- **Compromised-account detection** (schema 6071), in watch mode as above, with a review queue.
- **Fixed: a password could run code inside the server.** On Windows, with event scripting on, the script language
  JScript and an `OnClientValidatePassword` handler in `EventHandlers.js`, any client that could reach a sign-in -
  SMTP AUTH, POP3, IMAP or anything else that checks an account's password - and name an existing, active account
  could have JScript of its own run inside `hMailServer.exe`, with the rights of the service's account, before it
  had authenticated. That script could create any COM object, `WScript.Shell` among them - before 6.2.25 always,
  from 6.2.25 with `ScriptAllowedObjects` at its shipped `*` - and so run any command the service's account may.
  The server built each call as source text and escaped the apostrophe but not the backslash before it. The same shape was open to a remote POP3
  server through the UID it answers (`OnExternalAccountDownload`), and to a remote mail server through the error
  text it refuses a delivery with and the failed recipient's address (`OnDeliveryFailed`), each on a JScript server
  with that handler; and a rule's Run function action wrote its function name into the call unescaped in either
  language, so whoever may edit a rule, a domain's own administrator included, could do the same. VBScript servers
  were not open through a password, a UID or an error text, and the Linux builds had no script engine. The code was
  there in every release from 6.0.0 to 6.3.3. Every JScript value is now escaped in one helper, and a rule's
  function name must be a plain identifier ([EventScripts.md](hmailserver/docs/EventScripts.md)).
- **Fixed: an app password was given the account's private keys, sealed under the account password.**
  `GET /api/v1/me/smime` answered a request made with an app password every own S/MIME key as the webmail
  wrapped it, under a PBKDF2 key from the account password, as 6.3.3 already did; and, new in this release,
  `GET /api/v1/me/zero-access` the zero-access mailbox key's two sealed copies with their Argon2id salt and
  parameters, and `GET /api/v1/me/openpgp/keys` every own OpenPGP secret key. Whoever held a leaked app password -
  a credential for a mail program, which cannot change any of these - could take the copies away and guess the
  account password offline, and a right guess opened the mailbox key and the account. The three reads now leave
  the keys out, with `keys_withheld`, for an app password and for an administrator's support session, so a webmail
  session started with either cannot open them and says so. A zero-access row's Argon2id parameters
  are held to at least two passes over 19 MiB, where 8 MiB and one pass were taken
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Fixed: an app password could replace or remove the account's own S/MIME certificate and private key**
  (`PUT /api/v1/me/smime/own`, `DELETE /api/v1/me/smime/own/{fingerprint}`), whose wrapped key the store holds the
  only server copy of, as 6.3.3 already allowed; both are now refused to it with 403, as the same writes to an
  OpenPGP key of the account's own always were.
- **Fixed: an administrator's support session could change the account's keys.** A support session acts as the
  person in the webmail so an administrator can help them, but it could store or remove the account's own S/MIME
  certificate and key and a correspondent's certificate, as 6.3.3 already allowed, and - new in this release -
  store or remove an OpenPGP key of the account's own or a correspondent's, mark a correspondent's key verified or
  accept its change, upload, remove or publish the key the Web Key Directory serves for the address, and switch
  zero access on or off or re-wrap its key: planting what the person's mail is then sealed to, or storing the next
  delivery to a zero-access mailbox in the clear for the administrator it exists to keep out. Every one of those
  writes is now refused to a support session with 403 and `code` `support_session`, as the S/MIME authority's
  enrolment already was; the session still reads them, without the wrapped keys. A correspondent's S/MIME
  certificate, and the published OpenPGP key and its switch (`PUT`/`DELETE /api/v1/me/openpgp`,
  `PUT /api/v1/me/openpgp/publish`), are now refused to an app password too, as a correspondent's OpenPGP key
  always was - a leaked app password could otherwise have published its own key for the address. A support
  session can no longer remove or rewrite the webmail's note that an administrator opened the mailbox
  (`support_last`) or the consent (`support_allowed`) in `PUT /api/v1/me/preferences`, and an app password can
  no longer give that consent ([Roles.md](hmailserver/docs/Roles.md), [OpenPGP.md](hmailserver/docs/OpenPGP.md),
  [ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Fixed: a person could collect any Microsoft 365 mailbox with the server's own token.** On a server set up to
  collect Microsoft 365 mail with its own OAuth2 app token (`OutboundOAuth2*` or `OutboundOAuth2FixedToken`), an
  external account for a host on `FetchOAuth2Hosts` logs in with that token as whatever user name it gives. The new
  Other accounts page (`/api/v1/me/fetch-accounts`) let any person add one for `outlook.office365.com` naming
  another person's address, or rename the user on one an administrator had set up, and so copy - and with days to
  keep 0, delete - any mailbox the app registration reaches; a key restricted to named domains could do the same,
  and so could a domain administrator over COM, which had been open since 6.2.23 Alpha 1. Only a server
  administrator now adds or changes such an account. The fetchers also sent a bearer token without TLS when the account's security was none: they now
  send one over TLS only ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a relay that refused the OAuth2 login put the server's token in the bounce.** When the relay answered
  `AUTH XOAUTH2` with a refusal - Exchange Online's `535 5.7.3` for a mailbox with SMTP AUTH off, say - the bounce
  to the message's sender quoted the line this server had sent, and that line is the token in base64: the
  server's own, which sends as every mailbox the app registration was granted and, through `FetchOAuth2Hosts`,
  collects them. The SMTP log kept the line too, and the POP3 collector's log its own. The bounce now reads
  `hMailServer sent: AUTH XOAUTH2 <Token removed>` and the logs `AUTH XOAUTH2 ***`, as they have always left
  out a password; open since 6.2.22-pre1.
- **Fixed: an external account's user name or password could add commands to the collection.** Both are sent in
  command lines - POP3 `USER` and `PASS`, IMAP `LOGIN` - and a CR or LF in either ended the command and sent what
  followed as another, to the host and port the account names; the new Other accounts page let any person set
  them. A user name or password with a CR, LF or NUL is now refused by the REST routes and by `FetchAccount.Save`
  over COM, and the fetchers do not send one saved before.
- **Fixed: a person's other account could reach inside the network after it was saved.** Its host was judged
  public once, when it was saved, by the system's resolver; the server later resolved it again with its own DNS
  and connected to whatever came back, on any port from 1 to 65535. A name that answered a public address at
  the save and 127.0.0.1 or 10.x afterwards (DNS rebinding), or one the server's DNS answered differently (split
  horizon), made the server connect inside its own network every fifteen minutes and send a user name and
  password there. Schema 6104 marks an account a person makes on the Other accounts page, or moves to another
  server or port there, as `reader_created`; the fetcher connects such an account only to the public addresses
  the name resolves to at that moment (`HM5909` when there are none), and a person names port 110, 995, 143 or
  993 - both unless `reader_fetch_accounts_private_addresses` is on. Accounts from before 6104, and an
  administrator's, are not marked and behave as they did ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a host added to `FetchOAuth2Hosts` later gave the server's token to accounts others had named.**
  The save refuses anybody without the server's authority a fetch account for a host on the list, but the
  fetchers judged only the host at collection time, so an account a person, a key restricted to named domains
  or a domain administrator had set up for a host the administrator put on the list afterwards logged in with
  the server's own OAuth2 token as whatever user name it gave. Schema 6104 records whose word the server and
  the user name are (`hm_fetchaccounts.faorigin`): an account such a caller makes, or whose server, port or
  user name it changes, is `server_token_withheld`, and the fetchers log it in with its own user name and
  password instead. Accounts from before 6104 are not marked; a domain administrator's saved before this
  release cannot be told apart from a server administrator's
  ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a Web Push endpoint could point the server inside its own network.** With Web Push on (off by
  default), a person could subscribe an https endpoint at any address - a host on the private network, a cloud's
  metadata address - or plain http to any port of the server's own loopback, and the test push answered why the
  connection failed word for word, which mapped the network behind the server one request at a time. A push now
  goes to a public address only; plain http to the loopback is taken only while `WebPushLoopbackHttpForTests`
  (`loopback_http_for_tests` under `/api/v1/settings/push`), a switch for the regression suite that is off by
  default, is on, and then only from a request made on the machine itself that no proxy forwarded - a reverse
  proxy on the same machine that adds no forwarding header makes every person a caller from the machine; a
  failure is shown to the person as "could not be reached" and its reason written to the application log; and a
  push waits ten seconds at most, a test push five.
- **Fixed: one person could stall the webmail and the Deck for everybody.** The REST listener has four workers,
  and a request that waits on another server - a brand logo not yet known, a one-click unsubscribe, an OpenPGP key
  lookup, a test push - held one for up to its deadline, so one account keeping four of them in flight against a
  host that never answers, a made-up subdomain for each logo, left nothing to answer anybody else. An account now
  has one such request in flight at a time and the server two in all; the rest are answered `429` at once. A
  logo lookup or a key directory that took three seconds or more, in the DNS or at the host and whatever it came
  to, is held against its whole organizational domain by the Public Suffix List for an hour, so the next made-up
  subdomain is answered at once without asking it ([BrandLogos.md](hmailserver/docs/BrandLogos.md)).
- **A new installation is secure before anyone configures it.** A database this installation created - never an
  upgraded or attached one - is given a self-signed certificate for the machine (RSA 2048, 825 days), STARTTLS
  offered on 25, 587, 110 and 143, and TLS required for authentication on the Internet IP range, so that a password
  from another machine no longer crosses the network in the clear; My computer is left as it was. The Windows
  installer does it after the database step; on Linux `--create-database` marks the database and the unit's first
  start within a day does it, and the AppImage and the container run `--apply-new-install-defaults` by hand. The
  setup checklist's new Passwords only over TLS item judges every server, new or upgraded
  ([SecureDefaults.md](hmailserver/docs/SecureDefaults.md)).
- **A dedicated low-privilege service account on Windows.** A new installation runs the service as the virtual
  account `NT SERVICE\hMailServer` instead of LocalSystem; an upgrade keeps the account it has, and the installer's
  page, or `/serviceaccount=virtual|localsystem|keep`, offers the other. `hMailServer.exe /GrantServiceAccess`
  grants the account the program folder's Data, Logs, Temp and Database folders and folders an administrator
  approved, and nothing the configuration merely names. `hMailServer.INI` is read-only to the service, which works
  on with the database's values. What only LocalSystem could do is refused with a sentence: the live update's apply
  (an administrator runs the verified installer), the auto-ban's firewall rules and `Utilities.MakeDependent`.
  `ServiceAccountName` and `ServiceAccountPassword` are retired: `/Register` takes the account from
  `--service-account` alone ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md)).
- **What the first security scan found, fixed**: the 7-Zip and the restore above; the in-process testers' 379 bare
  `throw;` statements, which ended the service when a check failed, now fail the test suite; MIME boundaries drawn
  from a per-thread generator seeded from the operating system instead of `rand()` reseeded from the clock; the
  vendored zlib 1.3.2; and VEX statements for the advisories that do not reach the server.
- **An evidence pack for a buyer's security questionnaire**: where mail and personal data are kept, what leaves the
  server, ISO/IEC 27001:2022 Annex A and SOC 2 mapped to what the product provides, and the supply chain, each fact
  with the file it can be checked in and each gap named ([EvidencePack.md](hmailserver/docs/EvidencePack.md)).
- **Fixed: SCRAM-SHA-256 signed in with a password every other path refused.** SMTP `AUTH`, IMAP `AUTHENTICATE`
  and POP3 `AUTH` with `SCRAM-SHA-256` (or `-PLUS`) accepted the account's own password for an account with a
  second factor enrolled - whose own password is otherwise refused in every mail client, app passwords being
  the only credential - and a password past `PasswordPolicyMaximumAgeDays`, and never asked the domain's or the
  account's country policy: SCRAM proves the password without reaching `PasswordValidator` or
  `AccountLogon::Logon`, where those rules are. So a phished password of an account with TOTP could submit mail
  as it and read its mailbox. The second-factor and expiry gaps were in 6.3.3 as well. Each is now a forced
  failure of the exchange, answered as a wrong password is; the country refusal is charged to nothing, as it is
  on the other paths. The `OnClientValidatePassword` script hook is still not consulted for SCRAM, which has no
  password to give it ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md),
  [DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **Fixed: a sign-in under a domain alias escaped the domain's country policy.** The policy was looked up under
  the domain the client typed, and an alias domain has no policy of its own, so `bob@alias.example` was allowed
  from a refused country and then signed in as `bob@example.com`. The name is now resolved as the sign-in resolves
  it - domain aliases, then the default domain - before the policy is read
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: a token, a certificate or a Kerberos ticket signed in past the country policy.** `XOAUTH2`,
  `OAUTHBEARER`, `EXTERNAL` and `GSSAPI` on SMTP, IMAP and POP3 never asked the domain's or the account's
  country policy, which the password paths and the REST API ask, so a stolen bearer token signed in from a
  refused country. They now ask it where they ask the compromise hold (`AccountLogon::RefusedByHold`), and the
  refusal is charged to nothing, as a password's is ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: an SMTP session signed in without a password sent only one message.** At each `MAIL FROM` after the
  first the session's credentials are checked again, and the check validated the password the session kept -
  which only `PLAIN` and `LOGIN` keep. After `SCRAM-SHA-256`, `XOAUTH2`, `OAUTHBEARER`, `EXTERNAL` or `GSSAPI` the
  second message was refused with 550 "Login credentials no longer valid" and the session signed out. Such a
  session now has its account checked again instead: still active in an active domain, not a room, not held for
  review, not refused by its country policy, and for SCRAM not since given a second factor or an expired password.
- **Fixed: the IMAP master user opened a mailbox past the mailbox's country policy.** `AUTHENTICATE PLAIN` with an
  authorisation identity asked the country policy, and the compromise hold, of the master user only, so a master
  user in a domain with no policy signed in as a mailbox from a country the mailbox's domain refuses. The account
  signed into is now asked too, after the password, and the refusal is charged to nothing
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: a sign-in under a domain alias was not the account to SMTP's own controls.** After signing in as
  `bob@alias.example`, the send-stop of the quota policy, the sending quota (counted under a second name), restricted
  delivery, the takeover check, message recall, the address book and the message-size limit looked the account up
  by the name as typed, with the default domain alone, and found nothing - so none of them held. They now use the
  account the sign-in resolved to.
- **Fixed: a domain's maximum message size was lifted by `MAIL FROM:<>`.** A signed-in account was held to its own
  size limit whatever `MAIL FROM` named, but not to its domain's, which was read from the envelope sender's domain;
  naming no sender or another domain's address took the server's instead. The signed-in account's whole answer -
  its own value, else its domain's - now holds as well ([SettingLevels.md](hmailserver/docs/SettingLevels.md)).
- **Fixed: the backup-MX recipient callout's ceiling was per recipient domain.** A policy for a pattern such as
  `*.partner.example` gave every subdomain a sender named ten verifications a minute of its own, all to one
  primary, each holding a thread of the SMTP service for up to the timeout, and every address asked about stayed
  in memory. The ceiling is now the policy's and the primary's, at most 8 verifications run at once (2 to one
  server), and the remembered answers are swept and capped at 10,000
  ([RemoteDomains.md](hmailserver/docs/RemoteDomains.md)).
- **Fixed: one connection could put any correspondent on the server's block list.** A message to a spam trap
  blocked its envelope sender server-wide for `SpamTrapBlockDays` (30), whatever that sender was, and the
  envelope is whatever the client types: anybody who knew a trap address - published to be found - could have a
  bank's or a supplier's real mail held or marked for every mailbox. The sender is now blocked only when its
  domain's SPF passes for the connecting address; the address is banned and the hit recorded as before
  ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Fixed: the send API was not journalled.** A message an application sent through `POST /api/v1/send` reached
  neither of the two points where journal rules are evaluated, so a domain or account under a compliance rule
  had its application mail, and a journalled recipient's copy of it, missing from the archive. It is evaluated
  now, the key's sender counting as the sender when it is an account here. Journaling.md said mail that arrives
  was covered whichever way it arrived; it now says what is not covered in either direction: everything the
  server queues itself - the webmail, a send-later draft, an event script, a rule's forward, copy or reply, an
  automatic reply, a Sieve redirect, an account's forwarding, a delivery report
  ([Journaling.md](hmailserver/docs/Journaling.md)).
- **Fixed: a single sign-on ticket could be redeemed in another person's browser.** Found in the audit before
  this release, in the new OpenID Connect and SAML sign-in. The ticket a finished sign-in carried to the page in
  `/portal#sso=<ticket>` was redeemed by value alone, so anybody who could complete a sign-in of their own could
  send that link to someone else, and the server's own page would sign that person's browser into the sender's
  mailbox - login forgery, past both the `X-Requested-With` check and SAML's `hmsaml` cookie. The ticket is now
  bound to the browser that finished the sign-in by an `hmsso` cookie (HttpOnly, `SameSite=Strict`,
  `Path=/api/v1/session`) set beside it, and `POST /api/v1/session` refuses it without that cookie
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a one-time code asked for on top of a password could be guessed without limit.** Found in the audit
  before this release. Changing the password, making an app password or recovery codes, setting a recovery
  address and registering a passkey ask for the account's password and then its one-time code, and a wrong code
  there was neither counted nor logged, so somebody holding the password and a session (or, for the password
  change, an app password) could try codes at the request rate until one matched and leave with a credential
  that needs no second factor. The administrator's passkey registration had the same gap. A wrong code is now
  logged and charged like a wrong password, and after five in fifteen minutes every code for that account, or
  for the administrator, is refused with 429 for fifteen minutes
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: with a parent-domain relying party, a page on any sibling host could sign somebody in with their
  passkey.** Found in the audit before this release. A passkey's origin was compared with the request's own
  `Host` header, which the client writes, so with `passkey_relying_party` set to `example.com` as the document
  advised, a script on any host under `example.com` could collect a person's assertion and post it here naming
  that host, and be signed in as them with no password and no code. A new setting, `passkey_allowed_origins`,
  names the origins this server's pages are served at; once a relying party is set it is required, and the
  origin of every registration and sign-in must be one it names. A credential made in a frame of another site's
  page is refused too. With neither setting, the default, the check is unchanged
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a switched-off domain still let its people in with a passkey or single sign-on.** Found in the audit
  before this release. A password is refused for an account whose domain is not active, but a passkey, an OpenID
  Connect or SAML sign-in to an existing mailbox, and every browser session already open went on working. All
  three now check the domain, and a session ends at its next request once its domain is switched off
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: the forgotten-password route's timing said which mailboxes can be reset.** Found in the audit before
  this release. `POST /api/v1/password-reset` gave every address the same answer, but gave it only after making
  the link and queueing the message when the mailbox had a confirmed recovery address, so the answer came
  measurably later for exactly those mailboxes; and a reverse proxy on the same machine had no per-client limit
  at all. The answer now comes before the address is looked up, the link is made on the async work queue, and
  requests from loopback share a ceiling of thirty in any minute
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: a one-time code could be used again for as long as it was in its window.** Found in the audit before
  this release. No place that checks a TOTP code - the COM sign-in (`AuthenticateWithCode`) for an account and for
  the administrator, the administrator's Basic sign-in on the REST API, and the REST routes that ask for a code on
  top of a password - remembered the codes it had accepted, so a code somebody read over a shoulder, out of a
  proxy's log or off a phishing page, with the password beside it, was a whole credential for up to a minute and a
  half after its owner had used it. RFC 6238 says a verifier must not take a code twice. Each owner's last step
  taken is now remembered, in one record every one of those places shares: the same code, or one from an earlier
  step, is refused wherever it comes next. It is not counted as a wrong code - it is no guess, and the one who
  sends it again is most likely its owner - and the wrong codes that are counted are judged and counted under one
  lock, so that requests racing each other cannot all be judged before the fifth is counted
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: a wrong one-time code at the COM sign-in was not counted.** Found in the audit before this release.
  `AuthenticateWithCode` checked an account's code before its password and counted neither, and the
  administrator's wrong code was logged and nothing more, so somebody with the password could guess codes over
  COM at the speed of the calls. The password is now checked first, and a wrong code after a right password
  counts against the same budget the REST API keeps for that owner - five in fifteen minutes, then no code taken
  for fifteen, over COM and REST alike. COM stays outside the per-name lockout and the auto-ban, as its passwords
  are ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: behind a reverse proxy on the same machine, the administrator's one-time code could be guessed at the
  request rate.** Found in the audit before this release; present since the administrator's second factor came
  to the REST API. A wrong code in `X-hMailServer-OTP` on the administrator's Basic sign-in was charged only to
  the auto-ban, which leaves 127.0.0.1 and `::1` alone - and behind a proxy on this machine every request comes
  from there. It now counts against the administrator's budget, and once that is spent the answer is 429 with
  `code` `second_factor_locked` and `Retry-After`; the auto-ban is still charged once for each wrong code
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: behind a reverse proxy that terminates HTTPS, the session cookie was not marked `Secure`.** Found in
  the audit before this release. `hmailsession`, and the sign-in cookies `hmoidc`, `hmsso` (OpenID Connect) and
  `hmpasskey`, were marked `Secure` only when the REST listener itself spoke TLS, so behind the usual proxy in
  front of the plain loopback listener the session cookie of an HTTPS site was one a browser would also send over
  plain HTTP. They now follow the rule SAML's cookies already did: `Secure` over TLS, or when the proxy on this
  machine says `X-Forwarded-Proto: https` ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **Fixed: registering an administrator passkey skipped the one-time code when the administrator's second factor
  could not be opened.** Found in the audit before this release. With `AdministratorTotpSecret` enrolled but its
  secret unopenable on this machine - the key it was protected with lost - every administrator sign-in refuses,
  but `POST /api/v1/passkeys` for the administrator took the password alone from a session already open. It now
  refuses with 409, as the sign-ins do ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a domain's spam traps were not confined to the domain over REST.** A domain administrator, a role or an
  API key confined to one domain could declare a spam trap in any other domain on the server with `POST
  /api/v1/domains/{domain}/spam-traps` - and a hit on it bans the connecting address and blocks the envelope sender
  for every domain - and read another domain's traps and the senders and addresses that hit them with `GET` on the
  same path and on `.../spam-traps/hits`. The three routes are now scoped to the domain they name, as a trap's
  removal already was, and answer `403` for any other.
- **Fixed: a domain administrator could become the server administrator through an app password.** Where the server
  administrator's own mailbox sits in a domain somebody else administers, that domain's administrator could issue
  an app password on it (`POST /api/v1/accounts/{address}/app-passwords`, or COM's `AppPasswords.Add`), was handed
  the secret, and could then log on to COM with it - `Application.Authenticate` accepted an account's app password
  and gave the logon the account's whole role, past its second factor. COM no longer accepts an app password as a
  logon at all (it still opens the mailbox over IMAP, POP3 and SMTP), and a credential may no longer issue or take
  back an app password, share a folder, or write, flag, copy or delete mail, or revoke a device, of an account
  whose authority it does not take in, over REST or COM ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a domain administrator could delete or rename the server administrator's mailbox in its domain.** `DELETE
  /api/v1/accounts/{address}` and `POST /api/v1/accounts/{address}/move` asked only whether the account's domain
  was the credential's, so the server administrator's own mailbox could be deleted with its mail, or renamed so
  that its logon name no longer existed. Both now refuse, with `403`, an account whose authority the credential
  does not take in, as COM's `Account.MoveTo` always did; COM's `Account.Delete`, `Accounts.Delete` and
  `Accounts.DeleteByDBID` refuse it too ([Roles.md](hmailserver/docs/Roles.md),
  [MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- **Fixed: the REST listener read request headers out of the body.** A header the request's head did not carry was
  looked for in the body as well, so a cross-site form posting `text/plain` to `/api/v1/session` could carry an
  `Authorization` line of an account the attacker held and sign the victim's browser in to it (login CSRF), and an
  `X-Requested-With` line in a body satisfied the guards that ask for that header. Every header is now read from
  the head alone.
- **Fixed: a credential confined to one domain could set the webmail's branding for every domain.** `PUT
  /api/v1/portal/branding` and `PUT /api/v1/portal/files` name their domain in the body, where the authorisation
  did not look, so an API key or a role holding `manage_settings` and confined to one domain could rewrite the
  server's sign-in announcement and logo, or another domain's, and the file-link policy of either. A confined
  credential now sets only its own domains' and is refused the server's and any other's with `403`, and a pass
  over the scheduled sends (`POST /api/v1/scheduled/run`) is refused to it as server-wide
  ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a domain administrator could repoint the server administrator's fetch account.** The routes under
  `/api/v1/accounts/{address}/` for an account's rules, fetch accounts, OpenPGP keys, directory entry, booking
  policy, address-book shares, push devices, recovery address and codes and review state asked only whether the
  account's domain was the credential's. Where the server administrator's own mailbox sits in a domain somebody
  else administers, that domain's administrator could point its fetch account at a host of its own - the server
  then signed in there with the kept password - set rules on the mailbox, publish a key under its address, clear
  its recovery or exempt it from the compromised-account detection; a support session into the mailbox and a
  migration job copying mail into it asked the same and no more. Every route that changes an account named in its
  path now asks one rule, in one place, and refuses with `403` an account whose authority the credential does not
  take in; `build/check-role-choke-point.py` fails a new such route that is not in it. A migration, or a job of
  it, is started, paused, resumed, retried, cancelled or deleted only by a credential that takes in every account
  its jobs copy into ([Roles.md](hmailserver/docs/Roles.md), [Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: over COM, a domain administrator could change what is in the server administrator's mailbox in its
  domain, and point a rule or a fetch account at another domain's account.** COM's `FetchAccount`, `Rule` (with its
  actions and criteria), `IMAPFolder`, `IMAPFolderPermission` and `Message` writes, and `Account.DeleteMessages`,
  `RevokeDevice`, `ClearRecoveryAddress`, `ClearRecoveryCodes` and `RemovePushSubscriptions`, asked nothing of the
  account they were in - only how the object had been reached - so a domain administrator could add a fetch account
  or a rule to the server administrator's mailbox, share or delete its folders and mail, or clear its recovery; and
  a rule, a rule's action or a fetch account reached in its own domain could be given another account's id, in any
  domain, and saved there. Each now asks where it writes whether the credential reaches the account (the server
  administrator, an administrator of its domain, or the account itself) and takes in its authority, and refuses
  otherwise; an event script's objects are left as they were ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a key or a role confined to a domain could route the server administrator's mail in that domain to a URL
  of its own.** An inbound webhook route was judged by its domain alone, and a route takes an address's mail instead
  of its mailbox, so a credential holding `manage_inbound_webhooks` and confined to a domain could route the server
  administrator's address there - or the whole domain - and read its mail, password resets included. A route is now
  made, changed or removed only by a credential that takes in the account at its address (or the one its plus tag
  reaches), and a domain's route only by one that takes in every account of the domain with an admin level or a
  role; the refusal is `403` ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)).
- **Fixed: a domain administrator could not clear its own users' recovery address, recovery codes or push devices
  over REST.** `DELETE /api/v1/accounts/{address}/recovery`, `.../recovery/codes` and `.../push` (and `GET .../push`)
  read the whole address as a domain name, which no credential confined to a domain lists, so they answered `403`
  to every domain administrator and every key issued for named domains. They are now scoped to the address's
  domain, as the other account routes are.
- **Fixed: a large download over the REST API or the web services was cut off, and held whole in memory while it
  lasted.** The deadline that covers reading a request covered writing its response too, so a file link, an export
  or a message source the reader could not take in within the request deadline (30 seconds on the REST listener,
  15 on the web services) ended part way, every time - a 100 MB file link below about 27 Mbit/s - and the
  connection's five-minute limit cut off whatever outlived it. Meanwhile each download held the whole body three
  or four times over, so 64 slow readers of a 500 MB file link, which needs no sign-in, could hold some 32 GB of the
  mail server's memory. A response is now written in pieces and the deadline is progress: a reader that stops
  reading is closed after the request deadline. A file link, the export archive (`/export.zip`) and an eDiscovery
  export are sent from their file on disk 64 KB at a time, never read into memory whole. A large download - a
  file, or a body of 4 MB or more - may outlive the connection's limit for as long as its size takes at 128 KB a
  second, and at most sixteen are written at once; one more is answered `503` with `Retry-After: 30`
  ([PersonalData.md](hmailserver/docs/PersonalData.md), [Discovery.md](hmailserver/docs/Discovery.md)).
- **Fixed: a request head that never ended could hold 16 MB per connection.** The listener read a head into the
  buffer sized for the largest body a route takes and applied the 64 KB cap on a head only once its blank line
  arrived, so each connection could be made to hold 16 MB until the request deadline closed it. A head is now
  refused with `413` as soon as it passes the cap.
- Fixed: BATV tags, SRS addresses, the fabricated SCRAM salt of an unknown account and the Web Push endpoint's
  hash were made over a conversion that, on Windows, gave zero bytes for everything after the first character
  above U+00FF and, on Linux, `?` for each such character - the conversion the audit trail's chain was fixed of
  (under Administration). So two internationalised mailboxes of one length shared a BATV tag and an SRS hash, and
  a bounce to one was taken, or reversed, as a bounce to the other; a database password of Cyrillic letters added
  only its length to the SCRAM key; and two push endpoints of one length shared the hash the unique index is on.
  Each is now made over UTF-8. A String that holds UTF-8 widened one byte to a character - what the SMTP and IMAP
  command lines give, and so what BATV and SRS are handed - is hashed as those bytes, which the old conversion
  hashed too; so for such a mailbox, as for ASCII, a tag or an SRS address handed out before the upgrade still
  verifies, and the salt an unknown account is given does not change. `ClassTester` holds each to the old
  conversion over ASCII and over widened UTF-8, and to a difference over Cyrillic, and `Batv.cs` accepts a tag
  computed the old way. A push endpoint outside printable ASCII - which no browser gives - is refused.
- Fixed: a password of Cyrillic, Greek or CJK letters could be opened by any other password of its length. Every
  password hash (MD5, salted SHA-256, PBKDF2, Argon2id, scrypt, and the pepper) was made over the same
  conversion - NUL bytes on Windows from the first character above U+00FF on, `?` for each on Linux - so SASL
  PLAIN, whose fields are UTF-8, took `ключик-вход` for `пароль-тест`, and on Windows a REST API Basic credential
  of NUL bytes took either; the administrator password was hashed the same way. Hashes are now made over UTF-8,
  which leaves every ASCII password's hash as it was and lets such a password serve SCRAM-SHA-256. A password that
  arrives as its UTF-8 widened one byte to a character (IMAP `LOGIN`, REST Basic) is hashed as those bytes, as it
  was, so it and the same password decoded are one; a sign-in also tries a Latin-1 password's characters as bytes
  (for a client that sends UTF-8, those of the text it decodes to), which opens a hash made of them before, and
  that sign-in makes the hash again over UTF-8 (as it does one made under a pepper outside ASCII, under
  the pepper's UTF-8), after which it serves SCRAM-SHA-256 too. A password holding U+0000 is refused. COM's
  `Utilities.MD5` and `Utilities.SHA256` are the same code, so for text outside ASCII they now give the digest of
  its UTF-8. A hash the old conversion made of a password above U+00FF is refused and logged (upgrade item 24);
  on Linux such a hash still opens to the password with `?` for those characters until it is set again, and on
  both platforms SCRAM-SHA-256 still opens a PBKDF2 one to the bytes it was made over
  ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md), *A password outside Latin-1*).
- Fixed: the crash-dump helper, `hMailServer.Minidump.exe`, loaded `dbghelp.dll` by name, so a copy put in `Bin`
  would have run under the service's account at the next crash. It loads System32's, and the service starts
  the helper by its full path.
- Fixed: the Control Panel restarted the service after a settings change by starting `cmd.exe` elevated by name,
  and Windows looks for a bare name in the working directory first - wherever a file dialog last left it - so
  a `cmd.exe` put there would have run behind the elevation prompt. It runs System32's `cmd.exe` and
  `net.exe` by their full paths, and `DBUpdater.exe`, run as an administrator, opens an upgrade script in
  System32's Notepad by its full path, where it named `notepad.exe` alone.
- Fixed: the MySQL client's own DLLs were looked for in the current directory and along `PATH` as well; they
  are looked for beside `libmysql.dll` and in System32 only. The MariaDB client the x64 installer ships needs
  nothing else; a client copied into `Bin` by hand needs its DLLs beside it.

### Encryption

- **The server publishes its users' OpenPGP keys** (schema 6050) through Web Key Directory; publishing is off
  by default, and a secret key is refused at every entry point.
- **OpenPGP in the webmail** (schema 6080): end-to-end encryption in the browser, with the keys under a
  passphrase this server never sees; a correspondent's key remembered at first sight and a changed key shown
  as a warning (schema 6081); and a password-protected message for a correspondent with no key (schema 6082)
  ([OpenPGP.md](hmailserver/docs/OpenPGP.md)).
- **Zero-access mailboxes** (schema 6083): opt-in per account, delivered mail is encrypted to the account's own
  key once the filters have run, and the administrator cannot read, search or recover it
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)). A message that arrives already encrypted is stored as
  it arrived; whether it is, is read from its body and not its `Content-Type` - fixed before release, when a
  `multipart/encrypted` label over plain text, or an S/MIME v2 opaque-signed message with no `smime-type`,
  was stored in the clear; and the whole message has to be under the key, not merely hold some ciphertext -
  fixed before release, when real PGP/MIME ciphertext with a readable part beside it was stored as it arrived,
  that part in the clear. Gateway encryption to partner domains makes the same decision the same way, and
  encrypts such a message rather than sending the readable part as it was.
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
- **Mailbox encryption at rest** (schema 6099): switched on per account, after the domain's recovery key has been
  made, every message stored in the account's folders from then on is encrypted to an X25519 key of the account's
  own, with AES-256-GCM, as the message store's last stage. Mail that arrives while nobody is signed in is
  encrypted all the same; reading it needs the private half, which the account's own password opens at sign-in and
  which closes `MailboxKeyIdleMinutes` (15) after the last use. App passwords are refused for such a mailbox, an
  administrator's password reset is refused while the key is closed and goes through a recovery with the domain's
  passphrase, and it cannot be combined with zero access or a TOTP second factor. It is on REST and the Control
  Deck, with no webmail card and no Control Panel page, and `--encrypt-store` and `--decrypt-store` rewrite what
  was stored before. Off on every account ([EncryptionAtRest.md](hmailserver/docs/EncryptionAtRest.md)).
- **Secrets at rest** (schema 6103): an account's TOTP second factor and the `[Settings]` secrets (`SRSSecret`,
  `BATVSecret`, `OAuth2HmacSecret` and the rest) are sealed with AES-256-GCM under a stored-secrets key the
  installation's servers share, kept in each server's `hMailServer.ini` under DPAPI or the Linux key file, never in
  the database or a backup; the database records which key it is. The Windows installer makes the key on a new
  installation, and the container's `--create-database` on Linux; after an upgrade nothing is sealed until an
  administrator runs `--create-stored-secrets-key`, which the setup checklist offers. `--export-stored-secrets-key`
  and `--import-stored-secrets-key` move it to another server, the checklist stays open until it has been exported,
  and `--forget-sealed-secrets` with a lost key's id clears only what that key sealed.
  `ProtectStoredSecretsWithDPAPI=0` keeps everything as written. The step widens `accounttotpsecret` to 1024
  characters. The installer now writes the administrator password as PBKDF2 itself
  ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md)).
- **A certificate for an IP address is accepted on Linux**: an HTTPS endpoint given by its address - an object
  store, a webhook, an identity provider, the update feed - was refused when its certificate named the address as
  an iPAddress entry, which is where RFC 5280 puts it; and no server name is sent for an address.

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
- Fixed: the audit trail's chain did not cover a character above U+00FF. The fields were hashed through a
  conversion that, on Windows, hashed everything after the first such character as zero bytes and, on Linux,
  each such character as `?`, so a row naming a Cyrillic account or an internationalised address could be
  edited after that character (`deleted` to `created`, say) and `verify` still answered intact. The fields
  are now hashed as UTF-8 and the hash written in upper case; rows written before, in lower case, are checked
  the old way ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md), *Verifying the chain*).
  `GET /api/v1/audit` listed such a row through the same conversion, so its actor, object and detail came back
  as zero bytes or question marks, and its `actor`, `object_type` and `action` filters matched nothing outside
  ASCII; the listing is now UTF-8 and the filters are read as UTF-8.
- Fixed: the alerts carried text outside ASCII as something else. The REST routes widened the JSON body's UTF-8
  one byte at a time, so a rule's condition, address, webhook and secret, and a test alert's condition and
  summary, were stored as other text than was sent, and a condition was counted in bytes against its limit of 64
  characters; and
  `GET /api/v1/alerts/rules` and `/events`, the webhook's body and the mail wrote text through the conversion
  above, so an alert the server raised about a name outside Latin-1 arrived with zero bytes or question marks.
  Every field is now read and written as UTF-8; the mail's Subject is RFC 2047 encoded words when it is not
  ASCII; the condition in `X-hMailServer-Event` is percent-encoded outside printable ASCII, so no byte of it can
  end the header; and the webhook is signed with the secret's UTF-8, which for an ASCII secret and body is the
  signature it always was (`ClassTester` holds it to a value computed apart); a secret stored before this fix,
  as the bytes it was sent as, signs with those bytes, so no receiver's check changes. Text that arrived that
  way - an address from the SMTP command line, a rule written before - is written as the UTF-8 it stands for.
- **A setup checklist the server judges**, a sample domain that can be thrown away, **help beside every page**
  of the Control Panel, the Deck and the webmail from the documents built into the binary, and first-run tours
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md), [SampleDomain.md](hmailserver/docs/SampleDomain.md),
  [Help.md](hmailserver/docs/Help.md)).
- **The Control Panel on one design system**: a command bar, a sidebar that collapses to a rail and dialogs
  sized to their content ([ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)); 65 pages, ten more
  than at 6.3.3.
- **The Control Deck** in twenty languages besides English, redrawn on the webmail's design and usable without a pointer, with
  one connection's log followed live ([FollowConnection.md](hmailserver/docs/FollowConnection.md)). The census in
  [DeckParity.md](hmailserver/docs/DeckParity.md) counts 458 of the 466 properties the desktop Control Panel
  writes as writable over REST and reachable from a Deck view; the eight it lists as missing are an account's
  directory fields.
- **A person's data as one archive**, and an erasure that reaches the rest of it
  ([PersonalData.md](hmailserver/docs/PersonalData.md)); **mailbox rename and move** as one operation (schema 6056)
  ([MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- Fixed, in the person's export new in this release: an app password could download the whole account at
  `GET /api/v1/me/export.zip`, and the archive's `account.xml` carried the password's hash, the authenticator's
  seed in the clear, the app passwords' hashes and the fetch accounts' passwords and OAuth tokens, sealed only
  with a key built into every copy of the server. The download is now refused (403) to an app password and to a
  webmail session started with one, and `account.xml` - in every form of the export - leaves out the password,
  the seed, the hashes of the app passwords, recovery codes, password history and file passwords, the fetch
  accounts' secrets, a browser's push secret and the wrapped mailbox key of an account encrypted at rest
  ([PersonalData.md](hmailserver/docs/PersonalData.md)).
- Fixed, in the same export: its `logs.txt` took every log line in which the address occurred inside another,
  so that `ann@example.com`'s own download carried the sessions, peers and recipients of `joann@example.com` and
  `ann@example.com.au`. The address is now matched whole.
- Fixed, in the same export: each folder was built as one mbox in memory, deflated beside a copy of itself, and
  the 500 MB download ceiling was checked only once the whole archive was written, with nothing limiting how
  many were built at once - so any account, four requests at a time, could hold every HTTP worker (the webmail's
  and the whole API's) for minutes and push the service toward running out of memory. A folder past 256 MB is
  now written in parts, a mailbox whose mail alone is past 2 GB is refused (413) before anything is built, and a
  download is refused while an archive of the same account is being built (409) or two are on the server (429).
- Fixed, in the erasure of an address from the logs new in this release: every occurrence of the address inside
  another was rewritten too, so that erasing `ann@example.com` turned `joann@example.com` into `jo[erased]` and
  counted it as removed, and each log was truncated and written in place, so that a crash or a full disk part way
  lost the rest of it. The address is matched whole - still where a line quotes it (`'ann@example.com'`) or
  writes it after `=` or `/`, though not as the last part of an SRS address, which is another person's - and each
  log is written beside itself and renamed over it.
- Fixed: on PostgreSQL an erasure compared the message trace, the greylisting triplets, list memberships and
  aliases with the address exactly as given, and PostgreSQL compares text by its bytes, so a row written in another
  case (`Ann@Example.com`) survived an erasure of `ann@example.com` that reported success. Every store the
  erasure deletes rows from now compares both sides lower-cased.
- **eDiscovery** (schema 6097): a compliance officer opens a case, searches every mailbox, the deleted items and
  the archive by person, date, words and attachment, puts the mailboxes, domains or archived copies it is about on
  a legal hold, and exports what was found as EML and mbox with a manifest whose SHA-256s prove what was handed
  over. A hold stops the account's and its domain's deletion, a mailbox move to another domain, a purge of its
  deleted items, the retention sweep and a full restore, and keeps what its owner deletes among the deleted items
  until it is released. Every search, hold, export and download is on the audit trail. Two permissions,
  `view_discovery` (which the auditor role holds) and `manage_discovery`; the routes are `/api/v1/discovery/cases`,
  with a view on the Control Deck and a read-only page in the Control Panel
  ([Discovery.md](hmailserver/docs/Discovery.md)).
- Fixed, in the eDiscovery export new in this release: the .zip counted its entries in the 16 bits of the
  classic end record alone, so an export of more than 65,535 entries - a search may keep 100,000 hits, each
  written as an .eml - ended with a count modulo 65,536, and a reader that trusts it, .NET's `ZipArchive` among
  them, refused the evidence package or listed part of it. From 65,535 entries on the archive ends with the ZIP64
  end record and its locator, which count them in 64 bits; the person's export is written the same way
  ([Discovery.md](hmailserver/docs/Discovery.md)).
- **An erasure reaches the stores beside the mailbox**: messages held for review, messages that expire,
  password-protected messages, the recipients an account remembers writing to, the out-of-office memory and the
  correspondents' S/MIME certificates and OpenPGP keys. Every comma-joined list is matched whole, so that erasing
  `ann@` never touches `joann@`, and the archive index's own match is fixed the same way. A legal hold outranks the
  erasure, and `POST /api/v1/addresses/erase` answers `held` beside `removed`
  ([PersonalData.md](hmailserver/docs/PersonalData.md)).
- Fixed: a domain could be given the single-label name of a directory the server keeps its own files in under
  the data directory - `protected`, `confidential`, `recoverable`, `discovery` and the rest - over COM and REST
  (the Control Panel asked for a dot), and then shared that directory with the store. A backup left such a
  domain's mail out and a restore did not put it back; on Windows the recoverable, confidential and protected
  stores' clean-up could remove a mailbox directory named by a number; and renaming the domain carried the whole
  directory, the store's files with it, into the new name and deleted it where the store looks. Such a name is
  now refused for a new domain and for a rename; an existing one is reported at every start (`HM7597`) and on the
  checklist, and renaming it moves only its accounts' mailboxes
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md)). The operator's `GET /api/v1/accounts/{address}/export.zip`
  in the OpenAPI document now says, as the download does, that the credentials are left out of `account.xml`, and
  lists its 403, 409 and 429 answers.
- Fixed: renaming a domain failed half-way when any of its accounts had a Sieve script, a vacation reply's memory
  or a duplicate check on disk. The domain's Sieve tree was moved first, and each account's rename then refused
  to move its own Sieve directory onto the one that move had just made - after the mailboxes had moved and before
  the row took the new name, so the domain's mail was where its name did not look, and a second rename was refused
  because the new directory existed. An account's rename in the cascade now moves nothing that is not left under
  the old name.
- Fixed: 72 COM classes answered a refusal as a bare HRESULT; each now says why. The message trace's search
  answered nothing on SQL Server Compact (one parameter written twice and bound once).

### Migration and import

- **A migration that survives being interrupted** (schema 6090): mailboxes copied from another IMAP server,
  Google Workspace or Microsoft 365 as jobs, one per mailbox, throttled, resumed without copying anything twice,
  with contacts and calendars beside the mail ([Migration.md](hmailserver/docs/Migration.md)).
- **Other accounts in the webmail** (schema 6065): POP3 and IMAP accounts fetched from the page, and an import
  from Gmail or Outlook.com with a sign-in (OAuth with PKCE).
- **Fixed: one person could switch every import sign-in off.** The table of sign-ins waiting to finish refused new
  ones once it held a thousand, and each lived ten minutes, so one account starting an import a thousand times in
  under a minute kept every other person's import and every migration job's Google or Microsoft sign-in refused
  for as long as it went on. An account or a job now keeps three at most, a fourth dropping its own oldest, and a
  full table drops its oldest instead of refusing ([Migration.md](hmailserver/docs/Migration.md)).
- **PST import**: the server reads Outlook data files itself, with a reader of this project's own written from
  Microsoft's published specifications, as a migration of kind `pst` - one job per file into one account, with the
  migration row's throttle, resume marks, report and Deck view. Mail, folders and their flags and categories come
  in, and contacts, calendar items and tasks go into the account's address books and calendars as vCard and
  iCalendar, notes and journal entries as messages. ANSI files are read, and an OST where it is a PST in all but
  its name; the 4K-page layout, Windows Information Protection and an OST caching an Exchange mailbox are refused
  with a sentence saying what to do. The reader runs in a process of its own, killed on a pause, a cancel or a
  stop, so a file that crashes it costs one message, not the service. An administrator copies the file into
  `<PstImportDirectory>/<domain>/` - empty as shipped, so no PST job runs until an administrator names a
  directory - and the job is started over REST (`POST /api/v1/migrations`), `hmctl` or the Deck
  ([Migration.md](hmailserver/docs/Migration.md)).

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
  Pages serves from the project's releases, in place of GitHub's Releases API, which has answered 404 since 18
  September 2026. It trusts two signers: the GitHub workflow that signed every release up to 6.3.3, for a
  certificate issued before the suspension and a release no later than 6.3.3, and this project's GitLab CI pipeline
  for a `v*` tag, at the project's path and at the group's it is to move to (below), which is to sign the releases
  from 6.3.5 and has signed none yet. 6.3.4 was built and
  Authenticode-signed by hand, before that job existed, and carries no Sigstore bundle: every server, of any
  version, installs it by hand, and a live update offered it says it cannot be verified and downloads nothing. A
  file signed for one release tag is refused as any other release; on Windows the installer's own version resource
  must name the release it is installed as; and a version with a suffix is a pre-release whatever the feed says. On
  Windows the `Updates` folder admits changes from SYSTEM, the Administrators and the service alone, and the update
  helper runs the installer, and the rollback image, only with the SHA-256 the server read when it checked them,
  through a handle it holds until they have finished. **A server from 6.2.28 to 6.3.3 still looks at GitHub.**
  Under `[Settings]` in its `hMailServer.ini`, add
  `UpdateFeedUrl=https://updates.progressiverobot.com/releases.json`, then restart the service, and its update
  check works again. Its live update can then install the re-hosted releases up to 6.3.3. The first release signed
  on GitLab, 6.3.5, will be refused by that server's live update, which trusts only GitHub's signer; installing
  6.3.4 by hand first avoids it, since 6.3.4 trusts both. Until then keep `UpdateAutoDownload` off, or such a
  server downloads a GitLab-signed installer at every check and refuses it
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **Stable download links.** The feed's site answers `https://updates.progressiverobot.com/download/<name>`, where
  `<name>` is a file's name without its version - `hMailServer-x64.exe`, `hmailserver_amd64.deb`,
  `hmailserver.x86_64.rpm` and the rest, each with its `.cosign.bundle` beside it - with a redirect to that file of
  the newest stable release, and `/download/` with one to that release's page. GitLab's own
  `/-/releases/permalink/latest` follows the release published last, a pre-release included; these links never do,
  and a name the stable release does not carry answers 404, never another release's file. 6.3.4 carries the x64
  installer, the amd64 `.deb` and the x86-64 `.rpm`, and no bundle
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md#the-download-links)).
- **The live update already trusts where the project is going.** The project is to move from the personal
  namespace `Progressiverobot/hmailserver` to the group [`hmailserver`](https://gitlab.com/hmailserver), which the
  owner reserved on 26 September 2026, and a move changes the path every signing certificate names. A server trusts
  only the signers compiled into it, so 6.3.4 carries a third: the same pipeline at
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`, issuer `https://gitlab.com`,
  repository `https://gitlab.com/hmailserver/hmailserver`, with the project's id, `86730559`, which a transfer keeps,
  required in the certificate whatever `UpdateSigningIdentity`, `UpdateSigningIssuer` and `UpdateSourceRepository`
  say (`-` included) - so nobody who registers that path, now or later, can sign as this project, and a release
  signed after the move installs by the live update of every server from 6.3.4 on (the project moves only once a
  release signed before it has shown that Fulcio records the id). The id stays optional for the present path. A
  server that still has the three GitLab values `UpdateFeed.md` gave 6.2.28 to 6.3.3 set trusts the present path
  alone, and refuses what is signed after the move until they are cleared; a server from 6.2.28 to 6.3.3 checks no
  id, is never to be pointed at the group's path with them, and is upgraded by hand after the move
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **From 6.3.5, a release is made by GitLab CI**, from a protected `v*` tag. Only a Maintainer may create one, and
  since 26 September 2026 the project's only member with that role or above is its owner, whose tags these are
  ([GOVERNANCE.md](GOVERNANCE.md)). The release job refuses a tag that is not annotated, not on `master`, or not
  signed by the one key `master`'s `.gitlab/allowed_signers` lists, read from `master` and never from the tagged
  tree, and a file name the package registry already holds with other bytes; and it refuses while the security
  scans of the tagged commit have a finding the policy counts that no baseline holds and `master`'s
  `.gitlab/security-exceptions.yml` does not accept (see Build and tests). The pipeline builds the installer on
  the project's Windows runner after the whole suite has passed there; once the owner has started the `release`
  job, it Authenticode-signs the installer through Azure Artifact Signing with a federated credential made for
  that one tag, makes the signed installer's own SBOM, signs every file with Sigstore as the pipeline, checks each
  signature with the server's own `--verify-update`, and, once the installer smoke test has passed and the owner
  has started the last job,
  publishes the files to the package registry and a GitLab release, in the milestone of its version where the
  project has one, with this file's section as its notes, and refreshes the update feed. The job's
  checks run as the tagged commit has them, so they stop a mistake; what stops a hostile tag is that nobody else
  may create one. The container image, `registry.gitlab.com/progressiverobot/hmailserver`, is pushed, signed with
  cosign and scanned by the same pipeline once the project has moved to the group and the owner has switched it
  on, since GitLab.com cannot move a project whose registry holds an image; signed apt and dnf repositories on the
  feed's site are made from the stable releases' packages once the owner's signing key exists - an apt suite per
  distribution release, 6.3.4's `.deb` in `ubuntu26.04`'s, and dnf repositories only from the first release that
  carries `.rpm` files built on each distribution, since the one `.rpm` 6.3.4 carries is built on Ubuntu and goes
  in none ([PackageManagers.md](hmailserver/docs/PackageManagers.md#apt-and-dnf)). None of this has
  run: 6.3.4 was made by hand ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
  [RELEASE.md](RELEASE.md), [PackageManagers.md](hmailserver/docs/PackageManagers.md)).
- **Upgrades without a booked window**: a drain closes the listeners before it waits, the live update drains
  first, and with `UpgradeOnStart=1` in `[Database]` (a new bootstrap key, 0 by default) the service applies
  the schema steps itself as it starts ([Upgrading.md](hmailserver/docs/Upgrading.md)). A rolled-back update's
  outcome is reported as soon as the service is back.
- Fixed: on MySQL and MariaDB, two nodes applying the schema steps at once - both with `UpgradeOnStart=1`, or one
  and `--upgrade-database` - were not serialised: the version-row lock the second waited on was freed by the
  step's first DDL statement, so it ran the same step beside the first and stopped with a table or a column
  already there. Each step now holds a named lock (`GET_LOCK`) until it is over. A server that refuses
  `GET_LOCK` (MariaDB Galera) still upgrades, with the row lock alone, and says so for each step.
- **Backups**: encrypted with AES-256, the passphrase never inside the archive (schema 6059); one mailbox or one
  message restored from an archive ([Backup.md](hmailserver/docs/Backup.md)).
- Fixed: a backup with **Back up messages** copied `<data directory>\Confidential` and `<data directory>\Protected`,
  so the whole of every live message that expires - attachments and all, in the clear even for a mailbox encrypted
  at rest - and the ciphertext of every password-protected message outlived its expiry and withdrawal in each
  archive, and a full restore deleted and replaced both folders. Both are now left out of the backup and left in
  place by a restore, as the recoverable store and the eDiscovery files are. Archives taken by earlier 6.3.4 builds
  still hold those copies ([Backup.md](hmailserver/docs/Backup.md)).
- Fixed: a restore whose backup index was cut short or altered - ending inside a tag, a comment or a quoted
  value, or holding an entity reference without its `;` - read past the end of the index. Such an index is
  refused as malformed - a comment left open after its closing tag is only ignored - and a reference without
  its `;` is kept as written.
- **Monitoring**: syslog (RFC 5424, or the journal under systemd; off) ([Syslog.md](hmailserver/docs/Syslog.md));
  per-domain reports ([Reports.md](hmailserver/docs/Reports.md)) with spam, virus and storage figures per domain
  (schema 6062); the DMARC and TLS-RPT reports a domain receives, read and charted (schema 6070)
  ([DmarcReports.md](hmailserver/docs/DmarcReports.md)); outbound webhooks for five mail events (schema 6051)
  ([Webhooks.md](hmailserver/docs/Webhooks.md)); a daily check of the server's own addresses against block lists
  (schema 6057), on in a new installation and off after an upgrade
  ([ReputationWatch.md](hmailserver/docs/ReputationWatch.md)).
- **Fixed: on Linux, a syslog collector that went quiet held the service.** Over TCP or TLS the sink's connection,
  handshake and writes had only the socket's own timeouts, which Asio's synchronous calls do not honour on Linux,
  so TLS pointed at a plain-TCP port, or a collector that accepted and stopped reading, stopped the sink for good
  and left a reinitialise or a service stop waiting on it until systemd killed the service. Each step now has ten
  seconds and is given up on after that ([Syslog.md](hmailserver/docs/Syslog.md)).
- **Fixed: on Linux, an OpenTelemetry collector that went quiet held its exporter.** The OTLP exporters' three-second
  send and receive deadlines were passed in the Windows form, which Linux refuses, so a collector that accepted the
  connection and never answered held the exporter, and the stop that waits for it, for good. Linux is given the
  deadlines in its own form. This was so in every release with Linux packages, from 6.3.0.
- Fixed: the reports (`GET /api/v1/reports`) keyed and wrote every address and domain through the conversion the
  audit trail's chain was fixed of, so two senders or recipients at a local domain whose names differ after a
  character above U+00FF were counted as one row, named as zero bytes on Windows and question marks on Linux, in
  the JSON and in the CSV. Names are now keyed and written as UTF-8, domains lower-cased in A to Z as before, and the
  `domain` parameter is read as UTF-8. A trace row that holds an address as the SMTP command line gives it, its
  UTF-8 widened one byte to a character, is written as the UTF-8 it stands for, as it was, and counted with the
  same address decoded.
- **Linux**: the live update applies on Linux through a root helper outside the server's unit; a Helm chart and a
  Compose file beside the container image; `hmailserver --set-setting`, `--sample-domain` and
  `--apply-environment`.
- **Windows packages**: `winget` and Chocolatey manifests rendered from the installer a release actually
  published ([PackageManagers.md](hmailserver/docs/PackageManagers.md)); they are not in the public repositories
  yet.
- **HTTP**: gzip and deflate on both listeners for the answers that hold no secret
  ([ResponseCompression.md](hmailserver/docs/ResponseCompression.md)).
- **Storage compression and attachment de-duplication** (schema 6098): with `MessageStoreCompression` on (off as
  shipped), a message delivered or appended is stored deflated, and each MIME part of at least
  `MessageStoreDedupMinKB` (256 KB) is kept once for every message that carries the same bytes. Every reader sees
  the message as it arrived, and a stored file is checked by its SHA-256 before it is served; the queue is never
  compressed. `--measure-store` says what it would save and changes nothing, `--compress-store` and `--compress-store --decompress`
  rewrite what is stored, and `GET /api/v1/store`, the Deck's dashboard and the Control Panel's Performance page
  say what it saves. With it on, `Message.Filename` over COM names the stored form
  ([MessageStore.md](hmailserver/docs/MessageStore.md)).
- **Object storage for the message store**: messages on an S3-compatible store (AWS S3, MinIO, Wasabi, Backblaze
  B2, Ceph RGW) or Azure Blob Storage, with the database as the index. A message is written to the local disk first
  and moved into the store by a background committer, so a store that cannot be reached delays nothing and loses
  nothing; IMAP and POP3 are told to try again for a message only the store holds. TLS to the store is always
  verified. `--copy-store-to-object-storage` and `--copy-store-from-object-storage` copy both ways, verified and
  resumable. Off until an administrator names a store: `MessageStoreBackend` is `file` as shipped, and a server
  with no store reads and writes as before ([ObjectStorage.md](hmailserver/docs/ObjectStorage.md)).
- **Event scripts on Linux**, in JavaScript, in the QuickJS engine (Fabrice Bellard's release 2025-04-26, vendored
  as source), for the same events under the same function names with the same objects; `ScriptLanguage` takes a
  third word, `JavaScript`. Until now a Linux server refused any script. Windows keeps VBScript and JScript and
  refuses JavaScript at load. A script has no file, process or network access, and `ScriptTimeout` and a 64 MB
  ceiling bound it ([EventScripts.md](hmailserver/docs/EventScripts.md)).
- **SNMP**: a read-only SNMPv3 agent (authPriv only: HMAC-SHA-256 and AES-128, with SNMPv2c optional and off)
  answering the mail MIB (RFC 2789) and the standard groups from the same counters `/metrics` serves. Off, and
  answering nobody until a manager's address is in `SnmpAllowedAddresses`; its settings are on REST and applied at
  a restart, with no COM property or Control Panel page. Its enterprise arc is RFC 5612's documentation number, not
  one registered to this project ([Snmp.md](hmailserver/docs/Snmp.md)).
- **The live update's files are held on Windows.** Beside what the update-feed item says, a repaired `Updates`
  folder is emptied, one owned by another account is refused (`HM6894`), the folders are held open through the
  apply so that nothing above them can be swapped, the service's own SID is given Modify there, and the update
  helper loads DLLs from System32 alone ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **The Windows installer**, after its first smoke test on a clean Windows Server 2025: an upgrade no longer writes
  a Critical `HM5011` event to the Application log; the uninstaller waits for the service's process to end before
  it deletes anything, so `hMailServer.exe` and its DLLs are no longer left in `Bin`, and a file still in use is
  removed at the next restart; the event log source is removed; and an uninstall after an upgrade no longer leaves
  the COM AppID behind.
- Fixed: an administrator password with a space, a double quote or backslashes before one failed a new
  installation at the database step, and a silent upgrade that moved the schema, with exit code 3: the installer
  put it unquoted on `DBSetupQuick.exe`'s command line, and DBSetupQuick on `DBUpdater.exe`'s (`DBSetup.exe`,
  run for an external database, asked for it again). Both hand it on in the environment variable
  `HM_ADMIN_PASSWORD` now, the installer to `DBSetup.exe` too, as it already did for `hMailServer.exe
  --set-admin-password`, so it is on no command line. An unattended install may give the installer the password
  in `HM_ADMIN_PASSWORD` in place of `/adminpassword=`, which cannot carry a double quote (Inno Setup drops it)
  and which anybody listing processes can read; it is set in an elevated prompt, since it does not cross the UAC
  prompt ([Upgrading.md](hmailserver/docs/Upgrading.md)).
- **The product points at GitLab**: the Control Panel's About and stalled-mail links, the `Policy:` line of the
  `security.txt` the server serves, the systemd units' `Documentation=` and the Linux packages' homepage. The
  update feed field is labelled "Update feed URL" in all eighteen catalogues. So do the Arch Linux PKGBUILD's `url`
  and the release tag it clones, the Helm chart's home and sources, and the winget and Chocolatey templates'
  addresses and installer, which is the one in GitLab's package registry. The container image is named
  `registry.gitlab.com/progressiverobot/hmailserver` in the Compose file and the chart; nothing is published there
  yet (above), and the images published on `ghcr.io` before the suspension answer 404. The update check no longer
  sends GitHub's `X-GitHub-Api-Version` header. The release tags' signing keys are in `.gitlab/allowed_signers`,
  which was `.github/allowed_signers`, and the VEX document is `.gitlab/hmailserver.openvex.json`.
- **The project on GitLab**: issue templates for a bug, a feature, a question and a security report, the first
  three asking for the version, the platform, the database and a log excerpt; a merge request template; the
  Service Desk, an address for anyone without a GitLab account, whose message becomes a confidential issue
  (README's Getting help, [SUPPORT.md](SUPPORT.md)); the releases as an Atom feed; and the README's list of where
  each of these is. [GOVERNANCE.md](GOVERNANCE.md) gives the maintainers' roles on GitLab, and
  [Mirroring.md](hmailserver/docs/Mirroring.md) how to push-mirror the repository to a second forge, and the
  weekly export of the issues, merge requests, releases and wiki that a mirror does not carry; neither is set up
  yet.
- Fixed on Linux: a character-set conversion that failed read an unterminated buffer, where Windows had
  substituted; it now gives an empty string.
- Fixed on Linux: the name of the account the service runs as was read with `getpwuid`, whose answer is one
  buffer for the whole process, while the setup checklist, auto-ban's firewall check and the live update ask for
  it from threads of their own, so two at once could read each other's entry or a torn one. It is read into a
  buffer of the call's own (`getpwuid_r`).
- Fixed: the container image built from `hmailserver/source/Server/platform/packaging/docker/Dockerfile` installed
  the `.deb` without its two Recommends, so it had no 7-Zip - a backup failed with "Could not add the backup
  settings to the archive" - and no Python 3 for `hmctl`. It installs `7zip` and `python3` with the package.
  No image of 6.3.4 is published (above); this is the image built from the file.

### Build and tests

- The native server's line coverage was measured for the first time: 73.48% over the whole Windows regression
  suite. The three MIME fuzz harnesses gained a nightly job, and the Linux regression suite was split in two
  halves; both ran on GitHub Actions until 18 September 2026, and on GitLab CI the fuzz job is `linux-fuzz`, nightly once the owner creates its schedule,
  and the Linux suite is one job, `linux-suite`, on the project's own runner (see the CI item below).
- Fixed: the MIME fuzz harnesses did not build. Since encryption at rest, a message the MIME code cannot parse is
  kept aside in "Problematic messages", which calls `FileUtilities::Exists` and `CreateDirectory`, and the
  harness's stand-in for `FileUtilities` lacked both, so `linux-fuzz.sh` and `build-fuzz.ps1` stopped at
  `Mime.cpp`. It gains both, answering false like its other writers, so the harness still writes nothing; the
  parser is unchanged. Found by 6.3.4's release fuzz run.
- Twenty-one new checkers under `build/` hold the rules the batches added, among them `check-account-stores.py`
  (what a backup must carry), `check-com-error-info.py` (COM refusals a caller can read),
  `check-collection-iteration.py` (locked iteration over shared collections), `check-com-dispids.py`,
  `check-openapi-blocks.py` and the audit and role choke-point checks.
- The three GnuPG proofs of the gateway-encryption and zero-access fixtures run on Windows for the first time:
  they had handed Git for Windows' gpg a home directory it could not read.
- A load generator in the repository, `build/perf/load/loadgen.py`: messages per second in and out, IMAP sessions
  per core under a latency limit, memory per session and Dovecot's imaptest as a benchmark, against any server,
  with the machine's details in every result; and a lab recipe that runs it against hMailServer and against
  Postfix and Dovecot on the same machine under stated fairness rules
  ([PublishedNumbers.md](hmailserver/docs/PublishedNumbers.md)). No numbers have been measured with it yet.
- **Windows on ARM, compiled but not run.** A `Release|ARM64` configuration in the server's three projects,
  `-Platform ARM64` in `build.ps1`, `build-tools.ps1`, the three library scripts, `get-installer-binaries.ps1` and
  `get-dotnet-runtime.ps1`, and a second installer, `hMailServerARM64.iss` (`hMailServer-<version>-arm64.exe`),
  sharing the x64 installer's sections. The ARM64 server uses SQL Server or PostgreSQL; it has no built-in
  database, since SQL Server Compact has no ARM64 build, and no bundled MySQL client. An ARM64 server's update
  check looks for the ARM64 installer, and `VersionArchitecture` says `ARM64`. It was first compiled on
  26 September 2026, on the x64 development machine: the three ARM64 libraries, the server, the tools and an
  ARM64 installer all built, every native image an ARM64 one. That compile found two faults, both fixed for ARM64
  only: libpq's cross build wanted a time zone compiler that runs on the build machine, and is now built with
  the system's time zone data, which libpq never reads; and the tools were compiled against the x64-marked COM
  wrapper, which .NET will not load into an ARM64 process, so `generate-com-wrapper.ps1 -Platform ARM64` makes a
  machine-neutral one, `build-tools.ps1 -Platform ARM64` points the tools at it through the `HmInteropDirectory`
  property and a new `Tools/Directory.Build.targets`, and the ARM64 installer ships it. Nothing of it has run:
  the project has no ARM64 hardware, and 6.3.4 carries no ARM64 installer; the x64 build, its COM wrapper and
  its SBOM are unchanged. `hmailserver/docs/WindowsOnArm.md`.
- **CI on the project's own GitLab runners.** `.gitlab-ci.yml` runs the platform-neutral checkers, the Deck's and
  the webmail's script harnesses and, for a merge request from a fork, a sign-off check on GitLab's shared runners;
  and, for a push to `master`, a `batch*` branch or a `v*` tag only, the Linux build, its tester programs, the
  whole Linux suite, the packages, the container image, the cross-compile for AArch64, the database upgrade gates
  and the SBOMs on `linix`, and the Control Panel, the tools and the whole Windows suite on `bench150`. A nightly
  pipeline fuzzes the three MIME harnesses once the owner has created its schedule. Nothing reached from GitHub
  is used. Until 26 September 2026 only the update feed's `pages` job had run there, and the ported Linux scripts
  were run by hand on `linix`. The first pipeline of the whole file ran on `master` at `40c7d3e0c` that day
  (pipeline 2884863496), and it finished failed. The checkers, the Linux build and its packages, image,
  cross-compile, sanitizer build and SBOMs, and the update feed passed; `harnesses` and `update-helper` failed on
  the shared runners, `linux-suite` and both database upgrade gates on `linix`, `windows-gate` and
  `windows-dotnet` on `bench150`, and `dependency-scan`, `repo-hygiene`, `semgrep` and `static-analysis`, which
  may fail without failing the pipeline. Each failure was the tree's or the job's, not a fault of the server's, but
  for two of the findings `static-analysis` reported; and 6.3.4 fixes each but that job's: the helper job checked
  the systemd units on a checkout the runner makes world-writable; the portal harness depended on the runner's time
  zone; the CI image had no 7-Zip, which the suite's backup tests need; three tests assumed the SMTP port, the
  order two messages land in, and an INI section to be a setting; the upgrade gate wound a new database back to
  6029 by reading the later steps, and now starts from the create scripts of 6029 themselves and compares what it
  upgrades to with a new database, which found two columns of `hm_bouncecounts` the create scripts left without the
  default the 6055 step gives them (they now have it); GitLab refused two scans' reports, each two files of one
  type, and each now uploads one; `repo-hygiene` found six images not pinned by digest and a password written in
  the file for the upgrade gate's own database, and judged a job by whether it extends `.own-runners` rather than
  by the rules it runs by; and each finding Semgrep reported beyond its baseline is fixed, or suppressed with the
  reason. Of the findings `static-analysis` reported beyond its baseline, the two that were defects of the server's
  are fixed (above); the others have not yet been read into its baseline, so that job still fails. The owner's edit
  of the README on `master` started a second pipeline, which failed on `harnesses` the same way. No pipeline has
  run on the fixed tree yet ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md)).
- **The CI after its first review on GitLab.** The fuzz, Renovate, whole-history secret scan and export pipelines
  are chosen by their schedule's description - `fuzz`, `renovate`, `security`, `export` - since the project
  refuses pipeline variables and GitLab counts a schedule's as such; only the update feed's schedule exists yet.
  A later push no longer cancels the Linux suite or the Windows gate half-way, and a push that changes only
  Markdown at the repository's root or under `.gitlab/` starts nothing on the project's runners. A busy bench is
  a warning, except in a release tag's pipeline. The fuzz job's findings are its artifact, for Developers only,
  and kept out of the public job log, where an input could be an exploit. cosign's image is pinned by
  digest (2.6.5). GitLab's own SAST (`semgrep-sast`, which adds the server's C and C++) and secret detection
  (`secret_detection`, which fails on a finding; `.gitleaksignore` names the thirty findings of a scan of the
  whole history and the twelve the landing's scan found in this release's own commits, none of them a secret) run
  on the shared runners for `master` and merge requests.
  `forge-export` writes the weekly archive [Mirroring.md](hmailserver/docs/Mirroring.md) describes, and
  `package-repos` the apt and dnf repositories, each waiting for the owner's token or key.
  `build/check-release-chain.py` holds all of it, the release jobs and the container image's gate included, and
  `build/check-forge-export.py` the export.
- **Changes land through merge requests, signed off, from the first landing after 6.3.4.** A batch reaches
  `master` through a GitLab merge request,
  by fast-forward, once its branch's pipeline has passed and every thread is resolved: `build/land-via-mr.py`
  checks the batch, pushes it, opens the merge request, waits for the pipeline, merges without squashing, and
  records on the merge request whether somebody other than the author approved; without that approval - Zain Ul
  Abidin's, or the owner's for Zain's own, given on the tip it lands - it refuses to merge, unless the owner
  overrides it for one landing with `--land-without-approval "<reason>"`, which it records. From the first batch
  landed this way, every commit that reaches `master` carries its author's `Signed-off-by`, the maintainers'
  included: the `sign-off` job checks every merge request and every `batch*` branch
  (`build/sign-off.py`, which the landing script uses too), and Renovate signs its commits off. `master`'s rule is
  to take no direct push once the first batch has landed this way (`hmailserver/docs/ContinuousIntegration.md`,
  "Landing on master"). No batch has landed this way on GitLab yet; both scripts' self-tests run in `checkers`.
- **Nothing is downloaded from GitHub to build**: the seventeen GitHub workflows, Dependabot and the CodeQL
  configuration have left the tree and are in its history; `libraries\build-openssl.ps1` takes OpenSSL's release
  archive from Debian's archive, `openssl_<version>.orig.tar.gz`, which is OpenSSL's own tarball, held to the same
  pinned SHA-256, or from `-Archive`; `build-appimage.sh` uses linuxdeploy only from `TOOLS_DIR` and otherwise
  refuses; winflexbison comes from SourceForge; and the installer's build inputs come from GitLab's package
  registry.
- **Security scanning** on the same runner, in place of CodeQL, dependency review and Scorecard: cppcheck and
  clang-tidy held to a committed baseline, Semgrep, Trivy and OSV over the lock files, the CI image and the
  vendored libraries, and the repository checks Scorecard ran. They hold a release, not a change (next item).
- **A release is held by its security scans, and a secret is refused before it is pushed.**
  `build/ci/security-gate.py` reads what `static-analysis`, `semgrep`, `semgrep-sast` and `dependency-scan` found
  in the release's commit, and the release job - or, for a release made by hand, its runbook - refuses while one
  of them did not finish or found something `SECURITY.md`'s policy counts that is in no baseline and is not
  accepted, for at most a year, in `.gitlab/security-exceptions.yml`, which the owner keeps on `master`. That file
  accepts 455 findings at the release - 188 of GitLab's Semgrep analyzer, 263 of the static analysis and 4 of
  Semgrep's open rules, from the scans of `40c7d3e0c` and of the release tree - each read by two review agents
  under the owner's delegation and each with the reason it is not exploitable; the seven it found real are fixed
  in this release (under Security and identity). `build/scan-secrets.py` runs gitleaks with GitLab's rules in the
  two git hooks the repository ships (`build/hooks`, turned on with `git config core.hooksPath build/hooks`) and
  in the maintainer's landing, which pushes nothing while it finds a secret or cannot scan
  ([SECURITY.md](SECURITY.md), [CONTRIBUTING.md](CONTRIBUTING.md), "Secrets").
- **Dependency updates by Renovate** on the same runner, with nothing read from GitHub; inert until the owner
  creates its token and schedule.
- **A test lab on `linix`**: MySQL 8.4, MariaDB 11.4, PostgreSQL 17 and SQL Server 2022, Keycloak for the OpenID
  Connect and SAML sign-ins, Postfix and Dovecot as peers, Dovecot's `imaptest`, and Uptime Kuma
  ([TestLab.md](hmailserver/docs/TestLab.md)). Its first runs found the unencrypted MySQL connection, the
  certificate check that never read an iPAddress entry and the IMAP answers fixed above.
- **The installer smoke test** installs, upgrades and uninstalls on a throwaway Windows Server 2025 VM, from a
  snapshot, checking what the installer promises for each (`build/ci/installer-smoke.ps1`); its first run, of 6.3.2
  upgraded to 6.3.3, found the four installer defects fixed above.
- **An SBOM for every compiled file a release carries**: from 6.3.4, the Windows installer, the `.deb` and the
  `.rpm` each go out with `<file>.cdx.json`, a CycloneDX 1.6 document whose subject is that file by its SHA-256
  and SHA-512 and whose components are what the file holds and needs, made by `build/asset-sbom.py`, which also
  checks one against its file. The five GitLab releases before 6.2.3 that carry an installer carry one too since
  26 September 2026, made from each installer ([SECURITY.md](SECURITY.md), "Software bills of materials").
- **Every build input pinned and named**: the .NET Desktop Runtime the installer bundles is pinned to 10.0.12 by
  version and SHA-512, where it was whatever Microsoft's 10.0 link gave on the day; the SBOM names seventeen
  components, among them zlib, the installer's fetched files and the Linux packages' system libraries, and
  `build/check-sbom-inputs.py` fails when a build input is missing from it.
- New checkers: `check-message-store-seam.py`, `check-com-shared-copies.py`, `check-mysql-charset.py`,
  `check-installer-scripts.py`, `check-ci-image.py` and `check-sbom-inputs.py`.

Full notes: [the 6.3.4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.4).

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
