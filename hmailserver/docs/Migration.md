Migrating mail into hMailServer
===============================

How to bring existing mailboxes into this server: from another IMAP server, from
mbox files, from a Maildir, from Outlook's own data files (PST), and from the
original upstream hMailServer. What each route preserves, and what it does not.

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
| Outlook data files (PST) | A migration of kind `pst`: the server reads the file itself - see *Outlook data files* below | The folders under the IPM subtree, the special folders by what the store says they are, every message rebuilt as Internet mail with its attachments, read, draft, flagged, answered and forwarded, its categories as keywords, its internal date; contacts as vCards in the address books, calendar items and tasks in the calendars, notes and journal entries as messages; ANSI files, and OSTs whose store names its folders as a PST does | Contacts without an e-mail address and distribution lists (counted), attachments by reference and OLE objects; the 4K-page layout of an OST from Outlook 2013 on, an OST caching an Exchange mailbox and a file under Windows Information Protection (refused) - a migration from the mailbox's server, or Outlook over IMAP, is the route for those |
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
- The host has to resolve to public addresses only, and the port is 110 or 995
  (POP3) or 143 or 993 (IMAP). An administrator who wants people to collect from a
  mail server inside the network turns on `reader_fetch_accounts_private_addresses`,
  which allows any address and any port. The host is checked when the account is
  saved, and again every time the server collects: the account is marked
  `reader_created` (`hm_fetchaccounts.faorigin`, schema 6104), and the
  fetcher connects it only to the public addresses the name resolves to at that
  moment, through the server's own DNS. A name that answered a public address at
  the save and a private one later (DNS rebinding), or one the server's DNS
  answers differently from the system's (split horizon), is then not collected,
  and the error log says why (`HM5909`). An account a person moves to another
  server or port on the page is marked the same way, including one an
  administrator set up; a rename or a pause marks nothing. The mark stays when an
  administrator changes the account afterwards. Accounts made before 6104, and
  the ones the sign-in import makes (the provider's own server), are not marked
  `reader_created`, so where they connect is not judged again.
- The interval is at least 15 minutes, and a person has at most
  `reader_fetch_accounts_max` of them (3 by default; 0 takes the page away).
- An external account an administrator set up with what a person cannot set - the
  recipients read from the message, routing by them, a filter off - is listed on the
  page and can be collected now, but not changed or removed from there.
- A host on `FetchOAuth2Hosts` (`outlook.office365.com` by default) is refused, and an
  external account an administrator set up for one is not changed from the page: such
  an account logs in with the server's own OAuth2 token, which reaches every mailbox
  the app registration was granted, as whatever user name the account gives. Only a
  server administrator adds or changes one; a key restricted to named domains is
  refused with 403, and a domain administrator's `FetchAccount.Save` over COM is
  refused too. An account whose server, port or user name was named by somebody
  without the server's authority - a person on the page (`reader_created`), a key
  restricted to named domains, a domain administrator over COM, or a person who
  changed only the user name of an administrator's account - is marked
  `server_token_withheld` and never gets the server's token, even for a host an
  administrator adds to the list afterwards: it logs in with its own user name and
  password. So is one the sign-in import makes, which logs in with the person's own
  grant. A rename or a pause marks nothing, and nothing but deleting the account and
  making it again as a server administrator takes the mark away.

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
ten minutes and once, and an account - or a migration job - has at most three sign-ins
waiting to finish: a fourth drops its own oldest, and a server with a thousand waiting
drops the oldest of all, so nobody starting sign-in after sign-in can keep anybody
else's from starting; the browser that started it carries a nonce in an HttpOnly,
SameSite=Lax cookie that the callback checks, so a link someone else sends cannot attach
their mailbox to this account. An account signed in this way keeps its server, port,
address and encryption: the person can pause it, collect now, choose folders or remove
it, but a change that would carry the grant to another host is refused.

To offer it, register the server with each provider and set, on `PUT /api/v1/settings`
or on the Control Panel's Webmail page, the Import tab (which reaches them over the REST
API, so the REST API has to be on):

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
   A bearer token - the server's for a host on `FetchOAuth2Hosts`, or the person's
   grant - is sent over TLS only: with security *none*, or STARTTLS optional against
   a server that does not offer it, the fetcher logs in with nothing and says so in
   the error log.
3. Decide what happens on the far side. **Days to keep messages** is per collected
   message, and means what it means for any external account, POP3 or IMAP: -1
   deletes each message from the remote folder once it is safely filed here - a
   move; 0, the default, leaves every message there, so the two servers stay in
   step for as long as the account polls; a number of days N deletes a message N
   days after it was collected. A migration usually runs with the messages left in
   place until the cut-over, then one last poll and the account is disabled. (From
   6.2.25 to 6.3.5 an IMAP account read 0 as "delete" and -1 as "keep", the other
   way round from a POP3 account; the upgrade to 6.3.6, schema 6106, translated
   each IMAP account's value so that it goes on doing what it did, and the Control
   Panel, the Deck, the webmail and the REST API now all describe the one meaning.)
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
that credential's domains then lets it see the migration and add to it. A job copies
mail into its target account, so it is added - and a migration or a job started,
paused, resumed, retried, cancelled, signed in or deleted - only by a credential that
takes in every account the jobs copy into ([Roles.md](Roles.md)): a domain's
administrator is refused a job into the server administrator's own mailbox in its
domain, and a migration that has one. No permission was added for migrations. The audit trail records the migrations and their jobs
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
  by default, 1 to 64; `migration_connections_per_host` on `PUT /api/v1/settings`, or
  the Control Panel's Maintenance > Backup, restore & migration page, the Migrations
  card, over the REST API).
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
good - the newest fifty, each cut to 1,000 bytes, and as many of them as fit in 20,000
bytes of the report, so that long ones cannot crowd the folders out.

`GET .../summary.csv` is a migration's jobs one per row - job, source user, target,
state, phase, folders and messages done of their totals, skipped, bytes, attempts,
last error, and the start and finish in UTC seconds - for the person running five
hundred of them in a spreadsheet. A value that starts as a spreadsheet formula would
(`=`, `+`, `-`, `@`) is written behind an apostrophe, as text.

Two errors reach the server's error log, both about this server rather than the
source: `HM7240` when a resume mark could not be written (the job goes on, and an
interruption before the next mark repeats that work without copying anything twice),
or when a pass of the scheduler that starts and resumes the jobs stopped at an
unexpected error (the jobs it had not reached wait for the next pass, two seconds
later; said once an hour at most while it lasts),
and `HM7241` when a message could not be filed (the job pauses and tries again) - and
for a job of Outlook data files the same two as `HM7370` and `HM7371` (*Outlook data
files*, below). What
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

**On MySQL, the limit on names is gone from schema 6102.** Until then the tables
that hold a job's folders, contacts and address books were in MySQL's three-byte
`utf8`, and a name holding a character outside the Basic Multilingual Plane - an
emoji, some CJK - was refused there. Schema 6102 moves every table to `utf8mb4`,
and such a name is kept as it came, as it always was on SQL Server, SQL Server
Compact and PostgreSQL; see
[MigratingDatabaseBackend.md](MigratingDatabaseBackend.md#mysql-and-mariadb).

### Which route to use

- **One person moving their own mailbox**: the webmail's **Other accounts** (above).
- **A few mailboxes that must stay in step until the cut-over**: an external account
  with the mirror on, which polls until it is switched off.
- **Many mailboxes, or Google Workspace or Microsoft 365 with the contacts and
  calendars, and a report per mailbox**: a migration.
- **An Outlook data file (PST)**: a migration of kind `pst` - *Outlook data files*, below.
- **mbox files or a Maildir on disk**: the Import Tool, below.
- **The accounts themselves, in bulk**: the Import Tool's text-file import, below,
  before any of the others.

From mbox files
---------------

The Import Tool (`Start menu > hMailServer > Addons > Import Tool`, or
`Addons\ImportTool\ImportTool.exe` under the installation) imports a folder of mbox
files into one account, one IMAP folder per file, named after the file without its
extension; `Inbox` (any case) is the INBOX. Thunderbird's profile keeps its
folders this way (`Mail\<account>\Inbox`, `Sent`), and its `.msf` index files beside
them are ignored. Only the files directly in the chosen folder are listed: a
subfolder's files (Thunderbird's `Archives.sbd\2025`) are imported by choosing that
folder in a second run. Each message is streamed out of the file - a
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

Outlook data files (PST)
------------------------

**Built 24 September 2026, bounded 25 September 2026, and extended the same day** to
ANSI files, OSTs that are PSTs in all but name, contacts, calendar items, tasks, notes
and journal entries. A PST - the file Outlook keeps a mailbox or an archive in - is
read by this server itself, as a migration of kind `pst`: a job per file, each into
one account, with the migrations' state, throttle, resume marks and report (*Five
hundred mailboxes* above). The reader runs in a process of its own, which the service
starts for each phase of a job, gives the file and nothing else, and ends on a pause,
a cancel or a stop: a defect in the reader that ends its process costs one message,
not the service (*The reader's process*, below).

The reader is this project's own, written from Microsoft's published specifications
and nothing else: [MS-PST] for the file (its pages, blocks and B-trees - the Unicode
layout and the ANSI one - the heaps, property contexts and tables built on them, and
the store, folders and messages built on those), [MS-OXRTFCP] for compressed RTF,
[MS-OXRTFEX] for HTML and text carried in RTF, [MS-OXCMSG], [MS-OXPROPS] and
[MS-OXCMAIL] for what a message's properties mean as Internet mail, and
[MS-OXOCNTC], [MS-OXOCAL], [MS-OXOTASK] and [MS-OXCICAL] for contacts, appointments
and their recurrence, tasks, and how they are said in iCalendar. The server contains
no third-party PST code.

### Where the file goes

A job names a file; it never gives a path. The file is looked for only in the
subdirectory, named for the target account's domain, of the directory the setting
`pst_import_directory` names (`PstImportDirectory` in `hm_settings`, empty as shipped,
set with `PUT /api/v1/settings`, on the Control Deck's Settings page, or on the Control
Panel's Backup, restore & migration page, the Migrations card, over the REST API):

    D:\PstImport\                       <- pst_import_directory
    D:\PstImport\example.com\archive.pst <- a job for anna@example.com names archive.pst

The directory must be an absolute path on this machine (a share on another is
refused: the service's account usually cannot see one), and the subdirectory is the
domain's name in lower case. Copy the file there, where the account the service runs
as can read it. While the setting is empty, a PST job is refused with *No PST import
directory is set on this server*.

The name is checked when the job is added, and again when it runs. It must be a name
such as `archive.pst` or `2019/archive.pst`: a path, a drive letter, a share (`\\server\share`),
a leading separator or a `.` or `..` component is refused. Then the domain's
directory and the file are each resolved as the file system resolves them - every
link and junction followed, short names and case as they are on disk - and a file
that lands outside that domain's directory is refused, however its name was spelled.
The job checks the file it opened once more, by the final path of its own handle, so a
file swapped for a link between the check and the read is refused rather than read
(*The file changed between the check and the read; name it again.*) - and so is a
handle whose final path cannot be told, or that is on another machine, as the check
when the job was added refuses both. On Linux the file is opened without following a
link in its last name and without waiting on anything that is not a regular file (a
FIFO put in its place would otherwise hold the job, and every stop of the service
with it). A sentence about the file names it below the domain's directory, never by
where the directory is on this machine, since a domain's administrator reads it.

The rule is the same for everyone, the server administrator included, because the
directory is shared and a domain's administrator holds `manage_accounts` for their own
domain: without it they could import, into an account of their own, a file left for
another domain ([Roles.md](Roles.md)).

### Running one

    POST /api/v1/migrations               {"name": "Archives", "kind": "pst"}
    POST /api/v1/migrations/{id}/jobs     archive.pst,,anna@example.com
                                          2019/bert.pst,his-pst-password,bert@example.com
    POST /api/v1/migrations/{id}/start

A pst migration takes no host, port, security or `dav_url` (what a form sends for
them is not used); `concurrency` and `messages_per_minute` apply as to any migration,
and `MigrationConnectionsPerHost` does not, since there is no host. A job's
`source_user` is the file's name and its `secret` the file's password, when it has
one; a first CSV line naming the columns is skipped - `sourceuser,...`, or for a pst
migration `file,password,target` as a whole line (a mailbox or a file called `file`
is a job). The job keeps the one name the file has below the domain's directory,
however it was spelled - `2019\bert.pst`, `2019/bert.pst`, `2019/BERT.PST.` and its 8.3
name are all `2019/bert.pst` - so that one file is one job, and two migrations cannot
read it into one account at once.

`hmctl api POST /api/v1/migrations/{id}/jobs` takes one job as JSON -
`{"source_user": "archive.pst", "secret": "", "target": "anna@example.com"}` - and a
list of files goes as the CSV body above over REST, or into the Control Deck's
Migrations view, which makes the migration (kind `pst`) and takes the lines.

The permission is a migration's, `manage_accounts` in the target's domain, and
`manage_mailbox` there as well: a PST job writes into the account's folders. A file
whose store a finished job already gave that account is refused when its job runs -
*This file's store was imported into this account already, by job N of migration M* -
since a second job would file every message again; delete that migration to import it
once more.

### What is read

Only the folders a person sees in Outlook: the **IPM subtree** and everything under
it. Search folders, the folders outside the subtree (the search root, Outlook's views
and its own bookkeeping) and a folder's associated items (its views and rules, not
mail) are never read.

- Each folder is filed at its own path below the account's top level; a top-level
  **Inbox**, in any case, is the INBOX. A localised Inbox keeps its own name - the
  store names no Inbox - and the report shows where each folder went.
- The special folders are found by what the store says they are, **never by their
  names**: *Sent Items* and *Deleted Items* by the message store's
  `PR_IPM_SENTMAIL_ENTRYID` and `PR_IPM_WASTEBASKET_ENTRYID`, *Drafts* and *Junk
  E-mail* by the root folder's `PR_IPM_DRAFTS_ENTRYID` and the fifth entry of its
  `PR_ADDITIONAL_REN_ENTRYIDS` (else the Inbox's). Each is filed into the account's own
  folder of that use where it has one, and otherwise into a folder of the PST's name
  that is given the use (`\Sent`, `\Trash`, `\Drafts`, `\Junk`). An account folder that
  holds one of the four uses by its name alone (IMAP's name table: *Sent Items*,
  *Trash* ...) is given it for good before the import makes anything, so that no folder
  the import makes - an ordinary *Sent* listed before the account's *Sent Items* - can
  take it by name.
- An ordinary folder is filed at its own path even when its name is a special folder's
  - but never into a special folder: one whose path is the account's special folder's,
  or the one the PST's own special folder is filed into, arrives beside it as
  *Sent Items (PST)*, and the report names the rename. An ordinary *Junk E-mail* is an
  ordinary folder, and gets no `\Junk`.
- Names are made ones this server's folders can hold, and the report names every
  change: the hierarchy delimiter inside a name becomes `_`, a top-level name starting
  with `#` gets a `_` in front, an empty name is *Unnamed folder*, a name is cut, at a
  character, until its stored (modified UTF-7) form is at most 255 characters, and a
  folder deeper than 25 levels is filed into its ancestor at 25. Folders whose names
  differ only in case are one folder here, and sibling folders of one name are one
  line of the report, their counts added. A name is read as at most its first 1,024
  bytes; folders nested more than 64 levels below the IPM subtree are not read, and a
  file that names more than 100,000 folders, or whose folders' paths together pass
  16 MB, is refused.
- A folder's target is decided the first time the job sees it and kept - compared, at
  every run, as the job's row holds it (1,024 characters), so that a longer path is
  never cut short and split again - and the folders the job makes are subscribed.
- **Notes and journal entries** are filed as messages, each folder of them at its own
  path like any other; **contacts, calendar items and tasks** go into the account's
  address books and calendars in the job's contacts and calendars phases, and no mail
  folder is made for them (*Contacts, calendar items, tasks, notes and journals*,
  below). An appointment, a contact or a task inside a mail folder is counted by its
  class and not imported: those phases read the folders Outlook keeps such items in.

Each message is imported with:

- **Flags**: read is `\Seen`, unsent `\Draft`, flagged for follow-up (flag status 2)
  `\Flagged`, replied to `\Answered`, forwarded the keyword `$Forwarded`. `\Deleted` is
  never set.
- **Keywords**: its categories, each character an IMAP keyword cannot hold (a space,
  brackets, anything outside ASCII) made `_`; what does not fit in a message's 500
  characters of keywords is named in the report.
- **The internal date**: when it was delivered, else sent, else made, in this server's
  time as a delivered message's is - a delivery or sent time before 1981 or more than a
  day ahead is passed over for the next, with a warning, and a message with no date
  left is dated by its Date header, else as imported, with a warning.

### What a rebuilt message is

A PST keeps a message's properties, not the message as it arrived, so each is written
again as Internet mail, and **deterministically**: the same file gives the same bytes
on every run, which is how a resumed job knows what it filed before.

- **Headers**: the message's transport headers where it kept them (mail that was
  received), with MIME-Version and every `Content-` field left out and written again
  for the body built here. Otherwise they are built: From (the "sent representing"
  sender), Sender when it differs, To, Cc and - only where the recipient table has one
  - Bcc, Subject (Outlook's prefix marker removed), Date (sent, else delivered, else
  made, in UTC), Message-ID, In-Reply-To and References; none is invented. An
  Exchange sender or recipient is written by the SMTP address the PST keeps beside
  it; one it kept none for is written as its name alone (`Name:;`). Text outside ASCII
  is RFC 2047-encoded, and a line break inside a property never starts a header.
- **Bodies**: text/plain from the plain body; text/html from the HTML body, its bytes
  as they are and its charset named from the message's code page (a code page not in
  the table below gets no charset and a warning); else the HTML an RTF body carries
  (`\fromhtml1`), as UTF-8. multipart/alternative, multipart/related and
  multipart/mixed each appear only where they are needed.
- **Attachments**: a file is a part named by its long file name, cut to its last
  component, its control characters removed and RFC 2231-encoded, of its own MIME
  type or `application/octet-stream`; one with a Content-ID goes with an HTML body as
  a picture it shows. An embedded message is message/rfc822, rebuilt the same way, at
  most ten deep, and what it came in without is the message's warning too. Each
  multipart's boundary is one that no line of its parts begins with, since an
  embedded message's own lines go in as they are.
- **S/MIME** is kept as it was: a clear-signed message is its headers followed by the
  signed entity byte for byte; an opaque one is `application/pkcs7-mime`, with no
  `smime-type`, since the file does not say whether it is enveloped or signed.
- An identifier - a Message-ID, In-Reply-To or References - longer than a header line
  may be (998 characters) cannot be folded, and is left out with a warning.

### Contacts, calendar items, tasks, notes and journals

After the mail, the job's contacts and calendars phases open the file again and put
what it holds besides mail where a phone or the webmail would find it: through the
same stores CardDAV, CalDAV and the webmail write, past the same checks, as the text a
CardDAV or CalDAV client would have sent. Most of these items' properties are named
ones ([MS-PST] 2.4.7), found through the file's name-to-id map rather than by fixed
numbers. The phases read that map once, after the store is opened, as a unit of the
work budget of its own; where it cannot be read, no item is lost for it, each item
read without it says so, and the report says once why its contacts are counted
`no_address` and its events `no_start`. (The mail phase, which needs it only for a journal entry's
fields, reads it at the first entry, and tries it again at the next, three times in
all.)

**Which folders.** Every folder of the phase's kinds under the IPM subtree, but not a
folder Outlook keeps hidden (`PR_ATTR_HIDDEN`), and of contacts not the sub-classes of
`IPF.Contact` Exchange and Outlook keep for themselves - the recipient cache, the GAL
and organisational contacts, Skype's lists (`IPF.Contact.RecipientCache`,
`IPF.Contact.GalContacts` ...); the report names each folder left out, with why. A
folder under **Deleted Items** is imported, as the mail phase imports Deleted Items
(into the account's Trash) - but after every other folder, so that a live contact or
event always wins over a deleted copy of it, which is counted `already_there`: a
contact's address is looked for across the account's books, as it always is, and a
deleted event's UID across every one of the account's calendars. It goes into a book
of its name or a calendar of its own, never the default: one that cannot have its own
- its name the default book's or one no book may have, the account at its ceiling of
books, the job at its ceiling of calendars - is left out, its items counted
`folder_left_out`, and the report says why.

**Where they go** is decided when a folder's first item is stored - an empty folder
makes nothing - and kept in the folder's row of the job, so that a later run puts the
rest of the folder where its first items went. The default folders (below) go into the
account's default address book or calendar; every other contacts folder into a book
of its name and every other calendar or tasks folder into a calendar of its own, each
found or made, its name cut to the 255 characters a book's or a calendar's may have. A
folder that cannot have its own goes into the default one and the report's `errors`
say why: the account at its ceiling of 50 address books, the job at its ceiling of 50
calendars made, a name no book may have, or its own refused as often as an item may be
(below), when the item that asked goes there with it.

**Contacts** (`IPM.Contact`, in every contacts folder taken). The folder the root
folder names as the contacts folder (`PidTagIpmContactEntryId`), else the first outside
Deleted Items, goes into the account's default address book; every other into a book
of the folder's name. Each contact is kept as a **vCard 4.0**: `FN`
(the display name, else the given and family names, else the "file as" name), `N`
(family, given, middle, prefix, suffix), `NICKNAME`, `ORG` (company and department),
`TITLE`, up to three `EMAIL` (Email1 to Email3 - an Exchange address by the SMTP
address Outlook keeps beside it, the first marked preferred), `TEL` with vCard 4
types (work, home, cell, fax, pager; the primary, car, assistant and other numbers as
voice), `ADR` for the home, work and other addresses (the mailing address alone where
none of the three is there), `BDAY` (the day `PidLidBirthdayLocal` keeps, else the day
nearest `PidTagBirthday`, with a warning: that is a time in UTC in a zone the file does
not name, and a zone twelve or more hours from UTC puts it a day out), `URL` (the
business and personal home pages), `CATEGORIES`, `NOTE` (the contact's notes, cut to
256 KB with a warning where they are longer), `PHOTO` (the attachment
marked as the contact's picture, a JPEG, PNG or GIF of at most 512 KB, as a `data:`
URI) and `REV`. The card is what `GET /api/v1/me/contacts?fields=card` answers and
what CardDAV serves, byte for byte; the listing reads its birthday, note, telephones
and photo from it. The address book keeps each contact under one e-mail address - its
preferred one - and one address once in an account, so a contact with **no e-mail
address** cannot be kept (`no_address`), nor one whose address the account holds
already as a contact (`already_there`). An address the account only **collected** from
mail it sent, in the book the contact goes into, is adopted as CardDAV adopts one: the
row takes the PST's card and name, and is kept as a contact from then on (one
collected into another book is left there, `already_there`). A **distribution list**
(`IPM.DistList`) is counted (`distribution_list`) and not imported.

**Calendar items** (`IPM.Appointment`, and a meeting request kept in a calendar
folder, in every calendar folder) and **tasks** (`IPM.Task`, in every tasks folder).
The calendar and the tasks folder the root folder names (else the first of each
outside Deleted Items) go into the account's default calendar; every other folder into
a calendar of its own, named after the folder. Each appointment becomes a **VEVENT**:
`SUMMARY`, `LOCATION`, `DESCRIPTION` (cut to 256 KB with a warning where it is
longer), `CATEGORIES`, `CLASS` for a
private or confidential one, `TRANSP` and `X-MICROSOFT-CDO-BUSYSTATUS` from its busy
status, `STATUS:CANCELLED` for a cancelled meeting, `ORGANIZER` and an `ATTENDEE` per
recipient with an SMTP address (required, optional or a resource, with the answer
Outlook tracked), a `VALARM` for its reminder, and a `UID` from its global object id
([MS-OXCICAL]: the UID the id carries, else the id in hex) or, without one, a UID made
from the file and the item. A single appointment's start and end are UTC; an all-day
one is the days it covers in the zone it was made in, where the file names that zone -
where it names none, the day nearest its start in UTC, with a warning, since a zone
twelve or more hours from UTC puts that a day out. A **recurring** one is read from
its recurrence blob ([MS-OXOCAL] 2.2.1.44) and written in its own time zone - the
Windows zone Outlook names (`PidLidAppointmentTimeZoneDefinitionRecur`, else
`PidLidTimeZoneStruct`), carried in the object as a `VTIMEZONE` - so a weekly 09:00
meeting stays at 09:00 across a clock change: an `RRULE` (daily, weekly on its days,
monthly on a day or on the nth weekday, yearly; its interval, its count or its end as
an `UNTIL` in UTC, and `WKST`), an `EXDATE` for each occurrence deleted, and for each
occurrence moved or changed an override with its own `RECURRENCE-ID`, start, end,
subject and location. Outlook's "day 30 of every month" falls on the last day of a
shorter month, and is written so (`BYMONTHDAY=28,29,30;BYSETPOS=-1`). A zone whose
changes Outlook gives as dates of one year (a `wYear` set) is read as those dates every
year, by the `VTIMEZONE` and by the times alike. A blob must describe as many changed
occurrences as it says are changed, as [MS-OXOCAL] requires; one that does not is
`recurrence_unreadable`. Each task
becomes a **VTODO**: `SUMMARY`, `DESCRIPTION`, `DTSTART` and `DUE` as days, `STATUS`
(needs action, in process, completed), `PERCENT-COMPLETE`, `COMPLETED`, `PRIORITY` (1
for high importance, 9 for low), `CATEGORIES`, `CLASS` and a `VALARM` at its reminder
time. Outlook's "no date" (4501-01-01) is no date. Every object is parsed and
summarised as CalDAV parses and summarises what a client sends before it is stored;
one that does not pass is counted `not_storable` with the reason, one whose
recurrence cannot be read or said `recurrence_unreadable` - the whole series, never
a guess at it - and one with no time `no_start`. An object whose UID the calendar
holds already is not stored again (`already_there`). An object is stored under a
resource name made from its UID, as the webmail's are - the letters, digits and
`@._-` kept and every other character made `_`, at most 200 - so two UIDs that differ
only in the characters made `_` make one name: the second is refused by the store as
often as an item may be, and counted `not_saved`. The events are what
`GET /api/v1/me/calendar/events` expands and CalDAV serves; the tasks what
`GET /api/v1/me/tasks` lists.

**Notes** (`IPM.StickyNote`) and **journal entries** (`IPM.Activity`) are filed as
messages, in the mail phase, into a folder of their folder's name (*Notes*, *Journal*,
or what the file calls them): a note as its text, with its subject and date; a journal
entry as its text with its type, start, end, duration, contacts and companies written
above it (*Journal entry: Phone call*, *Start: 2024-03-05 10:00 UTC* ...). Outlook's
RTF copy of a note's or an entry's text is not kept beside it.

**Text.** A contact, an appointment or a task keeps 64 KB of a name or another short
text and 256 KB of its notes or description, each cut at a character with a warning
where it is longer, and leaves out, with a warning, a short text of more than 1 MB or
notes or a description of more than 4 MB - never the item for it. Notes or a
description Outlook kept only as RTF (no plain text beside it) are read through the RTF
reader the mail phase uses, at most 4 MB of it, charged to the budget before it is
decompressed, where the RTF carries plain text or is written in RTF; HTML carried in
RTF is left out, with a warning.

What a contact or an item comes in without - a text cut or left out, a picture that is
not a JPEG, PNG or GIF or is larger than 512 KB, a list of attendees that could not be
read, a day guessed for want of a zone - is named in the report's `errors`, the first
fifty of a run.

### What is thinner, and what is refused

- **Attachments by reference** (a path, a OneDrive or web link) and **OLE objects** are
  not imported; the report names each, with its method, and the message comes in
  without it.
- **An RTF body written in RTF** (neither HTML nor text carried in it) is kept as
  `body.rtf` beside the text, with a warning; the text part is then the RTF's own
  plain text where the message had no other.
- **Meeting requests and reports** (`IPM.Schedule.*`, `REPORT.*`) in a mail folder come
  in as the text Outlook keeps; the iCalendar part of a request and the
  multipart/report of a delivery report are not rebuilt. The appointment Outlook made
  of a request is in the calendar folder, and comes in as an event.
- **Eight-bit text** (`PT_STRING8`) in a code page outside the table below is not
  decoded: its ASCII is kept and every other byte becomes U+FFFD, with a warning.
- **A message larger than the reader takes** is not imported, and is counted
  `too_large` and named: one whose text and data together - its bodies, headers,
  recipients, attachments' names and bytes, and its embedded messages' the same - pass
  128 MB; whose plain, HTML or RTF body alone passes 64 MB; that holds more than 10,000
  attachments and embedded messages, counting those inside its embedded messages; or
  that costs more than its work budget (below). A subject, a header block, an
  identifier or a sender's name past its own limit (64 KB; 4 MB for the headers) is
  left out instead, with a warning (`property_too_large`), and the message comes in
  without it. Of more than 5,000 recipients the first 5,000 are kept
  (`recipients_left_out`); of a recipient's or an attachment's name, at most 64 KB is
  read. The account's message size limit is not asked of archived mail (*Quota*,
  below).
- **A damaged message** - a block whose checksum fails, a table that points outside
  itself - is counted `unreadable` and named, and the rest of its folder comes in.
  A damaged attachment or recipient list leaves the message without it, with a
  warning. A damaged contact, appointment or task is the same: that item alone.
- **A contact with no e-mail address**, or with only an Exchange address the file
  kept no SMTP address for, is not kept (`no_address`): this server's address book
  holds each contact under an address. A contact's second and third addresses are in
  its card; the address book finds it by the first. **Distribution lists** are counted
  (`distribution_list`), not made into contact groups.
- **A changed occurrence of a series** takes its time, subject, location and busy
  status from the recurrence blob; its own body, attendees, reminder and attachments,
  which Outlook keeps in a message embedded in the series, are not read, and it has
  the series' instead. An occurrence made all-day, or no longer all-day, on its own
  is written as its series is. A series is placed by its zone's rule in force when it
  starts; a change to that zone's rules in a later year is not followed. A series on
  a lunar or Hijri calendar is `recurrence_unreadable`: an `RRULE` cannot say it.
  Attachments of appointments and tasks are not imported.
- **Tasks**: a recurring task comes in as its current occurrence (its recurrence is
  not read); "waiting on someone else" and "deferred" are `NEEDS-ACTION`; a task
  request (`IPM.TaskRequest`), and who a task was assigned to, are not read.
- **An ANSI file names no code page for its folders' and its store's names**: they are
  read as Windows-1252; a message's text is read in the code page it names.
- **An appointment, a contact or a task in a mail folder** is counted by class, not
  imported.

A file the reader does not take fails its job at once, with the sentence it says:

| The file | The job says |
|---|---|
| The 4K-page layout (version 36): an OST of Outlook 2013 and later, or a PST in that layout | *This file is in the 4K-page layout (version 36) that Outlook 2013 and later write for an OST - Outlook's offline copy of a mailbox that lives on a server - with 4 KB pages and compressed blocks that Microsoft's published PST format ([MS-PST]) does not describe ...; migrate the mailbox from its server with a migration job, or export its folders to a PST in Outlook.* |
| An OST caching an Exchange mailbox, whose store names its folders by the server's EntryIDs | *This OST - Outlook's offline copy of a mailbox that lives on a server - names its folders by the server's own identifiers rather than by the file's, and how the one maps to the other is not in Microsoft's published PST format ([MS-PST]) ...; migrate the mailbox from its server with a migration job, or export its folders to a PST in Outlook.* |
| Protected with Windows Information Protection | *This PST is encrypted with Windows Information Protection, under a key that only the Windows account and device that protected it can use, so no server can decrypt it; open it in Outlook on that computer, as that account, and export its folders to a new PST saved as a personal file, with the protection removed.* |
| Any other version or encoding | *This PST's format (version N, encoding M) is not one this reader knows ...* |
| A damaged header, or a file shorter than its header says | *The file's header is damaged ...* / *The file is N bytes and its header says M: it was cut short ...; copy it again.* |
| A password, missing or wrong | *This PST is protected with a password; give it as the job's secret.* / *The password given is not this PST's ...* - retry the job with the password (`POST .../jobs/{job}/retry`, `{"secret": "..."}`) |
| Gone when the job runs | *There is no file ... What the job copied stays; put the file back and retry the job to go on from where it stopped.* |

A file **Outlook has open** (or any program holding it without sharing it) pauses the
job instead - *The file is open in Outlook or another program that does not share it;
close it, and the job tries again* - and it is tried again with the usual back-off,
or at once with a retry. The PST password is Outlook's: it is checked against the CRC
the file keeps, as [MS-PST] describes, and it protects nothing in the file itself.
Read the password check as this row's reading of the specification, tested against
files written here: no password-protected PST from Outlook was at hand.

**What is read of the older and the offline files, and why the rest is not.**

- **ANSI files** (Outlook 97 to 2002, `wVer` 14 or 15) are read. [MS-PST] specifies
  them beside the Unicode ones field for field: four-byte BIDs, IBs and NIDs, a
  512-byte header with one CRC, 12-byte page and block trailers with the BID before
  the CRC, narrower B-tree entries and data and subnode trees. Everything above that
  - heaps, property contexts, tables, the store, folders and messages - is the same,
  and a table's rows are packed into the ANSI block's 8,180 bytes. The layout was
  checked against a file written here from [MS-PST] (`ansi.pst`), which libpff - a
  reader written by other hands - also reads whole, attachment and code pages
  included; no ANSI file Outlook wrote was at hand.
- **An OST** (`wMagicClient` "SO") of version 23 - Outlook 2003 to 2010 - or an ANSI
  one has the pages, blocks, heaps and tables [MS-PST] describes; [MS-PST] names only
  the magic apart. It is read as a PST wherever its store names its folders as a PST
  does (24-byte PST EntryIDs, [MS-PST] 2.4.3.2). An OST that caches an Exchange
  mailbox names them instead by the server's own EntryIDs, and [MS-PST] says nothing
  of how those map to the file's nodes; such a file is refused at its store, with the
  sentence above. No OST from Outlook was at hand, so the second case - which is the
  usual one - is this reading of the specification rather than a file seen.
- **The 4K-page layout** (`wVer` 36, what Outlook 2013 and later write for an OST) stays
  refused. [MS-PST] describes 512-byte pages and 8 KB blocks only; a version-36 file
  has 4 KB pages, larger blocks and compressed blocks, and Microsoft has published
  neither their layout nor how they are compressed. What descriptions exist are third
  parties' reverse engineering, which this reader - written from Microsoft's
  specifications alone - does not take, and no such file was at hand to prove a
  reader against.
- **Windows Information Protection** (`bCryptMethod` 0x10, `NDB_CRYPT_EDPCRYPTED` in
  [MS-PST]) stays refused, and nothing else is honestly possible: the file's blocks
  are encrypted with a key Windows keeps for the enterprise identity of the account
  that protected it, which never leaves that account and device. Only Outlook there
  can open it, and export its folders to a PST with the protection removed.
- **A password-protected PST** is read with its password: [MS-PST] keeps the
  password as a CRC-32 in the message store's `PidTagPstPassword`, not as encryption -
  nothing in the file is encrypted with it - and the job asks for it only because the
  file's owner set it.

### The reader's process

The service does not read a PST itself. Each phase of a PST job - the mail, then the
contacts, then the calendar items and tasks - starts this server's own program as the
reader (`hMailServer.exe --pst-read <handle>` on Windows, `hmailserver --pst-read
<descriptor>` on Linux; not a command for an administrator), and asks it for the
store, the folders, a folder's list of messages and each message, a request and a
reply at a time over the reader's standard input and output. The reader opens nothing
by name: it is handed the handle (or descriptor) the job opened and checked against
the domain's import directory, so the file cannot be swapped between the check and
the read, and it has nothing else of the service's - no configuration, no database,
no log, no other handle. It reads with the same reader, limits and work budget as
before (below), rebuilds each message as Internet mail, and writes each contact, event
and task as the vCard or iCalendar text this server keeps; the service files and
stores what it is handed, through the same import and the same stores as ever.

**This is crash containment, not a sandbox.** The reader runs as the service's own
account - on Windows LocalSystem (or the service's virtual account) at the service's
own integrity level, on Linux the service's user - and can reach whatever that account
can reach on the machine. What the process boundary gives is that a defect in the
reader ends the reader and not the service, that a pause or a stop can end it at
once, and the limits below; it does not keep a reader that a hostile file had taken
over away from the machine. What bounds it:

| | Windows | Linux |
|---|---|---|
| Its account | The service's own, SYSTEM or the virtual account, at the service's integrity level; its token has every privilege removed but the one that lets a process walk a path | The service's user; no new privileges, no ambient capability (the service's `CAP_NET_BIND_SERVICE` is not passed on) |
| What it may start | Nothing: a job object of one process | - |
| Memory | 2 GB committed | 4 GB of address space |
| Files | None of the service's handles but the PST's; its errors to the null device | Every descriptor of the service's closed but the PST's; its errors to `/dev/null`; nothing it may write (`RLIMIT_FSIZE` 0), no core file |
| Priority | Below normal | nice 10 |
| Its desktop | The desktop, the clipboard, global atoms and other processes' windows closed to it | - |
| When the service ends | Killed with it (the job object's handle closes) | Killed with it (`PR_SET_PDEATHSIG`), in a process group of its own |

It does not run at a lower integrity level on Windows: the program is a Windows one,
and at low integrity it could not attach to the service's desktop to start at all.
Should a start with those rights fail on a host - the token refused, or the reader
ending before it says hello - one start without them is tried (on Windows still in
a job object of its own where the system allows one, on Linux still in its own
process group and ended with the service),
and if that one works every reader starts so until the service restarts, and `HM7552`
says so once. A reader that cannot be started at all pauses the job (`HM7550`) with
the usual back-off.

**A pause, a cancel, a restart or the service stopping** kills the reader at once -
a watchdog asks the job's stop every tenth of a second while it lives - and the job,
which was waiting for the reader's answer, writes its marks as it always did; the next
run goes on from them with a new reader. The same watchdog ends a reader that has not
answered one request in ten minutes (the work budget below bounds a message well
inside that) or that has not said hello in one minute.

**A reader that ends in the middle of a request** - a crash, a kill for time, an answer
that is not the protocol's - has failed on what it was asked. The service says so in
the error log (`HM7551`, at most ten a phase), starts a new reader, opens the store in
it again with what the file has cost so far - and a whole unit of the work budget more
(4,000,000 reads and 512 MB) for the request it ended in, whose work was never
answered - and asks once more. Ended a second time on
the same thing:

- **a message** is left out, counted `reader_failed` and named, and the job goes on
  from the next message with a new reader; so is **a contact, a calendar item or a
  task**;
- **a folder's list of messages** or of items leaves that folder uncopied, with that
  said;
- **the store or the list of folders** fails the file in the mail phase, and skips the
  phase in the contacts and calendars phases (the mail is copied by then);
- ten messages or items in a row fail the file: it ends the reader on everything, and
  is not read on by starting two readers for each of the rest.

The report's `pst` part counts the readers started again after one ended
(`reader_restarts`), across the job's runs and phases.

**One reader a job, four at once.** A job's phase has one reader at a time, those
started again included; the server runs at most four readers at once, and a PST job
that finds four running waits for one to end, asking its stop meanwhile.

**The test hook.** A file named after the PST with `.reader-test` added, beside it in
the domain's directory - `archive.pst.reader-test` - with lines `end-at 0x<nid>` or
`end-once-at 0x<nid>`, makes the reader end itself, as a crash would, when it is asked
for that message, item or folder: every time, or the first time in each phase. It is
how the regression fixture shows the service surviving a reader's crash, and it is
never there unless an administrator of the server put it there; every phase that finds
it says so in the error log (`HM7553`), naming the file and the node ids. Remove it to
import those messages.

### Resume, the reader's guard, and the report

A folder's messages are taken in the order of their node ids, and the job writes the
last one dealt with after every message, so an interruption anywhere - a restart, a
pause, the service stopping - goes on from the next. A job that ran before also reads
back what it filed, and the import's duplicate check keeps anything from arriving
twice.

What a file can make the reader do is bounded twice over, in its process as it was in
the service.
Every size, offset, count and level is checked against the block it is in and the
limit it is read with before anything is allocated from it, and every tree it walks -
the node and block B-trees, a data tree, a subnode tree, a heap's B-trees, the
folders - has a depth cap and refuses a page, a block or a node it has already been
to in that walk. And because a file can still ask for the same legitimate work again
and again - a table whose every row names one large value, an embedded message that
holds itself - every read is charged to a **work budget**: each page, block, heap
allocation, table cell, subnode, attachment and embedded message read counts one,
with the bytes it decodes. That holds for an ANSI file's pages, blocks and subnode
entries as for a Unicode one's, and for what the contacts and calendars phases read:
each contact, calendar item and task, each attachment looked at for a contact's
picture, the name-to-id map, an appointment's recurrence blob (its bytes, and each
changed occurrence) and its time zones (each rule).

| What is read | Its budget |
|---|---|
| One message read whole, the store opened, one folder of the list of folders, or one folder's list of messages | 4,000,000 reads and 512 MB decoded |
| One contact, calendar item or task read whole (its recurrence and time zones with it), the root folder read for the default contacts, calendar and tasks folders, or one folder's list of items | 4,000,000 reads and 512 MB decoded |
| The whole file, in one run of its job's mail phase - and again in each of its contacts and calendars phases, which open the file anew | 10,000,000 reads and one more for every 4 bytes of the file; 1 GB decoded and 64 bytes more for every byte of the file |
| What a message keeps | 128 MB, 10,000 attachments and embedded messages, 5,000 recipients, 100 warnings of at most 1,024 bytes |
| What a contact, calendar item or task keeps | 64 KB of a name or other short text and 256 KB of notes or a description, cut with a warning (a short text of more than 1 MB, or a body of more than 4 MB, left out with a warning), a 512 KB picture (the first attachment marked as one, each attachment looked at once), 5,000 attendees of at most 1,024 bytes a name, 100 warnings of at most 1,024 bytes; and as iCalendar or a vCard, 1 MB - a series whose changed occurrences, each repeating its description and attendees, would come to more is `too_large`, counted before they are built |
| What the list of folders keeps | 100,000 folders, 16 MB of paths, 1,000 sentences about what could not be read (the report names twenty) |

A message past its budget is counted `too_large` and named, and so is a contact, a
calendar item or a task; a file past its own fails the job - *Reading this file has
cost more work than any PST Outlook writes needs for its size* - with what it copied
kept, and a retry goes on from the marks with a budget of its own. A reader started
again after one ended goes on from what the file had cost the one before, so no file
earns work past its budget by ending the reader: each end is charged as the most one
request may cost, a whole unit (4,000,000 reads and 512 MB), which a large file's
budget has room for many times over and a very small one's for about twice in a
phase before the file is not read further. A pause, a cancel, a restart or the service
stopping ends the reader where it stands, never waiting for a file; the phase that
was reading puts its mark back where the last message or item left it. Memory that
runs short in the reader's process is its own ceiling reached (2 GB on Windows, 4 GB
of address space on Linux), which the same message or item would reach again: it is
counted `too_large`. Memory that runs short in the service is the server's, not the
file's: the mark stays before the message or item, and the job tries again.

What no bound can rule out is a defect in the reader itself that ends its process,
and that ends the reader, not the service (*The reader's process*, above). What still
runs in the service - the filing of each message, the storing of each contact and
event - keeps the guard it had while the reader ran there, as the fallback for a
service that ends for any reason while a message is in hand: before the job opens the
store and lists its folders, before a folder's list of messages and before each
message, it writes where it is, and after, that it got past. A run that finds that mark
still standing takes the thing once more; the second time, it leaves that message out
(`reader_stopped`: the service stopped twice while it was being read or filed), or that
folder, or fails the file, and says so - so that no file can stop the service at every
start. The contacts and calendars phases keep the same marks and the same guard, each
contacts, calendar or tasks folder with a row of its own, so an interrupted phase
goes on from the item after the last one it dealt with and stores nothing twice.

The report is a migration job's (above), and adds `pst`: the file, its size, the
store's name, record key, version (14 or 15 for an ANSI file) and encoding (`none`,
`permute` or `cyclic`), the folders that could not be read (`not_read`), the items
counted and not imported by
class (`not_mail`: the folders that are not mail by their container class, counted
afresh at every pass, and the messages in mail folders that are not mail by their
message class, counted once each - `not_mail_folders` and `not_mail_messages` are the
two parts), the refusal, if the file was refused, `list_attempts`, the guard's
count, and `reader_restarts`, the readers started again after one ended. Every string from the file is cut to a couple of hundred bytes with no control
characters in it, and at most twenty classes are named in each count, the rest
together; the contacts, calendar and tasks folders among `not_mail_folders` are what
the next two phases take. The report's `contacts` and `calendars` parts say what those
phases copied and what they skipped and why (`no_address`, `already_there`,
`distribution_list`, `not_a_contact`, `no_start`, `recurrence_unreadable`,
`not_storable`, `not_an_item`, `too_large`, `unreadable`, `not_saved`, `folder_left_out`,
`reader_failed`, `reader_stopped`), or why a
phase was skipped whole (*the file has no contacts
folder*); each contacts, calendar and tasks folder has the same counts, and the address
book or calendar it went into, as its target. Each folder has its source path, its
target, its special use, the messages the file says it holds, what was copied, what was skipped and why (`duplicate`,
`too_large`, `unreadable`, `not_mail`, `reader_failed`, `reader_stopped`), what a message came in
without (`warnings`: `attachment_not_imported`, `attachment_unreadable`,
`attachments_unreadable`, `recipients_unreadable`, `recipients_left_out`,
`embedded_too_deep`, `rtf_body_kept`, `rtf_unreadable`, `html_charset_unknown`,
`text_not_decoded`, `property_too_large`, `identifier_too_long`,
`categories_unreadable`, `keywords_left_out`, `delivery_time_out_of_range`,
`submit_time_out_of_range`, `date_missing`, `warnings_left_out`) and the renames.

The application log names every message and folder not imported, with the reason -
too large for the reader or for the 2 GB ceiling, unreadable, ended on twice by the
reader, left out by the guard - and the first fifty warnings of a run. The report's `errors` keep the most recent fifty
of those entries, and of the retries, as many as fit in the report. Duplicates (what a
job meets again after an interruption) and items that are not mail are counted, not
named.

### Quota

Each message is filed as an IMAP APPEND is, as archived mail: the 2 GB ceiling (and not
the account's message size limit, which is about mail sent to the account now - old
PSTs hold messages larger than today's limit), the room on the data directory's
volume, and the account's quota. A message the quota has no room for pauses the job
with the account's name and the reason - *the account has no room left under its
quota* - and it is tried again after 1, 5 and 15 minutes and then hourly, up to 24
attempts in all, after which the job fails and a retry resumes it; raise the quota (or
make room) and retry the job to go on at once. No message is filed past the quota by
the check each message makes; the check and the filing are not one step, so two jobs
into one account, or mail arriving meanwhile, can each pass it before the other's
message lands.

`HM7370` reaches the error log when a folder's resume mark could not be written (the
job goes on, and an interruption before the next mark repeats that work without
copying anything twice), and `HM7371` when a message could not be filed for a reason
of this server's - its database, its volume (the job pauses and tries again). The
contacts and calendars phases add two: `HM7410` when one of their resume marks, or
where a folder's items go, could not be written (as `HM7370`), and `HM7411` when a
contact, an appointment or a task could not be saved into the account's address book
or calendar, or its folder's own book or calendar could not be made. Such an item is
**held**: the folder's row keeps it with how often it has been refused, and the job
goes on to the items after it, which show whether the store works.
- **An item after it is stored**: the store works, so each held item is tried once
  more at once and, refused again, is its own refusal - a resource name another UID
  holds, a character a MySQL table cannot keep - counted `not_saved` and named, with
  no pause; `HM7411` says so once a run.
- **The folder ends with items still held**: they are tried once more; one stored
  shows the store works, and the others are counted as above. None stored: the store
  is not working, or proves nothing - as with `HM7371`, their counts go up and the job
  pauses with the job's usual pauses (1, 5 and 15 minutes when nothing else has failed
  before them) and tries them again when it reaches the folder's end.
- At most two items are held at once; a third refused with none stored since pauses
  the job with the mark before it.
- A held item still refused in its fourth run (three pauses) is decided by that count:
  counted `not_saved`, or - where it was its folder's own book or calendar that was
  refused - the folder goes into the default instead, and the item is tried there once
  (refused there too, it is `not_saved`); under Deleted Items such a folder is left
  out, its items counted `folder_left_out`. A database that answers the job's own
  records but refuses every new contact or event for that long - one at its size limit
  - therefore has the items held then counted `not_saved`; the job's own ceiling of 24
  attempts still stands, and a retry goes on from the marks.

Every read, of an item after the mark or of a held one, is first written into the
folder's row with its attempt, so the reader's guard leaves out an item that stopped
the service twice wherever it is read; a held item leaves the row only in the save that
writes its outcome, so no crash, stop or pause loses a held item or its count.

**On MySQL before schema step 6102**, the tables that hold contacts, address books and
a job's folders are in the three-byte `utf8` character set; step 6102 moves them to
`utf8mb4`, and from it MySQL keeps characters outside the Basic Multilingual Plane
like any other. Before it, a contact whose name or notes hold such a character - an
emoji, some CJK - is refused, and counted `not_saved` (at once, where a contact after
it is stored); and a book named with one cannot be made, so its folder goes into the
default book in its fourth run. A folder's name is kept in the job's own record of it
with such a character made U+FFFD, on every database and at every step - the record is
only shown, never looked up - so that no folder's name stops a job on a server of step
6101 or older. Calendars and their objects are `utf8mb4` at every step.

### The code pages

The HTML body's charset, and the text of a `PT_STRING8` property, come from this
table - on Windows through `MultiByteToWideChar`, on Linux through iconv by the name
in its third column, so that a message reads the same on both. An ANSI file keeps
nearly all its text as `PT_STRING8`: a message's in the code page it names, and the
names of its folders and its store - for which it names none - in Windows-1252:

| Code page | Charset | iconv |
|---|---|---|
| 437, 850, 852, 866 | ibm437, ibm850, ibm852, cp866 | CP437, CP850, CP852, CP866 |
| 874 | windows-874 | CP874 |
| 932 | shift_jis | CP932 |
| 936 | gb2312 | CP936 |
| 949 | ks_c_5601-1987 | CP949 |
| 950 | big5 | CP950 |
| 1250-1258 | windows-1250 ... windows-1258 | CP1250 ... CP1258 |
| 20127 | us-ascii | ASCII |
| 20866, 21866 | koi8-r, koi8-u | KOI8-R, KOI8-U |
| 28591-28599, 28603, 28605 | iso-8859-1 ... iso-8859-9, iso-8859-13, iso-8859-15 | ISO-8859-1 ... |
| 50220, 50221, 50222 | iso-2022-jp | ISO-2022-JP |
| 51932, 51949 | euc-jp, euc-kr | EUC-JP, EUC-KR |
| 54936 | gb18030 | GB18030 |
| 65000, 65001 | utf-7, utf-8 | UTF-7, UTF-8 |

### Windows and Linux

The reader and the job are the same code on both, and so is the reader's process:
started from the service's own program file (on Linux by `/proc/self/exe`, so that a
package upgrade that replaces the file on disk still starts the very program the
service runs), given the file's handle or descriptor, and killed on a stop - through a
job object on Windows and a process group on Linux. On Linux the import directory is a
POSIX path and the file's final path is read from `/proc/self/fd`. There is no Import
Tool on Linux, and on Windows the Import Tool does not read PST files: the file goes
into the import directory, and REST, hmctl or the Control Deck start the job. The row
was written without a compiler at hand, and so were its extension of 25 September and
its reader's process; the bench's build and the regression fixture `API.PstImport` are
what prove it on Windows, and no Linux test runs it.

### Outlook over IMAP, still

Outlook itself remains a reader of its own files, and the route for what this one
refuses or leaves out:

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
a comma-separated text file - three fields a line: the account's name without the
domain, its password and its size limit in MB (0 for none) - which is the usual first
step before any of the routes above, since each needs the local account to exist.
