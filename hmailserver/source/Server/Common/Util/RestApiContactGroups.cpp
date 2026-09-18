// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Contact groups (ContactGroups.h) over the address-book routes:
//
//    GET    /api/v1/me/contacts/groups                    every group, with its members resolved
//    POST   /api/v1/me/contacts/groups                    {name, members:[{id}|{address}]}
//    GET    /api/v1/me/contacts/groups/{id}
//    PUT    /api/v1/me/contacts/groups/{id}               {name} and/or {members}, the members replaced whole
//    DELETE /api/v1/me/contacts/groups/{id}
//    POST   /api/v1/me/contacts/groups/{id}/members       {id}|{address}, one member added
//    DELETE /api/v1/me/contacts/groups/{id}/members/{ref} a contact id or an address, one member removed
//
// A group is a row of hm_contacts (its listing, /api/v1/me/contacts, answers
// it too, with kind group) and a card the CardDAV address book serves as it
// is, so a phone sees what the page made and the page sees what a phone made.
// A member named by contact id is written into the card as urn:uuid of that
// contact's card, so its name and address follow the contact; one named by
// address that is a contact of the account is written the same way, and one
// that is not is written as mailto and shown as the address alone.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "ContactStore.h"
#include "ContactGroups.h"
#include "JsonDocument.h"
#include "VCard.h"
#include "Unicode.h"
#include "../BO/Account.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString AccountAddressUtf8(std::shared_ptr<const Account> account)
      {
         String address = account->GetAddress();
         address.ToLower();
         AnsiString utf8;
         Unicode::WideToMultiByte(address, utf8);
         return utf8;
      }

      String Wide(const std::string &text)
      {
         String out;
         Unicode::MultiByteToWide(AnsiString(text), out);
         return out;
      }

      // The account's rows, and the one of them that is the group asked for.
      // Another account's id, or a contact that is not a group, is not found.
      bool FindGroupRow(__int64 accountId, __int64 groupId, std::vector<ContactRecord> &all, ContactRecord &row, bool &readFailed)
      {
         readFailed = !ContactStore::List(accountId, all);
         if (readFailed)
            return false;

         for (size_t i = 0; i < all.size(); i++)
         {
            if (all[i].id == groupId && ContactGroups::IsGroup(all[i]))
            {
               row = all[i];
               return true;
            }
         }

         return false;
      }

      // A group is addressed by typing its name into To, Cc or Bcc, and that
      // list is split on commas and semicolons and read for "Name <address>",
      // so a name carrying one of those characters could be made here and then
      // never addressed. It is refused with the reason instead. A name a phone
      // writes over CardDAV is kept whatever it holds - the card is the
      // client's - and such a group is offered in the page and written to by
      // its Write button, but not by name in the field; the page's own names
      // come through here.
      bool NameCanBeAddressed(const String &name)
      {
         for (int i = 0; i < name.GetLength(); i++)
         {
            wchar_t c = name[i];
            if (c == ',' || c == ';' || c == '<' || c == '>' || c == '"' || c == '@' || c == '\r' || c == '\n' || c == '\t')
               return false;
         }
         return true;
      }

      // Whether another group of the account has this name: a group is
      // addressed by its name, so two with one name would leave the To field
      // to guess.
      bool NameTaken(const std::vector<ContactRecord> &all, const String &name, __int64 exceptId)
      {
         for (size_t i = 0; i < all.size(); i++)
         {
            if (all[i].id != exceptId && ContactGroups::IsGroup(all[i]) && all[i].name.CompareNoCase(name) == 0)
               return true;
         }
         return false;
      }

      const ContactRecord *FindRow(const std::vector<ContactRecord> &all, __int64 id)
      {
         for (size_t i = 0; i < all.size(); i++)
            if (all[i].id == id)
               return &all[i];
         return nullptr;
      }

      const ContactRecord *FindRowByAddress(const std::vector<ContactRecord> &all, const String &address)
      {
         for (size_t i = 0; i < all.size(); i++)
            if (!ContactGroups::IsGroup(all[i]) && all[i].address == address)
               return &all[i];
         return nullptr;
      }

      // One member as a request names it - {"id":12} or {"address":"a@b"},
      // or a bare "a@b" - into what the card writes. False with the reason
      // when it names nothing this address book can hold.
      bool ReferenceFor(std::shared_ptr<const Account> account, const std::vector<ContactRecord> &all, __int64 groupId,
                        const JsonValue &item, AnsiString &reference, AnsiString &problem)
      {
         reference = "";

         __int64 id = 0;
         String address;
         if (item.IsObject())
         {
            const JsonValue *idValue = item.Get("id");
            if (idValue && idValue->IsNumber())
               id = idValue->AsInt64();
            address = Wide(item.GetString("address"));
         }
         else if (item.IsString())
            address = Wide(item.AsString());
         else if (item.IsNumber())
            id = item.AsInt64();

         if (id > 0)
         {
            const ContactRecord *row = FindRow(all, id);
            if (!row)
            {
               problem = "no contact " + Int64Text(id);
               return false;
            }
            if (row->id == groupId)
            {
               problem = "a group cannot be a member of itself";
               return false;
            }

            ContactRecord member = *row;
            if (!ContactGroups::ReferenceForContact(account, member, reference))
            {
               problem = "contact " + Int64Text(id) + " could not be written";
               return false;
            }
            return true;
         }

         String ignored;
         ContactStore::SplitEntry(address, ignored, address);
         if (!ContactStore::IsValidAddress(address))
         {
            problem = "a member is a contact id or one e-mail address";
            return false;
         }

         // An address that is a contact is named as the contact, so that
         // its name and any later change of address follow.
         const ContactRecord *row = FindRowByAddress(all, address);
         if (row)
         {
            ContactRecord member = *row;
            if (!ContactGroups::ReferenceForContact(account, member, reference))
            {
               problem = "contact " + Int64Text(row->id) + " could not be written";
               return false;
            }
            return true;
         }

         reference = ContactGroups::ReferenceForAddress(address);
         return true;
      }

      // The members array of a body, into references, each once.
      bool ReadMembers(std::shared_ptr<const Account> account, const std::vector<ContactRecord> &all, __int64 groupId,
                       const JsonValue &body, std::vector<AnsiString> &references, AnsiString &problem)
      {
         references.clear();

         const JsonValue *members = body.Get("members");
         if (!members)
            return true;
         if (!members->IsArray())
         {
            problem = "members must be an array";
            return false;
         }
         if ((int) members->Size() > ContactGroups::MaximumMembers)
         {
            problem = "a group holds at most " + Int64Text(ContactGroups::MaximumMembers) + " members";
            return false;
         }

         for (size_t i = 0; i < members->Size(); i++)
         {
            AnsiString reference;
            if (!ReferenceFor(account, all, groupId, *members->At(i), reference, problem))
               return false;

            bool seen = false;
            for (size_t j = 0; !seen && j < references.size(); j++)
               seen = references[j].CompareNoCase(reference) == 0;
            if (!seen)
               references.push_back(reference);
         }

         return true;
      }

      // The group's references as its card carries them now.
      void CurrentReferences(const ContactRecord &row, std::vector<AnsiString> &references)
      {
         references.clear();
         AnsiString card;
         Unicode::WideToMultiByte(row.vcard, card);
         std::vector<VCardProperty> properties;
         AnsiString problem;
         if (VCard::Parse(card, properties, problem))
            VCard::MemberReferences(properties, references);
      }

      HttpResponse Problem(int status, const AnsiString &text)
      {
         HttpResponse response;
         response.status = status;
         response.content_type = "application/json";
         response.body = "{\"error\":\"" + RestApiServer::JsonEscapeUtf8(String(text)) + "\"}";
         return response;
      }
   }

   AnsiString
   RestApiServer::ContactGroupJson_(const ContactGroup &group, const ContactRecord *record, bool includeCard, const VisibleBook *book)
   {
      AnsiString members = "[";
      for (size_t i = 0; i < group.members.size(); i++)
      {
         const ContactGroupMember &member = group.members[i];
         if (i > 0)
            members += ",";
         members += "{\"id\":" + Int64Text(member.id) +
            ",\"name\":\"" + JsonEscape_(Utf8_(member.name)) +
            "\",\"address\":\"" + JsonEscape_(Utf8_(member.address)) +
            "\",\"kind\":\"" + (member.group ? "group" : "person") + "\"}";
      }
      members += "]";

      AnsiString recipients = "[";
      for (size_t i = 0; i < group.recipients.size(); i++)
      {
         const ContactGroupMember &recipient = group.recipients[i];
         if (i > 0)
            recipients += ",";
         recipients += "{\"id\":" + Int64Text(recipient.id) +
            ",\"name\":\"" + JsonEscape_(Utf8_(recipient.name)) +
            "\",\"address\":\"" + JsonEscape_(Utf8_(recipient.address)) + "\"}";
      }
      recipients += "]";

      AnsiString json = "{\"id\":" + Int64Text(group.id) +
         ",\"name\":\"" + JsonEscape_(Utf8_(group.name)) +
         "\",\"address\":\"\",\"source\":\"manual\",\"created\":\"" + JsonEscape_(Utf8_(group.created)) +
         "\",\"kind\":\"group\",\"uid\":\"" + JsonEscape_(group.uid) +
         "\",\"members\":" + members +
         ",\"recipients\":" + recipients;

      // Which address book it is in, when the listing carries more than one
      // (RestApiContacts.cpp). Always one of the READER'S own books: a group
      // is one account's entry and resolves against that account's whole
      // address book, so no group of a book somebody shares with the reader is
      // listed - the label says which of their own books this one is in.
      if (book)
      {
         json += ",\"book_id\":" + Int64Text(book->id) +
            ",\"book_name\":\"" + JsonEscapeUtf8(book->name) +
            "\",\"book_owner\":\"" + JsonEscapeUtf8(book->owner) +
            "\",\"book_mine\":" + AnsiString(book->mine ? "true" : "false") +
            ",\"book_writable\":" + AnsiString(book->writable ? "true" : "false");
      }

      if (includeCard && record)
         json += ",\"card\":\"" + JsonEscape_(Utf8_(record->vcard)) + "\"";

      json += "}";
      return json;
   }

   HttpResponse
   RestApiServer::HandleMeContactGroups_(const Caller &caller, RouteKind kind, __int64 groupId, const AnsiString &memberReference, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      const __int64 accountId = account->GetID();
      const AnsiString accountAddressUtf8 = AccountAddressUtf8(account);

      if (kind == RouteMeContactGroups)
      {
         std::vector<ContactGroup> groups;
         if (!ContactGroups::List(account, groups))
            return BuildResponse_(500, "{\"error\":\"the contacts could not be read\"}");

         AnsiString json = "{\"groups\":[";
         for (size_t i = 0; i < groups.size(); i++)
         {
            if (i > 0)
               json += ",";
            json += ContactGroupJson_(groups[i], nullptr, false);
         }
         json += "],\"count\":" + Int64Text((__int64) groups.size()) + "}";
         return BuildResponse_(200, json);
      }

      // Everything else reads the body as a document, since the members are
      // objects in an array.
      JsonValue body;
      if (kind == RouteMeContactGroupCreate || kind == RouteMeContactGroupUpdate || kind == RouteMeContactGroupMemberAdd)
      {
         std::string error;
         if (requestBody.GetLength() > 256 * 1024)
            return Problem(400, "the body is too large");
         if (!JsonValue::Parse(std::string(requestBody), body, error) || !body.IsObject())
            return Problem(400, "the body must be a JSON object");
      }

      if (kind == RouteMeContactGroupCreate)
      {
         std::vector<ContactRecord> all;
         if (!ContactStore::List(accountId, all))
            return BuildResponse_(500, "{\"error\":\"the contacts could not be read\"}");

         String name = Wide(body.GetString("name"));
         name.TrimLeft();
         name.TrimRight();
         if (name.IsEmpty())
            return Problem(400, "a group needs a name");
         if (name.GetLength() > ContactStore::MaximumNameLength)
            return Problem(400, "name is at most 255 characters");
         if (!NameCanBeAddressed(name))
            return Problem(400, "a group is addressed by its name, so the name cannot hold , ; < > \" or @");
         if (NameTaken(all, name, 0))
            return Problem(409, "a group with that name exists");

         // An address book holds so many groups and no more. Every read of it
         // resolves them all - the listing, the To field's completion, a send
         // addressed by name - and a book of groups that name one another
         // costs the square of its size to expand; the ceiling is what keeps
         // that cost off the worker threads the whole portal shares.
         if (ContactGroups::GroupCount(all) >= ContactGroups::MaximumGroups)
            return Problem(400, "an address book holds at most " + Int64Text(ContactGroups::MaximumGroups) + " groups");

         std::vector<AnsiString> references;
         AnsiString problem;
         if (!ReadMembers(account, all, 0, body, references, problem))
            return Problem(400, problem);

         ContactRecord inserted;
         if (!ContactGroups::Create(account, name, references, inserted))
            return BuildResponse_(500, "{\"error\":\"the group could not be saved\"}");

         all.clear();
         ContactRecord row;
         bool readFailed = false;
         if (!FindGroupRow(accountId, inserted.id, all, row, readFailed))
            return BuildResponse_(500, "{\"error\":\"the group could not be read back\"}");

         ContactGroupIndex index(accountAddressUtf8, all);
         ContactGroup group;
         ContactGroups::Resolve(index, row, group);
         return BuildResponse_(201, ContactGroupJson_(group, &row, false));
      }

      // The rest name one group, which must be this account's: another
      // account's id is 404, not 403, as with contacts.
      std::vector<ContactRecord> all;
      ContactRecord row;
      bool readFailed = false;
      if (!FindGroupRow(accountId, groupId, all, row, readFailed))
         return readFailed ? BuildResponse_(500, "{\"error\":\"the contacts could not be read\"}") : BuildResponse_(404, "{\"error\":\"no such group\"}");

      if (kind == RouteMeContactGroupGet)
      {
         ContactGroupIndex index(accountAddressUtf8, all);
         ContactGroup group;
         ContactGroups::Resolve(index, row, group);
         return BuildResponse_(200, ContactGroupJson_(group, &row, false));
      }

      if (kind == RouteMeContactGroupDelete)
      {
         if (!ContactStore::Delete(accountId, row.id))
            return BuildResponse_(500, "{\"error\":\"the group could not be deleted\"}");

         // A group in another group: that one no longer names it.
         ContactGroups::ForgetMember(account, row);
         return BuildResponse_(200, "{\"deleted\":true}");
      }

      String name = row.name;
      std::vector<AnsiString> references;
      CurrentReferences(row, references);

      if (kind == RouteMeContactGroupUpdate)
      {
         if (body.Get("name"))
         {
            name = Wide(body.GetString("name"));
            name.TrimLeft();
            name.TrimRight();
            if (name.IsEmpty())
               return Problem(400, "a group needs a name");
            if (name.GetLength() > ContactStore::MaximumNameLength)
               return Problem(400, "name is at most 255 characters");
            if (!NameCanBeAddressed(name))
               return Problem(400, "a group is addressed by its name, so the name cannot hold , ; < > \" or @");
            if (NameTaken(all, name, row.id))
               return Problem(409, "a group with that name exists");
         }

         if (body.Get("members"))
         {
            AnsiString problem;
            if (!ReadMembers(account, all, row.id, body, references, problem))
               return Problem(400, problem);
         }
      }
      else if (kind == RouteMeContactGroupMemberAdd)
      {
         AnsiString reference;
         AnsiString problem;
         if (!ReferenceFor(account, all, row.id, body, reference, problem))
            return Problem(400, problem);

         bool present = false;
         for (size_t i = 0; !present && i < references.size(); i++)
            present = references[i].CompareNoCase(reference) == 0;
         if (!present)
         {
            if ((int) references.size() >= ContactGroups::MaximumMembers)
               return Problem(400, "a group holds at most " + Int64Text(ContactGroups::MaximumMembers) + " members");
            references.push_back(reference);
         }
      }
      else if (kind == RouteMeContactGroupMemberRemove)
      {
         // The segment is a contact id or an address. A contact is removed by
         // every reference that names it - by UID or by address - and an
         // address by the references that carry it.
         std::vector<AnsiString> kept;
         bool numeric = !memberReference.IsEmpty() && memberReference.GetLength() < 19;
         for (int i = 0; numeric && i < memberReference.GetLength(); i++)
            numeric = memberReference[i] >= '0' && memberReference[i] <= '9';

         if (numeric)
         {
            const ContactRecord *member = FindRow(all, _atoi64(memberReference.c_str()));
            if (!member)
               return BuildResponse_(404, "{\"error\":\"not a member\"}");
            for (size_t i = 0; i < references.size(); i++)
               if (!ContactGroups::ReferenceNames(accountAddressUtf8, references[i], *member))
                  kept.push_back(references[i]);
         }
         else
         {
            String address;
            String ignored;
            Unicode::MultiByteToWide(memberReference, address);
            ContactStore::SplitEntry(address, ignored, address);
            if (address.IsEmpty())
               return BuildResponse_(404, "{\"error\":\"not a member\"}");

            const AnsiString wanted = ContactGroups::ReferenceForAddress(address);
            const ContactRecord *member = FindRowByAddress(all, address);
            for (size_t i = 0; i < references.size(); i++)
            {
               bool names = references[i].CompareNoCase(wanted) == 0 ||
                  (member && ContactGroups::ReferenceNames(accountAddressUtf8, references[i], *member));
               if (!names)
                  kept.push_back(references[i]);
            }
         }

         if (kept.size() == references.size())
            return BuildResponse_(404, "{\"error\":\"not a member\"}");
         references = kept;
      }
      else
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!ContactGroups::Save(account, row, name, references))
         return BuildResponse_(500, "{\"error\":\"the group could not be saved\"}");

      all.clear();
      if (!FindGroupRow(accountId, row.id, all, row, readFailed))
         return BuildResponse_(500, "{\"error\":\"the group could not be read back\"}");

      ContactGroupIndex index(accountAddressUtf8, all);
      ContactGroup group;
      ContactGroups::Resolve(index, row, group);
      return BuildResponse_(200, ContactGroupJson_(group, &row, false));
   }

   AnsiString
   RestApiServer::OpenApiContactGroupsPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/contacts/groups\":{\"get\":{\"summary\":\"The signed-in account's contact groups\",\"description\":\"{groups:[{id,name,uid,created,kind:group,members:[{id,name,address,kind}],recipients:[{id,name,address}]}],count}. A group is a row of the address book with a card whose kind is group (KIND:group in vCard 4.0, X-ADDRESSBOOKSERVER-KIND:group in 3.0), served over CardDAV as it is and made by a phone or a desktop client as well as here. members are what the card names and this address book can answer for - a contact by the UID of its card, an address that is not a contact as itself, a group in the group - in the card's order; recipients are every person reached, groups in groups expanded, once each. A reference the card carries that names nothing is kept in the card and left out of both. In To, Cc or Bcc, the group's name alone stands for its recipients: the page expands it as it is chosen, and POST /api/v1/me/messages expands an entry that is a group's name with no address in it, a scheduled send included. An entry no group answers to, and one two groups of the account answer to, is left exactly as it was and refused as not an address.\",\"responses\":{\"200\":{\"description\":\"The groups\"}}},\"post\":{\"summary\":\"Make a group\",\"description\":\"Body: name (required, and refused when another group of the account has it, case-insensitively, since a group is addressed by its name - a client that writes two group cards with one FN over CardDAV is not refused, and such a name is then left unexpanded in To rather than guessed between), members (optional, at most 500, which a CardDAV PUT is held to as well): each {id} of a contact of the account or of another group of it, or {address} - an address that is a contact is written as that contact, so its name follows; one that is not is written as mailto and shown as the address alone. An address book holds at most 200 groups, since every read of it resolves them all.\",\"responses\":{\"201\":{\"description\":\"The group\"},\"400\":{\"description\":\"No name, a member that is no contact of this account and no address, the group named as its own member, or the book already at its 200 groups\"},\"409\":{\"description\":\"A group with that name exists\"}}}},"
         "\"/api/v1/me/contacts/groups/{id}\":{\"get\":{\"summary\":\"One group\",\"responses\":{\"200\":{\"description\":\"The group\"},\"404\":{\"description\":\"No such group of this account\"}}},\"put\":{\"summary\":\"Rename a group, or replace its members\",\"description\":\"Body: name and/or members, as for POST; members given replaces the list whole, and what the card carries besides - a phone's own properties - stays. A group's card is rewritten in its own dialect, MEMBER in a 4.0 card and X-ADDRESSBOOKSERVER-MEMBER in a 3.0 one.\",\"responses\":{\"200\":{\"description\":\"The group\"},\"400\":{\"description\":\"As for POST\"},\"404\":{\"description\":\"No such group\"},\"409\":{\"description\":\"Another group has that name\"}}},\"delete\":{\"summary\":\"Remove a group\",\"description\":\"The members stay contacts; a group that named this one no longer does.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such group\"}}}},"
         "\"/api/v1/me/contacts/groups/{id}/members\":{\"post\":{\"summary\":\"Add one member\",\"description\":\"Body: {id} of a contact or of another group, or {address}. A member already there is left as it is, and the five hundredth is the last.\",\"responses\":{\"200\":{\"description\":\"The group\"},\"400\":{\"description\":\"Not a contact of this account and not an address\"},\"404\":{\"description\":\"No such group\"}}}},"
         "\"/api/v1/me/contacts/groups/{id}/members/{ref}\":{\"delete\":{\"summary\":\"Remove one member\",\"description\":\"ref is a contact id or an address (percent-encoded). A contact is removed by every reference that names it, by UID or by address.\",\"responses\":{\"200\":{\"description\":\"The group\"},\"404\":{\"description\":\"No such group, or not a member of it\"}}}}";
      return AnsiString(paths);
   }
}
