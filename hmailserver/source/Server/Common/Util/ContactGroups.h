// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Contact groups: a group in the address book, addressed by its name in To,
// and served over CardDAV as a vCard whose kind is group.
//
// A group is a row of hm_contacts like any contact, so that the two surfaces
// - the webmail's /api/v1/me/contacts routes and the CardDAV address book -
// list, create, change and delete it through the one store, and a phone that
// syncs the address book gets the group with the people. What makes the row
// a group is its card: KIND:group in a vCard 4.0 (RFC 6350 section 6.1.4)
// or X-ADDRESSBOOKSERVER-KIND:group in a 3.0, which is what Apple's Contacts
// writes, each member a MEMBER (4.0) or X-ADDRESSBOOKSERVER-MEMBER (3.0)
// line: urn:uuid:<the member card's UID>, or mailto:<address> for someone who
// is not a contact. The card is kept as the client sent it, as every card
// here is, and one the webmail makes is written in the 3.0 form, which is the
// one the address book serves for rows without a card and the one both
// Apple's clients and Thunderbird read.
//
// There is no table of memberships and no column for the kind. A group has
// no address of its own, and the address column is the row's identity, so a
// group row holds a marker there - "group:" and the card's UID
// (ContactStore::GroupAddress) - which no address can be and which both
// surfaces read without opening the card. The members are read from the card
// each time they are asked for, against the account's rows already in memory:
// an address book is small, and a cache would be a second truth to keep.
//
// Read once, though, not once per reference. A listing resolves every group
// against one ContactGroupIndex: the rows indexed by UID, by address and by
// id, each card parsed at most once and each row's members resolved at most
// once, so the cost of a listing is the size of the address book and not its
// square. Three ceilings keep the shape of the graph from being the cost:
// MaximumMembers on one card (on both surfaces - the API and a CardDAV PUT),
// MaximumGroups on an account's book, and MaximumExpansionSteps on the whole
// of one listing's expansion, since a book whose groups all name one another
// would otherwise be quadratic however cheap each step is. The last is a stop,
// like MaximumDepth: it cannot be reached by an address book anyone keeps.
//
// A reference that names nothing - a contact deleted by another client, a UID
// this address book never had - is kept in the card, since it is the client's,
// and left out of the answer. A contact deleted here is taken out of every
// group of the account that named it (ForgetMember), and an address erased
// from the server is taken out of every group of every account that named it
// by address (ForgetAddress). A group may be a member of a group; the
// recipients are the people reached through them all, once each, with a
// depth and a visited set so that a card naming itself ends.

#pragma once

#include <memory>
#include <vector>

namespace HM
{
   class Account;
   struct ContactRecord;

   // One member of a group, as the page and the send path see it.
   struct ContactGroupMember
   {
      ContactGroupMember() : id(0), group(false) { }

      __int64 id;            // the contact row, or 0 for an address that is not a contact
      String name;
      String address;        // lower-cased; empty for a member that is itself a group
      bool group;            // a member that is a group of this address book
      AnsiString reference;  // as the card carries it: urn:uuid:<uid> or mailto:<address>
   };

   struct ContactGroup
   {
      ContactGroup() : id(0) { }

      __int64 id;
      String name;
      AnsiString uid;
      String created;
      std::vector<ContactGroupMember> members;      // as the card lists them, those that resolve
      std::vector<ContactGroupMember> recipients;   // every person reached, nested groups expanded, one per address
   };

   // The rows of one address book, indexed for the groups in it. It holds a
   // reference to the vector it was built over and must not outlive it.
   class ContactGroupIndex
   {
   public:
      ContactGroupIndex(const AnsiString &accountAddressUtf8, const std::vector<ContactRecord> &all);
      ~ContactGroupIndex();

      // The indexes and the memory of what has been resolved, defined in
      // ContactGroups.cpp. The pointer is fixed; what it points at is the
      // work already done, which every resolution against this index shares.
      struct Data;
      const std::shared_ptr<Data> data;

   private:
      ContactGroupIndex(const ContactGroupIndex &);
      ContactGroupIndex &operator=(const ContactGroupIndex &);
   };

   class ContactGroups
   {
   public:
      // How many members one group may name - on both surfaces, so that a
      // card a client PUTs cannot hold more than a card the API writes - how
      // many groups one address book may hold, how long a UID may be for the
      // marker to fit its column beside the prefix, how deep a group in a
      // group in a group is followed, and how many member visits the whole of
      // one listing's expansion may cost.
      static const int MaximumMembers = 500;
      static const int MaximumGroups = 200;
      static const int MaximumUidLength = 200;
      static const int MaximumDepth = 8;
      static const int MaximumExpansionSteps = 2000000;

      // Whether the row is a group, by its marker, and how many of the rows
      // are groups - the ceiling above is counted with this.
      static bool IsGroup(const ContactRecord &record);
      static int GroupCount(const std::vector<ContactRecord> &all);

      // One group row resolved against the index the account's rows were put
      // in: the account's address (UTF-8, lower-cased) is what a row without a
      // UID of its own is referred to by, and the index knows it.
      static void Resolve(const ContactGroupIndex &index, const ContactRecord &record, ContactGroup &group);

      // Every group of the account, resolved, in the store's order. The first
      // lists the rows itself and is false when they could not be read.
      static bool List(std::shared_ptr<const Account> account, std::vector<ContactGroup> &groups);
      static void ListFrom(const ContactGroupIndex &index, std::vector<ContactGroup> &groups);

      // What a card writes to refer to a member: urn:uuid:<uid> for a contact
      // of the account - a row that has no UID of its own is given the one it
      // is served under first, so that the reference holds - and
      // mailto:<address> for an address that is not a contact. The first is
      // false when the row could not be written.
      static bool ReferenceForContact(std::shared_ptr<const Account> account, ContactRecord &member, AnsiString &reference);
      static AnsiString ReferenceForAddress(const String &address);

      // Whether a reference names the row: by UID, or by address for a
      // contact that has one.
      static bool ReferenceNames(const AnsiString &accountAddressUtf8, const AnsiString &reference, const ContactRecord &record);

      // A new group, under a UID of its own, with the members named; and a
      // group's name and members written into its card, in the card's own
      // dialect, everything else the card carries kept.
      static bool Create(std::shared_ptr<const Account> account, const String &name, const std::vector<AnsiString> &references, ContactRecord &inserted);
      static bool Save(std::shared_ptr<const Account> account, const ContactRecord &group, const String &name, const std::vector<AnsiString> &references);

      // A contact deleted: every group of the account whose card names it, by
      // UID or by address, is rewritten without it, so that the next sync does
      // not carry a member that is gone. Nothing here fails the delete that
      // called it; a card that could not be rewritten is reported as 6830.
      static void ForgetMember(std::shared_ptr<const Account> account, const ContactRecord &deleted);

      // An address erased from the server: every group of every account that
      // names it as a member - by mailto, bare, or in the brackets of a
      // "Name <address>" a hand-written card may carry - is rewritten without
      // it. The number of groups rewritten; -1 when the table could not be
      // read, when a card could not be rewritten, or when a card still carries
      // the address after the rewrite, so that the erasure's report says the
      // contact groups are not clean rather than claiming they are. Each of
      // those three is reported as 6831 with the group and the account.
      static int ForgetAddress(const String &address);

      // A recipient entry with no address in it - "Family" - that is the name
      // of one of the account's groups becomes that group's recipients, so a
      // group is addressed by its name over the API as it is in the page. The
      // two lists are the ones the send route splits the To field into, and
      // they are kept in step; an entry that is no group's name stays as it
      // was, to be refused as not an address a little later, and so does one
      // that two groups answer to - a name a client made twice is not guessed
      // at, since the sender cannot see which was picked.
      //
      // The first is whether a list holds any entry worth listing the groups
      // for at all; the second expands against groups already listed, so that
      // a send reads the address book once for its three fields rather than
      // three times, and not at all when every entry carries an address.
      static bool NeedsExpansion(const std::vector<String> &entries);
      static void ExpandEntries(const std::vector<ContactGroup> &groups, std::vector<String> &addresses, std::vector<String> &entries);
   };
}
