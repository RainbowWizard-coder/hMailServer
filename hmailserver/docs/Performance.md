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
after about two and a half seconds of work, answering where to go on from;
the page's *Search older messages* carries on. With the full-text index on
(`IndexerFullText`, off by default) a message the index has read whole and which
cannot hold the words is walked past without its file being opened and without
counting against the 2,000 - the same filter IMAP `SEARCH` trusts, which only
ever rules a message out - so an indexed folder is searched as far as the index
can be asked in two and a half seconds. The index is asked about the messages the
search walks past, two hundred to a query, before any of them is read, and what
it leaves is then read under the reading's own clock.
Until 7 October 2026 it was asked first about the whole account - its complete
messages, then every one of its terms that held the word, which a database can
only find by reading them all - and on SQL Server Compact with the hundred
thousand below that took 75 to 180 seconds a request, outside the clock and
holding the database's one connection; the answer then came after the listener's
deadline had passed and was cut at 64 KB (both fixed; `CHANGELOG.md`). On SQL
Server Compact a lookup costs some 0.2 ms a message, about what reading a small
message from a fast local disk does, so there the index spares reads without
making a search much faster; it pays where reading a message is slow - object
storage, a network share. Each answer says how many message files it opened
(`opened`), of the ones it walked past (`scanned`). The search of every folder
used to go folder by folder and stop at 2,000: with a hundred thousand in the
inbox it never reached another folder, and it had no way to go on. It now walks
the whole mailbox newest first by message id and answers `next_before_id`, and
the page continues from it.

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
for `self@uicheck.test`, the account below, are 1,322,787,827 bytes sent, which is
what `--dry-run --user self@uicheck.test` reports for them (the address is in every
message's `To:`, so another account's are another size); what the server stores is
read from it afterwards. The word
`quokka` is in the body of every 1,000th message (100 hits) and `invoice` in the
subject of every 100th (1,000 hits).

`build/perf/webmail-timing.py` times the four operations two ways: `--mode http`,
the REST calls the page makes, timed from the client with nothing else in the
way; and `--mode browser`, the page itself in headless Chromium through
Playwright, from an action to the rows it asked for being drawn. Its header
lists exactly what each measure is. The HTTP select-all adds a keyword to every
message and takes it off again, so the folder is left as it was; the browser's
presses *Mark as read*, which leaves the folder read, after which a second
*Mark as read* changes nothing ("a message that already has the flag is left alone
and costs nothing"). So the browser's select-all runs once, last, with the index
on - the index takes no part in a flag change - and the index-off pass leaves it
out (`--no-bulk`). The browser runs with the account's inbox in one list, its
`inbox` preference set for the run and put back afterwards: in tabs, an account's
default, the box selects the open tab alone, and *Mark as read* would change the
Primary tab's messages, three in five of the folder at seed 49, not the folder's.
It signs in again to put the preference back when its first session has ended
while the browser ran, and says so on standard error when it cannot.

Record with the numbers: the machine, the database (SQL Server Compact, MySQL,
PostgreSQL or SQL Server), the message store (disk, encrypted, object store),
whether the full-text index was on and had caught up, the server's logging (off,
the shipped default), and the build.

Running it
----------

Not in CI: it needs a running server with a seeded folder, and a hundred
thousand messages take a while to make and to walk.

**On the Windows bench**, from the root of a checkout of the build to be
measured, with the service running that build and nothing else running on it: no
pipeline job and no regression gate, whose setup also deletes every domain, the
seeded one with it. Every line runs as written in Windows PowerShell 5.1 or in
PowerShell 7, as an administrator:

```powershell
# Python, pinned and outside the PATH: a python.exe on the machine's PATH changes
# which tests the bench's next regression gate runs (ContinuousIntegration.md). The
# Python Software Foundation's own package on nuget.org, at the version and SHA-256
# libraries\get-libpq-tools.ps1 pins, unpacked into a folder of its own; nothing is
# installed. Then, for --mode browser, Playwright, pinned with every file's hash,
# and its Chromium.
$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force C:\perf\results | Out-Null
$nupkg = "$env:TEMP\python.3.14.7.zip"
Invoke-WebRequest -UseBasicParsing https://api.nuget.org/v3-flatcontainer/python/3.14.7/python.3.14.7.nupkg -OutFile $nupkg
if ((Get-FileHash $nupkg -Algorithm SHA256).Hash -ne '46a4da5529a92d18ff894911f6e6033a8253198d705b8161bf28c9123c87d46b') { throw 'not the pinned Python' }
Expand-Archive $nupkg C:\perf\python-package; Move-Item C:\perf\python-package\tools C:\perf\python; Remove-Item $nupkg, C:\perf\python-package -Recurse
$py = 'C:\perf\python\python.exe'
& $py -m ensurepip --default-pip
& $py -m pip install --require-hashes -r build\requirements-dev.txt
& $py -m playwright install chromium

# The REST listener on 127.0.0.1:9530 and a throwaway account, as the README shots use.
powershell -NoProfile -ExecutionPolicy Bypass -File build\readme-shots\bench.ps1 -Up

# No quota on the account: a hundred thousand messages are 1.3 GB at seed 49. Logging
# off, the shipped default: a regression gate leaves every log on, debug included,
# and that would be timed with the server. What it was is written down first.
$app = New-Object -ComObject hMailServer.Application; $null = $app.Authenticate('Administrator', 'testar')
$acc = $app.Domains.ItemByName('uicheck.test').Accounts.ItemByAddress('self@uicheck.test'); $acc.MaxSize = 0; $acc.Save()
$app.Settings.Logging | Format-List Enabled, LogApplication, LogDebug, LogSMTP, LogPOP3, LogIMAP, LogTCPIP, AWStatsEnabled, KeepFilesOpen
$app.Settings.Logging.Enabled = $false

# The folder, over one connection. --connections 4 is faster, and the UIDs then no
# longer follow the dates exactly. The password comes from the environment.
$env:HMPERF_PASSWORD = 'Original-Passw0rd!'
& $py build\perf\seed-folder.py --user self@uicheck.test --count 100000 --seed 49

# Index off, the default. Cold: restart the service, so that the first open reads the
# folder from the database, and wait five minutes without opening it.
Restart-Service hMailServer; Start-Sleep -Seconds 300
& $py build\perf\webmail-timing.py --user self@uicheck.test --mode http `
    --label "bench, SQL CE, 100k, index off" --json C:\perf\results\perf-index-off-http.json --markdown C:\perf\results\perf-index-off-http.md
& $py build\perf\webmail-timing.py --user self@uicheck.test --mode browser --no-bulk `
    --label "bench, SQL CE, 100k, index off" --json C:\perf\results\perf-index-off-browser.json --markdown C:\perf\results\perf-index-off-browser.md

# Index on: message indexing and the full-text index. Run the probe until the JSON's
# http.search.body_word_in_folder says "complete": true, "hits": 100 and "opened": 100
# - the search followed to its end opened no message but the hundred that hold the
# word, so the index has read every other one (why: under the recipe). Not
# MessageIndexing.TotalIndexedCount: that counts the metadata index, which reaches the
# folder's size long before the full-text index has read it.
$app = New-Object -ComObject hMailServer.Application; $null = $app.Authenticate('Administrator', 'testar')
$app.Settings.MessageIndexing.Enabled = $true; $app.Settings.SetIniSetting('IndexerFullText', '1')
& $py build\perf\webmail-timing.py --user self@uicheck.test --mode http --no-bulk --repeat 0 --pages 1 `
    --json C:\perf\results\probe.json --markdown C:\perf\results\probe.md

# Then cold again, and the browser's select-all last: it leaves the folder read.
Restart-Service hMailServer; Start-Sleep -Seconds 300
& $py build\perf\webmail-timing.py --user self@uicheck.test --mode http `
    --label "bench, SQL CE, 100k, index on" --json C:\perf\results\perf-index-on-http.json --markdown C:\perf\results\perf-index-on-http.md
& $py build\perf\webmail-timing.py --user self@uicheck.test --mode browser `
    --label "bench, SQL CE, 100k, index on" --json C:\perf\results\perf-index-on-browser.json --markdown C:\perf\results\perf-index-on-browser.md

# Put back: the index off and emptied; the domain, its messages and the listener gone.
$app = New-Object -ComObject hMailServer.Application; $null = $app.Authenticate('Administrator', 'testar')
$app.Settings.DeleteIniSetting('IndexerFullText'); $app.Settings.MessageIndexing.Enabled = $false
$app.Settings.MessageIndexing.Clear()
powershell -NoProfile -ExecutionPolicy Bypass -File build\readme-shots\bench.ps1 -Down
```

Why the five minutes: with the full-text index on, the indexer's start walks the
message table for orphaned index rows, right after a restart, and the first open
would share the database with it. Both passes wait the same, and the folder is
still read cold, on its first use. The logging may stay off afterwards: the next
gate's setup switches it on itself.

Why `opened`: a search opens a message's file only when the index cannot rule the
message out - when the index has not read it yet, or when its words are there. So
a search for `quokka`, followed to its end, that opened exactly its hundred hits
has had every other message of the folder ruled out by the index, which is the
index having caught up. Until 7 October 2026 the probe asked for one search of the
folder answered in one request instead, which a caught-up index gave because each
request asked the whole account's index first; a request now asks it two hundred
messages at a time under its own clock, so on SQL Server Compact a hundred
thousand take several requests whether the index has caught up or not.

**On Linux** the same two scripts run unchanged against a server with the REST
listener on, with the same `HMPERF_PASSWORD`: `--base https://host:port --insecure`
for a listener with its own certificate, `--port 993 --tls --insecure` for IMAPS.
`GET /api/v1/settings/indexing` and `PUT` of `{"enabled": true}` there, and
`PUT /api/v1/settings/ini/IndexerFullText` with `{"value": "1"}`, do what the COM
lines above do.

`seed-folder.py --dry-run` makes the messages without connecting and says how
many bytes a run would upload. A run stopped part-way is resumed with `--start`
and the number it printed.

Results
-------

Windows bench, SQL Server Compact, message store on disk, 100,000 messages in
the inbox (seed 49), logging off. The rows are the ones `webmail-timing.py
--markdown` writes, in its order; it writes one *Result* column a run, so each
column here is copied, cell by cell, from that pass's two tables, HTTP and
browser. The first time is the first after a restart, as the recipe runs it. The
browser's select-all is measured once, with the index on; its index-off cell is
"-".

| Operation | Mode | Measure | Full-text index off | Full-text index on |
|---|---|---|---|---|
| Open | HTTP | first page, first time (ms) | to be measured on the bench | to be measured on the bench |
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
| Select-all + flag | Browser | Mark as read over the folder, wall (ms) | - | to be measured on the bench |

Seeding itself, for the record: messages a second over one connection
(`per_second` in `seed-folder.py`'s summary), and the bytes stored, read from the
server afterwards (`(Get-ChildItem <DataFolder>\uicheck.test -Recurse -File |
Measure-Object Length -Sum).Sum`, the `DataFolder` of `hMailServer.ini`,
`C:\HMTest\Data` on bench150) - *to be measured on the bench*.
