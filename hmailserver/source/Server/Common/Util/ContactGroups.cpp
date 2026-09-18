// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See ContactGroups.h.

#include "StdAfx.h"

#include <map>
#include <set>

#include "ContactGroups.h"
#include "ContactStore.h"
#include "AddressBookStore.h"
#include "CardDavServer.h"
#include "VCard.h"
#include "Unicode.h"
#include "GUIDCreator.h"
#include "../Application/ErrorManager.h"
#include "../BO/Account.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int ContactGroups::MaximumMembers;
   const int ContactGroups::MaximumGroups;
   const int ContactGroups::MaximumUidLength;
   const int ContactGroups::MaximumDepth;
   const int ContactGroups::MaximumExpansionSteps;

   namespace
   {
      AnsiString Utf8(const String &text)
      {
         AnsiString out;
         Unicode::WideToMultiByte(text, out);
         return out;
      }

      String Wide(const AnsiString &text)
      {
         String out;
         Unicode::MultiByteToWide(text, out);
         return out;
      }

      AnsiString Lower(const AnsiString &text)
      {
         AnsiString out = text;
         out.ToLower();
         return out;
      }

      String LowerWide(const String &text)
      {
         String out = text;
         out.ToLower();
         return out;
      }

      AnsiString AccountAddressUtf8(std::shared_ptr<const Account> account)
      {
         String address = account->GetAddress();
         address.ToLower();
         return Utf8(address);
      }

      // The UID a row is referred to by: the card's own, or the one a row
      // without a card is served under. The second is a SHA-256, which is why
      // the index below computes each row's once and not once per reference.
      AnsiString UidOf(const AnsiString &accountAddressUtf8, const ContactRecord &record)
      {
         return record.uid.IsEmpty() ? CardDavServer::StableContactUid(accountAddressUtf8, record.id) : Utf8(record.uid);
      }

      bool ParseCard(const ContactRecord &record, std::vector<VCardProperty> &properties)
      {
         if (record.vcard.IsEmpty())
            return false;

         AnsiString problem;
         return VCard::Parse(Utf8(record.vcard), properties, problem);
      }

      // What a reference points at. A bare address is read as one, since a
      // client or an import may write it without the scheme.
      enum ReferenceKind
      {
         ReferenceUid,
         ReferenceAddress,
         ReferenceOther
      };

      ReferenceKind Classify(const AnsiString &reference, AnsiString &target)
      {
         AnsiString lower = Lower(reference);
         lower.TrimLeft();
         lower.TrimRight();
         target = "";

         if (lower.StartsWith("urn:uuid:"))
         {
            AnsiString trimmed = reference;
            trimmed.TrimLeft();
            trimmed.TrimRight();
            target = trimmed.Mid(9);
            return target.IsEmpty() ? ReferenceOther : ReferenceUid;
         }

         if (lower.StartsWith("mailto:"))
         {
            target = lower.Mid(7);
            return target.IsEmpty() ? ReferenceOther : ReferenceAddress;
         }

         if (lower.Find("@") > 0 && lower.Find(":") < 0 && lower.Find("<") < 0)
         {
            target = lower;
            return ReferenceAddress;
         }

         return ReferenceOther;
      }

      // The address a reference carries, whatever shape it is written in: none
      // for a UID, the value for a mailto or a bare address, and the brackets'
      // content for "Name <address>" - which is not a URI and so not a member
      // this address book resolves, but which a hand-written or an imported
      // card may hold, and which an erasure must still take the address out
      // of. Lower-cased; empty when the reference names no address.
      AnsiString AddressInReference(const AnsiString &reference)
      {
         AnsiString text = reference;
         text.TrimLeft();
         text.TrimRight();

         AnsiString lower = Lower(text);
         if (lower.StartsWith("urn:uuid:"))
            return "";
         if (lower.StartsWith("mailto:"))
            lower = lower.Mid(7);

         String name;
         String address;
         ContactStore::SplitEntry(Wide(lower), name, address);
         if (address.Find(_T("@")) <= 0)
            return "";
         return Utf8(LowerWide(address));
      }

      // Whether the text of a card still holds the address as a word of its
      // own - so that an erasure that could not take it out of a member line
      // says so rather than reporting the store clean. The bounds keep
      // alice@example.net from being found inside malice@example.net.
      bool AddressCharacter(char c)
      {
         return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || c == '_' || c == '+' || c == '%' || c == '@';
      }

      bool CardCarriesAddress(const AnsiString &card, const AnsiString &addressLower)
      {
         if (addressLower.IsEmpty())
            return false;

         // Lower-cased once and searched: FindNoCase would copy the card for
         // every occurrence, and a card may be a megabyte.
         const AnsiString text = Lower(card);
         int at = text.Find(addressLower);
         while (at >= 0)
         {
            const bool startsAWord = at == 0 || !AddressCharacter(text.GetAt(at - 1));
            const int after = at + addressLower.GetLength();
            const bool endsAWord = after >= text.GetLength() || !AddressCharacter(text.GetAt(after));
            if (startsAWord && endsAWord)
               return true;
            at = text.Find(addressLower, static_cast<size_t>(at + 1));
         }

         return false;
      }
   }

   // ------------------------------------------------------------------ index

   struct ContactGroupIndex::Data
   {
      Data() : all(nullptr), steps(0) { }

      AnsiString accountAddressUtf8;
      const std::vector<ContactRecord> *all;

      std::map<AnsiString, const ContactRecord *> byUid;      // lower-cased
      std::map<String, const ContactRecord *> byAddress;      // lower-cased, groups left out
      std::map<__int64, const ContactRecord *> byId;

      // Each row's members, resolved the first time they are asked for: a
      // group named by three other groups is parsed once, not three times.
      std::map<__int64, std::vector<ContactGroupMember> > members;

      // What the expansion has cost so far, over every group of this listing.
      int steps;
   };

   ContactGroupIndex::ContactGroupIndex(const AnsiString &accountAddressUtf8, const std::vector<ContactRecord> &all)
      : data(std::shared_ptr<Data>(new Data()))
   {
      data->accountAddressUtf8 = accountAddressUtf8;
      data->all = &all;

      bool anyGroup = false;
      for (size_t i = 0; !anyGroup && i < all.size(); i++)
         anyGroup = ContactStore::IsGroupAddress(all[i].address);

      for (size_t i = 0; i < all.size(); i++)
      {
         const ContactRecord &record = all[i];
         data->byId[record.id] = &record;

         if (!ContactStore::IsGroupAddress(record.address) && !record.address.IsEmpty())
         {
            const String address = LowerWide(record.address);
            if (data->byAddress.find(address) == data->byAddress.end())
               data->byAddress[address] = &record;
         }

         // A book with no group in it is never asked what a UID names, and
         // the stable UID of a row without a card is a SHA-256 each: not
         // computed until something could ask for it.
         if (!anyGroup)
            continue;

         const AnsiString uid = Lower(UidOf(accountAddressUtf8, record));
         if (!uid.IsEmpty() && data->byUid.find(uid) == data->byUid.end())
            data->byUid[uid] = &record;
      }
   }

   ContactGroupIndex::~ContactGroupIndex()
   {
   }

   namespace
   {
      const ContactRecord *FindByUid(const ContactGroupIndex &index, const AnsiString &uid)
      {
         std::map<AnsiString, const ContactRecord *>::const_iterator found = index.data->byUid.find(Lower(uid));
         return found == index.data->byUid.end() ? nullptr : found->second;
      }

      const ContactRecord *FindByAddress(const ContactGroupIndex &index, const AnsiString &addressLower)
      {
         std::map<String, const ContactRecord *>::const_iterator found = index.data->byAddress.find(Wide(addressLower));
         return found == index.data->byAddress.end() ? nullptr : found->second;
      }

      const ContactRecord *FindById(const ContactGroupIndex &index, __int64 id)
      {
         std::map<__int64, const ContactRecord *>::const_iterator found = index.data->byId.find(id);
         return found == index.data->byId.end() ? nullptr : found->second;
      }

      // The members a card names that this address book can answer for,
      // resolved once per row per index.
      const std::vector<ContactGroupMember> &ResolveMembers(const ContactGroupIndex &index, const ContactRecord &record)
      {
         ContactGroupIndex::Data &data = *index.data;

         std::map<__int64, std::vector<ContactGroupMember> >::iterator known = data.members.find(record.id);
         if (known != data.members.end())
            return known->second;

         // Inserted before it is filled, and filled through the reference the
         // map keeps stable: nothing here recurses, but the next caller finds
         // the work already done.
         std::vector<ContactGroupMember> &members = data.members[record.id];

         std::vector<VCardProperty> properties;
         if (!ParseCard(record, properties))
            return members;

         std::vector<AnsiString> references;
         VCard::MemberReferences(properties, references);

         for (size_t i = 0; i < references.size(); i++)
         {
            AnsiString target;
            ReferenceKind kind = Classify(references[i], target);

            ContactGroupMember member;
            member.reference = references[i];

            if (kind == ReferenceUid)
            {
               // A reference that names nothing, or the group itself, is not
               // a member; it stays in the card, which is the client's.
               const ContactRecord *found = FindByUid(index, target);
               if (!found || found->id == record.id)
                  continue;

               member.id = found->id;
               member.name = found->name;
               member.group = ContactStore::IsGroupAddress(found->address);
               if (!member.group)
                  member.address = found->address;
            }
            else if (kind == ReferenceAddress)
            {
               const ContactRecord *found = FindByAddress(index, target);
               if (found)
               {
                  member.id = found->id;
                  member.name = found->name;
                  member.address = found->address;
               }
               else
                  member.address = Wide(target);
            }
            else
               continue;

            members.push_back(member);
         }

         return members;
      }

      // Every person reached through the group and the groups in it, once
      // each by address; a visited set and a depth end a card that names
      // itself through another, and the index's budget ends a book whose
      // groups all name one another before it costs its square.
      void Flatten(const ContactGroupIndex &index, const ContactRecord &record, std::set<__int64> &visited, std::set<String> &seen,
                   int depth, std::vector<ContactGroupMember> &recipients)
      {
         if (depth > ContactGroups::MaximumDepth || !visited.insert(record.id).second)
            return;

         ContactGroupIndex::Data &data = *index.data;
         const std::vector<ContactGroupMember> &members = ResolveMembers(index, record);

         for (size_t i = 0; i < members.size(); i++)
         {
            if (++data.steps > ContactGroups::MaximumExpansionSteps)
               return;

            if (members[i].group)
            {
               const ContactRecord *inner = FindById(index, members[i].id);
               if (inner)
                  Flatten(index, *inner, visited, seen, depth + 1, recipients);
               continue;
            }

            if (members[i].address.IsEmpty())
               continue;
            if (!seen.insert(LowerWide(members[i].address)).second)
               continue;

            recipients.push_back(members[i]);
         }
      }

      // The references of a card, less those the test says to drop. True when
      // any was dropped.
      bool Without(const ContactRecord &group, const AnsiString &accountAddressUtf8, const ContactRecord *deleted, const AnsiString &addressLower,
                   std::vector<AnsiString> &kept)
      {
         kept.clear();
         std::vector<VCardProperty> properties;
         if (!ParseCard(group, properties))
            return false;

         std::vector<AnsiString> references;
         VCard::MemberReferences(properties, references);

         bool dropped = false;
         for (size_t i = 0; i < references.size(); i++)
         {
            bool drop = false;
            if (deleted)
               drop = ContactGroups::ReferenceNames(accountAddressUtf8, references[i], *deleted);
            else
            {
               // Every shape that carries the address, not only the two the
               // resolution reads: an erasure takes the address out of the
               // card whether or not the member line was ever a member.
               drop = !addressLower.IsEmpty() && AddressInReference(references[i]) == addressLower;
            }

            if (drop)
               dropped = true;
            else
               kept.push_back(references[i]);
         }

         return dropped;
      }

      // A display name in a header entry: quoted when it carries a character
      // that would otherwise split or restructure the entry.
      String HeaderEntry(const String &name, const String &address)
      {
         if (name.IsEmpty())
            return address;

         bool plain = true;
         for (int i = 0; plain && i < name.GetLength(); i++)
         {
            wchar_t c = name[i];
            plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == ' ' || c == '.' || c == '_' || c == '-' || c == '\'' || c > 127;
         }

         if (plain)
            return name + _T(" <") + address + _T(">");

         String quoted = name;
         quoted.Replace(_T("\\"), _T("\\\\"));
         quoted.Replace(_T("\""), _T("\\\""));
         return String(_T("\"")) + quoted + _T("\" <") + address + _T(">");
      }
   }

   bool
   ContactGroups::IsGroup(const ContactRecord &record)
   {
      return ContactStore::IsGroupAddress(record.address);
   }

   int
   ContactGroups::GroupCount(const std::vector<ContactRecord> &all)
   {
      int groups = 0;
      for (size_t i = 0; i < all.size(); i++)
      {
         if (IsGroup(all[i]))
            groups++;
      }
      return groups;
   }

   void
   ContactGroups::Resolve(const ContactGroupIndex &index, const ContactRecord &record, ContactGroup &group)
   {
      group = ContactGroup();
      group.id = record.id;
      group.name = record.name;
      group.uid = UidOf(index.data->accountAddressUtf8, record);
      group.created = record.created;
      group.members = ResolveMembers(index, record);

      std::set<__int64> visited;
      std::set<String> seen;
      Flatten(index, record, visited, seen, 0, group.recipients);
   }

   void
   ContactGroups::ListFrom(const ContactGroupIndex &index, std::vector<ContactGroup> &groups)
   {
      groups.clear();

      const std::vector<ContactRecord> &all = *index.data->all;
      for (size_t i = 0; i < all.size(); i++)
      {
         if (!IsGroup(all[i]))
            continue;

         ContactGroup group;
         Resolve(index, all[i], group);
         groups.push_back(group);
      }
   }

   bool
   ContactGroups::List(std::shared_ptr<const Account> account, std::vector<ContactGroup> &groups)
   {
      groups.clear();
      if (!account)
         return false;

      std::vector<ContactRecord> all;
      if (!ContactStore::List(account->GetID(), all))
         return false;

      ContactGroupIndex index(AccountAddressUtf8(account), all);
      ListFrom(index, groups);
      return true;
   }

   bool
   ContactGroups::ReferenceForContact(std::shared_ptr<const Account> account, ContactRecord &member, AnsiString &reference)
   {
      reference = "";
      if (!account)
         return false;

      // A collected address - one the person wrote to and has not kept - is
      // named by its address and not by a UID, because the CardDAV address
      // book does not serve such a row: a urn:uuid pointing at it would be a
      // reference the phone could never resolve. The page still shows the
      // contact's name, since a mailto is resolved against the account's rows
      // whatever their source.
      if (member.source == ContactStore::SourceCollected && !member.address.IsEmpty())
      {
         reference = ReferenceForAddress(member.address);
         return true;
      }

      if (member.uid.IsEmpty())
      {
         // The UID the row is served under already; written down so that the
         // reference holds whatever the account is called later.
         String stable = Wide(CardDavServer::StableContactUid(AccountAddressUtf8(account), member.id));
         if (!ContactStore::SetUid(account->GetID(), member.id, stable))
            return false;
         member.uid = stable;
      }

      reference = "urn:uuid:" + Utf8(member.uid);
      return true;
   }

   AnsiString
   ContactGroups::ReferenceForAddress(const String &address)
   {
      String lower = address;
      lower.TrimLeft();
      lower.TrimRight();
      lower.ToLower();
      return "mailto:" + Utf8(lower);
   }

   bool
   ContactGroups::ReferenceNames(const AnsiString &accountAddressUtf8, const AnsiString &reference, const ContactRecord &record)
   {
      AnsiString target;
      switch (Classify(reference, target))
      {
      case ReferenceUid:
         return UidOf(accountAddressUtf8, record).CompareNoCase(target) == 0;
      case ReferenceAddress:
         return !ContactStore::IsGroupAddress(record.address) && record.address == Wide(target);
      default:
         return false;
      }
   }

   bool
   ContactGroups::Create(std::shared_ptr<const Account> account, const String &name, const std::vector<AnsiString> &references, ContactRecord &inserted)
   {
      inserted = ContactRecord();
      if (!account)
         return false;

      // A version 4 UUID in the lower-case, brace-less form a card carries.
      String guid = GUIDCreator::GetGUID();
      guid.Replace(_T("{"), _T(""));
      guid.Replace(_T("}"), _T(""));
      guid.ToLower();
      if (guid.IsEmpty())
         return false;

      AnsiString uid = Utf8(guid);
      AnsiString card = VCard::GenerateGroup(uid, Utf8(name), references);

      // A group is made in the account's default book. Its members are resolved
      // against the whole address book rather than against one book of it, so
      // which book holds the group row decides only where a CardDAV client
      // finds the card - and is why no group is ever read or written through a
      // SHARE: a group of a book somebody shares would answer with the names
      // and addresses of rows in the books they did not share, so the listing
      // leaves it out and a group card PUT into somebody else's book is refused
      // (RestApiContacts.cpp, CardDavServer.cpp, SharedAddressBooks.md).
      const __int64 bookId = AddressBookStore::DefaultBookId(account->GetID());
      return ContactStore::InsertCard(account->GetID(), bookId, name, ContactStore::GroupAddress(guid), String(), guid, Wide(card), inserted);
   }

   bool
   ContactGroups::Save(std::shared_ptr<const Account> account, const ContactRecord &group, const String &name, const std::vector<AnsiString> &references)
   {
      if (!account)
         return false;

      AnsiString uid = UidOf(AccountAddressUtf8(account), group);
      AnsiString card = VCard::WithGroup(Utf8(group.vcard), uid, Utf8(name), references);
      return ContactStore::UpdateCard(account->GetID(), group.id, name, group.address, Wide(uid), Wide(card));
   }

   void
   ContactGroups::ForgetMember(std::shared_ptr<const Account> account, const ContactRecord &deleted)
   {
      if (!account)
         return;

      std::vector<ContactRecord> all;
      if (!ContactStore::List(account->GetID(), all))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6830, "ContactGroups::ForgetMember",
            "The address book of account " + StringParser::IntToString(account->GetID()) + " could not be read after contact " +
            StringParser::IntToString(deleted.id) + " was deleted; a group naming it keeps the reference, which the listing leaves out.");
         return;
      }

      AnsiString accountAddressUtf8 = AccountAddressUtf8(account);
      for (size_t i = 0; i < all.size(); i++)
      {
         if (!IsGroup(all[i]) || all[i].id == deleted.id)
            continue;

         std::vector<AnsiString> kept;
         if (!Without(all[i], accountAddressUtf8, &deleted, "", kept))
            continue;

         if (!Save(account, all[i], all[i].name, kept))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6830, "ContactGroups::ForgetMember",
               "Group " + StringParser::IntToString(all[i].id) + " of account " + StringParser::IntToString(account->GetID()) +
               " could not be rewritten without contact " + StringParser::IntToString(deleted.id) + "; it keeps the reference, which the listing leaves out.");
         }
      }
   }

   int
   ContactGroups::ForgetAddress(const String &address)
   {
      std::vector<ContactRecord> groups;
      if (!ContactStore::ListGroupRows(groups))
         return -1;

      String lower = address;
      lower.TrimLeft();
      lower.TrimRight();
      lower.ToLower();
      AnsiString wanted = Utf8(lower);
      if (wanted.IsEmpty())
         return 0;

      int rewritten = 0;
      bool left = false;
      for (size_t i = 0; i < groups.size(); i++)
      {
         std::vector<AnsiString> kept;
         const bool dropped = Without(groups[i], "", nullptr, wanted, kept);

         AnsiString card = Utf8(groups[i].vcard);
         if (dropped)
         {
            // The UID is the card's own, or the marker's tail for a row whose
            // card lost it: no account address is needed here.
            AnsiString uid = groups[i].uid.IsEmpty() ? Utf8(groups[i].address).Mid(6) : Utf8(groups[i].uid);
            card = VCard::WithGroup(card, uid, Utf8(groups[i].name), kept);
            if (ContactStore::UpdateCard(groups[i].accountId, groups[i].id, groups[i].name, groups[i].address, Wide(uid), Wide(card)))
               rewritten++;
            else
            {
               left = true;
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6831, "ContactGroups::ForgetAddress",
                  "Group " + StringParser::IntToString(groups[i].id) + " of account " + StringParser::IntToString(groups[i].accountId) +
                  " could not be rewritten without the erased address; it still names it.");
               continue;
            }
         }

         // Whether it was rewritten or never named the address as a member,
         // the card must not still carry it anywhere - in a note, in an
         // EMAIL a client put on a group card, in a property this server has
         // no rule for. One that does is said out loud rather than counted as
         // erased.
         if (CardCarriesAddress(card, wanted))
         {
            left = true;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6831, "ContactGroups::ForgetAddress",
               "The card of group " + StringParser::IntToString(groups[i].id) + " of account " + StringParser::IntToString(groups[i].accountId) +
               " still carries the erased address in a property this server does not rewrite; the address book is not clean.");
         }
      }

      return left ? -1 : rewritten;
   }

   bool
   ContactGroups::NeedsExpansion(const std::vector<String> &entries)
   {
      for (size_t i = 0; i < entries.size(); i++)
      {
         if (entries[i].Find(_T("@")) < 0)
            return true;
      }
      return false;
   }

   void
   ContactGroups::ExpandEntries(const std::vector<ContactGroup> &groups, std::vector<String> &addresses, std::vector<String> &entries)
   {
      if (entries.size() != addresses.size() || groups.empty() || !NeedsExpansion(entries))
         return;

      std::vector<String> newAddresses;
      std::vector<String> newEntries;
      for (size_t i = 0; i < entries.size(); i++)
      {
         if (entries[i].Find(_T("@")) >= 0)
         {
            newAddresses.push_back(addresses[i]);
            newEntries.push_back(entries[i]);
            continue;
         }

         String needle = entries[i];
         needle.TrimLeft();
         needle.TrimRight();
         if (needle.GetLength() >= 2 && needle.StartsWith(_T("\"")) && needle.EndsWith(_T("\"")))
            needle = needle.Mid(1, needle.GetLength() - 2);

         // Two groups of one name - which the routes refuse but a client
         // syncing two cards can leave behind - are not guessed between: the
         // entry stays as it was and is refused as not an address, which the
         // sender sees, rather than reaching whichever came first.
         const ContactGroup *group = nullptr;
         int named = 0;
         for (size_t j = 0; j < groups.size(); j++)
         {
            if (!needle.IsEmpty() && groups[j].name.CompareNoCase(needle) == 0)
            {
               named++;
               group = &groups[j];
            }
         }

         if (!group || named > 1)
         {
            newAddresses.push_back(addresses[i]);
            newEntries.push_back(entries[i]);
            continue;
         }

         for (size_t j = 0; j < group->recipients.size(); j++)
         {
            const ContactGroupMember &recipient = group->recipients[j];
            bool seen = false;
            for (size_t k = 0; !seen && k < newAddresses.size(); k++)
               seen = newAddresses[k].CompareNoCase(recipient.address) == 0;
            if (seen)
               continue;

            newAddresses.push_back(recipient.address);
            newEntries.push_back(HeaderEntry(recipient.name, recipient.address));
         }
      }

      addresses = newAddresses;
      entries = newEntries;
   }
}
