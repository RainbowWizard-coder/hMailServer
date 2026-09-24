# SCIM provisioning

An organisation whose people are kept in an identity provider - Microsoft Entra
ID, Okta - wants their mailboxes to follow the directory: made when somebody
joins, renamed when their address changes, suspended when they leave and removed
a month later, with their aliases and the lists they belong to. That is what
SCIM 2.0 (RFC 7643 and RFC 7644) is for, and this server answers it at
`/scim/v2` on the REST listener.

**The LDAP backend authenticates; SCIM provisions.** The two are different
questions. Directory authentication ([README](../../README.md), *LDAP directory
authentication*) checks a password against the directory when somebody signs in,
and directory provisioning reads the directory on a schedule. SCIM is the other
direction: the identity provider calls this server and says what changed, as it
changes. And it is the deliberate version of what single sign-on's
`oidc_auto_provision` does in passing ([PasskeysAndSingleSignOn.md](PasskeysAndSingleSignOn.md),
*Mapping and provisioning*): that makes a mailbox for whoever turns up with a
verified address and never touches it again; SCIM makes, changes and removes
exactly the people the organisation assigned to this server.

| Route | What it does |
|---|---|
| `GET /scim/v2/ServiceProviderConfig` | What this server supports: patch, filter (200 at most), changePassword and etag; not bulk and not sort |
| `GET /scim/v2/Schemas`, `/Schemas/{urn}` | The User and Group schemas, listing only what this server keeps |
| `GET /scim/v2/ResourceTypes`, `/ResourceTypes/User`, `/ResourceTypes/Group` | The two resource types |
| `GET`, `POST /scim/v2/Users` | The accounts: listed and filtered, or one made |
| `GET`, `PUT`, `PATCH`, `DELETE /scim/v2/Users/{id}` | One account: read, replaced, changed, removed |
| `GET`, `POST /scim/v2/Groups` | The distribution lists: listed and filtered, or one made |
| `GET`, `PUT`, `PATCH`, `DELETE /scim/v2/Groups/{id}` | One list: read, replaced, changed, removed |

Every answer the REST listener gives under `/scim/` is `application/scim+json`,
the refusals included: an error is RFC 7644's error document,
`{"schemas":["urn:ietf:params:scim:api:messages:2.0:Error"],
"status":"400","scimType":"invalidFilter","detail":"..."}`, and that holds for the
`401`, `403`, `429`, `409` and `500` the listener gives before or around a SCIM
route as well as for the routes' own. The exceptions are the HTTP layer's own
refusals, which come before the path is read: `400` for a request it cannot
parse, `408` for one that does not arrive in time, and `413` for a body over
64 KB - a `PUT` of a very large group can meet that one. Those are the listener's
plain JSON, `{"error":"..."}`.

---

## The key and the permission

SCIM answers to an **API key and nothing else**. An identity provider holds a
bearer token, and a key is the credential that can be confined to domains,
revoked on its own and made to hold one permission alone. The administrator
password, an account's password and a browser session are answered `401` on
`/scim/`, in SCIM's shape, with `WWW-Authenticate: Bearer`, exactly as no
credential is - but for a browser session's `POST`, `PUT`, `PATCH` or `DELETE`
without `X-Requested-With`, which the listener's own cross-site check answers
`403` before the credential is asked about.

The key's role must hold **`provision`** ([Roles.md](Roles.md)). The server
administrator holds it, so **every key issued with `"scope":"full"` provisions**,
the ones issued before schema 6096 included, as they already reach the account
routes; no other built-in role does, so a read-only key - the auditor - is `403`.
The way to issue a key that provisions and does nothing else is a custom role
holding `provision` alone:

```text
POST /api/v1/roles     {"name":"entra-provisioning","description":"Microsoft Entra ID","permissions":"provision"}
POST /api/v1/apikeys   {"label":"Entra provisioning","role":"entra-provisioning"}
```

(the key with the administrator password, as every key route is; the role with
it or with a credential whose role holds `manage_roles`. The second answers the
`hmapi_...` token once, and that token is the identity provider's secret).
`provision` is needed for SCIM's reads as well as its writes: an identity
provider reads only to match its people before it writes them, and an auditor
reads the same accounts, aliases and lists at
`/api/v1/domains/{domain}/accounts`, `/aliases` and `/lists`. An auditor's key
cannot read `/scim/`.

**A key confined to named domains** (`"domains":"example.com"`) provisions those
domains and nothing else, and learns nothing about the rest: every listing is
filtered to them, an account or a list elsewhere is the same `404` as one that is
not there, and an address elsewhere - a user, an alias, a list, a list's member -
is refused `400 invalidValue` with the very sentence a domain this server does not
host gets.

**A key may not change or remove an account whose authority it does not take
in** (`RoleManager::Encompasses`, the rule every interface applies), and is
answered `403`, as the REST account routes answer it. Taking in means holding
every permission the account's role holds, in every domain it reaches:

* a key whose role holds `provision` alone takes in no administrator at all - not
  a domain administrator, not a help desk, not an auditor;
* a full key confined to named domains takes in an administrator of those domains
  and nobody else: not an account in them that **administers another domain**, not
  one that **reaches every domain** - the server administrator, by admin level or
  by the `serveradmin` role, and an auditor given no list of domains;
* a full key issued for every domain takes in every account.

The REST account routes judge a full key confined to named domains by its list
in the same way - `PUT /api/v1/accounts/{address}`, the password reset, giving
and taking away a role. Until SCIM was added they did not: any key holding
`administer_server` took in every account, whatever its list said.

A SCIM request is a request of the REST listener: `RestApiPort` has to be on, the
listener reachable from the identity provider's service, over TLS with a
certificate it trusts (`RestApiCertificateFile`, or the ACME certificate); a body
is at most 64 KB; and the key's requests count against the listener's budget of
200 in ten seconds per credential, beyond which the answer is `429` with
`Retry-After`.

---

## The mapping

**A User is an account.**

| SCIM | This server |
|---|---|
| `id`, `userName` | The account's address. The id is the address, so **a rename changes the id** (below). `userName` is compared without regard to case. |
| `name.givenName`, `name.familyName` | The first and last names the account keeps, sixty characters each. |
| `name.formatted`, `displayName` | The two names composed, answered; not kept apart. A `displayName` written with no name beside it, to an account that has **no name yet**, is split at its last space - `Ada Lovelace` is `Ada` and `Lovelace`. Written alone to an account that has a first or a last name, it is accepted and not kept: the names are what it is composed of, and they feed signatures, the directory and the impersonation check, so a change of the display name alone in the directory - `Ada Lovelace (Finance)` - does not become a first name and a last name nobody chose. Map `name.givenName` and `name.familyName` to change the names. |
| `active` | The account's switch. `false` **suspends** it: every sign-in is refused, its browser sessions are ended and the live IMAP, POP3 and SMTP connections of its sign-in list dropped, mail to it is refused - and nothing is deleted. `true` restores it. |
| `emails` | The address, `type` `work` and `primary`, and every **alias whose value is the address**, `type` `other`, never primary - in the domains the key reaches. |
| `password` | Write-only, never answered. Judged as every password chosen on this server is: the password policy, the reuse history, the published breach corpus when the breach check is on ([PasswordBreachCheck.md](PasswordBreachCheck.md)); a refusal is `400 invalidValue` with the policy's or the check's sentence, and `503` when the breach check could not be asked and is set to refuse. A new password **ends the account's browser sessions and drops its live connections**, as a suspension does: an identity provider resets a password because somebody else may know it. The old password goes into the reuse history once the whole change is written. |
| `externalId` | The identity provider's own id for the person, in `hm_accounts.accountexternalid` (schema 6096): at most 255 characters, compared exactly, and carried by a backup and by COM's `Account.ExternalId`. |
| `meta.resourceType`, `meta.location`, `meta.version` | `User`; the URL the resource is at, from the scheme and the `Host` the request came with; and a weak ETag computed from the representation the server answers. |

**`meta.created` and `meta.lastModified` are not answered.** `hm_accounts` and
`hm_distributionlists` keep no time of creation or of change, and RFC 7643 lets a
service provider leave out an attribute it does not keep. The version is a hash of
what a client would read, so it changes exactly when that does, and it is what
`If-Match` is compared with.

**A Group is a distribution list** ([DistributionLists.md](DistributionLists.md)).

| SCIM | This server |
|---|---|
| `id`, `displayName` | The list's address. |
| `members` | The list's **members**: `[{"value":"<address>"}]`, a user's id or an address elsewhere. |

A list the list manager runs ([MailingLists.md](MailingLists.md)) keeps requests
still waiting and addresses that left as rows beside its members; those are not
members, SCIM neither answers nor changes them, and a list's **membership query**
is neither shown nor changed either. A member SCIM adds is a confirmed member,
receiving each message, as an administrator's add is - a waiting or departed row
for the address is confirmed rather than doubled - and a member SCIM removes has
its row deleted.

What the schemas have and this server does not keep - a User's `title`,
`nickName`, `userType`, `preferredLanguage`, `locale`, `timezone`, `profileUrl`,
`phoneNumbers`, `ims`, `photos`, `addresses`, `entitlements`, `roles`,
`x509Certificates`, `groups`, a name's `middleName` and honorifics, a Group's
`externalId`, and any extension's attributes such as the enterprise user's
`department` and `manager` - is **accepted and not kept**: a body carrying it is
taken, an operation on it does nothing, and the resource answered never carries it.
`GET /scim/v2/Schemas` lists only what is kept. An attribute or a path that names
nothing of the schema at all - a misspelling - is refused by name, `400
invalidValue` or `400 invalidPath`.

---

## Users

### Making one

`POST /scim/v2/Users` with a User. `userName` is required and must be at a
domain this server hosts and the key reaches. `active` is `true` unless the body
says otherwise. An `emails` entry of `type` `work`, or marked `primary`, must be
the `userName` itself; every other entry is an alias, made at once, at a domain
the key provisions - not an address something else has, not one of the domain's
spam traps, and within the domain's limit of aliases, all of which is judged
before the account is made. The account gets the largest size its domain allows
(none, where the domain sets no maximum), its INBOX, and nothing else of note. If
the database refuses an alias after the account is made, the account and the
aliases this request made are removed again - and nothing else: an alias that
already pointed at the address stays.

A body with no `password` - which is what Entra sends - makes the account with
thirty-two random bytes for one, which nobody is ever told: the person signs in
through single sign-on or the directory, or an administrator or the identity
provider sets one later. No policy runs on that password, since nobody chose it.

An address an account, an alias or a list already has is `409 uniqueness`.
`201` answers the User, with `Location` and `ETag`.

### Reading, filtering and paging

`GET /scim/v2/Users/{id}` answers one, with `ETag`. `GET /scim/v2/Users` answers a
`ListResponse`, in the order the accounts were made, with these filters and no
others:

| Filter | Matches |
|---|---|
| `userName eq "..."` | The account at that address, without regard to case |
| `userName co "..."`, `userName sw "..."` | The accounts whose address contains, or starts with, the text |
| `externalId eq "..."` | The accounts with that external id, exactly |
| `emails.value eq "..."` | The account at that address, or the account an alias at that address points at |
| `active eq true`, `active eq false` | The active, or the suspended, accounts |

Any other filter - another attribute, another operator, `and` and `or`, a
malformed one - is `400 invalidFilter`. `startIndex` counts from 1 (a smaller one
is 1) and `count` is at most 200, the default; `count=0` answers the total and no
resource; either one that is not a whole number is `400 invalidValue`. `sortBy`
and `sortOrder` are **ignored**, as RFC 7644 lets a server that says it does not
sort ignore them: a listing comes back in the order the resources were made.
`attributes` and `excludedAttributes` name top-level attributes (`name.givenName`
asks for the whole of `name`); `id`, `schemas` and `meta` are always answered.

An account and an alias are found by their address through the lookups sign-in
and delivery make, which the address columns' indexes serve on SQL Server and
MySQL; on PostgreSQL those lookups compare lower-cased, as every lookup of an
address there does. The aliases whose value is an account's address - its `emails` - are found
by that value, which no index covers on any backend, so reading one user reads
`hm_aliases` through; a page of the listing reads it once for every user on the
page.

### Changing one

**`PUT /scim/v2/Users/{id}`** takes the body a `POST` takes. An attribute it
leaves out is not asserted and is **left as it is** (RFC 7644 leaves that choice
to the server); `emails`, when given, is the whole set of aliases.

**`PATCH /scim/v2/Users/{id}`** takes a `PatchOp` - `op` in any case, as Entra
capitalises it: `add` and `replace` on `active`, `userName`, `externalId`,
`displayName`, `name`, `name.givenName`, `name.familyName`, `password` and
`emails`; `remove` on `externalId`, `displayName`, `name`, `name.givenName` and
`name.familyName`; and these forms of `emails`. A `remove` of `active`,
`userName` or `password`, or a `replace` of one of them with `null`, is `400
invalidValue`, and so is the whole `PATCH` it is part of: an account always has a
switch, an address and a password, so the fix is a `replace` with a value -
`false`, the new address, a new password.

| Path | Means |
|---|---|
| `emails` | `add` adds the entries, `replace` makes them the whole set; `remove` is refused - the account's own address cannot be removed |
| `emails[type eq "work"].value` | The account's own address. Written as itself it is taken - Entra writes it every time - and written as another address it is `400`: a rename is `userName`'s |
| `emails[type eq "other"].value` | An alias: `add` adds one, `replace` makes it the only one, `remove` removes every alias |
| `emails[value eq "..."]` | `remove` removes that alias; the account's own address is `400`, an address `emails` does not hold is `400 noTarget` |

An operation with no `path` carries an object of paths and values, each applied
as an operation of its own - which is how Entra sends a replace, with keys such as
`"name.givenName"` and `"emails[type eq \"work\"].value"`. A boolean may come as
the string `"True"` or `"False"`, as Entra sent it before its 2020 fix. The
operations apply in order to the account as it will be.

Both answer the User as it now is. **`If-Match`** is honoured on `PUT`, `PATCH`
and `DELETE`: a version other than the current one is `412`, and nothing is
written.

**A change is judged whole before anything is written.** The change itself, the
rename's new address and whether the mailbox is in use, every new alias against
what its save will ask - an address something else has, a spam trap - each
domain's limit of aliases counted at the fullest point the writes reach (a rename
keeps the old address as an alias, before the change's removals), the account's
own save, and a new password against the policy, the reuse history and the breach
check: a refusal of any of it is answered with nothing written. Then the writes,
in the order that leaves least to put back - the rename, the account's own
columns, the aliases removed, the aliases made. If the database refuses one of
them after the first, or another request took an address between the judging and
the writing, what this change wrote is put back: the account's columns as they
were, the aliases it removed made again, the aliases it made removed; nothing
changed, and the refusal is answered. **A rename that happened stays**, since
undoing it would be a second move of the mailbox: the answer is then `500`,
saying the account was renamed and where it is now - its new id - and that the
rest was not made, and HM7221 says so in the error log. Sessions a suspension or
a new password ended are not given back.

### Suspending

`active` `false` is the suspension an identity provider sends when somebody
leaves or is disabled. It is the account's own switch, so every sign-in -
IMAP, POP3, SMTP, the webmail, CalDAV and CardDAV, single sign-on - is refused
from the next attempt, and mail to the account is refused; and the suspension
also ends every browser session the account holds and drops every live IMAP, POP3
and SMTP connection its sign-in list registers, as revoking every device does
([DevicesAndSessions.md](DevicesAndSessions.md)) - the moment the account's row
says it is suspended, before anything else of the same change is written. Every
write to an account that leaves it suspended does the same again, so a retry ends
whatever an earlier attempt that failed part way left open. Its mailbox, its
aliases and its lists are untouched. `true` puts it back.

### Renaming

A **`PUT` with a new `userName`** - and, since Entra sends it that way, a `PATCH`
of `userName` - renames the account through the mailbox move
([MailboxMove.md](MailboxMove.md)): everything keyed on the account follows it, the
message store and the Sieve tree are moved, and the aliases, list memberships,
queued recipients and quarantine entries naming the old address are rewritten.
It is refused `409` while the mailbox is in use - a POP3 session, an IMAP session
with a folder selected - and when another account, a list or another account's
alias already has the new address. A new address that is **one of the account's
own aliases** - an identity provider making a person's alias their address, which
it cannot send as two steps - takes that alias's place: the alias is removed just
before the move and the address becomes the account's, and if the move is then
refused the alias is made again. The new address must be at a domain the key
provisions, and the domain the account leaves must have room for the alias the
old address is kept as, when it limits its aliases. A `userName` that differs from
the address only in capitalisation is not a new one: nothing is moved, and the
account keeps its spelling.

**The id changes**, because the id is the address: the answer carries the new one,
and the old id is `404` from then on. **The old address is kept as an alias** of
the new one, so mail to it goes on arriving, and it appears in `emails` as `type`
`other`, never primary. The request that renames does not remove it. A **later**
`PUT` whose `emails` leaves it out, or a `PATCH` that replaces `emails` without it
or removes it, removes it - which is how an identity provider that sends the whole
of `emails` every time, as Okta does, ends the old address.

**Every alias of the account is part of `emails`**, including one an administrator
made in the Control Panel: an identity provider that replaces `emails` removes the
aliases it does not list. Where aliases are managed on this server rather than in
the directory, do not map `emails` beyond the work address.

### Removing one

**`DELETE /scim/v2/Users/{id}`** ends the account's sessions and drops its
connections, removes the account and its mailbox, and then what names its
address: every alias `emails` answered for it, and **its row in every
distribution list** - a member, a request still waiting or a departure recorded,
in any domain. `PersistentAccount`'s delete leaves both alone, since they name an
address and no account; an alias left behind would take mail for somebody who is
gone, and a membership left behind would be a member nobody is, whose copy of
every list message is refused, and whose memberships an account made later at the
same address would inherit. A list's membership query is a rule and names nobody,
so it is not changed. `204`. The account goes first, so a delete the database
refuses leaves everything as it was (`500`); if the account is removed and an
alias or a list row cannot be, the answer is `500` naming what is left, and HM7224
says the same in the error log. Nothing of it can be put back but from a backup.

**Entra sends the `DELETE` thirty days after a user is soft-deleted**, and sends
`active` `false` straight away: the suspension is the safe first step, and the
thirty days are the time to take anything that must be kept.

---

## Groups

`GET`, `POST /scim/v2/Groups` and `GET`, `PUT`, `PATCH`, `DELETE
/scim/v2/Groups/{id}` do for distribution lists what the user routes do for
accounts.

* **`POST`** takes `displayName` - the list's address, at a domain the key
  provisions - and `members`. The list is made public, active and not managed, as
  the REST list route makes one; the Control Panel and the Deck change the rest.
* **`GET /scim/v2/Groups`** filters on `displayName eq "..."` and nothing else.
* **`PUT`** takes `displayName` and `members`, the whole set when given; left out,
  they are left as they are.
* **`PATCH`** takes `add` and `replace` of `members`, `remove` of `members` with a
  value (those members - Entra's form - an address that is no member being nothing
  to do) or without (every member), `remove` of `members[value eq "..."]` (`400
  noTarget` for an address that is no member), and `replace` of `displayName`.
* A new `displayName` changes the list's address **inside its own domain only**;
  another domain is `400`, since moving a list moves who may post to it and whose
  policy it answers to. **The old address is not kept**: unlike an account's
  rename, nothing is left behind at it, so mail to it - and an alias whose value
  it is - stops reaching the list at once.
* **`DELETE`** removes the list, its members and its archive.

A member is an address - a user's id, or an address anywhere - and never the
list's own address or another distribution list's: a list inside a list, or
inside itself, is expanded again and again until the recursion limit and reaches
nobody the identity provider meant, so either is `400 invalidValue`; name the
inner list's members instead. A list an administrator nested before is left as it
is.

A key confined to named domains **adds and removes only addresses in them**: a
list is a way to send to its members, and a key for one tenant does not decide
whose mailboxes elsewhere a message reaches. It is judged on the members it names
and on no others. A member elsewhere that an administrator gave the list is not
the key's to add or to remove, so a `PUT` or a `replace` that leaves it out
leaves it where it is, a `remove` without a value takes out only the members in
the key's domains, and an operation that names it is `400`, naming it. The key
can go on changing the rest of the list.

---

## Errors

| Status | `scimType` | When |
|---|---|---|
| `400` | `invalidSyntax` | The body is not a JSON object, or a `PatchOp` has no operations or an operation no `op` |
| `400` | `invalidValue` | A value refused: an address at a domain the key does not provision, a password the policy or the breach check refuses, a name past sixty characters, the work address written as another, an attribute the schema does not have, an alias at a spam trap or past the domain's limit of aliases, a list as a member, or a `remove` of `active`, `userName` or `password` (or a `replace` of one with `null`) |
| `400` | `invalidPath` | A `PATCH` path that names nothing of the schema, or a form of it this server does not take |
| `400` | `noTarget` | A `remove` whose filter matches nothing |
| `400` | `invalidFilter` | A filter this server does not answer |
| `401` | | No API key: no credential, the administrator password, an account's password, a session |
| `403` | | The key's role does not hold `provision`, or the account holds authority the key does not; a session's write without `X-Requested-With` |
| `404` | | No resource with that id, one at a domain the key does not reach, or a path SCIM does not serve |
| `405` | | A method the resource does not take |
| `409` | `uniqueness` | The address is taken; without a `scimType`, the mailbox is in use |
| `412` | | `If-Match` names a version other than the current one |
| `429` | | The key's request budget is spent; `Retry-After` says how long |
| `500` | | A write failed part way - the detail says what stands: a rename that happened, or what could not be put back - or a `DELETE` removed the account and not everything naming it |
| `501` | | `/Bulk`, `/Me` and `POST .search`, which this server does not take |
| `503` | | The breach check could not be asked, and is set to refuse |

`sortBy` and `sortOrder` are not refused: they are ignored (*Reading, filtering
and paging*).

Five diagnostics in the error log name what a SCIM write left behind, and none
fires on a change that was refused before it wrote anything:

| Code | Means | What to do |
|---|---|---|
| HM7220 | An account `POST` made, whose alias was then refused, could not be removed again with the aliases the request made | Remove what is left of the account and its aliases by hand |
| HM7221 | A change renamed the account, and a later write of the same change failed; the rest was put back and the rename stands | Nothing, unless the change is still wanted: send it again against the new id |
| HM7222 | A list `POST` made, whose member was then refused, could not be removed again | Remove the list by hand |
| HM7223 | A change failed part way and what it had written could not all be put back - the message names what (an alias made or removed, the account's own columns) | Look at the account and its aliases, and put right what the message names |
| HM7224 | A `DELETE` removed the account, and an alias or a distribution-list row naming its address could not be removed | Remove what the message names by hand |

---

## Setting up Microsoft Entra ID

1. Issue the key (above) and note the token.
2. In the Entra admin centre, **Enterprise applications** > **New application** >
   **Create your own application** > *Integrate any other application you don't
   find in the gallery (Non-gallery)*.
3. **Provisioning** > **Automatic**. **Tenant URL**: `https://<host>:<port>/scim/v2`,
   where the port is `RestApiPort`. **Secret token**: the `hmapi_...` token.
   **Test connection** asks for a user that does not exist and expects an empty
   list, which is what a filter this server answers gives it.
4. **Mappings** > *Provision Microsoft Entra ID Users*:
   * `userName` from `userPrincipalName` where the principal name is the mailbox
     address, or from `mail` where it is not. It must be an address at a domain
     this server hosts.
   * `emails[type eq "work"].value` must be the same address as `userName`: map it
     from the same attribute, or delete the mapping.
   * `active` (Entra's `Switch([IsSoftDeleted], ...)`), `name.givenName`,
     `name.familyName`, `displayName` and `externalId` (`objectId`, or the default
     `mailNickname`) are kept.
   * **Delete the mappings this server does not keep**, so that nobody reads the
     mapping list as what the mailboxes hold: `title`, `preferredLanguage`,
     `addresses[...]`, `phoneNumbers[...]`, `name.formatted` (composed here from the
     two names), and every
     `urn:ietf:params:scim:schemas:extension:enterprise:2.0:User:` attribute -
     `employeeNumber`, `department`, `manager` and the rest. Left in place they are
     accepted and not kept, and cost nothing but that misunderstanding.
5. **Mappings** > *Provision Microsoft Entra ID Groups*, for mail-enabled groups
   that should be distribution lists: map `displayName` from `mail` - a group's
   id here is its address, not its name - keep `members`, and delete
   `externalId`'s mapping, which is accepted and not kept.
6. Assign the users and groups, and switch **Provisioning Status** on.

Entra matches a user by `userName eq` (and a group by `displayName eq`) before it
makes one, updates with `PATCH` - path-less replaces, capitalised operations,
`emails[type eq "work"].value` - all of which are taken, sends `active` `false`
when somebody is disabled or soft-deleted, and `DELETE` thirty days later. The
`aadOptscim062020` flag some tenant URLs carry is a query parameter this server
ignores.

## Setting up Okta

1. Issue the key (above).
2. **Applications** > **Create App Integration** > *SAML 2.0* or *SWA*, then on
   **General** enable **SCIM provisioning**.
3. **Provisioning** > **Integration**: **SCIM connector base URL**
   `https://<host>:<port>/scim/v2`; **Unique identifier field for users**
   `userName`; **Supported provisioning actions**: *Import New Users and Profile
   Updates*, *Push New Users*, *Push Profile Updates*, *Push Groups*;
   **Authentication Mode** *HTTP Header*, and the token in **Authorization**.
4. **Provisioning** > **To App**: enable *Create Users*, *Update User Attributes*
   and *Deactivate Users*.
5. **Push Groups** sends a group under its Okta name as `displayName`, and a
   group's `displayName` here is the distribution list's address: a push of
   `Engineering` is `400 invalidValue`. Name each pushed Okta group with the list's
   address (`engineering@example.com`), or leave *Push Groups* off.

Okta sends `PUT` with the whole user for a profile update, `emails` with the
primary address alone - so an alias made on this server is removed by Okta's next
update (see *Renaming* above) - and `locale` and `groups: []`, which are accepted
and not kept. It deactivates with `active` `false`. Its import pages through
`GET /scim/v2/Users` with `startIndex` and `count`.

**Google Workspace**, at the time of writing, provisions automatically only the
applications in its own catalogue, and a custom application cannot be pointed at a
SCIM endpoint; so there are no Google steps here to give, and nothing in this
server is specific to any one provider.

---

## The audit trail

Every SCIM write reaches the audit trail ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md))
with nothing written for it here: `hm_accounts`, `hm_aliases`,
`hm_distributionlists` and `hm_distributionlistsrecipients` are audited tables, and
the REST listener installs the key as the actor of every request it authorises. So
each row names **`key:<id>`**, with the type `apikey` and the interface `REST`,
and `GET /api/v1/apikeys` (the administrator password's) maps the id to the key's
label. A rename is the mailbox move's entry, `moved`, naming both addresses and
the old address kept as an alias, followed by the account's `updated` and a row
for each alias the same change removed or made. What a change that failed part way
put back is on the trail too, under the same key. The application log carries a
line per change: `REST API: SCIM key:<id> made account ...`, `... renamed ...`,
`... suspended ...`, `... removed account ...`.

---

## What this does not do

* **No bulk, no sorting**, and no `POST .search`. ServiceProviderConfig says bulk
  and sort are not supported. `/Bulk` and `POST .search` answer `501`; `sortBy` and
  `sortOrder` are ignored, and a listing comes back in the order the resources were
  made.
* **No enterprise user extension**, and no attribute of the User schema beyond the
  table above: a manager, a department, a telephone number or an address is
  accepted and not kept.
* **No schema discovery of custom attributes.** The schemas list what this server
  keeps, and that list is fixed.
* **No groups but distribution lists.** The sharing groups (`hm_groups`,
  `/api/v1/groups`) are folder-permission principals an administrator keeps for
  IMAP sharing - who may open whose folders - not identity groups; an identity
  provider's security group is not a sharing decision this server should take
  from it. A group here is a list mail is sent to.
* **Not a SCIM client.** This server never calls the identity provider; what
  changes on this server is not sent back.
* **No `/Me`**, and no `created` or `lastModified` (see *The mapping*).
* **No group `externalId`**: `hm_distributionlists` has no column for it, so it is
  accepted and not kept, and a group cannot be filtered on it.
* **Filters are the six above**, one comparison each; no `and`, `or`, `not`,
  `pr`, `gt` or `lt`.
* **A list's address changes only inside its domain**, and a list's membership
  query, its moderation and its list-manager fields are the Control Panel's and
  the Deck's.
* **Nothing but SCIM and COM shows `externalId`**: not the Control Panel, not the
  Deck, not the REST account routes (`GET /api/v1/accounts/{address}` carries no
  `external_id`), not `hmctl` or the PowerShell module. COM's `Account.ExternalId`
  reads and writes it, and a backup carries it.
* **A group's rename keeps nothing at the old address** (*Groups*), where an
  account's keeps an alias.

---

## Where it lives

| | |
|---|---|
| The routes, the mapping, the filters and the PATCH paths | `Common/Util/RestApiScim.cpp`, members of `RestApiServer` |
| The route kinds, the key-only rule, `provision` for every SCIM route, and the SCIM-shaped `401`, `403`, `429` and `500` | `Common/Util/RestApiServer.cpp` (`ParseRoute_`, `Authorize_`, `PermissionFor_`, `IsMutatingRoute_`, `ProcessRequest_`) |
| The permission | `RolePermission::Provision`, token `provision`, in `Common/Application/RoleManager.{h,cpp}` |
| Which accounts a key may change | `RoleManager::Encompasses`, the rule the REST account routes and COM apply too |
| `externalId` | `hm_accounts.accountexternalid`, schema 6096 (`Upgrade6095to6096*.sql`); `Account`, `PersistentAccount`, `Account.XMLStore`/`XMLLoad`, COM `Account.ExternalId` |
| The regression tests | `RegressionTests/API/Scim.cs` |
