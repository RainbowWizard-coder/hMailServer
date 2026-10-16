Administrator roles
===================

Who may administer what. Until September 2026 this server had two answers — the
administrator password, which can do everything, and an account with an *admin
level*, which administers its own domain — and an API key could be narrowed to
read-only and to a list of domains. A **role** is the named version of the same
idea: a small, fixed set of **permissions**, four built-in roles that hold some of
them, and an **assignment** that gives an account a role in named domains.

Nothing changes on the day you upgrade. An account with no assignment holds exactly
the authority its admin level has always given it, and the upgrade writes no
assignment for anybody.

The built-in roles
------------------

| Role | For | Domains when the assignment names none |
|---|---|---|
| `serveradmin` | Everything, including the secrets no other role reads. What the administrator password and an account with admin level *Server* have always been. | Every domain |
| `domainadmin` | The accounts, aliases, lists and mailboxes of a domain. What an account with admin level *Domain* has always been. | The account's own domain |
| `helpdesk` | Finding an account, resetting its password, unlocking it, and reading where its mail went — and nothing else. | The account's own domain |
| `auditor` | Reading everything the administration exposes, the audit trail included, and changing nothing. | Every domain |

The four built-in roles cannot be edited or deleted. Their permissions are in the
server's code, not in its database, so that a change to the `hm_roles` table cannot
hand anybody a permission; the table's permission column is kept in step for anyone
reading it and is not consulted.

What each role may do
---------------------

✅ holds the permission. A permission marked *server-wide* is reached only by a
credential with no list of domains: the queue, the quarantine, the archive, the logs,
the settings and the rest carry every domain's mail, so a role confined to named
domains is refused them whatever it holds.

| Permission | What it reaches | `serveradmin` | `domainadmin` | `helpdesk` | `auditor` |
|---|---|:-:|:-:|:-:|:-:|
| `administer_server` | Everything, including the reads of secrets such as password hashes and private keys. Every COM gate that has always asked "is this the server administrator?" asks for this. | ✅ | | | |
| `administer_domain` | Every COM gate that has always asked "is this a domain administrator?" | ✅ | ✅ | | |
| `view_configuration` | The status, settings, listeners, certificates, rules, routes, anti-spam lists, the roles and who holds them, the alert rules and what they raised, a domain's DKIM key rotation, a domain's spam traps and what has addressed them, the bounce settings *(server-wide)* and how a bounce would be classifiedand complaint settings, how a bounce would be classified, and every domain's complaint rate — never a secret, what the reputation watch last found — never a secret the server holds | ✅ | ✅ | | ✅ |
| `view_accounts` | Domains, accounts, aliases, lists, groups *(server-wide)*, fetch accounts, the OpenAPI document, and whether a mailbox could be moved now and what it has been through | ✅ | ✅ | ✅ | ✅ |
| `view_mailbox` | Another account's folders and messages; from 6.3.6 each read is on the audit trail under the credential that made it ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md), *Mail read through an administrator's route*) | ✅ | ✅ | | ✅ |
| `view_queue` | The delivery queue *(server-wide)* | ✅ | | | ✅ |
| `view_quarantine` | The quarantine *(server-wide)* | ✅ | ✅ | | ✅ |
| `view_archive` | The archive *(server-wide)*; from 6.3.6 each search of its index and each entry read is on the audit trail under the credential that made it ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md), *Mail read through an administrator's route*) | ✅ | | | ✅ |
| `view_logs` | The logs *(server-wide)* | ✅ | | | ✅ |
| `view_message_trace` | The message trace — see below — and, with `view_configuration`, the reports and a domain's suppression list and bounce cou, a domain's suppression list and bounce counts, and a domain's complaints | ✅ | ✅ | ✅ | ✅ |
| `view_audit` | The audit trail *(server-wide)* | ✅ | | | ✅ |
| `manage_accounts` | Create, change and delete accounts, aliases, lists, groups, fetch accounts and app passwords, a domain's own allow and block list, a domain's spam traps, a domain's DKIM key rotation (rotate, check, switch, retire and cancel now), a domain's suppression list (an address added or removed by hand), and an account's OpenPGP public keys (stored or removed domain's suppression list (an address added or removed by hand), and renaming a mailbox or moving one to another domain - which needs the permission in **both** domains, the one the mailbox leaves and the one it goes to ([MailboxMove.md](MailboxMove.md)) | ✅ | ✅ | | |
| `reset_password` | `POST /api/v1/accounts/{address}/password`, `Application.ResetPassword` | ✅ | ✅ | ✅ | |
| `unlock_account` | `POST /api/v1/accounts/{address}/unlock` and `Application.UnlockAccount` (both locks), `Account.UnlockMailbox` (the POP3 mailbox lock alone; an account's own is its own to clear) - each on the audit trail as `unlocked` - and clearing the logon-failure list (`POST /api/v1/settings/logon-failures/clear` *(server-wide)*; `Settings.ClearLogonFailureList` answers the server administrator only), on it as `logon failures` / `cleared`. See [The logon-failure list](#the-logon-failure-list) for what a confined help desk does instead | ✅ | ✅ | ✅ | |
| `manage_mailbox` | Another account's folders, messages and folder rights, a support session, and adding a migration job of Outlook data files (PST), beside `manage_accounts` - see [PST import files](#pst-import-files) | ✅ | ✅ | | |
| `manage_queue` | Retrying or deleting a queued message *(server-wide)* | ✅ | | | |
| `manage_quarantine` | Releasing or deleting held mail *(server-wide)* | ✅ | ✅ | | |
| `manage_archive` | Holding and releasing archived messages *(server-wide)* | ✅ | | | |
| `manage_settings` | Every setting, listener, certificate, route, rule and anti-spam list (the server's allow and block list among them), an alert rule, a test alert, the breach check's endpoint test, and clearing the record of spam trap hits *(server-wide)* - which name the hosts and addresses this server connects to, an alert rule's webhook among them, inside its own network as well ([Webhooks.md](Webhooks.md)), and the commands it runs: **the server administrator's equal**, see [What manage_settings reaches](#what-manage_settings-reaches); the webmail's branding and its file-link policy (`PUT /api/v1/portal/branding` and `/api/v1/portal/files`), whose domain is in the body: a confined credential sets its own domains' and is refused the server's and any other's | ✅ | | | |
| `manage_domains` | Which domains exist *(server-wide)* | ✅ | | | |
| `manage_server` | Reinitialise, back up, reload scripts, clear caches, install an update, run the alert pass, the DKIM key rotation pass, the suppression list's sweep or the feedback loops' sweep, the DKIM key rotation pass, a reputation check or a pass over the scheduled sends now *(server-wide)* | ✅ | | | |
| `manage_roles` | Roles and who holds them *(server-wide)* | ✅ | | | |
| `send_through_api` | Sending mail as an application through `POST /api/v1/send`, as the one sender an **API key** names in its `send_from` - the permission alone sends nothing, and only a key reaches the route ([SendApi.md](SendApi.md)). A custom role may be given it, which is how a key that sends and does nothing else is made; a key reads its own sends without any permission, and nobody else's | ✅ | | | |
| `provision` | Provisioning over SCIM 2.0 at `/scim/v2` - the organisation's identity provider making, renaming, suspending and removing accounts, with their aliases, and distribution lists ([Scim.md](Scim.md)). Only an **API key** reaches `/scim/`, and its reads need this as its writes do, so an auditor's key reads nothing there (an auditor reads the same accounts at `/api/v1/domains/{domain}/accounts`). A custom role may be given it, which is how a key that provisions and does nothing else is made; a key confined to named domains provisions those alone | ✅ | | | |
| `view_discovery` | eDiscovery's cases, their searches and hits, their holds and their exports ([Discovery.md](Discovery.md)); from 6.3.6 each page of a search's hits read is on the audit trail under the credential that read it ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md), *Mail read through an administrator's route*); downloading an export needs `view_mailbox` beside it, since it hands over the mail itself - the custodians' deleted items among it, which no other administrative route hands over - and a search that reads the archive needs `view_archive` as well, to run, to read and to download; the auditor holds all three. A confined credential reads only the cases whose every search and hold is inside its domains, and an empty case only when it opened it | ✅ | | | ✅ |
| `manage_discovery` | Opening and closing a case, running a search, placing and releasing a legal hold on an account, a domain or archived copies, and making an export ([Discovery.md](Discovery.md)). A confined credential names no custodian and holds nothing outside its domains, and never every mailbox | ✅ | | | |
| `manage_inbound_webhooks` | Routing an address or a domain to a URL its mail is posted to, changing and removing such a route, and trying again or removing a message one kept ([InboundWebhooks.md](InboundWebhooks.md)) - as much as reading every message the route takes. Meant for an **API key's** role, confined to the application's domain: a route is judged in its own domain, and one made by any credential but the server's administrator reaches public addresses only. Reading the routes is `view_configuration`'s, and a kept message needs `view_mailbox` beside it; running a pass now is *(server-wide)*. A custom role may be given it | ✅ | | | |

The REST API's **key store** (`/api/v1/apikeys`) and the `[Settings]` section of
`hMailServer.ini` (`/api/v1/settings/ini`) answer to the administrator password and to
nothing else — not to an account holding `serveradmin`, and not to a key of any role or
scope. A key that could mint keys could issue itself a replacement with no restriction
on it, and the file holds the server's secrets. An account is refused with a 403 and
the reason; a key with the 401 that keeps "valid but not permitted" indistinguishable
from "not a key". The same holds for putting a `[Settings]` key back from its audit
row (`POST /api/v1/audit/{id}/revert`): a role holding `manage_settings` puts a
database setting back from the trail, and is refused a `[Settings]` key's row with the
same 403 - since 6.3.6; 6.3.4 and 6.3.5 put it back.

The **audit trail** (`/api/v1/audit`) and the **alerts** (`/api/v1/alerts/...`) are refused to
every API key too, whatever its scope or role, and with the same 401: a stolen key must not
read the record of what it did, nor silence the alerting or point it somewhere else. They
are *not* the administrator password's alone. An account that holds a role reaches them
through the permission decision like any other route — `view_audit` for the trail, which is
what the `auditor` role is for; `view_configuration` to read the alert rules and events;
`manage_settings` to change a rule or raise a test alert; `manage_server` to run a pass —
provided its role reaches every domain. The trail records every domain's changes and the
alerts watch the whole server, so a role confined to named domains is refused all of them.

Two things are deliberately not permissions of any role but `serveradmin`:

* **Reading a secret.** An auditor that could read a password hash or a private key
  is one offline attack away from being every role at once, so no `view_` permission
  covers them.
* **Handing out authority.** No custom role may hold `administer_server`,
  `administer_domain` or `manage_roles` — each is a way to write "server administrator"
  without calling it that — and a credential may never give an account a role holding a
  permission, or reaching a domain, that it does not hold itself.

The same rule stops a narrow role **taking over** a wider one. A credential may not
reset the password of, or change, an account whose authority it does not take in —
every permission, in every domain. A help desk resets an ordinary account's password
and is refused a domain administrator's or an auditor's, even in its own domain,
because otherwise it could sign in as them; a domain administrator may change the
accounts of its domain as it always could, and is refused an auditor's.

Over REST it is **one rule**, asked in one place for every route that changes an
account named by the address in its path, or what is in it (`ChangesAccountByAddress_`
in `RestApiServer.cpp`): `PUT` and `DELETE /api/v1/accounts/{address}`, the move, the
password reset, giving and taking away a role, the mailbox encryption key, a support
session, and every route under `/api/v1/accounts/{address}/` that changes the account's
app passwords, folder permissions, messages, devices, rules, fetch accounts, OpenPGP
keys, directory entry, booking policy, address-book shares, push devices, recovery
address or recovery codes, or its review state. SCIM's writes ask it too, and so does a
migration: a job is added into an account, and a migration or one of its jobs started,
paused, resumed, retried, cancelled, signed in or deleted, only by a credential that
takes in every account the jobs copy into. So does an inbound webhook route, which takes
an address's mail instead of its mailbox: it is made, changed or removed only by a
credential that takes in the account at its address (or the one its plus tag reaches),
and a whole domain's route only by one that takes in every account of the domain with an
admin level or a role ([InboundWebhooks.md](InboundWebhooks.md)). `build/check-role-choke-point.py` fails a new
route that changes an account by its address and is not in that one list. Three are
left out, each for its reason: the two unlocks (`/unlock` and `/review/unlock`) —
letting the owner try again takes nothing over — and `POST .../export`, which reads the
account out to the server's disk, changes nothing in it, and needs `manage_roles`,
which only the server administrator holds. The refusal says so: *that account holds
authority this credential does not*.

Over COM the same question is asked where the write is made, not where the object was
reached — a rule's `AccountID`, a fetch account's `AccountID`, a rule action's or
criterion's `RuleID` can be pointed elsewhere after the object was reached — by
`Account.Save`, `Account.Delete`, `Accounts.Delete` and `DeleteByDBID`, `Account.MoveTo`,
`Application.ResetPassword`, the app passwords (`AppPasswords.Add`, `Delete`,
`DeleteByDBID`, `AppPassword.Save` and `Delete`), and every write to what is in an
account: `FetchAccount.Save`, `Delete`, `DownloadNow` and `AccountID`, `FetchAccounts.Delete`
and `DeleteByDBID`; `Rule.Save`, `Delete`, `MoveUp`, `MoveDown` and `AccountID`,
`Rules.DeleteByDBID`, and the same for a rule's actions and criteria (`RuleID`
included); `IMAPFolder.Save` and `Delete`, `IMAPFolders.Add` and `DeleteByDBID`;
`IMAPFolderPermission.Save` and `Delete`, `IMAPFolderPermissions.Add`, `Delete` and
`DeleteByDBID`; `Message.Save` and `Copy`, `Messages.DeleteByDBID` and `Clear`; and
`Account.DeleteMessages`, `RevokeDevice`, `ClearRecoveryAddress`, `ClearRecoveryCodes`
and `RemovePushSubscriptions`. Each asks that the credential reach the account as
`Links.Account` decides — the server administrator, an administrator of its domain, or
the account itself — and take in its authority; a domain's or the server's rules answer
to that domain's administrator or to the server administrator. An event script's
objects carry no credential and are left as they were, on the thread running the
script. A `Message` or `FetchAccount` a program creates itself (`CreateObject`, `new`)
carries none of its own either, and since 6.3.6 answers each of these with the
credential of an `Application` that the program's own process holds and has called
`Authenticate` on: the most privileged of them where it holds several (the server
administrator, then a domain administrator, then any other, and the latest signed in
among equals), and none once that `Application` is released or a later `Authenticate`
on it fails. That gives the process nothing it does not hold already: it could ask the
same `Application` for `CreateMessage`, or reach the account's `FetchAccounts`. The
process is the one the call came from over local RPC, COM's transport between the
processes of one machine - by the process id the kernel hands RPC with the call and that
process's creation time, which must be the one it had when it signed in, so a process
given the id of one that has ended is not taken for it. A call from another machine
answers for no process. The process is the unit: one that also runs code it does not
trust - a web server that runs several sites or applications in one process - lends its
sign-in to all of them, as it shares its memory with them, so a program signs in from a
process of its own there. A .NET program keeps its `Application` alive (`GC.KeepAlive`)
until the message is saved: the garbage collector releases one that the program no
longer uses, and with it the sign-in. Anywhere else an object without a credential is
refused every one of these: a `Message` or `FetchAccount` a program created itself while
its process holds no `Application` signed in, and what an `Application` handed out
before a later `Authenticate` on it failed. A program may also compose mail with
`Application.CreateMessage` after `Authenticate`. `Attachments.Add` and
`Attachment.SaveAs`, which read and write a file on the server's disk by its path,
answer the server administrator and the event scripts alone.

What names a domain beyond the credential's own
-----------------------------------------------

A credential confined to named domains — a domain administrator, a help desk, a
full-scope API key issued for named domains, an account assigned `serveradmin`
with a list of domains — administers what is inside its domains. A handful of
changes name, or reach, something beyond them even on a
route that names one of the credential's own domains, and are the server
administrator's alone whatever permission the confined credential holds. Each is
refused the same way over REST (`Authorize_`, and the domain and rule handlers)
and over COM (`COMAuthentication`, `InterfaceDomain`, `InterfaceDomainAlias`,
`InterfaceRule` and `InterfaceRuleAction`), and a refusal changes nothing:

* **A domain's name.** `PUT /api/v1/domains/{domain}` and `Domain.Name` rename the
  domain; the name the server answers for is the server's to set, as which
  domains exist is, so a confined credential keeps every other setting on its
  domain and is refused a change of the name. A `PUT` that sends the name
  unchanged is an ordinary save and is not refused.
* **A domain alias.** Creating one (`POST /api/v1/domains/{domain}/domain-aliases`,
  `DomainAliases.Add` with `DomainAlias.Save`) maps a name onto the domain, which
  the server applies before it decides a domain is local, so which names map in
  is the server's. A confined credential lists and removes its own domain's
  aliases and is refused creating one.
* **The custom roles.** `POST /api/v1/roles`, `PUT` and `DELETE
  /api/v1/roles/{name}`: a role is assigned across domains, so changing one is not
  a domain's to do. A confined credential reads the role definitions
  (`view_configuration`) and is refused changing them, even a full key issued for
  named domains, which holds `manage_roles`.
* **The metric history.** `GET /api/v1/metrics/history` is counted from
  server-wide counters that carry no domain, as the report's server-wide sections
  are, so a confined credential is refused it and reads its own domains' traffic
  from the per-domain reports instead. `GET /api/v1/status` gives the current
  totals and stays readable, by design — its version and state are what the Deck
  needs.
* **The relay host.** A domain's relay host is where delivery connects when the
  domain's mail leaves, with no address rule, so a confined credential's relay
  host is held to a public address, as a reader's fetch host is
  ([RestrictedDelivery.md](RestrictedDelivery.md) names the reader rule): an
  internal name or address is the server administrator's to set. Only a change of
  the relay host is judged, against the domain as stored: a save that leaves it as
  it is - a `PUT` that does not name it or sends it back unchanged, and
  `Domain.Save` after the Control Panel's domain dialog, which writes back every
  field it loaded - is never refused on it, so a domain whose server administrator
  set an internal smart host stays the confined credential's to administer, and
  clearing the host is never refused either.
* **Three rule actions.** On a domain's or an account's rule, `bind_to_address`,
  `send_using_route` and `script_function` reach past the rule's owner — to one
  of the server's addresses, a configured route's stored credentials, or a
  function of the server's event script — and are the server administrator's to
  set; a confined credential keeps every other rule action
  ([MailFlowRules.md](MailFlowRules.md)). On a global rule, which only the server
  administrator saves, all three are as they were.

Mail that a COM credential queues - `Message.Save` of a new message to deliver, and of
one already in the queue, whose file its delivery reads - claims to be from only an
address that credential may send as (since 6.3.6): its envelope sender, and the one
mailbox in its `From` field and in its `Sender` field, read in the header as it is
written. The server administrator and the event scripts send as anybody. An account
sends as the addresses authenticated SMTP lets it write in `MAIL FROM` when
`SmtpAuthenticatedSenderCheck` is on, and the webmail's send route always
(`SMTPConnection::SenderPermittedFor`, [SendAs.md](SendAs.md)): its own address, an
alias of it, a mailbox whose owner granted it the post right on their INBOX, an address
it verified, a masked address of its own, and the null sender. A domain administrator
sends as those and as any address of a domain it administers - its own, or the ones a
`domainadmin` role given to it names - or of that domain's aliases, whose passwords it
may already set and whose mail it may already read: a domain this server holds, since a
role given every domain (`*`) reaches any name, and not an account there whose authority
it does not take in, such as the server administrator's mailbox or an administrator of
another domain. For them a `From` or `Sender` field written more than once, or naming
anything but one mailbox written as one - a list, an address in a comment or in the
display name, as written or once its encoded words are decoded - is refused, since
another program could read another address in it. So is a header not written as one: for
them every field stands on a line of its own under a name of printable ASCII, with no
NUL, lone carriage return or line feed, or line of white space alone in a value, and the
header as the program gave it is the header written. The writer keeps a value to one
field, writing a line break that does not fold it - a space or a tab and more after it -
as spaces, so that a `Subject` that holds a line `From: ...` adds no field; for them a
value that holds one is refused rather than sent changed, and the `From` and `Sender`
read are the ones written. COM holds an account to this
whatever `SmtpAuthenticatedSenderCheck` says, as the webmail does: COM writes the
envelope and the fields apart, so it is the fields that the recipient reads that matter,
and before 6.3.6 no mail queued over COM carried an account's credential (a message a
program made itself carried none), so there is no signed-in sender's habit to keep, as
the switch keeps SMTP's.

The server administrator's own mailbox may sit in a domain somebody else administers;
that domain's administrator is refused all of these on it, over both interfaces.

`administer_server` is the whole server only where the grant reaches every domain.
A full key confined to named domains, and an account assigned `serveradmin` with a
list of domains, are `serveradmin` confined to those domains: the server
administrator in each of them and in no other, which over COM is a domain
administrator of the domains the list names (the server's settings, a domain alias,
a domain's name and the rest above are refused, as they are over REST), and over
REST is refused every server-wide route and every domain the list leaves out. The
decision is one place — `RoleManager::Allows` answers `administer_server` only for a
grant with no list, which `COMAuthentication::GetIsServerAdmin` and
`RestApiServer::HoldsServerAuthority_` both read. Such a grant is judged by its list
like any confined grant: it takes in an administrator of those domains, and
not an account there that administers another domain, or reaches every domain — the
server administrator, by admin level or by role, and an auditor given no list — nor
another account holding `administer_server`. Before SCIM was added it took in every
account, whatever its list said.

### What `manage_settings` reaches

**`manage_settings` is the server administrator's equal, and is to be given only to
somebody you would give the administrator password.** That holds for a custom role
holding it over every domain, for an account assigned `serveradmin`, and for an API
key of `"scope":"full"`, which is `serveradmin`. It is not a narrower permission than
it looks; it is decided so, in 6.3.6, because it cannot be made one without taking
the settings themselves away from it:

* **Network reach.** The settings name the hosts the server connects to, and the
  server connects to them as itself, inside its own network as well as outside: the
  breach check's endpoint (`breach_check_url`), the key server (`keyserver_url`), the
  sandbox's (`sandbox_*_url`), SpamAssassin and ClamAV (`spamassassin_host`,
  `clamav_host`), the syslog host (`syslog_host`), the message store's object storage
  (`message_store_object_endpoint`), the identity provider (`oidc_issuer`), the mail
  importers' token addresses (`import_oauth_*_token_url`), an alert rule's webhook. A
  holder of `manage_settings` makes the server reach what the server reaches, which on
  most networks is more than the holder reaches.
* **Commands.** The external virus scanner's command line (`custom_scanner_executable`,
  `clamwin_executable`) is a setting, and the server runs it as its service account.
* **Some `[Settings]` keys, by what they are for - not the section by name, not the key
  store.** A holder of `manage_settings` writes the `[Settings]` keys behind a few typed
  settings: `tarpit_count` and `tarpit_delay` (`SmtpTarpitCount`,
  `SmtpTarpitDelaySeconds`), `rewrite_envelope_from_when_forwarding`
  (`RewriteEnvelopeFromWhenForwarding`) and `user_interface_language` (`UseLanguage`);
  the webmail's branding and its file-link policy (`WebmailBrandName`,
  `WebmailBrandLogo`, `WebmailAnnouncement` and the `WebmailLink*` keys, the server's
  and each domain's - a confined holder its own domains' alone); and a settings profile's
  `[Settings]` entries when it applies or reverts one
  (`POST /api/v1/settings/profiles/{name}/apply` and `/revert`; a full-scope key too),
  with the profile's own values or the ones the server had before it
  ([Profiles.md](Profiles.md)). Each is that key, with that row's check or the
  profile's values; none names a key of its own choosing. What stays the administrator
  password's alone is the section by name: listing it, writing or removing any key of
  it through `/api/v1/settings/ini` - `AutoBanCommand`, the command the auto-ban runs,
  among them - and putting a `[Settings]` key back from the audit trail; and the API
  keys (above). Until 6.3.6's second security review this paragraph said the whole
  section was the administrator password's, which the rows and the profiles above had
  never kept to.

Two **readings** of the network that need no setting changed and reach API keys as
well are held to less than the administrator. The first is the breach check's
endpoint test (`POST /api/v1/settings/breach-check/test`), which takes any URL in its
body and reports at once whether something answered, its status, how many entries
came back and how long it took. (The coarser readings that come with the reach itself stay with it: an
alert rule's webhook says whether its endpoint answered 2xx in the events' delivery
state, and a webhook subscription's test, run and last error say whether its endpoint
answered, refused or timed out - for an account whose role holds `manage_settings`
or `manage_server` respectively, never for an API key; [Webhooks.md](Webhooks.md).)
Asked by any credential but the
administrator password - an API key, an account holding a role - since 6.3.6 it
reaches a public address only, over https, through `HttpProxy` when one is set, the
body's endpoint and the configured one alike, and answers `reachable: false` with the
reason for one on the server's own network; 6.3.4 and 6.3.5 tested any address for
any of them. The administrator password tests a mirror on the server's own network as
before. An inbound webhook route made by any credential but the administrator is held
to public addresses in the same way ([InboundWebhooks.md](InboundWebhooks.md)).

The second is the never-ban list's check (`POST /api/v1/never-ban-names/check`), which
takes any address and reports the reverse names the server's own resolver gives for it
and whether each resolves back - for an address inside the network, the names
split-horizon DNS keeps for the hosts there. Asked by any credential but the
administrator password, since 6.3.6's second security review it answers 403 for an
address that is not public - private, loopback, link-local, carrier NAT, reserved or
documentation - and checks a public one as before; until then it checked any address
for any of them ([AutoBan.md](AutoBan.md)).

Not every reading is held so. The server's diagnostics (`POST /api/v1/diagnostics/run`,
`manage_server`, a full-scope key too) connect, when no relayer is set, to port 25 of
the host named as `test_domain` - any name or address, one on the server's own network
among them - and say which addresses the name resolved to and whether each took the
connection. That is one port, and
the reach the holder has anyway: a holder of `manage_settings` or `manage_server` is to
be somebody you would give the administrator password (above).

### The message trace

The trace search matches an address as a substring across every message the server
has recorded, so it cannot be confined to a domain by the search itself. A role that
reaches every domain (the auditor, an unrestricted key) searches it as the server
administrator does. A role confined to named domains — the help desk — traces one whole
address in a domain it reaches, and is shown only the events that name that address
exactly: `user@example.test` does not also show `otheruser@example.test`, and never
`user@example.test.elsewhere`. The most recent events for anyone and one queued
message's story are server-wide.

### The logon-failure list

`POST /api/v1/settings/logon-failures/clear` is classified with `unlock_account`, which
`helpdesk` holds — and it is **server-wide**, so a help desk confined to named domains is
refused it. That is not an oversight in the route, it is what the table can say:
`hm_logon_failures` has three columns — `ipaddress1`, `ipaddress2`, `failuretime` — and
records the address a failure came from and nothing else. There is no account and no
domain in it, so there is no per-domain form of "clear the failures for my customer"
to offer: an address is not a domain, and the same address is what a brute-force run
against every domain on the server looks like. Clearing the list resets the auto-ban
counter for everybody, which is why only a credential that reaches every domain may do
it. `Settings.ClearLogonFailureList` over COM answers the server administrator alone.

A help desk that reaches only its own domain lifts a lockout for the person in front of
it instead: `POST /api/v1/accounts/{address}/unlock` clears the failed-authentication
lockout and the POP3 mailbox lock for **one account**, is scoped to that account's
domain, and is on the audit trail as `unlocked`. A password reset clears the lockout
with it. If the auto-ban has already placed a ban, that is an IP range and is removed
through `/api/v1/ipranges`, which is `manage_settings` and server-wide for the same
reason.

### The reports

`GET /api/v1/reports` needs **both** `view_configuration` and `view_message_trace`. Most of a report
is counts per domain and per day, but its `senders` and `recipients` sections list which addresses
sent and received the most, which is who mails whom: the message trace's to show, not the
configuration's. `domainadmin` and `auditor` hold both and read the reports as before (the domain
administrator for its own domains); `helpdesk` holds only the trace and is refused, as it always was;
a custom role holding `view_configuration` alone is refused and told it lacks `view_message_trace`.
The Control Deck leaves the *Reports* view out for a role without both. This is the one route that
needs two permissions — `RestApiServer::SecondPermissionFor_` names the second — and
`build/check-role-choke-point.py` holds both to the auditor rule. See [Reports.md](Reports.md).

### PST import files

A migration job of kind `pst` needs what every migration job needs, `manage_accounts`
in the target account's domain, and `manage_mailbox` there as well, since it writes
into the account's folders - asked when the job is added
(`RestApiServer::CallerMayManageMailboxIn_`, beside the choke point, as the archive's
second permission is). Its file is looked for only in that domain's own
subdirectory of `pst_import_directory` (`<directory>/<domain>/`), resolved as the file
system resolves it, links and junctions followed: a domain administrator reaches only
the files placed there for their own domain, and not a file left for another domain's,
however the name is spelled; a refusal names the file below that directory, never where
the directory is on the server. The rule is the same for the server administrator. See
[Migration.md](Migration.md), *Outlook data files (PST)*.

### What a refusal says

A route a role may not call answers **403** with the role and the permission it lacks:
`the role 'helpdesk' does not hold the permission 'manage_settings'`. An address in a
domain the role does not reach answers `the role 'helpdesk' does not reach that domain`
**whether or not that account exists** — a refusal that told "no such account" from
"not yours" would let a help desk in one domain list every mailbox on the server one
request at a time. Over COM, `Application.ResetPassword` and `UnlockAccount` answer
`No such account.` alike for both.

Giving somebody a role
----------------------

**In the Control Panel:** *Accounts & domains → Administrator roles* lists the roles
and who holds them. Type an address, choose a role, optionally name the domains, and
*Give the role*; select a row and *Take the role away*. The role box starts with no
role chosen, and `serveradmin` - the role that holds `administer_server`, everything in
every domain - is asked about before it is given, Cancel being the answer Enter gives.

**In the Control Deck:** the *Roles* view does the same.

**Over REST** (needs `manage_roles`):

```
PUT    /api/v1/role-assignments/helen@example.test   {"role":"helpdesk","domains":"example.test,example.org"}
DELETE /api/v1/role-assignments/helen@example.test
GET    /api/v1/role-assignments
GET    /api/v1/roles
```

**Over COM:** `Application.AssignRole("helen@example.test", "helpdesk", "example.test")`,
an empty role name to take it away; `Account.RoleName`, `RoleDomains` and
`RolePermissions` read it back.

**An API key:** `POST /api/v1/apikeys` with `"role":"helpdesk"` writes a `Role=` line
to the key store. A key without one is what it always was: `Scope=full` is
`serveradmin` and `Scope=readonly` is `auditor`. A full key unconfined is the server
administrator's equal - it holds `manage_settings` over every domain ([What
manage_settings reaches](#what-manage_settings-reaches)) - and is kept as the
administrator password is. `Domains=` confines a key with a role
exactly as it confines one without; an empty `Domains=` is every domain for a key,
whatever its role.

### Domains

An assignment with no domains reaches **every domain** for `serveradmin` and
`auditor`, and **the account's own domain** for every other role, custom roles
included. Naming domains is how an administrator widens a confined role; an auditor
given a list is confined to it, and so is `serveradmin` given one — an account
assigned `serveradmin` with a list is the server administrator in the domains it
names and a domain administrator of them over COM, not the whole server (see
[What names a domain beyond the credential's own](#what-names-a-domain-beyond-the-credentials-own)). `*` is every domain, for any role — which is what a
custom role holding a server-wide permission such as `manage_settings` needs before an
account can use it at all. (A key's `Domains=` line still reads `*` as the name of no
domain, as it always has.) The listings answer `every_domain` (REST) or a fourth
field (COM) so that nobody has to remember the rule to read an assignment. A
credential confined to named domains sees only the assignments of accounts in those
domains: who administers another customer's domain is not a domain administrator's to
know.

### Custom roles

Made over REST (`POST /api/v1/roles` with a name and a comma-separated permission
list, needs `manage_roles`), listed and given out in both editors. A permission this
server does not know is dropped rather than refused, so a custom role always ends with
less authority than it asked for and never more.

**A custom role somebody holds cannot be deleted.** `DELETE /api/v1/roles/{name}` answers
`409` with a sentence saying how many accounts hold the role, and the Deck's *Delete* on
the role's row shows that sentence: each holder must first be given another role or have
this one taken away. The assignments are never taken away to make a role deletable,
because an account with no assignment holds what its admin level gives it - an account
whose level says server administrator, narrowed to a custom role, would be the server
administrator again, with nothing said. Taking a role away from an account is a decision
about that account, and on the audit trail as one. A delete that loses a race with an
assignment written after the holders were counted is refused with the same `409`: the
assignments' foreign key to the role has no cascade, so the database refuses it and the
holders are counted again. A role nobody holds deletes as it always has, a built-in role
never does (`400`), and COM has no way to delete a role at all.

**An assignment decides, even when its role holds nothing.** A custom role whose stored
permissions name nothing this version has - a row edited by hand, or a permission a later
version renamed - gives its holders nothing, and they do not fall back to their admin
level: an account narrowed from server administrator to such a role is an ordinary
account until it is given a role that holds something or has the assignment taken away.
Neither editor saves a role whose permissions come to nothing, so only an edit to the
table, or an upgrade, leaves one.

Where each role works
---------------------

| | Control Deck | REST API | Control Panel (COM) |
|---|---|---|---|
| `serveradmin` | Every view | Every route | Every page |
| `domainadmin` | The dashboard, DANE / TLSA, domains, fetch accounts, lists, roles and the reports for its domains; not the settings, rules, routes, remote domains, certificates, ports, IP ranges, groups, backup, server messages, queue, logs or audit trail, which are server-wide | Its domains' accounts, aliases, lists and mailboxes, the status, and who holds a role in its domains | Domains and Administrator roles, as a domain administrator always has |
| `helpdesk` | Domains, fetch accounts and lists, with *Reset password* and *Unlock* on every account | The reset, the unlock, the account listings and the trace of an address in its domains | Welcome and About only — use the Deck. `Application.ResetPassword`, `UnlockAccount` and `GlobalObjects.MessageTrace` answer a script |
| `auditor` | Every view that reads, the audit trail and the reports included | Every read, the audit trail and the alert rules and events included (an API key is refused those whatever its role); every write refused | Welcome, About and Administrator roles. COM's settings objects answer only the server administrator, so an auditor reads the configuration through the Deck or REST |

A page or a view a role may not use is left out rather than shown and refused, and so
is a server-wide view for a role confined to named domains. That is
a courtesy, not the boundary: the server refuses the request whatever a page draws, and
a Control Panel connected to a server that predates roles shows every page.

An account that holds a role signs in to the Control Deck with its own address and
password (and second factor, where it has one). A request authenticated with one of the
account's **app passwords** never carries its role: an app password is a credential for
a mail program. Over REST it is an ordinary account's request; over COM it is not a
logon at all - `Application.Authenticate` and `AuthenticateWithCode` answer an app
password as they answer a wrong password, whatever role the account holds. A **support
session** an administrator opens into a mailbox is the
portal as that person and does not carry their role either. Nor does it reach what makes
up the person's cryptographic identity: it reads the OpenPGP ring, the key published for
the address, the S/MIME store and the zero-access state without the wrapped private
keys, and every write to them - a key stored, published or removed, a correspondent's
key marked verified or its change accepted, zero access switched on or off or its key
re-wrapped - is refused with 403 and `code` `support_session`, as the S/MIME
authority's enrolment already was ([SmimeAuthority.md](SmimeAuthority.md)). Nor can it
touch the person's record of support access: the `support_allowed` and `support_last`
preferences - the consent, and the note of who last opened a session and when, which the
webmail shows until the person dismisses it - are refused to a support session, and
`support_allowed` cannot be given with an app password either.

An account holding a role is still a mailbox holder: its own mail, vacation message and
settings under `/api/v1/me` are its own to change, whatever the role.

For contributors: one decision
------------------------------

"May this credential do this, here" is answered by `RoleManager::Allows` and nothing
else. `build/check-role-choke-point.py` runs on every pull request and fails when:

* `RoleManager::Allows` is called outside `RoleManager`, the REST choke point
  (`RestApiServer::Authorize_`) or the COM one (`COMAuthentication`);
* a REST route is added that is not classified in `RestApiServer::PermissionFor_`,
  `IsSelfServiceRoute_` or `IsSessionRoute_` — a route nobody classified is a route
  that decides for itself;
* a route `IsMutatingRoute_` calls mutating needs a permission the auditor holds, or a
  read needs one it lacks — for a route that needs two permissions
  (`SecondPermissionFor_`), a read needs both held and a write at least one not held, and
  the route must be classified in `PermissionFor_` too.

`--self-test` shows each rule firing on text written to break it.

Every interface installs the acting credential as the thread's actor, twice over and
from the one grant: `RoleManager::CurrentActor`, and the audit trail's own actor
(see [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)). The REST dispatcher installs
both for the length of a request, and COM installs both on every authority question
(`COMAuthentication::InstallActor_`), since a COM call may arrive on a different thread
from the one that authenticated. An event script that signs in to COM does so on the
server thread that fired the event, so the script host takes both actors off the thread
around every script and puts back what was there, and nothing the server does on the
thread afterwards is recorded under the script's credential. What the audit trail records follows the role, not
`AdminLevel`: an account holding `serveradmin` or `domainadmin` is an `administrator`,
every other account is an `account`, and the role a change was made under ends its
detail as `[role: helpdesk]`. A password a help desk resets is therefore on the record
as the help desk's, over COM and over REST.

Storage
-------

Schema step 6047 adds `hm_roles` (`rolename`, `roledescription`, `rolebuiltin`,
`rolepermissions`; the four built-in rows seeded) and `hm_roleassignments`
(`raaccountid`, `raroleid`, `radomains`; one role per account, removed with the
account). Roles are data, not settings: nothing is added to `hm_settings` or to
`hMailServer.ini`.

Both tables are audited: a role created, changed or deleted, and a role given or taken
away, is a row in the audit trail with the credential that did it - in words, not ids. A
role assignment's row names the account by its address and says the role and the domains
it reaches before and after (`role: helpdesk -> auditor; domains: the account's own domain
-> every domain`); a role's row names the role and says its permissions and description
before and after. A delete that is refused - a built-in role, or a custom role somebody
still holds - changes nothing and writes nothing, so the one row a role's deletion adds is
for a deletion that happened, and no assignment ever goes with a role. A change that
changes nothing writes nothing: a role taken from an account that holds none adds no row.

A backup carries each account's assignment with the account (`AccountStores`), with the
role written by its name rather than its id, so that a restore onto another server
finds that server's role of the same name. That matters in both directions: without
it a restore would leave a help desk whose admin level is a normal user holding
nothing, and would hand an account whose admin level is server administrator, but
which had been narrowed to the help desk, the whole server back.

The **custom roles** travel too. Whenever a backup carries the domains or the settings,
the rows of `hm_roles` that are not built in are written into the archive as a
server-wide `<Roles>` store beside the settings, and a restore puts them back **first**,
before any domain is removed and so before any assignment is written. An archive
restored onto a fresh server therefore keeps every assignment, custom roles included. A
role the restoring server already has is **kept as it is there** and never overwritten
with the archive's definition: a role that is here already is this server's
administrator's decision, and every credential holding it that is not in the archive -
an API key issued with that role, whose store no archive carries; on a settings-only
restore, every account holding it - would otherwise be widened with nothing said and no
actor to record it under. A kept role whose permissions or description differ from the
archive's is named in the backup log with both, so that an administrator who wants the
archive's definition makes that change deliberately. A custom role the server has and
the archive does not is left alone; a restored role is judged as one made over REST, so
a permission no custom role may hold is dropped. A role that cannot be written stops
the restore before any domain is removed, and the log names the roles created before it.

The **admin level waits for the assignment.** An account's row is saved before its
assignment can be, and the archive carries the level beside the assignment - so for as
long as the row stood with no assignment beside it, an account narrowed from server
administrator to help desk was the server administrator, and a restore that stopped
between the two (over a folder that would not load, a row that would not write) left it
that way for good. So an account whose archive entry carries an assignment is saved as an
ordinary account, the assignment is put back first thing after the row - before the
account's fetch accounts, rules, folders and other stores - and only then is the archived
level given back, behind the row that narrows it. Whatever fails afterwards, the account
holds at most what its role gives it.

A role that neither the archive nor the restoring server has - a hand-edited archive,
or one taken before custom roles were carried - is not guessed at, and the account is
not left to fall back on its admin level either: the assignment is dropped, the withheld
admin level stays withheld (the account is an ordinary one), and the backup log and the
error log (`HM6129`) name the account, the missing role and the level that was not
given back. An administrator gives the authority back deliberately.
