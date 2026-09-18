Offline mail in the webmail
===========================

Everything this page describes is the first account's - the one the webmail signed in as. A second account added beside it ([SecondAccount.md](SecondAccount.md)) never enters the store, and the Settings card says so while that account is shown.

Gmail's *offline mail*: the webmail keeps the mail of the last days in the browser's
own storage, so a person can read and search it - text, formatting and attachments -
with no connection, and a message written then is sent when one is back.

The webmail has always kept two things for a connection that goes: the inbox listing
and the last thirty-one opened messages (in `localStorage`), and an outbox of sends
that failed for want of a connection. This adds the third thing, and it is the
reader's choice, under a ceiling the administrator sets.

What the reader chooses
-----------------------

Settings, *Offline in this browser*: **Keep for reading offline** offers *Nothing beyond
the inbox listing* and then the steps - the last 7, 14, 30, 60, 90, 180 or 365 days -
up to the server's ceiling, with the ceiling itself when it falls between steps. The
choice is the account's `offline_days` preference, so it follows the person between
browsers; what is kept does not.

Nothing is kept until the reader chooses. A browser someone else can open should not fill
itself with a mailbox unasked, and Gmail's own offline mail is opt-in for the same reason.
The card says so plainly: *it lives in this browser and nowhere else; anyone who can open
this browser can read it, and signing out removes it.*

What the administrator sets
---------------------------

`reader_offline_days_max` on `PUT /api/v1/settings` (the `ReaderOfflineDaysMax` row of
`hm_settings`): the most days a reader may choose. 30 by default; at most 365; 0 offers
none, and the card then says the administrator has switched it off, and that the listing
and the last opened messages are still kept as before. A ceiling lowered later clamps a
choice already made.

How it fills
------------

The store is IndexedDB, database `hmPortalOffline`, four object stores:

| Store | What it holds |
|---|---|
| `rows` | For each kept message: the row a listing draws (subject, sender, recipients, date, flags, snippet, whether it has attachments), the folder, the received time, its size, and the lowercased words a search looks through. Read whole for a folder listing or a search; small. |
| `messages` | The message's JSON as `GET /api/v1/me/messages/{id}` answered it - its text and HTML, its attachment list - and the keys of its parts. |
| `parts` | `<id>/html`, the HTML document the server renders for the message (its `cid:` images inlined), and `<id>/<n>`, each attachment, as blobs with their media types. An attachment above 8 MB is not kept, and a message's parts stop at 32 MB together. |
| `meta` | The folder tree as the server last listed it. |

A message goes in when it is **opened** and the window covers it, and in the
**background**: after sign-in, after a choice is saved, whenever the connection comes
back, on the next visit to Settings, and a few seconds after the change probe reports a
change (mail that arrived, a move or a delete made here or from another client - one
walk for a burst of them), the page asks `GET /api/v1/me/offline/messages`
for the window - every message received since it opened, in every folder the account may
read, by id from the oldest up, paged by `after_id` in pages of 500 - and then drops what
the window no longer holds (older than it, moved, deleted), brings a flag or a folder that
moved on the server into step, and fetches the bodies still missing newest first, twenty-five
at a time with a pause between slices, each through the ordinary message route with its
parts. The listing route reads no file and copies no message: the id, folder, size, received
time and flags are read off the folders the server already holds in memory, one pass per
page, holding no more than the 501 rows a page may need (a heap of the smallest ids past
`after_id`, one over the page to know whether there is more), so a walk of a mailbox costs
the server that pass and nothing else, however many messages the window has. The whole
window's count and size in bytes come back on every page.

The page acts only on an answer that really carries the choice. A window the server
could not read - the database gone for a moment - is a 500, never *nothing chosen*, and
the page leaves what it keeps as it is; a listing that says nothing is chosen while the
page holds a choice is not pruned by either. Emptying the store, which *nothing chosen*
does, is for the reader's decision and not for a failed query.

Before each body the page asks `navigator.storage.estimate()` whether it will fit; when the
window is bigger than what the browser allows the site, the card says so - *the last 30
days are about 1.8 GB and this browser has room for about 900 MB: the newest are kept and
the rest are not* - and the run stops keeping rather than failing. The card also counts:
*412 of the 480 messages of the last 30 days are kept here, 610 MB. This browser allows this
site about 2.0 GB.*

What works without a connection
-------------------------------

- **A message** opens from the store, with the note *No connection* over the page. Its
  HTML body is a frame whose source is the server's `/html` route, and each attachment is a
  link to its route; the service worker tries the network first and, when that fails,
  answers both from the `parts` store under the headers the server sends them with - the
  HTML under the same `Content-Security-Policy` (nothing runs, nothing remote loads), an
  attachment as a download under `sandbox`. A part it does not hold answers 503 rather than
  something made up.
- **A folder** lists what the store holds for it, newest first, conversations grouped as
  usual.
- **A search** runs over the kept words: subject, sender, recipients and the body text
  (the words of the HTML when there is no plain part). The operators the store can answer
  work - `from:`, `to:`, `subject:`, `in:`, `label:`, `category:`, `has:attachment`,
  `is:unread`, `is:read`, `is:flagged`, `is:unflagged`, `is:answered`, a quoted phrase,
  `-word`; `larger:` and `smaller:` against the kept size (`750`, `10k`, `2M`, `1G`);
  `before:` and `after:` (a `YYYY-MM-DD` day) and `older_than:` and `newer_than:` (`7d`,
  `2w`, `3m`, `1y`) against the received time, read as the server reads them but on this
  browser's clock; `filename:` against the names of the kept attachments - and an operator
  it cannot (`has:link`, `in_reply_to:`), or a value that is not one, lets a message through
  rather than hiding it. `OR` is ignored. The note under the list says how many messages
  were searched.
- **A page opened with no connection** - the browser had the tab closed, or the page was
  reloaded - shows the kept mail of the account that was last signed in here, with its
  folders, and no session is asked for: the mail is already in this browser. When the
  connection is back the page asks for the account as it does at every start: the moment the
  browser says it is online, and otherwise every thirty seconds, since a listener that is
  restarting or a proxy that is down fails the request exactly as a network that went does
  while the browser was online all along and no online event will ever come for it. A
  session that has ended puts the sign-in gate up, which forgets everything.
- **A message written** then goes into the outbox as before, and is sent when the
  connection is back.

What it does not do
-------------------

- It does not keep anything until the reader chooses to, and the default is nothing.
- It keeps no message the server cannot parse into a body (`truncated`), no attachment above
  8 MB, and no more than 32 MB of parts for one message; such a message is kept with what
  fits, and the missing part answers 503 offline.
- It does not answer `has:link` or `in_reply_to:` offline, and it does not thread `OR`. A
  date operator is measured on the browser's clock, not the server's; a message kept before
  this version carries no attachment names, so `filename:` lets it through.
- It does not survive sign-out: the database is deleted, and so is the name of whose mail
  it held. A session that ends while the page is open does the same.
- It is not encrypted beyond what the browser does with its own storage. Anyone who can
  open this browser profile can read what is kept; the card says so, and the choice to keep
  is the reader's for that reason.
- It is not a backup, and it is not synchronised back: a flag set offline is not sent when
  the connection returns (the outbox carries sends, not flag changes).
- It does not keep mail for a shared or public folder the account may read but does not
  own any differently from its own: what the listing route lists, the store keeps.

Clear
-----

**Clear what is kept** on the card deletes every kept message and part now and sets the
choice to nothing, so the store does not fill straight back up; the reader chooses again
when they want it back.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/offline` | `{days_max, days, since}`: the ceiling, the reader's choice under it (clamped), and the moment the window opens at in the form a listing's `received` is written in; `since` is empty when `days` is 0. |
| `PUT /api/v1/me/offline` | Body `{days}`, 0 to `days_max`; writes the `offline_days` preference; answers as GET does; 400 above the ceiling or for anything but a whole number, and 400 *at most 100 preferences* - the preferences route's own answer - when the account's hundred are all taken by other keys. |
| `GET /api/v1/me/offline/messages?after_id=&limit=` | `{days, since, count, bytes, messages:[{id, folder_id, uid, size, received, flags}], more, next_after_id}`: the window, oldest id first, at most 500 a page; `count` and `bytes` are the whole window's. Empty, with `days` 0, when nothing is chosen. |

All three answer 500 when the choice could not be read, and PUT when it could not be
saved; none of them answers a failed read as a choice of nothing.

The setting is `reader_offline_days_max` in the settings group (`ReaderOfflineDaysMax` in
`hm_settings`). No table was added: the mail lives in the browser and nowhere else on this
account of it.

Proof
-----

`API/RestApiOffline.cs` (port 9641; in the Windows suite and linked into the Linux one,
which exercises the route's POSIX clock and the createtime comparison against that
database) proves the choice under the ceiling - refused above it, clamped when the ceiling
falls, nothing under a ceiling of 0, the preference it is written as - the listing: what
was received within the window in two folders, oldest id first, paged by `after_id` and
`limit` with the whole window's count, and not a message received in 2020, not one marked
deleted, and not another account's - and the cap: an account whose hundred preferences are
all taken is refused the choice with the preferences route's 400 and words, a slot freed
lets it in, and a new choice once among the hundred takes no new slot.
`build/portal-script-test.js` runs the page against an in-memory IndexedDB, thirty-seven
checks: the card's steps under the ceiling, the choice saved, the walk and the bodies
fetched newest first with their HTML document and attachments, the folder tree kept, the
counts and the room note; a window the server could not read (500) leaving the store, the
owner and the choice as they were, and a listing that says nothing is chosen while the
page holds a choice dropping nothing; a message and a folder and six searches answered
with the network down (words, `-word`, `is:` and `has:`, then a size, a day, a span and a
file name each way, and an operator the store cannot answer letting a message through),
the service worker answering an attachment and the HTML document under the server's
headers and 503 for a part it does not hold; the page asking for the account again with
the connection gone the moment before (the same `load()` a cold open takes, driven through
the reload a password change makes) and showing the kept mail under the kept folders with
no session asked for, a folder and a message read from the store, the account asked for
again on a timer while the server stays out of reach and the kept message left on the
page, then the account, its folders and the window asked for again the moment the
connection is back; the same cut with the network up throughout, brought back by the
timer alone with no online event; sign-out removing the database and whose it was, a new
sign-in filling it again from the choice the account carries, a change the probe reports
arming one walk for a few seconds later and that walk fetching the arrival into the store;
Clear; and a ceiling of 0.
