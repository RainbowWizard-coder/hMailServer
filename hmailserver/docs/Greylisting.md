Greylisting, hardened
=====================

What greylisting is here, and the three things added to it on 18 September 2026
for one operator who read the code and asked (discussions #281, #282 and #284):
a triplet key that does not truncate, a wait that moves when a sender retries
too soon and a refusal when it keeps doing so, and a bypass list that is a
privilege granted to a named domain rather than a property of every SPF pass in
the world.

Every one of the three is off by default. A server upgraded to this release and
left alone runs the greylisting it always ran - with one exception, which is
written out under [the triplet key](#the-triplet-key-281) below: the key
case-folds the domain, and on PostgreSQL (and on MySQL with a binary collation)
`A@Example.com` and `a@example.com` were two triplets before this release and
are one now. That is what discussion #281 asked for, it is a change at the
defaults, and it makes a sender wait once less rather than once more.

What greylisting does
---------------------

Greylisting (Evans, 2003) answers the first attempt to deliver from a **triplet**
- the envelope sender, the recipient and the connecting address - with `451
Please try again later.` A mail server retries from its queue and is let through
once the **initial delay** has passed (`GreyListingInitialDelay`, 30 minutes by
default); the triplet is then remembered for `GreyListingFinalDelete` hours and
never challenged again. A sender that never retries - most spam engines - never
gets in. A triplet that is retried but never confirmed is forgotten after
`GreyListingInitialDelete` hours.

It is switched on server-wide (`GreyListingEnabled`) and off per recipient domain
(the domain's own greylisting switch). It is skipped for an address on the
**greylisting white list**, for a sender on the **anti-spam white list**, when
`BypassGreylistingOnSPFSuccess` is on and the message passed SPF, and when
`BypassGreylistingOnMailFromMX` is on and the connecting address is one of the
sender domain's A or MX records.

The triplet key (#281)
----------------------

Until schema 6072 the sender and recipient of a triplet were stored in two
`nvarchar(200)` columns and written cut to 200 characters, and the triplet was
looked up by those columns. RFC 5321 allows an address of 254 characters, and
this server accepts one that long; two different addresses sharing their first
200 characters were therefore one triplet. Plus-addressing and the bounce
addresses some sending services build get there.

Each address is now keyed by a **fixed-width digest**, stored beside it:

1. ASCII whitespace - space, tab, CR, LF - is trimmed from both ends;
2. the domain, everything after the last `@`, is lower-cased; the local part is
   left exactly as received, because RFC 5321 says its case is the receiving
   server's business and this server treats it case-insensitively elsewhere but
   must not assume every other server does;
3. the result is taken as UTF-8;
4. its MD5 is written as 32 lower-case hexadecimal characters.

Step 2 folds `A` to `Z` and nothing else, written out by hand rather than left
to the runtime's `ToLower`, whose idea of a lower-case letter is the process
locale's - the Turkish locale alone folds `I` to a dotless `i` - and a
normalisation that is declared frozen must not depend on a setting. A domain
name's letters are ASCII: an internationalised domain arrives here as its
A-label.

**This one step is a change at the defaults**, and the only one in this
release. Until now the triplet was matched by comparing the address columns,
and what that compared with depended on the backend: SQL Server and SQL Server
Compact compare case-insensitively under their usual collations, so
`A@Example.com` and `a@example.com` were already one triplet there, but
PostgreSQL - and MySQL where the column's collation is binary - compare byte
for byte and made them two. Now they are one everywhere, which is what
discussion #281 asked for. The effect on a live server is that a sender whose
mail server varies the case of the domain is challenged once rather than once
per spelling, so it errs towards letting mail through sooner rather than later,
and it applies to new triplets only: the rows written before the upgrade keep
being matched by their addresses until they are converted or expire.

That normalisation is frozen: changing any step would orphan every stored key.
MD5 because this is an index key and not a security boundary - 128 bits separate
greylist rows, and a longer digest would widen the index for nothing. The
digest is text rather than `binary(16)`, which the discussion proposed, because
the server's parameter layer speaks text and every backend it supports compares
a 32-character string identically; the sixteen extra bytes a row buy that.

The address columns are widened to 320 and still hold the address as received,
for the operator who reads the table; they are no longer part of the key. The
index is on `(glipaddress1, glipaddress2, glsenderhash, glrecipienthash)`. It is
**not a unique constraint**, on any backend, and that is deliberate: MySQL and
PostgreSQL never had one, and on SQL Server the old one turned a race - two
connections presenting the same new triplet in the same instant - into a failed
insert, which the code answers by letting the message through. Two rows for one
triplet cost nothing; a bypass on a race does.

**What happens at upgrade.** Nothing is rewritten. Every existing row keeps its
addresses and gets a NULL key. The server looks a triplet up by its key first;
while rows without a key remain, a miss is looked up by the address columns as
they were written - the first 200 characters - and a row found that way is given
its key and the whole of the addresses, once. A row that is never matched again
expires as any pending or confirmed row does, and once the sweep finds no row
without a key the second lookup is never made again. The transition costs one
extra query per new triplet for as long as legacy rows remain, which on a
default configuration is at most `GreyListingFinalDelete` (36 days). Two old
rows that were already one because of the truncation stay one until they
expire; nothing can tell them apart.

`Settings.AntiSpam.ClearGreyListingTriplets` (the Control Panel's **Clear
greylisting triplets**) empties the table, keys and all, and is the way to skip
the transition on a server that would rather greylist every sender once than
keep the old rows.

The sliding wait and the abuse refusal (#282)
---------------------------------------------

A pending triplet has a **window**: the block end, initially the first attempt
plus the initial delay. Classic greylisting counts a retry before the window
opens (`glblockedcount`) and does nothing else, so a sender that retries every
few seconds from a timer rather than from a queue walks through on its first
attempt after the window opens, and its hammering cost it nothing.

Two settings, both in `hm_settings`, both on the Control Panel's **Greylisting**
tab, in the anti-spam settings group over REST and on `Settings.AntiSpam` over
COM:

| Setting | Default | What it does when set |
|---|---|---|
| `GreyListingSlideOnEarlyRetry` | off | Each retry before the window opens **moves the window** to now plus the initial delay, and moves the row's expiry with it so a row cannot expire mid-slide. A sender that retries too soon never reaches the window; a well-behaved server that retries once too quickly is delayed a little longer. |
| `GreyListingAbuseThreshold` | 0 | The **attempt** before the window opens at which the triplet is answered **`550 5.7.1`** instead of `451`, and locked. 0 never refuses. The operator who asked for it uses 20. |
| `GreyListingAbuseLockMinutes` | 60 | How long that lock lasts: the window and the expiry are both set to now plus this many minutes. Means nothing while the threshold is 0. Between 1 and 10080. |

The two are independent: a threshold with the slide off refuses a sender that
hammers faster than the initial delay would have opened the window; the slide
with no threshold only makes a patient bot wait longer. Together they are what
stops the hammer, which is why the discussion calls the threshold the important
half.

**It counts attempts, not retries.** The counter behind the threshold is
`glblockedcount`, and it goes up once for every `RCPT TO` that names the triplet
before its window opens. A server that has twenty queued messages for one
recipient and delivers them down one connection makes twenty attempts in a few
seconds and reaches a threshold of 20, having retried nothing: from this end
those twenty look exactly like twenty retries, because SMTP does not say which
message a `RCPT TO` is for and the triplet is the sender, the recipient and the
address rather than any one message. The server has nowhere to record when the
previous attempt for a triplet was - the row carries its creation, its window
and its expiry, and no last-seen column - so a rule like "only count an attempt
a few seconds after the last one" cannot be written without another schema step,
and this release does not take one. Set the threshold with that in mind: it is
off by default, the operator who asked for it uses 20, and a bulk sender to one
mailbox is the case to think about before turning it on.

While a triplet is locked, every attempt is `550` and is counted. When the lock
passes, the triplet does **not** walk through on the strength of having waited
out its own refusal: it is reset in place - counters at zero, a fresh window of
the initial delay - and the next attempt is deferred as a newcomer's is. A
confirmed triplet - one that has passed at least once - is never refused,
whatever its count and whatever the threshold; that is what `glpassedcount` is
consulted for, so lowering the threshold below a confirmed triplet's old count
does nothing to it.

Only a triplet the lock was actually applied to is in a lock. The lock is what
writes one and the same stamp to the row's window and to its expiry - a pending
row's expiry is `GreyListingInitialDelete` hours away while its window is
`GreyListingInitialDelay` minutes away - and that is the evidence the row
carries. The count alone is not: turning the threshold on, or lowering it,
finds pending triplets whose stored count already clears it, and reading those
as locked would refuse a sender with a `550` that no lock had ever been applied
to, or throw away a wait it had already served. Such a triplet takes the
ordinary path: its next attempt before the window either walks through, because
the window has passed, or crosses the threshold there and then, with the lock
written and the log line below printed.

The `550` reads:

```
550 5.7.1 Too many delivery attempts before the greylisting delay had passed. This sender, recipient and address are refused for a while; a mail server that retries from its queue never sees this.
```

and the application log gets one line per lock:

```
Greylisting: sender@example.com to reader@example.test from 203.0.113.9 made 20 delivery attempts before its window opened and is refused until 2026-09-18 11:32:07 (GreyListingAbuseThreshold 20).
```

A `550` is permanent to the sender, so a misconfigured retry loop at a
legitimate sender is stranded until the lock passes and then starts over. That
is the trade the setting makes, and it is why it ships at 0.

For a message with an empty envelope sender the verdict - the `451` and the
`550` alike - is held over until after `DATA`, as it always was for the `451`:
a `RCPT TO` from the null sender may be another server verifying that a
recipient exists, and a `550` there would tell it the recipient does not.

The bypass list (#284)
----------------------

`BypassGreylistingOnSPFSuccess` skips greylisting for **every** SPF pass. A
hijacked mailbox at a well-configured provider passes SPF, and often DKIM and
DMARC, for the hours it is used to spray; under the global setting that mail is
never challenged. The opposite case exists too: a handful of domains whose
operators will never get PTR, HELO or SPF tidy, whose mail an administrator still
wants, and wants challenged only if the cheap authentication tests already went
against it.

The bypass list is a table of **privileges granted** (`hm_greylisting_bypass`):

| Field | Meaning |
|---|---|
| pattern | A domain such as `example.com`, matched exactly; or a suffix with a leading dot such as `.mail.example.com`, which covers that name and every name under it, at a label boundary (`notmail.example.com` is not covered). Stored lower-cased. |
| match | What the pattern is compared with: the domain of the envelope sender (`MAIL FROM`), the HELO/EHLO host, or either. |
| require SPF, DKIM, DMARC | The results the message must carry, any combination. **At least one is required.** An entry requiring nothing is refused, because a domain to be let through on no evidence belongs on the greylisting white list by its addresses, and the discussion is explicit that this list is not that. |
| note | Why the entry is there. |

On a greylisting candidate the server decides in this order: the global SPF
bypass, as before; the A/MX bypass, as before; then the list - the first entry
that covers the sender and whose required results are all passes skips
greylisting; then greylisting as usual. The list never skips SPF, DKIM or DMARC
themselves, and it is not an allow list: the message still goes through every
spam test and every score.

**Whose pass it has to be.** A required result must belong to a name the entry
covers. That is the whole of the rule, and without it an entry grants its
privilege on somebody else's authentication, because the three tests
authenticate three different identities and a bare "it passed" cannot tell them
apart:

| Requirement | The identity that must be covered | What the rule stops |
|---|---|---|
| SPF | The identity `check_host()` was given: the envelope sender's domain, or the HELO host where there is no envelope sender (RFC 7208 section 2.4). For an entry matched on the HELO, a check of the HELO name itself as well - section 2.3 - made only when a matching entry asks for it, and only while SPF is switched on. | `HELO gw1.mail.partner.test` with `MAIL FROM:<x@evil.test>`, where `evil.test` publishes a passing record. The HELO is free text; the pass authorises a different name. |
| DKIM | Every `d=` whose signature verified. | A message from `x@partner.example` signed `d=attacker.test` with the attacker's own published key. Anybody can sign a message with a key they publish; the question is whose signature it is. |
| DMARC | The `RFC5322.From` domain, which is the only thing DMARC evaluates. | An entry on the envelope domain `partner.example` satisfied by a message whose `From:` is `@attacker.test` and passes DMARC for `attacker.test`. |

A pass that carries no identity at all satisfies nothing, deliberately: the
entry names a domain, and a result that cannot be attributed to that domain is
not evidence about it.

The practical consequence for an entry matched on the HELO and requiring SPF is
that the HELO name must publish a policy of its own naming the connecting
address. That is a real requirement and not a formality - it is the only thing
that makes a HELO name mean anything - and a sending domain that has not
published one wants an entry matched on the envelope instead.

**When the decision is made.** Greylisting decides at `RCPT TO`. SPF is known by
then. DKIM and DMARC are not - they exist only once the message has been
received - so an entry requiring either cannot be judged at `RCPT TO`. Such an
entry lets the recipient through for now, and the greylisting it was spared is
run **after `DATA`**, with the DKIM and DMARC results in hand: if the entry is
satisfied the message is accepted; if not, the triplet check runs then, for the
first time, and answers `451` or `550` after the body has been received. A `451`
after `DATA` is legal SMTP, and it is what this server has always done for the
empty sender. The cost is the body's transfer, once, for a sender whose entry
was not satisfied; that is the price of requiring a result that lives in the
body. An entry requiring DKIM while DKIM verification is off, or DMARC while
DMARC is off, can never be satisfied, so it is not waited for: the message is
greylisted at `RCPT TO` as if the entry were not there, rather than received
first and greylisted after. The Control Panel's field labels say which
requirements are decided after `DATA`.

**And it is the whole message that is deferred.** After `DATA` there is one
reply for every recipient, so when the greylisting held over for one recipient
answers `451` or `550`, the transaction ends for all of them - including a
recipient at a domain with greylisting switched off, which was accepted at
`RCPT TO` and never entered greylisting at all. At `RCPT TO` this does not
happen: a `451` there refuses that one recipient and the session goes on. The
alternative after `DATA` - drop the deferred recipients and answer `250` -
would be worse, because `250` tells the sender every recipient was delivered
and the greylisted ones would simply lose the message; a deferral costs a
retry, on which the triplet has aged and everybody is served, once. It is
another reason an entry that requires DKIM or DMARC is a heavier thing than one
that requires SPF.

The list is `AntiSpam.GreyListingBypassEntries` over COM (an entry has
`Pattern`, `MatchOn` 1 to 3, `RequireSPF`, `RequireDKIM`, `RequireDMARC`,
`Note`, `Save` and `Delete`), `/api/v1/greylisting-bypass` over REST (`GET`,
`POST`; `PUT` and `DELETE` at `/{id}`; `match` is `envelope`, `helo` or `both`
and the three requirements are booleans), the **Greylisting bypass list** page
of the Control Panel beside the white list, and the same-named table of the
Deck's **Lists** view. It is in the backup beside the white list, and a change
to it is on the audit trail. The server reads it into memory once and again after
any save or delete through those interfaces; a row written straight into the
table by hand is seen at the next such save, or at the next start. Empty by
default, and then nothing changes.

What it does not do
-------------------

* **It does not slide or refuse a confirmed triplet.** Once a triplet has
  passed, only `GreyListingFinalDelete` applies to it.
* **It does not move the initial delay's unit.** The delay is still whole
  minutes, so the regression test that proves the slide takes sixty-six seconds
  and says so.
* **It does not rewrite old rows at upgrade.** They are converted as they are
  matched and expire if they are not; the reasons are above.
* **The bypass list does not bypass the tests.** SPF, DKIM and DMARC run and
  score as before; the list only decides whether greylisting is skipped, and it
  decides nothing for a message that fails them.
* **A DKIM or DMARC entry does not decide before the body.** It cannot; see
  above. An operator who wants greylisting decided before `DATA` for a domain
  should require SPF alone for it.
* **The list is not consulted for a recipient domain that has greylisting off,
  or on a server where it is off**, since there is nothing to bypass. A
  recipient at such a domain can still be caught by the deferral of a message
  whose other recipient was greylisted after `DATA`; see above.
* **It does not count retries, only attempts.** `GreyListingAbuseThreshold`
  reads a counter that every early `RCPT TO` for the triplet advances; see the
  threshold, above.
* **It does not authenticate the HELO name for you.** An entry matched on the
  HELO and requiring SPF needs the HELO name itself to publish a record naming
  the connecting address; an envelope pass is not lent to it.

Where to look when it does not behave
-------------------------------------

* A sender is refused `550` unexpectedly: the application log has the lock line
  quoted above, naming the count and the threshold. The lock lasts
  `GreyListingAbuseLockMinutes`; **Clear greylisting triplets** lifts every lock
  at once, at the cost of greylisting every sender once more.
* A domain on the bypass list is still greylisted: the entry requires a result
  the message did not carry, or requires DKIM or DMARC while that test is
  switched off, or matches on the wrong thing - an envelope entry does nothing
  for a sender whose `MAIL FROM` is at a different domain from its HELO - or the
  result the message carried belongs to a name the entry does not cover, which
  is the commonest of the four once the others are ruled out: read the
  `Authentication-Results` header of a message that arrived and check that
  `header.d`, the `From` domain or `smtp.mailfrom` is a name the pattern covers.
  With debug logging on, the debug log names the entry that was satisfied or
  waited for.
* An upgraded server greylists a sender it knew: the old row had no key and was
  matched by its first 200 characters exactly as before, so a known triplet is
  still known; a sender that is challenged again was not a confirmed triplet or
  had expired.

The tests are `RegressionTests.AntiSpam.GreyListingHardened` (the triplet key
through the abuse threshold, the threshold and its lock, the slide against a
classic control, the SPF-only entry at `RCPT TO`, the DKIM entry after `DATA`,
a HELO suffix at a label boundary, the three negative controls of the identity
rule - a valid signature from another domain, an SPF pass for an unrelated
envelope domain under a HELO entry, and a DMARC pass for another `From` domain,
each paired with the positive case that proves the result really was a pass -
and a row from before schema 6072 found by its addresses, rewritten with its
key and then found by the key alone) and
`RegressionTests.API.RestApiGreylistBypass` (the resource, its refusals, who
may reach it, the three settings and their bounds).
