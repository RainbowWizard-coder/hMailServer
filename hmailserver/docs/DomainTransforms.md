What a domain does to a message
================================

Three things a domain can be told to do to the mail passing through it: mark
what comes from outside the organisation, say when a sender has never written
before, and put a legal footer on what leaves. All three are per domain, all
three are off on every existing and every new domain, and while they are off
this costs nothing — no extra read of a message, no extra query, no extra write.

They are configured in three places, which are the same three settings:

* the Control Panel's domain dialog, **Tagging and disclaimer**;
* the Control Deck's domain editor, **What this domain does to a message**;
* `PUT /api/v1/domains/{name}` — `external_tag_subject`, `external_tag_header`,
  `external_tag_text`, `first_contact_tip`, `disclaimer_enabled`,
  `disclaimer_plain_text`, `disclaimer_html`.

Over COM they are `Domain.ExternalTagSubject`, `.ExternalTagHeader`,
`.ExternalTagText`, `.FirstContactTip`, `.DisclaimerEnabled`,
`.DisclaimerPlainText` and `.DisclaimerHTML`. Schema 6044 added the seven
columns and the table behind the first-contact note.

Who counts as outside
---------------------

One rule, in one function — `MessageOrigin::IsExternalSender` — because the
answer decides what a reader is told about a stranger, and two copies of it
would eventually disagree. A sender is **outside** when none of these is true:

1. **The session authenticated.** Read from the topmost `Received:` header,
   which is the one this server wrote before the message file existed, and only
   when its `by` clause names this server. `X-AuthUser` is deliberately not
   consulted: the header is added only when the field is absent, so a sender who
   supplies their own keeps it — it is a claim, not a fact.
2. **A trusted incoming relay handed it over.** The address the message was
   received from is in the incoming-relay list. See the sharp edge below.
3. **Every address the message names is at a domain this server hosts** — the
   envelope sender and the `From:` address, each checked against the accounts,
   the active aliases, the domains and the domain aliases. *Either* of them
   being outside is enough: a message whose envelope claims a hosted domain
   while its visible `From` does not is outside, because the disagreement is
   resolved in the direction that warns.

A message with no `From` and a null return path, from an unauthenticated session
and no trusted relay, is outside: nothing about it belongs to this installation.
A bounce **this** server generated is inside, because its `From` is a hosted
postmaster address.

### The sharp edge: an inbound gateway

Condition 2 means that if you run a filtering gateway in front of this server
**and list it as an incoming relay**, nothing arriving through it is ever
tagged. That is the correct reading of the relay list — it is a statement that
the machine is part of your own edge, and it is what lets this server look past
it for the real originating address — but it does disable the tag for every
message a gateway user receives. If you want the tag and you have a gateway,
the choice is between the tag and the relay declaration; there is no third
answer today, and pretending otherwise would be worse than saying so.

### What this is not

It is **not** anti-spoofing. A message that forges a hosted address in both its
envelope and its `From` is judged inside and is not tagged. The control for that
is DMARC with a policy on your own domain, which this server enforces on the way
in. External-sender tagging labels *origin*; it does not authenticate identity,
and no mail system's tag does.

The external-sender tag
-----------------------

Two switches, because they cost different things.

**`external_tag_subject`** puts the tag text at the front of the Subject.
Effective in every client, including ones that show nothing else — and it
**breaks any DKIM signature the sender made over their Subject**, exactly as the
anti-spam subject prefix (`PrependSubject`) has always done, and for exactly the
same reason: the bytes the signature covers change. This is a deliberate repeat
of an existing trade rather than a new one. The inbound DKIM verdict is
unaffected, because the spam tests run during the SMTP conversation and this
runs at delivery; what breaks is any *downstream* re-verification, and the
forwarding case is what ARC exists for.

**`external_tag_header`** stamps `X-hMailServer-External: YES` instead.
Prepending a field is signature-safe — DKIM selects duplicate fields from the
bottom up — so the sender's signature is untouched. The webmail turns it into a
line above the message: *This message came from outside your organisation.*
Prefer this one wherever the reader can show it.

**`external_tag_text`** is the subject tag, at most 100 characters; empty means
the shipped `[EXTERNAL]`. It is per domain so that a domain whose readers do not
read English can say it in the language they do. A subject that already begins
with the tag — a reply coming back from outside — does not collect a second one;
the comparison ignores case.

The tag is applied to **the recipient account's own copy**, at local delivery,
for two reasons. The switch belongs to the *recipient's* domain, which is not
known until the message has been split between its recipients; and a message
addressed both here and to the outside world must not carry the tag out with it.
It runs above the account's rules and its Sieve script, so both can match on the
header, and below the per-account spam settings, so the subject it reads is the
one the reader will see.

Because it rewrites that account's file, a domain with tagging on does not share
one delivered file between several recipients (`DeliveryHardLinks`). That is the
same cost the per-account spam settings already carry.

Nothing stops a sender writing `X-hMailServer-External` themselves. A forged one
can only make their own message look *more* suspicious, never less, so it is not
a hole — but where a domain has turned the feature on, the field is made to mean
what this server says: a forged copy on a message judged inside is removed.

The first-contact note
----------------------

`first_contact_tip` stamps `X-hMailServer-First-Contact: YES` when an **outside**
sender writes to an account that has had mail from other people but never from
them. The webmail shows it as *You do not usually get mail from this sender.*

### Where the memory lives, and what it costs

A table, `hm_knownsenders`: one row per account and sender address, with a count
and two timestamps. Not the message index — `hm_messages` knows what arrived but
not who from, because the sender is in the file rather than in a column, so
answering the question from it would mean opening every message in the mailbox
on every delivery.

Per delivered copy, with the switch on: one indexed `SELECT`, then one `UPDATE`
when the sender is known, or one further `SELECT` and one `INSERT` the first time
a given address writes to a given account. Two round trips in the steady state.
With the switch off: nothing at all — the row is never looked for.

The memory has a second half. When an account that **signed in** sends a message,
each of its recipients is recorded as somebody that account knows, so their reply
is not announced as a first contact. Only for an authenticated session: on an
unauthenticated one the envelope sender is a claim, and believing it would let
anybody seed an account's memory with the addresses they meant to write from and
so silence the note in advance.

### Two honest limits

**A cold start.** A brand-new installation, or the day the switch is turned on,
has no memory, so for a while every sender looks new. The one degenerate case is
suppressed — the very first message an account ever receives carries no note,
because there is nothing for it to be unusual against — but the days after that
are noisy by construction. Microsoft 365's first-contact tip behaves the same way
in a new tenant. Expect a week.

**A restore from backup.** The memory comes back with either kind of restore: a
restore of the database, because it is a table in it, and the built-in restore,
because the archive carries `hm_knownsenders` under each account beside the
address book and the rest (see [Backup.md](Backup.md)). Either way the memory is
as of the backup: senders who first wrote after it are forgotten and their next
message is announced as a first contact again. One redundant note each; nothing
is lost. A restore of the mail store alone — the message files put back with
neither the database nor an archive — has no memory at all and behaves like a
fresh installation, and so does restoring an archive written before the note
existed, which has no memory to give back.

The domain disclaimer
---------------------

`disclaimer_enabled` appends the domain's legal footer to mail leaving the
organisation. `disclaimer_plain_text` and `disclaimer_html` are the two forms;
filling in only the plain one uses it for both, with its line breaks turned into
line breaks in HTML, which is what the domain signature already does.

It runs where the domain signature runs — `OutgoingFooters`, one function that
puts the signature on and then the disclaimer, against the same `MessageData` and
committed by the same single write — because that is the one place on this
server where an outgoing message's body is modified, and a second one would be a
second thing to keep correct. Every way a person sends goes through it (below).
All of them are **before** `DKIMSigner::Sign`, which runs at delivery, so this
server's own signature covers the footer rather than being broken by it.

The footer is appended to the message's `text/plain` part and to its `text/html`
part, in place, wherever those sit in a multipart tree — so a
`multipart/alternative` gets one copy in each and a `multipart/mixed` carrying an
attachment gets it in the text, not after the attachment. A message that has
neither a text nor an HTML part (one attachment and nothing else) is left alone
rather than given a text part it never had.

The one change of shape: when the HTML footer carries an image as a `data:` URL
(`<img src="data:image/png;base64,...">`, a logo), that image is sent as a part of
its own that the HTML names by `cid:`, since Gmail and Outlook do not display a
`data:` image. The HTML part is put inside a `multipart/related` part in its own
place, beside the image — a message that is only HTML becomes that related part,
and an HTML part already inside one has the image added to it — and every `data:`
image in that HTML moves with it, the sender's own included. A footer with no such
image leaves the message's shape as it was. [Signatures.md](Signatures.md)
describes the move, which the signature shares.

### What it is never added to

* **A signed or encrypted message** — `multipart/signed`, `multipart/encrypted`,
  `application/pkcs7-mime`, `application/x-pkcs7-mime`, `application/pgp-encrypted`
  or `application/pgp-signature`. **What happens instead: the message is sent
  exactly as the sender wrote it, with no footer**, and a line naming the message
  and its content type is written to the application log so that an administrator
  whose footer is missing from precisely the mail their finance team signs can
  find out why. There is no option to do otherwise, and there should not be:
  appending to a `multipart/signed` breaks verification in every client that
  checks, and an encrypted part cannot be appended to at all because the server
  cannot read it. A wrapper around the signed part would be a different message
  with a different shape, which is not what "append a footer" means.
* **An automatic reply, a vacation message or a delivery report** — anything
  RFC 3834 marks as auto-submitted.
* **A bounce** — a null envelope sender.
* **A list posting** — `List-Id`, `List-Unsubscribe`, or a `Precedence:` of
  bulk, list or junk.
* **A message with no recipient outside this server.** A footer is a statement to
  the outside world; on a note between two colleagues it is noise. A message
  addressed to *both* an outside partner and a colleague does get one, and the
  colleague sees it too — there is one message and one body.
* **A message this server already footed**, which carries
  `X-hMailServer-Disclaimer` naming the domain that added it.
* **A message relayed in from outside that merely claims a hosted sender
  domain.** The footer goes on mail this installation actually sent:
  authenticated, or from an address that is an account here. Attaching a
  company's legal text to a forgery would be worse than attaching it to nothing.
* **A message over 80 MB** — larger than the server reads a message whole at,
  which a sender can submit only by SMTP (the webmail's send takes 16 MB a
  request at most), and there only where the message size limit was raised past
  that (20 MB by default). Its header alone is read, so the exclusions above are
  decided as for any message, but not its body, and finding the text part to
  append to would mean reading all of it: appending blind at the end of the file
  would put the footer after a multipart message's closing boundary, or inside
  an encoded part, where no client shows it. **What happens instead: the message
  is sent as written, with no footer**, and the application log says so
  (`DisclaimerAdder - Message ... is larger than the 80 MB a message is read
  whole at, so the domain's disclaimer was not appended`). A rule's disclaimer
  is not appended to it either ([MailFlowRules.md](MailFlowRules.md)). Until 13
  October 2026 such a message was not read at all, and the footer was left off
  without a word.

### Once, not once per hop

A reply that quotes a message which already carried the footer does not collect
a second copy. The body is compared with the footer after both have had their
quote markers, their tags and their white space removed, so the `>` prefixes a
plain-text reply adds and the tags an HTML client wraps around it do not hide the
match. A reply that trims the quotation away entirely gets a fresh footer, which
is correct: there is no longer a copy in it.

A footer shorter than eight characters is never treated as already present —
a needle that short would match almost any body, and the administrator would get
no footer at all.

Every way a person sends
------------------------

The disclaimer and the domain signature go on a message however the person sent
it, by the same rules, because all three paths call the same function
(`Common/Util/OutgoingFooters.cpp`):

| Path | Where | Which domain's settings |
|---|---|---|
| A mail client over SMTP | `SMTPConnection::DoPreAcceptMessageModifications_` | the `MAIL FROM` address's |
| The webmail's send (`POST /api/v1/me/messages`), which is also its reply, forward and phishing report | `RestApiServer::HandleMeMessageSend_` | the envelope sender's: the account, or the identity chosen in From |
| The webmail's send-later | `ScheduledMailTask::SendDraft` | the envelope sender's, which is always the account |

The sender's domain is resolved from the address the message is sent from, as
`MAIL FROM` resolves it — the domain aliases applied, then the domain looked up
by the address. It is **the sending address's** domain, not the signed-in
account's: an account's own message gets its own domain's disclaimer and
signature, and a message it sends from an identity at another hosted domain — an
alias there, or an address whose owner granted it the post right — gets that
other domain's, exactly as the same message over SMTP with that address in
`MAIL FROM` would. (Which *account's* signature counts is below.)

Until 15 September 2026 only the first path had either footer: the webmail wrote
its messages into the queue itself, so a domain with the disclaimer on sent every
browser-written message to a partner without it and logged nothing, and the
domain signature had always had the same gap.

On the two webmail paths the footers go on after the message is written and
before its size is taken, so the copy the webmail keeps in the folder designated
`\Sent` is the message as it went, footers included. A mail client over SMTP
keeps its own copy, which never had them.

A draft scheduled with a chosen From at another hosted domain is a sharp edge:
send-later sends with the **account** as the envelope sender, so it gets the
account's domain's footers, where the same message sent at once would get the
chosen address's domain's. That is send-later's envelope, not a rule of the
footers.

### The account's own signature and the webmail

The webmail writes the account's signature into the compose box itself, where
the person sees it and can change it, from the same account settings the server
reads. So on the two webmail paths the server does not add the **account's**
signature a second time; it adds only what the **domain** adds, decided against
the account's signature exactly as over SMTP:

* **Set if not specified in the account** — nothing, when the account has a
  signature; the domain's, when it has none.
* **Append to the account's signature** — the domain's, below the account's
  that the page wrote.
* **Overwrite the account's signature** — the domain's, below the account's.
  This is the one method whose result the webmail cannot match: the page has
  already put the account's signature in a body the person read and sent, and
  the server does not take text out of a message. A domain that means to
  replace every account's signature should also switch the accounts' own off.

The account meant is the one signed in to the webmail, whichever identity the
message is sent from, because that is whose signature the page wrote; its name
fills the macros (`%User.FirstName%` and the rest). Over SMTP it is the account
the `MAIL FROM` address belongs to, if any.

Two consequences of leaving the account's signature to the page, both of which
differ from SMTP:

* **What the page did not write, the server does not add.** The account's
  signature is in the body only when the compose box put it there — on a new
  message, a reply and a forward. A draft opened again in the webmail keeps the
  body it was saved with. A draft written in a desktop client and saved to Drafts
  over IMAP is a draft like any other to the send-later route
  (`POST /api/v1/me/drafts/{id}/schedule`), which does not ask who wrote it, so
  when it is scheduled it goes out with **no account signature**, where the same
  message sent over SMTP would have had one; so does anything else that posts to
  `POST /api/v1/me/messages` without writing the signature into its text, which
  that route's OpenAPI description says. The domain's part is added by its method
  on all of these, as above.
* **The account's signature follows the page, not the domain's two switches.**
  The page puts it on a reply and on a note to a colleague whatever
  `signature_add_to_replies` and `signature_add_to_local_mail` (the Control
  Panel's "Add signature to replies" and "Add signature to local e-mail") say.
  Those switches still govern what the server adds — with replies off, a webmail
  reply gets no domain signature — but they cannot take out what the page wrote.
  Over SMTP they keep both signatures off.

A mail client over SMTP is unchanged in this: the server adds the account's
signature and the domain's by the method, as it always has — except on a signed
or encrypted message, below.

### Signed and encrypted messages get no signature either

Since 15 September 2026 the **signature** follows the disclaimer's rule for a
`multipart/signed`, `multipart/encrypted`, PKCS#7 or PGP message: it is sent as
written, with a line in the application log (`SignatureAdder - Message ... is
signed or encrypted`), and the disclaimer writes a line of its own
(`DisclaimerAdder - Message ...`) when it is on and would otherwise have applied.
Before, the account's or the domain's signature was appended to the text part
inside a `multipart/signed` — breaking the S/MIME signature of every such message
a desktop client sent from an account or a domain with a signature on — and the
webmail's own S/MIME send would have been broken the same way the moment the
signature reached it. This is a change on the SMTP path too, and deliberately: an
installation that relied on a signature inside signed mail will now see none
there, and it belongs in the release notes as a change of behaviour.

### Nor does a message over 80 MB

Since 13 October 2026 the signature follows the disclaimer's rule for a message
larger than the 80 MB the server reads a message whole at (above): its header is
read, so the signature's own conditions are decided as for any message, but the
text it would go after is not, and the message is sent as written, with a line in
the application log (`SignatureAdder - Message ... is larger than the 80 MB a
message is read whole at, so the signature was not appended`). Before, such a
message was not read at all, and it went without its signature and without a
word.

### What is not a person sending

These put mail in the queue as well, and get neither footer, because none of them
carries a message a person wrote:

* the webmail's **read receipt** and **list unsubscribe**, which the server
  composes on a click and marks `Auto-Submitted` itself;
* a script's `Message.Save` over COM;
* automatic replies, Sieve notifications and vacations, quota and virus notices,
  alerts, and the DMARC and TLS reports;
* forwarding, rules, distribution-list postings re-submitted to their members,
  mirroring and quarantine release, which send on a message this server already
  accepted once — with its footers, if it was ours;
* external fetching and import, which bring mail in.

What the webmail shows
----------------------

`GET /api/v1/me/messages/{id}` carries three fields the reader uses:
`external_stamped` (this server's own verdict, from the stamped header),
`first_contact`, and `external` — which stays what it has always been, the
sender's domain against the account's, now also true whenever the server
stamped. The badge beside the sender comes from `external`; the two lines above
the message come from `external_stamped` and `first_contact`, so they appear only
when the recipient's domain asked for them.

Where the code is
-----------------

| | |
|---|---|
| `Common/Util/MessageOrigin.*` | The rule, and its unit tests (`MessageOriginTester`, run from `ClassTester::DoTests`). |
| `Common/Util/ExternalSenderTagger.*` | The tag and the note, applied at local delivery. |
| `Common/Util/DisclaimerAdder.*` | The footer's rules, applied at submission beside the signature. |
| `Common/Util/SignatureAdder.*` | The signature's rules. |
| `Common/Util/OutgoingFooters.*` | The one function that applies both, called by every way a person sends. |
| `Common/Persistence/PersistentKnownSender.*` | `hm_knownsenders`. |
| `SMTP/LocalDelivery.cpp` | Where the tagger is called. |
| `SMTP/SMTPConnection.cpp` | Where the footers are applied to SMTP submission, and the outgoing half of the memory. |
| `Common/Util/RestApiServer.cpp`, `Common/Application/ScheduledMailTask.cpp` | Where the footers are applied to the webmail's send and its send-later. |
| `test/RegressionTests/SMTP/DomainTransforms.cs` | The delivery tests. |
| `test/RegressionTests/API/WebmailFooters.cs` | The footers on the webmail's send and send-later, every exclusion, the account's signature and an identity, two domains kept apart (an identity at the second domain taking the second's), and a signed message on both paths. |
| `test/RegressionTests/API/RestApiDomains.cs` | The REST round trip. |
| `test/RegressionTests/Infrastructure/BackupAccountStores.cs` | The memory through a backup and a restore. |
