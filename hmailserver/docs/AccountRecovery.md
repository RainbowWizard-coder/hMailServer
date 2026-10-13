Account recovery: a recovery address and one-time codes
======================================================

A forgotten password and a lost authenticator used to be the same thing: a ticket.
Only an administrator could reset a mailbox password, and only an administrator
could clear the second factor. This is the other half - a person sets a recovery
address and a set of one-time codes for themselves, and uses them when they need
them.

It has three parts, and they are independent:

* a **recovery address** - another mailbox of the person's, confirmed by following
  a link sent to it, which a password-reset link can later be sent to;
* **ten one-time recovery codes**, shown once, each usable once, which stand in for
  a one-time code from an authenticator app;
* a **forgotten-password flow** from the webmail's sign-in card, which uses the
  first of those.

What it needs to work
---------------------

`account_recovery_enabled` on `PUT /api/v1/settings` is **on** by default, and on a
stock installation it still does nothing at all: an account that has set no recovery
address cannot be sent a link, and a server that has no public address of its own
cannot write one.

The links are written against the server's public base URL - `confidential_base_url`,
falling back to `quarantine_digest_base_url` and then to the import sign-in's
`ImportOAuthRedirectBaseUrl`. With none of the three set, the webmail's recovery card
says so, the confirmation cannot be sent, and `POST /api/v1/password-reset` answers
503. Set one before telling people the feature exists.

Two more settings: `recovery_reset_link_minutes` (30 by default, 5 to 1440) is how
long a password-reset link lives, and `recovery_verify_link_hours` (24, 1 to 168) how
long the link that confirms an address lives. The Control Panel has all three on
Connections & protocols > Webmail, the Sign-in tab, which reads and writes them over the
REST API, so the REST API has to be on.

The recovery address
--------------------

On the webmail's **Security** page, under *If you forget your password*. Setting one
asks for the account's own password again - never an app password, and a request that
was itself authenticated with an app password is refused - and for the second factor
when the account has one.

The address is stored **unverified** and a message goes to it with a link. Until that
link is followed the address counts for nothing: no reset link will be sent to it.
Changing the address makes it unverified again and asks for the new one to be
confirmed, and ends every link that was in flight for the old one.

That message is charged against **the same per-address budget a password-reset link is
charged against** - one a minute and six an hour for the address it would go to - and
against a second counter of the same size for **the account that asked**, so that naming
a fresh address each time is not a way round the first. `PUT /api/v1/me/recovery` and
`POST /api/v1/me/recovery/resend` both answer 429 with `retry_after` when either
refuses. A verification message is the same capability as a reset
link, the server posting mail to an address somebody named, and the credential in front
of the route is no reason to leave it unmetered: an account can name any address in the
world, and a leaked app password would otherwise be a way to drive that at a stranger.
Setting an address asks the budget *before* the row is written, so a refusal never
leaves an address stored that its confirmation could not be sent for.

Removing an address and asking for its confirmation again are both refused to a request
that was itself authenticated with an app password, as setting one is, and as every
other act that ends or mints one of the account's credentials is. An app password is a
credential deliberately handed to a mail program; it has no business taking away the
account's own way back in, or making the server post mail on its behalf.

The link opens a page with a button rather than confirming on the GET. Mail security
products follow every link in a message before the reader sees it, and a GET that
confirmed would be followed by the scanner rather than by the person.

**The address is never answered in full again** - not to the account that set it, not
to an administrator, not over COM, not in an export. What is shown everywhere is the
masked form: the first letter of the local part, the first letter of each domain
label, and the top-level label whole. `other@elsewhere.example` reads as
`o....@e.........example`, which is enough to recognise an address you chose and not
enough to write to one you did not.

The one-time codes
------------------

Ten, made together, shown **once** at the moment they are made, and stored hashed with
the same scheme the account password uses (`PreferredHashAlgorithm`, and never
`ETNone`). Making a set ends whatever set the account had. Nothing here can read a
code back: not an administrator, not a database dump, not the code that checks them.

The ten rows are written before the old set is removed, and a write that fails rolls
the new ones back, so a generator that stopped halfway leaves the account with the set
it already had rather than with some number of working credentials nobody has ever
seen. "The recovery codes could not be made; nothing has changed" is a sentence that
has to be true.

A code is presented in the `X-hMailServer-Recovery-Code` header wherever a one-time
code from an authenticator app is asked for, and the webmail offers a field for one
beside the authenticator's at every one of them:

* `POST /api/v1/session` - signing in to the webmail. An account with a second factor
  enrolled cannot sign in with its own password (see *Second factors* below); a code
  makes it possible once. The answer then carries `recovery_codes_left`, and the page
  says how many are left.
* `POST /api/v1/me/password` - changing the password.
* `POST /api/v1/me/app-passwords`, `PUT /api/v1/me/recovery` and
  `POST /api/v1/me/recovery/codes` - minting a credential.

At sign-in the account's own password is verified **before** the code is looked at,
and the code is **spent only once the whole attempt has been accepted**. Both halves of
that ordering are deliberate. A code is compared against every unused code the account
has, each comparison a deliberately expensive hash, so checking the code first would let
anybody who can name a mailbox spend ten of them a request; and spending it only at the
end means somebody who holds a code and is guessing at the password cannot burn the set.
A wrong code leaves the account password refused exactly as no code at all would, and
that refusal is counted by the ordinary per-name lockout and the per-IP auto-ban.

Both halves live where they can be true of every route rather than of one. The match
is made inside `PasswordValidator::ValidatePassword`
(`PasswordValidator::SecondFactorOffer`), which is behind the per-name lockout and the
domain country policy that `AccountLogon::Logon` puts ahead of every verification -
outside it, a locked name would still cost an Argon2id verification a request and a
right password would cost ten more than a wrong one, which is a password oracle for
exactly the accounts a second factor protects. And every route that takes a code
matches it first and spends it last: the password form, the app-password form and the
recovery form can all still refuse what they were asked to do - a password the policy
will not have, an address that is not an address - and ten refusals must not leave an
account with no authenticator and no codes.

Every place a one-time code is checked after a password it has already accepted counts
a wrong code, and they all count against the same record:

* the REST routes that ask for a code **on top of** the password -
  `POST /api/v1/me/password`, `POST /api/v1/me/app-passwords`, `PUT /api/v1/me/recovery`,
  `POST /api/v1/me/recovery/codes`, and `POST /api/v1/passkeys` for an account and for the
  administrator;
* the administrator's Basic sign-in on the REST API, the code in `X-hMailServer-OTP`,
  `POST /api/v1/session` from the Control Deck included;
* the COM sign-in, `Application.AuthenticateWithCode`, for an account and for the
  administrator - which is what the Control Panel and scripts use.

A wrong code (from the authenticator or from the set) is a guess at six digits by
somebody who has the password. Each is logged. On the REST API each is charged to the
source address's auto-ban and, for an account, to the per-name lockout when
`AccountLockoutThreshold` is set; COM has no source address and stays outside the
lockout and the auto-ban, as its passwords always have, so that administration stays
reachable. And each owner - an account's address, or the administrator - has a budget
of its own that needs no setting and that every one of those places shares: **five
wrong codes in fifteen minutes, and for the next fifteen every code for that owner is
refused** - the right one too - with 429, `code` `second_factor_locked` and
`Retry-After` on the REST API, and as a refused sign-in over COM, with a line in the log
that says why. A loopback peer meets the budget as much as any other: the auto-ban
leaves 127.0.0.1 alone, and behind a reverse proxy every request comes from there, so
before the audit that preceded 6.3.4 the administrator's Basic sign-in behind a proxy
had no brake at all but the request rate. A request that carries no code at all is not
counted; it is a page that has not been told one is needed, and it gets the 401 that
names the header. The budget is in memory, as the per-name lockout is: a restart of the
service, a `Reinitialize`, or a stop of the REST listener clears it, and so does
enrolling or removing that owner's second factor. Without it the only brake was the
request rate, 200 in ten seconds, and a code - three valid in a million at any moment -
fell to guessing in hours, minting an app password or a passkey that outlives the
session it was guessed from.

**A code is taken once** (RFC 6238, section 5.2). Once a code has been accepted for an
owner - at any of the places above, over COM or over REST - the same code is refused
wherever it comes next, and so is the code of any earlier step, for as long as it stays
in its window of a minute and a half. Without that, a code read over a shoulder, out of
a proxy's log or off a phishing page was good for the rest of its window, and the
password beside it made the two a whole credential. Such a code is refused with the
same 401 as a wrong one, and logged, but it is **not** counted against the budget above,
charged to the auto-ban or to the per-name lockout: it is no guess - it can never be
taken, whoever sends it - and the one most likely to send it again is the owner, within
the half minute the authenticator still shows it. What it changes for people:

* Somebody who has just used a code - to sign in to the Control Deck, say - and is asked
  for one again within the same half minute waits for the authenticator's next code.
  So does somebody whose request was refused for another reason after the code was
  checked - a new password the policy will not have, say: the code is taken when it is
  checked, so that two requests carrying it cannot both be the first, and sending the
  form again with the same code is refused until the next one is typed.
* A script that sent the administrator's password and a code with **every** request
  signs in once instead - `POST /api/v1/session`, and the `hmailsession` cookie from
  there, with `X-Requested-With: hMailServer` on the requests that change something -
  or uses an API key. Only the first of its requests in each half minute would be
  accepted otherwise; the rest are refused, though not counted, so such a script does
  not lock the administrator out.

The record of steps taken is kept in memory too, and only for the minute and a half a
step can be in a window. A restart of the service forgets it, which would let a code
taken in the last minute and a half before the restart be taken once more after it; a
`Reinitialize` and a stop of the REST listener keep it.

The forgotten password
----------------------

**Forgotten your password?** on the sign-in card asks for an address and posts it to
`POST /api/v1/password-reset`, which takes no credentials.

Because it takes none, another site could otherwise have each of its visitors'
browsers post it and have this server mail a reset link to an address the page
names. Since 6.3.7 the request is refused when the browser reports it as
cross-site — a `Sec-Fetch-Site` of `cross-site` or `same-site`, or an `Origin`
that is not this server's — so only this server's own sign-in card, and a script
that sends no such header, reach it ([DevicesAndSessions.md](DevicesAndSessions.md)).
The double opt-in, the single answer for every address, and the rate limits
below all stand behind that.

The answer is **always the same**: 202, and one sentence saying that if the address
belongs to a mailbox here and that mailbox has a confirmed recovery address, a link is
on its way. That is the answer for an address that is not a mailbox, for a mailbox
with no recovery address, for one whose address has never been confirmed, for a
directory-linked account whose password this server does not hold, and for a request
that has just been rate limited. A refusal that differed would be an address
directory for anybody who asked.

It is also given **in the same time**. Only the rate limits below are decided before
the answer; looking the address up, making the link and queueing the message happen
afterwards, on the server's async work queue. Done inline, as they were until the audit
before 6.3.4, a mailbox with a confirmed recovery address was answered several database
round trips and a file write later than a name with no mailbox, and a stranger with a
stopwatch could sort a list of addresses into the ones that can be reset and the rest.
What the queued work found - no such mailbox, no confirmed address, a link sent, a
failure - is in the application log, and nowhere else.

The rate limits are charged **before** anything is looked up, and for every address
alike. Per address: one request a minute and six an hour, which is what stops a link
being mailed at somebody over and over. Per client address: twenty an hour and no
minute rule at all, because a client address is very often a shared one - an office
NAT, a mail gateway, a webmail front end on loopback - and a minute rule there would
let one person's forgotten password stop everybody else's. Both are in memory, and
both are counted for an address that has no mailbox exactly as for one that has.

**The per-client counter is skipped when the peer is loopback**, and an operator should
know why. Nothing in this server reads a forwarded-for header, so on the ordinary way of
publishing a webmail - TLS terminated by a reverse proxy in front of hMailServer - every
request would arrive from the proxy's address and twenty an hour would be a ceiling on
the whole installation, met by the twenty-first person as the same silent 202. A proxy
on the *same* machine is therefore not counted per client. It has a ceiling of its own
instead: **thirty requests in any sixty seconds from loopback, all together** - far more
than people forget passwords at, and a limit on how fast anybody can drive the route
through the proxy. A proxy on **another** host still shares one per-client bucket, and
there is no setting for either ceiling: if a busy installation behind a remote proxy
finds resets stopping, the application log names the address that was limited, and the
answer is to put the proxy on the mail server or to accept the cap.

The link goes to the confirmed recovery address and **nowhere else** - never to the
mailbox itself, which is the mailbox somebody who cannot get in cannot read. It is 32
random bytes, stored only as a SHA-256 digest, good once, and dead after
`recovery_reset_link_minutes`. Asking again replaces it, so only one is ever live.

`GET /portal/reset?token=...` is the page it opens: a form for the new password,
served with no script, no remote anything, a form action pointing only back at this
server, and `Cache-Control: no-store`. Posting it - or `POST
/api/v1/password-reset/complete` with the same token - does five things:

1. the new password goes through the same **password policy**, **reuse history** and
   **breach check** as the webmail's own password form, so a password chosen here is
   one an administrator could have set;
2. the link is spent the moment a password is **accepted**; a password one of those
   three refuses leaves it alive, so the person can choose again on the same link;
3. the password is written and `accountpasswordchanged` stamped;
4. **every browser session of the account is ended** - a password is reset most
   urgently when somebody else may have had it;
5. a note goes to the mailbox **and** to the recovery address saying the password
   changed, and the `account.password-reset` alert is raised.

A token that is unknown, used, expired, or whose account has since changed or cleared
its recovery address, answers 410 with the same sentence, and counts against the source
address as a failed logon does. So does one whose account has been **disabled** or
linked to a directory since the link went out: those are the two properties the request
was refused for in the first place, asked again half an hour later, because an
administrator who disables an account in response to a report is doing exactly that
inside a live link's life.

Second factors, and what a lost authenticator can do
----------------------------------------------------

This server's rule is that an account with a second factor enrolled cannot use its own
password as a mailbox credential at all (`PasswordValidator::ValidatePassword`); app
passwords are what mail clients use instead. A recovery code lifts that for one
request, which is what gets somebody whose authenticator is gone back into the webmail
and back to a working app password.

**What it does not do:** enrol a new authenticator. A second factor is still set and
cleared by an administrator, over COM and the Control Panel; there is no self-service
enrolment in the webmail, and this row did not add one. Somebody who has lost their
authenticator gets back in with a code and then asks for the second factor to be
re-enrolled.

The threat model, said plainly
------------------------------

**A recovery address is exactly as strong as the mailbox that receives it.** Anybody
who can read that mailbox can reset this password. It is a convenience for the person
who has forgotten a password, and it is not a second factor; an installation that
cannot accept that should set `account_recovery_enabled` to 0, and the webmail's card
disappears.

The codes are the same trade in the other direction: ten strings on paper are ten
credentials that survive a lost phone, and ten credentials somebody can find in a
drawer. They are drawn from the same alphabet an app password uses - Crockford-style
base32 without the characters that are read as other characters - and ten characters
of thirty symbols is a little over 49 bits, which is not guessable at the rate a
sign-in is counted at.

Nothing is stored that can be read back. The recovery address is the one exception,
and it is stored in the clear because the server has to be able to post to it; what
follows from that is the masking rule above, which is about what a surface *shows*,
not about what the database holds. An administrator with the database can read a
recovery address, as they can read any address the server routes mail to.

What an administrator can and cannot do
---------------------------------------

**Can**: see whether an account has a recovery address, whether it has been confirmed,
the masked spelling of it, and how many of its codes are unused, on three surfaces:

* REST - `GET /api/v1/accounts/{address}` carries `recovery_address_set`,
  `recovery_address_verified`, `recovery_address` (masked) and
  `recovery_codes_unused`;
* the Control Deck's account editor, which has a **Recovery** card;
* the Control Panel's account dialog, which has a **Recovery** tab, over the COM
  properties `Account.RecoveryAddress` (masked), `Account.RecoveryAddressVerified`
  and `Account.RecoveryCodesUnused`.

And clear either of them, as two separate acts, because they answer two separate
reports:

* *"that address is not mine any more"* - `DELETE /api/v1/accounts/{address}/recovery`,
  the Deck's **Clear the recovery address**, the Control Panel's button, or
  `Account.ClearRecoveryAddress()` over COM. It also ends every reset link in flight,
  and leaves the codes alone.
* *"somebody else has seen my code card"* -
  `DELETE /api/v1/accounts/{address}/recovery/codes`, the Deck's **Clear the recovery
  codes**, the Control Panel's button, or `Account.ClearRecoveryCodes()` over COM. It
  leaves the address alone. A code stands in for the account's second factor, so a set
  somebody else has read is a standing way past it - and the account holder's own
  replacement from the webmail is exactly what somebody locked out of the webmail cannot
  reach, which is why this exists at all.

A completed password reset deliberately does **not** end the code set. A reset through
the recovery address says nothing about the codes, and somebody whose authenticator is
already gone would be left with no second factor and no codes at the one moment they
are getting back in. An administrator ends a set when there is a reason to.

**Cannot**: read the address in full through any interface, read a recovery code, set
a recovery address on somebody's behalf, or generate codes for them. Those are the
account holder's, proved with the account's own password. An administrator who could
set the address could reset the password by pointing it at themselves, which is the
one thing this whole feature has to make impossible.

Setting a recovery address, clearing one, ending a code set and making a set of codes
are on the audit trail (`AuditTrail::RecordEvent`, object type `account`, actions
`recovery-address-set`, `recovery-address-cleared`, `recovery-codes-cleared`,
`recovery-codes-generated`).
Confirming an address and completing a reset are **not**, and that is a real gap said
plainly: both happen on unauthenticated pages where there is no actor on the thread, so
a trail row would say a change nobody made. What those two leave instead is an
application-log line each, the `hm_accounts` write the statement chokepoint already
records for the password itself, and - for the reset - the `account.password-reset`
alert, seeded on with no cool-down.

Where the rows are
------------------

Schema 6076, three tables:

| Table | What it holds | In the backup |
|---|---|---|
| `hm_accountrecovery` | one row per account: the address, whether it is confirmed, when it was set and last changed | yes |
| `hm_recoverycodes` | the hash of each code, its encryption type, and when it was used | yes |
| `hm_recoverytokens` | the one-shot links: kind, digest, the address it was sent to, expiry | no |

The tokens are not carried for the reason the quarantine digest's are not: a restored
capability is one nobody issued, and the two kinds here are exactly the two that change
who can read a mailbox. The address and the codes are carried because they are the
person's own way back in, and a restore that dropped them would take it away without
saying so - the person would find out at the one moment they needed it.

The retention task's sweep - six-hourly, or `Utilities.RunMessageRetention` on demand -
removes the links that have been used or have run out.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/recovery` | `{enabled,available,address,address_set,address_verified,updated,codes:{total,unused,created}}`; `address` masked. |
| `PUT /api/v1/me/recovery` | `{address,password}`; stores it unverified and sends the confirmation. |
| `DELETE /api/v1/me/recovery` | Removes the address and every link for it. |
| `POST /api/v1/me/recovery/resend` | The confirmation again, for an address not yet confirmed. 429 when the budget will not have it yet. |
| `POST /api/v1/me/recovery/codes` | `{password}`; answers `{codes:[...]}`, the only time they are shown. |
| `DELETE /api/v1/accounts/{address}/recovery` | An administrator clears the address. |
| `DELETE /api/v1/accounts/{address}/recovery/codes` | An administrator ends the code set. |
| `POST /api/v1/password-reset` | `{address}`; always 202, always the same sentence. |
| `POST /api/v1/password-reset/complete` | `{token,password}`; the new password. |
| `GET`/`POST` `/recovery/verify?token=` | The page that confirms an address. No credentials. |
| `GET`/`POST` `/portal/reset?token=` | The page that sets the new password. No credentials. |

See also
--------

* [DevicesAndSessions.md](DevicesAndSessions.md) - what a password reset ends, and how
  the sessions it ends are listed and revoked by hand.
* [PasswordBreachCheck.md](PasswordBreachCheck.md) - the corpus a password chosen here
  is checked against.
* [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md) - the `account.password-reset` alert
  and what the trail holds.
