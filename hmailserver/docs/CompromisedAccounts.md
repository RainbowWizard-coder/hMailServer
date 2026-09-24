Compromised-account detection
=============================

A stolen password does not look like an attack from where the server stands.
The client authenticates, the session is ordinary, the messages are well-formed,
and the per-account sending limit - so many messages a day, whoever is sending
them, by SMTP, the webmail's send or the send API - is a ceiling most takeovers never touch before the damage is done. What
changes is the *shape* of what the account does: the account that sent six
messages in its busiest hour sends forty in this one, to addresses it has never
written to, at an hour it never sends at, from a network it has never signed in
from.

That is what this judges, at the moment an authenticated account submits a
message - SMTP `AUTH` followed by `DATA`, the webmail's send, or a draft the
webmail scheduled for later when its minute comes - against the account's own
history. Nothing here reads the message's text.

**Watch by default.** A new installation and an upgraded one both start in
`watch`: every authenticated submission builds the account's baseline, and a
submission in the shape of a takeover raises the `account.compromised` alert
saying what enforcing would have done - and does nothing else. The false
positive of this control is a person locked out of their own mail, so nothing is
held and nobody is locked until an administrator has watched what it would do to
their users and set `compromise_detection_mode` to `enforce`.

The four signals
----------------

Each is judged against the account's baseline: its authenticated submissions in
the last `compromise_baseline_days` days (30), one row per submission in
`hm_accountactivity`, and the addresses it has written to in that time
(`hm_accountrecipients`).

| Signal | Scores when | Points |
|---|---|---|
| **volume** | the messages in the last hour, this one included, reach `compromise_volume_floor` (30) *and* exceed `compromise_volume_factor` (4) times the account's busiest hour in the baseline | 2 |
| **recipients** | the distinct recipients in the last hour reach `compromise_recipient_floor` (20) *and* at least `compromise_new_recipient_share` per cent of them (80) are addresses the account has never written to | 2 |
| **hour** | the account has not submitted at this hour of the day (UTC) in the baseline | 1 |
| **address** | the client's network - the /24 of an IPv4 address, the /64 of an IPv6 one - has not been seen for this account in the baseline | 1 |

The points add up, and `compromise_score_to_lock` (3) is a takeover. One new
network is a phone on a different connection; one late night is a late night;
a surge to strangers from a strange place at a strange hour is not. The volume
and recipients signals are what a takeover looks like, and either alone with one
of the two small signals is enough; the two small signals alone never are.

An account with fewer than `compromise_minimum_history` submissions (20) in the
baseline has no baseline. For it only the two floors count: a brand-new account
that sends thirty messages to twenty strangers in its first hour is locked, which
is the only shape that means anything without history, and nothing subtler is
held against it. Per account, **sensitivity** shifts the score that locks:
`relaxed` needs one point more, `strict` one fewer; and an account marked
**exempt** is never judged at all - which is where a newsletter sender, a
ticketing system or a scanner that mails strangers in bulk belongs, rather than
in a server-wide threshold raised until it catches nothing.

What enforcing does
-------------------

When the mode is `enforce` and a submission completes the shape:

1. **The message is held.** It is saved with `hm_messages.messagetype` 4
   (`Message::HeldForReview`), which the deliverer's query never selects, with a
   row in `hm_heldmessages` carrying the sender, the recipients, the subject and
   the reason. The account's other messages still queued from the last hour are
   held the same way, since they were submitted by the same hand. A message the
   deliverer has already locked is in flight and is left alone.
2. **The account is locked.** `AccountLockout` holds the name until it is
   released: at the check every password logon makes - SMTP `AUTH`, POP3,
   IMAP, ManageSieve, the DAV services, the webmail and REST sign-in - with the
   same reply as the failed-authentication lockout, and at the check the
   logons that prove identity without a password make instead
   (`AccountLogon::RefusedByHold`): an OAuth2 bearer token (`XOAUTH2`,
   `OAUTHBEARER`), a Kerberos ticket (`GSSAPI`) and a client certificate
   (`EXTERNAL`), on all three protocols. A token proves who the client is,
   not that the account is theirs to use, so a locked account is refused with
   a valid one too - and, as with the lockout, that refusal is charged to
   nothing, since the client most likely to keep presenting a valid token for
   a locked account is its owner's. Unlike the lockout it has no clock and no
   password clears it. The lock is a row (`hm_accountreview.arvlocked`), so it
   survives a restart. Its browser sessions are ended.
3. **The alert is raised.** `account.compromised`, seeded on, with the account,
   the client address, the score, the signals and one sentence per signal; and
   the `account.locked` webhook, with the reason.

The client that submitted the held message is answered `250 Queued`, as it would
be for any message: a takeover learns nothing from the reply, and a legitimate
sender's mail is a review away from being sent. A lock refuses the next logon,
not the session that is up, so what that session goes on submitting is held on
the standing lock without being judged again - and never re-locked or re-alerted.
A draft the webmail scheduled before the lock is held on it the same way when
its minute comes.

**The alert is raised once per account per hour**, in either mode. Every
message after the shape completes scores again for the rest of the hour - the
last-hour counts stay past the floors - and a surge is one alert, not one per
message: without that, one bulk sender in watch mode would spend the server's
alert ceiling (`alert_max_per_hour`) on itself and push every other condition
into the digest for the rest of the hour. `hm_accountreview.arvalertedat` is
what says the hour is not up. A lock is always said, whatever the hour, since it
is a new state.

Reviewing
---------

**Held for review** on the Control Deck lists what is held - when, which account,
to whom, the subject, the reason, how it came in - with **Release** and
**Discard** on each row; below the list, one account's review state is looked up
by address: the lock and since when, the last shape seen, the baseline (how many
submissions in the window, whether that is a baseline yet, the busiest hour, the
hours of the day and the client networks seen, the recipients remembered), the
two choices - exempt, sensitivity - and **Unlock**.

Releasing a message puts it back into the delivery queue as if it had just been
submitted; discarding deletes it. A released message the webmail sent - a
scheduled draft included - gets its copy in the sender's folder designated
`\Sent` at the release, marked read, when the account has such a folder and its
quota has room: the send made none, because a copy of a message that was then
discarded would say it was sent. Neither releasing nor discarding unlocks the
account: that is a decision of its own, made either with the compromise unlock
alone (`POST /api/v1/accounts/{address}/review/unlock`, the Deck's button on the
review state) or with the general unlock that clears every lock the account is
behind (`POST /api/v1/accounts/{address}/unlock`, `Application.UnlockAccount`
over COM, the Accounts row's **Unlock** on the Deck).

**An unlock is a fresh start.** What the account submitted before it has been
looked at, and does not count towards the next hour's volume or recipients
(`hm_accountreview.arvunlockedat` is where the hour starts from), so the next
message after a false positive is not the same shape again and the account is
not locked back out by the message it was let in to send. A takeover that goes
on after the unlock is caught again once it reaches the floors again.

A held message nobody has looked at in `compromise_held_days` days (7) is
discarded by the retention sweep, and the sweep is where the baseline forgets
what the window no longer covers; a held message younger than ten minutes is
left alone by the sweep's orphan pass whatever its rows say, since its review
row may still be on its way. An account being deleted takes its held mail with
it: nobody is left to release it.

Every one of these is on the audit trail: a release or a discard names the
message, the sender, the recipients and the subject; a change of exemption or
sensitivity names the account; an unlock says which lock was cleared.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/review` | `{mode, held_days, messages:[{id, message_id, account, sender, recipients, subject, size, held, source, reason}]}`, newest first. `held` in seconds since 1970; `source` is `smtp` or `webmail`. |
| `POST /api/v1/review/{id}/release` | Back into the delivery queue. |
| `DELETE /api/v1/review/{id}` | Gone. |
| `GET /api/v1/accounts/{address}/review` | `{address, mode, exempt, sensitivity, locked, locked_at, unlocked_at, alerted_at, score, signals, reason, baseline:{days, history, minimum_history, has_baseline, peak_hour, last_hour, recipients_known, held, hours, networks}}`. `unlocked_at` is the last unlock, `alerted_at` the last alert for the account, each in seconds since 1970 and 0 for never. |
| `PUT /api/v1/accounts/{address}/review` | Any of `exempt` (true or false) and `sensitivity` (`default`, `relaxed`, `strict`). A field left out keeps its value; an unknown field is a 400 naming it. |
| `POST /api/v1/accounts/{address}/review/unlock` | The compromise lock alone, cleared. `{unlocked}` says whether it was locked. |

The held list has the delivery queue's shape - one entry names recipients in
any number of domains, and releasing one delivers mail - so it takes the queue's
permissions (`view_queue`, `manage_queue`) and is refused to a credential
confined to named domains. An account's review state is that account's: reading
it is `view_accounts`, the two choices are `manage_accounts`, the unlock is
`unlock_account`, each scoped to the account's domain. All six are refused to an
API key: a key that could release held mail could send what a takeover of its
own account got held, and one that could mark an account exempt could switch the
detection off for the account it stole. The same refusal reaches the two other
routes a key could have used to the same end: the general unlock
(`POST /api/v1/accounts/{address}/unlock`) clears the failed-authentication
lockout and the POP3 mailbox lock for a key as it always did, but keeps the
compromise lock and answers `compromise_lock_cleared: false`; and every
`compromise_*` key of `PUT /api/v1/settings` is refused to a key with a 403
naming it, and nothing in that body is applied. The administrator password and
an account holding the permission do both.

The settings
------------

All on `PUT /api/v1/settings`, the Deck's Settings page, `hmctl settings set` and
`hmctl config`; every range is refused in the setting's own sentence rather than
clamped.

| Setting | Default | What it is |
|---|---|---|
| `compromise_detection_mode` | `watch` | `off` records and judges nothing; `watch` records the baseline and alerts; `enforce` holds, locks and alerts. |
| `compromise_baseline_days` | 30 | The window the baseline covers, and how long the record of submissions and recipients is kept. 7 to 365. |
| `compromise_minimum_history` | 20 | The submissions an account needs in the window before its busiest hour, its hours and its networks count. Below it only the floors are judged. |
| `compromise_volume_factor` | 4 | The volume signal's multiple of the busiest baseline hour. 2 to 100. |
| `compromise_volume_floor` | 30 | The fewest messages in an hour that can be a volume signal. |
| `compromise_recipient_floor` | 20 | The fewest recipients in an hour that can be a recipients signal. |
| `compromise_new_recipient_share` | 80 | The percentage of the hour's recipients that must be strangers. |
| `compromise_score_to_lock` | 3 | The score that is a takeover; sensitivity shifts it by one either way. 1 to 6. |
| `compromise_held_days` | 7 | How long a held message waits for a review before the sweep discards it. 1 to 90. |

What it does not do
-------------------

- **It does not read the message.** Nothing about the text, the links or the
  attachments is judged; that is the outbound content policy row, which is a
  different control. A takeover that sends one message an hour to one stranger
  each is below every floor, and is meant to be.
- **It judges authenticated submissions only.** SMTP `AUTH` + `DATA`, the
  webmail's send and a scheduled draft when its minute comes are submissions.
  Mail that arrives for local delivery from outside, mail a rule forwards, mail
  the server generates itself, and the read receipt or unsubscribe a reader asks
  the webmail to send (one message, to the one address that wrote to them)
  build no baseline and are never held.
- **The baseline is the account's own, on this server.** A restore brings back no
  baseline: `hm_accountactivity` and `hm_accountrecipients` are traffic and are
  not in the backup, so a restored account is judged by the floors alone until
  it has a baseline again, as a new one is. The lock and the last shape are not
  carried either, and nor - this is a gap - are the exemption and the
  sensitivity: they are set again after a restore, and the audit trail holds
  who set them.
- **It does not drop a session that is up.** The lock refuses the next logon;
  an SMTP or IMAP connection already authenticated stays connected, and what it
  submits is held on the lock.
- **The hour signal is UTC.** An account whose people work across time zones
  has a wide set of hours seen, which is the right answer; the signal is worth
  one point for that reason.
- **The client address is the connection's peer.** Behind a proxy that does not
  speak PROXY protocol or XCLIENT every client is the proxy, the network is
  always seen, and the address signal never scores - the other three still do.
- **There is no Control Panel page.** The desktop Control Panel drives the
  server over COM, and the review state, the held list and the settings are
  REST: the Deck, `hmctl settings` and the OpenAPI document are where they are.
