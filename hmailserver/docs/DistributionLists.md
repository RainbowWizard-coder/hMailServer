Distribution lists that define themselves
=========================================

A distribution list has always been a list of addresses somebody keeps up to
date. Since schema 6074 it can instead — or as well — carry a **membership
query**: one short text on the list that says *who* its members are, answered
against the server's accounts at the moment a message is sent to it. Exchange
calls the shape a dynamic distribution group.

The point is the mailing list nobody maintains. `everyone@example.com` with the
membership `domain` reaches every active account of `example.com` on the day the
message is sent, including the person who joined this morning and excluding the
one whose account was switched off last night, without anybody having opened the
Control Panel.

A list people join and leave themselves - by mail and by page, with a copy of
their own that carries their way out, an archive, a daily digest and bounce
handling - is a distribution list with the **list manager** switched on, and is
described in [MailingLists.md](MailingLists.md). A managed list keeps its
membership query: the accounts it answers are sent each message. They have no
row of their own, so they choose no delivery; they leave by the link in their
copies, or by the list's mailto address, which answers with a link to confirm,
and a row is then kept saying the address has left - which the query's answer
does not get past.

The five forms
--------------

The query is the `Membership` property of a list (`Membership` over COM,
`membership` over REST, *Membership query* in the Control Panel and the Control
Deck). It is one of:

| Text | Members |
|---|---|
| *(empty)* | **A static list**: the addresses under Recipients and nothing else. This is every list made before this feature existed, and it is still what a new list starts as. |
| `domain` | Every **active** account of the list's own domain. |
| `domain:other.example` | Every active account of that domain, which this server has to host. |
| `pattern:sales-*@example.com` | Every active account whose address matches the wildcard. `*` is any run of characters, `?` is exactly one, and case is ignored. |
| `everyone` | Every active account of every active domain on the server. |

The text is stored lower-cased and trimmed, in the one spelling this table
gives. `Domain: Example.com ` is saved as `domain:example.com`.

The rows stay
-------------

A query is a **union** with the list's own recipients, never a replacement for
them. `everyone@example.com` can be `domain` and still carry the one contractor
at another company under Recipients. An address that is both a recipient and an
answer to the query is delivered to **once**: the expansion deduplicates by
address, as it always did for two recipients that named the same mailbox.

The rows come first, in the order they are held, and the query's answers follow
in address order. That is the order the members are expanded in, and the order
`GET /api/v1/lists/{address}/members` lists them in.

Everything else about the list is unchanged
-------------------------------------------

A query list is a distribution list. It honours:

* **Who may send to it** — public, membership, announcement, or anyone in the
  domain. Under *membership* mode, a sender the query would answer is a member
  and may post: asking that question of one address does not resolve the whole
  query, it asks the account cache whether that account is one the query
  reaches.
* **Require SMTP authentication** and **Require sender address**, unchanged.
* **Moderation** — a moderator address still takes a posting the mode would have
  refused, and the moderator's authenticated resend still reaches the members,
  which now means the query's answers as well as the rows.
* **The bounce address**, which is stamped on every copy either way.

What it does *not* do
---------------------

* **There is no directory-attribute form**, and that is a decision rather than
  an absence. The row this was built for asked for "everyone with a given
  directory attribute". `hm_accounts` carries no department and no title — an
  account's first and last name are the whole of what it says about the person —
  so `title:` and `department:` would need a column, an editor and a migration
  before they could mean anything, and those are not in this row. But the server
  *does* group accounts: `hm_groups` and `hm_group_members` are real, and groups
  are administered over COM, in the Control Panel, in the Deck and over REST. A
  `group:<name>` form would be one more branch in the parser and one more
  `select` joining those two tables to `hm_accounts`. It is deliberately not
  built here: a group is an **ACL principal for shared folders**, not a
  statement about the person, and it is server-wide and belongs to no domain, so
  like `everyone` it would be a form only the server administrator could set.
  Making a group a mail concept as well is a design decision worth taking on its
  own; the parser is where the form goes on the day it is taken.
* **Nothing is stored, and nothing outlives the message.** There is no
  membership table that could drift out of date — that is the whole point — and
  the cost is that the query is run against the database for the message that is
  being sent, and again each time the members route is called. For a domain of a
  few thousand accounts that is one indexed `select`; it is not free, and a
  server sending ten thousand messages an hour to a query list will do ten
  thousand of them.

  It is resolved **once per SMTP transaction**, not once per `RCPT TO`. The
  recipient parser remembers what each list answered for the message it is
  building and forgets it with that message, so repeating `RCPT TO` for the same
  list on one connection — which an unauthenticated sender may do to a public
  list as often as it likes — does not buy a scan of the accounts per command.
  For the same reason a `pattern:` query compiles its expression once for the
  whole scan rather than once per mailbox, and the expansion's
  "already a recipient?" test is an index rather than a walk of everything added
  so far. None of that changes an answer: it is the same query, asked once.
* **It answers accounts, and only accounts.** Not aliases, not other
  distribution lists, not external addresses. Those are what the rows are for.
* **A disabled account is never a member**, and neither is any account of a
  disabled domain. There is no form that includes one.
* **It is not a group.** Account groups (`Settings.Groups`) are an ACL
  principal for shared folders; this is mail delivery. The two do not read each
  other.
* **No nesting of queries.** A query answers addresses, and each address then
  goes through the same expansion a recipient row does — so an answer that is
  itself a list would expand, but no query can name another list's query.

When the query cannot be answered
---------------------------------

A query is judged when it is **saved**: `domain:` that names a domain this
server does not host, a `pattern:` with no `@`, or any text that is none of the
five forms, is refused by the same sentence over COM, over REST and in the
Control Panel, and never reaches the column.

A query that was valid and has stopped being so — the named domain deleted or
switched off since — is *not* an error at delivery. The message is delivered to
the list's rows, the reason is written to the error log with code 6980, and the
members route answers it in a `problem` field. A query that answers nobody is a
list that is smaller than it was, not a refused message.

**"Answers nobody" and "could not be answered" are not the same thing**, and
the server never treats them as one. If the accounts cannot be *read* — the
database locked by a backup, a dropped connection, the domain the query names
unreadable — then who the list delivers to is unknown, and the union with the
rows would hide that perfectly: one row expands, `RCPT TO` is answered `250`,
and a message meant for a thousand people reaches one of them with nothing
logged and nothing ever retried, because the sender was told it arrived. So:

* at delivery the recipient is **refused with `451 4.3.2`** and the sender
  retries. Nothing is expanded, and code 6982 says why in the error log;
* `GET /api/v1/lists/{address}/members` answers **503** with the reason, rather
  than a short list that would read as the membership;
* a *save* of `domain:` or `pattern:` whose domain could not be read says
  "could not be read", not "this server does not host it" — the second sends an
  administrator looking for a configuration problem that is not there.

Renaming a domain follows the query: when `old.example` becomes `new.example`,
every list whose membership is `domain:old.example`, or a pattern ending
`@old.example`, is rewritten with the new name and the change is logged. A list
whose query this server cannot read is left alone and named in the log (code
6981).

Who may set which form
----------------------

A credential confined to named domains — a domain administrator over COM, an
API key with a domain scope over REST — may set `domain`, a `pattern:` in a
domain it holds, or nothing. `everyone`, `domain:` naming another tenant's
domain, and a pattern whose domain part is itself a wildcard all reach accounts
that credential does not administer, and are refused. The server administrator
may set any of them. "The domains it holds" is the whole of its grant, not just
the list's own domain: an administrator whose grant names two domains may set
`domain:` naming either, over COM exactly as over REST.

**The same rule governs reading the answer.** A form a credential may not set
is a form it may not read the answer to, because the disclosure is identical: a
list in a tenant's own domain whose membership the *server* administrator set to
`everyone` would otherwise hand that tenant every active address on the server,
and the Control Deck fetches it the moment the list is opened.
`GET /api/v1/lists/{address}/members` therefore answers **403** to a confined
credential when the list's query reaches past its domains. When it does answer,
every address in the answer is in a domain that credential holds, by the same
rule that allowed the query.

The surfaces
------------

* **Control Panel** — *Membership query* on the distribution list's properties,
  under the bounce address, with the five forms spelled out in its note.
  Recipients are still edited separately.
* **Control Deck** — the query is a column of the lists table (a static list
  says *Its addresses alone* rather than showing an empty cell) and a field of
  both the create and the edit forms. Opening a list also shows **Delivers to
  now**: every address a message would reach at this moment, each marked *Kept
  by hand* or *From the query*.
* **REST** — `membership` is a field of the list document, of
  `POST /api/v1/domains/{domain}/lists` and of `PUT /api/v1/lists/{address}`;
  an empty string on the update turns a query list back into a static one and
  leaves its rows alone. `GET /api/v1/lists/{address}/members` answers
  `address`, `membership`, `membership_kind` (`static`, `domain`,
  `named_domain`, `pattern`, `everyone`, or `unreadable` for a text this build
  cannot parse — a restore writes the column without judging it, and calling
  that `static` would contradict the `membership` beside it), `count`, `members`
  — each with `address` and `source` of `row` or `query` — and `problem` when
  the query could not be answered.
* **COM** — `DistributionList.Membership`, a string property, read and written
  like the moderator and bounce addresses.
* **hmctl** — `hmctl list list --domain example.com` shows the query beside each
  list; `hmctl list update all@example.com --set membership=domain` sets it; and
  `hmctl listmember list --address all@example.com` is the members route.

Where it is in the code
-----------------------

`Common/Util/DistributionListMembership.{h,cpp}` is the whole of it: the parse,
the save-time validation, the resolution against `hm_accounts`, the union with
the rows, the single-address membership test the sender check uses, and the
rewrite a domain rename needs. `SMTP/RecipientParser.cpp` calls `Members()`
where it used to walk the recipient rows, and `IsMember()` where it used to stop
at the end of them; it is also where the per-transaction memo lives, keyed on
the `MessageRecipients` the transaction is filling, so that the memo cannot
outlive the message by construction rather than by remembering to clear it.
`Persistence/PreSaveLimitationsCheck.cpp` is where a bad query is refused, so
every writer is refused it in the same words.

The column is `hm_distributionlists.distributionlistmembership`, 200 characters,
empty by default, added by schema 6074. A backup taken before 6074 restores as a
tree of static lists, which is what those lists were.
