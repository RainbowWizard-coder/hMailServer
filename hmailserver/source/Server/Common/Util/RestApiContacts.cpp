// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's address book: GET and POST /api/v1/me/contacts, PUT
// and DELETE /api/v1/me/contacts/{id}, and the collection of recipients from
// what the account sends through the API. Phase A of the roadmap's address-book
// row - the per-account store; Phase B, the CardDAV address book over the same
// rows, is CardDavServer.cpp.
//
// The store is hm_contacts (schema 6032), read and written through ContactStore
// so that this surface and CardDAV cannot disagree about what a contact is.
// The filtering the completion popup asks for (q=) is done here in memory,
// since a case-insensitive substring match is not spelled the same on the four
// database backends and an address book is small. A group (ContactGroups.h)
// is a row of the same store and is listed here with kind group, its members
// resolved, so the completion offers it by name; its own routes are
// RestApiContactGroups.cpp.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "ContactStore.h"
#include "ContactCard.h"
#include "ContactGroups.h"
#include "Unicode.h"
#include "VCard.h"
#include "../BO/Account.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int ContactListDefaultLimit = 200;
      const int ContactListMaximumLimit = 1000;

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }
   }

   // A contact as the page reads it: the row, and what the card a phone
   // stored carries beside the name and address - the birthday, the note, the
   // phones, whether there is a photo (served at /photo, since it is an image
   // and not something to hand out a thousand times in a listing) - and the
   // card itself when asked for (fields=card), so an export carries it whole.
   AnsiString
   RestApiServer::ContactJson_(const ContactRecord &contact, bool includeCard)
   {
      ContactCard::Fields fields;
      ContactCard::Read(contact.vcard, fields);

      AnsiString phones = "[";
      for (size_t i = 0; i < fields.phones.size(); i++)
         phones += (i > 0 ? ",\"" : "\"") + JsonEscape_(Utf8_(fields.phones[i])) + "\"";
      phones += "]";

      AnsiString json = "{\"id\":" + Int64Text(contact.id) +
         ",\"name\":\"" + JsonEscape_(Utf8_(contact.name)) +
         "\",\"address\":\"" + JsonEscape_(Utf8_(contact.address)) +
         "\",\"source\":\"" + (contact.source == ContactStore::SourceCollected ? AnsiString("collected") : AnsiString("manual")) +
         "\",\"created\":\"" + JsonEscape_(Utf8_(contact.created)) +
         "\",\"kind\":\"person\",\"birthday\":\"" + JsonEscape_(Utf8_(fields.birthday)) +
         "\",\"note\":\"" + JsonEscape_(Utf8_(fields.note)) +
         "\",\"phones\":" + phones +
         ",\"has_photo\":" + (fields.hasPhoto ? "true" : "false");

      if (includeCard)
         json += ",\"card\":\"" + JsonEscape_(Utf8_(contact.vcard)) + "\"";

      json += "}";
      return json;
   }

   HttpResponse
   RestApiServer::HandleMeContacts_(const Caller &caller, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // q= narrows to names and addresses containing the text, case-insensitively
      // (the completion popup's use); limit= caps the answer, 200 by default;
      // source= is manual (what the person kept) or collected (what they wrote
      // to); kind= is person or group, both without it; fields=card puts the
      // stored card on each contact.
      String needle = String(QueryParameter_(query, "q"));
      needle.ToLower();
      AnsiString sourceFilter = QueryParameter_(query, "source");
      if (!sourceFilter.IsEmpty() && sourceFilter != "manual" && sourceFilter != "collected")
         return BuildResponse_(400, "{\"error\":\"source must be manual or collected\"}");
      AnsiString kindFilter = QueryParameter_(query, "kind");
      if (!kindFilter.IsEmpty() && kindFilter != "person" && kindFilter != "group")
         return BuildResponse_(400, "{\"error\":\"kind must be person or group\"}");
      const bool includeCard = QueryParameter_(query, "fields") == "card";
      int limit = ContactListDefaultLimit;
      AnsiString limitText = QueryParameter_(query, "limit");
      if (!limitText.IsEmpty())
      {
         limit = atoi(limitText.c_str());
         if (limit < 1)
            limit = 1;
         if (limit > ContactListMaximumLimit)
            limit = ContactListMaximumLimit;
      }

      std::vector<ContactRecord> contacts;
      if (!ContactStore::List(account->GetID(), contacts))
         return BuildResponse_(500, "{\"error\":\"the contacts could not be read\"}");

      // A group (ContactGroups.h) is listed among the contacts with its
      // members resolved against the rows already in hand, so that the
      // completion offers it by name and expands it with no second request.
      // The index is built once for the whole listing: every group of the
      // book is resolved against it, so a card is parsed once and a row's
      // UID computed once however many groups name it.
      String accountAddress = account->GetAddress();
      accountAddress.ToLower();
      const AnsiString accountAddressUtf8 = Utf8_(accountAddress);
      ContactGroupIndex groupIndex(accountAddressUtf8, contacts);

      AnsiString json = "{\"contacts\":[";
      int count = 0;
      int total = 0;
      for (const ContactRecord &contact : contacts)
      {
         const bool group = ContactGroups::IsGroup(contact);
         if (!kindFilter.IsEmpty() && (kindFilter == "group") != group)
            continue;

         if (!sourceFilter.IsEmpty() && (sourceFilter == "collected") != (contact.source == ContactStore::SourceCollected))
            continue;

         bool matches = needle.IsEmpty();
         if (!matches)
         {
            String lowerName = contact.name;
            lowerName.ToLower();
            String lowerAddress = group ? String() : contact.address;
            lowerAddress.ToLower();
            matches = lowerName.Find(needle) >= 0 || (!lowerAddress.IsEmpty() && lowerAddress.Find(needle) >= 0);
         }

         if (matches)
         {
            total++;
            if (count < limit)
            {
               if (count > 0)
                  json += ",";
               if (group)
               {
                  ContactGroup resolved;
                  ContactGroups::Resolve(groupIndex, contact, resolved);
                  json += ContactGroupJson_(resolved, &contact, includeCard);
               }
               else
                  json += ContactJson_(contact, includeCard);
               count++;
            }
         }
      }

      json += "],\"count\":" + Int64Text(count) + ",\"total\":" + Int64Text(total) + "}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeContactCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // Body: name and address; or card, a vCard kept as sent - what the page's
      // import hands over, so a card exported from a phone and brought in here
      // keeps what the phone stored. name and address beside a card override
      // the card's FN and preferred EMAIL for the row.
      String name;
      String address;
      String ignored;
      ContactStore::SplitEntry(JsonUtf8Value_(requestBody, "address"), name, address);
      String givenName = JsonUtf8Value_(requestBody, "name");
      givenName.TrimLeft();
      givenName.TrimRight();
      if (!givenName.IsEmpty())
         name = givenName;

      String card;
      String uid;
      if (requestBody.Find("\"card\"") >= 0)
      {
         card = JsonUtf8Value_(requestBody, "card");
         std::vector<VCardProperty> properties;
         AnsiString problem;
         if (!VCard::Parse(Utf8_(card), properties, problem))
            return BuildResponse_(400, "{\"error\":\"card is not a vCard: " + JsonEscape_(problem) + "\"}");

         AnsiString cardName;
         AnsiString cardAddress;
         VCard::ExtractNameAndAddress(properties, cardName, cardAddress);
         if (name.IsEmpty())
         {
            Unicode::MultiByteToWide(cardName, name);
            name.TrimLeft();
            name.TrimRight();
         }
         if (address.IsEmpty())
         {
            String fromCard;
            Unicode::MultiByteToWide(cardAddress, fromCard);
            ContactStore::SplitEntry(fromCard, ignored, address);
         }
         Unicode::MultiByteToWide(VCard::UidOf(properties), uid);
      }

      if (name.GetLength() > ContactStore::MaximumNameLength)
         return BuildResponse_(400, "{\"error\":\"name is at most 255 characters\"}");

      if (!ContactStore::IsValidAddress(address))
         return BuildResponse_(400, "{\"error\":\"address must be one e-mail address\"}");

      __int64 existing = 0;
      if (ContactStore::FindByAddress(account->GetID(), address, existing))
         return BuildResponse_(409, "{\"error\":\"a contact with that address exists\",\"id\":" + Int64Text(existing) + "}");

      ContactRecord inserted;
      bool ok = card.IsEmpty()
         ? ContactStore::Insert(account->GetID(), name, address, ContactStore::SourceManual, inserted)
         : ContactStore::InsertCard(account->GetID(), name, address, String(), uid, card, inserted);
      if (!ok)
         return BuildResponse_(500, "{\"error\":\"the contact could not be saved\"}");

      return BuildResponse_(201, ContactJson_(inserted, false));
   }

   HttpResponse
   RestApiServer::HandleMeContactUpdate_(const Caller &caller, __int64 id, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The row must be this account's; another account's id is 404, not 403,
      // so that the ids of one address book say nothing about another.
      ContactRecord contact;
      if (!ContactStore::Get(account->GetID(), id, contact))
         return BuildResponse_(404, "{\"error\":\"no such contact\"}");

      // A group has no address to change and a card whose name is written
      // another way: its own routes do that (RestApiContactGroups.cpp).
      if (ContactGroups::IsGroup(contact))
         return BuildResponse_(400, "{\"error\":\"this is a group; change it at /api/v1/me/contacts/groups/" + Int64Text(id) + "\"}");

      String name = contact.name;
      String address = contact.address;

      if (requestBody.Find("\"name\"") >= 0)
      {
         name = JsonUtf8Value_(requestBody, "name");
         name.TrimLeft();
         name.TrimRight();
         if (name.GetLength() > ContactStore::MaximumNameLength)
            return BuildResponse_(400, "{\"error\":\"name is at most 255 characters\"}");
      }

      if (requestBody.Find("\"address\"") >= 0)
      {
         String ignored;
         ContactStore::SplitEntry(JsonUtf8Value_(requestBody, "address"), ignored, address);
         if (!ContactStore::IsValidAddress(address))
            return BuildResponse_(400, "{\"error\":\"address must be one e-mail address\"}");

         __int64 other = 0;
         if (ContactStore::FindByAddress(account->GetID(), address, other) && other != id)
            return BuildResponse_(409, "{\"error\":\"a contact with that address exists\",\"id\":" + Int64Text(other) + "}");
      }

      // source: manual keeps a collected address as a contact, which is the
      // one direction there is - what the person wrote to does not become
      // something they merely wrote to again.
      int source = contact.source;
      if (requestBody.Find("\"source\"") >= 0)
      {
         String wanted = JsonUtf8Value_(requestBody, "source");
         if (wanted.CompareNoCase(_T("manual")) == 0)
            source = ContactStore::SourceManual;
         else
            return BuildResponse_(400, "{\"error\":\"source can only be set to manual, which keeps a collected address as a contact\"}");
      }

      if (!ContactStore::Update(account->GetID(), id, name, address))
         return BuildResponse_(500, "{\"error\":\"the contact could not be saved\"}");

      if (source != contact.source && !ContactStore::SetSource(account->GetID(), id, source))
         return BuildResponse_(500, "{\"error\":\"the contact could not be kept\"}");

      ContactRecord saved;
      if (!ContactStore::Get(account->GetID(), id, saved))
         return BuildResponse_(500, "{\"error\":\"the contact could not be read back\"}");

      return BuildResponse_(200, ContactJson_(saved, false));
   }

   HttpResponse
   RestApiServer::HandleMeContactDelete_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      ContactRecord contact;
      if (!ContactStore::Get(account->GetID(), id, contact))
         return BuildResponse_(404, "{\"error\":\"no such contact\"}");

      if (!ContactStore::Delete(account->GetID(), id))
         return BuildResponse_(500, "{\"error\":\"the contact could not be deleted\"}");

      // The groups that named it - a contact or a group in a group - no
      // longer do (ContactGroups.h).
      ContactGroups::ForgetMember(account, contact);

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   // Every recipient of a message the account sent becomes a contact of the
   // account, once, marked collected - shared with the SMTP submission path
   // through ContactCard::Collect.
   void
   RestApiServer::CollectContacts_(std::shared_ptr<const Account> account, const std::vector<String> &entries)
   {
      ContactCard::Collect(account, entries);
   }

   // The photo a phone stored on the card, as the image it is.
   HttpResponse
   RestApiServer::HandleMeContactPhoto_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      ContactRecord contact;
      if (!ContactStore::Get(account->GetID(), id, contact))
         return BuildResponse_(404, "{\"error\":\"no such contact\"}");

      AnsiString bytes;
      AnsiString contentType;
      if (!ContactCard::Photo(contact.vcard, bytes, contentType))
         return BuildResponse_(404, "{\"error\":\"the contact has no photo\"}");

      HttpResponse response;
      response.status = 200;
      response.content_type = contentType;
      response.body = bytes;
      response.extra_headers =
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: sandbox\r\n"
         "Cache-Control: private, max-age=3600\r\n";
      return response;
   }
}
