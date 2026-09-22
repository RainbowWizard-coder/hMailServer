Message recall within the server
================================

An unread message sent to accounts on this server can be withdrawn by its
sender. Outlook does this within Exchange, and nothing does it across the
internet: a message that has left for another server is that server's, and
this one does not pretend otherwise. What this server does is withdraw, from
its own mailboxes, the copies that nobody has read yet - and say, in numbers,
what became of the rest.

The sender does it from the webmail: **Recall** in the menu of a message in
the Sent folder, a sentence to confirm, and a sentence back. A program does it
with `POST /api/v1/me/recall`. Nothing is sent to anybody: no recall notice
reaches the recipients, because the notice is the part of Exchange's recall
everybody hates, and the Sent copy's own mark is the record. The decision and
the rows are in `Common/Util/MessageRecall.cpp`; the route is in
`RestApiMessageRecall.cpp`.

What is recorded, and what is not
---------------------------------

A message this server accepted **from one of its own accounts** gets, for
every recipient that resolved to a **local account** when it was accepted, one
row in `hm_recall` (schema 6085): the sender's account id, the message's
`Message-ID` without its angle brackets, the recipient's account id, the queue
row the message was accepted as, the recipient's own message row once the copy
is delivered, when it was recorded, and - later - when and how it was recalled.

"From one of its own accounts" means one of two doors:

* an **authenticated SMTP submission**, credited to the account that signed
  in - the session's identity, never the envelope's; or
* the **webmail's send** (`POST /api/v1/me/messages`), which knows the account
  outright.

Nothing is recorded for:

* mail from outside;
* mail this server writes itself - reports, digests, quota warnings;
* an **unauthenticated submission** from a local IP range in a local account's
  name: the envelope sender there is a claim, and a recall keyed on a claim
  would let anybody delete mail from other people's mailboxes by writing an
  address into `MAIL FROM`;
* a message with a **`Message-ID` longer than 255 characters**: it is not
  recallable, and the debug log says so at acceptance. A message with no
  `Message-ID` would not be either, but there is none in practice: a
  submission without one is given one at acceptance (`SMTPMessageHeaderCreator`,
  as RFC 6409 lets a submission server), and the webmail always writes one;
* a message that expires (a confidential message): it has *Withdraw* of its
  own, and what sits in the queue for it is the notice, not the message;
* the 201st and later recipient rows of one sender in a rolling day: **at most
  200 recipient rows per sender per day** are recorded, because a bulk
  sender's rows are not what this is for. Past that nothing more is recorded
  for the day, and the application log says so once per sender per day.

A recipient reached by expansion after `RCPT TO` - a distribution list
member, an alias target - gets its row at delivery, when rows for **the same
queue message** already exist and the delivered file carries the rows'
`Message-ID`: that is how a member's copy is tied to its sender without
trusting the `From` header, and without trusting the `Message-ID` alone,
since a `Message-ID` is not a secret and anybody may write one. The
`Message-ID` check is what guards a database that hands out an `hm_messages`
id again after a restart (MySQL 5 recomputes `AUTO_INCREMENT` from what is
left): an outsider's message accepted under a queue id whose rows are still
within `RecallDays` is not tied to them. (The recipient parser resolves lists
and aliases at `RCPT TO`, so in practice the rows are written at acceptance
and delivery only fills in the recipient's message row.)

**A copy the reader makes within their own mailbox is followed.** Every move
a reader can make is copy-and-delete - IMAP `MOVE` and `COPY`, the webmail's
move, Archive, Junk and Trash - and gives the copy a new `hm_messages` id.
`PersistentMessage::CopyToIMAPFolder` marks the copy with the id it came
from, and the save that assigns the new id writes a row beside the old one
(same sender, `Message-ID`, recipient and queue row), so a recipient may hold
several rows and the recall finds the message wherever they filed it. A copy
into another account's folder - a shared folder, a delegated mailbox - is
that account's and gets no row.

The recall
----------

`POST /api/v1/me/recall` with `{"message_id": "<the Message-ID>", "sent_id":
n}` - `sent_id` optional, the id of the copy in the Sent folder. Self-service:
the signed-in account's own rows only. Refused with 403 to a request that came
in on an app password (the webmail does not offer Recall to such a session),
and counted against the credential's request rate as every request to the
API is, reads included.

The rows for (this account, that `Message-ID`) not older than `RecallDays` are
read and judged **per recipient**, since a recipient who filed or copied the
message holds several rows:

| Outcome    | What it means, and what was done                                                                                                                                                                                                                                                                                                                                                                       |
|------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `read`     | Some live copy of theirs carries `\Seen`. Nothing was done: whoever has read it keeps it, every copy.                                                                                                                                                                                                                                                                                                  |
| `recalled` | No copy of theirs was read and at least one unread copy was there; every unread copy has been deleted **the way the webmail deletes a message** - through the same `DeleteOwnMessage_` the page's delete route calls, so an IMAP session with the folder selected sees the `EXPUNGE`, the recover-deleted store gets its copy while the server-wide `recover_deleted_days` is on, and the change notifications fire. |
| `gone`     | No copy of theirs is there any more - deleted by the reader, never delivered (a rule dropped it, or it was still in the queue when the recall ran), a copy whose file carries another `Message-ID`, or the recipient account itself is gone (its `address` is then empty in the answer). Nothing was done.                                                                                              |
| `failed`   | The database did not answer while a copy was being looked at, or a delete could not be made. Nothing of that recipient's is marked, and a later call decides them again.                                                                                                                                                                                                                                 |

The answer is `{"message_id", "recalled", "read", "gone", "failed",
"elsewhere", "sent_marked", "recipients": [{"address", "outcome"}]}`.
**404** when the account has no rows for that id within `RecallDays` - which
is also the answer for somebody else's message, since the rows are keyed by
the sender.

A recall is **idempotent**: rows decided stay, marked with their outcome, and
a second call answers the same outcomes. A row answered `read` stays `read`
even if the reader later marks the message unread; the recall was asked and
answered once. Two exceptions, both in the recipient's favour: a copy still
in the queue when the recall runs is answered `gone` but its row is left
unmarked, so once the delivery has filled it in the next call decides it; and
a recipient answered `failed` is left entirely unmarked, so the next call
tries again. Two recalls of one message at once agree: when the second finds
the copy already taken it reads the mark the first left and answers that.

`elsewhere` is how many distinct addresses the Sent copy names in `To` and
`Cc` whose domain this server does not host (after the domain aliases), when
`sent_id` is given and is the caller's. A recipient elsewhere is not in the
table at all: this server never reached them and cannot. `Bcc` is not in the
Sent copy's headers and is not counted. When at least one copy was recalled,
the Sent copy gets the keyword `$Recalled`, and `sent_marked` says whether it
carries it.

**Read is `\Seen` and nothing more.** A client that fetched the message
without marking it seen - a POP3 client, an IMAP client that peeks, a client
whose preview pane does not set the flag - has the message, and the recall
cannot know. This is exactly the honesty Exchange's recall lacks, and the
page's confirm sentence says it: *whoever has read it keeps it, and nobody
outside this server is reached.*

How long
--------

`RecallDays` in `hm_settings` - `recall_days` under `PUT /api/v1/settings` -
is 14 by default, 1 to 365. Rows older than that are swept by the minute task
(`ScheduledMailTask`, beside the expiry sweeps of the other kept things) and by
the retention task (the one `Utilities.RunMessageRetention` runs on demand).
A recall of a message older than that answers 404 as if it had never been
recorded, whether or not the sweep has run yet.

The page
--------

In the webmail, a message in the folder designated `\Sent` whose `From` is
the account or one of its identities and which is younger than `RecallDays`
(the page learns the number from `GET /api/v1/me` as `recall_days`) offers
**Recall** in its row menu and in the open message's *More* menu - unless the
session came in on an app password, which `GET /api/v1/me` also says, in
which case the item is not offered rather than refused. The confirm
sentence: *Recall this message from the people on this server who have not
read it? Whoever has read it keeps it, and nobody outside this server is
reached.* The toast afterwards: *Recalled from 2, already read by 1, not on
this server: 1*, with the number already gone appended when there is one. The
Sent row shows a **Recalled** badge from the keyword. The strings are in the
23 webmail catalogues.

What it is not
--------------

* It does not reach another server. A recipient at another domain - even one
  this server relays for - has the message, and `elsewhere` counts them so
  the sender knows.
* It does not know who has read what beyond `\Seen`.
* It does not send a notice. The recipients whose copies went learn nothing
  from this server; the sender's Sent copy carries the mark.
* It does not recall a message the reader already deleted, printed or
  photographed. A reader who forwarded it unread loses their own copy and
  keeps nothing; the forwarded copy is out of reach.
* It does not recall a message submitted without authentication, or by a
  program using an app password (the sending is allowed; the recalling is the
  person's own act).
* It does not touch the queue: a copy not yet delivered when the recall runs
  is answered `gone` and is delivered afterwards, its row left unmarked so
  that the next call reaches it. The queue is seconds long on a healthy
  server; a message held for review is the one case where it is not.

The rows and the settings
-------------------------

`hm_recall`: `recallid`, `recallsenderid` (foreign key to `hm_accounts`, the
row goes with the sender), `recallmsgid` (the `Message-ID`, at most 255
characters), `recallrecipientid`, `recallqueueid`, `recallmessageid` (0 until
delivered), `recallcreated`, `recallrecalledat` (0 until recalled) and
`recalloutcome` (0 pending, 1 recalled, 2 read, 3 gone). Indexed by sender,
by queue id (a local delivery costs one select and nothing more for a message
nobody recorded), by message id (a copy within a mailbox costs one select)
and by creation time (the minute sweep).

`RecallDays`: `hm_settings`, 14, 1 to 365; `recall_days` over REST. There is
no COM property and no Control Panel page: the feature is the person's own,
and the one number an administrator sets is on the settings route with the
other webmail settings.

Tests
-----

`RegressionTests/API/RestApiMessageRecall.cs`, REST port 9806, fifteen tests,
each with its negative control: a webmail send to two local accounts and one
outside address recalled from both unread copies (POP3 counts, the `EXPUNGE`
an IMAP session with the folder selected sees, `elsewhere` 1, the `$Recalled`
keyword, a second call answering the same with `sent_marked` still true), a
copy marked `\Seen` over IMAP staying and answered `read` while the other
goes, a copy deleted before the recall answered `gone`, a copy moved with
IMAP `MOVE` recalled from where it was filed and a copy made with `COPY`
recalled twice over while a read recipient's two copies both stay, an
authenticated SMTP submission recalled by the `Message-ID` the client wrote,
another account's recall of that id answered 404 with nothing deleted, an
unauthenticated local-range submission in a local account's name not
recorded, an authenticated session whose `MAIL FROM` names another local
account credited to the login, a submission without a `Message-ID` given one
at acceptance and recalled by it while one over 255 characters is not
recorded (the log line, the 400), the 200-per-day ceiling (the
send that crosses it recorded up to the line, the next not at all, the log
saying so once), a distribution list's two members both recalled, a recalled
copy in the recipient's recover-deleted list and put back, an address at a
domain alias counted as here, rows older than `RecallDays` swept by the
retention task and answered 404 while the same rows without the sweep answer
200, and an app password refused with 403. The page's part is in
`build/portal-script-test.js`: Recall offered only in the folder designated
`\Sent`, only for an own `From` within `recall_days`, hidden without a
`Message-ID` and on a session that came in on an app password, the confirm
sentence, the POST's path and body, the toast per outcome and the badge.

Not proved by a test, and said here so nobody believes otherwise: that a
message that expires is not recorded (the code skips it before `Record`);
and the `failed` outcome, which needs a database that stops answering
between two statements.
