The webmail at a hundred thousand messages
==========================================

What the webmail does with one folder of a hundred thousand messages, measured
rather than assumed: how long it takes to open the folder, to reach its end, to
search it and to select all of it and change every message. This page is the
method, the tools that make and time such a folder, and the table the numbers go
in. **The table is filled in from the bench and from nowhere else**: a cell that
says *to be measured on the bench* has not been measured, and no number on this
page was estimated.

The four operations, and what each costs the server
---------------------------------------------------

Every path below reads the folder's shared message collection - the one IMAP,
POP3 and delivery use (`MessagesContainer`) - which holds one small object per
message and no message text. What changed on 25 September 2026 is how much of
it a request copies and how much of the store it reads.

**Opening a folder** - `GET /api/v1/me/folders/{id}/messages`, one page of 200.
It used to copy the whole folder for every page (a new object for each of the
hundred thousand messages, made under the lock that IMAP sessions and delivery
wait on) and walk the copy twice more, to show two hundred. Now one pass under
the lock counts the folder and copies only the page (`Messages::VisitNewestFirst`),
and each row's file head is read twice where it was read three times: Subject,
From and Date and the conversation headers now come from one read, and the
recipients, the tab and the snippet from a second (the snippet parses the whole
file), which is skipped for a row whose extras the server has kept. The listing
also names the folder's newest UID from the same pass (`newest_uid`), which is
what a select-all made from it means by every message. What still grows with
the folder: that one pass,
which touches each message's small object once; and the first open after a
server start, or after the folder has been idle in the cache for thirty minutes,
which reads every row of the folder from the database into the cache - what an
IMAP `SELECT` of the same folder does. The folder tree and the change probe the
page polls every few seconds count each folder the same way; they no longer
build, and throw away, a set of every message still carrying `\Recent` (after an
import that is every message).

**Scrolling** - the list is a page of 200 rows, and the page never holds more;
Older and Newer move a page at a time by UID (`before_uid`). A folder of three
pages or more now also offers **Oldest**, which opens the last page in one
request: the listing names the `before_uid` under which the folder's oldest page
lies (`oldest_before_uid`), found in the same pass. Every page costs what the
first costs, wherever it is in the folder.

**Searching** - `q` on a folder's listing, and `GET /api/v1/me/search` across
every folder. A request opens at most 2,000 message files and now also stops
after about two and a half seconds of reading, answering where to go on from;
the page's *Search older messages* carries on. With the full-text index on
(`IndexerFullText`, off by default) a message the index has read whole and which
cannot hold the words is walked past without its file being opened and without
counting against the 2,000 - the same filter IMAP `SEARCH` trusts, which only
ever rules a message out - so an indexed folder is searched as far as it has
candidates. The search of every folder used to go folder by folder and stop at
2,000: with a hundred thousand in the inbox it never reached another folder, and
it had no way to go on. It now walks the whole mailbox newest first by message
id and answers `next_before_id`, and the page continues from it.

**A change in what `GET /api/v1/me/search` answers**, for anything that reads it
besides the webmail: a search with more hits than `limit` used to answer
`complete: true, more: true` when it had read everything it was allowed to, with
no way to reach the hits it cut. It now stops at `limit` hits and one more and
answers `complete: false, more: true` with `next_before_id`, the `before_id` that
goes on from there; `complete` is false, with `next_before_id`, whenever the
answer is not the end of the search. The hits are newest first across every
folder, by message id, where they were folder by folder.

**Select-all and one action** - ticking the page's box offers every message in
the folder (or in the inbox tab that is open). Taken up, the page used to read
the folder into itself two hundred at a time, stop at five thousand whatever the
folder held, and then send one request per message. Now the bulk bar's *Mark as
read*, *Mark as unread*, *Archive*, *Junk*, *Delete* and *Move* send the
selection itself to `POST /api/v1/me/folders/{id}/bulk`: every message up to the
newest UID the listing on screen read when the reader chose (so mail arriving
meanwhile is not swept up), narrowed to the tab by the rule the page's tabs use. The server works
through it a step at a time - at most 1,000 messages a call for a flag, 500 for a
move or a delete, and no new message taken on after about two and a half
seconds of work - and each answer says how far it got; the bar counts *Done so
far* as it goes, and Undo after a move sends the moved messages back by the ids
the move gave them, a thousand to a request. The originals of a move or a delete
are expunged fifty at a time inside the same clock, so a moved message is held
twice - original and copy, both counted against the quota - for no longer than a
slice. Each message is changed exactly as its own route changes it: a flag the way `STORE` writes it (with the folder's next
mod-sequence, so CONDSTORE clients see it), a move as a copy and an expunge, a
delete into Trash unless it is final. A message that already has the flag is
left alone and costs nothing. `deleted: false` is refused with 400: the
selection is of messages not flagged deleted, as the listing is, so there would
be nothing to undelete; one message is undeleted by its own route. **A move into
or out of Junk teaches the spam filter one message at a time**, as the same move
over IMAP does: with SpamAssassin on, each message moved queues one lesson, so
Junk over a whole folder of a hundred thousand queues a hundred thousand. What
still grows with the folder: a changed flag is a row written, a move is a file
copied, and a tab-narrowed selection reads the head of every message to decide
its tab. A **quick step** over a whole-folder
selection still reads the folder into the page, at most five thousand messages,
as before.

Not changed, and not among the four: the Storage, Subscriptions and Scheduled
views still copy each folder they look at, and emptying a Junk or Trash folder
copies it once per 500 messages removed.

Method
------

One folder of 100,000 messages in one account, made by
`build/perf/seed-folder.py` over IMAP `APPEND` - the server's own way in, so the
files, rows, UIDs, quota and index are exactly what real mail makes (the script's
header says why not the database or the Maildir import). The messages are the
same on every run for the same `--seed`: senders shaped like an inbox (people,
newsletters, social notifications, machine senders, mailing lists, so every tab
has mail), dates evenly over five years, a fifth of them replies, 5 % with an
attachment of 20 to 512 KB, most older ones read. A hundred thousand at seed 49
are 1,323,987,827 bytes, which is what `--dry-run` reports for them. The word
`quokka` is in the body of every 1,000th message (100 hits) and `invoice` in the
subject of every 100th (1,000 hits).

`build/perf/webmail-timing.py` times the four operations two ways: `--mode http`,
the REST calls the page makes, timed from the client with nothing else in the
way; and `--mode browser`, the page itself in headless Chromium through
Playwright, from an action to the rows it asked for being drawn. Its header
lists exactly what each measure is. The HTTP select-all adds a keyword to every
message and takes it off again, so the folder is left as it was; the browser's
presses *Mark as read*, which leaves the folder read - run it last.

Record with the numbers: the machine, the database (SQL Server Compact, MySQL,
PostgreSQL or SQL Server), the message store (disk, encrypted, object store),
whether the full-text index was on and had caught up, and the build.

Running it
----------

Not in CI: it needs a running server with a seeded folder, and a hundred
thousand messages take a while to make and to walk.

**On the Windows bench**, with the service running the build to be measured:

```powershell
# The REST listener on 127.0.0.1:9530 and a throwaway account, as the README shots use.
powershell -NoProfile -File build\readme-shots\bench.ps1 -Up

# No quota on the account: a hundred thousand messages are 1.3 GB at seed 49.
$app = New-Object -ComObject hMailServer.Application; $null = $app.Authenticate('Administrator', 'testar')
$acc = $app.Domains.ItemByName('uicheck.test').Accounts.ItemByAddress('self@uicheck.test'); $acc.MaxSize = 0; $acc.Save()

# The folder. --connections 4 is faster, and the UIDs then no longer follow the
# dates exactly.
python build\perf\seed-folder.py --user self@uicheck.test --password Original-Passw0rd! --count 100000 --seed 49

# Cold: restart the service so the first open reads the folder from the database.
Restart-Service hMailServer

python build\perf\webmail-timing.py --user self@uicheck.test --password Original-Passw0rd! `
    --mode both --label "bench, SQL CE, 100k, index off" --json perf-index-off.json --markdown perf-index-off.md

# Again with the full-text index on, once the indexer has worked through the folder
# (IndexerFullText=1 and message indexing enabled; see the README's [Settings] list).

powershell -NoProfile -File build\readme-shots\bench.ps1 -Down
```

**On Linux** the same two scripts run unchanged against a server with the REST
listener on: `--base https://host:port --insecure` for a listener with its own
certificate, `--port 993 --tls --insecure` for IMAPS.

`seed-folder.py --dry-run` makes the messages without connecting and says how
many bytes a run would upload. A run stopped part-way is resumed with `--start`
and the number it printed.

Results
-------

Windows bench, SQL Server Compact, message store on disk, 100,000 messages in
the inbox (seed 49). The rows are the ones `webmail-timing.py --markdown` writes.

| Operation | Mode | Measure | Full-text index off | Full-text index on |
|---|---|---|---|---|
| Open | HTTP | first page, first time after a restart (ms) | to be measured on the bench | to be measured on the bench |
| Open | HTTP | first page, warm median (ms) | to be measured on the bench | to be measured on the bench |
| Open | HTTP | folder tree, warm median (ms) | to be measured on the bench | to be measured on the bench |
| Scroll | HTTP | Oldest page in one step (ms) | to be measured on the bench | to be measured on the bench |
| Scroll | HTTP | every page by Older: pages / total (ms) | to be measured on the bench | to be measured on the bench |
| Scroll | HTTP | one Older page, median / p95 (ms) | to be measured on the bench | to be measured on the bench |
| Search | HTTP | body word, folder: first answer (ms) | to be measured on the bench | to be measured on the bench |
| Search | HTTP | body word, folder: to the end - requests / total (ms) / hits | to be measured on the bench | to be measured on the bench |
| Search | HTTP | body word, every folder: first answer (ms) | to be measured on the bench | to be measured on the bench |
| Search | HTTP | body word, every folder: to the end - requests / total (ms) / hits | to be measured on the bench | to be measured on the bench |
| Search | HTTP | subject word, folder: first answer (ms) | to be measured on the bench | to be measured on the bench |
| Search | HTTP | subject word, folder: to the end - requests / total (ms) / hits | to be measured on the bench | to be measured on the bench |
| Select-all + flag | HTTP | keyword set: requests / wall (ms) / changed | to be measured on the bench | to be measured on the bench |
| Select-all + flag | HTTP | keyword cleared: requests / wall (ms) | to be measured on the bench | to be measured on the bench |
| Open | Browser | rows drawn, first time (ms) | to be measured on the bench | to be measured on the bench |
| Open | Browser | rows drawn, warm median (ms) | to be measured on the bench | to be measured on the bench |
| Scroll | Browser | list to its foot (ms) | to be measured on the bench | to be measured on the bench |
| Scroll | Browser | Oldest pressed to rows drawn (ms) | to be measured on the bench | to be measured on the bench |
| Scroll | Browser | one Older press, median (ms) | to be measured on the bench | to be measured on the bench |
| Search | Browser | first answer drawn (ms) | to be measured on the bench | to be measured on the bench |
| Search | Browser | to the end (ms) / continuations | to be measured on the bench | to be measured on the bench |
| Select-all + flag | Browser | Mark as read over the folder, wall (ms) | to be measured on the bench | to be measured on the bench |

Seeding itself, for the record: messages a second over one connection, and the
bytes stored - *to be measured on the bench*.
