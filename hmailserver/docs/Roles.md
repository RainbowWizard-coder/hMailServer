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
| `view_mailbox` | Another account's folders and messages | ✅ | ✅ | | ✅ |
| `view_queue` | The delivery queue *(server-wide)* | ✅ | | | ✅ |
| `view_quarantine` | The quarantine *(server-wide)* | ✅ | ✅ | | ✅ |
| `view_archive` | The archive *(server-wide)* | ✅ | | | ✅ |
| `view_logs` | The logs *(server-wide)* | ✅ | | | ✅ |
| `view_message_trace` | The message trace — see below — and, with `view_configuration`, the reports and a domain's suppression list and bounce cou, a domain's suppression list and bounce counts, and a domain's complaints | ✅ | ✅ | ✅ | ✅ |
| `view_audit` | The audit trail *(server-wide)* | ✅ | | | ✅ |
| `manage_accounts` | Create, change and delete accounts, aliases, lists, groups, fetch accounts and app passwords, a domain's own allow and block list, a domain's spam traps, a domain's DKIM key rotation (rotate, check, switch, retire and cancel now), a domain's suppression list (an address added or removed by hand), and an account's OpenPGP public keys (stored or removed domain's suppression list (an address added or removed by hand), and renaming a mailbox or moving one to another domain - which needs the permission in **both** domains, the one the mailbox leaves and the one it goes to ([MailboxMove.md](MailboxMove.md)) | ✅ | ✅ | | |
| `reset_password` | `POST /api/v1/accounts/{address}/password`, `Application.ResetPassword` | ✅ | ✅ | ✅ | |
| `unlock_account` | `POST /api/v1/accounts/{address}/unlock` and `Application.UnlockAccount` (both locks), `Account.UnlockMailbox` (the POP3 mailbox lock alone; an account's own is its own to clear) - each on the audit trail as `unlocked` - and clearing the logon-failure list (`POST /api/v1/settings/logon-failures/clear` *(server-wide)*; `Settings.ClearLogonFailureList` answers the server administrator only), on it as `logon failures` / `cleared`. See [The logon-failure list](#the-logon-failure-list) for what a confined help desk does instead | ✅ | ✅ | ✅ | |
| `manage_mailbox` | Another account's folders, messages and folder rights, and a support session | ✅ | ✅ | | |
| `manage_queue` | Retrying or deleting a queued message *(server-wide)* | ✅ | | | |
| `manage_quarantine` | Releasing or deleting held mail *(server-wide)* | ✅ | ✅ | | |
| `manage_archive` | Holding and releasing archived messages *(server-wide)* | ✅ | | | |
| `manage_settings` | Every setting, listener, certificate, route, rule and anti-spam list (the server's allow and block list among them), an alert rule, a test alert, and the breach check's endpoint testthe breach check's endpoint test, and clearing the record of spam trap hits *(server-wide)* | ✅ | | | |
| `manage_domains` | Which domains exist *(server-wide)* | ✅ | | | |
| `manage_server` | Reinitialise, back up, reload scripts, clear caches, install an update, run the alert pass, the DKIM key rotation pass, the suppression list's sweep or the feedback loops' sweep, the DKIM key rotation pass or a reputation check now *(server-wide)* | ✅ | | | |
| `manage_roles` | Roles and who holds them *(server-wide)* | ✅ | | | |
| `send_through_api` | Sending mail as an application through `POST /api/v1/send`, as the one sender an **API key** names in its `send_from` - the permission alone sends nothing, and only a key reaches the route ([SendApi.md](SendApi.md)). A custom role may be given it, which is how a key that sends and does nothing else is made; a key reads its own sends without any permission, and nobody else's | ✅ | | | |
| `provision` | Provisioning over SCIM 2.0 at `/scim/v2` - the organisation's identity provider making, renaming, suspending and removing accounts, with their aliases, and distribution lists ([Scim.md](Scim.md)). Only an **API key** reaches `/scim/`, and its reads need this as its writes do, so an auditor's key reads nothing there (an auditor reads the same accounts at `/api/v1/domains/{domain}/accounts`). A custom role may be given it, which is how a key that provisions and does nothing else is made; a key confined to named domains provisions those alone | ✅ | | | |

The REST API's **key store** (`/api/v1/apikeys`) and the `[Settings]` section of
`hMailServer.ini` (`/api/v1/settings/ini`) answer to the administrator password and to
nothing else — not to an account holding `serveradmin`, and not to a key of any role or
scope. A key that could mint keys could issue itself a replacement with no restriction
on it, and the file holds the server's secrets. An account is refused with a 403 and
the reason; a key with the 401 that keeps "valid but not permitted" indistinguishable
from "not a key".

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
accounts of its domain as it always could, and is refused an auditor's. Over COM this
is `Account.Save` and `Application.ResetPassword`; over REST, the password reset,
`PUT /api/v1/accounts/{address}`, giving and taking away a role, and SCIM's writes. The
refusal says so: *that account holds authority this credential does not*.

`administer_server` takes in everything only where it reaches every domain. A full
key confined to named domains is `serveradmin` confined to them, and it is judged by
its list like any confined grant: it takes in an administrator of those domains, and
not an account there that administers another domain, or reaches every domain — the
server administrator, by admin level or by role, and an auditor given no list. Before
SCIM was added it took in every account, whatever its list said.

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
*Give the role*; select a row and *Take the role away*.

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
`serveradmin` and `Scope=readonly` is `auditor`. `Domains=` confines a key with a role
exactly as it confines one without; an empty `Domains=` is every domain for a key,
whatever its role.

### Domains

An assignment with no domains reaches **every domain** for `serveradmin` and
`auditor`, and **the account's own domain** for every other role, custom roles
included. Naming domains is how an administrator widens a confined role; an auditor
given a list is confined to it. `*` is every domain, for any role — which is what a
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
a mail program. A **support session** an administrator opens into a mailbox is the
portal as that person and does not carry their role either.

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
