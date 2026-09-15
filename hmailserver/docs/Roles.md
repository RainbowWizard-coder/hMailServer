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
| `view_configuration` | The status, settings, listeners, certificates, rules, routes, anti-spam lists, the roles and who holds them — never a secret | ✅ | ✅ | | ✅ |
| `view_accounts` | Domains, accounts, aliases, lists, groups, fetch accounts, the OpenAPI document | ✅ | ✅ | ✅ | ✅ |
| `view_mailbox` | Another account's folders and messages | ✅ | ✅ | | ✅ |
| `view_queue` | The delivery queue *(server-wide)* | ✅ | | | ✅ |
| `view_quarantine` | The quarantine *(server-wide)* | ✅ | ✅ | | ✅ |
| `view_archive` | The archive *(server-wide)* | ✅ | | | ✅ |
| `view_logs` | The logs *(server-wide)* | ✅ | | | ✅ |
| `view_message_trace` | The message trace — see below | ✅ | ✅ | ✅ | ✅ |
| `view_audit` | The audit trail | ✅ | | | ✅ |
| `manage_accounts` | Create, change and delete accounts, aliases, lists, groups, fetch accounts and app passwords | ✅ | ✅ | | |
| `reset_password` | `POST /api/v1/accounts/{address}/password`, `Application.ResetPassword` | ✅ | ✅ | ✅ | |
| `unlock_account` | `POST /api/v1/accounts/{address}/unlock`, `Application.UnlockAccount`, clearing the logon-failure list | ✅ | ✅ | ✅ | |
| `manage_mailbox` | Another account's folders, messages and folder rights, and a support session | ✅ | ✅ | | |
| `manage_queue` | Retrying or deleting a queued message *(server-wide)* | ✅ | | | |
| `manage_quarantine` | Releasing or deleting held mail *(server-wide)* | ✅ | ✅ | | |
| `manage_archive` | Holding and releasing archived messages *(server-wide)* | ✅ | | | |
| `manage_settings` | Every setting, listener, certificate, route, rule and anti-spam list *(server-wide)* | ✅ | | | |
| `manage_domains` | Which domains exist *(server-wide)* | ✅ | | | |
| `manage_server` | Reinitialise, back up, reload scripts, clear caches, install an update *(server-wide)* | ✅ | | | |
| `manage_roles` | Roles and who holds them *(server-wide)* | ✅ | | | |

The REST API's **key store** (`/api/v1/apikeys`) and the `[Settings]` section of
`hMailServer.ini` (`/api/v1/settings/ini`) answer to the administrator password and to
nothing else — not to an account holding `serveradmin`, and not to a key of any role or
scope. A key that could mint keys could issue itself a replacement with no restriction
on it, and the file holds the server's secrets. An account is refused with a 403 and
the reason; a key with the 401 that keeps "valid but not permitted" indistinguishable
from "not a key".

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
is `Account.Save` and `Application.ResetPassword`; over REST, the password reset and
`PUT /api/v1/accounts/{address}`. The refusal says so: *that account holds authority this
credential does not*.

### The message trace

The trace search matches an address as a substring across every message the server
has recorded, so it cannot be confined to a domain by the search itself. A role that
reaches every domain (the auditor, an unrestricted key) searches it as the server
administrator does. A role confined to named domains — the help desk — traces one whole
address in a domain it reaches, and is shown only the events that name that address
exactly: `user@example.test` does not also show `otheruser@example.test`, and never
`user@example.test.elsewhere`. The most recent events for anyone and one queued
message's story are server-wide.

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
less authority than it asked for and never more. Deleting a custom role deletes its
assignments; an account that held it falls back to what its admin level gives it.

Where each role works
---------------------

| | Control Deck | REST API | Control Panel (COM) |
|---|---|---|---|
| `serveradmin` | Every view | Every route | Every page |
| `domainadmin` | The dashboard, DANE / TLSA, domains, fetch accounts, lists, roles and the reports for its domains; not the settings, rules, routes, remote domains, certificates, ports, IP ranges, groups, backup, server messages, queue, logs or audit trail, which are server-wide | Its domains' accounts, aliases, lists and mailboxes, the status, and who holds a role in its domains | Domains and Administrator roles, as a domain administrator always has |
| `helpdesk` | Domains, fetch accounts and lists, with *Reset password* and *Unlock* on every account | The reset, the unlock, the account listings and the trace of an address in its domains | Welcome and About only — use the Deck. `Application.ResetPassword`, `UnlockAccount` and `GlobalObjects.MessageTrace` answer a script |
| `auditor` | Every view that reads, the audit trail and the reports included | Every read, the audit trail included (an API key is refused the trail whatever its role); every write refused | Welcome, About and Administrator roles. COM's settings objects answer only the server administrator, so an auditor reads the configuration through the Deck or REST |

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
  read needs one it lacks.

`--self-test` shows each rule firing on text written to break it.

Every interface installs the acting credential as the thread's actor, twice over and
from the one grant: `RoleManager::CurrentActor`, and the audit trail's own actor
(see [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)). The REST dispatcher installs
both for the length of a request, and COM installs both on every authority question
(`COMAuthentication::InstallActor_`), since a COM call may arrive on a different thread
from the one that authenticated. What the audit trail records follows the role, not
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
away, is a row in the audit trail with the credential that did it.

A backup carries each account's assignment with the account (`AccountStores`), with the
role written by its name rather than its id, so that a restore onto another server
finds that server's role of the same name. That matters in both directions: without
it a restore would leave a help desk whose admin level is a normal user holding
nothing, and would hand an account whose admin level is server administrator, but
which had been narrowed to the help desk, the whole server back. A role the restoring
server does not have is not guessed at: that assignment is left out and the backup log
names the account and the role.
