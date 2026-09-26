A message that expires
======================

Gmail's *confidential mode*: the sender sets an expiry - an hour, a day, a week, a
month - and the message stops being readable when it passes. The message itself is
not sent. It is kept on this server, and what crosses the internet is a short notice
with a link; the reader proves they are a recipient on the page the link opens, and
the page shows the message with nothing to download, forward, copy or print. The
sender can withdraw it early, and sees when it was first opened and by whom.

What it is, and what it is not
------------------------------

**It is not end-to-end encryption.** A message this server can delete on a timer is a
message this server can read. Anyone who administers the server, or a backup of it,
can read a kept message while it exists. For a message nobody but the recipient can
read, use S/MIME or OpenPGP from the webmail, or the password-protected message
planned in the roadmap - which is end to end because the plaintext is never on this
server, and which therefore cannot expire on a timer.

**It does not stop a determined recipient.** A reader who can see a message can
photograph the screen, and a reader signed in to this server's webmail could save the
page. The reading page offers no download, no forward, no copy button and no print
stylesheet, and it asks browsers not to cache or index it; that is all a page can do.
What this stops is a message being forwarded on by accident, or found in a mailbox
years later by whoever has that mailbox by then.

The webmail says the same beside the option, and the API's description says it too.
A product that implied more would be lying to the sender.

Sending
-------

On the webmail's compose form, **Expires** (never, in an hour, a day, a week, a month)
and, once an expiry is chosen, an optional **Passcode** - a word the sender gives the
readers another way, by phone or in person. Over the API, `expires_in` (seconds from
now, 1 to 7,776,000 - ninety days) and `passcode` (4 to 64 characters) on
`POST /api/v1/me/messages`.

The send is refused when the server has no public address to write the link against
(403, `confidential_unavailable`), and when the message is one the page signed or
encrypted (`mime`), because the reading page could not show it. An `expires_in` that is
not a whole number of seconds - negative, a fraction, a string, `true`, `null` - is
refused with 400 rather than read as "no expiry": a client that wrote the key asked for
an expiry, and sending the message in full, for ever, would be the opposite. So is a
`passcode` without `expires_in`.

The kept copy is the mailbox's, kept elsewhere: it counts against the mailbox's quota
with everything else the account keeps (413, `quota_exceeded`), and an account has at
most 1,000 outstanding at once (400, `confidential_ceiling`), so the data disk is bounded
by the quotas and not by the request rate.

A message that expires is sent now. It cannot be scheduled with *Send at* - the webmail
hides that row while an expiry is chosen and refuses if the button is reached anyway -
because a scheduled draft is sent as it is, and a draft does not keep the expiry: reopen
one and *Expires* is back at *Never*.

What happens to the message:

- The full message - footers, attachments and all - is kept as a file under
  `<data directory>/Confidential/<account id>/<token>.eml`, with a row in
  `hm_confidential` (schema 6069): the sender, the subject, the recipients, the
  expiry, when it was first opened and by whom, and whether it was withdrawn.
- Each recipient (To, Cc and Bcc alike) is sent a **notice** in its place: the same
  From, To, Cc and Subject, and a short text saying who sent a confidential message,
  until when it can be read, and the link `<base URL>/confidential/<token>`. The
  notice goes through the same delivery pipeline as any other message.
- The sender's **Sent** folder gets the full message, not the notice.

The base URL is `confidential_base_url` on `PUT /api/v1/settings` (the setting
`ConfidentialBaseUrl`): the address the webmail is reached at from outside, such as
`https://mail.example.com`. When it is empty, `quarantine_digest_base_url` and then
`import_oauth_redirect_base_url` serve, since they name the same address. With none of
the three set, the webmail does not offer an expiry and the route refuses one.

Reading
-------

The page at `/confidential/<token>` takes no credentials: the token, 256 random bits,
names one message. Before it shows anything, the reader proves they are a recipient in
one of three ways:

- **Signed in to this server's webmail** as one of the recipient addresses: the page
  opens at once. The account's own session, that is - a support session an
  administrator opened as the account is not the recipient reading it, and is asked
  for proof like anyone else, so the sender's record of who opened it stays true.
- **A one-time code** sent to one of the recipient addresses. The reader types an
  address; if it is one the message went to, a six-digit code is mailed there from the
  sender's domain's postmaster. The page answers exactly the same way whether or not
  the address is a recipient's - the same page and the same status on the first ask,
  on a second ask inside a minute, and on a wrong code - and applies the same limits,
  so nothing it says tells a stranger who the message went to. A code works for ten
  minutes and five tries; a new one can be asked for once a minute per address, five
  a day per address, and six an hour per message, to any address; and after thirty
  wrong answers in all, codes stop opening the message until the sender sends it
  again. Those ceilings are what keep a six-digit code from being guessed over a
  ninety-day life, and a link from being used to mail someone a code a minute.
- **The passcode**, when the sender set one. Ten wrong answers pause it for a quarter
  of an hour. It is stored as a PBKDF2 hash with its own salt, so a copy of the table
  does not give up a short passcode offline.

A wrong code or passcode counts against the source address the way a failed logon
does, and trips the same short refusal of that address on this listener.

A proof sets a cookie scoped to the message's own path for half an hour, so the page
can be reopened without proving again. The first opening is recorded with the address
that proved it (or `passcode`), and the sender's list shows it.

The page shows the text (the plain text part, or the text of the HTML when there is
none), HTML-escaped, with the images the message carried shown inline as data the page
itself holds; other attachments are named but not offered. Its headers: a content
security policy that runs no script, fetches nothing and frames nowhere,
`X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, `X-Robots-Tag: noindex`, and
`Cache-Control: no-store`. A print stylesheet hides everything.

After the expiry, or a withdrawal, the page answers 410 and says which; an unknown
token answers 404.

The sender's side
-----------------

**Messages that expire** in the webmail - on the Sent folder's menu, and in the
command palette - lists what was sent with an expiry: the subject, the recipients,
when it expires, whether and by whom it was opened, and a **Withdraw** button that
ends it now.

| Route | What it does |
|---|---|
| `GET /api/v1/me/confidential` | `{available, max_seconds, items:[{id, subject, recipients, created, expires, expired, opened, opened_by, revoked, passcode, link}]}`, newest first; times in seconds since 1970. |
| `POST /api/v1/me/confidential/{id}/revoke` | Withdraws it: the page refuses from now on and the kept copy is deleted. |
| `GET /confidential/{token}` | The reading page; `POST` with `step=code&address=`, `step=verify&address=&code=`, or `step=passcode&passcode=` proves. |

When the copies go
------------------

The kept file is deleted when the expiry passes or the message is withdrawn: the
scheduled-mail task sweeps every minute, and the retention task sweeps too, so
`Utilities.RunMessageRetention` runs it on demand. The row names the file until the
file is gone, so a delete that fails - a reader's page had it open, a scanner held it -
is reported (error 6784) and tried again a minute later, for as long as it takes; the
page refuses a withdrawn or expired message on the row, not on the file. The row stays
thirty days after the expiry so the sender's list still says what happened, and then
goes. The directory of an account that no longer exists is removed by the retention
task's sweep - decided by asking the database for that account, never by a lookup that
came back empty - and no sweep touches a file while the database does not answer.

An address's erasure ([PersonalData.md](PersonalData.md)) takes the address out of the
recipients of a message another account sent, and out of who opened it, which then
reads `[erased]`; the other recipients go on reading it. A message the address sent, or
one left with no recipient, is ended as a withdrawal ends it, and its row goes at once.
A message whose account is under a legal hold is left as it is.

The copies are not in the backup: the sender's own copy is in the mailbox the backup
carries, and a message kept to be deleted at a time is not one to carry into an
archive that outlives it. The table is listed in `AccountStores` under *elsewhere*
with that reason, and **Back up messages** leaves `<data directory>\Confidential`,
where the copies are, out of its copy of the data directory; a restore leaves that
folder where it is. Archives taken by 6.3.4 builds before that folder was left out
do hold the copies - whole, attachments and all, and in the clear even for a mailbox
encrypted at rest - including those that have expired or been withdrawn since, so
treat them as you would the mail itself, or delete them ([Backup.md](Backup.md)).
