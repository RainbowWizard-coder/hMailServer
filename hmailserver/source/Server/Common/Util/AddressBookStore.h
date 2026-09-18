// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The account's address books and who they are shared with, hm_addressbooks and
// hm_addressbookshares (schema 6079).
//
// Until 6079 an account had one address book: every row of hm_contacts with its
// account id. It now has several, and hm_contacts carries contactbookid saying
// which one a contact is in. The upgrade stamps that column 0 on every existing
// row and migrates nothing, because 0 means "this account's default book" -
// the book EnsureDefault makes the first time anything asks the account for its
// books, and which then stamps the account's 0 rows with its own id. An
// installation nobody has looked at since the upgrade therefore reads exactly
// as it did, and one that has been looked at has no 0 rows left; ListInBook
// answers the default book with either spelling so that the two can never
// disagree even half way through.
//
// THE DEFAULT BOOK is the account's lowest book id - the one EnsureDefault made
// - and not a name, so renaming it is free and a book made later can never
// become it. It cannot be deleted, as the default calendar cannot.
//
// A SHARE hands one book to one other account of this server, or to every
// account of the SHARER'S OWN domain, read-only or writable - a domain-wide
// share cannot be left by the people who get it, so it is only ever aimed where
// the sharer's own domain administrator can take it back, which the route that
// removes a share is scoped to (RestApiAddressBooks.cpp). shareaccountid names the
// account and sharedomainid is 0; sharedomainid names the domain and
// shareaccountid is 0. Exactly one of the two is set, which is why the unique
// index is on all three columns and why neither of them carries a foreign key:
// a column that is 0 half the time cannot have one. What takes the place of the
// keys is UnshareAllForAccount, called when an account goes, and
// UnshareAllForDomain, called when a domain does.
//
// The contacts of a shared book belong to the BOOK'S OWNER, not to the sharee:
// a row of hm_contacts is always the owner's, and what a share changes is who
// may read and write it. Everything that reads contacts therefore reads them
// under the owner's account id, and the sharee's own account id never appears
// on a row of a book somebody else owns.

#pragma once

#include <vector>

namespace HM
{
   struct AddressBookRecord
   {
      AddressBookRecord() : id(0), accountId(0), created(0) { }

      __int64 id;
      __int64 accountId;   // the owner
      String name;         // the display name, unique per account
      __int64 created;     // UTC seconds
   };

   // One book handed to one account, or to a whole domain.
   struct AddressBookShareRecord
   {
      AddressBookShareRecord() : id(0), bookId(0), accountId(0), domainId(0), write(false), created(0) { }

      __int64 id;
      __int64 bookId;
      __int64 accountId;   // the account it is shared with, or 0 for a domain share
      __int64 domainId;    // the domain it is shared with, or 0 for an account share
      bool write;          // may add, change and delete contacts; else read-only
      __int64 created;     // UTC seconds

      // The address or the domain name the share names, when the listing joined
      // it. Empty when it was not asked for.
      String target;
   };

   class AddressBookStore
   {
   public:
      // What EnsureDefault calls the book it makes. Only ever a starting name:
      // the default book is the lowest id, not this string.
      static const char *DefaultName;

      // One account's books, and how many people one book may be shared with.
      // Both are ceilings on the shape rather than on the walk: every listing
      // of the books reads every share of each of them.
      static const int MaximumBooks = 50;
      static const int MaximumShares = 200;
      static const int MaximumNameLength = 255;

      // The account's default book, made the first time it is asked for - and,
      // when it is made, the account's contacts with contactbookid 0 are stamped
      // with its id. False when the table could not be read or the row not made.
      static bool EnsureDefault(__int64 accountId, AddressBookRecord &book);

      // The id alone, for a caller that only needs somewhere to put a contact.
      // 0 when the book could not be made, which every caller treats as "the
      // default book" and so writes a row an EnsureDefault will adopt later.
      static __int64 DefaultBookId(__int64 accountId);

      // Whether this book is the account's default one.
      static bool IsDefault(__int64 accountId, __int64 bookId);

      // One of the account's own books. Another account's id is simply not
      // found, so the ids of one account's books say nothing about another's.
      static bool Get(__int64 accountId, __int64 bookId, AddressBookRecord &book);

      // A book by id alone, whoever owns it: what a share resolves through.
      static bool GetAny(__int64 bookId, AddressBookRecord &book);

      // The account's own books, the default first and the rest in the order
      // they were made. The default is made if the account has none yet, so
      // this is a WRITE the first time it is called for an account: only a
      // route that is already writing calls it.
      static bool ListBooks(__int64 accountId, std::vector<AddressBookRecord> &books);

      // The same listing without making anything: an account nobody has asked
      // for its books yet answers an EMPTY vector rather than a default book.
      // What reads it then says is "the default book, which holds the rows at
      // contactbookid 0" - book id 0, which is what those rows carry and what
      // every other part of this feature already reads as the default book.
      // The administrator's read of somebody's books is this one, because
      // check-role-choke-point.py proves that a route this server calls
      // non-mutating needs only permissions the auditor holds, and an auditor
      // that inserts a row and stamps a column is not an auditor.
      static bool ListBooksReadOnly(__int64 accountId, std::vector<AddressBookRecord> &books);

      // One of the account's own books by name, which is unique per account.
      static bool FindByName(__int64 accountId, const String &name, AddressBookRecord &book);

      // A second, third book: a team's, a project's. False when the name is
      // taken, the ceiling is reached or the row could not be made.
      static bool CreateBook(__int64 accountId, const String &name, AddressBookRecord &created);

      // A new name for one of the account's books, the default included.
      static bool RenameBook(__int64 accountId, __int64 bookId, const String &name);

      // The book with its contacts and its shares. The default book cannot be
      // deleted, and a book that is not the account's own is not found.
      static bool DeleteBook(__int64 accountId, __int64 bookId);

      // A name a book may be called: printable, trimmed, no line breaks.
      static bool IsUsableName(const String &name);

      // The shares of a book, each with the address or domain name it names.
      static bool ListShares(__int64 bookId, std::vector<AddressBookShareRecord> &shares);

      // The books other accounts share with this one - directly, and through
      // its domain - in parallel with the share rows that grant them. A book
      // shared with the account by name as well as through its domain appears
      // once, under the wider of the two rights.
      static bool ListSharedWith(__int64 accountId, __int64 domainId,
                                 std::vector<AddressBookRecord> &books,
                                 std::vector<AddressBookShareRecord> &shares);

      // The share of a book with an account or a domain, if there is one. Pass
      // 0 for the one that does not apply.
      static bool ShareOf(__int64 bookId, __int64 accountId, __int64 domainId, AddressBookShareRecord &share);

      // Share, or change what a share allows; and take a share back.
      static bool Share(__int64 bookId, __int64 accountId, __int64 domainId, bool write);
      static bool Unshare(__int64 bookId, __int64 accountId, __int64 domainId);

      // Every share granted to an account, and every share granted to a domain:
      // what goes when the account or the domain does. Neither column carries a
      // foreign key, so these are what keep the table from outliving its targets.
      static bool UnshareAllForAccount(__int64 accountId);
      static bool UnshareAllForDomain(__int64 domainId);

      // The name of one of the account's books, for the backup, which carries a
      // contact's book as its name rather than as an id a restore reassigns.
      // False when the account has no such book, which the backup reads as "the
      // default book".
      static bool NameOf(__int64 accountId, __int64 bookId, String &name);

      // And back: the id of the account's book with this name, or 0. The restore
      // has put the books back before any contact, so a name it does not find is
      // a hand-edited archive rather than an ordering mistake.
      static __int64 IdOfName(__int64 accountId, const String &name);
   };
}
