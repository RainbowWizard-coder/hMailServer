A second account in the same page
=================================

Two mailboxes side by side in the webmail, which Outlook does and Gmail does not:
the account the person signed in as, and one more, added under **Settings, Other
accounts** with its address and password. Both stand at the top of the folder
column with their unread counts, and one press switches the whole page - the
folders, the list, the reading pane, the From of a new message, the contacts,
the settings - to the other. Nothing is collected from the second account and
nothing is copied: each mailbox stays its own, read live.

This is not the *Other accounts* feature beside it on the same page. That one
collects mail from another mailbox **into** this one on a schedule; this one shows
two mailboxes **beside** each other, and each keeps its own folders, its own
rules and its own sent mail.

Two things are worth saying before the mechanics, because they are what makes this
acceptable and what limits it.

**The second account's session lives in the page, not in the browser.** The first
account's session is a cookie the browser holds and the page never sees. A cookie
is one per origin, so the second account cannot have one too; its session is a
token the page holds in memory and sends itself on that account's requests. That
token is written nowhere - not to `localStorage`, not to `sessionStorage`, not to
a cookie, not into a URL - so reloading the page forgets it, and the person adds
the account again. The Settings card says this in as many words.

**Everything the browser keeps between visits stays the first account's.** The
mail kept for reading offline, the outbox a message waits in with no connection,
the Web Push subscription of this device, the service worker and the installed
app's badge belong to the account the person signed in as, whichever account the
page is showing. The second account's mail never enters the offline store, and
its push routes are never asked. The Settings cards for both say whose they are
while the second account is shown. The smaller things the browser keeps are the
first account's too: the recent searches offered under the empty search box, what
the page has learned about which senders use tracking pixels, the tally behind
the Important marker, and the marks that say which follow-ups and reminders were
announced today. While the second account is shown, nothing is read from those
and nothing is written to them - a search made there is not remembered, the
Important marker does not learn - and what has to be remembered for that account,
which follow-ups and reminders it has already been told about, is held in the page
with its token and goes when the token goes.

**The second account's session is the mailbox and nothing more.** An account
that holds an administrative role - an admin level, or a role assignment - can
administer the server on its own sign-in, whose cookie no script reads. Its
session in the bearer form carries none of that: the server drops the role from
every request made on a bearer, as it drops it for an app password and for a
support session, so the token reaches `/api/v1/me` and `/api/v1/session` and is
refused everywhere else exactly as an ordinary account is. The reason is the one
that keeps the form from the administrator password: a token a page holds in
script is a credential a cross-site script in that page could read, and it must
not be a credential that administers anything.

What the person sees
--------------------

*Adding.* Under Settings, Other accounts, the card **A second account beside this
one** takes an address and a password. The address the page is already signed in
as is refused before any request, and the same account under a spelling the page
cannot know to be the same - its address at an alias of its domain - is refused
by the server, which knows, in the same words on the card; a wrong password is the
server's refusal, said on the card, with the first account's session untouched
either way. On success the card says
what was added and how long its session lasts, and the two accounts appear at the
top of the folder column, each with an avatar, its address, a word saying which
is the one signed in, and the unread count of its inbox. The count of the account
not shown is kept fresh every half minute with the change route, as that account.

*Switching.* A press on the other account is, to the page, a sign-out of the view
and a sign-in of the other: everything the page holds in memory or on the screen
for the account shown goes - the open message, the list, the folders, the draft,
the address book, the unlocked S/MIME keys - and the other account is read whole
under its own credential. A message being written is saved as a draft of the
account it was written in before the credential changes, and a send waiting on
the undo delay goes first. The address bar lands on the other account's inbox.

*What follows the active account.* Folders, the list, search, the reading pane,
compose (From is the active account's identities), contacts, calendar, tasks,
filters, rules, the Security page and every setting: all of it is read through
the same request path, which carries the token when the second account is the
one shown and nothing but the cookie otherwise. An account's second factor is
where it always was - a one-time code is asked for at a password change and at
making an app password, never at sign-in - and both work as the second account
as they do as the first. A message opened in one account is answered from that
account: the switch closes it, so a reply from the other account to it is not a
thing.

*Ending.* **Remove the second account** on the card ends its session on the
server (`DELETE /api/v1/session` with the token) and forgets the token; the first
account is not touched. Signing out ends the second account's session first, with
its token, then the first's; and when the first account's session ends under the
page - it idled out while the second account was being read - the page ends the
second account's session with its token, too, on its way to the sign-in form,
rather than leaving it to stand until it idles out. When the server ends the
second session itself - it idled past the policy, its lifetime ran out, its
password changed, its owner ended it from their own Security page - the page
comes back to the first account, says so, and offers the form again.

*Where the second account sees it.* On that account's own Security page, in its
own webmail, the session is listed with the badge **A second account in a webmail
page**, and can be ended from there like any other.

What is thinner than the row asks
---------------------------------

- **One second account, not several.** While one is added the card shows it and
  not the form; **Remove the second account** ends its session and brings the
  form back for another.
- **A reload forgets it.** This is the price of holding the token in memory and
  not in the browser's storage, and it is paid on purpose: what the browser keeps
  survives the page, and a credential a script can read should not.
- **No pop-out.** A message or a draft opened in a window of its own loads that
  window as a reload of the page would - as the first account - so the two
  pop-out buttons are hidden while the second account is shown.
- **No moving from Gmail or Outlook.com into it from here.** That sign-in leaves
  the page for the provider and comes back to it reloaded, which forgets the
  second account; the two buttons wait while it is shown, with a note saying to
  do it from that account's own sign-in. The other-accounts form on the same card
  works as the second account, since it leaves the page for nothing.
- **A confidential message's link asks for proof.** The reading page a
  confidential link opens (`/confidential/{token}`) authenticates by the cookie
  alone, so a link in one of the second account's messages asks the reader to
  prove who they are rather than opening at once.
- **Nothing of it is kept in the browser.** Its searches are not remembered and
  none are offered; the Important marker learns nothing from it; which follow-ups
  and reminders it was told about is held in the page, with the token.
- **Remote images are not loaded in the second account's messages.** A frame's
  own request carries the cookie, which is the first account's, so the second
  account's HTML is fetched by the page with its token and shown from a blob. A
  document shown that way is under the page's own policy as well as its own, and
  the page allows no remote image; the bar over the message says so instead of
  offering the two buttons. The message's own images (`cid:` parts) show as
  always.
- **A download is fetched by the page.** An attachment, the message source, a
  folder as mbox and the whole account as a zip are links the browser would
  follow with the cookie; for the second account the page fetches them with the
  token and saves them from a blob. The first account's links are untouched.
- **S/MIME keys are unlocked by hand.** The password typed to add the account is
  sent once and not kept, so the second account's keys are not opened at the
  switch; the Security page's unlock box opens them. The same holds for the first
  account after a switch back.
- **The one-session-per-device policy counts it.** A session in the bearer form
  is a session of that account from this device; with the policy on for its
  domain, adding it here ends that account's own session in this browser, and its
  own sign-in later ends this one.

The server's part
-----------------

`POST /api/v1/session?bearer=1` is the one route added, and it is the existing
route with one query parameter. With the account's address and password as HTTP
Basic it answers `201` with `bearer: true`, the `token` in the body, `address`,
`device`, `idle_seconds` and `lifetime_seconds`, and **no `Set-Cookie`**. The
session it starts is in every other respect the one the cookie form starts: the
same table, the same idle timeout and lifetime from the same policy (the
account's domain's, or the server's defaults of thirty minutes and twelve hours),
the same bind-to-address and one-per-device rules, ended by the same password
change and the same device revocation, listed by `GET /api/v1/me/sessions` with
`bearer: true`.

The token is accepted as `Authorization: Bearer <token>` - sixty-four hex digits,
which no API key is, since every key begins with `hmapi_` - and is looked up
exactly as the cookie's value is: the SHA-256 of what was presented compared in
constant time against every row, the policy re-read at each request, the row
dropped when it has expired. A token made for one form is accepted in that form
only: a cookie's value presented as a bearer is refused, and a bearer's value in a
`Cookie` header is refused, each leaving the session standing for the client
that holds it properly. A bearer that is not found is not counted as an
authentication failure, as an expired cookie is not - a page whose second
session ran out is an ordinary event, and counting it would auto-ban the person's
own address.

A request on a bearer session must carry `X-Requested-With: hMailServer` to
change anything, as a request on the cookie must. `DELETE /api/v1/session` with
the bearer ends that session and answers `ended: true, bearer: true` with no
`Set-Cookie`, because the cookie in that browser is another account's. The token
is never logged; the sign-in's log line says only that the account started a
session in the bearer form and from where.

The form is never minted for the administrator: `POST /api/v1/session?bearer=1`
with the administrator password is `403`. The administrator's session is the
Control Deck's cookie, and a token for the administrator held in a page's script
would put the whole server within reach of one cross-site script. For the same
reason a bearer session is the mailbox and nothing more: an account that holds an
administrative role - an admin level, or a role assignment - reaches the
administration API on its password and on its cookie, and never on a bearer. The
server drops the role from every request that arrives on one, as it does for an
app password and for a support session, so the administration routes answer the
bearer `403` exactly as they answer an ordinary account, and `GET /api/v1/session`
on it says `administrative: false` with no permissions and no domains.

The account the request's own cookie stands for is refused the form with `409`.
The page sends the cookie with the Basic sign-in as it sends it with everything,
and the server reads it: when it stands for the account whose password was just
presented - under its own spelling, or under an alias domain's, which the sign-in
resolves as IMAP and POP3 do - the sign-in is refused before the one-per-device
rule could end the cookie's session for it. A cookie that has expired, another
account's, the administrator's, or no cookie at all is not this case.

The message HTML route (`GET /api/v1/me/messages/{id}/html`) now writes its
content security policy into the document as a `<meta>` tag as well as sending it
as a header, so the document is under the same policy wherever it is shown - in a
frame pointed at the route, and in a frame filled from a blob the page fetched.

What it costs, said plainly
---------------------------

A cookie marked `HttpOnly` is a credential a script in the page cannot read; a
token the page holds is one it can. A cross-site scripting flaw in the webmail
would therefore hand an attacker the second account's session, where it would
hand them only the use of the first account's for as long as the page was open.
What makes this acceptable is the policy the webmail is served under: no inline
script, no `eval`, nothing loaded from anywhere but this server
(`RestApiPortal.cpp`), and every value from the server written as text and never
as markup. The token is bound to the same idle and lifetime ceilings as a cookie,
compared in constant time, never logged and never in a URL; a reload forgets it;
and it is never the administrator's.

Where to look
-------------

- `hmailserver/source/Server/Common/Util/RestApiServer.cpp` -
  `HandleSessionCreate_` (the bearer form), `AuthenticateSessionToken_` (one
  lookup for both forms), `Authenticate_` (the bearer branch),
  `HandleSessionDelete_`, `HandleMeSessions_`, `HandleMeMessageHtml_`.
- `hmailserver/source/Server/Common/Util/Portal.js` - the block *A second
  account in the same page*, `call` and `apiFetch` at the top, `forgetView`,
  `renderHtml`, and the `offlineMine` guards in the offline store, the search
  history, the tracker habits, the Important tally and the follow-up and
  reminder marks.
- `hmailserver/source/Server/Common/Util/Portal.html` - the `account-nav`
  column, the Settings card, the notes on the Offline, Notifications and
  Other-accounts cards.
- `build/portal-script-test.js` - the block *A second account in the same page*
  (sixty-seven checks) and the fake server's second account.
- `hmailserver/test/RegressionTests/API/RestApiSecondAccount.cs` - seven tests
  against the live server on port 9666.
