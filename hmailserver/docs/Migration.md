Migrating mail into hMailServer
===============================

How to bring existing mailboxes into this server: from another IMAP server, from
mbox files, from a Maildir, from Outlook, and from the original upstream
hMailServer. What each route preserves, what it does not, and what to do about the
one format the server does not read.

If you are moving an *installation* of hMailServer rather than mail from somewhere
else, this is the wrong page: [Upgrading.md](Upgrading.md) covers a release-to-release
upgrade and the move from the upstream project, and
[MigratingDatabaseBackend.md](MigratingDatabaseBackend.md) covers a change of database.

The routes at a glance
----------------------

| From | Route | Keeps | Does not keep |
|---|---|---|---|
| Another IMAP server (Dovecot, Exchange, Gmail, Microsoft 365, the old hosting) | An external account with **Mirror every folder** on: the server itself collects every folder over IMAP | Folders and their hierarchy, every message byte for byte, `\Seen` `\Flagged` `\Answered` `\Draft` `\Deleted`, the internal date | Subscriptions, custom keywords, ACLs, what `\Noselect` nodes hold (nothing) |
| Many mailboxes at once: an IMAP server, Google Workspace, Microsoft 365 | A migration: a job per mailbox, run, throttled, resumed and reported on by the server - see *Five hundred mailboxes* below | Folders into their special-use counterparts, every message byte for byte, the five flags and keywords, the internal date; contacts and calendars | Rules, signatures, shared mailboxes, ACLs, subscriptions, Gmail's labels as labels (a message is copied once) |
| mbox files (Thunderbird's profile, Apple Mail export, a Unix spool) | The Import Tool, one IMAP folder per file | Every message; the date from the `Received` or `Date` header | Flags: mbox has no standard for them, and the Import Tool sets none |
| A Maildir (Dovecot, Courier, Postfix local delivery) | The Import Tool, the folders and flags read from the tree | The Maildir++ hierarchy, every message, the flags the file names carry, the date from the headers | Custom keywords; the arrival time in the file name (the header date is used instead) |
| Outlook (PST) | Outlook itself, through IMAP - see below | Whatever Outlook uploads: folders, messages, read and flagged state | Anything Outlook does not put on the wire |
| The upstream hMailServer, in place | The installer, over the existing installation | Everything | Nothing - see [Upgrading.md](Upgrading.md) |
| Accounts in bulk | The Import Tool's text-file import | Addresses, passwords, quotas | Mail - pair it with one of the routes above |

From the webmail, by the person moving
--------------------------------------

A person can do the IMAP route for their own mailbox without an administrator:
**Settings, Other accounts** in the webmail. They pick their old provider (Gmail,
Outlook.com, Yahoo Mail and iCloud Mail fill in the server, port and encryption, and
say what each asks for - an app password for three of them), give the user name and
password, choose whether to leave the mail there, and tick **Bring the folders over
too** for a move. It is an external account like any other, made through
`/api/v1/me/fetch-accounts`, with these differences from one an administrator makes:

- The spam and virus filters are on for new mail and the recipients are never read
  out of the collected messages; a request that asks otherwise is refused by name.
  (Folders brought over arrive as copies, as described below, without the filters.)
- The host has to resolve to public addresses only. An administrator who wants
  people to collect from a mail server inside the network turns on
  `reader_fetch_accounts_private_addresses`. The check is made when the account is
  saved.
- The interval is at least 15 minutes, and a person has at most
  `reader_fetch_accounts_max` of them (3 by default; 0 takes the page away).
- An external account an administrator set up with what a person cannot set - the
  recipients read from the message, routing by them, a filter off - is listed on the
  page and can be collected now, but not changed or removed from there.

Outlook.com no longer accepts a password from another mail server, and Gmail takes
only an app password. Where an administrator has registered the server with them (below),
the page offers **Sign in to Gmail** and **Sign in to Outlook.com** instead.

### Signing in rather than giving a password

The person presses the button, signs in at Google or Microsoft and agrees to let the
server read their mail over IMAP, and comes back to the Other accounts page with an
external account made for them: the provider's IMAP server on port 993 with TLS, the
address they signed in as, the filters on, and a login with XOAUTH2 using the grant
rather than a password. No password is stored; the refresh token is kept protected in
the database as a password is, and the person can take the permission back at the
provider at any time. Signing in again as the same address gives the existing account
the new grant instead of adding a second one.

With **Bring the folders over** ticked, the first collection lists the folders there and
brings nothing. The page then offers them as a list to tick: a folder that only shows
mail kept in the others - Gmail's All Mail, Starred and Important, which the server
recognises by the `\All`, `\Flagged` and `\Important` attributes the provider sends,
whatever language their names are in - starts unticked, so nothing arrives twice. What
is left unticked stays where it is; the choice can be changed later with **Folders**.

The flow is OAuth 2.0's authorization code grant with PKCE (S256). The state is good for
ten minutes and once; the browser that started it carries a nonce in an HttpOnly,
SameSite=Lax cookie that the callback checks, so a link someone else sends cannot attach
their mailbox to this account. An account signed in this way keeps its server, port,
address and encryption: the person can pause it, collect now, choose folders or remove
it, but a change that would carry the grant to another host is refused.

To offer it, register the server with each provider and set, on `PUT /api/v1/settings`:

| Setting | What it is |
|---|---|
| `import_oauth_redirect_base_url` | The address the webmail is reached at from outside, e.g. `https://mail.example.com`. The redirect URI to register is this plus `/portal/oauth/callback`. |
| `import_oauth_google_client_id`, `import_oauth_google_client_secret` | A **Web application** OAuth client in the Google Cloud console, with the Gmail API enabled and the `https://mail.google.com/` scope on the consent screen. Google requires the secret. |
| `import_oauth_microsoft_client_id`, `import_oauth_microsoft_client_secret` | An app registration in Microsoft Entra that accepts personal Microsoft accounts (for Outlook.com), with the delegated `IMAP.AccessAsUser.All` and `offline_access` permissions and a Web redirect URI. The secret is for a Web platform registration. |
| `import_oauth_google_authorize_url`, `import_oauth_google_token_url`, `import_oauth_microsoft_authorize_url`, `import_oauth_microsoft_token_url` | The endpoints; the public ones by default. A single tenant or a national cloud has its own. |

The secrets are write-only and stored protected. A provider is offered once it has a
client id (and, for Google, the secret) and the base URL is set. Until Google verifies an
app that asks for the Gmail scope, only the test users named on its consent screen can
sign in - which is enough for an organisation moving its own people.


From another IMAP server
------------------------

The server has an IMAP client of its own - the one that collects external accounts -
and with the mirror switch on it does what `imapsync` does: every folder the remote
lists, every message not yet collected, filed into the local folder of the same
name exactly as the remote holds it.

1. Create the local account the mail is going to.
2. On it, add an external account: **server type IMAP**, the remote host, port and
   security (implicit TLS on 993, STARTTLS on 143, or a host on `FetchOAuth2Hosts`
   for XOAUTH2), the remote credentials, and **Mirror every folder** on. Over COM
   that is `FetchAccount.ServerType = 1` and `FetchAccount.MirrorFolders = true`.
3. Decide what happens on the far side. **Days to keep messages** is per collected
   message, as for any external account: 0 deletes each message from the remote
   folder once it is safely filed here - a move - and any other value leaves it
   there, so the two servers stay in step for as long as the account polls. A
   migration usually runs with the messages left in place until the cut-over, then
   one last poll and the account is disabled.
4. Poll. **Download now** starts a collection at once; otherwise the account polls
   at its interval. A large mailbox is collected over several polls if the remote
   server ends the session first; nothing is lost, because a message is recorded
   as collected only after it has been filed, and the next poll takes up where the
   last one stopped.

What it does, folder by folder: `LIST "" "*"`, then for each mailbox that can be
selected, `SELECT`, `UID SEARCH ALL`, and `UID FETCH (FLAGS INTERNALDATE BODY.PEEK[])`
for every UID not on record for that mailbox. The message is stored byte for byte -
no `X-hMailServer-ExternalAccount` header, no delivery, no rules, no anti-spam or
anti-virus, because it is a copy of mail the other server had already accepted, not
new mail arriving. The remote flags become the local flags and the remote internal
date becomes the local one, so a mailbox migrated today does not read as having
arrived today. The remote hierarchy delimiter is mapped to the local one:
`Archive/2025` on a server that separates with `/` becomes `Archive.2025` here, the
folder `2025` inside `Archive`. Mailbox names in modified UTF-7 pass through as they
are; both servers speak it.

Each mailbox keeps its own record of what has been collected (the record names the
mailbox, its UIDVALIDITY and the UID), so the same UID in two mailboxes is two
messages, a second poll asks each mailbox only for what is new, and a mailbox whose
UIDVALIDITY changes is collected afresh - the same rules the INBOX-only collection
has always followed, applied per folder.

Things to know before switching it on:

- **Use a new external account for the mirror.** An account that has been
  collecting the INBOX without the mirror keeps its records under a bare key; the
  mirror's keys name the mailbox. Switching the mirror on for that account
  collects the INBOX once more.
- A mailbox the remote refuses to `SELECT`, or lists as `\Noselect`, is skipped with
  a line in the application log, and the mailboxes after it are still collected.
  A mailbox name the remote sends as a literal (rare; a very long name) is skipped
  the same way.
- The `OnExternalAccountDownload` script event does not fire for a mirrored
  message, and the per-message override of *days to keep* it offers does not
  apply; the account's value does.
- Subscriptions are not copied. The local folders are created subscribed, so a
  client that shows subscribed folders sees them all.
- It is a mirror of messages, not of the account: ACLs, quotas, sieve or vacation
  settings on the far side are not read.

Five hundred mailboxes: migration jobs
--------------------------------------

The external account above moves one mailbox, and somebody has to make it. Moving an
organisation - five hundred mailboxes from an old IMAP server, from Google Workspace
or from Microsoft 365 - needs something the server runs itself: a job per mailbox
with a state, no more load on the source than it will bear, a job that goes on from
where it was after a network failure or a restart, a report per mailbox saying what
was copied and what was not, and the contacts and calendars beside the mail. That is
a **migration**. It is administered over REST, under `/api/v1/migrations`, and in the
Control Deck's **Migrations** view. There is no COM interface for it.

### The model

A migration is the batch. It names its source - `kind` `imap` (any IMAP server, a
password per mailbox), `google` or `microsoft365` (each mailbox signed in to by its
owner, below) - with the host, the port and the security (`tls` unless another is
given, and the port then 993 with `tls` and 143 with STARTTLS or none); how many of
its jobs run at once (`concurrency`, 1 to 32, 4 by default); and how many messages a
minute each job may fetch (`messages_per_minute`, 0 for no limit). A Google or
Microsoft 365 migration reads the provider's own IMAP server - `imap.gmail.com` or
`outlook.office365.com`, port 993, over TLS - and a host, a port or a security naming
anything else is refused, since the mailbox owner's token is sent there.

A **job** is one mailbox there copied into one account here: the source user name,
the secret (the password; for Google and Microsoft 365 a refresh token, or nothing
yet), and the target account, which must exist already - create the accounts first,
with the Import Tool's text-file import (below) or `POST
/api/v1/domains/{domain}/accounts`. A job is `queued`, `running`, `paused`, `done`,
`failed` or `cancelled`, and goes through the phases `mail`, `contacts`, `calendars`
and `report`.

    POST /api/v1/migrations               {"name": "Old server", "host": "imap.old.example"}
    POST /api/v1/migrations/{id}/jobs     the CSV below, or one job as JSON
    POST /api/v1/migrations/{id}/start
    GET  /api/v1/migrations/{id}/jobs     every job's state, progress and last error
    GET  /api/v1/migrations/{id}/jobs/{job}/report
    GET  /api/v1/migrations/{id}/summary.csv

`pause` and `resume` act on a whole migration: a running job stops between two
commands - two IMAP commands, or two DAV or Graph requests - writes what it copied and
its resume marks, and waits; `resume` queues it again. A job has `retry` - a failed,
cancelled or paused job is queued again from its marks, its attempts counted afresh,
and the body may carry `secret`, the mended password; the Deck's Retry has a box for
it - and `cancel`: it stops, and what it copied stays. `DELETE` removes a migration,
its jobs and their marks, and is refused while a job of it is running; what was copied
stays where it was copied to. Jobs may be added to a migration at any time, and a
finished migration given new jobs runs again. Deleting the account a job copies into
cancels its unfinished jobs, with "The target account was deleted." as the last error.

Reading needs `view_accounts`. Everything that writes needs `manage_accounts`, and
adding a job needs it in the target account's domain. A credential confined to named
domains sees a migration only when it has a job copying into one of those domains and
none copying anywhere else; it cannot create a migration, since one with no jobs is no
domain's, so an administrator of the whole server creates it, and a job into one of
that credential's domains then lets it see the migration and add to it. No permission
was added for migrations. The audit trail records the migrations and their jobs
(`hm_migrations`, `hm_migrationjobs`, a job named by its mailbox and target, "anna@old
-> anna@example.com") under whoever created, started, paused, resumed or deleted
them, or added, retried or cancelled a job - the cancel of a running job as an event,
since the job's own thread writes that row - with a secret recorded as changed and
never by its value; a job's own progress is not recorded.

### The CSV of jobs

One line per mailbox, three fields:

    sourceuser,secret,target
    anna@old.example,Anna's old password,anna@example.com
    "Bert, the second",hunter2,bert@example.com
    carla@example.com,,carla@example.com

A first line naming the columns is skipped, a UTF-8 byte order mark before it (what
a spreadsheet's "CSV UTF-8" writes) is ignored, blank lines are ignored, and a field
holding a comma or a quote is quoted as RFC 4180 quotes it. One request takes up to
5,000 lines. Every line is checked before any job is made - three fields, a source
user, a target that is an account of this server in a domain the credential
administers, a secret for an IMAP source, no mailbox twice in one migration, and none
that a job of another migration is copying into that account now (409: each job knows
only what it wrote itself, so two at once would copy every message twice) - and a line
that fails refuses the whole request, naming it:

    {"error":"line 3: the target nobody@example.com is not an account of this server","line":3}

A target that is not even an address is not repeated back ("the third field, the
target, is not an address..."), since a row with its columns the wrong way round has
a password there. Nothing is half added. One job can be added as a JSON object
instead, `source_user`, `secret` and `target`. A secret is kept protected in the
database, as a fetch account's password is, and never returned. The Deck's view takes
the lines pasted, or reads a file chosen into the box so it can be looked at before it
is sent. A second migration of a mailbox whose first job has finished copies it
again, message for message.

### What a job does

For the mail, a job that signs in gets its access token first - a provider that
cannot issue one just now is a retry, and the source is not spoken to - then logs in
to the source - `LOGIN`, or `AUTHENTICATE XOAUTH2` with the token - asks `NAMESPACE`
where other users' mailboxes and the shared ones are, lists its folders, and maps each
to one here. Other users' and shared mailboxes are left, with a note in the report:
they are somebody else's, copied by that person's own job. The INBOX goes to the
INBOX. A folder of special use (`\Sent`, `\Drafts`, `\Junk`, `\Trash`, `\Archive`, by
the attribute the source sends, or when it sends none by the names this server's own
folders are known by) goes to the account's own folder of that use, so Gmail's
`[Gmail]/Sent Mail` lands in the account's Sent folder whatever that is called. Every
other folder goes to the same path, with the source's hierarchy delimiter written as
this server's.

It opens each folder with `EXAMINE` and fetches the messages above its mark one at a
time, `UID FETCH (UID FLAGS INTERNALDATE BODY.PEEK[])`, so nothing on the source is
marked read. A folder the source will not open for now - `[UNAVAILABLE]`, `[INUSE]`,
`[LIMIT]`, `[SERVERBUG]`, `[THROTTLED]`, Microsoft 365's "not connected" - is a retry;
one it says it will never open - `[NONEXISTENT]`, `[NOPERM]`, a bare NO, a BAD - is
left uncopied, with a note on the folder, an entry in the report's errors and a line in
the application log, and the job goes on. A message the source will not give (a NO to
its FETCH, a damaged message) is asked for three times and then skipped as `refused`,
said the same way. Each message is stored byte for byte with its flags (`\Seen`,
`\Answered`, `\Flagged`, `\Draft`, `\Deleted`), its keywords and its internal date,
through the one import entry every importer of a mailbox here shares, which asks what
`APPEND` asks: the largest message the account may receive, room on the data volume,
and the quota. A message too large is skipped and counted as `too_large`. An account
over its quota, or a volume without room, pauses the job with the reason, to go on
when that is mended, because skipping would lose mail. A folder that cannot be made in
the account just now (the database away) is a retry; one whose name the account never
takes is left, said as above. No rule, filter or scan runs over a copied message: it
is mail the other server had already accepted.

Then the contacts and the calendars (below), then the report, and the job is done.
A job is one copy, not a synchronisation: mail that reaches the source after a folder
was finished is not collected. Move the MX records first and migrate after, or keep
an external account with the mirror (above) for the mailboxes that must stay in step
until the cut-over.

### The resume rule

After every 50 messages dealt with - copied or skipped - and at the end of each
folder, the job writes the folder's UIDVALIDITY and the highest UID it has dealt with
(`hm_migrationfolders`), with its counters and its report. Whatever stops it - a
network failure, a pause, a cancel, the service stopping or failing - it goes on from
those marks the next time it runs, and a folder marked done is not opened again.

That leaves up to 49 messages copied after the last mark and before a crash. So a job
that has run before first reads, for every folder of its plan - one it finished before
included - what it wrote there since it first started: the Message-ID and the size of
each message, or where there is none its date, subject and size. A message found among
them in its folder is not stored again but counted as `duplicate`, and the check
across folders (below) sees every folder's. Two different messages a sender gave one
Message-ID are both copied, unless they are also the same size. A folder whose
UIDVALIDITY changed on the source since its mark was written is taken again from its
first message against the same keys, and the report counts the restart. However often
a job is interrupted, each message arrives once.

A stop of the service leaves its running jobs `running` in the database, as they
were; the next start queues them again and they go on from their marks. A pause or a
cancel asked for just before the stop is kept. A job whose row says running with
nothing in the service running it - its thread ended before it could write what
became of it - is found by the scheduler and started again from its marks, and a
retry takes it too.

A failure that time may mend - the connection lost, the source answering 429 or a
5xx, the source unable to log anybody on just now (`[UNAVAILABLE]`, `[INUSE]`,
`[LIMIT]`, an `[ALERT]`, a BAD, a dropped connection), the provider's token endpoint
busy (no answer, a 429 or a 5xx, `temporarily_unavailable`, `server_error`), the
database away, a quota full - pauses the job with the error and tries again after 1,
5 and 15 minutes and then every hour. A run that copies a message counts the attempts
afresh; after 24 attempts in a row without one the job is `failed`, its last error
kept. The source refusing the credentials themselves - `[AUTHENTICATIONFAILED]`,
`[AUTHORIZATIONFAILED]`, `[EXPIRED]`, or a NO with no code - or the provider refusing
the grant fails the job at once, since no retry mends a wrong password;
`POST .../jobs/{job}/retry` with `{"secret": "..."}` queues it again.

### Throttling

Three ceilings, each applied when a job is started:

- A migration runs at most `concurrency` of its jobs at once.
- Every migration from the same source host shares `MigrationConnectionsPerHost` (4
  by default, 1 to 64; `migration_connections_per_host` on `PUT /api/v1/settings`).
  Two migrations from one server never hold more sessions there between them than
  that, because a provider counts sessions per client address, not per migration.
- The server never runs more than 64 jobs in all.

Each running job holds one IMAP session. Within a job, `messages_per_minute` spaces
the fetches evenly; 0 fetches as fast as the source answers.

### Google Workspace and Microsoft 365: signing in

A `google` or `microsoft365` migration logs in with OAuth rather than a password,
through the registration the webmail's sign-in buttons use (above:
`import_oauth_redirect_base_url` and the provider's client id and secret). A job
added with an empty secret waits for its owner. The answer to the request that added
it carries `signin_url`, a link for that person to open - shown once, and kept here
only as a digest. The link works once: opening it spends it, and a person who then
does not finish needs a new one. They sign in at the provider as the mailbox the job
copies, grant what is asked, and the job starts when the migration runs; a sign-in as
anyone else attaches nothing and gives the link back for the right person.
`POST .../jobs/{job}/signin` makes a new link: the one before it stops working, and
so does a sign-in begun from it and not yet finished. Signing in for a job that failed
or was cancelled - its owner signing in again after the grant was refused - queues it
again. A job can be given a refresh token as its secret instead, where an
administrator has one.

The scopes asked for, which the client's registration must allow:

| Provider | Mail | Contacts | Calendars |
|---|---|---|---|
| Google | `https://mail.google.com/` | `https://www.googleapis.com/auth/carddav` | `https://www.googleapis.com/auth/calendar` |
| Microsoft 365 | `https://outlook.office.com/IMAP.AccessAsUser.All`, `offline_access` | Microsoft Graph `Contacts.Read` | Microsoft Graph `Calendars.Read` |

`openid` and `email` are asked for as well, to know who signed in. A Google client
needs the Gmail API, the CardDAV API and the CalDAV API enabled in the Cloud console
and the scopes on its consent screen; a Microsoft Entra registration needs the
delegated permissions, and a work or school tenant may need an administrator to
consent to them for the organisation. Someone who leaves the contacts or the
calendars unticked at the consent screen still has their mail copied: that phase is
skipped with its reason - from Google "the sign-in did not grant ...", which Google's
answer says in so many words, and from Microsoft 365 "the sign-in does not reach them:
...", with the provider's refusal of the token for Graph.

### Contacts and calendars

After the mail, a job copies the contacts and the events:

- From an IMAP server, by CardDAV and CalDAV, found by RFC 6764 discovery at
  `https://<host>/.well-known/carddav` and `/.well-known/caldav` with the job's
  user name and password. `dav_url` names another place when the DAV server is not
  the IMAP host. It must be `https`, except to this machine itself, where plain
  `http` is allowed so a test can read the server's own DAV.
- From Google, by Google's CardDAV (the default address book) and CalDAV (the
  primary calendar).
- From Microsoft 365, by Microsoft Graph: `/me/contacts` and `/me/events`, turned
  into vCards and iCalendar events in the event's own time zone. A recurring series
  keeps its rule.

The password or the token goes only to the origin - scheme, host and port - the
reader begins at. A redirect, an href or a next page on another host is followed
without it, a redirect from `https` to plain `http` is refused, and Graph's next page
is taken only from `graph.microsoft.com`. A DAV server that sends its clients to
another host (iCloud does) therefore gets no password there: name that host as the
migration's `dav_url`.

The first address book or calendar goes to the account's default one, and every
other to one of the same name, made when missing. A contact whose address the
account already holds is not copied again (`already_there`, the address book's own
rule of one address once), nor an event whose UID the calendar holds, nor a card or
an event over a megabyte (`too_large`: what this server's own CardDAV and CalDAV
take). A phase that cannot run - nothing listening at the host, a name that does not
resolve, a 404, a token without the scope, an answer too large or not DAV - is
skipped with the reason in the report, and never fails the job. A host that is there
and busy - a timeout, a 429, a 5xx - is tried again, and after six attempts in a row
the phase is skipped with the last reason rather than failing a job whose mail is
copied.

### The report

`GET .../jobs/{job}/report` is JSON, written at every resume mark and at the end, so
it can be read while the job runs. For each folder: the source's name and the target
folder, its special use, how many messages the source held, how many were copied,
how many were skipped and why (`duplicate`, `another_folder`, `too_large`, `refused` -
a message the source would not give - `gone` - expunged on the source between the
listing and the fetch - and `empty`, answered without a body), how often its
UIDVALIDITY restarted it, and a note where one applies: a folder left and why, other
people's mailboxes, a view not copied, and a folder whose messages were copied once
into another. Then the contacts and calendars, each copied, skipped with the reasons,
or skipped whole with the phase's reason; the bytes; the duration; the first and last
message dates; and the errors - the failures met and retried, and what was left for
good - the newest fifty.

`GET .../summary.csv` is a migration's jobs one per row - job, source user, target,
state, phase, folders and messages done of their totals, skipped, bytes, attempts,
last error, and the start and finish in UTC seconds - for the person running five
hundred of them in a spreadsheet. A value that starts as a spreadsheet formula would
(`=`, `+`, `-`, `@`) is written behind an apostrophe, as text.

Two errors reach the server's error log, both about this server rather than the
source: `HM7240` when a resume mark could not be written (the job goes on, and an
interruption before the next mark repeats that work without copying anything twice),
and `HM7241` when a message could not be filed (the job pauses and tries again). What
the source did - a wrong password, a lost connection, a folder it refused - goes to
the job's `last_error`, its report and the application log. A server at the host and
port that is not IMAP is said to be so, without what it answered.

### What is not migrated

- **Rules and filters** - Gmail's filters, Outlook's inbox rules, Sieve scripts - are
  not read. Make them again as the account's rules.
- **Signatures, out-of-office replies and forwarding** are not read.
- **Shared mailboxes and permissions**: other users' and shared mailboxes the source
  lists under its NAMESPACE are left, with a note, and a Microsoft 365 shared mailbox,
  which nobody signs in as, cannot be a job; what others could open is not read.
- **Subscriptions**: every folder is created subscribed.
- **Gmail's labels as labels.** Over IMAP a label is a folder, so a message with
  three labels is in three folders, and All Mail holds every message once more. For a
  `google` migration a message is copied once, into the first folder it is met in -
  the INBOX first, then the folders as Gmail lists them - and counted as skipped
  `another_folder` in the others; the report's note on such a folder says so. All Mail
  is taken last and brings only what no other folder had: the messages archived under
  no label. Starred and Important are views and are not copied as folders; the star
  arrives as the message's `\Flagged` flag. The same rule keeps any source's `\All`
  folder from copying a message twice, before an interruption and after one.
- **Contact groups**, and contacts with no e-mail address, are skipped and counted.
- **From Microsoft 365**, the default contacts folder and the default calendar only,
  and a recurring series without the occurrences that were moved or cancelled on
  their own.

### Which route to use

- **One person moving their own mailbox**: the webmail's **Other accounts** (above).
- **A few mailboxes that must stay in step until the cut-over**: an external account
  with the mirror on, which polls until it is switched off.
- **Many mailboxes, or Google Workspace or Microsoft 365 with the contacts and
  calendars, and a report per mailbox**: a migration.
- **mbox files or a Maildir on disk**: the Import Tool, below.
- **The accounts themselves, in bulk**: the Import Tool's text-file import, below,
  before any of the others.

From mbox files
---------------

The Import Tool (`Start menu > hMailServer > Import Tool`, or
`Addons\ImportTool\ImportTool.exe` under the installation) imports a folder of mbox
files into one account, one IMAP folder per file, named after the file without its
extension; `Inbox` (any case) is the INBOX. Thunderbird's profile keeps its
folders this way (`Mail\<account>\Inbox`, `Sent`, `Archives.sbd\2025`), and its `.msf`
index files beside them are ignored. Each message is streamed out of the file - a
mailbox larger than memory is fine - its line endings made CRLF, and handed to the
server, which files it and takes the date from its `Received` or `Date` header.
mbox carries no flags the tool could read, so every imported message is unread.

From a Maildir
--------------

The same tool, the **Select Maildir** button. Point it at the Maildir itself - the
directory holding `cur`, `new` and `tmp` - and it lists the INBOX and every
Maildir++ folder beside them (`.Sent`, `.Archive.2025`: the names start with a dot
and use a dot as the separator, which is also this server's default, so
`.Archive.2025` becomes the folder `2025` inside `Archive`). Every file in `new` and
`cur` is a message; `tmp` is never read.

The flags a file's name carries after `:2,` are set on the copy once the folder is
imported: `S` seen, `F` flagged, `R` answered, `D` draft, `T` deleted. Line endings
are made CRLF, as with mbox. The date comes from the message's headers rather than
from the timestamp in the file name.

A colon is not allowed in a Windows file name, so a Maildir cannot be copied here
with its names intact: whatever brought it over either replaced the colon or dropped
the flags with it. The tool reads `;2,` and `!2,` as well as `:2,`, which covers the
copies that replaced it. If your copy shows names ending in `_2,FS` or without the
info part at all, the flags did not survive the copy - the messages still import,
unread, and the fix is to copy again with a tool that maps the character (WSL, or an
archive made on the Unix side and opened with 7-Zip, keep it).

If the server's IMAP hierarchy delimiter has been changed from `.`, a Maildir++
folder name is still split on `.`, because that is what the format means by it.

From Outlook
------------

The server does not read PST files, and this is deliberate. PST is a proprietary
format whose readable implementations are either commercial or GPL-licensed
libraries with a long record of fidelity problems on real archives, and a migration
tool that silently drops or damages a message is worse than none. Outlook itself,
which wrote the file, is the reliable reader of it:

1. Add the hMailServer account to Outlook as an IMAP account.
2. Drag the folders from the PST-backed account onto the IMAP account, or copy
   them. Outlook uploads each message with `APPEND`, flags and dates included, and
   creates the folders as it goes.
3. When the folder counts match, remove the PST account from Outlook.

The same works from any client that speaks IMAP, and it is also the way out of a
mail service that offers no IMAP access but has a desktop client that does.

From the upstream hMailServer
-----------------------------

Run this fork's installer over the existing installation. The database is upgraded
in place, the message store is untouched, and the settings carry over.
[Upgrading.md](Upgrading.md) has the detail, including what to back up first.

Accounts in bulk
----------------

The Import Tool's other button creates or updates the accounts of one domain from
a comma-separated text file - address, password, and optionally the quota - which is
the usual first step before any of the routes above, since each needs the local
account to exist.
