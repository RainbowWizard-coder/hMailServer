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
* a message with **no `Message-ID`**, or one longer than 255 characters: it is
  not recallable, and the debug log says so at acceptance;
* a message that expires (a confidential message): it has *Withdraw* of its
  own, and what sits in the queue for it is the notice, not the message;
* the 201st and later recipient rows of one sender in a rolling day: **at most
  200 recipient rows per sender per day** are recorded, because a bulk
  sender's rows are not what this is for. Past that nothing more is recorded
  for the day, and the application log says so once per sender per day.

A recipient reached by expansion after `RCPT TO` - a distribution list
member, an alias target - gets its row at delivery, when rows for **the same
queue message** already exist: that is how a member's copy is tied to its
sender without trusting the `From` header, and without trusting the
`Message-ID` either, since a `Message-ID` is not a secret and anybody may write
one. (The recipient parser resolves lists and aliases at `RCPT TO`, so in
practice the rows are written at acceptance and delivery only fills in the
recipient's message row.)

The recall
----------

`POST /api/v1/me/recall` with `{"message_id": "<the Message-ID>", "sent_id":
n}` - `sent_id` optional, the id of the copy in the Sent folder. Self-service:
the signed-in account's own rows only. Refused with 403 to a request that came
in on an app password, and rate-limited like every other write of the
self-service API.

The rows for (this account, that `Message-ID`) not older than `RecallDays` are
read. For each recipient copy on this server:

| Outcome    | What it means, and what was done                                                                                                                                                                                                                                                                                                       |
|------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `gone`     | The recipient's message row is no longer there - deleted by the reader, never delivered (a rule dropped it, or it was still in the queue when the recall ran), moved into a folder another account owns, or the recipient account itself is gone. Nothing was done.                                                                       |
| `read`     | The copy carries `\Seen`. Nothing was done: whoever has read it keeps it.                                                                                                                                                                                                                                                              |
| `recalled` | The copy was unread and has been deleted **the way the webmail deletes a message** - through the folder's own collection, so an IMAP session with the folder selected sees the `EXPUNGE`, the recover-deleted store gets its copy where that account has the window on, and the change notifications fire. The copy is found wherever the reader filed it. |
| `failed`   | The delete itself failed (the database did not answer). The row is not marked, and a later call tries again.                                                                                                                                                                                                                             |

The answer is `{"message_id", "recalled", "read", "gone", "failed",
"elsewhere", "sent_marked", "recipients": [{"address", "outcome"}]}`.
**404** when the account has no rows for that id within `RecallDays` - which
is also the answer for somebody else's message, since the rows are keyed by
the sender.

A recall is **idempotent**: rows recalled stay, marked with their outcome, and
a second call answers the same outcomes. A row answered `read` stays `read`
even if the reader later marks the message unread; the recall was asked and
answered once.

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
cannot know. A copy the reader made with IMAP `COPY` is a second row the
table never saw, and stays. This is exactly the honesty Exchange's recall
lacks, and the page's confirm sentence says it: *whoever has read it keeps it,
and nobody outside this server is reached.*

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
**Recall** in its row menu and in the open message's *More* menu. The confirm
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
* It does not recall a message the reader already deleted, copied, forwarded,
  printed or photographed.
* It does not recall a message submitted without authentication, or by a
  program using an app password (the sending is allowed; the recalling is the
  person's own act).
* It does not touch the queue: a copy not yet delivered when the recall runs
  is answered `gone` and is delivered afterwards. The queue is seconds long
  on a healthy server; a message held for review is the one case where it is
  not, and a recall before its release does not reach it.

The rows and the settings
-------------------------

`hm_recall`: `recallid`, `recallsenderid` (foreign key to `hm_accounts`, the
row goes with the sender), `recallmsgid` (the `Message-ID`, at most 255
characters), `recallrecipientid`, `recallqueueid`, `recallmessageid` (0 until
delivered), `recallcreated`, `recallrecalledat` (0 until recalled) and
`recalloutcome` (0 pending, 1 recalled, 2 read, 3 gone). Indexed by sender and
by queue id; a local delivery costs one select on the queue id and nothing
more for a message nobody recorded.

`RecallDays`: `hm_settings`, 14, 1 to 365; `recall_days` over REST. There is
no COM property and no Control Panel page: the feature is the person's own,
and the one number an administrator sets is on the settings route with the
other webmail settings.

Tests
-----

`RegressionTests/API/RestApiMessageRecall.cs`, REST port 9806: a webmail send
to two local accounts and one outside address recalled from both unread
copies (POP3 counts, the `EXPUNGE` an IMAP session with the folder selected
sees, `elsewhere` 1, the `$Recalled` keyword, a second call answering the
same), a copy marked `\Seen` over IMAP staying and answered `read` while the
other goes, an authenticated SMTP submission recalled by the `Message-ID` the
client wrote, another account's recall of that id answered 404 with nothing
deleted, an unauthenticated local-range submission in a local account's name
not recorded, a distribution list's two members both recalled, rows older than
`RecallDays` swept by the retention task and answered 404 while the same rows
without the sweep answer 200, and an app password refused with 403.
