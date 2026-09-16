Renaming a mailbox, and moving one to another domain
====================================================

Somebody marries, or a department is renamed, or a company moves from
`example.co.uk` to `example.com` and the mailboxes have to go with it. Until
16 September 2026 the answer here was to make a second account and copy the mail
into it, and that answer was wrong in a way that is easy to miss: the copy is not
the account. It has a new id, and everything the account *is* hangs off that id —
its rules, its folders and their subscriptions and access-control entries, its
application passwords, its second factor, its contacts, its calendars, its
preferences, its known senders, its password history, its safe-sender and
blocked-sender lists, its group memberships and its administrative role. A copy
loses all of it, silently, and the person notices a week later when a filter
stops firing.

A move keeps the row. Everything above follows without being touched, because
none of it names the address.

What this is, in one sentence
-----------------------------

`new_address` carries the domain, so **renaming and moving are the same
operation**: `ann@example.com` to `anne@example.com` is a rename,
`ann@example.com` to `ann@example.net` is a move, and the server does not
distinguish them.

How to do it
------------

**Control Panel.** Open the account, and on the General tab press **Move or
rename…**. The dialog says whether anything is holding the mailbox before it
offers you the button. The account dialog closes when the move succeeds, because
everything on its tabs was read for a mailbox that is now somewhere else.

**Control Deck.** Open the account editor; the **Move or rename** card is below
the account's fields. A mailbox something is holding says so and the button is
disabled. After the move the page goes to the accounts of the domain the mailbox
is in now.

**A shell.**

```
hmctl mailbox status ann@example.com
hmctl mailbox move ann@example.com ann@example.net
hmctl mailbox move ann@example.com anne@example.com --drop-alias
```

**PowerShell.**

```powershell
Get-HmMailboxMove -Address ann@example.com
Move-HmMailbox -Address ann@example.com -NewAddress ann@example.net
```

`Move-HmMailbox` asks before it moves anything (`ConfirmImpact` is High) and
`-WhatIf` sends nothing at all.

**REST.**

```
GET  /api/v1/accounts/ann%40example.com/move
POST /api/v1/accounts/ann%40example.com/move
     {"new_address": "ann@example.net", "keep_alias": true}
```

The address may be written as it is (`ann@example.com`) or percent-encoded
(`ann%40example.com`): the listener decodes a path segment before it routes, so
both name the same account. A separator is the one thing never decoded, so
`%2F` stays `%2F` and a path segment can never become two.

**COM.** `Account.MoveTo(NewAddress, KeepOldAddressAsAlias)`, or
`Utilities.MoveMailbox(Address, NewAddress, KeepOldAddressAsAlias)` when you have
an address rather than an account object. There is no `Save()` for either:
the mailbox has moved by the time the call returns.

The old address
---------------

**It is kept as an alias by default**, pointing at the new address, so mail to it
goes on arriving in the same mailbox. That is the default because it is what
keeps mail flowing: a correspondent's address book, a mailing list somebody
signed up to, a printer configured three years ago and a password-reset form all
hold the old address, and none of them will be told.

Turn it off (`keep_alias` false, `--drop-alias`, the unchecked box) only when the
address is to stop working: mail to it is then refused at `RCPT TO`, which is the
honest answer, and the sender finds out.

An alias is an ordinary alias once it exists. Delete it from the domain's aliases
when the address has been dead long enough.

What moves
----------

| | |
|---|---|
| **The message store** | The directory `<data>/<domain>/<local part>` and everything under it. `hm_messages` holds only the GUID file name; the full path is rebuilt from the **account address** at read time, so the directory and the address have to change together or every message in the mailbox becomes unreadable. |
| **The Sieve tree** | The account's filter scripts and its vacation state, which live outside the mailbox directory and were the half a rename used to leave behind. |
| **Everything keyed on the account id** | Rules, IMAP folders with their subscriptions, special-use designations and access-control entries, application passwords, the second factor, contacts, calendars, preferences, known senders, password history, safe and blocked senders, group memberships, quarantine-digest state and the account's role assignment. None of these are touched; they follow the row. |
| **Aliases that point at the account** | An alias whose *value* is the old address is re-pointed at the new one. Aliases in **any** domain, not only the account's own — which is exactly the case a cross-domain move creates. |
| **Distribution list memberships** | A list with the old address as a member now has the new one. |
| **Mail in flight** | A recipient of a message still in the delivery queue that was already resolved to this account is rewritten, so the message is filed into the moved mailbox rather than into a directory that is no longer there. What the sender wrote (`recipientoriginaladdress`) is left exactly as it was: it is evidence, and a bounce quotes it. |
| **Quarantined mail** | A held message whose recipient list names the old address is rewritten, so the account still reaches its own held mail. The recipients are text inside one column and SQL Server Compact refuses a long parameter compared with a column, so the rows are read and matched here rather than searched — up to a ceiling of **200,000 rows**. A quarantine bigger than that keeps the old address in the rows beyond it, and those messages are then listed for the old address rather than the new one. |

What does not move, on purpose
------------------------------

**The archive** (`ArchiveDir` and `hm_archiveindex`) and **the message trace**
record what happened at an address at a time. They are a record of the past, and
rewriting them would make this server's account of it disagree with every other
party's copy of the same messages. They stay under the old address:
`Utilities.SearchArchive` and `GET /api/v1/message-trace/address/<old>` go on
finding them there.

One consequence worth knowing: `Utilities.EraseAddressTraces` and the erasure
routes work by address, so erasing the *new* address will not reach the archive
written under the old one. Erase both.

**The role's reach.** An account's role assignment moves with it, but the
*domains* the role reaches are the role's own list and are not rewritten. A
domain administrator moved into another domain still administers the domains
their assignment names. Check it after a cross-domain move.

**A message being delivered while the move runs.** Delivery builds its path from
the account address, and the account row carries the old one until the store has
been copied — so a message that arrives during the copy is written under the old
directory. If the copy has not reached that part of the tree yet it is carried
across; if it has, it goes with the old tree. The window after the copy *is*
closed: the caches are dropped the moment the row carries the new address and the
old directory is then swept once for anything that landed in it (anything that
could not be carried is `HM6948`). Nothing in the server records a delivery in
progress, so there is nothing to wait on — which is why a move of a large busy
mailbox belongs out of hours.

What is refused, and nothing is changed when it is
--------------------------------------------------

Every one of these is decided **before** the first byte is moved.

| Refusal | Why |
|---|---|
| An address that is not an address, or is over 255 characters | The account's own validation, asked of the address it is moving to. |
| A domain this server does not hold | A mailbox in a domain the server does not host would receive nothing. |
| A domain that is not active | The same, less obviously. Asked only of a move *into another* domain: renaming a mailbox inside a deactivated domain cannot make it any less reachable than it already is, and has always been allowed. |
| The domain being moved into is at its maximum number of accounts | The account's own save asks that of new accounts only, by an id of zero, and an account arriving from another domain has an id — so moving mailboxes in could otherwise take a domain past a limit that creating one there could not. |
| The domain being moved into would exceed its maximum total size | The same reason. Both are asked only where the move crosses domains. |
| An account, an alias or a distribution list already at the new address | Each said by name, so you know which of the three to go and look at. |
| The address the account is already at | Nothing to do. A change of *capitalisation* alone is allowed, and moves no directory. |
| The mailbox is in use | See below. |
| Message file names are still full paths | An installation that predates hMailServer 5.4's partial paths cannot have a mailbox renamed at all: the paths would go on naming the directory this is about to empty. Run the Data Directory Synchronizer first. |
| The credential may not administer accounts in **both** domains | The one the mailbox is leaving and the one it is going to. Over REST the second is in the request body, where no generic authorisation can see it, so the handler asks. |

### In use

A move is refused while something is holding the mailbox, because a session that
is already open holds the account **as it was** and would go on building message
paths from the old address. Two things can be asked of the running server, and
both are:

* **a POP3 session**, which holds the mailbox lock from authentication until its
  socket closes;
* **an IMAP session with one of the account's folders selected**, which is what
  a `SELECT` registers with the notification server.

`Account.InUseReason`, `GET …/move` and `hmctl mailbox status` all answer the
same sentence, so a page can grey the button out and a script can wait.

What is **not** counted, and is worth knowing: an IMAP session that has
authenticated and selected nothing, a webmail request in flight, a ManageSieve
session, and — the one that can cost a message rather than only an error — an
**SMTP delivery to this mailbox in flight**. Nothing records any of them. In
practice a move is done out of hours or after telling the person to close their
client; the checks above catch the case that actually bites, which is a client
left open on a desk, and the paragraph above on a message being delivered says
what the delivery case costs.

The POP3 lock is released when the session's socket closes, which is **after**
the client has read the reply to `QUIT` — so a script that reads a mailbox and
then moves it should wait on `InUseReason` rather than assume.

Either it completes, or the account is as it was
------------------------------------------------

There is no transaction across a filesystem and four database backends, so the
order is chosen to make each step undoable by the next one failing:

1. **The store.** The mailbox directory and then the Sieve tree, each copied and
   then deleted, so a failure leaves the original where it was. If the Sieve tree
   cannot be moved, the mailbox is put back. A copy that fails part way through
   raises rather than returns — one unreadable or locked file is enough — and the
   half-copied tree at the destination is removed before the refusal is answered,
   so the move can be asked for again.
2. **The account row.** One `UPDATE` of the address and the domain id. If it
   fails, the store is moved back. The caches are dropped as soon as it has been
   written, and the old directory swept once for late arrivals (see *A message
   being delivered while the move runs*, above).
3. **The references.** One statement each for the aliases, the list memberships
   and the queued recipients, and one per rewritten quarantine entry. A failure
   here is reported (`HM6942` to `HM6945`) and the move stands: an alias that was
   not re-pointed still names an address the left-behind alias answers, and
   putting a completed move back would be the larger change, not the smaller one.
   A rewrite that failed writes nothing and therefore *counts* nothing, so the
   summary's `references_incomplete` is what tells a zero that means "nothing to
   do" from a zero that means "it could not be done"; the sentence in the journal
   and on the audit trail says so too.
4. **The alias.** If it cannot be created, the move stands and `HM6941` says that
   mail to the old address will be refused until an alias is added by hand.

The one thing this cannot survive is the process being killed part way through,
and that is what the journal is for.

### The journal, and what an interrupted move leaves behind

`hm_mailboxmoves` (schema 6050) carries one row per move, opened before the first
step and closed after the last. A row at a stage short of `complete` is a move
that was interrupted. The server says so once per start, as `HM6947` in the
application log, naming both addresses and the stage.

Each stage is written **before** the step it names wherever that step is not a
single statement, so a row never claims less than has happened. The store move is
a copy and a delete that takes as long as the mailbox is big, and it has a stage
of its own for exactly that reason; the three database steps are one statement
each, so a row that stops at one of them has had either all or none of the next
one applied, and the instruction below covers both.

| Stage | What is on disk and in the database |
|---|---|
| `started` | Nothing has been changed. The mailbox is at the old address and works. |
| `moving the store` | The copy was under way. The tree may be wholly at the old address, partly at both, or wholly at the new one with the old gone — look at both directories. The database has not been touched, so the account still answers at the old address, and whether its mail is readable depends on which of the three it is. |
| `store moved` | The mailbox directory and the Sieve tree are at the **new** address. The account row either still says the **old** one or has just been written with the new one — look at the account's address. While it says the old one the account authenticates and its mail is unreadable. Move the two directories back by hand, or finish the move (see below). |
| `account written` | The store and the account row agree; the aliases, list memberships, queued recipients and quarantine entries have not all been rewritten, and the old address is not an alias. Mail already queued for the old address may bounce, and an alias that pointed at the account names an address that no longer exists. |
| `references rewritten` | Everything but the alias. Add the old address as an alias by hand if it was meant to be kept. |
| `complete` | The move finished. |
| `rolled back` | A step failed and everything before it was put back. The account is as it was. |
| `rollback failed` | A step failed and something could not be put back. The row's detail says what, and `HM6940` is in the application log. |

To finish an interrupted move by hand from `moving the store` or `store moved`,
ask for the move again — `Account.MoveTo`, `POST /api/v1/accounts/<the address
the account still has>/move`, `hmctl mailbox move`, or the button in either
administration tool — naming the address the directories now have. The store step
finds a source that has gone and a destination that is there, treats itself as
already done, and the rest of the move runs.

Setting `Account.Address` and saving finishes a **same-domain** rename the same
way, because that path calls this one. It cannot finish a move that crossed
domains: an account's address is confined to the domain it is listed under, and
the save refuses one that is not — which is why the move is an operation of its
own.

Read the journal with `Account.LastMove`, `Utilities.GetMailboxMoves`,
`GET /api/v1/accounts/<address>/move` or `hmctl mailbox status`.
`Utilities.GetMailboxMoves("")` is every mailbox on the server, and is the
server administrator's.

The journal is **not** in the backup. A restore is not a moment in a move: the
account comes back at the address the archive holds, so a carried row would
describe a move this store has no trace of. The audit trail is the record a
person reads.

The audit trail
---------------

A move is **one entry**: object type `account`, action `moved`, naming the
address it left, the address it took, what it carried and whether the old address
was kept. The rows a move writes — the account, the alias, the list memberships —
are written with no actor on the thread, precisely so that one act is one entry
rather than four.

A move that failed and could **not** be put back is recorded as
`account / move failed`, with what was left where. A move that failed and was put
back is not recorded, because nothing changed.

Who may do it
-------------

`manage_accounts`, in **both** domains: the one the mailbox is leaving and the
one it is going to. The server administrator and a domain administrator of both
domains have it; the help-desk role does not, and neither does the auditor. A
credential may also not move an account that holds administrative authority the
credential does not itself hold — the same rule that stops a help desk resetting
a domain administrator's password.

`view_accounts` is enough to read `GET …/move` and the journal, so an auditor can
see that a mailbox moved without being able to move one.

Setting the address directly
----------------------------

Changing `Account.Address` and saving still works, and since 16 September 2026 it
goes through this same implementation — so it carries the store, the Sieve tree,
the aliases, the list memberships, the queued recipients and the quarantine, and
it journals what it did. Two things differ, and both are compatibility rather
than design:

* it leaves **no alias**, because that setter never has; and
* it does **not** refuse while the mailbox is in use, because a script that reads
  a mailbox and then renames it would fail on a race it cannot see.

Use the move operation when you want either of those. The address field is also
still confined to the account's own domain, because an account's domain is the
domain it is listed under; a cross-domain move has to be the operation.

A domain rename (`Domain.Name` and `Save()`) is a different thing again: it moves
the whole domain's tree at once and walks its accounts to write the new domain
into each address, and it keeps the path it has always taken.

Error codes
-----------

| | |
|---|---|
| `HM6940` | A step failed and something could not be put back. The message says what is where. |
| `HM6941` | The move succeeded but the alias for the old address could not be created. |
| `HM6942` | Aliases pointing at the account could not be re-pointed. |
| `HM6943` | Distribution list memberships could not be moved. |
| `HM6944` | Messages in the delivery queue still name the old address. |
| `HM6945` | Quarantined messages still name the old address. |
| `HM6946` | The move could not be journalled. It still happened, and is on the audit trail. |
| `HM6947` | A move was interrupted; said once per start. |
| `HM6948` | Mail delivered to the old address while the move was running could not be carried into the new directory. The message names both directories. |
