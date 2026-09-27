# The organisation's directory

Outlook calls it the Global Address List, Google calls it the directory: the
people of an organisation, findable by everyone in it, with a title and a
telephone number beside the name. Until now this server's completion knew only
the reader's own address book, so a colleague you had never written to was an
address you had to be told.

The directory is **one domain at a time, and always the reader's own**. There is
no cross-domain form and no unauthenticated form. A server that hosts twenty
unrelated domains must not let one of them read another's names, and a listing
of every address at a domain, handed out without a password, is an address list
for whoever asks.

## What is in it

Every **active** account of the domain whose owner has not opted out, and every
**enabled distribution list** of the domain. The name and address come from
`hm_accounts` and `hm_distributionlists`, which already hold them; everything an
organisation adds on top is a row of `hm_directory` (schema 6078):

| Field | Column | Set by | Width |
|---|---|---|---|
| Job title | `dirtitle` | the administrator or the person | 100 |
| Department | `dirdepartment` | the administrator or the person | 100 |
| Work telephone | `dirphone` | the administrator or the person | 60 |
| Mobile | `dirmobile` | the administrator or the person | 60 |
| Office | `diroffice` | the administrator or the person | 100 |
| Photo | `dirphoto`, `dirphototype` | the administrator or the person | 100 KB decoded |
| Administrator's note | `dirnote` | the administrator only | 255 |
| Opt-out | `diroptout` | **the person**, and the administrator | |

A row is made the first time a field is set, so an account with nothing set has
no row and is listed from `hm_accounts` alone. The row goes when the account
does (`ON DELETE CASCADE`), and it is carried in the backup - the opt-out above
all, because a restore that dropped it would put everybody who had asked to be
left out back into the list without telling them.

An account that is **not active** is not in the directory. It cannot sign in and
mail to it is refused, so listing it would be offering a colleague an address
that bounces.

## The two switches

Both are rows of `hm_settings`, not keys of `hMailServer.ini`, and both are on
by default. The Control Panel has both on Connections & protocols > Webmail, the
What people may do tab, which reads and writes them over the REST API, so the
REST API has to be on.

`DirectoryEnabled` - whether the directory is answered at all. On, as Outlook's
and Google's are. **Turn it off on a server whose one domain holds people who
are not one organisation**: on a free-mail or shared-hosting installation, every
signed-in account being able to read every address at the domain is a spam list
with a password on it. With it off, the listing is empty and says `enabled:
false`, the webmail hides its Directory page and its Settings card, the
completion stops asking, and the CardDAV collection is still there with nothing
in it - so a phone that had subscribed syncs to nothing rather than failing
every poll.

`DirectoryUserEditable` - whether a person may set their own title, department,
telephone numbers and photo from the webmail's Settings page. On. With it off
those are the administrator's alone and the webmail hides the form, saying why.

**The opt-out is never what a switch takes away.** It is a person saying "leave
me out of the list", and it goes through whatever `DirectoryUserEditable` says.

## The opt-out is not invisibility

Out of the listing, out of everybody's To field, out of the Directory page and
out of the CardDAV collection. It does **not** hide the mailbox: mail still
arrives, the address still works, and anybody who already knows it can still
write. The doc says so and every surface that offers the opt-out says so too,
because a person who reads it as "hide me" would be wrong about something that
matters to them.

## The surfaces

**The webmail.** A Directory page (`#/directory`) with a search box and a card
per person - the photo, the name, the address, the title and department, the
telephone numbers and where they sit - with **Write** and **Add to contacts**. A
distribution list is a card too, marked as one, and is offered no "add to
contacts": there is no person behind it. The To, Cc and Bcc completion asks the
address book and the directory together and shows the reader's own contacts
first, then the directory hits, each labelled: the people already known before
the people merely findable, which is the order Outlook and Gmail use. An address
that is both is offered once, as the contact, since that is the row carrying the
name the reader chose. Under Settings, **Your directory entry** is the person's
own fields, their photo and the opt-out.

**REST.** `GET /api/v1/me/directory?q=&limit=` is the listing; `GET` and `PUT
/api/v1/me/directory/me` are the person's own entry; `GET
/api/v1/me/directory/{id}/photo` is one entry's photo and `GET
/api/v1/me/directory/me/photo` the reader's own - answered whether or not they
are listed, because a person who has opted out still has to be able to see the
photo they chose. `GET` and `PUT /api/v1/accounts/{address}/directory` are the
administrator's, with the note, and `GET
/api/v1/accounts/{address}/directory/photo` is the photo for an administrative
page. A credential confined to named domains is scoped by the address in the
path exactly as the account routes are.

**COM.** `Account.DirectoryTitle`, `.DirectoryDepartment`, `.DirectoryPhone`,
`.DirectoryMobile`, `.DirectoryOffice`, `.DirectoryOptOut`, `.DirectoryNote`,
`.DirectoryPhoto` (base64) and `.DirectoryPhotoType` (read-only). The row is
read at the first get and written by `Account.Save()` with the rest, so a
script sets a title and a telephone number in the same breath as a name.

**The Control Panel** has a Directory entry tab in the account dialog, with the
note and the opt-out. (The tab that configures Active Directory authentication,
which used to be called Directory, is now called Windows directory, because two
tabs called Directory in one dialog is a trap.) **The Control Deck** has the
same card in its account editor.

**CardDAV.** One more collection, read-only, beside the account's own address
books and the ones shared with it ([SharedAddressBooks.md](SharedAddressBooks.md)):

```
/dav/addressbooks/<address>/contacts/    the account's own book, read and write
/dav/addressbooks/<address>/book-<id>/   another of its own; shared-<id>/ one shared with it
/dav/addressbooks/<address>/directory/   the organisation's, read-only
/dav/addressbooks/<address>/directory/account-<id>.vcf
/dav/addressbooks/<address>/directory/list-<id>.vcf
```

A PROPFIND on the home lists the directory beside every book, so a client that
walks the tree finds it without being told its address. The collection answers `getctag`, `sync-token`
and the three reports the account's own book answers, so a phone subscribes to
it and polls it in exactly the same way. `current-user-privilege-set` says
`read` and nothing else, and PUT, DELETE and PROPPATCH are refused 403 with the
reason - a client is told the collection is read-only rather than being invited
to try. The cards are generated from the rows: `UID` (stable for the address, so
a person is one card rather than a new one every sync), `FN`, `N`, `EMAIL`, and
the `TITLE`, `ORG`, `TEL` and `PHOTO` the entry has. A distribution list is
written as an organisation card with an empty `N`, under its address: the table
behind it has no name. The photo is read for the card a client actually asks
for and never to answer a poll - see below.

## A photo is judged by its bytes

A JPEG, a PNG or a GIF, at most 100 KB decoded. What is stored is decided by
what the bytes begin with and never by a media type the sender named, and the
type answered when the photo is served is read from the bytes again - a browser
told `image/png` about a file that is not one is exactly the sniffing problem
the `nosniff` header exists for. A photo is never carried inline in the listing:
the completion asks that route on every keystroke, and a hundred photos in one
answer would be megabytes.

The bytes are kept base64 in a text column rather than as a blob. The three
create scripts already carry PEM certificates, armoured keys and whole vCards
that way; the four database backends agree about a text parameter and do not
agree about a binary one.

**One photo is one query, and a listing is none.** Nothing that lists the
directory reads a photograph: the listing selects the media type (which is how
it knows there is one) and never the bytes. `GET /api/v1/me/directory/{id}/photo`
reads that one row, joined to `hm_accounts` on the reader's own domain, active
and not opted out - so the route is scoped by the same three conditions as the
listing without materialising it. The CardDAV collection builds its cards
without `PHOTO` and fills in one card's photo only when a client asks for that
card's body (a GET, or `address-data` in a report); its ETags are computed over
the card without the photo plus the row's `dirupdated`, so the tag a listing
gives and the tag a fetched card carries are the same tag without the listing
having read anything. Otherwise a phone polling `getctag` every few minutes, or
a page drawing a hundred faces, cost the whole domain's photographs each time.

## The opt-out is the one field on the audit trail

`hm_directory` is not an audited table, and says so in
`build/check-audit-choke-point.py`: a job title and a telephone number are
written by the person themselves from the webmail as often as by an
administrator, and decide nothing about what the server does with mail. The
opt-out is not that. Clearing somebody else's puts a person who asked to be left
out back into the listing, the completion, the page and the collection of their
domain - a decision of theirs reversed by another hand - so a change of
`diroptout` through `PUT /api/v1/accounts/{address}/directory` or through
`Account.Save()` over COM is recorded with `AuditTrail::RecordEvent` as
`directory-opt-out-changed`, naming the account and which way it went. The
person's own change from the webmail is not: there is no other hand in it.

## What it deliberately does not do

* **It is one domain.** There is no "every domain on this server" listing and no
  way to ask for another domain's, by any spelling of any route. A server that
  is one organisation with several domains has several directories.
* **No group or office-hierarchy structure.** Outlook's GAL carries a manager, a
  reports-to chain and address-book views; this carries a flat list per domain
  with a department as free text. Sorting people by department is the page's
  search box, not a tree.
* **Nothing is learned from anywhere else.** The entry is typed, by an
  administrator or by the person. There is no import from Active Directory or
  LDAP, even on a server that authenticates against one, and no CSV import of
  entries - `hmctl accounts import` sets accounts, not directory entries.
* **No hmctl verb of its own.** Both routes are reachable with `hmctl api`,
  which is what that verb is for; the row named no command and one was not
  invented.
* **The directory is not searched by an outside client.** There is no LDAP
  server here: a mail client that wants a directory over LDAP is not served, and
  the answer for a phone is the CardDAV collection above.
* **A list is an address, and not even a name.** `hm_distributionlists` has no
  name column at all - a list is an address and the recipients behind it - so a
  list is drawn under its address, and its card's `FN` and `ORG` are that
  address. Its members are not in the directory either, and a list has no photo,
  no title and no opt-out of its own; an administrator who wants one out of the
  directory disables it or leaves the domain's directory off.
* **The completion's ordering is contacts first, then the directory**, and there
  is no ranking by how often the reader writes to somebody.

## What proves it

`RegressionTests.API.RestApiDirectory` on ports 9746 and 9747, with both
listeners up, because the claim is that these are one set of rows: what the
administrator writes over the account route is read by a colleague over the
reader's route and fetched as a vCard under `/dav/`. It covers the
administrator's fields and the person's own, the note that is answered to one
and not the other, the opt-out hiding a person from the listing, the completion
query and the photo route while leaving their own photo reachable and their mail
arriving, a disabled account absent, a distribution list present, an account of
a second domain absent from this domain's directory and present in its own, the
locked server refusing the fields and letting the opt-out through, the switch
off emptying it without breaking a client, the CardDAV collection with its
cards, ETags, ctag, sync report and its 403 on PUT, DELETE and PROPPATCH, and a
photo refused for its bytes and for its size. Four of its assertions are there
because the first review of this row found them missing: a completion query
whose text is not ASCII (`q=Ing%C3%A9nieure`, which a query string decoded with
the machine's code page can never match), the PROPPATCH that the doc and the row
both claimed was refused 403 and was answering a bare 405, the ETag and the
content length of a card with a photo agreeing between the collection listing
and the fetched card, and an administrator clearing somebody's opt-out appearing
on the audit trail. `RegressionTests.API.DirectoryCom` covers the COM
properties. The webmail's harness has twenty-five checks, the Control Deck's has
seven, and `build/check-portal-script.py` holds the page's `img-src` to what its
script actually does - the photos are loaded by URL from this server, and a
policy of `data:` and `blob:` alone forbids every one of them.
