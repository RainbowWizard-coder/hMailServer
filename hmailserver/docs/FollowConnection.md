# Follow one connection live

*Every line the server logs for one session, as it is written — what a grep through a
gigabyte of log for a session id stood in for.*

Built 17 September 2026. The page is the **Connections** section at the top of **Logs** in
the Control Deck; the routes are `GET /api/v1/logs/sessions` and
`GET /api/v1/logs/sessions/{id}`.

What it is
----------

Every SMTP, POP3 and IMAP conversation this server has is a *session* with a number, and
every line the log writes for that conversation carries the number — the `41` in
`"IMAPD"  5220  41  "2026-09-17 09:00:00.120"  "192.0.2.7"  "SENT: A1 OK LOGIN completed"`.
Until now the way to read one conversation was to note the number and search the day's log
for it, once the conversation was over.

The Logs page now opens with the sessions the server has open, newest first, and the last
two hundred that ended: the id, the protocol, the address it came from, the port it came in
on, the account once it has signed in, and when it started or when it ended. **Follow**
beside any of them opens a pane and puts every line the log holds for that session in it,
then keeps adding lines as the server writes them. **Copy** puts the whole pane on the
clipboard; **Stop following** ends it. A session that ended a second ago is still in the
list, so an SMTP conversation that lasted a quarter of a second can be picked and read.

How it works
------------

**The ring.** `Logger::Write_` is the one point every log entry passes through — it is where
the syslog sink and the OTLP log exporter hang — and it now also copies each entry into a
ring of the last 4,096 (`Common/Util/SessionFollow`). Each entry is kept with its sequence
number, its category, its time, its session and its message, the message cut at 2,048
characters. The ring is always on: there is no setting, because a ring that filled only
while somebody watched would have nothing to show for the seconds before they pressed
Follow. Its cost is one lock and one string copy per line the log mask lets through, and a
few megabytes of memory at most.

**Which lines are a session's.** The conversation lines — `SMTPD`, `SMTPC`, `POP3D`,
`POP3C`, `IMAPD` — carry the session id in their own column and are filed by it. The
`DEBUG` and `TCPIP` lines carry none; they name the session in their text instead
(`TCP connection started for session 41`, `TCPConnection - TLS/SSL handshake completed.
Session Id: 41, ...`, `Ending session 41`), so an entry with no session column is read
once, as it arrives, for the first number after the word *session*, and is filed under
that. A line that does neither is nobody's and is not shown.

A line matched by its text is an **inference**, not something the logger recorded, and the
number has to follow a space. That rule is not cosmetic. Plenty of lines with no session
column quote text a remote party chose — a DAV request target, an envelope sender, a user
name, the name of something somebody created — and without it the token `session41`
anywhere in one of them would file that line under session 41 and put a sentence of
somebody else's writing in front of an administrator debugging session 41. A space cannot
be put in a request target, and every line this server writes itself has one there. It is
a guess made safe rather than a guarantee: a `DEBUG` or `TCPIP` line in the pane is there
because its text said so, and the text is not always the server's own.

**The list.** A session is listed by `TCPConnection` itself: when the connection starts
(after a PROXY protocol header has been read, so the address is the client's and not the
proxy's), when it signs in (the three places IMAP, POP3 and SMTP record a sign-in), and
when it ends (the destructor, after its own `Ending session` line, so a follower reads
that line before the session is shown as ended). The client connections this server opens
— the delivery client, the POP3 and IMAP fetchers — are listed too, marked `client`, under
`SMTP client`, `POP3 client` and `IMAP client`.

It is every connection of that kind and not only the three mail protocols: a connection to
a SpamAssassin daemon is listed as `SpamAssassin` and the Control Panel's connection test
as `Diagnostics`, both short-lived and both with nothing but their debug lines to follow.
What is *not* here is anything that is not a `TCPConnection`: the anti-virus scanner talks
to `clamd` over a socket of its own, and the HTTP listener's requests are not sessions at
all.

A connection that never opens — a delivery to a host that refuses — is **not** in the
list, because the list is written where the connection succeeds. Its session id was
allocated all the same, and with debug logging on the ring holds the lines that name it
(`Pre-creating session 44`, `Ending session 44`), so following it by that id still answers
them, with `known: false`; there is just nothing in the page to click.

**A follow is a cursor, not a stream.** The HTTP listener (`HttpServer.cpp`) writes a
response whole; it cannot hold one open and append to it, which is what
`text/event-stream` would need. So the follow is a long poll:

```
GET /api/v1/logs/sessions/41?after=0&wait=10
```

answers the session's lines after sequence 0 — everything the ring still holds — and the
`cursor` to ask after next time. When there is no line yet and `wait` is given, the
request waits on the ring's condition variable for up to that many seconds and answers the
moment the next line lands. The lines still appear when they are written; what the page
pays is one request per batch of lines rather than one connection. The Deck asks with
`wait=10` in a loop.

The wait is bounded twice. Twenty seconds at most, under the listener's thirty-second
request deadline. And one waiting follow at a time, because a waiting follow holds one of
the listener's four worker threads for the length of its wait — and those four threads are
not the Deck's. The same listener and the same handler answer `/portal`, `/portal.js` and
every `/api/v1/me` route, which is the webmail of every mailbox on this server, so half
the pool held by a page somebody left open on a Follow loop would be half the webmail's
threads, and the route asks for nothing stronger than `view_logs` — which the built-in
read-only auditor role holds and a read-only API key resolves to. A quarter of the pool is
what a follow may take. A second follow is answered at once with `waited: false`, and the
Deck then pauses a second before asking again: it costs that follower a second of latency
and costs the mail nothing. When the
listener stops it wakes every waiting follow *and* stops granting waits until it starts
again, so neither a follow already waiting nor one that arrives in the instant after the
wake can hold the shutdown up for twenty seconds.

**Falling behind.** The ring holds 4,096 entries across every session. A debug-logged IMAP
client can fill it in minutes, so a follow whose cursor points before the oldest entry
still held answers `dropped: true` and starts from what is left; the page says so. An
answer carries at most 500 lines; a follower far behind catches up over several.

The routes
----------

Both are under the log's own permission, `view_logs`, and both are server-wide: a
session's lines are whichever domain's traffic it carried, so a credential confined to
some domains is refused them, as it is refused the log files.

`GET /api/v1/logs/sessions` answers

```json
{"sessions":[{"id":41,"port":143,"client":false,"open":true,"protocol":"IMAP",
  "peer":"192.0.2.7","account":"anna@example.com","started":"2026-09-17 09:00:00","ended":""},
  ...],
 "open":3,"ended":17,"truncated":false,"held_lines":4096,"logging_enabled":true}
```

— the open sessions newest first, then the ended ones newest first. `truncated` is true
when more than a thousand are open and only the newest thousand are listed. `held_lines`
is how many entries the ring holds now, and `logging_enabled` whether the log mask lets
anything through at all.

`GET /api/v1/logs/sessions/{id}?after=<sequence>&wait=<seconds>` answers

```json
{"session":41,"known":true,"open":true,"logged":true,"cursor":5133,"dropped":false,
 "waited":true,"protocol":"IMAP","peer":"192.0.2.7","account":"anna@example.com",
 "started":"2026-09-17 09:00:00","ended":"",
 "lines":[{"seq":5131,"time":"2026-09-17 09:00:02.410","category":"IMAPD","message":"RECEIVED: A7 NOOP"},
          {"seq":5133,"time":"2026-09-17 09:00:02.411","category":"IMAPD","message":"SENT: A7 OK NOOP completed"}]}
```

`known` is false for a session neither open nor among the last two hundred that ended; the
route is 404 when the session is unknown and no line is held for it either. `logged` says
whether the log mask lets this protocol's lines through — the one thing a follower of an
empty session most wants to know, and the page says which switch to turn on. `after` or
`wait` that is not a number is 400; a `wait` above twenty is twenty. A `wait` on a session
that has ended, or on one the server never heard of, is not honoured at all: the last line
of a session is written before it is listed as ended, so there is nothing left to wait
for, and a wait that could never end holds one of the listener's four worker threads for
nothing.

`known`, `open`, `account`, `peer`, `started` and `ended` are read *after* the wait, not
before it. A wait lasts up to twenty seconds and a session can sign in or end inside them;
an answer built before the wait would tell the page a session was open when it had gone,
and with debug logging off nothing wakes the waiter when a session ends, so that stale
answer would be the one that came back at the deadline and the page would carry it for a
whole round trip.

What it does not do
-------------------

* **Nothing survives a restart, and nothing past the ring.** The ring is memory. A line
  older than the last 4,096 entries is in the log files and nowhere else; the log-file
  tail beside it on the same page, and the syslog and OTLP exports, are the durable
  record. This is a window, not an archive.
* **It shows what the log mask lets through, and no more.** With IMAP logging off, an IMAP
  session is listed and its follow is empty; the page says so and names the switch. Debug
  logging is what makes the `TCPIP` and `DEBUG` lines — the TLS handshake, the timeouts,
  the disconnect — appear beside the conversation.
* **Only what goes through `TCPConnection`.** The REST API, the webmail and the DAV
  services run on the HTTP listener, ManageSieve on a server of its own, and the anti-virus
  scanner talks to `clamd` over a socket of its own: none of their log lines carries a
  session id, so none of them is a session here. The Logs page says "the SMTP, POP3 and
  IMAP sessions it answers, and the ones it opens itself", which is the whole of it — the
  `SpamAssassin` and `Diagnostics` connections above are part of *the ones it opens
  itself*, not a fourth protocol the page has left out.
* **The IMAP fetcher logs only through `DEBUG`**, so an `IMAP client` session shows the
  debug lines that name it and no conversation; the POP3 fetcher and the delivery client
  log their conversations and show them.
* **No event stream.** When the listener can hold a response open, this route can grow a
  `text/event-stream` form beside the cursor one; the ring and the wait would not change.
* **No Control Panel page and no `hmctl` verb.** The row named the Deck. The desktop
  program's *Live log* is the older, whole-log form of this over COM and is unchanged.
* **No filtering inside a session** — every line the session has is shown, in order.
* **The pane holds the last 5,000 lines.** The server bounds one answer at 500 and the ring
  at 4,096, but the page appends answer after answer for as long as the tab is open, so it
  lets go of the front: past five thousand lines the oldest fall out, `Copy` takes what is
  there, and the note under the title says so. Those lines are in the log file listed on
  the same page.
* **It does not list past a thousand open sessions.** The route lists the newest thousand
  and says `truncated: true`, and the page says so under the list — "More connections are
  open than this list holds; the newest 1000 are shown" — but there is no paging and no
  way to reach the rest from here. A server with a thousand connections open at once has a
  bigger question than this page answers.

Where it is proved
------------------

`RegressionTests.API.RestApiSessionFollow` opens two IMAP sessions at once and finds each
in the list by its own account; follows the first from the start and sees its `LOGIN` and
neither the password nor the other session's sign-in; sends `B7 NOOP` on the second and
`A7 NOOP` on the first and, following the first from the cursor, sees `A7 NOOP` and its
answer, nothing from before the cursor, and no trace of `B7 NOOP`, with every line either
the session's own conversation or one naming its id; logs out and sees the session listed
as ended, with its `LOGOUT`; refuses an unknown id with 404, a non-numeric `after` with
400 and a request with no credential with 401. A second fixture starts a follow with
`wait=10` on another thread, sends a NOOP a second later, and sees the follow answer within
a few seconds saying it waited — the proof that a line wakes a follower rather than a timer
— and, while that one is waiting, makes a second follow and sees it answered at once with
`waited: false`, which is the waiter budget of one. A third makes two IP ranges whose names
differ by a space (`follow-session1999123-trap` and `follow-session 1999124-trap`), each
written to the application log verbatim: the follow of the first number is 404, because a
line quoting somebody else's text is filed under no session, and the follow of the second
carries the line, because that is how a line with no session column names one at all.

The fixture is linked into `LinuxRegressionTests` as well, so both halves run on the Linux
server: the `%I64d`-to-`%lld` rewrite the cursor and the sequence numbers go through, the
times, and the wait on a `std::condition_variable` under glibc.

The Deck's half is in `build/deck-script-test.js`, which runs the page against a recorded
server: the list drawn with the open sessions and the ended ones, `Follow` asking from
sequence 0 with a wait and then from the cursor the server answered, the lines drawn oldest
first, `Copy`, `Stop following`, a second `Follow` after a stop that the stopped one must
not pour its lines into, a session the server no longer holds refused in the server's own
words, a pane fed 5,200 lines that keeps the last 5,000 of them and says so, and the empty
state.
