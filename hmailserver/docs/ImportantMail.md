Important, and sections of the inbox
===================================

Gmail's *importance marker*: a small arrow beside the sender, lit on the mail this
person is likely to want first, and three sections - **Important**, **Starred**,
**Everything else** - the inbox can be drawn in.

Two things about it are worth saying before anything else, because they are what
this feature is and is not.

**It is decided in the browser.** No model is trained anywhere, nothing is learned
on the server, and nothing about who this person reads or writes to is sent to the
server to decide it. The page already knows everything the heuristic uses; it
counts it in `localStorage`, under one key per account, and forgets the whole of it
at sign-out.

**What it decides is one IMAP keyword.** A message the page judges important is
given `$Important`; one the reader says is not is given `$NotImportant`. They are
ordinary keywords on the message, written through the flags route every other flag
goes through, so Thunderbird, a phone and `hmctl` all see them, an IMAP SEARCH finds
them, and the mark survives this browser, this page and this release.

The heuristic
-------------

Each message in the inbox is judged **once**, the first time this session lists it,
and only while the marker is set to decide on its own. Every reason is something the
reader could be told in one clause, and the reasons add up; **two points** make a
message important.

| Reason | Points | What it means |
|---|---|---|
| you marked mail from this sender important | +4 | The reader pressed the marker on a message from this address before, in this browser. |
| you marked mail from this sender not important | -4 | The same, the other way. |
| you write to this sender | +3 | The address is in the account's address book - which includes the contacts the server collected from the mail this account sent - or the reader has written to it from this page since signing in. |
| you answered this sender | +2 | A reply left this page addressed to them. |
| you open mail from this sender | +1 | Two or more messages from them have been opened here. |
| you starred mail from this sender | +1 | A star was put on one of their messages here. |
| it was sent to you by name | +1 | This account, or one of its identities, is named in a To of five or fewer. |
| it is in one of the other tabs | -2 | The tab bar's Social, Promotions, Updates or Forums - the reader's own word for the sender, where they have given one, rather than the server's. |

The reader's own mark is worth two reasons on purpose: it is the one signal that
should outweigh the one thing arguing the other way, so a newsletter the reader
raised stays raised. It is counted **once per mark**, not once per message: raising
a conversation of twenty is one word about that sender, so one *not important*
later is enough to take it back.

*It was sent to you by name* asks the **To** line and not Cc, because the folder
listing carries To and does not carry Cc. A message that names this account only in
Cc therefore does not earn the point, and a mailing that hides its list in Cc is not
denied it - but it is one point of the two, so nothing is marked on it alone.

An address is whatever a stranger wrote in a From header, so it is never looked up
as a property of the object it is looked up in: the address book, the tally and the
per-sender tab words are all asked for an *own* property, and the maps built from
headers have no prototype. A sender calling itself `constructor` or `__proto__` is a
stranger like any other.

Mail the reader sent is never judged and never carries a marker: Sent and Drafts have
no marker at all, no menu item and no keys.

The judgement is always the reader's to overrule
------------------------------------------------

A message that already carries `$Important` or `$NotImportant` is left alone for
good. The page never judges it again - not on the next listing, not in the next
session, not after a sign-out - because either keyword means somebody has already
said what this message is.

The marker can be changed from four places, and all four do the same thing: put one
keyword on and take the other off, on every message of the conversation.

* The arrow on the row, which is a button: pressed, it toasts *Marked as important*
  or *Marked as not important* with an **Undo** that puts back exactly the keywords
  each message had. Where the server takes the change for some of a conversation and
  refuses the rest - a folder the ACL will not let this account write to, a message
  already carrying as many keywords as the server allows - the page says so *and*
  offers the Undo over the messages that did change, rather than reporting a total
  failure over a change that was half made.
* **Mark as important** / **Not important** in the open message's overflow menu, and
  the same entry in the command palette.
* The same entry in a row's right-click menu.
* `+` or `=` marks the open message, or the row under the cursor, important; `-`
  marks it not important. Gmail's keys.

A marker that this page lit says why, in the reader's language, as its tooltip and
as what a screen reader reads out: *Important because you write to this sender.
Click to mark as not important.* A marker the reader lit says only *Important. Click
to mark as not important* - the page does not explain the reader to themselves.

The choice in Settings
----------------------

**Importance marker**, on the Settings page beside the inbox layout:

| Choice | What happens |
|---|---|
| *Decided here from whom I write to and what I open* | The default. The heuristic runs, and the reader can correct it. |
| *Only what I mark by hand* | The marker is on every row and works, but nothing is judged and nothing is counted: the tally is not written to. |
| *No marker* | No marker anywhere - no arrow, no menu entry, no keys, no **Important** in the sidebar, no Important section. Keywords already on messages stay where they are; nothing is removed. |

**Inbox sections** offers *No sections* (the default) and *Important, Starred,
Everything else*.

Both are preferences of the account, written through `PUT /api/v1/me/preferences`
as `important_marker` and `inbox_sections`, so they follow the person between
browsers. There is no new route and no new server setting: the preferences store
attaches no meaning to a key.

The sections, and how they meet the tabs
----------------------------------------

With sections on, the inbox is drawn as up to three folding headings, each with a
count - and, where any of its mail is unread, *12, 3 unread*. A section with nothing
in it is not drawn.

A message goes into the first section that claims it: **Important** if it is marked
important, else **Starred** if it carries a star, else **Everything else**. A
conversation goes where its messages send it - important if any of them is - so a
conversation is never in two places.

**The tabs sort first and the sections divide what the open tab shows.** Under
*Primary*, the sections hold Primary's mail; under *Promotions*, Promotions'. With
the inbox in *Focused and Other*, they divide the open one of those two; with the
inbox in *One list*, they divide the whole of it. Neither replaces the other: the
tabs answer *what kind of mail is this*, from what the sender put in the headers, and
the sections answer *does this matter to me*, from what this reader has done.

The rows under the sections are one list to the keyboard, the cursor and the bulk
selection, exactly as they are without them. A heading is a button: it folds its
section away, keeps its count while folded, and which sections are folded is
remembered in this browser (and only there - it is a view, not a setting).

A section does not come between the list and its rows for a screen reader either:
the box around one is presentational, its heading is a heading, and its rows are a
group that heading names and that the button folds - so the list is still a list of
items, with the count of them a screen reader announces.

**Important** in the sidebar, under Starred, is the search `label:$Important` over
every folder the account may read.

What it does not do
-------------------

* **It does not learn on the server.** Nothing about what this person reads is sent
  anywhere to decide the marker. That also means it does not follow the person to
  another browser: a new browser starts from the address book and the message's own
  recipients, and learns the rest again from what is done in it. The `$Important`
  keywords already on the mail do follow, because they are on the mail.
* **It is not Gmail's model.** Gmail weighs signals this page does not have -
  everyone else's behaviour, the sender's reputation across a whole service, what
  was read and for how long. This is eight explainable reasons and a threshold; it
  will be wrong, which is why every message it marks can be unmarked in one press
  and is then never judged again.
* **It does not sort, hide or delay anything.** Nothing is moved, nothing is
  filtered out of the inbox and no notification is suppressed for being unimportant.
  The marker is a mark, and the sections are an arrangement of the same list.
* **There is no bulk mark.** The toolbar over a selection archives, files, deletes
  and marks read; it has no star and no label on it either, and the marker follows
  that.
* **It does not judge outside the inbox.** Other folders show the marker and can be
  marked by hand, but nothing there is judged automatically.
* **It does not judge what it cannot write back.** A listing drawn from what this
  browser kept for reading offline - because the server cannot be reached - is shown
  and never judged: a marker no request could carry would be one no other client
  would ever see. A mark made while the connection is there does reach both copies
  this browser keeps, the message and the saved listing, so it is on the mail when
  the connection is not.
* **It does not act on the first listing until the address book has been read.**
  Whom the reader writes to is the heaviest reason, so the first inbox listing of a
  session waits for the contacts and then judges.
* **It does not judge everything at once.** At most twenty-five messages are marked
  in one pass, so that opening a folder of thousands does not become a burst of
  requests; the rest are judged on the next listing.
* **The tally is not a record of anything.** It holds one small count per address,
  at most five hundred addresses per heading, the least-counted dropped first, and
  it is removed when the session ends however it ends - a sign-out, or a session the
  server has ended. A session that ends while the page is not open leaves no address
  to name its tally by, so the page sweeps out **every** tally it finds rather than
  the one it can name: on a shared browser nothing of the last person is left behind.

Where it is
-----------

Everything is in the page: `Portal.html` and `Portal.js`, which the server serves
from `PortalPageData.cpp`. There is no C++ change, no schema change, no COM member,
no setting and no new route - the keyword goes through `PUT
/api/v1/me/messages/{id}/flags`, the two choices through `PUT
/api/v1/me/preferences`, and the sidebar's view through `GET /api/v1/me/search`.
Forty-three checks in `build/portal-script-test.js` walk the whole of it against a
recorded server, and two more sit with the offline checks: a fresh session that marks
nothing, mail from a contact marked and saying why, a newsletter to a list left
alone, the reader's correction and its Undo, the reader's mark teaching this browser
and the next message from that sender being marked, the sections with their counts,
their folding and the shape a screen reader is given, the sections under a tab, the
marker switched off, the sign-out that forgets every tally in this browser, the marks
the reader made never being judged again, a sender whose address is the name of a
property every object has being nobody the reader writes to, a conversation counted
once rather than once a message, a conversation the server half refuses still
offering the way back, a listing drawn from the offline store never being judged,
and a keyword reaching both copies this browser keeps.
