# Shared address books

An account's contacts used to be one list. Since schema 6079 they are kept in
**address books**: the account starts with one, it may make others, and it may
**share** one with another account of this server or with every account of one
of its domains - read-only, or with permission to write. What is shared appears
in the other person's webmail as a second book beside their own, in their To,
Cc and Bcc completion labelled with whose it is, and on their phone as another
CardDAV collection.

This is the team address book: the sales list, the suppliers, the people on
call. It is not a directory of the organisation - that is a different row of the
roadmap, and this one does not read `hm_accounts`.

## What it is made of

| Table | What it holds |
|---|---|
| `hm_addressbooks` | One row per address book: `bookid`, the owner's `bookaccountid`, the name a person chose and when it was made. |
| `hm_addressbookshares` | One row per share: the book, **either** an account id **or** a domain id, whether the share may write, and when it was made. |
| `hm_contacts.contactbookid` | Which book a contact is in. |

`AddressBookStore` (`Common/Util/AddressBookStore.h`) owns the SQL, as
`ContactStore` owns the contacts', so that the webmail, CardDAV and the
administrator's routes cannot disagree about what a book is.

### The default book, and why the upgrade migrates nothing

The upgrade adds `contactbookid` with a default of **0** and moves no row. 0
means *the account's default book*. The default book is made the first time
anything asks an account for its books (`AddressBookStore::EnsureDefault`,
which calls it `Contacts`), and that same call stamps the account's `0` rows
with the new book's id - one statement, once per account, on the first read.
Until it runs, a listing of the default book answers `contactbookid = 0` as
well as its own id, so the two spellings can never disagree half way through.

The consequence is that an installation nobody has looked at since the upgrade
behaves exactly as it did, and none of the four upgrade scripts has to read the
accounts table or run a data migration that the four backends would each spell
differently.

The default book is **the account's lowest book id**, not a name, so renaming it
is free and a book made later can never become it. It cannot be deleted.

### One address, one book

The unique index on `hm_contacts` is `(contactaccountid, contactaddress)` and
stays that way: **one account holds one address once, across all its books**. A
contact lives in one book and is *moved* between books
(`PUT /api/v1/me/contacts/{id}` with `book`), not copied into two. Two different
accounts may of course each have the same address.

### Whose the contacts are

A contact of a shared book belongs to the book's **owner**. Every read and every
write goes through the owner's account id, and the sharee's own id never reaches
a row of somebody else's book. A contact added to a team's book by a colleague
with write permission is the owner's row, and it stays with the book if the
share is taken back.

## Sharing

The owner shares from the **Contacts** page of the webmail: pick the book, press
*Sharing*, and type either an address - `someone@example.com`, one account - or a
domain name - `example.com`, every account of that domain. *Can edit* makes the
share writable; without it the share is read-only.

A domain-wide share names **the sharer's own domain** and no other, and a share
aimed at a different hosted domain is **403**. The reason is that nobody in the
receiving domain asked for the book and nobody in it can leave the share, since
the share was not made to them: what makes that bearable for one's own domain is
that the domain's administrator can remove it, and the route that removes a
share is scoped to the *owner's* domain. Aimed elsewhere, the same share would
put an unleavable book in every Contacts page and every To completion of another
domain, with neither its people nor its own administrator able to take it off.
An account of another domain is shared with **by address** - and can leave.

`hm_addressbookshares` carries `shareaccountid` **or** `sharedomainid`, and the
other is 0. Neither column carries a foreign key, because a column that is 0
half the time cannot have one; what takes the place of the keys is
`UnshareAllForAccount`, called when an account is deleted, and
`UnshareAllForDomain`, called when a domain is. Both are reported rather than
fatal if they fail, as the neighbouring tidy-ups are, and the reason is the
same: ids are reissued, and a row left behind would hand somebody's address book
to whoever takes the number next.

A book shared with an account **and** with that account's domain is one book to
the sharee, under the wider of the two rights.

A sharee may **leave** a book shared with them by name - the share is given
back, nothing of the owner's is touched. A share made to the whole domain is not
one person's to take back, and leaving it is refused with that sentence: only
the owner, or an administrator, removes a domain share.

## The routes

| Route | What it does |
|---|---|
| `GET /api/v1/me/address-books` | Every book the account can see: its own (the default first), then the shared ones. Each carries `id`, `name`, `owner`, `mine`, `writable`, `is_default`, `created`, `contacts`, and for its own books `shares`. |
| `POST /api/v1/me/address-books` | Makes one, by `name`. At most 50 per account; two of one account cannot share a name. |
| `PUT /api/v1/me/address-books/{id}` | Renames it. Only the owner. |
| `DELETE /api/v1/me/address-books/{id}` | The owner's book goes with every contact in it and every share of it; the default book cannot be deleted. A shared book is *left* instead. |
| `GET /api/v1/me/address-books/{id}/shares` | Who it is shared with. Only the owner. |
| `PUT /api/v1/me/address-books/{id}/shares/{target}` | Shares it, or changes what the share allows. `target` is an address, or a domain name that must be the sharer's own domain (403 otherwise); the body's `write` is false by default. At most 200 shares per book. |
| `DELETE /api/v1/me/address-books/{id}/shares/{target}` | Takes the share back. |
| `GET /api/v1/accounts/{address}/address-books` | **Administrator.** An account's books, how many contacts each holds, and who each is shared with. It **makes nothing**: an account that has never asked for its books is reported at the default book it will make, `"id": 0`. |
| `DELETE /api/v1/accounts/{address}/address-books/{id}/shares/{target}` | **Administrator.** Removes one share. Recorded on the audit trail as `address-book-share-removed`. |

The contact routes take a book:

* `GET /api/v1/me/contacts` with **no** `book` answers every book the account
  **owns** - which, with no extra book made, is exactly what it answered before
  6079, and is what the CSV and vCard exports mean by "my contacts".
* `book=<id>` is one book, the account's own or one shared with it.
* `book=all` is every book it can see, its own first, and every contact then
  carries `book_id`, `book_name`, `book_owner`, `book_mine` and `book_writable`.
  This is what the To, Cc and Bcc completion asks for.
* `POST /api/v1/me/contacts` takes `book` (the default when absent), and
  `PUT /api/v1/me/contacts/{id}` takes `book` to move a contact between books of
  one account.

A write into a book shared read-only is **403** on every one of these, and a
book the account cannot see at all is **404** rather than 403, so the ids of one
address book say nothing about another's.

The administrator's two routes are deliberately the shape of the sharing and
nothing more. There is **no** route here that reads the contacts of somebody's
address book: an administrator who needs those has the account export
(`POST /api/v1/accounts/{address}/export`) and the support session, both of
which say what they are and are recorded.

The administrator's *read* also writes nothing. `AddressBookStore::ListBooks`
makes the default book and stamps the account's `0` rows with its id, which is a
write; the administrator route calls `ListBooksReadOnly` instead and reports the
book the account has not made yet at id 0. It needs only `ViewAccounts`, which
the **auditor** role holds, and `check-role-choke-point.py` proves route by route
that a route this server calls non-mutating needs no permission the auditor
lacks - a proof that would still pass while the handler behind it inserted a row.
The `/me` routes do call `ListBooks`, because an account asking for its own books
is what makes them: one insert and one statement, the first time, on its own rows.

A failed read never becomes an answer that means the opposite. If
`hm_addressbookshares` cannot be read, the listing is **500** rather than a list
of the account's own books that reads exactly like a share taken back; a count
that could not be made is 500 rather than `"contacts": 0`, and shares that could
not be read are 500 rather than `"shares": []`. The CardDAV surface does the
same: an unreadable store is 500 and never a 404 on a collection, because a
client told 404 throws its copy of the address book away.

## Over CardDAV

The home collection now holds one collection per book the account can see:

```
/dav/addressbooks/<address>/contacts/      the default book, where it has always been
/dav/addressbooks/<address>/book-<id>/     another book of the account's own
/dav/addressbooks/<address>/shared-<id>/   a book somebody shares with it
/dav/addressbooks/<address>/directory/     the organisation's directory, read-only (Directory.md)
```

A client that has been syncing since 6.3 keeps syncing: its collection is still
`contacts/`, with the same ETags and the same `getctag`.

A shared collection behaves as its share says:

* `current-user-privilege-set` on a read-only one grants `read` and
  `read-current-user-privilege-set` and nothing else, so a client is told before
  it tries.
* `PUT` and `DELETE` on a resource in it are **403** with
  *this address book is shared with you read-only*.
* `DELETE` on the collection itself gives the **share** back and leaves the book
  where it is - what leaving a shared calendar does. A collection the caller can
  see only through a domain share cannot be left this way, and says so.
* `displayname` and `addressbook-description` name the owner, and `owner` is the
  owner's principal rather than the reader's.
* A **group** row in a shared collection is not served, and a group card `PUT`
  into one is **403**. See *No group is shared* below.
* A `PUT` that names an address the owner already holds in **another** of their
  books is **409**, not an adoption: a collected address is taken over by a
  client's card only when the row is already in the book being written, so a
  share is never a way to move one of the owner's contacts into it. The refusal
  carries the other row's URL only when the writer can already see where it is.

`MKCOL` is still refused: a book is made from the Contacts page or over
`/api/v1/me/address-books`, not from a DAV client.

## In the webmail

The Contacts page gains an **Address books** card: a list of the books with how
many contacts each holds, a field to make one, and *Sharing*, which opens the
owner's panel - rename, share with an address or a domain, the shares with
*Remove* beside each, and *Delete this address book*. A book somebody shares
shows whose it is, whether it is writable, and *Leave this address book*.

When the chosen book is shared read-only the add form is gone, *Import* goes with
it, the page says *This address book is shared with you read-only*, and its
contacts offer *Write* and no *Remove* - because the server would refuse the
delete and refuse every card the import posted, and a control that cannot work is
worse than none. *Export as vCard* and *Export as CSV* stay: reading a book
somebody shares with you is the whole point of it.

The To, Cc and Bcc completion asks for every book the reader can see and labels
an entry from a shared book with the book's name, the reader's own first. A
group is never among them, whichever book is chosen: the listing carries none
from a book that is not the reader's own, and the page filters one out again
before drawing it.

## In the backup

`hm_addressbooks` is a **carried** per-account store
(`Common/Application/AccountStores.cpp`), listed **before** `hm_contacts`: a
contact carries the **name** of the book it is in rather than an id the restore
reassigns (the `RoleAddressBook` column role), and the archive is read in the
order it was written, so the books are back before the contacts that name them.
A name the restored account has no book for does **not** drop the contact - it
comes back in the default book, which is also what an archive written before
6079 says by carrying no book at all.

`hm_addressbookshares` is **not** carried, and `AccountStores.cpp` says why: a
share names another account of this server by id, or a whole domain by id,
neither of which an archive restored elsewhere can map, and an id that *is*
restored is reassigned. The books and their contacts come back; the sharing is
set again. The audit trail holds who set it before.

## What it deliberately does not do

* **No group is shared**, in either direction, and this is enforced rather than
  incidental. A contact group (`Contacts.md`) is one account's address book
  entry, and its members are resolved against that account's **whole** address
  book - every book of it, shared or not - because a group made in the webmail
  always goes into the default book while its members may be anywhere. So:
  * a group row in a book somebody shares with you is **not listed** by
    `GET /api/v1/me/contacts` at any `book=`, is **not found** by id on the
    routes that take one, and is **not served** over CardDAV. Listing one would
    answer with the names and addresses of contacts in books that were never
    shared;
  * a group card `PUT` into a book you do not own is **403**, even with write.
    Otherwise a writable share would be a way to *ask* for those rows - plant a
    group naming them by address, or by the UID a row is served under, and read
    the resolved members back - and a way to plant a recipient list the owner's
    own compose would expand by name;
  * the group routes stay scoped to the signed-in account, and a group made in
    the webmail always goes into that account's default book.
* **A writable share does tell the sharee whether the owner already holds an
  address.** A contact is one address per account across all of its books, so a
  card for an address the owner holds elsewhere is refused 409 while a new one is
  taken. That is inherent in the unique index, not something the answer chooses
  to disclose; what the refusal does not say is *where* the other row is.
* **No copy of a contact in two books of one account.** The unique index is on
  the account and the address; a contact is moved rather than copied.
* **A contact is not moved between accounts.** `book` on a contact update must
  name a book of the same account; a contact in a book somebody shares with you
  stays theirs.
* **Deleting a book deletes the contacts in it.** There is no "move them
  somewhere first" and no undo. The webmail asks before it does it, naming the
  book.
* **No per-contact permission.** A share is read-only or writable for the whole
  book, as the calendar's is.
* **No sharing to a group** (`hm_groups`), and no sharing outside this server.
  The target is one account of this server, or the sharer's own domain.
* **The collected addresses are not shared.** What a person merely wrote to
  (`source: collected`) goes into their default book, never into a book they
  share with a team, and CardDAV serves none of it until it is kept.
* **An administrator cannot read a shared book's contacts** from these routes,
  only the shape of the sharing. That is on purpose.
* **`hm_addressbookshares` is not in the backup.** See above.
* **The Control Panel and the Control Deck have no page for this.** The
  administrator's two routes are REST and `hmctl`-less; the sharing is the
  person's own, made in their webmail.

## What is tested

`API/RestApiSharedAddressBooks.cs` runs against a real server on ports 9756 and
9757, with both the REST and the CardDAV listeners up, because the claim is that
they are one store:

* A book made, renamed, listed, and the 51st refused.
* A book shared read-only: the sharee lists it, reads its contacts, and is
  refused 403 when it writes one - over REST and over CardDAV.
* A book shared writable: the sharee adds a contact, and the row belongs to the
  owner.
* Unsharing: the sharee stops seeing the book in both surfaces.
* A domain-wide share seen by another account of that domain, and that account
  refused when it tries to leave it; and a domain-wide share aimed at a
  *different* hosted domain refused 403, while an account of that domain is
  shared with by address and leaves the share itself.
* No group crossing a share: the owner's group, in the very book they shared,
  neither listed to the sharee nor found by id nor served in the sharee's
  CardDAV collection - while the owner's own phone still syncs it - and the
  member it names, which lives in a book that was never shared, nowhere in the
  sharee's answer; and a group card `PUT` into a book shared **writable**
  refused 403 while a plain contact into the same book is taken.
* A card `PUT` into a writable share for an address the owner collected into
  their own default book: 409, the row still where it was, and no path into a
  book the writer cannot see in the refusal.
* The administrator's listing of an account that has never asked for its books:
  `"id": 0`, the same answer twice, and a real id only once the account itself
  asks - which is the auditor invariant, checked from outside.
* The completion (`book=all`) finding a contact of a shared book, labelled.
* CardDAV `PROPFIND` of the home listing `shared-<id>`, its
  `current-user-privilege-set` without `write`, and a `PUT` into it refused 403.
* The backup carrying the books, in `Infrastructure/BackupAccountStores.cs`.

The webmail's harness (`build/portal-script-test.js`) draws the card, shares
with an address and with a domain, removes a share, makes and deletes a book,
leaves a shared one, proves that a read-only book offers no way to change
anything - no add form, no *Remove*, no *Import*, and *Export* still there - and
hands the page a listing that carries a group out of somebody else's book to
prove the page draws it nowhere and offers it in no To field, whatever a server
answers.
