A sample domain that can be thrown away
=======================================

A new hMailServer has nothing in it. Every page of the Control Panel, every view
of the Control Deck and every screen of the webmail is an empty table with a
sentence explaining what would be there, which is exactly the wrong thing to hand
somebody who is trying to find out what the product does. The regression suite
builds a domain with mailboxes and mail in it several hundred times a day for its
own purposes; until now nothing offered the same thing to a person.

One command makes it, and one takes it away and says what it removed.

    hmailserver --sample-domain create        # on Linux, beside the server
    hmctl sample-domain create                # anywhere, over the REST API
    POST /api/v1/setup/sample-domain          # the route both of those use

    hmailserver --sample-domain remove
    hmctl sample-domain remove
    DELETE /api/v1/setup/sample-domain

There is a button as well: on the Control Panel's Welcome page, and on the
Control Deck's Checklist view, under the twelve first-hour items. Both show what
is in the sample once it is there, and both turn into a Remove button.

What it makes
-------------

| | |
|---|---|
| the domain | `example.test`, active, with every default a domain made in the Control Panel gets: the plus-addressing character, the signature settings, no limits, no domain alias. Its postmaster is `alice@example.test`. |
| three mailboxes | `alice@example.test` (Alice Okafor), `bob@example.test` (Bob Ferreira) and `carol@example.test` (Carol Ridley), all ordinary accounts with no administrative level. |
| one password | one for all three, minted by the server when the sample is made and shown once in the answer that makes it. It is not printed here and it is not stored anywhere - see **The password**, below. |
| one distribution list | `everyone@example.test`, public, with the three mailboxes as its members. |
| one rule | on Alice's account, named *File the invoices*: a subject containing `Invoice` is moved to a folder called `Invoices`. The folder is not created by the sample - the rule creates it the first time it fires, which is how you can tell the rule really ran. |
| eight messages | between the three mailboxes and to the list, with real subjects and bodies: a welcome and its reply, two notices to the list, an invoice (which the rule files), a message with a CSV attachment, and a two-message thread about a backup report. The list expands, so fourteen copies are delivered. |

The messages are not written into the mailboxes. Each one is composed as an RFC
5322 message, written to the delivery queue and handed to the server's own
deliverer - the same path a message accepted over SMTP takes once DATA ends. So
everything that happens to real mail happens to these: the distribution list
expands, the rule files the invoice, quota is counted, the message index is
written and every IMAP and webmail session is told. They are in the mailboxes a
few seconds after the call answers.

The password
------------

All three mailboxes sign in with one password. The server mints it when it makes
the sample - twenty characters of Crockford base32 from the same generator an app
password comes out of, grouped in fives so it can be read off a screen - and says
it **once**, in the answer to the call that made the sample:

    example.test was created with 3 mailboxes, the list everyone@example.test and
    8 messages on their way into it. All of the mailboxes sign in with the
    password 7TQ4M-KJ9XD-2VWHF-BNRE5, which is shown here once and is not stored
    anywhere.

That sentence is what `hmctl sample-domain create` prints, what
`hmailserver --sample-domain create` prints, what the Control Panel shows in the
box that reports the creation, and what the route answers in `message` beside the
`password` field. The Control Deck shows it on the card until the tab is
reloaded.

Nothing keeps the clear text. The accounts carry the hash, as every account does,
so `GET /api/v1/setup/sample-domain` cannot answer the password and neither can
the Control Panel or `hmctl sample-domain status`: they say it was shown once.
**Write it down.** If you lose it, remove the sample and make it again - that is
a second's work and gives a new one.

An earlier version of this feature used a fixed password printed in this file.
That was wrong, and the reason is worth keeping written down: this server's
default address range lets an *authenticated* session relay local-to-remote from
anywhere on the Internet, so a published password made three known-credential
mailboxes - three open relays - out of every installation that had ever tried the
sample and not removed it. A random password costs the sample nothing: it is
still one call and still asks no questions.

Two things follow, and neither is softened here:

* **The sample bypasses the password policy and the breach check.** It is the
  only place in this server that sets an account password without asking either
  of them. Twenty random base32 characters are ninety-eight bits; a policy that
  insisted on a digit would refuse some of them for no reason, and a breach check
  has nothing to find in a value that has never existed before.
* **They are still ordinary mailboxes.** They can sign in from anywhere the
  server's address ranges allow, and relay through it. Remove the sample before
  this server takes real mail.

The marker, and the domain it will not touch
--------------------------------------------

When the sample makes `example.test` it records that domain's database id in the
setting `sample_domain_id`. The removal removes the domain that carries exactly
that id *and* is still named `example.test`, and nothing else.

That is the whole point of the marker. `example.test` is a name RFC 2606 reserves
for exactly this kind of use, so it is a name somebody else may well have used
first - the regression suite's own domain is `example.test`. So:

* a domain named `example.test` that this server did not make is **never created
  over** (`POST` answers 409) and **never removed** (`DELETE` answers 409). The
  state is reported as `foreign`, and both buttons disappear;
* a marker whose domain has already been deleted by hand is cleared, and the
  removal says so rather than reporting that it removed something. A domain table
  that could not be **read** is a different answer: the state is `unknown`, both
  calls refuse with 500 and nothing is changed, because clearing the marker on a
  read that failed would orphan a sample that is still on the server - it would
  read as `foreign` for ever afterwards, refuse both calls, and be counted by the
  checklist as your first domain;
* the sample is left out of the first-hour checklist
  ([SetupChecklist.md](SetupChecklist.md)) - out of the domain count, out of the
  account count, and out of the SPF, DKIM and DMARC lookups, because `.test` is a
  reserved name that no zone answers for. The domain item carries a not-needed
  finding naming the sample, so the count says why it is what it is. An
  `example.test` somebody made themselves is a domain like any other and is
  counted.

What the removal reports
------------------------

The removal deletes the domain the way deleting a domain anywhere else in this
server deletes one: the accounts with their mail and their folders, the aliases,
the distribution lists, the rules, the domain aliases and the data and Sieve
directories. It counts the accounts, the distribution lists, the rules and the
messages first and reports each:

    example.test was removed: 3 mailboxes, 1 distribution list(s), 1 rule(s)
    and 14 message(s) went with it.

The sample's own mail still in the **delivery queue** goes too, counted as
`queued` and named in the sentence when there is any. That matters because the
creation answers as soon as the eight messages are queued, so making the sample
and removing it a second later - the obvious thing to do when trying the feature
out - would otherwise leave queued copies addressed to mailboxes that had just
been deleted, and the server would write an error-log line for each. Only a
message whose *every* recipient is one of these mailboxes is dropped: one you
sent from a sample mailbox to an outside address still has a real recipient on
it and is left alone. A message the deliverer is holding at that instant is
delivered rather than dropped, which is harmless - the accounts are still there
while it is being delivered.

**Anything you added to the sample goes with it.** If you spent an afternoon
learning on `example.test` and made a fourth mailbox, it is in that count and it
is gone. The sample is not a sandbox that protects your work; it is a domain that
can be thrown away.

Who may do it
-------------

* Reading the state (`GET`) needs `view_configuration`, like reading the
  checklist beside it.
* Making and removing it need `manage_server` - the server administrator and
  nobody else, including over COM, where all three are refused to anything but a
  server administrator. Not `manage_domains`, deliberately: the creation makes
  three mailboxes and hands out their password, and the removal deletes a whole
  domain with everything in it. It is also why the password is in the answer to
  the creation alone and not in the state document, which `view_configuration` -
  a weaker permission - can read.
* A credential confined to named domains is refused all three: the sample makes a
  domain this server does not have yet, so a credential restricted to the ones it
  does have names nothing here.

Over COM the three are `Utilities.GetSampleDomain`, `Utilities.CreateSampleDomain`
and `Utilities.RemoveSampleDomain`, which answer the same three documents. A
refusal is raised as an error carrying the server's own sentence.

The command line, on Linux
--------------------------

`hmailserver --sample-domain create|remove` reads the configuration, opens the
database and does the work without opening a listener. Run it as the user the
service runs as: it writes to the log directory as the server does, and running
it as root leaves today's log owned by root.

The messages go into the delivery queue. A server running beside this picks them
up on its next pass, which is within the minute; a stopped server delivers them
when it starts. The command says so rather than leaving it to be discovered.

Exit status: 0 when it was done, 2 when it was refused (already there, not there,
or an `example.test` this did not make - all of them answers rather than
failures), 1 when the server could not do it. `hmctl sample-domain` answers the
same three, so a script can read one number whichever of the two it drives.

`--sample-domain` starts the server short of its listeners, and since 6.3.6 it
leaves the auto-ban's firewall integration alone: what is below the server -
Windows Defender Firewall rules, the packaged Linux hook's nftables set or
iptables chain, whatever `AutoBanCommand` did - is the running server's to keep,
or the next start's ([AutoBan.md](AutoBan.md), Below the server across a stop and
a start). Until 6.3.6 it told every ban in force to the firewall again as a start
did.

What it does not do
-------------------

* **Nothing outside the domain.** No listener, no certificate, no route, no
  global rule, no anti-spam setting, and no setting at all but the marker. A
  sample domain does not make this server ready to receive mail from the
  Internet, and the first-hour checklist still says what is left to do.
* **No choices.** There is exactly one sample domain, it is always
  `example.test`, and what is in it is fixed. A sample with options is a
  configuration screen, which is the thing this exists to let somebody skip.
* **No calendar entries, no contacts and no tasks.** The mail is there; the other
  three stores are empty, so the webmail's calendar, address book and task list
  still open on nothing.
* **Not a sandbox.** Everything in it is real: real accounts that can sign in,
  real mail in a real store, counted against real quota, and a real domain that a
  misconfigured outbound route could try to send to.
* **It is not undone.** Removing the sample and making it again gives new
  database ids and new messages; nothing is restored.
