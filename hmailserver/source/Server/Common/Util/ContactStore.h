// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The account's address book, hm_contacts (schema 6032): one row per account
// and address, with a name, a source (0 = added by the user, 1 = collected
// from a message the account sent) and a creation time. Two surfaces read and
// write it - the webmail's /api/v1/me/contacts routes (RestApiContacts.cpp)
// and the CardDAV address book (CardDavServer.cpp) - and this is the one place
// the SQL and the rule for what counts as an address live, so that a contact
// either surface writes is one the other accepts.
//
// Since schema 6079 an account may have several books (AddressBookStore.h) and
// contactbookid says which of them a row is in. A row an upgrade found carries
// 0, which means the account's default book, and is stamped with that book's id
// the first time anything asks the account for its books; ListInBook answers
// the default book with either spelling so that the two cannot disagree while
// the stamping has not happened. Every row is still the ACCOUNT'S: a book
// shared with somebody else holds its owner's rows, and nothing here ever
// writes a row under the account a book was shared with.
//
// The unique index is on (contactaccountid, contactaddress) and stays that way,
// so one account holds one address once ACROSS its books: a contact lives in
// one book and is moved rather than copied.

#pragma once

#include <vector>

namespace HM
{
   struct ContactRecord
   {
      ContactRecord() : id(0), accountId(0), bookId(0), source(0) { }

      __int64 id;
      __int64 accountId;
      __int64 bookId;   // the address book it is in; 0 is the account's default
      String name;
      String address;   // lower-cased; for a group, the marker GroupAddress gives
      int source;
      String created;   // hMailServer system date, YYYY-MM-DD HH:MM:SS

      // What CardDAV keeps beside the name and address (schema 6040): the
      // resource name a client created the contact under - empty for a row the
      // webmail made, which CardDAV serves as <id>.vcf - the card's UID, and the
      // card as the client sent it, empty for a row that never came from a
      // client, for which CardDAV makes a card from the name and address.
      String uri;
      String uid;
      String vcard;
   };

   class ContactStore
   {
   public:
      static const int SourceManual = 0;
      static const int SourceCollected = 1;

      // Every contact of the account, in every one of its books, by name then
      // address. False when the table could not be read. This is what the
      // completion, the groups, the export and the erasure read: they are about
      // the person's whole address book rather than about one book of it.
      static bool List(__int64 accountId, std::vector<ContactRecord> &contacts);

      // The contacts of one book of the account. isDefault says this is the
      // account's default book, in which case the rows an upgrade left at 0 are
      // in it too.
      static bool ListInBook(__int64 accountId, __int64 bookId, bool isDefault, std::vector<ContactRecord> &contacts);

      // How many there are, without reading them: a card is the large part of a
      // row, and the listing of the address books shows a count beside every
      // book's name. False when the table could not be read.
      static bool CountInBook(__int64 accountId, __int64 bookId, bool isDefault, int &count);

      // One contact, which must be the account's own: another account's id is
      // simply not found, so the ids of one address book say nothing about
      // another.
      static bool Get(__int64 accountId, __int64 id, ContactRecord &contact);

      // One contact by id alone, whoever owns it: what a contact of a SHARED
      // book resolves through, the caller having checked the share first.
      static bool GetAny(__int64 id, ContactRecord &contact);

      // Whether the account already has this address, and its row id if so.
      // Across every book, because the index is.
      static bool FindByAddress(__int64 accountId, const String &address, __int64 &id);

      static bool Insert(__int64 accountId, __int64 bookId, const String &name, const String &address, int source, ContactRecord &inserted);

      // The book a contact of the account is in.
      static bool SetBook(__int64 accountId, __int64 id, __int64 bookId);

      // A new name and address for a contact. A card stored for it follows:
      // its FN, N and preferred EMAIL become the name and address, and the
      // rest of the card - the properties this store has no columns for - stays
      // as the client sent it.
      static bool Update(__int64 accountId, __int64 id, const String &name, const String &address);

      // Keeps a collected address as a contact (or the reverse): the source
      // alone changes, the card and the rest stay.
      static bool SetSource(__int64 accountId, __int64 id, int source);

      // The contact a CardDAV client created under this resource name in this
      // book, if any. Two books of one account may hold the same resource name,
      // since each is a collection of its own to the client, so the book is
      // part of the question.
      static bool FindByUri(__int64 accountId, __int64 bookId, bool isDefault, const String &uri, ContactRecord &contact);

      // A contact from a CardDAV client: the card is kept as sent, under the
      // client's resource name and UID; and a card sent for an existing contact,
      // whose name and address it also carries.
      static bool InsertCard(__int64 accountId, __int64 bookId, const String &name, const String &address, const String &uri,
                             const String &uid, const String &vcard, ContactRecord &inserted);
      static bool UpdateCard(__int64 accountId, __int64 id, const String &name, const String &address,
                             const String &uid, const String &vcard);

      // False when there was no such contact of this account.
      static bool Delete(__int64 accountId, __int64 id);

      // A group (ContactGroups.h) is a row like any other, with a card whose
      // kind is group, and it has no address of its own. The address column is
      // the row's identity in this table - unique per account, not null - so a
      // group row holds a marker there instead: "group:" and the card's UID,
      // which no address can be (IsValidAddress refuses it) and which makes
      // two groups with one UID the same conflict two contacts with one
      // address are. Both surfaces tell a group from a contact by this marker,
      // without opening the card.
      static String GroupAddress(const String &uid);
      static bool IsGroupAddress(const String &address);

      // The UID column alone, for a contact that never had one (the webmail's
      // rows) the moment a group needs to refer to it by urn:uuid.
      static bool SetUid(__int64 accountId, __int64 id, const String &uid);

      // How many groups the account's book holds, without reading the cards:
      // the ceiling ContactGroups::MaximumGroups is counted with this, on
      // both surfaces. False when the table could not be read.
      static bool CountGroups(__int64 accountId, int &count);

      // Every group row of every account, for the erasure of an address from
      // the address books it is a member of. False when the table could not
      // be read.
      static bool ListGroupRows(std::vector<ContactRecord> &groups);

      // One address, with a local part and a domain, no whitespace or line
      // breaks, and short enough for the column.
      static bool IsValidAddress(const String &address);

      // "Name <address>", "<address>" or "address" -> the name (may be empty)
      // and the address, lower-cased, without the brackets.
      static void SplitEntry(const String &entry, String &name, String &address);

      // The rule the two surfaces share for a name: trimmed, at most this long.
      static const int MaximumNameLength = 255;
   };
}
